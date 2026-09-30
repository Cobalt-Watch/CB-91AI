#!/usr/bin/env python3
"""Flash the CB-91AI self-test firmware through pyOCD and capture the RTT log.

Runs, in order: `pyocd list`, `pyocd erase --chip` (unlocks a fresh or
protected nRF52840 through CTRL-AP), `pyocd flash <hex>` without reset, a RAM
clear followed by a reset (so that no stale RTT control block of the previous
image survives), then attaches to the RTT channel for a while and saves
everything to firmware/logs/.

The erase step is the only one allowed to unlock the chip. The flash and RTT
steps run with the pyOCD option `auto_unlock` disabled: by default pyOCD mass
erases, without any error, a chip whose debug port is locked (APPROTECT
enabled) as soon as it connects to it. That wipes the firmware that was just
programmed and hides the real problem, which is a firmware that locks the port.

Usage:
    python firmware/tools/bringup.py                # erase, flash MCUboot + the built app image, capture 40 s
    python firmware/tools/bringup.py --no-erase     # keep UICR, settings and the other slot, just reprogram
    python firmware/tools/bringup.py --no-flash     # only capture the RTT log
    python firmware/tools/bringup.py --hex firmware/build/mcuboot/zephyr/zephyr.hex firmware/build/app/zephyr/zephyr.signed.confirmed.hex

Requirements: Python 3.8+, `pip install pyocd`, an ST-Link (or any CMSIS-DAP /
J-Link probe) wired to the DIO, CLK and GND pads. On Windows the ST-Link USB
driver from STMicroelectronics must be installed.
"""

import argparse
import datetime
import os
import pathlib
import queue
import re
import shutil
import subprocess
import sys
import threading
import time

FIRMWARE_DIR = pathlib.Path(__file__).resolve().parents[1]
PREBUILT_DIR = FIRMWARE_DIR / "prebuilt"
BUILD_DIR = FIRMWARE_DIR / "build"
# MCUboot followed by the signed self-test image, programmed in one go. Only the
# bootloader is shipped prebuilt; the signed application image comes from a local
# sysbuild (firmware/build/...), so prefer that. MCUboot executes the images in
# place, with revert: it erases an image whose trailer does not say "confirmed",
# so the image to program over SWD is zephyr.signed.confirmed.hex, the slot 0
# variant. The plain image is the last fallback, for a build done without sysbuild.
DEFAULT_HEXES = [PREBUILT_DIR / "cb91ai_mcuboot.hex", PREBUILT_DIR / "cb91ai_selftest.signed.hex"]
if not all(path.is_file() for path in DEFAULT_HEXES):
    built = [BUILD_DIR / "mcuboot" / "zephyr" / "zephyr.hex",
             BUILD_DIR / "app" / "zephyr" / "zephyr.signed.confirmed.hex"]
    DEFAULT_HEXES = built if all(path.is_file() for path in built) else [PREBUILT_DIR / "cb91ai_selftest.hex"]
LOG_DIR = FIRMWARE_DIR / "logs"
TARGET = "nrf52840"
# nRF52840 RAM, cleared before the first boot of a freshly programmed image
RAM_BASE = 0x20000000
RAM_SIZE = 0x40000
# Never let pyOCD erase the chip on its own to get past a locked debug port.
NO_AUTO_UNLOCK = ["-O", "auto_unlock=false"]
# Fail at once when no probe is found instead of waiting for one forever.
NO_WAIT = ["--no-wait"]
# Log lines of pyOCD itself look like "0000742 I Target type is nrf52840 [board]".
PYOCD_LOG_LINE = re.compile(r"^\d{7} [A-Z] ")
# The Zephyr shell redraws its prompt with ANSI escape sequences around every
# log line; strip both so that the saved log stays readable.
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
SHELL_PROMPT = re.compile(r"cb91ai:~\$ ?")
# RAM is not cleared by programming: the first read of the RTT buffer may
# return leftovers of the previous run and raw bytes. Drop control characters.
CONTROL_CHARS = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")


def pyocd_command():
    exe = shutil.which("pyocd")
    if exe:
        return [exe]
    return [sys.executable, "-m", "pyocd"]


def probe_present(base, env):
    """Run `pyocd list` and tell whether at least one probe was found."""
    print("$ " + " ".join(base + ["list"]), flush=True)
    result = subprocess.run(base + ["list"], env=env, capture_output=True, text=True, errors="replace")
    sys.stdout.write(result.stdout)
    sys.stdout.write(result.stderr)
    sys.stdout.flush()
    if result.returncode != 0 or "No available debug probes" in result.stdout + result.stderr:
        return False
    if "no libusb library" in result.stdout + result.stderr:
        return False
    return True


