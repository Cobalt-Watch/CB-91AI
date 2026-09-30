#!/usr/bin/env python3
"""Capture the CB-91AI shell and log over the USB CDC-ACM port, no probe needed.

Finds the COM port by USB VID:PID (default 2FE3:0004, the Zephyr test IDs used
by the self-test firmware), opens it, optionally sends shell commands, prints
everything and saves it to firmware/logs/usb-<date>.txt.

Usage:
    python firmware/tools/usb_console.py                        # capture 40 s
    python firmware/tools/usb_console.py --seconds 10 --cmd "kernel uptime"
    python firmware/tools/usb_console.py --port COM7 --cmd "device list"
    python firmware/tools/usb_console.py --list      # shell port and MCUmgr port

The board exposes two CDC-ACM ports: interface 0 is the shell and log port
used here, interface 2 is the MCUmgr (SMP) port for `smpmgr --port COMx`.

Requirements: `pip install pyserial`. Windows binds its usbser driver to a
CDC-ACM device by itself; no driver to install.
"""

import argparse
import datetime
import pathlib
import re
import sys
import time

import serial
import serial.tools.list_ports

FIRMWARE_DIR = pathlib.Path(__file__).resolve().parents[1]
LOG_DIR = FIRMWARE_DIR / "logs"
DEFAULT_VID = 0x2FE3
DEFAULT_PID = 0x0004
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
SHELL_PROMPT = re.compile(r"cb91ai:~\$ ?")
CONTROL_CHARS = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")


def interface_number(port):
    """USB interface number of a composite-device port, e.g. 2 for a location '1-2:x.2'."""
    location = port.location or ""
    if "x." in location:
        try:
            return int(location.rsplit("x.", 1)[1].split(".")[0])
        except ValueError:
            pass
    match = re.search(r"MI_(\d+)", port.hwid or "")
    return int(match.group(1)) if match else 0


def matching_ports(vid, pid):
    """Ports of the board, interface 0 (shell) first, then interface 2 (MCUmgr)."""
    ports = [p for p in serial.tools.list_ports.comports() if p.vid == vid and p.pid == pid]
    return sorted(ports, key=lambda p: (interface_number(p), p.device))


def find_port(vid, pid, index=0):
    ports = matching_ports(vid, pid)
    return ports[index].device if len(ports) > index else None


def list_ports():
    ports = []
    for p in serial.tools.list_ports.comports():
        ids = f" ({p.vid:04x}:{p.pid:04x})" if p.vid is not None else ""
        ports.append(f"{p.device}{ids} {p.description}")
    return ports


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="COM port (default: first port matching --vid/--pid)")
    parser.add_argument("--vid", type=lambda s: int(s, 16), default=DEFAULT_VID, help="USB vendor ID, hex")
    parser.add_argument("--pid", type=lambda s: int(s, 16), default=DEFAULT_PID, help="USB product ID, hex")
    parser.add_argument("--seconds", type=float, default=40, help="capture duration")
    parser.add_argument("--interface", type=int, default=0,
                        help="which of the board ports to open: 0 shell (default), 1 MCUmgr")
    parser.add_argument("--list", action="store_true", help="list the board ports and exit")
    parser.add_argument("--cmd", action="append", default=[], help="shell command to send (repeatable)")
    args = parser.parse_args()

    if args.list:
        for i, p in enumerate(matching_ports(args.vid, args.pid)):
            role = "shell and log" if i == 0 else "MCUmgr (SMP)" if i == 1 else "?"
            print(f"{p.device}: interface {i}, {role}, location {p.location}")
        return 0
    port = args.port or find_port(args.vid, args.pid, args.interface)
    if not port:
        print(f"No CDC-ACM port with VID:PID {args.vid:04x}:{args.pid:04x}.")
        print("Ports present: " + ("; ".join(list_ports()) or "none"))
        return 1

    LOG_DIR.mkdir(exist_ok=True)
    log_path = LOG_DIR / f"usb-{datetime.datetime.now():%Y%m%d-%H%M%S}.txt"
    print(f"Opening {port}, capture {args.seconds} s -> {log_path}", flush=True)
    lines = 0
    with serial.Serial(port, 115200, timeout=0.2) as ser, open(log_path, "w", encoding="utf-8") as log:
        ser.dtr = True  # the shell waits for DTR before it starts talking
        log.write(f"# usb capture {datetime.datetime.now():%Y-%m-%d %H:%M:%S} on {port}\n")
        deadline = time.time() + args.seconds
        pending = list(args.cmd)
        next_cmd = time.time() + 1.0
        buf = b""
        while time.time() < deadline:
            if pending and time.time() >= next_cmd:
                cmd = pending.pop(0)
                print(f"> {cmd}", flush=True)
                log.write(f"> {cmd}\n")
                ser.write((cmd + "\r\n").encode())
                next_cmd = time.time() + 1.0
            try:
                chunk = ser.read(4096)
            except serial.SerialException as exc:
                # Seen on this bench: Windows drops the CDC-ACM port now and then
                # ("ClearCommError failed") while the board keeps running. Keep
                # what was captured instead of dying with a traceback.
                print(f"(COM port lost: {exc})", flush=True)
                log.write(f"# port lost: {exc}\n")
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace")
                text = CONTROL_CHARS.sub("", SHELL_PROMPT.sub("", ANSI_ESCAPE.sub("", text)))
                if text.strip():
                    print(text, flush=True)
                    log.write(text + "\n")
                    lines += 1
    print(f"\n{lines} line(s) captured, log saved to {log_path}")
    return 0 if lines else 1


if __name__ == "__main__":
    sys.exit(main())
