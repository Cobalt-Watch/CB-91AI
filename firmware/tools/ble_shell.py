#!/usr/bin/env python3
"""Run the self-test's `cb91ai` commands over Bluetooth LE, on a board sealed in its watch.

    python firmware/tools/ble_shell.py --address AA:BB:CC:DD:EE:FF "keys probe 300" "lcd full"
    python firmware/tools/ble_shell.py --address AA:BB:CC:DD:EE:FF --wav voice.wav "rec get"
    python firmware/tools/ble_shell.py --address AA:BB:CC:DD:EE:FF --wav "k1-{n}.wav" "take get 1" "take get 2"
    python firmware/tools/ble_shell.py --address AA:BB:CC:DD:EE:FF --status "arm"

Each argument is one command, without the "cb91ai" prefix; they run in order on one
connection and the output of each is printed as it comes. The board runs them in its
main loop (src/remote.c, self-test 0.1.46 and later), and only `cb91ai` commands: the
generic shells of the bench stay out of reach of the radio.

--wav writes what `rec get` or `take get <n>` streams (16 kHz, 16-bit, mono) as a WAV file,
once its length and CRC match what the board announced; "{n}" in the path becomes the number
of the take, so that one connection fetches all five takes of the storage flash (self-test
0.1.52 and later). --status reads the status line at the end, on the
same connection. From this PC a watch in slow advertising takes 10 to 70 s to connect:
`adv fast <minutes>` keeps it advertising fast for a test session, and a press on a case
button does the same for 30 s.

Requirements: `pip install bleak`. The output is also written, line by line, to
firmware/logs/bleshell-<date>.txt.
"""

import argparse
import asyncio
import datetime
import pathlib
import re
import struct
import sys
import time
import wave
import zlib

from bleak import BleakClient, BleakScanner

FIRMWARE_DIR = pathlib.Path(__file__).resolve().parents[1]
LOG_DIR = FIRMWARE_DIR / "logs"
COMMAND_CHAR = "c0b91a11-1db7-4cd3-868b-8a527460db61"
DATA_CHAR = "c0b91a12-1db7-4cd3-868b-8a527460db61"
STATUS_CHAR = "c0b91a01-1db7-4cd3-868b-8a527460db61"
END_MARK = 0x04
CONNECT_ATTEMPTS = 3
SCAN_TIMEOUT_S = 90.0
SAMPLE_RATE_HZ = 16000
TRAILING_DATA_S = 5.0

_log_file = None


def out(text=""):
    """Print, and keep a copy in the log file."""
    print(text, flush=True)
    if _log_file:
        _log_file.write(text + "\n")
        _log_file.flush()


class Replies:
    """What the board sends back: the text of the running command, and data chunks."""

    def __init__(self):
        self.text = bytearray()
        self.code = None
        self.done = asyncio.Event()
        self.chunks = {}

    def reset(self):
        self.text.clear()
        self.code = None
        self.done.clear()
        self.chunks.clear()

    def on_command(self, _characteristic, payload):
        if payload and payload[0] == END_MARK:
            try:
                self.code = int(bytes(payload[1:]).decode() or "0")
            except ValueError:
                self.code = None
            self.done.set()
        else:
            self.text += payload

    def on_data(self, _characteristic, payload):
        if len(payload) >= 4:
            offset = struct.unpack_from("<I", payload)[0]
            self.chunks[offset] = bytes(payload[4:])

    def data(self):
        """The chunks put back in order; ValueError on a gap."""
        joined = bytearray()
        for offset in sorted(self.chunks):
            if offset != len(joined):
                raise ValueError(f"gap in the stream: {len(joined)} bytes, then offset {offset}")
            joined += self.chunks[offset]
        return bytes(joined)


async def connect(address):
    last = None
    for attempt in range(1, CONNECT_ATTEMPTS + 1):
        started = time.monotonic()
        try:
            device = await BleakScanner.find_device_by_address(address, timeout=SCAN_TIMEOUT_S)
            if device is None:
                raise RuntimeError(f"not seen advertising in {SCAN_TIMEOUT_S:.0f} s")
            client = BleakClient(device, timeout=30.0, winrt={"use_cached_services": False})
            await client.connect()
            out(f"connected to {address} in {time.monotonic() - started:.1f} s, "
                f"MTU {client.mtu_size}")
            return client
        except Exception as exc:  # noqa: BLE001 - every failure means "try again"
            last = exc
            out(f"connection attempt {attempt}/{CONNECT_ATTEMPTS} failed "
                f"({type(exc).__name__}: {exc})")
            await asyncio.sleep(2.0)
    raise RuntimeError(f"no connection to {address}: {last}")


