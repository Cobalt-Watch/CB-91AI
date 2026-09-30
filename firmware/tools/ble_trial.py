#!/usr/bin/env python3
"""Advertising trials of lot N1a from the PC: play the phone that keeps a pending connection.

    python firmware/tools/ble_trial.py --address AA:BB:CC:DD:EE:FF --auto 5 --profile apple
    python firmware/tools/ble_trial.py --address AA:BB:CC:DD:EE:FF --count 10

The self-test 0.1.53 and later has a trial mode (`cb91ai adv trial apple|link`, src/ble.c): its
radio keeps silent between trials, and a press on a case button starts a burst of advertising
that the watch times, from its first packet to the connection of a phone. This tool connects as
soon as it hears the watch, reads the result the watch measured (trial characteristic
C0B91A03), then lets the watch drop the link, as a product session ends, and starts again.

--auto N runs N trials with no press: the tool puts the watch in trial mode, and as soon as it
is connected asks for the next burst (`adv trial go`, which starts when the link ends), so that
a failed read does not leave the watch silent; after the last trial it takes one more burst to
turn trial mode off. Without --auto the watch must already be in trial mode, and the wearer
presses a button for each trial; the tool stops after --count trials. A result already counted,
or none (trial mode off: normal advertising), is not counted; three failures end the series.

The scanner of this PC listens about a tenth of the time: its delays are those of a slow phone
and say nothing of real phones. What they prove is the chain: burst, connection, timing, end of
the session, next burst. The phones are measured by the apps of N1a, or by nRF Connect with
"auto connect" on Android, the watch showing each delay on its glass.

Requirements: `pip install bleak`. Output also in firmware/logs/bletrial-<date>.txt.
"""

import argparse
import asyncio
import datetime
import pathlib
import statistics
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

FIRMWARE_DIR = pathlib.Path(__file__).resolve().parents[1]
LOG_DIR = FIRMWARE_DIR / "logs"
TRIAL_CHAR = "c0b91a03-1db7-4cd3-868b-8a527460db61"
COMMAND_CHAR = "c0b91a11-1db7-4cd3-868b-8a527460db61"
END_MARK = 0x04
NONE = 0xFFFFFFFF
PROFILES = ("apple", "link")
# Interval of each step of each profile, ms (src/ble.c)
STEP_MS = {"apple": (20.0, 152.5, 1022.5), "link": (100.0, 1000.0)}
# The longest window (apple, 5 min), plus the time to fetch the result
SCAN_TIMEOUT_S = 330.0
DROP_TIMEOUT_S = 40.0
MAX_FAILURES = 3

_log_file = None


def out(text=""):
    """Print, and keep a copy in the log file."""
    print(text, flush=True)
    if _log_file:
        _log_file.write(text + "\n")
        _log_file.flush()


def parse_trial(value):
    """The 16 bytes of the trial characteristic, as a dict."""
    fmt, number, profile, step, connect_ms, read_ms, interval, timeout = struct.unpack(
        "<BBBBIIHH", bytes(value))
    if fmt != 1:
        raise ValueError(f"trial record of format {fmt}, this tool knows format 1")
    return {
        "number": number,
        "profile": PROFILES[profile] if profile < len(PROFILES) else f"#{profile}",
        "step": step,
        "connect_ms": None if connect_ms == NONE else connect_ms,
        "read_ms": None if read_ms == NONE else read_ms,
        "interval_ms": interval * 1.25,
        "timeout_ms": timeout * 10,
    }


def describe(trial):
    if trial["number"] == 0:
        return "no trial yet"
    if trial["connect_ms"] is None:
        return f"trial {trial['number']} ({trial['profile']}): no connection"
    step_ms = STEP_MS.get(trial["profile"], ())
    step_text = (f"{step_ms[trial['step']]:g} ms" if trial["step"] < len(step_ms)
                 else "?")
    read = ("no read" if trial["read_ms"] is None
            else f"first read {trial['read_ms'] / 1000:.3f} s after")
    return (f"trial {trial['number']} ({trial['profile']}): connected {trial['connect_ms'] / 1000:.3f} s "
            f"after the first packet, step {trial['step'] + 1} ({step_text}), {read}; "
            f"link {trial['interval_ms']:.2f} ms, supervision {trial['timeout_ms']} ms")


async def command(client, line, timeout=15.0):
    """Run one `cb91ai` command over the test service (src/remote.c); returns its code."""
    done = asyncio.Event()
    reply = {"text": bytearray(), "code": None}

    def on_notify(_characteristic, payload):
        if payload and payload[0] == END_MARK:
            try:
                reply["code"] = int(bytes(payload[1:]).decode() or "0")
            except ValueError:
                reply["code"] = None
            done.set()
        else:
            reply["text"] += payload

    await client.start_notify(COMMAND_CHAR, on_notify)
    try:
        await client.write_gatt_char(COMMAND_CHAR, line.encode(), response=True)
        await asyncio.wait_for(done.wait(), timeout)
    finally:
        try:
            await client.stop_notify(COMMAND_CHAR)
        except Exception:  # noqa: BLE001 - the link may be going
            pass
    text = bytes(reply["text"]).decode(errors="replace").strip()
    out(f"> {line}")
    for row in text.replace("\r\n", "\n").split("\n"):
        if row:
            out(f"  {row}")
    return reply["code"]


