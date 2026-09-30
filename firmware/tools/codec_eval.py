#!/usr/bin/env python3
"""What a speech codec does to the transcription of a voice note (lot K3).

For each WAV file recorded by the watch (16 kHz, mono, 16-bit: `ble_shell.py --wav`), this
tool makes the versions a note could reach the phone in, coding and decoding them on the
PC with FFmpeg (liblc3, the library the watch runs; libopus; IMA ADPCM), transcribes every
version with Whisper (FFmpeg's whisper filter, whisper.cpp models), and counts, word by
word, how far each transcript lands from the transcript of the original and from the
sentence that was read.

    python firmware/tools/codec_eval.py firmware/logs/k1-A*.wav --ref firmware/tools/k1_sentences.txt
    python firmware/tools/codec_eval.py firmware/logs/voice-0.1.51-take1.wav --codecs lc3-16,lc3-24

The LC3 of the PC stands for the watch's: on the microphone take of 2026-09-20, decoded by
both, the signal-to-noise ratio against the original differs by 0.2 dB at most (15.8,
17.7 and 18.6 dB on the PC; 15.8, 17.9 and 18.7 dB on the watch, at 16, 24 and 32 kbit/s);
the bit streams are not identical, the encoder computing in floating point on both.

--ref gives the sentences read, one per line as `<id>: <sentence>`; a WAV file whose name
ends with `-<id>` (k1-A1.wav for A1) is compared with it. Numbers are compared as digits,
whether spoken as words or written by Whisper as digits, and "heures" as "h", so that
the writing of a time or an amount does not count as an error.

Requirements: FFmpeg 8 or later built with liblc3, libopus and whisper (the "full" builds
for Windows are), numpy, and whisper.cpp models (ggml-*.bin, from
https://huggingface.co/ggerganov/whisper.cpp) in --models. The decoded versions, the
transcripts and the report go to firmware/logs/k3/ (ignored by git: recordings of a voice
never go into the repository).
"""

import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import wave

import numpy as np

FIRMWARE_DIR = pathlib.Path(__file__).resolve().parents[1]
OUT_DIR = FIRMWARE_DIR / "logs" / "k3"
DEFAULT_MODELS_DIR = pathlib.Path(os.environ.get("CB91AI_WHISPER_MODELS",
                                                 pathlib.Path.home() / "whisper-models"))
DEFAULT_MODELS = "ggml-large-v3-turbo-q5_0.bin,ggml-small-q5_1.bin"
RATE = 16000

# Name: FFmpeg encoder options and file suffix. Opus as the watch would run it (VOIP,
# complexity 0, 20 ms, variable rate).
CODECS = {
    "lc3-16": (["-c:a", "liblc3", "-b:a", "16000", "-frame_duration", "10"], ".lc3"),
    "lc3-24": (["-c:a", "liblc3", "-b:a", "24000", "-frame_duration", "10"], ".lc3"),
    "lc3-32": (["-c:a", "liblc3", "-b:a", "32000", "-frame_duration", "10"], ".lc3"),
    "opus-16": (["-c:a", "libopus", "-b:a", "16000", "-application", "voip",
                 "-compression_level", "0", "-frame_duration", "20", "-vbr", "on"], ".opus"),
    "opus-24": (["-c:a", "libopus", "-b:a", "24000", "-application", "voip",
                 "-compression_level", "0", "-frame_duration", "20", "-vbr", "on"], ".opus"),
    "adpcm": (["-c:a", "adpcm_ima_wav"], ".wav"),
}
DEFAULT_CODECS = "lc3-16,lc3-24,lc3-32"


def ffmpeg(*args, cwd=None):
    result = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args],
                            capture_output=True, text=True, cwd=cwd)
    if result.returncode != 0:
        raise RuntimeError(f"ffmpeg {' '.join(args)}: {result.stderr.strip()[-400:]}")


def check_ffmpeg(codecs):
    def listing(what):
        return subprocess.run(["ffmpeg", "-hide_banner", what], capture_output=True,
                              text=True).stdout
    encoders, filters = listing("-encoders"), listing("-filters")
    needs = {"liblc3" for c in codecs if c.startswith("lc3")}
    needs |= {"libopus" for c in codecs if c.startswith("opus")}
    missing = [n for n in sorted(needs) if n not in encoders]
    if " whisper " not in filters:
        missing.append("the whisper filter")
    if missing:
        sys.exit(f"this FFmpeg lacks {', '.join(missing)}: take a full build, version 8 or later")


