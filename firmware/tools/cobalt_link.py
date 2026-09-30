#!/usr/bin/env python3
"""Cobalt Link, protocol version 1, on the PC side: the messages of the harness (lot C1).

    python firmware/tools/cobalt_link.py --self-test

The messages the phone app and this harness exchange with the watch over the Cobalt Link
service, byte for byte as the Cobalt Link specification defines them: parse what the
watch notifies on TX, write what goes to RX. --self-test checks them against the vectors that
the watch's own code is tested with (firmware/tests/vectors/link_v1.txt): this side parses the
watch's messages and writes the phone's, the watch the other way round, so the two agree byte
for byte.

With --address and scenarios, the harness plays the phone over Bluetooth, one connection per
scenario, and checks the course of the session (HELLO, TIME, settings, held session, BYE, and
an SMP command within the session, as an update from the phone app would):

    python firmware/tools/cobalt_link.py --address AA:BB:CC:DD:EE:FF all

The `light` session sets the light of the LIGHT button as the app will (key 0x05), and
leaves it so: `--light white|breathe|blink|rainbow`, or `--light effect,red,green,blue`.

    python firmware/tools/cobalt_link.py --address AA:BB:CC:DD:EE:FF --light rainbow light

The voice notes (lot E1): `notes` takes every note waiting as the app will, checks its CRC and
its header (container v2), and keeps it in firmware/logs/notes:
the note as sent (.cbn2), a standard LC3 file (.lc3, liblc3's format) and the voice (.wav, by
FFmpeg); `--whisper` also transcribes it. The other sessions leave the notes on the watch (they
answer each offer "damaged", which the watch does not offer again in that session). On a
development build, `resume` cuts the link in the middle of a note and takes the rest from its
offset, and `hundred` has the watch record and send a hundred short takes in a row (key 0x7F);
`delay` times, take by take, the end of a take, the offer of its note and its first byte (ENF-06,
`--takes` and `--take-s`), in a held session whose latency of 30 is on:

    python firmware/tools/cobalt_link.py --address AA:BB:CC:DD:EE:FF notes --whisper
    python firmware/tools/cobalt_link.py --address AA:BB:CC:DD:EE:FF resume hundred
    python firmware/tools/cobalt_link.py --address AA:BB:CC:DD:EE:FF --takes 10 --take-s 2 delay

Also on a development build: `leave` leaves the note of a take waiting on the watch, put off as
a phone may do (before an update, which must keep it: EF-65), and `recall`, on a confirmed
image (one in test goes back after 10 min), leaves one, waits until the watch calls the phone
again, 15 min after the link when its rhythm starts afresh (lot E2: 15, 30, then 60 min), reads
when from the watch's status line (call= and end=, its uptime at the last call and at the end
of the last link), then takes the notes: 18 min.

    python firmware/tools/cobalt_link.py --address AA:BB:CC:DD:EE:FF leave
    python firmware/tools/cobalt_link.py --address AA:BB:CC:DD:EE:FF recall

`name` gives the watch its name (key 0x06, 11 characters of printable ASCII at most), reads it
back, checks that a longer one is refused, then waits to hear it in the scan response, where the
watch puts it once the link is down:

    python firmware/tools/cobalt_link.py --address AA:BB:CC:DD:EE:FF --name "CB91 Demo" name

`thermo` reads the temperatures of the status line in a held session, every --every s (5)
for --minutes (10): temp=, the thermometer of the "tE P" screen (the nRF52840's own since
0.2.1+12, in hundredths of a degree; the BMA400's in 0.2.1+11, a reading behind), and die=, the
nRF52840 beside it in the test image 0.2.1+11 only. The readings go to
firmware/logs/thermo-<address>-<date>.csv, then a summary gives each one's range, its noise
from one reading to the next, and how often the whole degree shown would change, rounded
reading by reading (the watch holds it longer since 0.2.1+12):

    python firmware/tools/cobalt_link.py --address AA:BB:CC:DD:EE:FF --minutes 20 thermo

`journal` reads the journal of the temperature and the cell (lot T1, key 0x12,
the Cobalt Link specification) as the app will: SET the cursor (--from, 0 for all), whose
VALUE is the first page, then GET until an empty page; the records go to
firmware/logs/journal-<address>-<date>.csv, then a summary. On a development image, --period
first sets the journal's period until the next boot (key 0x7D), for a test in minutes:

    python firmware/tools/cobalt_link.py --address AA:BB:CC:DD:EE:FF --period 5 journal
    python firmware/tools/cobalt_link.py --address AA:BB:CC:DD:EE:FF journal

`wrist` follows the raise of the wrist and the double tap (lot D4) on a development image: the
tuning of the accelerometer (key 0x7C) read, or set first by --wrist, then the counters of the
status line every 2 s for --minutes, each change printed as the wearer tries the gestures.
Since 0.2.1+17, --wrist takes four more numbers (wake reference, orientation reference,
osr_lp, flags: lib/accel.h), and the session reads the accelerometer's status and registers
(key 0x7B) at its start, at each poll (the wake-ups it saw, with flag 2) and at its end:

    python firmware/tools/cobalt_link.py --address AA:BB:CC:DD:EE:FF --minutes 5 wrist
    python firmware/tools/cobalt_link.py --address AA:BB:CC:DD:EE:FF --wrist 2,1,60,25,3,3000,500 wrist
    python firmware/tools/cobalt_link.py --address AA:BB:CC:DD:EE:FF --wrist 3,2,60,25,3,3000,500,1,0,0,2 wrist

Requirements: `pip install bleak`; FFmpeg built with liblc3 for the .wav, and its whisper
filter for --whisper (as codec_eval.py). Output also in firmware/logs/cobaltlink-<date>.txt.
"""

import argparse
import pathlib
import struct
import subprocess
import sys
import zlib

VECTORS = pathlib.Path(__file__).resolve().parents[1] / "tests" / "vectors" / "link_v1.txt"

PROTOCOL_VERSION = 1
SERVICE_UUID = "c0b91b00-256e-46f9-8d7b-d9c643908667"
SMP_UUID = "da2e7828-fbce-4e01-ae9e-261174997c48"  # the SMP characteristic of MCUmgr
RX_UUID = "c0b91b01-256e-46f9-8d7b-d9c643908667"  # phone to watch, write without response
TX_UUID = "c0b91b02-256e-46f9-8d7b-d9c643908667"  # watch to phone, notify
TO_WATCH = 0x80

# Watch to phone
HELLO, NOTE_OFFER, NOTE_DATA, NOTE_END, VALUE, EVENT, BYE_WATCH = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0F
# Phone to watch
TIME, NOTE_ACCEPT, NOTE_ACK, RESULT, SET, GET, BYE_PHONE = 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x8F

NAMES = {HELLO: "HELLO", NOTE_OFFER: "NOTE_OFFER", NOTE_DATA: "NOTE_DATA", NOTE_END: "NOTE_END",
         VALUE: "VALUE", EVENT: "EVENT", BYE_WATCH: "BYE_WATCH", TIME: "TIME",
         NOTE_ACCEPT: "NOTE_ACCEPT", NOTE_ACK: "NOTE_ACK", RESULT: "RESULT", SET: "SET",
         GET: "GET", BYE_PHONE: "BYE_PHONE"}

REASONS = {0: "phone", 1: "note waiting", 2: "sync asked", 3: "daily", 4: "after an update",
           5: "after pairing"}
KEY_TIME_FORMAT, KEY_DISPLAY_S, KEY_TX_POWER, KEY_HOLD, KEY_LIGHT, KEY_COUNTERS, KEY_STATUS = (
    0x01, 0x02, 0x03, 0x04, 0x05, 0x10, 0x11)
KEY_NAME, NAME_MAX = 0x06, 11  # the name of the watch, in the scan response
KEY_JOURNAL = 0x12  # the journal of the temperature and the cell (lot T1): SET cursor, GET pages
KEY_DEBUG_JOURNAL_S = 0x7D  # development builds: the journal's period, 1 to 600 s
KEY_DEBUG_WRIST = 0x7C  # development builds: the tuning of the wrist (lot D4)
KEY_DEBUG_ACCEL = 0x7B  # development builds, read only: the accelerometer's status and registers
# Development builds: the trial of the calls (lot N1a, risk R11): SET u8 minutes (0 stops),
# u8 profile; GET the trial and the last call a phone answered, as the watch timed it
KEY_DEBUG_TRIAL = 0x7A
TRIAL_PROFILES = {"product": 0, "apple": 1}
TRIAL_NONE = 0xFFFFFFFF
WRIST_FIELDS = ("wake", "samples", "orient", "duration", "tap", "lowpower_ms", "faceup_mg")
# Since 0.2.1+17: wake reference (1 once asleep, 2 the previous sample), orientation reference
# (0 the watch, 1 acc_filt2, 2 acc_filt_lp), osr_lp 0 to 3, flags (1 orientation on
# acc_filt_lp, 2 each wake-up seen); lib/accel.h
WRIST_FIELDS_MORE = ("wake_ref", "orient_ref", "osr_lp", "flags")
# Since 0.2.1+28: the turn of the wrist a look needs, Y up by that many mg; since 0.2.1+29,
# the tip of the glass (Z up by that many mg) and the height of 12 o'clock after a turn
# (watch/src/look.h); flag 4 judges the orientation on Z only, as up to 0.2.1+27
WRIST_FIELDS_LOOK = ("toward_mg", "tip_mg", "up12_mg")
KEY_DEBUG_TAKE = 0x7F  # development builds: a take of that many seconds, no voice detector
# From the start of a take to its first sample kept (EF-20): the micro's supply and settling,
# 60 ms, then the two first blocks of 100 ms dropped while the PDM filter settles
MIC_WARMUP_S = 0.26
STATE_FLASH_FULL = 0x08
LIGHT_STEADY, LIGHT_BREATHE, LIGHT_BLINK, LIGHT_RAINBOW = 0, 1, 2, 3
ACK_RECEIVED, ACK_DAMAGED = 0, 1
RESULT_SUCCESS, RESULT_FAILURE = 0, 1

# Fixed fields of each message of the watch, after the type byte
_WATCH_LAYOUTS = {
    HELLO: ("<BBBBBHBIHBI", ("version", "reason", "state", "fw_major", "fw_minor",
                             "fw_revision", "notes", "note_bytes", "battery_mv", "codecs",
                             "watch_id")),
    NOTE_OFFER: ("<III", ("id", "size", "crc32")),
    NOTE_END: ("<I", ("id",)),
    VALUE: ("<BB", ("key", "status")),
    EVENT: ("<BB", ("gesture", "button")),
    BYE_WATCH: ("<B", ("reason",)),
}
_COUNTERS = ("<IIIHHH", ("notes", "recorded_s", "connected_s", "boots", "watchdog_resets",
                         "events_lost"))
_TRIAL = ("<BHBIBIIHHH", ("format", "minutes_left", "profile", "number", "reason", "connect_ms",
                          "read_ms", "interval", "latency", "timeout"))


class ProtocolError(ValueError):
    """A message the harness cannot read: say so, never guess."""


def parse(data):
    """A notification of the watch, as (name, fields). Extra bytes of a later version are
    ignored; NOTE_DATA and VALUE keep their tail as `data` and `value`."""
    data = bytes(data)
    if not data:
        raise ProtocolError("empty message")
    kind = data[0]
    if kind & TO_WATCH:
        raise ProtocolError(f"type 0x{kind:02x} goes to the watch, not from it")
    if kind == NOTE_DATA:
        return "NOTE_DATA", {"data": data[1:]}
    if kind not in _WATCH_LAYOUTS:
        raise ProtocolError(f"unknown type 0x{kind:02x} (a later version?)")
    layout, names = _WATCH_LAYOUTS[kind]
    size = struct.calcsize(layout)
    if len(data) < 1 + size:
        raise ProtocolError(f"{NAMES[kind]}: {len(data)} bytes, {1 + size} expected")
    fields = dict(zip(names, struct.unpack_from(layout, data, 1)))
    if kind == VALUE:
        fields["value"] = data[1 + size:]
    return NAMES[kind], fields