async def meet(address, timeout):
    """Scan until the watch advertises, then connect: the pending connection of a phone.
    Returns (client, event set when the link drops, seconds until seen, seconds until
    connected), or Nones when the watch was not heard."""
    started = time.monotonic()
    device = await BleakScanner.find_device_by_address(address, timeout=timeout)
    if device is None:
        return None, None, None, None
    seen = time.monotonic() - started
    dropped = asyncio.Event()
    client = BleakClient(device, timeout=20.0, disconnected_callback=lambda _c: dropped.set(),
                         winrt={"use_cached_services": False})
    await client.connect()
    return client, dropped, seen, time.monotonic() - started


async def leave(client):
    try:
        await asyncio.wait_for(client.disconnect(), 10)
    except Exception:  # noqa: BLE001 - the link may already be gone
        pass


async def session(args):
    results = []
    auto = args.auto > 0
    total = args.auto if auto else args.count
    last_number = None
    failures = 0

    if auto:
        out(f"putting the watch in trial mode ({args.profile}); if it is in trial mode already, "
            "press a case button so that it advertises")
        client, _, _, _ = await meet(args.address, SCAN_TIMEOUT_S)
        if client is None:
            out("the watch was not heard")
            return 2
        try:
            if (await command(client, f"adv trial {args.profile} {args.minutes}") != 0
                    or await command(client, "adv trial go") != 0):
                return 2
        finally:
            await leave(client)

    while len(results) < total and failures < MAX_FAILURES:
        if not auto:
            out(f"trial {len(results) + 1}/{total}: press a case button of the watch")
        try:
            client, dropped, seen, joined = await meet(args.address, SCAN_TIMEOUT_S)
        except Exception as exc:  # noqa: BLE001 - Windows may give up; scan again
            failures += 1
            out(f"connection failed ({type(exc).__name__}: {exc}); scanning again")
            continue
        if client is None:
            out(f"the watch was not heard in {SCAN_TIMEOUT_S:.0f} s")
            break
        try:
            if auto and await command(client, "adv trial go") != 0:
                # First of all: the next burst starts when this link ends, even
                # if what follows fails. Refused: trial mode is off.
                out("the watch refused the next burst: trial mode is off")
                break
            trial = parse_trial(await client.read_gatt_char(TRIAL_CHAR))
            if trial["number"] == 0 or trial["number"] == last_number:
                # Not a trial connection: normal advertising, trial mode off
                failures += 1
                out(f"no new trial on the watch ({describe(trial)})")
            else:
                last_number = trial["number"]
                results.append(trial)
                out(f"{describe(trial)}. PC: heard after {seen:.1f} s, "
                    f"connected after {joined:.1f} s")
            if not auto:
                # As a phone would: let the watch end the session
                try:
                    await asyncio.wait_for(dropped.wait(), DROP_TIMEOUT_S)
                    out("  the watch dropped the link")
                except asyncio.TimeoutError:
                    out(f"  still connected after {DROP_TIMEOUT_S:.0f} s: leaving")
        except Exception as exc:  # noqa: BLE001 - count it and carry on
            failures += 1
            out(f"trial failed ({type(exc).__name__}: {exc})")
        finally:
            await leave(client)

    if auto:
        # The last `go` left a burst pending: take it to turn trial mode off
        client, _, _, _ = await meet(args.address, SCAN_TIMEOUT_S)
        if client is not None:
            try:
                await command(client, "adv trial off")
            finally:
                await leave(client)
        else:
            out("trial mode left on: it lapses by itself, or `adv trial off`")

    delays = [t["connect_ms"] / 1000 for t in results if t["connect_ms"] is not None]
    reads = [t["read_ms"] / 1000 for t in results if t["read_ms"] is not None]
    if delays:
        out(f"{len(delays)} connection(s): delay after the first packet min {min(delays):.3f} s, "
            f"median {statistics.median(delays):.3f} s, max {max(delays):.3f} s")
    if reads:
        out(f"first read after the connection: median {statistics.median(reads):.3f} s")
    return 0 if len(delays) == total else 1


def main():
    global _log_file
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--address", required=True,
                        help="Bluetooth address of the board (two boards may advertise)")
    parser.add_argument("--auto", type=int, default=0, metavar="N",
                        help="N trials with no press: the tool sets trial mode, asks for each "
                             "burst, and turns trial mode off at the end")
    parser.add_argument("--count", type=int, default=5,
                        help="trials to wait for when the wearer presses (without --auto)")
    parser.add_argument("--profile", choices=PROFILES, default="apple",
                        help="burst profile set by --auto")
    parser.add_argument("--minutes", type=int, default=30,
                        help="trial mode lapses after this long without a trial (--auto)")
    args = parser.parse_args()
    LOG_DIR.mkdir(exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    with open(LOG_DIR / f"bletrial-{stamp}.txt", "w", encoding="utf-8") as log:
        _log_file = log
        try:
            code = asyncio.run(session(args))
        except Exception as exc:  # noqa: BLE001 - report and keep the log
            out(f"{type(exc).__name__}: {exc}")
            code = 2
    return code


if __name__ == "__main__":
    sys.exit(main())
