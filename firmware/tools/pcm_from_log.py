#!/usr/bin/env python3
"""Turn the speech dumps of a CB-91AI shell log into WAV files.

`cb91ai codec dump` and `cb91ai codec roundtrip ...` (codec bench image)
print 16-bit PCM as base64 lines:

    PCM-BEGIN <name> <rate> <samples> <delay>
    ~<base64> ...
    PCM-END <samples> <crc32>

This tool reads a log written by rtt_shell.py or usb_console.py, checks the
sample count and the CRC of every dump, and writes one WAV file per dump next to
the log (firmware/logs/ is ignored by git: recordings of a voice never go into
the repository). When the log holds the original capture and round trips of it,
the signal-to-noise ratio of each round trip against the original is printed,
the codec delay taken out: a rough figure, since a speech codec shapes its noise
for the ear, not for this ratio. Listening and transcription are the real test.

Usage:
    python firmware/tools/pcm_from_log.py firmware/logs/rtt-20260920-190000.txt
    python firmware/tools/pcm_from_log.py <log> --normalize    # same gain on every clip, peak at -1 dBFS
"""

import argparse
import array
import base64
import math
import pathlib
import re
import sys
import wave
import zlib

BEGIN = re.compile(r"^PCM-BEGIN (\S+) (\d+) (\d+) (-?\d+)\s*$")
END = re.compile(r"^PCM-END (\d+) ([0-9a-fA-F]{8})\s*$")


def parse(path):
    """Yield (name, rate, delay, samples as array('h'), problems) for each dump."""
    current = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        begin = BEGIN.match(line)
        if begin:
            current = {"name": begin.group(1), "rate": int(begin.group(2)),
                       "announced": int(begin.group(3)), "delay": int(begin.group(4)), "data": bytearray(),
                       "problems": []}
            continue
        if current is None:
            continue
        if line.startswith("~"):
            try:
                current["data"] += base64.b64decode(line[1:], validate=True)
            except ValueError:
                current["problems"].append("a damaged base64 line")
            continue
        end = END.match(line)
        if end:
            data = bytes(current["data"])
            if len(data) // 2 != int(end.group(1)) or int(end.group(1)) != current["announced"]:
                current["problems"].append(
                    f"{len(data) // 2} samples received, {current['announced']} announced, {end.group(1)} sent")
            if zlib.crc32(data) != int(end.group(2), 16):
                current["problems"].append("CRC mismatch: part of the dump was lost on the way")
            samples = array.array("h")
            samples.frombytes(data[:len(data) // 2 * 2])
            if sys.byteorder == "big":
                samples.byteswap()
            yield current["name"], current["rate"], current["delay"], samples, current["problems"]
            current = None
    if current is not None:
        yield current["name"], current["rate"], current["delay"], array.array("h"), ["no PCM-END: log cut short"]


def level(samples):
    if not samples:
        return 0.0, 0
    mean = sum(samples) / len(samples)
    rms = math.sqrt(sum((s - mean) ** 2 for s in samples) / len(samples))
    return rms, max(abs(s) for s in samples)


def snr_db(original, decoded, delay):
    """Signal-to-noise ratio of `decoded` against `original`, in dB; `decoded`
    lags `original` by `delay` samples."""
    count = min(len(original), len(decoded) - delay)
    if count <= 0:
        return None
    signal = sum(original[i] ** 2 for i in range(count))
    noise = sum((original[i] - decoded[i + delay]) ** 2 for i in range(count))
    if signal == 0 or noise == 0:
        return None
    return 10 * math.log10(signal / noise)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=pathlib.Path, help="log written by rtt_shell.py or usb_console.py")
    parser.add_argument("--out", type=pathlib.Path, help="folder of the WAV files (default: next to the log)")
    parser.add_argument("--normalize", action="store_true",
                        help="one common gain for all the clips, loudest peak at -1 dBFS: easier to listen to, "
                             "still fair to compare")
    args = parser.parse_args()

    dumps = list(parse(args.log))
    if not dumps:
        print("no PCM-BEGIN ... PCM-END block in this log", file=sys.stderr)
        return 1
    out_dir = args.out or args.log.parent
    out_dir.mkdir(parents=True, exist_ok=True)

    gain = 1.0
    if args.normalize:
        peak = max((level(samples)[1] for _, _, _, samples, _ in dumps), default=0)
        gain = 32767 * 10 ** (-1 / 20) / peak if peak else 1.0
        print(f"common gain: x{gain:.2f} ({20 * math.log10(gain):+.1f} dB)")

    original = None
    status = 0
    for index, (name, rate, delay, samples, problems) in enumerate(dumps, 1):
        rms, peak = level(samples)
        line = f"{index}. {name}: {len(samples) / rate:.2f} s, rms {rms:.0f}, peak {peak}"
        if delay == 0 and not name.startswith(("lc3-", "adpcm-")):
            original = samples
        elif original is not None:
            ratio = snr_db(original, samples, delay)
            if ratio is not None:
                line += f", SNR {ratio:.1f} dB against the original (delay {delay})"
        for problem in problems:
            line += f"\n   PROBLEM: {problem}"
            status = 1
        if samples:
            wav_path = out_dir / f"{args.log.stem}-{index}-{name}.wav"
            scaled = array.array("h", (max(-32768, min(32767, round(s * gain))) for s in samples))
            if sys.byteorder == "big":
                scaled.byteswap()
            with wave.open(str(wav_path), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(rate)
                wav.writeframes(scaled.tobytes())
            line += f"\n   -> {wav_path}"
        print(line)
    return status


if __name__ == "__main__":
    sys.exit(main())
