#!/usr/bin/env python3
"""Update the CB-91AI firmware over USB: upload, mark for test, reset, verify.

Uses the smpmgr command line (`pip install smpmgr`) on the MCUmgr CDC-ACM port
of the board. `smpmgr upgrade` alone does not mark the image for the next boot
in this single-image swap setup (it only does so for slot numbers other than
0), hence this script: it uploads the signed image, reads its hash back from
the secondary slot, marks it for test, resets the board, waits for the swap
(about 30 s for a 240 kB image), then shows the version that boots and whether
the self-test confirmed it. If the new image fails to boot or to confirm,
MCUboot reverts to the previous one at the next reset.

Usage:
    python firmware/tools/usb_update.py firmware/build/app/zephyr/zephyr.signed.bin
    python firmware/tools/usb_update.py --port COM10 firmware/build/app/zephyr/zephyr.signed.bin
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from usb_console import DEFAULT_PID, DEFAULT_VID, find_port  # noqa: E402

ENV = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUTF8="1")


def smp(port, *args, timeout=120):
    cmd = ["smpmgr", "--port", port, *args]
    print("$ " + " ".join(cmd), flush=True)
    result = subprocess.run(cmd, env=ENV, capture_output=True, text=True, errors="replace", timeout=timeout)
    return result.returncode, result.stdout + result.stderr


def image_states(port):
    """Return {slot: {"version", "hash", "active", "confirmed", "pending"}} from `image state-read`."""
    code, out = smp(port, "image", "state-read")
    flat = re.sub(r"\s+", "", out)
    states = {}
    for m in re.finditer(r"slot=(\d),version='([^']*)',image=[^,]*,hash=HashBytes\('([0-9A-Fa-f]+)'\),"
                         r"bootable=(\w+),pending=(\w+),confirmed=(\w+),active=(\w+)", flat):
        states[int(m.group(1))] = {"version": m.group(2), "hash": m.group(3), "pending": m.group(5) == "True",
                                   "confirmed": m.group(6) == "True", "active": m.group(7) == "True"}
    return states


def wait_for_port(vid, pid, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        port = find_port(vid, pid, 1)
        if port:
            return port
        time.sleep(1.0)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", help="signed image, e.g. build/app/zephyr/zephyr.signed.bin")
    parser.add_argument("--port", help="MCUmgr COM port (default: the board's second CDC-ACM port)")
    parser.add_argument("--vid", type=lambda s: int(s, 16), default=DEFAULT_VID)
    parser.add_argument("--pid", type=lambda s: int(s, 16), default=DEFAULT_PID)
    parser.add_argument("--swap-timeout", type=float, default=90, help="seconds to wait for the board after reset")
    args = parser.parse_args()

    image = pathlib.Path(args.image)
    if not image.is_file():
        print(f"image not found: {image}")
        return 2
    port = args.port or find_port(args.vid, args.pid, 1)
    if not port:
        print("MCUmgr port not found: is the board on USB and running a firmware with two CDC-ACM ports?")
        return 1

    before = image_states(port)
    active = next((s for s in before.values() if s["active"]), None)
    print(f"running: {active['version'] if active else '?'} on {port}")

    code, out = smp(port, "image", "upload", str(image), timeout=600)
    if code != 0:
        print(out[-2000:])
        print("upload failed")
        return 1
    print("upload done")

    after = image_states(port)
    candidate = next((s for slot, s in sorted(after.items()) if not s["active"]), None)
    if candidate is None:
        print("no image found in the secondary slot after the upload")
        return 1
    print(f"uploaded image: version {candidate['version']}, hash {candidate['hash'][:16]}...")

    code, out = smp(port, "image", "state-write", candidate["hash"])
    if code != 0 or not image_states(port).get(1, {}).get("pending"):
        print(out[-1500:])
        print("could not mark the image for test")
        return 1
    print("marked for test, resetting")
    smp(port, "os", "reset")

    print(f"waiting for the swap (up to {args.swap_timeout:.0f} s)...", flush=True)
    time.sleep(5)
    port = wait_for_port(args.vid, args.pid, args.swap_timeout)
    if not port:
        print("the board did not come back on USB: power-cycle it, MCUboot reverts if the image failed")
        return 1
    time.sleep(8)  # let the self-test run and confirm the image
    final = image_states(port)
    running = next((s for s in final.values() if s["active"]), None)
    if running is None:
        print("could not read the image state after the reset")
        return 1
    print(f"running now: {running['version']} ({'confirmed' if running['confirmed'] else 'NOT confirmed, will revert'})")
    return 0 if running["version"] == candidate["version"] and running["confirmed"] else 1


if __name__ == "__main__":
    sys.exit(main())