def counters(value):
    """The value of KEY_COUNTERS (EF-73) as a dict."""
    layout, names = _COUNTERS
    if len(value) < struct.calcsize(layout):
        raise ProtocolError("counters: value too short")
    return dict(zip(names, struct.unpack_from(layout, value)))


def trial(value):
    """The value of KEY_DEBUG_TRIAL (lot N1a) as a dict."""
    layout, names = _TRIAL
    if len(value) < struct.calcsize(layout) or value[0] != 1:
        raise ProtocolError("trial: value too short, or of another format")
    return dict(zip(names, struct.unpack_from(layout, value)))


def describe_trial(t):
    """A line for the trial of the calls and its last call answered."""
    profile = {v: k for k, v in TRIAL_PROFILES.items()}.get(t["profile"], t["profile"])
    state = (f"trial on, {t['minutes_left']} min left, {profile} profile"
             if t["minutes_left"] else "no trial")
    if not t["number"]:
        return f"{state}; no call answered during a trial yet"
    read = ("not read yet" if t["read_ms"] == TRIAL_NONE
            else f"first read {t['read_ms']} ms after the connection")
    return (f"{state}; call {t['number']} ({REASONS.get(t['reason'], t['reason'])}): "
            f"connected {t['connect_ms']} ms after its first advertisement, {read}, link "
            f"{t['interval'] * 1.25:.2f} ms / latency {t['latency']} / {t['timeout'] * 10} ms")


def journal_page(value):
    """The value of KEY_JOURNAL (the Cobalt Link specification): the journal's
    identifier, and its records from the first index, each a dict."""
    if len(value) < 9:
        raise ProtocolError("journal: page too short")
    ident, first, count = struct.unpack_from("<IIB", value)
    if len(value) < 9 + 8 * count:
        raise ProtocolError(f"journal: {count} records announced, {(len(value) - 9) // 8} given")
    records = []
    for i in range(count):
        time_s, temp_cc, cell = struct.unpack_from("<IhH", value, 9 + 8 * i)
        records.append({"index": first + i, "time_s": time_s, "temp_c": temp_cc / 100,
                        "cell_mv": cell & 0x1FFF, "take": bool(cell & 0x4000),
                        "link": bool(cell & 0x8000)})
    return ident, records


def time_msg(utc_ms, tz_minutes):
    return struct.pack("<Bqh", TIME, utc_ms, tz_minutes)


def note_accept(note_id, offset):
    return struct.pack("<BII", NOTE_ACCEPT, note_id, offset)


def note_ack(note_id, verdict, window_s):
    return struct.pack("<BIBB", NOTE_ACK, note_id, verdict, window_s)


def result(note_id, code):
    return struct.pack("<BIB", RESULT, note_id, code)


def set_msg(key, value):
    value = bytes(value)
    if 2 + len(value) > 20:
        raise ProtocolError("a SET value fits 18 bytes")
    return bytes([SET, key]) + value


def get_msg(key):
    return bytes([GET, key])


def bye(reason=0):
    return bytes([BYE_PHONE, reason])


# ---- The voice notes (lot E1): container v2, the Cobalt Link specification ---------------

NOTE_VECTORS = VECTORS.with_name("note_v2.txt")
NOTE_MAGIC = b"CBN2"
NOTE_HEADER_SIZE = 64
CODEC_LC3 = 1
NOTE_ENDS = {0: "a press", 1: "silence", 2: "its longest length", 3: "the flash full",
             4: "a fault of the microphone"}
# Bytes 0 to 63, little-endian: 53-55 reserved (0), 60-62 reserved (0xff)
_NOTE_LAYOUT = "<4sBBBBIIHHIqhHIIIbBBBB3xI3xB"
_NOTE_FIELDS = ("magic", "version", "header_size", "codec", "channels", "id", "rate_hz",
                "frame_us", "frame_bytes", "bitrate", "utc_ms", "tz_minutes", "flags",
                "duration_ms", "data_size", "data_crc", "gain_db", "high_pass_hz", "fw_major",
                "fw_minor", "fw_revision", "header_crc", "state")


def note_header(raw):
    """The header of a note as a dict, or ProtocolError if these bytes are not one."""
    raw = bytes(raw)
    if len(raw) < NOTE_HEADER_SIZE:
        raise ProtocolError(f"{len(raw)} bytes, shorter than a note header")
    h = dict(zip(_NOTE_FIELDS, struct.unpack_from(_NOTE_LAYOUT, raw)))
    if h["magic"] != NOTE_MAGIC or h["version"] != 1 or h["header_size"] != NOTE_HEADER_SIZE:
        raise ProtocolError(f"not a note v2: {raw[:6].hex()}")
    if zlib.crc32(raw[:56]) != h["header_crc"]:
        raise ProtocolError("note header CRC wrong")
    h["time_approx"] = bool(h["flags"] & 1)
    h["end"] = (h["flags"] >> 4) & 0xF
    return h


def check_note(raw, offer):
    """A note received whole, against its offer and its own header: its header."""
    raw = bytes(raw)
    if len(raw) != offer["size"]:
        raise ProtocolError(f"{len(raw)} bytes received, {offer['size']} offered")
    if zlib.crc32(raw) != offer["crc32"]:
        raise ProtocolError(f"CRC32 {zlib.crc32(raw):08x}, {offer['crc32']:08x} offered")
    h = note_header(raw)
    frames = raw[NOTE_HEADER_SIZE:]
    if h["id"] != offer["id"]:
        raise ProtocolError(f"the header says note {h['id']}, note {offer['id']} was offered")
    if h["data_size"] != len(frames) or zlib.crc32(frames) != h["data_crc"]:
        raise ProtocolError("the frames are not those of the header")
    if h["state"] != 0xFF:
        raise ProtocolError(f"state 0x{h['state']:02x}: a note is offered complete (0xff)")
    return h