def filter_path(path):
    """A path as an option of an FFmpeg filter: unescaped twice, by the graph, then the
    options, hence two backslashes before a colon, a quote or a space."""
    text = str(pathlib.Path(path).resolve()).replace("\\", "/")
    if any(c in text for c in ",;[]"):
        raise ValueError(f"{text}: no , ; [ or ] in a path given to a filter")
    for special in (":", "'", " "):
        text = text.replace(special, "\\\\" + special)
    return text


def read_wav(path):
    with wave.open(str(path)) as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2 or w.getframerate() != RATE:
            raise ValueError(f"{path}: 16 kHz, mono, 16-bit expected")
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)


def payload_bytes(path):
    """Sum of the packet sizes: the bytes a note would weigh, container left out."""
    result = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a", "-show_entries",
                             "packet=size", "-of", "csv=p=0", str(path)],
                            capture_output=True, text=True, check=True)
    return sum(int(x) for x in result.stdout.split() if x.strip().isdigit())


def code(source, codec, work):
    """Encode and decode one file; returns the decoded WAV and the payload in bytes."""
    options, suffix = CODECS[codec]
    coded = work / f"{source.stem}.{codec}.coded{suffix}"
    decoded = work / f"{source.stem}.{codec}.wav"
    ffmpeg("-i", str(source), *options, str(coded))
    ffmpeg("-i", str(coded), "-ar", str(RATE), "-ac", "1", "-c:a", "pcm_s16le", str(decoded))
    return decoded, payload_bytes(coded)


def snr_db(ref, test, span=400):
    """Rough ratio of the original to the difference, delay taken out: a speech codec shapes its
    noise for the ear, not for this figure, which only tells a broken chain from a working one."""
    n = min(len(ref), len(test)) - 2 * span
    if n <= 0:
        return float("nan")
    x = ref[span:span + n]
    best = None
    for lag in range(-span, span + 1):
        y = test[span + lag:span + lag + n]
        err = float(np.sum((x - y) ** 2))
        if best is None or err < best:
            best = err
    return 10 * np.log10(float(np.sum(x ** 2)) / max(best, 1e-9))


def levels(samples):
    """Voice and floor: rms of the loudest and of the quietest 100 ms, in dBFS."""
    block = RATE // 10
    rms = [np.sqrt(np.mean(samples[i:i + block] ** 2)) for i in range(0, len(samples) - block + 1, block)]
    db = [20 * np.log10(max(r, 0.5) / 32768) for r in rms] or [float("nan")]
    peak = 20 * np.log10(max(np.max(np.abs(samples)), 0.5) / 32768)
    return max(db), min(db), peak


def transcribe(wav, model, cache):
    """Text of one file by one model, cached by content and model."""
    key = hashlib.sha256(wav.read_bytes()).hexdigest()[:16] + ":" + model.name
    if key in cache:
        return cache[key]
    with tempfile.TemporaryDirectory() as tmp:
        dest = pathlib.Path(tmp) / "text.txt"
        ffmpeg("-i", str(wav), "-af",
               f"whisper=model={filter_path(model)}:language=fr:queue=30:use_gpu=0:"
               f"destination={filter_path(dest)}:format=text", "-f", "null", "-")
        text = " ".join(dest.read_text(encoding="utf-8").split()) if dest.exists() else ""
    cache[key] = text
    return text


# ---- Comparison of transcripts --------------------------------------------------------

UNITS = {"zéro": 0, "zero": 0, "un": 1, "une": 1, "deux": 2, "trois": 3, "quatre": 4, "cinq": 5,
         "six": 6, "sept": 7, "huit": 8, "neuf": 9}
TEENS = {"dix": 10, "onze": 11, "douze": 12, "treize": 13, "quatorze": 14, "quinze": 15,
         "seize": 16}
TENS = {"vingt": 20, "vingts": 20, "trente": 30, "quarante": 40, "cinquante": 50, "soixante": 60}


