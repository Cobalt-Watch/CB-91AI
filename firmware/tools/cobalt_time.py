#!/usr/bin/env python3
"""Read, set and check the wall clock of a CB-91AI over Bluetooth LE (EI-03, EF-02, EV-10).

    python firmware/tools/cobalt_time.py                 # read: error against network time, correction
    python firmware/tools/cobalt_time.py --set           # set the time (and let the watch calibrate)
    python firmware/tools/cobalt_time.py --watch 120     # follow the error for 120 min

The reference is network time (SNTP), not the clock of the PC, which drifts when
Windows follows no time server. Setting the time twice, hours apart and with no
reset in between, is all the watch needs to measure its own crystal and refine
its correction: `--set` tonight and `--set` tomorrow morning calibrate it within
a few ppm. The phone application will do the same at each connection.

A read is stamped by the watch somewhere between the request and the answer:
on the watch application, whose link has a peripheral latency of 30, a request
waits up to 0.9 s for the watch to listen, and the PC's stack adds its own
delays. So each measure takes several reads: each one bounds the error between
the watch's clock minus the end of the read and minus its start, and the
tightest bounds keep the error within some tens of ms.

Every measure goes to a log, firmware/logs/clock-<address>.csv, where the
TIME of cobalt_link.py goes too, and gives the drift since the last setting
logged, by either tool, or the one `--since` names: what the watch gained on
network time, in ppm and in seconds a day, and the error of its crystal that
follows. Seven days of measures with no setting in between are EV-10 (ENF-08:
2 s a day at most). The watch application has the time characteristic in its
development build only (debug service); cobalt_link.py runs with --no-time
meanwhile.

Requirements: `pip install bleak`.
"""

import argparse
import asyncio
import csv
import datetime
import pathlib
import struct
import sys
import time

from lfxo_drift import network_offset

TIME_CHARACTERISTIC = "c0b91a02-1db7-4cd3-868b-8a527460db61"
SMP_SERVICE = "8d53dc1d-1db7-4cd3-868b-8a527460aa84"
FLAGS = {1: "set", 2: "approximate (restored after a reset)", 4: "calibrated"}
FLAG_APPROXIMATE = 2
LOGS_DIR = pathlib.Path(__file__).resolve().parents[1] / "logs"
LOG_FIELDS = ("pc_utc", "event", "reference_ms", "watch_ms", "error_ms", "ppb", "flags",
              "uptime_ms", "round_trip_ms", "low_ms", "high_ms")
SETTINGS = ("after-set", "time")  # the events that set the watch's clock
READS = 8
# From the watch's stamp to the end of the read: at least one connection interval of the
# watch application (30 ms), the answer going out at the event after the request
RESPONSE_MS = 30
# Shorter than this, the tens of ms of a measure outweigh the drift
DRIFT_MIN_S = 600
EV10_DAYS = 7
ENF08_S_PER_DAY = 2.0


async def find(address, timeout=40.0):
    from bleak import BleakScanner
    if address:
        return await BleakScanner.find_device_by_address(address, timeout=timeout)
    return await BleakScanner.find_device_by_filter(
        lambda d, adv: SMP_SERVICE in [u.lower() for u in (adv.service_uuids or [])],
        timeout=timeout)


async def measure(client, ntp_offset, reads=READS):
    """(error of the watch in ms, or None while its time is not set; decoded fields). Each
    read bounds the error: the watch's clock minus the end of the read is a floor, minus its
    start a ceiling; the error is the highest floor plus the answer's own way, within the
    lowest ceiling."""
    low = high = None
    shortest = None
    for _ in range(reads):
        before = time.time()
        raw = bytes(await client.read_gatt_char(TIME_CHARACTERISTIC))
        after = time.time()
        unix_ms, uptime_ms, ppb, tz, flags = struct.unpack("<qqihB", raw[:23])
        fields = {"unix_ms": unix_ms, "uptime_ms": uptime_ms, "ppb": ppb, "tz": tz,
                  "flags": flags, "reference_ms": (after + ntp_offset) * 1000}
        shortest = min(shortest or after - before, after - before)
        fields["round_trip_ms"] = shortest * 1000
        if not unix_ms:
            fields["low_ms"] = fields["high_ms"] = None
            return None, fields
        floor = unix_ms - (after + ntp_offset) * 1000
        ceiling = unix_ms - (before + ntp_offset) * 1000
        low = floor if low is None else max(low, floor)
        high = ceiling if high is None else min(high, ceiling)
    fields["low_ms"], fields["high_ms"] = low, high
    # Bounds that cross say the clock moved during the measure (a setting from elsewhere)
    error_ms = min(low + RESPONSE_MS, high) if high >= low else (low + high) / 2
    return error_ms, fields