def lc3_file(h, frames):
    """The frames of a note as a .lc3 file of liblc3 (tools/lc3bin.c), which FFmpeg reads."""
    step = h["frame_bytes"]
    if h["codec"] != CODEC_LC3 or not step or len(frames) % step:
        raise ProtocolError(f"codec {h['codec']}: {len(frames)} bytes in frames of {step}")
    samples = len(frames) // step * h["rate_hz"] * h["frame_us"] // 1_000_000
    head = struct.pack("<9H", 0xCC1C, 18, h["rate_hz"] // 100, h["bitrate"] // 100,
                       h["channels"], h["frame_us"] // 10, 0, samples & 0xFFFF, samples >> 16)
    return head + b"".join(struct.pack("<H", step) + frames[i:i + step]
                           for i in range(0, len(frames), step))


def describe_note(h, size):
    import datetime
    when = "no time on the watch"
    if h["utc_ms"]:
        tz = datetime.timezone(datetime.timedelta(minutes=h["tz_minutes"]))
        when = datetime.datetime.fromtimestamp(h["utc_ms"] / 1000, tz).strftime("%Y-%m-%d %H:%M:%S")
        when += " (approximate)" if h["time_approx"] else ""
    return (f"note {h['id']}: {h['duration_ms'] / 1000:.2f} s of LC3 at {h['bitrate'] // 1000} "
            f"kbit/s, {size} bytes, recorded {when}, ended by "
            f"{NOTE_ENDS.get(h['end'], h['end'])}, firmware {h['fw_major']}.{h['fw_minor']}."
            f"{h['fw_revision']}, gain {h['gain_db']} dB")


def _vectors(path):
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        name, raw, *pairs = line.split()
        yield number, name, bytes.fromhex(raw), dict(p.split("=", 1) for p in pairs)


def _write(name, f):
    """The bytes this side writes for a message of the phone, from its fields."""
    if name == "TIME":
        return time_msg(int(f["utc_ms"]), int(f["tz_minutes"]))
    if name == "NOTE_ACCEPT":
        return note_accept(int(f["id"]), int(f["offset"]))
    if name == "NOTE_ACK":
        return note_ack(int(f["id"]), int(f["verdict"]), int(f["window_s"]))
    if name == "RESULT":
        return result(int(f["id"]), int(f["code"]))
    if name == "SET":
        return set_msg(int(f["key"]), bytes.fromhex(f["value"]))
    if name == "GET":
        return get_msg(int(f["key"]))
    if name == "BYE_PHONE":
        return bye(int(f["reason"]))
    raise ProtocolError(f"{name} is not a message of the phone")


def self_test(path=VECTORS):
    """Every vector: parse the watch's, write the phone's. Returns the number of failures."""
    failures = checked = 0
    for number, name, raw, fields in _vectors(path):
        checked += 1
        try:
            if raw[0] & TO_WATCH:
                got = _write(name, fields)
                ok = got == raw
                detail = f"wrote {got.hex()}"
            else:
                got_name, got = parse(raw)
                want = {k: (bytes.fromhex(v) if k in ("data", "value") else int(v))
                        for k, v in fields.items()}
                ok = got_name == name and got == want
                if ok and name == "VALUE" and fields["key"] == str(KEY_COUNTERS):
                    ok = counters(got["value"]) == {"notes": 42, "recorded_s": 630,
                                                   "connected_s": 95, "boots": 7,
                                                   "watchdog_resets": 1, "events_lost": 0}
                if ok and name == "VALUE" and fields["key"] == str(KEY_DEBUG_TRIAL):
                    ok = trial(got["value"]) == {
                        "format": 1, "minutes_left": 87, "profile": 1, "number": 12,
                        "reason": 2, "connect_ms": 1834, "read_ms": 2410, "interval": 24,
                        "latency": 0, "timeout": 400}
                if ok and name == "VALUE" and fields["key"] == str(KEY_JOURNAL):
                    ident, recs = journal_page(got["value"])
                    ok = ident == 0xC0B91AA1 and [
                        (r["index"], r["time_s"], r["temp_c"], r["cell_mv"], r["link"])
                        for r in recs] == [(1020, 1790194800, 21.75, 2903, False),
                                           (1021, 1790195400, -3.25, 2851, True)]
                detail = f"read {got_name} {got}"
        except ProtocolError as exc:
            ok, detail = False, str(exc)
        if not ok:
            failures += 1
            print(f"line {number}: {name}: {detail}")
    # What the harness must refuse
    for bad, why in ((b"", "empty"), (bytes([TIME]), "wrong way"), (bytes([0x40]), "unknown"),
                     (bytes([NOTE_OFFER, 1, 2]), "short")):
        checked += 1
        try:
            parse(bad)
            failures += 1
            print(f"accepted a message it should refuse ({why})")
        except ProtocolError:
            pass
    # The note container, on the vectors the watch's own code is checked with
    for number, name, raw, fields in _vectors(path.with_name(NOTE_VECTORS.name)):
        checked += 1
        try:
            if name == "HEADER":
                h = note_header(raw)
                got = {k: h[k] for k in fields if k in h}
                want = {k: int(v) for k, v in fields.items() if k in h}
                ok = got == want and h["time_approx"] and (h["fw_major"], h["fw_minor"],
                                                          h["fw_revision"]) == (0, 2, 2)
                detail = f"read {got}"
            else:
                offer = {k: int(v) for k, v in fields.items()}
                h = check_note(raw, offer)
                lc3 = lc3_file(h, raw[NOTE_HEADER_SIZE:])
                ok = len(lc3) == 18 + 3 * (2 + 20) and lc3[:4] == bytes([0x1C, 0xCC, 18, 0])
                detail = f"lc3 file of {len(lc3)} bytes"
        except ProtocolError as exc:
            ok, detail = False, str(exc)
        if not ok:
            failures += 1
            print(f"{NOTE_VECTORS.name} line {number}: {name}: {detail}")
    # A byte changed anywhere in a note: refused
    _, _, note, fields = next(v for v in _vectors(path.with_name(NOTE_VECTORS.name))
                              if v[1] == "NOTE")
    for at in (0, 8, 40, 63, NOTE_HEADER_SIZE, len(note) - 1):
        checked += 1
        bad = bytearray(note)
        bad[at] ^= 0x01
        try:
            check_note(bad, {k: int(v) for k, v in fields.items()})
            failures += 1
            print(f"accepted a note with byte {at} changed")
        except ProtocolError:
            pass
    # The thermo session's arithmetic: the decimals of the status line, the degree shown
    for line, key, want in (("dis=16 temp=21.5 die=21.25", "temp", 21.5),
                            ("dis=16 temp=21.5 die=-0.25", "die", -0.25),
                            ("dis=16 temp=- die=-", "temp", None),
                            ("dis=16 temp=-3.0 die=-", "die", None)):
        checked += 1
        if _status_f(line, key) != want:
            failures += 1
            print(f"{line!r}: {key} read {_status_f(line, key)}, {want} expected")
    for degrees, want in ((21.4, 21), (21.5, 22), (-0.25, 0), (-0.5, -1), (-5.2, -5)):
        checked += 1
        if _shown(degrees) != want:
            failures += 1
            print(f"{degrees} degC shown as {_shown(degrees)}, {want} expected")
    # The wrist: the tuning's bytes, and the counters of the status line
    checked += 1
    tuning = wrist_tuning(struct.pack("<BBBBBHh", 2, 1, 60, 25, 3, 3000, -200))
    if tuning != {"wake": 2, "samples": 1, "orient": 60, "duration": 25, "tap": 3,
                  "lowpower_ms": 3000, "faceup_mg": -200}:
        failures += 1
        print(f"wrist tuning read {tuning}")
    checked += 1
    more = [3, 2, 60, 25, 3, 3000, 500, 1, 0, 1, 2]
    tuning = wrist_tuning(wrist_tuning_bytes(more))
    if len(wrist_tuning_bytes(more)) != 13 or len(wrist_tuning_bytes(more[:7])) != 9 or \
            tuning != dict(zip(WRIST_FIELDS + WRIST_FIELDS_MORE, more)):
        failures += 1
        print(f"wrist tuning of 0.2.1+17 read {tuning}")
    checked += 1
    look = more + [300]
    tuning = wrist_tuning(wrist_tuning_bytes(look))
    if len(wrist_tuning_bytes(look)) != 15 or \
            tuning != dict(zip(WRIST_FIELDS + WRIST_FIELDS_MORE + WRIST_FIELDS_LOOK, look)):
        failures += 1
        print(f"wrist tuning of 0.2.1+28 read {tuning}")
    checked += 1
    look = more + [300, 500, -20]
    tuning = wrist_tuning(wrist_tuning_bytes(look))
    if len(wrist_tuning_bytes(look)) != 19 or \
            tuning != dict(zip(WRIST_FIELDS + WRIST_FIELDS_MORE + WRIST_FIELDS_LOOK, look)):
        failures += 1
        print(f"wrist tuning of 0.2.1+29 read {tuning}")
    checked += 1
    if _status_wrist("temp=24.00 log=12 wrist=7/3/1 wz=-850") != (7, 3, 1, -850) or \
            _status_wrist("temp=24.00 log=12") is not None:
        failures += 1
        print("wrist counters not read from the status line")
    # The accelerometer's registers (key 0x7B): low power, X 5, Y -3, Z 512 counts; the
    # wake-up reference 0, -1, 32 (x 31.25 mg); the orientation one 16, -512, 512 counts
    checked += 1
    regs = bytes([0x90, 0, 0, 0x02, 0x05, 0x00, 0xFD, 0x0F, 0x00, 0x02, 0, 0, 0])
    config = bytearray(38)
    config[0x31 - 0x19:0x34 - 0x19] = bytes([0x00, 0xFF, 0x20])
    config[0x39 - 0x19:0x3F - 0x19] = bytes([0x10, 0x00, 0x00, 0x0E, 0x00, 0x02])
    value = bytes([1, 0x03, 0x00, 0x13, 0x08]) + struct.pack("<HHH", 7, 1, 2) + b"\0" + regs \
        + bytes(config) + bytes([0x03, 0x06])
    d = accel_diag(value)
    if (d["power"], d["acc_mg"], d["orient_ref_mg"], d["wake_ref_mg"], d["wakes"],
            d["overruns"], d["empty"], d["seen0"], d["regs"][0x58]) != \
            ("low power", (10, -6, 1000), (31, -1000, 1000), (0, -31, 1000), 7, 1, 2, 0x13, 6) \
            or len(accel_lines(d)) != 3:
        failures += 1
        print(f"accel read {d}")
    summary = thermo_summary([(0, 21.0, 21.25), (5, 21.5, 21.25), (10, 21.0, 21.25)])
    checked += 1
    if len(summary) != 3 or "change 2 time(s), 1 of them back" not in summary[0] or \
            "change 0 time(s), 0 of them back" not in summary[1] or "+0.08" not in summary[2]:
        failures += 1
        print(f"thermo summary: {summary}")
    print(f"cobalt_link: {checked} check(s), {failures} failure(s)")
    return failures


# ---- Sessions over Bluetooth (lot C1): the harness plays the phone ------------------------

# "notes" takes the notes waiting as the app does, and keeps them in NOTES_DIR
SCENARIOS = ("notes", "basic", "settings", "hold", "bye", "smp")
# Not in "all": a setting left changed, or takes recorded for the test on a development
# build (key 0x7F)
EXTRA_SCENARIOS = ("light", "resume", "hundred", "leave", "recall", "name", "thermo", "journal",
                   "wrist", "txpower", "delay", "status", "daily", "dailycheck", "trial",
                   "trialread")
# The call of the phone again (firmware/watch/src/recall.h, lot E2): past the window of a call
# and the end of a link, 15 min of silence, then 30, then 60 at each call again
RECALL_WINDOW_S, RECALL_SILENCE_S, RECALL_SILENCE_MAX_S = 130, 900, 3600
REASON_NOTE = 1
# The daily call (firmware/watch/src/daily.h): a day without any contact with the phone; the
# development build shortens the day (key 0x7E, 60 to 86400 s, until the next boot)
REASON_DAILY, KEY_DEBUG_DAILY, DAILY_S = 3, 0x7E, 86400
LOGS_DIR = pathlib.Path(__file__).resolve().parents[1] / "logs"
NOTES_DIR = LOGS_DIR / "notes"
options = argparse.Namespace(whisper=None, takes=100, take_s=2, window=10, name=None,
                             minutes=10.0, every=5.0, journal_from=0, period=None, wrist=None,
                             trace=False, tx_power=0, daily=60, trial=(60, 0, None))
# Seconds to add to the PC clock for network time (SNTP, measured once per run): the TIME of a
# session is what the watch calibrates its crystal on (EF-02), and a phone follows network time,
# while this PC's clock follows no time server and even jumps at a restart
ntp_offset_s = 0.0
network_time = False  # a time server answered: TIME carries network time
# False with --no-time: the sessions leave the watch's clock alone, for a drift run of
# cobalt_time.py (EV-10: seven days with no setting)
send_time = True
LIGHTS = {"white": [0, 255, 255, 255], "breathe": [1, 255, 255, 255],
          "blink": [2, 255, 255, 255], "rainbow": [3, 0, 0, 0]}
light_choice = LIGHTS["white"]

# SMP echo (OS group 0, command 0), version 2 header, sequence 0x42: {"d": "cobalt"}
SMP_ECHO = bytes([0x0A, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x42, 0x00,
                  0xA1, 0x61, 0x64, 0x66]) + b"cobalt"
SMP_BUSY_S = 30.0  # the watch ends no session sooner after an SMP command (session.h)


class Link:
    """One connection to the watch, its notifications in a queue."""

    def __init__(self, client, log, connect=None):
        self.client = client
        self.log = log
        self.connect = connect  # a new connection to the same watch: reopen()
        self.queue = None
        self.gone = None
        self.quiet = False    # NOTE_DATA not logged one by one
        self.decline = True   # an offer nobody waits for is answered "damaged"
        self.rates = []       # (bytes, seconds) of each note received
        self.wavs = []        # the notes decoded, transcribed after the sessions
        self.watch_id = 0

    async def open(self):
        import asyncio
        self.queue = asyncio.Queue()
        self.gone = asyncio.Event()
        await self.client.start_notify(TX_UUID, self._on_notify)
        self.log(f"subscribed to TX, MTU {self.client.mtu_size}")

    async def reopen(self):
        """Cut this link from the phone's side, as a phone out of range does, and connect
        again: the new Link."""
        import asyncio
        self.log("  the phone cuts the link")
        await asyncio.wait_for(self.client.disconnect(), 10)
        await asyncio.sleep(2.0)
        link = await self.connect()
        link.watch_id = self.watch_id
        return link

    def _on_notify(self, _characteristic, data):
        self.queue.put_nowait(bytes(data))

    async def expect(self, timeout, *names):
        """The next message of the watch, which must be one of `names`. With `decline`, a
        note offered when no offer is awaited is left on the watch ("damaged": it is not
        offered again in this session), as a phone that takes it later would do."""
        import asyncio
        import time
        until = time.monotonic() + timeout
        while True:
            try:
                raw = await asyncio.wait_for(self.queue.get(),
                                             max(until - time.monotonic(), 0.001))
            except asyncio.TimeoutError as exc:
                raise ProtocolError(f"nothing from the watch in {timeout:.0f} s, "
                                    f"{' or '.join(names)} expected") from exc
            name, fields = parse(raw)
            if not (self.quiet and name == "NOTE_DATA"):
                self.log(f"< {name} {fields}")
            if name == "NOTE_OFFER" and name not in names and self.decline:
                await self.send(note_ack(fields["id"], ACK_DAMAGED, 0), "NOTE_ACK damaged: left")
                continue
            if names and name not in names:
                raise ProtocolError(f"{name} came, {' or '.join(names)} expected")
            return name, fields

    async def send(self, data, what):
        self.log(f"> {what} {bytes(data).hex()}")
        await self.client.write_gatt_char(RX_UUID, data, response=False)


async def _time(link):
    """TIME, as a phone sends it after HELLO, and in the watch's clock log of cobalt_time.py,
    where the next drift starts; none with --no-time (the watch needs none)."""
    import datetime
    import time
    from cobalt_time import log_setting
    if not send_time:
        link.log("  no TIME (--no-time): the watch keeps its own time")
        return
    now = datetime.datetime.now().astimezone()
    utc_ms = int((time.time() + ntp_offset_s) * 1000)
    await link.send(time_msg(utc_ms, int(now.utcoffset().total_seconds() // 60)), "TIME")
    log_setting(link.client.address, utc_ms, network_time)


async def _hello(link):
    import time
    started = time.monotonic()
    _, hello = await link.expect(2.0, "HELLO")
    link.log(f"  HELLO after {time.monotonic() - started:.2f} s: protocol {hello['version']}, "
             f"firmware {hello['fw_major']}.{hello['fw_minor']}.{hello['fw_revision']}, "
             f"reason {REASONS.get(hello['reason'], hello['reason'])}, notes {hello['notes']}, "
             f"cell {hello['battery_mv']} mV, state 0x{hello['state']:02x}, "
             f"watch 0x{hello['watch_id']:08x}")
    if hello["version"] != PROTOCOL_VERSION:
        raise ProtocolError(f"protocol {hello['version']}, this harness speaks {PROTOCOL_VERSION}")
    return hello


async def _value(link, key, expect_status=0):
    """The VALUE of `key`; with expect_status None, its value if good, None if not."""
    _, value = await link.expect(3.0, "VALUE")
    if expect_status is None and value["key"] == key:
        return value["value"] if value["status"] == 0 else None
    if value["key"] != key or value["status"] != expect_status:
        raise ProtocolError(f"VALUE of key {value['key']} status {value['status']}, "
                            f"key {key} status {expect_status} expected")
    return value["value"]


async def _watch_ends(link, reason, within):
    _, bye_fields = await link.expect(within, "BYE_WATCH")
    if bye_fields["reason"] != reason:
        raise ProtocolError(f"BYE reason {bye_fields['reason']}, {reason} expected")
    import asyncio
    try:
        await asyncio.wait_for(link.gone.wait(), 5.0)
        link.log("  the watch cut the link")
    except asyncio.TimeoutError as exc:
        raise ProtocolError("BYE came but the link stayed") from exc


async def _receive(link, offer, offset=b"", cut_at=None):
    """The bytes of an offered note from len(offset) on, after the `offset` bytes the phone
    had already; with `cut_at`, only up to about that many (the resume scenario)."""
    import time
    data = bytearray(offset)
    await link.send(note_accept(offer["id"], len(data)), f"NOTE_ACCEPT at {len(data)}")
    asked = time.monotonic()
    started = None
    quiet = link.quiet
    link.quiet = True  # one line per note, not one per notification
    try:
        while True:
            name, fields = await link.expect(10.0, "NOTE_DATA", "NOTE_END")
            if name == "NOTE_END":
                if fields["id"] != offer["id"]:
                    raise ProtocolError(f"NOTE_END of note {fields['id']}")
                break
            if started is None:
                started = time.monotonic()  # the rate from the first data on
            data += fields["data"]
            if len(data) > offer["size"]:
                raise ProtocolError(f"{len(data)} bytes, more than the {offer['size']} offered")
            if cut_at is not None and len(data) >= cut_at:
                break
    finally:
        link.quiet = quiet
    ended = time.monotonic()
    link.accepted, link.first_data = asked, started  # for the delay scenario (ENF-06)
    got = len(data) - len(offset)
    if started is not None and got > 0:
        took = max(ended - started, 1e-3)
        link.log(f"  {got} bytes of note {offer['id']} in {took:.2f} s: {got / took / 1000:.1f} "
                 f"kB/s, first data {(started - asked) * 1000:.0f} ms after the acceptance")
        link.rates.append((got, took))
    return bytes(data)


def _save(link, raw, h):
    """The note on the PC, as sent and as a standard LC3 file: fast, before the
    acknowledgement, which the watch awaits 5 s at most."""
    NOTES_DIR.mkdir(parents=True, exist_ok=True)
    stem = NOTES_DIR / f"note-{link.watch_id:08x}-{h['id']:05d}"
    stem.with_suffix(".cbn2").write_bytes(raw)
    stem.with_suffix(".lc3").write_bytes(lc3_file(h, raw[NOTE_HEADER_SIZE:]))
    return stem


def _decode(stem):
    """The LC3 file as a WAV by FFmpeg: None without FFmpeg, else whether it decoded."""
    try:
        done = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i",
                               str(stem.with_suffix(".lc3")), "-ar", "16000", "-ac", "1",
                               "-c:a", "pcm_s16le", str(stem.with_suffix(".wav"))],
                              capture_output=True, text=True, check=False)
    except FileNotFoundError:
        return None
    return done.returncode == 0


async def _take_note(link, offer, window, offset=b"", suffix="", listen=True):
    """An offered note, from `offset` on: received, checked, kept, acknowledged, then decoded
    to WAV, the verdict of the app following within `window` (green if it decoded). With
    `listen`, --whisper transcribes it once the sessions are over."""
    import asyncio
    raw = await _receive(link, offer, offset)
    try:
        h = check_note(raw, offer)
    except ProtocolError as exc:
        await link.send(note_ack(offer["id"], ACK_DAMAGED, 0), "NOTE_ACK damaged")
        raise ProtocolError(f"note {offer['id']} damaged: {exc}") from exc
    link.log(f"  {describe_note(h, len(raw))}{suffix}")
    stem = _save(link, raw, h)
    await link.send(note_ack(offer["id"], ACK_RECEIVED, window),
                    f"NOTE_ACK received, verdict within {window} s")
    decoded = await asyncio.to_thread(_decode, stem)
    if decoded is None:
        link.log("  FFmpeg not found: no WAV")
    elif decoded:
        link.log(f"  kept as {stem.with_suffix('.wav')}")
        if listen:
            link.wavs.append(stem.with_suffix(".wav"))
    else:
        link.log(f"  FFmpeg could not decode {stem.with_suffix('.lc3').name}")
    if window:
        # What the app does with it, the verdict: green on the LED, red if it failed
        await link.send(result(offer["id"], RESULT_FAILURE if decoded is False else RESULT_SUCCESS),
                        "RESULT")
    if decoded is False:
        raise ProtocolError(f"note {offer['id']}: its LC3 frames do not decode")
    return h


async def _debug_take(link, seconds):
    """A take of the watch, started from here (key 0x7F, development builds): when its VALUE
    came, within a connection interval of the start of the take."""
    import time
    await link.send(set_msg(KEY_DEBUG_TAKE, [seconds]), f"SET take {seconds} s")
    value = await _value(link, KEY_DEBUG_TAKE, expect_status=None)
    if value is None:
        raise ProtocolError("the watch refused the take: a product build, or a take runs")
    return time.monotonic()


async def _take_all(link):
    """Every note offered, those HELLO counted and any kept meanwhile, until the BYE (0) of the
    watch, which then cuts the link: the number received."""
    import asyncio
    link.decline = False
    received = 0
    while True:
        kind, fields = await link.expect(5.0 + options.window, "NOTE_OFFER", "BYE_WATCH")
        if kind == "BYE_WATCH":
            break
        await _take_note(link, fields, options.window)
        received += 1
    link.log(f"  {received} note(s) received" if received else "  no note waiting")
    if fields["reason"] != 0:
        raise ProtocolError(f"BYE reason {fields['reason']}, 0 expected")
    await asyncio.wait_for(link.gone.wait(), 5.0)
    link.log("  the watch cut the link")
    return received


async def _leave_note(link):
    """A take started from here (development builds), its note put off as a phone may do (a
    NOTE_ACK "damaged": the watch keeps it and offers it again), then the phone's BYE: the note
    waits on the watch. Its offer."""
    import asyncio
    await link.send(set_msg(KEY_HOLD, [1]), "SET hold 1")
    await _value(link, KEY_HOLD)
    await link.send(get_msg(KEY_COUNTERS), "GET counters")
    last = counters(await _value(link, KEY_COUNTERS))["notes"]
    await _debug_take(link, options.take_s)
    while True:  # the notes waiting before come first, oldest first
        _, offer = await link.expect(options.take_s + 8.0, "NOTE_OFFER")
        await link.send(note_ack(offer["id"], ACK_DAMAGED, 0), "NOTE_ACK damaged: put off")
        if offer["id"] > last:
            break
    await link.send(bye(0), "BYE")
    await asyncio.wait_for(link.gone.wait(), 5.0)
    link.log(f"  note {offer['id']} left on the watch: {offer['size']} bytes, "
             f"CRC 0x{offer['crc32']:08x}")
    return offer


async def _listen_for_call(address, since, silence_s, until_s, log):
    """The watch's advertising heard from `since` (time.monotonic()) to `until_s` seconds after
    it, until it advertises fast (100 ms) again: the seconds from `since` to that burst, None
    if none was heard. This PC's scanner hears by fits (nothing for half a minute at times, less
    after a minute of scanning, hence a new scan every 20 s): fast advertising is told by the gaps
    between events under half a second, three in 10 s, which the slow one (1 s) never gives.
    Nothing counts before `silence_s` less 5 s: no call can come sooner, and the 30 s of fast
    advertising a development build gives every disconnection are left out with the rest."""
    import asyncio
    import time
    from bleak import BleakScanner
    after_cut_s, span_s, short_s, shorts = silence_s - 5.0, 10.0, 0.5, 3
    heard = []  # one time per event: the packets of an event come within 50 ms
    address = address.upper()

    def on_advertising(device, _advertisement):
        if device.address.upper() == address:
            t = time.monotonic() - since
            if not heard or t - heard[-1] > 0.05:
                heard.append(t)

    report = 120.0
    while time.monotonic() - since < until_s:
        async with BleakScanner(detection_callback=on_advertising):
            await asyncio.sleep(min(20.0, max(until_s - (time.monotonic() - since), 0.1)))
        gaps = [b for a, b in zip(heard, heard[1:]) if b >= after_cut_s and b - a < short_s]
        for i in range(len(gaps) - shorts + 1):
            if gaps[i + shorts - 1] - gaps[i] <= span_s:
                return gaps[i]
        now = time.monotonic() - since
        if now >= report:
            log(f"  {now:.0f} s: {len(heard)} advertising event(s) heard, slow")
            report += 120.0
    return None


def _status_s(status, name):
    """A number of seconds of the status line (`up=123s`), None if missing."""
    import re
    found = re.search(rf"\b{name}=(\d+)s\b", status)
    return int(found.group(1)) if found else None


def _status_n(status, name):
    """A plain number of the status line (`again=2`), None if missing."""
    import re
    found = re.search(rf"\b{name}=(\d+)\b", status)
    return int(found.group(1)) if found else None


def _status_f(status, name):
    """A decimal number of the status line (`temp=21.5`, `die=-0.25`), None if missing or
    unknown (`temp=-`)."""
    import re
    found = re.search(rf"\b{name}=(-?\d+(?:\.\d+)?)(?=\s|$)", status)
    return float(found.group(1)) if found else None


def _shown(degrees):
    """The whole degree the "tE P" screen shows: rounded half away from zero, as ui.c does."""
    import math
    whole = math.floor(abs(degrees) + 0.5)
    return -whole if degrees < 0 and whole else whole


def thermo_summary(rows):
    """What the thermo session read, field by field (rows of seconds, temp=, die=): range,
    changes and noise from one reading to the next, how often the whole degree shown would
    change, and how often it would go back where it came from (21, 22, 21: the jumps that a
    slow drift does not make); then the gap between the two."""
    import math
    lines = []
    for label, column in (("temp=", 1), ("die=", 2)):
        values = [row[column] for row in rows if row[column] is not None]
        if not values:
            continue
        if len(values) < 2:
            lines.append(f"  {label}: {len(values)} reading")
            continue
        steps = [b - a for a, b in zip(values, values[1:])]
        noise = math.sqrt(sum(s * s for s in steps) / len(steps) / 2)
        shown = [_shown(v) for v in values]
        moves = [b - a for a, b in zip(shown, shown[1:]) if a != b]
        back = sum(1 for a, b in zip(moves, moves[1:]) if (a > 0) != (b > 0))
        lines.append(f"  {label}: {len(values)} readings, {min(values):.2f} to {max(values):.2f} "
                     f"degC, mean {sum(values) / len(values):.2f}; "
                     f"{sum(1 for s in steps if s)} change(s) from one reading to the next, "
                     f"the largest {max(abs(s) for s in steps):.2f}; noise {noise:.2f} degC; "
                     f"the whole degree shown would change {len(moves)} time(s), "
                     f"{back} of them back")
    gaps = [row[2] - row[1] for row in rows if row[1] is not None and row[2] is not None]
    if gaps:
        lines.append(f"  die= minus temp=: {sum(gaps) / len(gaps):+.2f} degC on average")
    return lines


def wrist_tuning(value):
    """The value of KEY_DEBUG_WRIST as a dict (watch/src/wrist.h): 9 bytes, 13 since
    0.2.1+17."""
    if len(value) < 9:
        raise ProtocolError("wrist: value too short")
    tuning = dict(zip(WRIST_FIELDS, struct.unpack_from("<BBBBBHh", value)))
    if len(value) >= 13:
        tuning.update(zip(WRIST_FIELDS_MORE, value[9:13]))
    if len(value) >= 15:
        looks = min((len(value) - 13) // 2, len(WRIST_FIELDS_LOOK))
        tuning.update(zip(WRIST_FIELDS_LOOK, struct.unpack_from(f"<{looks}h", value, 13)))
    return tuning


def wrist_tuning_bytes(values):
    """--wrist as the value of a SET of KEY_DEBUG_WRIST: 7 numbers, 11 for 0.2.1+17, 12 for
    0.2.1+28, 14 for 0.2.1+29."""
    return (struct.pack("<BBBBBHh", *values[:7]) + bytes(values[7:11])
            + b"".join(struct.pack("<h", v) for v in values[11:14]))


def _mg12(lsb, msb):
    """A 12-bit two's complement of the BMA400 at +/-4 g (512 per g), in mg."""
    raw = ((msb & 0x0F) << 8) | lsb
    return round((raw - 0x1000 if raw & 0x800 else raw) * 1000 / 512)


def accel_diag(value):
    """The value of KEY_DEBUG_ACCEL as a dict (watch/src/wrist.h, wrist_diag(); the registers
    of lib/accel.c, accel_dump())."""
    if len(value) < 65 or value[0] != 1:
        raise ProtocolError("accel: value too short, or of another layout")
    regs = {address: value[12 + address] for address in range(0x0D)}
    regs.update({0x19 + i: value[25 + i] for i in range(38)})
    regs.update({0x57: value[63], 0x58: value[64]})
    wakes, overruns, empty = struct.unpack_from("<HHH", value, 5)
    return {
        "stat0": value[1], "stat1": value[2], "seen0": value[3], "seen1": value[4],
        "wakes": wakes, "overruns": overruns, "empty": empty,
        "power": ("sleep", "low power", "normal", "?")[(regs[0x03] >> 1) & 3],
        "acc_mg": tuple(_mg12(regs[4 + 2 * a], regs[5 + 2 * a]) for a in range(3)),
        # 12 bits as the data (p. 79), and 8 signed bits of 31.25 mg for the wake-up (p. 75)
        "orient_ref_mg": tuple(_mg12(regs[0x39 + 2 * a], regs[0x3A + 2 * a]) for a in range(3)),
        "wake_ref_mg": tuple(round(struct.unpack("b", bytes([regs[0x31 + a]]))[0] * 31.25)
                             for a in range(3)),
        "regs": regs,
    }


def accel_lines(d):
    """What accel_diag() found, for the log."""
    regs = d["regs"]
    return [f"  accelerometer {d['power']}, acc {d['acc_mg']} mg; orientation reference "
            f"{d['orient_ref_mg']} mg, wake-up reference {d['wake_ref_mg']} mg",
            f"  since boot: {d['wakes']} wake-up(s) seen, {d['overruns']} overrun(s), "
            f"{d['empty']} empty line(s); last status {d['stat0']:02x} {d['stat1']:02x}, "
            f"every bit seen {d['seen0']:02x} {d['seen1']:02x}",
            "  registers " + " ".join(f"{a:02x}={regs[a]:02x}" for a in
                                      (0x19, 0x1a, 0x1b, 0x1f, 0x20, 0x21, 0x23, 0x2a, 0x2b,
                                       0x2d, 0x2f, 0x30, 0x35, 0x36, 0x38, 0x57))]


def _status_wrist(status):
    """The wrist counters of the status line (`wrist=changes/raises/taps wz=z`), or None."""
    import re
    found = re.search(r"\bwrist=(\d+)/(\d+)/(\d+) wz=(-?\d+)", status)
    return tuple(int(g) for g in found.groups()) if found else None


async def _wrist(link):
    """The tuning of the wrist (lot D4) set if --wrist gives one, then the counters followed
    every 2 s for --minutes while the wearer tries the gestures: each change printed. With
    --trace, the three axes read about once a second, printed and kept in a CSV."""
    import asyncio
    import datetime
    import time
    if options.wrist is not None:
        await link.send(set_msg(KEY_DEBUG_WRIST, list(wrist_tuning_bytes(options.wrist))),
                        "SET wrist")
        value = await _value(link, KEY_DEBUG_WRIST, expect_status=None)
        if value is None:
            raise ProtocolError("the watch refused the tuning: a product image, an image "
                                "before 0.2.1+17 given 11 numbers, or out of range?")
    else:
        await link.send(get_msg(KEY_DEBUG_WRIST), "GET wrist")
        value = await _value(link, KEY_DEBUG_WRIST)
    link.log(f"  tuning {wrist_tuning(value)}")
    diag = await _accel(link)
    if diag is not None:
        for line in accel_lines(diag):
            link.log(line)
    say = link.log
    start = time.monotonic()
    last = None
    status = None
    turn = 0
    rows = []
    link.log = lambda _text: None
    try:
        while time.monotonic() - start < options.minutes * 60 and not link.gone.is_set():
            asked = time.monotonic()
            try:
                # A trace reads the axes every turn and the counters every third: each
                # request waits for the watch's next rendezvous at a latency of 30 (~1 s)
                if status is None or not options.trace or turn % 3 == 0:
                    await link.send(get_msg(KEY_STATUS), "GET status")
                    status = (await _value(link, KEY_STATUS)).decode(errors="replace")
                seen = await _accel(link) if diag is not None else None
            except ProtocolError:
                continue
            turn += 1
            now = _status_wrist(status)
            if options.trace and now is not None and seen is not None:
                x, y, z = seen["acc_mg"]
                stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-5]
                rows.append((stamp, x, y, z, seen["power"], now[0], now[1], now[2]))
                say(f"  {stamp}  acc x {x:5d} y {y:5d} z {z:5d} mg, {seen['power']}, changes "
                    f"{now[0]}, raises {now[1]}, taps {now[2]}, orientation reference z "
                    f"{seen['orient_ref_mg'][2]} mg")
            if now is not None and seen is not None:
                now = now + (seen["wakes"], seen["overruns"])
            if now is not None and now != last:
                if last is not None and len(now) == len(last):
                    more = (f", wake-ups +{now[4] - last[4]}, overruns +{now[5] - last[5]}"
                            if len(now) > 4 else "")
                    say(f"  {time.strftime('%H:%M:%S')}  changes +{now[0] - last[0]}, raises "
                        f"+{now[1] - last[1]}, taps +{now[2] - last[2]}, last z {now[3]} mg"
                        + more)
                last = now
            await asyncio.sleep(max(0.0, (0.3 if options.trace else 2.0)
                                    - (time.monotonic() - asked)))
    finally:
        link.log = say
        if rows:
            path = LOGS_DIR / (f"wrist-{link.client.address.replace(':', '')}-"
                               f"{datetime.datetime.now().strftime('%Y%m%d-%H%M%S')}.csv")
            with open(path, "w", encoding="utf-8") as out:
                out.write("time,x_mg,y_mg,z_mg,power,changes,raises,taps\n")
                for row in rows:
                    out.write(",".join(str(v) for v in row) + "\n")
            say(f"  {len(rows)} reading(s) kept in {path}")
    if last is not None:
        say(f"  in all since boot: {last[0]} changes, {last[1]} raises shown, {last[2]} taps"
            + (f", {last[4]} wake-ups seen, {last[5]} overruns" if len(last) > 4 else ""))
    if diag is not None and not link.gone.is_set():
        try:
            diag = await _accel(link)
        except ProtocolError:
            return  # a late answer of the trace: the counters above are enough
        for line in accel_lines(diag):
            link.log(line)


async def _accel(link):
    """KEY_DEBUG_ACCEL read and decoded; None on an image without it (before 0.2.1+17)."""
    await link.send(get_msg(KEY_DEBUG_ACCEL), "GET accel")
    value = await _value(link, KEY_DEBUG_ACCEL, expect_status=None)
    return accel_diag(value) if value is not None else None


def journal_summary(ident, records):
    """What the journal holds: its span, the temperature, the cell."""
    import datetime
    if not records:
        return [f"  journal {ident:08x}: empty" if ident is not None else "  no journal"]
    first, last = records[0], records[-1]
    temps = [r["temp_c"] for r in records]
    cells = [r["cell_mv"] for r in records]
    rest = [r["cell_mv"] for r in records if not r["take"] and not r["link"]]

    def when(r):
        return datetime.datetime.fromtimestamp(r["time_s"]).strftime("%Y-%m-%d %H:%M:%S")

    lines = [f"  journal {ident:08x}: {len(records)} record(s), index {first['index']} to "
             f"{last['index']} ({last['index'] - first['index'] + 1 - len(records)} skipped), "
             f"from {when(first)} to {when(last)}",
             f"  temperature {min(temps):.2f} to {max(temps):.2f} degC, "
             f"mean {sum(temps) / len(temps):.2f}",
             f"  cell {cells[0]} mV first, {cells[-1]} mV last, {min(cells)} to {max(cells)} mV; "
             f"{len(records) - len(rest)} record(s) under load (a take or a link)"]
    return lines


async def _journal(link):
    """The whole journal from options.journal_from, page by page as the app will (the VALUE
    of the SET is the first page): a CSV in firmware/logs, then the summary."""
    import datetime
    if options.period is not None:
        await link.send(set_msg(KEY_DEBUG_JOURNAL_S, list(struct.pack("<H", options.period))),
                        f"SET journal period {options.period} s")
        if await _value(link, KEY_DEBUG_JOURNAL_S, expect_status=None) is None:
            raise ProtocolError("the watch refused the period: a product image?")
    cursor = options.journal_from
    await link.send(set_msg(KEY_JOURNAL, list(struct.pack("<I", cursor))),
                    f"SET journal from {cursor}")
    say = link.log
    records = []
    pages = misses = 0
    ident = None
    link.log = lambda _text: None  # a page is a line of hex otherwise
    try:
        while True:
            try:
                value = await _value(link, KEY_JOURNAL)
            except ProtocolError:
                # A page that did not come: SET again after the last record received,
                # as the app does (section 4.1); a late page is taken once
                misses += 1
                if misses > 5 or link.gone.is_set():
                    raise
                await link.send(set_msg(KEY_JOURNAL, list(struct.pack("<I", cursor))),
                                f"SET journal from {cursor} again")
                continue
            ident, recs = journal_page(value)
            pages += 1
            recs = [r for r in recs if r["index"] >= cursor]
            if not recs:
                break
            records.extend(recs)
            cursor = recs[-1]["index"] + 1
            await link.send(get_msg(KEY_JOURNAL), "GET journal")
    finally:
        link.log = say
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = LOGS_DIR / f"journal-{link.client.address.replace(':', '')}-{stamp}.csv"
    with open(path, "w", encoding="utf-8") as out:
        out.write("index,time_utc,temp_c,cell_mv,take,link\n")
        for r in records:
            when = datetime.datetime.fromtimestamp(r["time_s"], datetime.timezone.utc)
            out.write(f"{r['index']},{when.isoformat()},{r['temp_c']:.2f},{r['cell_mv']},"
                      f"{int(r['take'])},{int(r['link'])}\n")
    say(f"  {len(records)} record(s) in {pages} page(s) ({misses} asked again), kept in {path}")
    for line in journal_summary(ident, records):
        say(line)


async def _thermo(link):
    """Both thermometers every options.every s for options.minutes, in a held session: a CSV
    in firmware/logs, then the summary."""
    import asyncio
    import datetime
    import time
    say = link.log
    rows = []
    missed = 0
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = LOGS_DIR / f"thermo-{link.client.address.replace(':', '')}-{stamp}.csv"
    start = time.monotonic()
    link.log = lambda _text: None  # two lines a reading otherwise: the readings say enough
    try:
        while time.monotonic() - start < options.minutes * 60 and not link.gone.is_set():
            asked = time.monotonic()
            try:
                await link.send(get_msg(KEY_STATUS), "GET status")
                status = (await _value(link, KEY_STATUS)).decode(errors="replace")
            except ProtocolError:
                missed += 1  # a VALUE lost or late: this PC's small transactions
                if missed > 10:
                    raise
                continue
            t = asked - start
            temp, die = _status_f(status, "temp"), _status_f(status, "die")
            rows.append((t, temp, die))
            say(f"  {t:6.0f} s  temp={'-' if temp is None else f'{temp:.2f}'}"
                + ("" if die is None else f"  die={die:.2f}"))
            await asyncio.sleep(max(0.0, options.every - (time.monotonic() - asked)))
    finally:
        link.log = say
        with open(path, "w", encoding="utf-8") as out:
            out.write("s,temp,die\n")
            for t, temp, die in rows:
                out.write(f"{t:.1f},{'' if temp is None else temp},{'' if die is None else die}\n")
        say(f"  {len(rows)} readings ({missed} lost) kept in {path}")
    for line in thermo_summary(rows):
        say(line)


async def _heard_name(address, want, seconds):
    """The name this PC hears in the watch's advertising within `seconds`, as soon as it is
    `want`; the last one heard otherwise (None: nothing heard)."""
    import asyncio
    import time
    from bleak import BleakScanner
    heard = []
    address = address.upper()

    def on_advertising(device, advertisement):
        if device.address.upper() == address and advertisement.local_name:
            heard.append(advertisement.local_name)

    until = time.monotonic() + seconds
    while time.monotonic() < until and want not in heard:
        async with BleakScanner(detection_callback=on_advertising):  # a new scan: see ble_trial.py
            await asyncio.sleep(min(10.0, max(until - time.monotonic(), 0.1)))
    return want if want in heard else (heard[-1] if heard else None)


async def scenario(name, link):
    """One course of a session (the Cobalt Link specification)."""
    import asyncio
    hello = await _hello(link)
    link.watch_id = hello["watch_id"]
    await _time(link)
    if hello["state"] & STATE_FLASH_FULL:
        link.log("  the watch says its flash is nearly full")
    if name == "notes":
        await _take_all(link)
    elif name == "basic":
        # Nothing waiting: 2 s for a last word, then BYE (0) and the watch cuts
        await _watch_ends(link, 0, 5.0)
    elif name == "settings":
        await link.send(get_msg(KEY_TIME_FORMAT), "GET format")
        was = (await _value(link, KEY_TIME_FORMAT))[0]
        await link.send(set_msg(KEY_TIME_FORMAT, [24]), "SET format 24")
        if (await _value(link, KEY_TIME_FORMAT)) != bytes([24]):
            raise ProtocolError("the format did not become 24")
        await link.send(set_msg(KEY_TIME_FORMAT, [13]), "SET format 13")
        await _value(link, KEY_TIME_FORMAT, expect_status=2)
        await link.send(set_msg(KEY_TIME_FORMAT, [was]), f"SET format {was} back")
        await _value(link, KEY_TIME_FORMAT)
        await link.send(get_msg(KEY_COUNTERS), "GET counters")
        link.log(f"  counters {counters(await _value(link, KEY_COUNTERS))}")
        await link.send(get_msg(KEY_STATUS), "GET status")
        status = (await _value(link, KEY_STATUS)).decode(errors="replace")
        link.log(f"  status {status}")
        if "dis=" not in status:
            raise ProtocolError(f"status line cut at {len(status)} characters")
        # The light of LIGHT (key 0x05): read, a breath of blue, refusals, back
        await link.send(get_msg(KEY_LIGHT), "GET light")
        light = await _value(link, KEY_LIGHT)
        link.log(f"  light: effect {light[0]}, colour {light[1]} {light[2]} {light[3]}")
        await link.send(set_msg(KEY_LIGHT, [LIGHT_BREATHE, 0, 0, 255]), "SET light breath blue")
        if (await _value(link, KEY_LIGHT)) != bytes([LIGHT_BREATHE, 0, 0, 255]):
            raise ProtocolError("the light did not become a breath of blue")
        await link.send(set_msg(KEY_LIGHT, [LIGHT_STEADY, 0, 0, 0]), "SET light black")
        await _value(link, KEY_LIGHT, expect_status=2)
        await link.send(set_msg(KEY_LIGHT, [9, 255, 255, 255]), "SET light unknown effect")
        await _value(link, KEY_LIGHT, expect_status=2)
        await link.send(set_msg(KEY_LIGHT, list(light)), "SET light back")
        await _value(link, KEY_LIGHT)
        await link.send(get_msg(0x7E), "GET unknown key")
        await _value(link, 0x7E, expect_status=1)
        await _watch_ends(link, 0, 5.0)
    elif name == "wrist":
        # The raise of the wrist and the double tap (lot D4), tuned at the wrist
        await link.send(set_msg(KEY_HOLD, [1]), "SET hold 1")
        await _value(link, KEY_HOLD)
        await _wrist(link)
        if not link.gone.is_set():
            await link.send(bye(0), "BYE")
            await asyncio.wait_for(link.gone.wait(), 5.0)
    elif name == "journal":
        # The journal of the temperature and the cell (lot T1), as the app will read it
        await link.send(set_msg(KEY_HOLD, [1]), "SET hold 1")
        await _value(link, KEY_HOLD)
        await _journal(link)
        await link.send(bye(0), "BYE")
        await asyncio.wait_for(link.gone.wait(), 5.0)
    elif name == "thermo":
        # The two thermometers side by side (2026-09-25): each GET of the status line
        # re-arms the hold, so the session lasts as long as the readings
        await link.send(set_msg(KEY_HOLD, [1]), "SET hold 1")
        await _value(link, KEY_HOLD)
        await _thermo(link)
        if not link.gone.is_set():
            await link.send(bye(0), "BYE")
            await asyncio.wait_for(link.gone.wait(), 5.0)
    elif name == "hold":
        await link.send(set_msg(KEY_HOLD, [1]), "SET hold 1")
        await _value(link, KEY_HOLD)
        link.log("  held: nothing should come for 6 s")
        try:
            name_, _ = await link.expect(6.0)
            raise ProtocolError(f"{name_} came in a held session")
        except ProtocolError as exc:
            if "nothing from the watch" not in str(exc):
                raise
        await link.send(bye(0), "BYE")
        await asyncio.wait_for(link.gone.wait(), 5.0)
        link.log("  the watch cut the link after the phone's BYE")
    elif name == "bye":
        await link.send(bye(0), "BYE")
        await asyncio.wait_for(link.gone.wait(), 5.0)
        link.log("  the watch cut the link at once")
    elif name == "txpower":
        # The transmit power (key 0x03, EF-31): 0, 4 or 8 dBm, for the advertising and the
        # link, until the next boot; a range test then measures it (ble_rssi.py --pc-only)
        await link.send(set_msg(KEY_TX_POWER, [options.tx_power & 0xFF]),
                        f"SET tx power {options.tx_power} dBm")
        if (await _value(link, KEY_TX_POWER)) != bytes([options.tx_power & 0xFF]):
            raise ProtocolError("the watch refused the transmit power")
        await link.send(get_msg(KEY_TX_POWER), "GET tx power")
        power = struct.unpack("b", await _value(link, KEY_TX_POWER))[0]
        link.log(f"  transmit power {power} dBm")
        if power != options.tx_power:
            raise ProtocolError(f"transmit power {power} dBm, {options.tx_power} asked")
        await _watch_ends(link, 0, 5.0)
    elif name == "light":
        # The light of LIGHT, as the app will set it (key 0x05), left so
        await link.send(set_msg(KEY_LIGHT, light_choice), f"SET light {light_choice}")
        if (await _value(link, KEY_LIGHT)) != bytes(light_choice):
            raise ProtocolError("the watch refused the light")
        await _watch_ends(link, 0, 5.0)
    elif name == "smp":
        # An update from the phone app runs SMP within the session: the first chunk alone
        # waits 10 to 18 s for the erase of a slot. The watch must not say BYE meanwhile.
        import time
        answer = asyncio.Event()
        await link.client.start_notify(SMP_UUID, lambda _c, _d: answer.set())
        await link.client.write_gatt_char(SMP_UUID, SMP_ECHO, response=False)
        sent = time.monotonic()
        link.log("> SMP echo")
        await asyncio.wait_for(answer.wait(), 5.0)
        link.log(f"  SMP answered; no BYE for {SMP_BUSY_S - 2:.0f} s")
        try:
            name_, _ = await link.expect(SMP_BUSY_S - 2)
            raise ProtocolError(f"{name_} came {time.monotonic() - sent:.1f} s after SMP")
        except ProtocolError as exc:
            if "nothing from the watch" not in str(exc):
                raise
        await _watch_ends(link, 0, 8.0)
        link.log(f"  BYE {time.monotonic() - sent:.1f} s after the SMP command")
    elif name == "resume":
        # A note cut half way by the link, then taken from where it stopped (E1)
        await link.send(set_msg(KEY_HOLD, [1]), "SET hold 1")
        await _value(link, KEY_HOLD)
        await _debug_take(link, 5)
        _, offer = await link.expect(12.0, "NOTE_OFFER")
        part = await _receive(link, offer, cut_at=offer["size"] // 2)
        link = await link.reopen()
        await _hello(link)
        await _time(link)
        while True:  # oldest first: the notes left earlier come before it
            _, again = await link.expect(5.0, "NOTE_OFFER")
            if again["id"] == offer["id"]:
                break
            await link.send(note_ack(again["id"], ACK_DAMAGED, 0), "NOTE_ACK damaged: left")
        if again != offer:
            raise ProtocolError(f"offered as {again}, first as {offer}")
        await _take_note(link, again, 0, offset=part, suffix=f", resumed at byte {len(part)}")
        await _watch_ends(link, 0, 5.0)
    elif name == "leave":
        # A note waiting on the watch: before an update, which must keep it (EF-65)
        await _leave_note(link)
    elif name == "recall":
        # Lot E2: a note put off, then silence: the watch calls the phone again 15 min after
        # the link (30 or 60 min when it has called again unanswered since its last call for
        # another reason: again= of the status line), past the window of its last call. When,
        # the watch says it by its own clock: its uptime at the last call and at the end of the
        # last link (call= and end=); what this PC hears of its advertising is only told
        import time
        await link.send(get_msg(KEY_STATUS), "GET status")
        status = (await _value(link, KEY_STATUS)).decode(errors="replace")
        if "img=confirmed" not in status:
            raise ProtocolError("an image in test goes back to the previous one after 10 min, "
                                "before any call again: confirm it first")
        again = _status_n(status, "again")
        if again is None:
            raise ProtocolError("no again= in the status line")
        silence = min(RECALL_SILENCE_S << min(again, 2), RECALL_SILENCE_MAX_S)
        offer = await _leave_note(link)
        cut = time.monotonic()
        due = RECALL_WINDOW_S + silence
        link.log(f"  listening until call again number {again + 1}, due {silence} to {due} s "
                 f"after the link")
        burst = await _listen_for_call(link.client.address, cut, silence, due + 20.0, link.log)
        link.log(f"  fast advertising heard {burst:.0f} s after the link" if burst is not None
                 else "  no fast advertising heard (this PC's scanner hears by fits)")
        link = await link.connect()
        hello = await _hello(link)
        link.watch_id = hello["watch_id"]
        link.decline = False
        await _time(link)
        await link.send(get_msg(KEY_STATUS), "GET status")
        asked = time.monotonic()
        offers = []  # offered with HELLO, before the VALUE: taken after it
        while True:
            kind, fields = await link.expect(5.0, "NOTE_OFFER", "VALUE")
            if kind == "NOTE_OFFER":
                offers.append(fields)
                continue
            if fields["key"] != KEY_STATUS or fields["status"] != 0:
                raise ProtocolError(f"VALUE of key {fields['key']} status {fields['status']}")
            break
        status = fields["value"].decode(errors="replace")
        link.log(f"  status {status}")
        up, called, ended = (_status_s(status, name_) for name_ in ("up", "call", "end"))
        if None in (up, called, ended):
            raise ProtocolError("no up=, call= or end= in the status line")
        recall = called - ended
        link.log(f"  the watch called again {recall} s after the end of the link, by its clock; "
                 f"reason {REASONS.get(hello['reason'], hello['reason'])}")
        for waiting in offers:
            await _take_note(link, waiting, options.window)
        received = len(offers) + await _take_all(link)
        if hello["reason"] != REASON_NOTE:
            raise ProtocolError(f"HELLO reason {hello['reason']}, {REASON_NOTE} expected")
        # The link that ended is this harness's: the two clocks agree on when, else another
        # link or a restart came between
        if abs((up - ended) - (asked - cut)) > 5:
            raise ProtocolError(f"the last link ended {up - ended} s before by the watch's clock, "
                                f"{asked - cut:.0f} s by this PC's")
        # Whole seconds on the watch, and 5 s between two wake-ups of its loop
        if not silence - 1 <= recall <= due + 6:
            raise ProtocolError(f"a call {recall} s after the link, {silence} to {due} s expected")
        if received < 1:
            raise ProtocolError(f"note {offer['id']} was not offered again")
    elif name == "daily":
        # The daily call (the Cobalt Link specification), first half: the day made --daily
        # seconds (key 0x7E, development builds), then this link ends. Leave the watch alone
        # then, with no program of this PC holding it: Windows connects again by itself to a
        # watch paired with it while one does, and any link puts the call off by a day.
        # `dailycheck` once the day is over: the call, and the day given back.
        period = options.daily
        # The day counts from the end of the window of the last call when that comes after
        # the link (the call at boot, a recall answered): held till then, so that it counts
        # from this link and dailycheck knows when to expect the call
        await link.send(get_msg(KEY_STATUS), "GET status")
        status = (await _value(link, KEY_STATUS)).decode(errors="replace")
        up, called = _status_s(status, "up"), _status_s(status, "call")
        if up is not None and called is not None and up < called + RECALL_WINDOW_S:
            wait = called + RECALL_WINDOW_S - up + 1
            await link.send(set_msg(KEY_HOLD, [1]), "SET hold 1")
            await _value(link, KEY_HOLD)
            link.log(f"  the window of the last call ends in {wait} s: the link held till then")
            await asyncio.sleep(wait)
        await link.send(set_msg(KEY_DEBUG_DAILY, list(struct.pack("<I", period))),
                        f"SET daily {period} s")
        await _value(link, KEY_DEBUG_DAILY)
        await link.send(bye(0), "BYE")
        await asyncio.wait_for(link.gone.wait(), 5.0)
        link.log(f"  the daily call due {period} s after this link: dailycheck after that")
    elif name == "dailycheck":
        # Second half: the watch called with the reason "daily" --daily seconds after the end
        # of the last link, by its own clock (call= and end=); the day given back, or the watch
        # would call every few minutes until its next boot
        await link.send(get_msg(KEY_STATUS), "GET status")
        status = (await _value(link, KEY_STATUS)).decode(errors="replace")
        link.log(f"  status {status}")
        await link.send(set_msg(KEY_DEBUG_DAILY, list(struct.pack("<I", DAILY_S))),
                        f"SET daily {DAILY_S} s again")
        await _value(link, KEY_DEBUG_DAILY)
        await link.send(bye(0), "BYE")
        await asyncio.wait_for(link.gone.wait(), 5.0)
        called, ended = (_status_s(status, name_) for name_ in ("call", "end"))
        if None in (called, ended):
            raise ProtocolError("no call= or end= in the status line")
        if hello["reason"] != REASON_DAILY:
            raise ProtocolError(f"HELLO reason {hello['reason']}, {REASON_DAILY} expected")
        link.log(f"  the watch called {called - ended} s after the end of the last link, by its "
                 f"clock")
        # Whole seconds on the watch, and 5 s between two wake-ups of its loop; a link after
        # the call (ended past called) leaves nothing to check
        if called >= ended and not options.daily <= called - ended <= options.daily + 6:
            raise ProtocolError(f"a call {called - ended} s after the link, {options.daily} "
                                f"expected")
    elif name == "name":
        # The name of the watch (key 0x06): refused when too long, taken once the link is
        # down, then heard in the scan response
        import asyncio
        wanted = options.name
        await link.send(set_msg(KEY_NAME, list(b"CB-91AI du bureau")), "SET name too long")
        await _value(link, KEY_NAME, expect_status=2)
        await link.send(set_msg(KEY_NAME, list(wanted.encode("ascii"))), f"SET name {wanted!r}")
        if (await _value(link, KEY_NAME)) != wanted.encode("ascii"):
            raise ProtocolError("the watch did not keep the name")
        await link.send(get_msg(KEY_NAME), "GET name")
        if (await _value(link, KEY_NAME)) != wanted.encode("ascii"):
            raise ProtocolError("the name read back differs")
        await link.send(bye(0), "BYE")
        await asyncio.wait_for(link.gone.wait(), 5.0)
        heard = await _heard_name(link.client.address, wanted, 60.0)
        link.log(f"  heard in the scan response as {heard!r}")
        if heard != wanted:
            raise ProtocolError(f"the watch advertises {heard!r}, {wanted!r} expected")
    elif name == "trial":
        # The trial of the calls (lot N1a, Cobalt Link key 0x7A, development builds):
        # for --trial minutes the radio behaves as the product's, silent between its calls,
        # and the watch times each call to the connection of the phone; the third figure makes
        # the day of the daily call that long (key 0x7E), so that the watch calls regularly.
        # From the end of this link, the watch answers during its calls only. 0 minutes stops
        # the trial and gives the day back.
        minutes, profile, every = options.trial
        if minutes == 0:
            every = DAILY_S
        await link.send(set_msg(KEY_DEBUG_TRIAL, [minutes, profile]),
                        f"SET trial {minutes} min, profile {profile}")
        await _value(link, KEY_DEBUG_TRIAL)
        if every is not None:
            await link.send(set_msg(KEY_DEBUG_DAILY, list(struct.pack("<I", every))),
                            f"SET daily {every} s")
            await _value(link, KEY_DEBUG_DAILY)
        await link.send(get_msg(KEY_DEBUG_TRIAL), "GET trial")
        link.log(f"  {describe_trial(trial(await _value(link, KEY_DEBUG_TRIAL)))}")
        await link.send(bye(0), "BYE")
        await asyncio.wait_for(link.gone.wait(), 5.0)
        if minutes:
            link.log("  from now on the watch answers during its calls only: a click on ALARM "
                     "(the glass lit), a note" + (f", or every {every} s or so (the daily call)"
                                                  if every else ""))
    elif name == "trialread":
        # What the watch timed of the last call a phone answered during a trial
        await link.send(get_msg(KEY_DEBUG_TRIAL), "GET trial")
        link.log(f"  {describe_trial(trial(await _value(link, KEY_DEBUG_TRIAL)))}")
        await link.send(bye(0), "BYE")
        await asyncio.wait_for(link.gone.wait(), 5.0)
    elif name == "status":
        # The status line, then the phone's BYE: what the watch says of itself
        await link.send(get_msg(KEY_STATUS), "GET status")
        link.log(f"  {(await _value(link, KEY_STATUS)).decode(errors='replace')}")
        await link.send(bye(0), "BYE")
        await asyncio.wait_for(link.gone.wait(), 5.0)
    elif name == "delay":
        # ENF-06: from the end of a take to the first byte of its note, in a held session once
        # the latency of 30 is on (the preferred parameters come 5 s after the connection).
        # The end of a take is placed from the VALUE of its SET, within an interval of its
        # start, then the warm-up of the micro and the length its note carries.
        import re
        import statistics
        import time
        await link.send(set_msg(KEY_HOLD, [1]), "SET hold 1")
        await _value(link, KEY_HOLD)
        await asyncio.sleep(6.0)
        await link.send(get_msg(KEY_STATUS), "GET status")
        status = (await _value(link, KEY_STATUS)).decode(errors="replace")
        params = re.search(r"\bcp=(\S+)", status)
        link.log(f"  link {params[1] if params else '(no cp= in the status line)'}: "
                 f"interval ms/latency/supervision ms")
        rows = []
        for count in range(1, options.takes + 1):
            started = await _debug_take(link, options.take_s)
            try:
                _, offer = await link.expect(options.take_s + 8.0, "NOTE_OFFER")
            except ProtocolError:
                # Does the watch still answer? Its status line, and its notes waiting
                link.log(f"  take {count}: no offer; asking the watch")
                await link.send(get_msg(KEY_STATUS), "GET status")
                await _value(link, KEY_STATUS)
                raise
            offered = time.monotonic()
            h = await _take_note(link, offer, 0, listen=False)
            end = started + MIC_WARMUP_S + h["duration_ms"] / 1000
            row = (offered - end, link.accepted - offered, link.first_data - link.accepted,
                   link.first_data - end)
            rows.append(row)
            link.log(f"  take {count} of {options.takes}: end to offer {row[0] * 1000:.0f} ms, "
                     f"offer to acceptance {row[1] * 1000:.0f} ms, acceptance to first byte "
                     f"{row[2] * 1000:.0f} ms; end to first byte {row[3] * 1000:.0f} ms")
        for i, what in enumerate(("end to offer", "offer to acceptance",
                                  "acceptance to first byte", "end to first byte")):
            values = [r[i] * 1000 for r in rows]
            link.log(f"  {what}: median {statistics.median(values):.0f} ms, "
                     f"{min(values):.0f} to {max(values):.0f} ms over {len(values)} take(s)")
        await link.send(bye(0), "BYE")
        await asyncio.wait_for(link.gone.wait(), 5.0)
    elif name == "hundred":
        # Takes started from here, each received as it ends, in a held session (E1:
        # "cent notes sans perte")
        await link.send(set_msg(KEY_HOLD, [1]), "SET hold 1")
        await _value(link, KEY_HOLD)
        for count in range(1, options.takes + 1):
            await _debug_take(link, options.take_s)
            _, offer = await link.expect(options.take_s + 8.0, "NOTE_OFFER")
            h = await _take_note(link, offer, 0, listen=False)
            link.log(f"  take {count} of {options.takes}: note {h['id']}, "
                     f"{h['duration_ms']} ms, whole")
        await link.send(bye(0), "BYE")
        await asyncio.wait_for(link.gone.wait(), 5.0)
    return link


async def run_sessions(address, names, log):
    """Each scenario on a connection of its own; returns the failures."""
    import asyncio
    from bleak import BleakClient, BleakScanner
    failures = 0
    rates = []
    wavs = []
    for number, name in enumerate(names):
        log(f"--- scenario {name}")
        clients = []

        async def connect():
            device = await BleakScanner.find_device_by_address(address, timeout=90.0)
            if device is None:
                raise LookupError("the watch was not heard in 90 s")
            gone = asyncio.Event()
            client = BleakClient(device, timeout=30.0,
                                 disconnected_callback=lambda _c: gone.set(),
                                 winrt={"use_cached_services": False})
            clients.append(client)
            await client.connect()
            link = Link(client, log, connect)
            await link.open()
            link.gone = gone
            link.rates = rates
            link.wavs = wavs
            return link

        try:
            await scenario(name, await connect())
            log(f"scenario {name}: passed")
        except LookupError as exc:
            # Out of reach: the next scenarios would each wait as long
            log(f"scenario {name}: FAILED ({exc}); the others are not played")
            failures += len(names) - number
            break
        except Exception as exc:  # noqa: BLE001 - report and go on
            failures += 1
            log(f"scenario {name}: FAILED ({type(exc).__name__}: {exc})")
        finally:
            for client in clients:
                try:
                    await asyncio.wait_for(client.disconnect(), 10)
                except Exception:  # noqa: BLE001 - the watch may have cut already
                    pass
        await asyncio.sleep(2.0)
    if rates:
        total, took = sum(r[0] for r in rates), sum(r[1] for r in rates)
        slowest = min(r[0] / r[1] for r in rates)
        log(f"notes: {len(rates)} transfer(s), {total} bytes in {took:.1f} s of data, "
            f"{total / took / 1000:.1f} kB/s on the whole, {slowest / 1000:.1f} kB/s the slowest")
    if options.whisper and wavs:
        # After the sessions: a transcription takes longer than the watch waits for an
        # acknowledgement (18 s for 1 s of voice with the large model on this PC's CPU)
        import codec_eval  # FFmpeg's whisper filter, as for lot K3
        for wav in wavs:
            text = codec_eval.transcribe(wav, pathlib.Path(options.whisper), {})
            log(f"{wav.name}: « {text} »")
    return failures


def decode_files(paths, whisper):
    """Notes the phone kept (app/android, lot N1b), .cbn2 files or folders of them: checked as
    a session checks them, then a .lc3 and a .wav next to each, and --whisper's transcription.
    No watch needed. The number that failed."""
    files = []
    for p in paths:
        files += sorted(p.rglob("*.cbn2")) if p.is_dir() else [p]
    failures, wavs = 0, []
    for f in files:
        raw = f.read_bytes()
        try:
            h = note_header(raw)
            frames = raw[NOTE_HEADER_SIZE:]
            if h["data_size"] != len(frames) or zlib.crc32(frames) != h["data_crc"]:
                raise ProtocolError("the frames are not those of the header")
            stem = f.with_suffix("")
            stem.with_suffix(".lc3").write_bytes(lc3_file(h, frames))
            decoded = _decode(stem)
            print(f"{f}: {describe_note(h, len(raw))}; "
                  + ("WAV" if decoded else "no WAV (FFmpeg missing or refused)"))
            if decoded:
                wavs.append(stem.with_suffix(".wav"))
        except ProtocolError as exc:
            failures += 1
            print(f"{f}: {exc}")
    if whisper and wavs:
        import codec_eval
        for wav in wavs:
            print(f"{wav.name}: « {codec_eval.transcribe(wav, pathlib.Path(whisper), {})} »")
    print(f"{len(files) - failures} of {len(files)} note(s) read")
    return 1 if failures or not files else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--self-test", action="store_true",
                        help="check the messages against the shared vectors")
    parser.add_argument("--decode", nargs="+", type=pathlib.Path, metavar="CBN2",
                        help="notes the phone kept (.cbn2 files, or folders of them): checked, "
                             "then a .lc3 and a .wav next to each, --whisper transcribes them; "
                             "no watch needed")
    parser.add_argument("--vectors", type=pathlib.Path, default=VECTORS)
    parser.add_argument("--address", help="Bluetooth address of the watch, for a session")
    parser.add_argument("--trial", default="60",
                        help="for the trial session: MINUTES[,PROFILE[,EVERY_S]], minutes 0 "
                             "(stop) to 240, profile product or apple, and the day of the daily "
                             "call made EVERY_S (60 to 86400) for regular calls (lot N1a)")
    parser.add_argument("--no-time", action="store_true",
                        help="send no TIME: the watch's clock runs free, as a drift run of "
                             "cobalt_time.py needs (EV-10)")
    parser.add_argument("--daily", type=int, default=60,
                        help="for the daily session: the day made this many seconds, 60 to 86400, "
                             "until the end of the session")
    parser.add_argument("--tx-power", type=int, default=0, choices=(0, 4, 8),
                        help="for the txpower session: the transmit power in dBm, until the next "
                             "boot")
    parser.add_argument("--light", default="white",
                        help="for the light session: white, breathe, blink, rainbow, or "
                             "effect,red,green,blue")
    parser.add_argument("--whisper", action="store_true",
                        help="transcribe the notes received, once the sessions are over")
    parser.add_argument("--whisper-model", type=pathlib.Path,
                        default=pathlib.Path.home() / "whisper-models"
                        / "ggml-large-v3-turbo-q5_0.bin", help="the whisper.cpp model")
    parser.add_argument("--takes", type=int, default=100, help="takes of the hundred and delay sessions")
    parser.add_argument("--take-s", type=int, default=2,
                        help="length of each take of hundred, delay, leave and recall, 1 to 60 s")
    parser.add_argument("--name", help="for the name session: 1 to 11 characters of printable "
                                       "ASCII, no space at either end")
    parser.add_argument("--window", type=int, default=10,
                        help="verdict window of the notes session, 0 to 60 s (0: no RESULT)")
    parser.add_argument("--minutes", type=float, default=10.0,
                        help="length of the thermo session, 0.5 to 240 min")
    parser.add_argument("--every", type=float, default=5.0,
                        help="the thermo session reads the thermometers every so many s, 2 to 60")
    parser.add_argument("--from", dest="journal_from", type=int, default=0,
                        help="the journal session reads from this index (0: all)")
    parser.add_argument("--period", type=int,
                        help="the journal session first sets the journal's period, 1 to 600 s, "
                             "until the next boot (development images only)")
    parser.add_argument("--wrist",
                        help="for the wrist session, a tuning set first: " + ",".join(WRIST_FIELDS)
                             + "[," + ",".join(WRIST_FIELDS_MORE) + "[,"
                             + ",".join(WRIST_FIELDS_LOOK) + "]] (development images only; "
                             "four more since 0.2.1+17, one more since 0.2.1+28, two more "
                             "since 0.2.1+29)")
    parser.add_argument("--trace", action="store_true",
                        help="the wrist session reads the three axes about once a second, "
                             "prints them and keeps them in a CSV (0.2.1+17 and later)")
    parser.add_argument("scenarios", nargs="*", choices=SCENARIOS + EXTRA_SCENARIOS + ("all",),
                        help="sessions to play with the watch (lot C1)")
    args = parser.parse_args()
    global light_choice
    options.whisper = args.whisper_model if args.whisper else None
    options.takes, options.take_s, options.window = args.takes, args.take_s, args.window
    options.name = args.name
    options.minutes, options.every = args.minutes, args.every
    options.journal_from, options.period = args.journal_from, args.period
    options.trace = args.trace
    options.tx_power = args.tx_power
    options.daily = args.daily
    if not 60 <= args.daily <= 86400:
        parser.error("--daily: 60 to 86400 s")
    try:
        parts = args.trial.split(",")
        minutes = int(parts[0])
        profile = TRIAL_PROFILES[parts[1]] if len(parts) > 1 else 0
        every = int(parts[2]) if len(parts) > 2 else None
        if len(parts) > 3 or not 0 <= minutes <= 240 or (
                every is not None and not 60 <= every <= 86400):
            raise ValueError
    except (ValueError, KeyError):
        parser.error("--trial: MINUTES[,PROFILE[,EVERY_S]], 0 to 240, product or apple, "
                     "60 to 86400")
    options.trial = (minutes, profile, every)
    if args.wrist is not None:
        try:
            options.wrist = [int(v, 0) for v in args.wrist.split(",")]
        except ValueError:
            options.wrist = []
        if len(options.wrist) not in (len(WRIST_FIELDS),
                                      len(WRIST_FIELDS) + len(WRIST_FIELDS_MORE),
                                      len(WRIST_FIELDS) + len(WRIST_FIELDS_MORE) + 1,
                                      len(WRIST_FIELDS) + len(WRIST_FIELDS_MORE)
                                      + len(WRIST_FIELDS_LOOK)):
            parser.error("--wrist: " + ",".join(WRIST_FIELDS) + "[," +
                         ",".join(WRIST_FIELDS_MORE) + "[," + ",".join(WRIST_FIELDS_LOOK) + "]]")
    if not 0.5 <= args.minutes <= 240 or not 2 <= args.every <= 60:
        parser.error("--minutes: 0.5 to 240; --every: 2 to 60")
    if not 0 <= args.journal_from <= 0xFFFFFFFF or (
            args.period is not None and not 1 <= args.period <= 600):
        parser.error("--from: 0 to 2^32 - 1; --period: 1 to 600")
    if "name" in args.scenarios:
        ok = args.name is not None and 1 <= len(args.name) <= NAME_MAX and \
            args.name == args.name.strip() and all(0x20 <= ord(c) <= 0x7e for c in args.name)
        if not ok:
            parser.error("--name: 1 to 11 characters of printable ASCII, no space at either end")
    if not 1 <= args.take_s <= 60 or not 0 <= args.window <= 60:
        parser.error("--take-s: 1 to 60; --window: 0 to 60")
    if args.light in LIGHTS:
        light_choice = LIGHTS[args.light]
    else:
        try:
            light_choice = [int(v, 0) for v in args.light.split(",")]
        except ValueError:
            light_choice = []
        if len(light_choice) != 4 or not all(0 <= v <= 255 for v in light_choice):
            parser.error("--light: white, breathe, blink, rainbow, or four numbers 0 to 255")
    if args.self_test:
        return 1 if self_test(args.vectors) else 0
    if args.decode:
        return decode_files(args.decode, args.whisper_model if args.whisper else None)
    if not args.address or not args.scenarios:
        parser.error("--self-test, or --address and scenarios ("
                     + ", ".join(SCENARIOS + EXTRA_SCENARIOS) + ", all)")
    import asyncio
    import datetime
    names = SCENARIOS if "all" in args.scenarios else tuple(args.scenarios)
    logs = pathlib.Path(__file__).resolve().parents[1] / "logs"
    logs.mkdir(exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    with open(logs / f"cobaltlink-{stamp}.txt", "w", encoding="utf-8") as log_file:
        def log(text):
            print(text, flush=True)
            log_file.write(text + "\n")
            log_file.flush()
        global ntp_offset_s, network_time, send_time
        send_time = not args.no_time
        ntp = None
        if send_time:
            try:
                from lfxo_drift import network_offset
                ntp = network_offset(samples=4)
            except (ImportError, OSError):
                ntp = None
        if not send_time:
            log("no TIME in these sessions (--no-time): the watch's clock runs free")
        elif ntp:
            ntp_offset_s, network_time = ntp[0], True
            log(f"network time ({ntp[2]}) is the PC clock {ntp[0] * 1000:+.0f} ms: TIME "
                f"carries network time")
        else:
            log("no time server reached: TIME carries the PC clock, which skews the watch's "
                "calibration of its crystal")
        failures = asyncio.run(run_sessions(args.address, names, log))
        log(f"{len(names) - failures} of {len(names)} scenario(s) passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
