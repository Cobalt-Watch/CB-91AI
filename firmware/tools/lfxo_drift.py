#!/usr/bin/env python3
"""Measure the 32.768 kHz crystal of a CB-91AI against network time (V2-20).

    python firmware/tools/lfxo_drift.py --minutes 60

The RTC1 counter of the nRF52840 (the Zephyr system clock, 32768 ticks per
second, 24 bits) is read through the ST-Link every few seconds without halting
the core, and compared with the clock of the PC. The PC clock itself drifts by
tens of ppm when Windows does not follow a time server, so its offset to NTP is
sampled at both ends of the run and taken out. Result: the crystal error in ppm,
positive when the watch runs fast, which is the value of the clock correction
(`cb91ai time ppb <value>` on the shell, or
CONFIG_CB91AI_CLOCK_DEFAULT_PPB for every V1 board, EF-02).

Accuracy is set by the NTP samples (a few milliseconds each): about +/-10 ppm
for 30 min, +/-2 ppm for 3 h, better overnight. The board must not reset during
the run: no flash, no update. The firmware does not matter.

Requirements: `pip install pyocd`, the ST-Link on the SWD pads.
The samples go to firmware/logs/lfxo-<date>.csv.
"""

import argparse
import datetime
import pathlib
import socket
import struct
import sys
import time

from pyocd.core.helpers import ConnectHelper

FIRMWARE_DIR = pathlib.Path(__file__).resolve().parents[1]
LOG_DIR = FIRMWARE_DIR / "logs"
RTC1_COUNTER = 0x40011504
RTC1_PRESCALER = 0x40011508
COUNTER_BITS = 24
NOMINAL_HZ = 32768.0
NTP_SERVERS = ["time.google.com", "time.cloudflare.com", "time.windows.com", "pool.ntp.org"]
NTP_EPOCH_DELTA = 2208988800  # 1900 to 1970


def sntp_offset(server, timeout=2.0):
    """(offset, round trip) in seconds, offset = server clock - local clock."""
    packet = b"\x23" + 47 * b"\0"  # LI 0, version 4, mode 3 (client)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        t0 = time.time()
        sock.sendto(packet, (server, 123))
        data, _ = sock.recvfrom(64)
        t3 = time.time()
    if len(data) < 48:
        raise OSError("short NTP answer")

    def stamp(offset):
        seconds, fraction = struct.unpack("!II", data[offset:offset + 8])
        return seconds - NTP_EPOCH_DELTA + fraction / 2**32

    t1, t2 = stamp(32), stamp(40)
    return ((t1 - t0) + (t2 - t3)) / 2, (t3 - t0) - (t2 - t1)


def network_offset(samples=8):
    """Offset of the best (shortest round trip) of several NTP exchanges."""
    best = None
    for server in NTP_SERVERS:
        got = 0
        for _ in range(samples):
            try:
                offset, rtt = sntp_offset(server)
            except OSError:
                continue
            got += 1
            if best is None or rtt < best[1]:
                best = (offset, rtt, server)
            time.sleep(0.2)
        if got >= samples // 2:
            break
    return best


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--minutes", type=float, default=60.0, help="length of the run")
    parser.add_argument("--period", type=float, default=5.0, help="seconds between samples")
    args = parser.parse_args()

    start_ntp = network_offset()
    if start_ntp:
        print(f"PC clock: {start_ntp[0] * 1000:+.1f} ms from {start_ntp[2]} "
              f"(round trip {start_ntp[1] * 1000:.0f} ms)", flush=True)
    else:
        print("no NTP server reached: the result will include the drift of the PC clock",
              flush=True)

    session = ConnectHelper.session_with_chosen_probe(
        blocking=False, return_first=True, target_override="nrf52840",
        options={"connect_mode": "attach", "auto_unlock": False, "resume_on_disconnect": False})
    if session is None:
        print("no debug probe found")
        return 1

    LOG_DIR.mkdir(exist_ok=True)
    log = LOG_DIR / f"lfxo-{datetime.datetime.now():%Y%m%d-%H%M%S}.csv"
    samples = []
    with session, log.open("w", encoding="utf-8") as out:
        target = session.board.target
        if target.read32(RTC1_PRESCALER) != 0:
            print("RTC1 prescaler is not 0: not the Zephyr system clock?")
            return 1
        out.write("pc_time,ticks\n")
        last = None
        wraps = 0
        end = time.time() + args.minutes * 60
        next_report = time.time() + 300
        while time.time() < end:
            before = time.time()
            counter = target.read32(RTC1_COUNTER)
            after = time.time()
            if after - before > 0.02:
                time.sleep(args.period)
                continue  # slow read: the time stamp is not trustworthy
            if last is not None and counter < last:
                wraps += 1
            last = counter
            ticks = counter + (wraps << COUNTER_BITS)
            stamp = (before + after) / 2
            samples.append((stamp, ticks))
            out.write(f"{stamp:.6f},{ticks}\n")
            out.flush()
            if time.time() > next_report and len(samples) > 10:
                next_report += 300
                print(f"  {len(samples)} samples, raw so far "
                      f"{raw_ppm(samples):+.1f} ppm (PC clock not corrected)", flush=True)
            time.sleep(args.period)

    end_ntp = network_offset()
    if len(samples) < 10:
        print("not enough samples")
        return 1

    raw = raw_ppm(samples)
    elapsed = samples[-1][0] - samples[0][0]
    print(f"{len(samples)} samples over {elapsed / 60:.1f} min, log in {log}")
    print(f"crystal against the PC clock: {raw:+.1f} ppm")
    if start_ntp and end_ntp:
        # offset = NTP - PC. If it grows, the PC clock runs slow by that much.
        pc_ppm = -(end_ntp[0] - start_ntp[0]) / elapsed * 1e6
        print(f"PC clock against NTP: {pc_ppm:+.1f} ppm "
              f"(offset {start_ntp[0] * 1000:+.1f} ms -> {end_ntp[0] * 1000:+.1f} ms)")
        corrected = raw + pc_ppm
        uncertainty = (start_ntp[1] + end_ntp[1]) / 2 / elapsed * 1e6
        print(f"crystal against NTP: {corrected:+.1f} ppm, +/-{uncertainty:.1f} ppm "
              f"from the NTP round trips")
        print(f"clock correction: {corrected * 1000:+.0f} ppb "
              f"({corrected * 0.0864:+.1f} s per day uncorrected)")
    return 0


def raw_ppm(samples):
    """Least-squares slope of ticks against PC time, as an error in ppm."""
    n = len(samples)
    mean_t = sum(s[0] for s in samples) / n
    mean_k = sum(s[1] for s in samples) / n
    num = sum((s[0] - mean_t) * (s[1] - mean_k) for s in samples)
    den = sum((s[0] - mean_t) ** 2 for s in samples)
    return (num / den / NOMINAL_HZ - 1.0) * 1e6


if __name__ == "__main__":
    sys.exit(main())
