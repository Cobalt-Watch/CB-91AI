#!/usr/bin/env python3
"""Measure the radio link of a CB-91AI at a fixed spot, both ways, for a before and after comparison.

    python firmware/tools/ble_rssi.py --address AA:BB:CC:DD:EE:FF --label "before VSS_PA, 1 m"

1. Connects and reads, --reads times, the RSSI of this PC as the board hears it (`cb91ai rssi`,
   the READ_RSSI of its controller): the receive path of the board. Fast advertising is held for a
   few minutes on the way out.
2. Disconnects and listens to its advertising for --scan seconds: the RSSI of the board as this PC
   hears it, the transmit path of the board.

Median and spread (10th and 90th percentiles) of both go to firmware/logs/rssi-<date>.txt, with the
label and the status line of the board, and to one line per run in firmware/logs/rssi-summary.txt,
so that runs of different days compare side by side. The spot matters: a few centimetres, a turn of
the watch or a person walking by change a reading by several dB. Keep the board in the same place
and orientation for every run, off the wrist, and make several runs.

Written for V2-23 (range at a known distance, before and after the VSS_PA rework of V2-33).
Requirements: `pip install bleak` (the board side goes through ble_shell.py).
"""

import argparse
import asyncio
import datetime
import pathlib
import re
import statistics
import sys

from bleak import BleakScanner

import ble_shell

LOG_DIR = ble_shell.LOG_DIR
SUMMARY = LOG_DIR / "rssi-summary.txt"
REPLY_TIMEOUT_S = 10.0


def spread(values):
    """Median and the 10th and 90th percentiles, as text."""
    if not values:
        return "nothing received"
    if len(values) < 2:
        return f"{values[0]} dBm (one value)"
    deciles = statistics.quantiles(values, n=10)
    return (f"median {statistics.median(values):.1f} dBm, 10 % {deciles[0]:.1f}, "
            f"90 % {deciles[-1]:.1f}, {len(values)} values")


async def board_side(address, reads, interval):
    """RSSI of this PC as the board hears it, over one connection; the status line too."""
    client = await ble_shell.connect(address)
    replies = ble_shell.Replies()
    values = []
    status = ""

    async def run(command):
        replies.reset()
        await client.write_gatt_char(ble_shell.COMMAND_CHAR, command.encode(), response=True)
        await asyncio.wait_for(replies.done.wait(), REPLY_TIMEOUT_S)
        return bytes(replies.text).decode(errors="replace")

    try:
        await client.start_notify(ble_shell.COMMAND_CHAR, replies.on_command)
        # Fast advertising for the scan that follows (100 to 150 ms)
        await run("adv fast 5")
        for _ in range(reads):
            match = re.search(r"rssi (-?\d+) dBm", await run("rssi"))
            if match:
                values.append(int(match[1]))
            await asyncio.sleep(interval)
        status = bytes(await client.read_gatt_char(ble_shell.STATUS_CHAR)).decode(errors="replace")
    finally:
        try:
            await asyncio.wait_for(client.disconnect(), 10)
        except Exception:  # noqa: BLE001 - the link may already be gone
            pass
    return values, status


async def pc_side(address, seconds):
    """RSSI of the board's advertising as this PC hears it."""
    values = []

    def on_advertising(device, advertisement):
        if device.address.upper() == address:
            values.append(advertisement.rssi)

    async with BleakScanner(detection_callback=on_advertising):
        await asyncio.sleep(seconds)
    return values


async def measure(args):
    address = args.address.upper()
    if args.pc_only:
        board, board_text, status = [], "not measured (--pc-only)", "no status line"
    else:
        ble_shell.out(f"run \"{args.label}\": the board hears this PC ({args.reads} readings)")
        board, status = await board_side(address, args.reads, args.interval)
        board_text = spread(board)
        ble_shell.out(f"  board hears the PC: {board_text}")
        ble_shell.out(f"  status: {status}")
    ble_shell.out(f"run \"{args.label}\": this PC hears the board ({args.scan:.0f} s of advertising)")
    pc = await pc_side(address, args.scan)
    ble_shell.out(f"  PC hears the board: {spread(pc)}")
    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    with open(SUMMARY, "a", encoding="utf-8") as summary:
        summary.write(f"{stamp} | {args.label} | board hears the PC: {board_text} | "
                      f"PC hears the board: {spread(pc)} | {status}\n")
    ble_shell.out(f"summary appended to {SUMMARY}")
    return 0 if pc and (board or args.pc_only) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--address", required=True, help="Bluetooth address of the board")
    parser.add_argument("--label", required=True,
                        help='what this run is, e.g. "before VSS_PA, 1 m, flat, 12 h to the PC"')
    parser.add_argument("--reads", type=int, default=20, help="RSSI readings on the board side")
    parser.add_argument("--interval", type=float, default=0.5, help="seconds between readings")
    parser.add_argument("--scan", type=float, default=60.0, help="seconds of listening on the PC side")
    parser.add_argument("--pc-only", action="store_true",
                        help="the PC side only: the watch application has no shell to read the "
                             "board side (its transmit power: cobalt_link.py txpower)")
    args = parser.parse_args()
    LOG_DIR.mkdir(exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    with open(LOG_DIR / f"rssi-{stamp}.txt", "w", encoding="utf-8") as log:
        ble_shell._log_file = log  # one log for both sides
        try:
            return asyncio.run(measure(args))
        except RuntimeError as exc:
            ble_shell.out(str(exc))
            return 2


if __name__ == "__main__":
    sys.exit(main())