def describe(error_ms, fields):
    flags = ", ".join(text for bit, text in FLAGS.items() if fields["flags"] & bit) or "not set"
    if fields["unix_ms"]:
        local = datetime.datetime.fromtimestamp(fields["unix_ms"] / 1000, datetime.timezone.utc) \
            + datetime.timedelta(minutes=fields["tz"])
        crossed = " (bounds crossed: the clock moved?)" if fields["high_ms"] < fields["low_ms"] else ""
        print(f"watch: {local:%Y-%m-%d %H:%M:%S} local (UTC{fields['tz']:+d} min), "
              f"error {error_ms:+.0f} ms against network time, between "
              f"{fields['low_ms']:+.0f} and {fields['high_ms']:+.0f}{crossed}")
    else:
        print("watch: time not set")
    print(f"       correction {fields['ppb']:+d} ppb ({fields['ppb'] / 1000:+.1f} ppm), "
          f"uptime {fields['uptime_ms'] / 1000:.0f} s, {flags}; "
          f"shortest read {fields['round_trip_ms']:.0f} ms")


def log_file(address, path=None):
    if path:
        return pathlib.Path(path)
    return LOGS_DIR / f"clock-{(address or 'any').replace(':', '').upper()}.csv"


def _append(path, row):
    path.parent.mkdir(parents=True, exist_ok=True)
    new = not path.exists()
    with path.open("a", newline="", encoding="utf-8") as out:
        writer = csv.writer(out)
        if new:
            writer.writerow(LOG_FIELDS)
        writer.writerow([datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")]
                        + row)


def _ms(value):
    return "" if value is None else f"{value:.0f}"


def append(path, event, error_ms, fields):
    """One measure in the log, under a header when the file is new."""
    _append(path, [event, _ms(fields["reference_ms"]), fields["unix_ms"], _ms(error_ms),
                   fields["ppb"], fields["flags"], fields["uptime_ms"],
                   _ms(fields["round_trip_ms"]), _ms(fields["low_ms"]), _ms(fields["high_ms"])])


def log_setting(address, utc_ms, network):
    """A TIME of cobalt_link.py in the watch's clock log: a setting, where the next drift
    starts, its error nil when it carried network time and unknown otherwise."""
    _append(log_file(address), ["time", _ms(utc_ms), utc_ms, "0" if network else "",
                                "", "", "", "", "", ""])


def parse_since(text):
    """Network time in ms of a setting named by hand: a date and time, local unless it says
    otherwise ("2026-09-26 23:39:04.542"), or ms since 1970, as a TIME message carries them."""
    if text.isdigit():
        return float(text)
    when = datetime.datetime.fromisoformat(text)
    if when.tzinfo is None:
        when = when.astimezone()
    return when.timestamp() * 1000


def baseline(path, since):
    """(network time in ms, error in ms or None) of the setting the drift counts from:
    --since, the watch set to network time then; else the last setting logged, by --set or
    by a TIME of cobalt_link.py; None without either."""
    if since:
        return parse_since(since), 0.0
    if not path.exists():
        return None
    start = None
    with path.open(newline="", encoding="utf-8") as log:
        for row in csv.DictReader(log):
            if row.get("event") in SETTINGS:
                start = (float(row["reference_ms"]),
                         float(row["error_ms"]) if row.get("error_ms") else None)
    return start


def report_drift(error_ms, fields, start):
    """What the watch gained since the setting that started the run, and what that says. The
    correction changes only at a setting: the one in use now ran the whole run."""
    if start is None or error_ms is None:
        return
    when = datetime.datetime.fromtimestamp(start[0] / 1000).strftime("%Y-%m-%d %H:%M:%S")
    if start[1] is None:
        print(f"       set at {when} from the PC clock: no drift to count from it")
        return
    elapsed_s = (fields["reference_ms"] - start[0]) / 1000
    if elapsed_s < DRIFT_MIN_S:
        print(f"       set at {when}, {elapsed_s:.0f} s ago: too soon for a drift")
        return
    gained_ms = error_ms - start[1]
    ppm = gained_ms / elapsed_s * 1000
    per_day_s = gained_ms / elapsed_s * 86.4
    days = elapsed_s / 86400
    print(f"       since the setting of {when} ({days:.2f} days): {gained_ms:+.0f} ms, "
          f"drift {ppm:+.2f} ppm = {per_day_s:+.2f} s a day")
    print(f"       the crystal runs {fields['ppb'] / 1000 + ppm:+.2f} ppm "
          f"(correction in use {fields['ppb'] / 1000:+.2f} ppm)")
    if fields["flags"] & FLAG_APPROXIMATE:
        print("       a reset since: the time lost its length, counted in this drift")
    if days >= EV10_DAYS:
        verdict = "met" if abs(per_day_s) <= ENF08_S_PER_DAY else "NOT met"
        print(f"       EV-10: {days:.1f} days without a setting, ENF-08 "
              f"({ENF08_S_PER_DAY:g} s a day at most) {verdict}")


async def connect(address):
    from bleak import BleakClient
    device = await find(address)
    if device is None:
        return None
    client = BleakClient(device, timeout=40.0)
    await client.connect()
    return client


async def run(args):
    from bleak import BleakClient
    ntp = network_offset()
    if ntp:
        print(f"PC clock: {ntp[0] * 1000:+.0f} ms from {ntp[2]}")
    else:
        print("no NTP server reached: using the PC clock as it is")
    ntp_offset = ntp[0] if ntp else 0.0
    path = log_file(args.address, args.log)
    start = baseline(path, args.since)

    client = await connect(args.address)
    if client is None:
        print("no CB-91AI found (a button press brings fast advertising back for 30 s)")
        return 1
    try:
        error_ms, fields = await measure(client, ntp_offset)
        describe(error_ms, fields)
        append(path, "read", error_ms, fields)
        report_drift(error_ms, fields, start)

        if args.set:
            tz = int(datetime.datetime.now().astimezone().utcoffset().total_seconds() // 60)
            # Stamped as it leaves: where it lands in the round trip, the measure after says
            now_ms = int((time.time() + ntp_offset) * 1000)
            await client.write_gatt_char(TIME_CHARACTERISTIC, struct.pack("<qh", now_ms, tz),
                                         response=True)
            print("time set")
            error_ms, fields = await measure(client, ntp_offset)
            describe(error_ms, fields)
            # The start of the next run, from the error measured, not the one intended
            append(path, "after-set", error_ms, fields)
            if error_ms is not None and abs(error_ms) > 100:
                print(f"       the setting landed {-error_ms:.0f} ms after it left: the watch "
                      f"will count that in its next calibration (with the latency of the watch "
                      f"application, set it with cobalt_link.py, whose TIME goes at the start of "
                      f"a link)")

        if args.watch:
            print(f"following the error for {args.watch:g} min, one measure every {args.period:g} s")
            began = time.time()
            first = None
            while time.time() - began < args.watch * 60:
                await asyncio.sleep(args.period)
                try:
                    error_ms, fields = await measure(client, ntp_offset, reads=4)
                except Exception as exc:  # noqa: BLE001 - the link drops now and then: reconnect
                    print(f"  link lost ({type(exc).__name__}), reconnecting", flush=True)
                    try:
                        await client.disconnect()
                    except Exception:  # noqa: BLE001
                        pass
                    client = await connect(args.address)
                    if client is None:
                        print("  not found, next try in a period", flush=True)
                        client = BleakClient(args.address or "00:00:00:00:00:00")
                        continue
                    error_ms, fields = await measure(client, ntp_offset, reads=4)
                if error_ms is None:
                    print("time not set (the board was reset by a power cycle?): use --set")
                    return 1
                append(path, "read", error_ms, fields)
                first = first or (time.time(), error_ms)
                elapsed = time.time() - first[0]
                drift = (error_ms - first[1]) / elapsed * 1000 if elapsed > 60 else float("nan")
                print(f"  +{(time.time() - began) / 60:6.1f} min  error {error_ms:+7.0f} ms  "
                      f"residual drift {drift:+6.1f} ppm", flush=True)
    finally:
        try:
            await client.disconnect()
        except Exception:  # noqa: BLE001
            pass
        print(f"log: {path}")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--address", help="Bluetooth address; default: first board advertising SMP")
    parser.add_argument("--set", action="store_true", help="set the watch to network time")
    parser.add_argument("--watch", type=float, default=0.0, metavar="MINUTES",
                        help="keep the connection and follow the error")
    parser.add_argument("--period", type=float, default=60.0,
                        help="seconds between measures with --watch")
    parser.add_argument("--since", metavar="WHEN",
                        help="the setting the drift counts from, when the log has not got it: "
                             "local date and time (\"2026-09-26 23:39:04.542\") or ms since 1970 "
                             "(a TIME message)")
    parser.add_argument("--log", help="CSV log of the measures and settings; default: "
                                      "firmware/logs/clock-<address>.csv")
    args = parser.parse_args()
    if args.since:
        try:
            parse_since(args.since)
        except ValueError:
            parser.error("--since: a date and time such as \"2026-09-26 23:39:04.542\", "
                         "or ms since 1970")
    return asyncio.run(run(args))


if __name__ == "__main__":
    sys.exit(main())