def numbers_to_digits(words):
    """French numbers in words become digits: "mille deux cent cinquante" -> "1250",
    "quatre vingt dix sept" -> "97", and "quarante deux dix huit" -> "42 18": a new number
    starts where the next word cannot continue the current one."""
    out = []
    state = {"total": 0, "current": 0, "active": False, "last": None}

    def flush():
        if state["active"]:
            out.append(str(state["total"] + state["current"]))
        state.update(total=0, current=0, active=False, last=None)

    def start(value, kind):
        flush()
        state.update(current=value, active=True, last=kind)

    for i, word in enumerate(words):
        following = words[i + 1] if i + 1 < len(words) else None
        active, last = state["active"], state["last"]
        small = state["current"] % 100
        after_round = active and last in ("hundred", "thousand") and small == 0
        if word in ("zéro", "zero"):
            flush()
            out.append("0")
        elif word in UNITS:
            value = UNITS[word]
            if (active and last == "tens" and small in (20, 30, 40, 50, 60, 80)) or after_round or \
                    (active and last == "teen" and small in (10, 70, 90) and value >= 7):
                state["current"] += value
                state["last"] = "unit"
            else:
                start(value, "unit")
        elif word in TEENS:
            if (active and last == "tens" and small in (60, 80)) or after_round:
                state["current"] += TEENS[word]
                state["last"] = "teen"
            else:
                start(TEENS[word], "teen")
        elif word in TENS:
            if word.startswith("vingt") and active and last == "unit" and small == 4:
                state["current"] += 76  # quatre-vingt: the 4 becomes 80
                state["last"] = "tens"
            elif after_round:
                state["current"] += TENS[word]
                state["last"] = "tens"
            else:
                start(TENS[word], "tens")
        elif word in ("cent", "cents"):
            if active and last == "unit" and state["current"] < 10:
                state["current"] *= 100
                state["last"] = "hundred"
            elif active and last == "thousand" and state["current"] == 0:
                state["current"] = 100
                state["last"] = "hundred"
            else:
                start(100, "hundred")
        elif word == "mille":
            if active and state["total"] == 0:
                state["total"] = (state["current"] or 1) * 1000
                state["current"] = 0
                state["last"] = "thousand"
            else:
                start(0, "thousand")
                state["total"] = 1000
        elif word == "et" and active and last == "tens" and following in ("un", "une", "onze"):
            continue
        else:
            flush()
            out.append(word)
    flush()
    return out


def normalize(text):
    t = text.lower().replace("’", "'").replace("œ", "oe").replace(" ", " ").replace(" ", " ")
    t = re.sub(r"(\d)\s*h\s*(\d)", r"\1 h \2", t)
    t = re.sub(r"(\d)\s*h\b", r"\1 h", t)
    t = re.sub(r"(\d) (\d{3})\b", r"\1\2", t)
    t = t.replace("€", " euros ")
    t = re.sub(r"[-'.,;:!?«»\"()…/]", " ", t)
    words = numbers_to_digits(t.split())
    return ["h" if w in ("heure", "heures") else "euros" if w == "euro" else w for w in words]


def edits(ref, hyp):
    """Levenshtein distance between two sequences."""
    prev = list(range(len(hyp) + 1))
    for i, r in enumerate(ref, 1):
        cur = [i]
        for j, h in enumerate(hyp, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (r != h)))
        prev = cur
    return prev[-1]


# ---- Main ---------------------------------------------------------------------------------

def load_refs(path):
    refs = {}
    if path:
        for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
            if ":" in line and not line.lstrip().startswith("#"):
                key, sentence = line.split(":", 1)
                refs[key.strip()] = sentence.strip()
    return refs