def write_wav(path, samples, text, written):
    """Check the stream against `rec get: N of M bytes, crc32 X` (or `take get <n>: ...`),
    then write the WAV; "{n}" in the path becomes the number of the take."""
    match = re.search(r"(?:rec|take) get(?: (\d+))?: (-?\d+) of (\d+) bytes, crc32 ([0-9a-f]{8})",
                      text)
    if not match:
        raise ValueError("no `rec get` or `take get` summary in the reply")
    take, sent, total, crc = match[1], int(match[2]), int(match[3]), int(match[4], 16)
    if sent != total or len(samples) != total:
        raise ValueError(f"{len(samples)} bytes received, {sent} sent of {total}")
    if zlib.crc32(samples) != crc:
        raise ValueError(f"CRC mismatch: {zlib.crc32(samples):08x} received, {crc:08x} sent")
    if "{n}" in str(path):
        path = str(path).replace("{n}", take or "rec")
    elif written:
        raise ValueError(f"a second stream would overwrite {path}: put {{n}} in the path")
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE_HZ)
        wav.writeframes(samples)
    seconds = len(samples) / 2 / SAMPLE_RATE_HZ
    out(f"wrote {path} ({len(samples)} bytes, {seconds:.2f} s), CRC checked")
    return path


async def trailing_data(replies, text):
    """Wait for the last data notifications of a stream. The end of the command's output comes
    on another characteristic, and Windows may hand it over before the last chunks of data:
    on 2026-09-23 two takes of 192000 bytes were cut short by 556 and 3094 bytes that way."""
    match = re.search(r"(?:rec|take) get(?: \d+)?: -?\d+ of (\d+) bytes", text)
    if not match:
        return
    total = int(match[1])
    deadline = time.monotonic() + TRAILING_DATA_S
    while sum(len(c) for c in replies.chunks.values()) < total and time.monotonic() < deadline:
        await asyncio.sleep(0.02)


async def session(args):
    client = await connect(args.address)
    replies = Replies()
    written = []
    code = 0
    try:
        await client.start_notify(COMMAND_CHAR, replies.on_command)
        if args.wav:
            await client.start_notify(DATA_CHAR, replies.on_data)
        for command in args.commands:
            replies.reset()
            out(f"> {command}")
            started = time.monotonic()
            await client.write_gatt_char(COMMAND_CHAR, command.encode(), response=True)
            try:
                await asyncio.wait_for(replies.done.wait(), args.timeout)
            except asyncio.TimeoutError:
                out(f"no end of reply after {args.timeout:.0f} s")
                code = 1
                break
            text = bytes(replies.text).decode(errors="replace")
            for line in text.replace("\r\n", "\n").strip("\n").split("\n"):
                out(line)
            out(f"(return code {replies.code}, {time.monotonic() - started:.1f} s)")
            if replies.code:
                code = 1
            if args.wav:
                await trailing_data(replies, text)
            if args.wav and replies.chunks:
                try:
                    written.append(write_wav(args.wav, replies.data(), text, written))
                except ValueError as exc:
                    out(f"WAV not written: {exc}")
                    code = 1
        if args.status:
            value = await client.read_gatt_char(STATUS_CHAR)
            out(f"status: {bytes(value).decode(errors='replace')}")
    finally:
        try:
            await asyncio.wait_for(client.disconnect(), 10)
        except Exception:  # noqa: BLE001 - the link may already be gone
            pass
    return code


def main():
    global _log_file
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("commands", nargs="*", help='commands without "cb91ai", e.g. "keys"')
    parser.add_argument("--address", required=True,
                        help="Bluetooth address of the board (two boards may advertise)")
    parser.add_argument("--wav", type=pathlib.Path,
                        help='write what `rec get` or `take get <n>` streams here; "{n}" in the '
                             "path becomes the number of the take")
    parser.add_argument("--status", action="store_true", help="read the status line at the end")
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="seconds to wait for the end of each reply")
    args = parser.parse_args()
    if not args.commands and not args.status:
        parser.error("nothing to do: give commands, or --status")
    LOG_DIR.mkdir(exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    with open(LOG_DIR / f"bleshell-{stamp}.txt", "w", encoding="utf-8") as log:
        _log_file = log
        try:
            code = asyncio.run(session(args))
        except RuntimeError as exc:
            out(str(exc))
            code = 2
    return code


if __name__ == "__main__":
    sys.exit(main())