def run(cmd, env):
    print("$ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, env=env).returncode


def capture_rtt(base, env, seconds, log_path):
    """Stream `pyocd rtt` output to the terminal and to log_path for `seconds`.

    Returns (firmware_lines, pyocd_messages): the number of lines that came
    from the firmware itself, and the pyOCD warnings and errors seen while
    attaching, which explain an empty capture.
    """
    print(f"RTT capture for {seconds} s -> {log_path}", flush=True)
    proc = subprocess.Popen(
        # Attach without halting: the Bluetooth controller asserts (lll_adv.c
        # "Actual EVENT_OVERHEAD_START_US") if the core is stopped for a while.
        base + ["rtt", "-t", TARGET] + NO_AUTO_UNLOCK + NO_WAIT + ["-O", "connect_mode=attach"],
        env=env,
        stdin=subprocess.PIPE,  # keep stdin open: the RTT command also reads the console
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    lines = queue.Queue()

    def reader():
        for line in proc.stdout:
            lines.put(line)
        lines.put(None)

    threading.Thread(target=reader, daemon=True).start()
    deadline = time.time() + seconds
    firmware_lines = 0
    messages = []
    with open(log_path, "w", encoding="utf-8") as log:
        log.write(f"# pyocd rtt capture {datetime.datetime.now():%Y-%m-%d %H:%M:%S}\n")
        while time.time() < deadline:
            try:
                line = lines.get(timeout=0.5)
            except queue.Empty:
                if proc.poll() is not None:
                    break
                continue
            if line is None:
                break
            line = CONTROL_CHARS.sub("", SHELL_PROMPT.sub("", ANSI_ESCAPE.sub("", line)))
            if not line.strip():
                continue
            sys.stdout.write(line)
            sys.stdout.flush()
            log.write(line)
            if PYOCD_LOG_LINE.match(line):
                if line[8] in "WEC":  # warning, error, critical
                    messages.append(line.rstrip())
            else:
                firmware_lines += 1
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
    return firmware_lines, messages


def explain_empty_capture(messages):
    text = "\n".join(messages)
    if "APPROTECT enabled" in text:
        return ("The debug port is locked (APPROTECT enabled), so RTT cannot read the target. "
                "Either the chip was never unlocked, or the firmware locked the port at boot "
                "(on dies up to build code D, any UICR.APPROTECT value other than 0xFF locks "
                "it). Run again without --no-erase to recover, then fix the firmware.")
    if "Control block not found" in text:
        return ("pyOCD found no RTT control block in RAM: the firmware is not running "
                "(empty flash, wrong hex, or a crash before logging started). Check the "
                "flash step above; `pyocd commander -t nrf52840 -O auto_unlock=false "
                "-c \"read32 0 16\"` shows whether the flash is blank (all ff).")
    return ("No RTT output. Press the reset button (U108) while the capture runs, or check "
            "that the firmware was flashed and that the probe stayed connected.")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hex", nargs="+", default=[str(path) for path in DEFAULT_HEXES],
                        help="hex file(s) to program, in order")
    parser.add_argument("--no-erase", action="store_true", help="skip the chip erase / unlock step")
    parser.add_argument("--no-flash", action="store_true", help="do not program, only capture RTT")
    parser.add_argument("--seconds", type=int, default=40, help="RTT capture duration")
    args = parser.parse_args()

    try:
        import pyocd  # noqa: F401
    except ImportError:
        print("pyOCD is not installed: run `pip install pyocd` first.")
        return 2

    base = pyocd_command()
    env = dict(os.environ, PYTHONUNBUFFERED="1")

    if not probe_present(base, env):
        print("No debug probe: check the ST-Link USB connection and its driver. If pyOCD "
              "complains about libusb, run `pip install --force-reinstall libusb-package`.")
        return 1

    if not args.no_flash:
        hex_paths = [pathlib.Path(path) for path in args.hex]
        for hex_path in hex_paths:
            if not hex_path.is_file():
                print(f"hex file not found: {hex_path}")
                return 2
        if not args.no_erase:
            if run(base + ["erase", "--chip", "-t", TARGET] + NO_WAIT, env) != 0:
                print("Chip erase failed. Check the wiring (DIO, CLK, GND) and that the board is powered.")
                return 1
        # Left to itself, pyOCD erases the whole chip when that is faster and the image
        # starts at address 0: with --no-erase, only the sectors that are reprogrammed,
        # so that the UICR, the settings partition and the other slot are kept.
        sector_only = ["--erase", "sector"] if args.no_erase else []
        if run(base + ["flash", "-t", TARGET] + NO_AUTO_UNLOCK + NO_WAIT
               + ["-O", "resume_on_disconnect=false", "--no-reset"] + sector_only
               + [str(path) for path in hex_paths], env) != 0:
            print("Programming failed. If pyOCD reports APPROTECT enabled, run again without "
                  "--no-erase so that the chip is unlocked first.")
            return 1
        # Neither programming nor a reset clears the RAM. The RTT control block of
        # the previous image keeps its "SEGGER RTT" signature there, and pyOCD may
        # pick that stale block instead of the new one and mix old lines into the
        # capture. Zero the RAM while the core is still halted, then reset.
        if run(base + ["commander", "-t", TARGET] + NO_AUTO_UNLOCK + NO_WAIT
               + ["-O", "connect_mode=attach", "-c", "halt",
                  "-c", f"fill 32 {RAM_BASE:#x} {RAM_SIZE:#x} 0", "-c", "reset"], env) != 0:
            print("Could not clear the RAM before the first boot; the capture may contain "
                  "lines of the previous run.")
        # The first boot programs the UICR (REGOUT0, and APPROTECT on revision 3
        # dies) and resets once.
        time.sleep(1.5)

    LOG_DIR.mkdir(exist_ok=True)
    log_path = LOG_DIR / f"selftest-{datetime.datetime.now():%Y%m%d-%H%M%S}.txt"
    count, messages = capture_rtt(base, env, args.seconds, log_path)
    print(f"\n{count} firmware line(s) captured, log saved to {log_path}")
    if count == 0:
        print(explain_empty_capture(messages))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