def ref_for(wav, refs):
    stem = wav.stem
    for key, sentence in refs.items():
        if stem == key or stem.endswith("-" + key):
            return key, sentence
    return None, None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("wavs", nargs="+", type=pathlib.Path)
    parser.add_argument("--ref", help="sentences read, one per line as `<id>: <sentence>`")
    parser.add_argument("--codecs", default=DEFAULT_CODECS,
                        help=f"among {', '.join(CODECS)} (default {DEFAULT_CODECS})")
    parser.add_argument("--models-dir", type=pathlib.Path, default=DEFAULT_MODELS_DIR)
    parser.add_argument("--models", default=DEFAULT_MODELS, help="whisper.cpp model files")
    parser.add_argument("--jobs", type=int, default=4, help="transcriptions run at once")
    parser.add_argument("--name", default=None, help="name of the report (default: a date)")
    args = parser.parse_args()

    codecs = [c.strip() for c in args.codecs.split(",") if c.strip()]
    unknown = [c for c in codecs if c not in CODECS]
    if unknown:
        parser.error(f"unknown codec(s): {', '.join(unknown)}")
    models = [args.models_dir / m.strip() for m in args.models.split(",") if m.strip()]
    missing = [str(m) for m in models if not m.exists()]
    if missing:
        parser.error(f"missing model(s): {', '.join(missing)}")
    check_ffmpeg(codecs)
    refs = load_refs(args.ref)

    name = args.name or datetime.datetime.now().strftime("k3-%Y%m%d-%H%M%S")
    work = OUT_DIR / name
    work.mkdir(parents=True, exist_ok=True)
    cache_path = OUT_DIR / "transcripts-cache.json"
    cache = json.loads(cache_path.read_text(encoding="utf-8")) if cache_path.exists() else {}

    # Every version of every file
    rows = []
    for wav in args.wavs:
        original = read_wav(wav)
        key, sentence = ref_for(wav, refs)
        voice, floor, peak = levels(original)
        versions = {"original": (wav, len(original) * 2)}
        for codec in codecs:
            versions[codec] = code(wav, codec, work)
        for version, (path, size) in versions.items():
            decoded = read_wav(path)
            rows.append({
                "file": wav.name, "id": key, "reference": sentence, "version": version,
                "path": str(path), "kbps": size * 8 / (len(original) / RATE) / 1000,
                "snr_db": None if version == "original" else snr_db(original, decoded),
                "voice_dbfs": voice, "floor_dbfs": floor, "peak_dbfs": peak,
            })
        print(f"{wav.name}: {len(versions)} versions made (voice {voice:.1f}, floor {floor:.1f}, "
              f"peak {peak:.1f} dBFS)", flush=True)

    # Transcriptions, a few at a time
    jobs = [(row, model) for row in rows for model in models]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(transcribe, pathlib.Path(row["path"]), model, cache): (row, model)
                   for row, model in jobs}
        for done, future in enumerate(concurrent.futures.as_completed(futures), 1):
            row, model = futures[future]
            row.setdefault("text", {})[model.name] = future.result()
            if done % 10 == 0 or done == len(futures):
                print(f"  {done}/{len(futures)} transcriptions", flush=True)
                cache_path.write_text(json.dumps(cache, ensure_ascii=False, indent=1),
                                      encoding="utf-8")

    # Distances: to the sentence read, and to the transcript of the original by the same model
    originals = {(r["file"], m.name): normalize(r["text"][m.name])
                 for r in rows if r["version"] == "original" for m in models}
    for row in rows:
        row["vs_original"], row["vs_reference"], row["ref_words"] = {}, {}, None
        for model in models:
            hyp = normalize(row["text"][model.name])
            row["vs_original"][model.name] = edits(originals[(row["file"], model.name)], hyp)
            if row["reference"]:
                ref = normalize(row["reference"])
                row["ref_words"] = len(ref)
                row["vs_reference"][model.name] = edits(ref, hyp)

    (work / "results.json").write_text(json.dumps(rows, ensure_ascii=False, indent=1),
                                       encoding="utf-8")
    report = render(rows, models, ["original"] + codecs, name)
    (work / "report.md").write_text(report, encoding="utf-8")
    print(report)
    print(f"\nreport: {work / 'report.md'}")


def render(rows, models, versions, name):
    lines = [f"# {name}: codecs and transcription", ""]
    files = sorted({r["file"] for r in rows})
    lines.append(f"{len(files)} file(s); models: {', '.join(m.name for m in models)}.")
    lines.append("")
    head = "| Version | kbit/s | SNR (dB) |"
    rule = "|---|---|---|"
    for m in models:
        short = m.stem.replace("ggml-", "")
        head += f" words changed vs original, {short} | errors vs sentence read, {short} |"
        rule += "---|---|"
    lines += [head, rule]
    for version in versions:
        sel = [r for r in rows if r["version"] == version]
        kbps = np.mean([r["kbps"] for r in sel])
        snrs = [r["snr_db"] for r in sel if r["snr_db"] is not None]
        line = f"| {version} | {kbps:.1f} | {np.mean(snrs):.1f} |" if snrs else f"| {version} | {kbps:.0f} | |"
        for m in models:
            changed = sum(r["vs_original"][m.name] for r in sel)
            with_ref = [r for r in sel if r["reference"]]
            if with_ref:
                errors = sum(r["vs_reference"][m.name] for r in with_ref)
                words = sum(r["ref_words"] for r in with_ref)
                line += f" {changed} | {errors} of {words} ({100 * errors / words:.1f} %) |"
            else:
                line += f" {changed} | |"
        lines.append(line)
    lines += ["", "## Transcripts", ""]
    for f in files:
        sel = [r for r in rows if r["file"] == f]
        first = sel[0]
        lines.append(f"### {f}")
        lines.append("")
        lines.append(f"Voice {first['voice_dbfs']:.1f} dBFS, floor {first['floor_dbfs']:.1f} dBFS, "
                     f"peak {first['peak_dbfs']:.1f} dBFS.")
        if first["reference"]:
            lines.append(f"Read: « {first['reference']} »")
        lines.append("")
        for m in models:
            lines.append(f"| {m.stem.replace('ggml-', '')} | Transcript | vs original | vs read |")
            lines.append("|---|---|---|---|")
            for r in sel:
                vs_ref = r["vs_reference"].get(m.name, "")
                lines.append(f"| {r['version']} | {r['text'][m.name]} | {r['vs_original'][m.name]} | "
                             f"{vs_ref} |")
            lines.append("")
    return "\n".join(lines)


if __name__ == "__main__":
    main()
