#!/usr/bin/env python3
"""Scan for the CB-91AI over Bluetooth LE, report the RSSI, connect and read it.

Usage:
    python firmware/tools/ble_test.py                       # scan 8 s, connect, read services
    python firmware/tools/ble_test.py --seconds 20 --no-connect   # RSSI survey only (V2-23)
    python firmware/tools/ble_test.py --name CB-91AI

Requirements: `pip install bleak` and a Bluetooth adapter on the PC. The
result is printed and appended to firmware/logs/ble-<date>.txt.
"""

import argparse
import asyncio
import datetime
import pathlib
import sys

from bleak import BleakClient, BleakScanner

FIRMWARE_DIR = pathlib.Path(__file__).resolve().parents[1]
LOG_DIR = FIRMWARE_DIR / "logs"
CHARACTERISTICS = [
    ("manufacturer", "2a29"),
    ("model", "2a24"),
    ("firmware revision", "2a26"),
    ("hardware revision", "2a27"),
    ("battery level", "2a19"),
]


def uuid16(short):
    return f"0000{short}-0000-1000-8000-00805f9b34fb"


async def run(args, out):
    out(f"Scanning {args.seconds} s for '{args.name}'...")
    found = {}

    def on_advertisement(device, adv):
        if adv.local_name == args.name or device.name == args.name:
            found.setdefault(device.address, []).append(adv.rssi)

    async with BleakScanner(on_advertisement):
        await asyncio.sleep(args.seconds)

    if not found:
        out(f"'{args.name}' not seen: check that the firmware advertises and that the PC Bluetooth is on.")
        return 1
    for address, rssis in found.items():
        out(f"{args.name} at {address}: {len(rssis)} advertisements, RSSI min/avg/max = "
            f"{min(rssis)}/{sum(rssis) // len(rssis)}/{max(rssis)} dBm")
    if args.no_connect:
        return 0

    address = next(iter(found))
    out(f"Connecting to {address}...")
    async with BleakClient(address, timeout=20.0) as client:
        out(f"connected: {client.is_connected}")
        for label, short in CHARACTERISTICS:
            try:
                value = await client.read_gatt_char(uuid16(short))
                text = f"{value[0]} %" if short == "2a19" else value.decode("utf-8", "replace")
                out(f"  {label}: {text}")
            except Exception as exc:  # noqa: BLE001 - report and carry on
                out(f"  {label}: error {exc}")
        out("  services: " + ", ".join(service.uuid for service in client.services))
    out("disconnected")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", default="CB-91AI", help="advertised device name")
    parser.add_argument("--seconds", type=float, default=8.0, help="scan duration")
    parser.add_argument("--no-connect", action="store_true", help="scan only, do not connect")
    args = parser.parse_args()

    LOG_DIR.mkdir(exist_ok=True)
    log_path = LOG_DIR / f"ble-{datetime.datetime.now():%Y%m%d-%H%M%S}.txt"
    with open(log_path, "w", encoding="utf-8") as log:
        def out(text):
            print(text, flush=True)
            log.write(text + "\n")
        result = asyncio.run(run(args, out))
    print(f"log saved to {log_path}")
    return result


if __name__ == "__main__":
    sys.exit(main())
