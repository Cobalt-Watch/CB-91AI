#!/usr/bin/env python3
"""Update the CB-91AI firmware over Bluetooth LE, in one command.

This is the normal way to load a firmware: the board in the watch has no other
port, and the bench board takes the same path so that it stays proven. SWD
(bringup.py) is the fallback, and the only way to change MCUboot itself.

    python firmware/tools/ble_update.py                 # the local build, variant of the free slot
    python firmware/tools/ble_update.py image.bin --address AA:BB:CC:DD:EE:FF
    python firmware/tools/ble_update.py --status        # image slots and status line, no update
    python firmware/tools/ble_update.py --next-build    # the local build again, build number + 1
    python firmware/tools/ble_update.py --loop 10       # endurance series (EV-08)
    python firmware/tools/ble_update.py --cut-upload 50 # EV-08: drop the link mid-upload

MCUboot executes the images in place (direct execution with revert): an update
goes to the slot the running image does not occupy, and sysbuild builds one
variant of the image per slot (build/app and build/app_slot1_variant). The tool
reads which slot is free and sends the variant linked for it; the firmware
refuses the other one. MCUboot then starts the image with the higher version:
**an update must carry a higher version than the running one**, build number
(VERSION_TWEAK) included, and the tool says so before it sends anything.

Sequence (MCUmgr SMP over the BLE transport), on two connections: read the
slots, upload to the free one, mark the image for test, reset; then, once the
new image runs, check it, confirm it and read the status line. About 25 s once
connected. A step that fails is tried again, with those that follow, on a fresh
connection. An image that does not come back, or that nobody confirms, is given
up by MCUboot at the next reset, and the previous one runs again.

--next-build signs the local build again with the build number of the running
image plus one: the way to reload the same sources, since MCUboot would not
start an equal version. --loop N chains N such updates. Before each one the
board is left alone until it is back at rest (display off, slow main loop) and
the status line is read; it is read again right after any failed upload, where
`up=` and `rst=` tell a reset (watchdog: rst=10) from a lost link. A table of
retries, connection and erase times, throughput and duration closes the series.
Signing takes the command of the build itself, from build.ninja, with its key.

--cut-upload PERCENT drops the link once that share of the image is sent, shows
the slots and stops: the running image must not care. --no-confirm then --reset
shows the way back: an image left in test is given up at the next reset.

An interrupted upload is never resumed. On 2026-09-23 the two uploads resumed
after chunks had gone unanswered gave images MCUboot refused, and every upload
started from zero passed. After a failed attempt, or when the status line says
`sp=k` (an upload of an earlier run left half done), the tool erases the free
slot over SMP, which also resets the upload state of the board, and sends the
whole image again. A chunk may wait 20 s for its answer (CHUNK_TIMEOUT_S), and
the traceback of each failure goes to the log.

Requirements: `pip install smpclient bleak` (smpmgr brings both).
The output is also written, line by line, to firmware/logs/bleupdate-<date>.txt.
"""

import argparse
import asyncio
import datetime
import pathlib
import re
import shlex
import subprocess
import sys
import time
import traceback

from bleak import BleakClient, BleakScanner
from smpclient import SMPClient
from smpclient.exceptions import SMPUploadError
from smpclient.generics import error
from smpclient.mcuboot import IMAGE_TLV, ImageInfo
from smpclient.requests.image_management import ImageErase, ImageStatesRead, ImageStatesWrite
from smpclient.requests.os_management import ResetWrite, TaskStatisticsRead
from smpclient.transport import ble as smp_ble
from smpclient.transport.ble import SMPBLETransport

FIRMWARE_DIR = pathlib.Path(__file__).resolve().parents[1]
LOG_DIR = FIRMWARE_DIR / "logs"
BUILD_DIR = FIRMWARE_DIR / "build"
VARIANTS = ("app", "app_slot1_variant")  # sysbuild images linked for slot 0 and for slot 1
SMP_SERVICE = "8d53dc1d-1db7-4cd3-868b-8a527460aa84"
DEBUG_SERVICE = "c0b91a00-1db7-4cd3-868b-8a527460db61"
DEBUG_CHARACTERISTIC = "c0b91a01-1db7-4cd3-868b-8a527460db61"

REQUEST_TIMEOUT_S = 6.0
CHUNK_TIMEOUT_S = 20.0
ERASE_TIMEOUT_S = 90.0  # 114 pages of 85 ms, two to three times longer during a connection
CONNECT_TIMEOUT_S = 90.0  # slow advertising (1 s) and a PC that hears one event in twenty
ATTEMPTS = 5
REBOOT_WAIT_S = 4  # MCUboot checks the image, then the connection attempt waits for the advertiser
BACK_TIMEOUT_S = 150
IDLE_WAIT_S = 45  # the display goes off 30 s after boot, the main loop then slows down

STEPS = ("state", "upload", "mark", "reset", "check")
STOP = object()  # returned by a step that ends its sequence early

_log_file = None


class UploadCut(Exception):
    """The upload was dropped on purpose (--cut-upload)."""


class Refused(Exception):
    """The board answered no: trying again would not change its mind."""


def out(text=""):
    print(text, flush=True)
    if _log_file:
        _log_file.write(text + "\n")
        _log_file.flush()


class UpdateStats:
    """What one update cost: failed attempts per step, and where the time went."""

    def __init__(self):
        self.label = "?"
        self.code = None
        self.retries = dict.fromkeys(STEPS, 0)
        self.connect_s = 0.0  # spent getting connected: slow advertising, a PC that listens little
        self.erase_s = None   # answer to the first chunk: the slot erase, when it was not done ahead
        self.kbps = None      # after the erase
        self.back_s = None    # from the reset to the new image checked
        self.total_s = None

    @property
    def retry_count(self):
        return sum(self.retries.values())


# ---- Images -------------------------------------------------------------------


def version_tuple(text):
    """(major, minor, revision, build) of "0.1.31", "0.1.31.4" or "0.1.31+4"."""
    numbers = [int(n) for n in re.findall(r"\d+", str(text))]
    return tuple((numbers + [0, 0, 0, 0])[:4])


def version_text(version):
    return f"{version[0]}.{version[1]}.{version[2]}+{version[3]}"


def image_version(path):
    ver = ImageInfo.load_file(str(path)).header.ver
    return (ver.major, ver.minor, ver.revision, ver.build_num)


def local_build():
    """Signed images of the local build: one per slot, or a single one without variant."""
    images = [BUILD_DIR / variant / "zephyr" / "zephyr.signed.bin" for variant in VARIANTS]
    return [image for image in images if image.is_file()]


def from_files(paths):
    """Provider of the image to send: among `paths`, the variant linked for the free slot."""
    def provide(free_slot, running):
        if len(paths) == 1:
            return paths[0]
        by_address = sorted(paths, key=lambda p: ImageInfo.load_file(str(p)).header.load_addr)
        return by_address[free_slot]
    return provide


def sign_again(slot, version):
    """The local build of the variant of `slot`, signed as `version` by the command of the build."""
    ninja = (BUILD_DIR / VARIANTS[slot] / "build.ninja").read_text(encoding="utf-8", errors="replace")
    found = re.search(r"(\S*imgtool\.py) sign (.+?) (\S+zephyr\.bin) \S+zephyr\.signed\.bin", ninja)
    if not found:
        raise RuntimeError(f"no imgtool command in the build of {VARIANTS[slot]}: build first")
    imgtool, options, unsigned = found.group(1), shlex.split(found.group(2)), found.group(3)
    options[options.index("--version") + 1] = version_text(version)
    target = LOG_DIR / "series" / f"cb91ai-{version_text(version)}-slot{slot}.signed.bin"
    target.parent.mkdir(parents=True, exist_ok=True)
    # The signature is random (RSA-PSS): signing twice gives two files, and an upload
    # cut half way only resumes with the very same file. Keep the one already made
    # from this build.
    if not (target.is_file() and target.stat().st_mtime > pathlib.Path(unsigned).stat().st_mtime):
        subprocess.run([sys.executable, imgtool, "sign", *options, unsigned, str(target)],
                       check=True, capture_output=True)
    return target


def next_build(free_slot, running):
    """Provider: the local build signed with the build number of the running image plus one."""
    return sign_again(free_slot, (*running[:3], running[3] + 1))


# ---- Link ---------------------------------------------------------------------


async def find_address(name, seconds=10.0):
    """First device advertising the SMP service, or the given name."""
    found = {}

    def on_advertisement(device, adv):
        uuids = [u.lower() for u in (adv.service_uuids or [])]
        if SMP_SERVICE in uuids or (name and (adv.local_name or device.name) == name):
            found.setdefault(device.address, (adv.local_name or device.name, adv.rssi))

    async with BleakScanner(on_advertisement):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and not found:
            await asyncio.sleep(0.25)
    if not found:
        return None
    address, (label, rssi) = next(iter(found.items()))
    out(f"found {label or '?'} at {address}, RSSI {rssi} dBm")
    return address


class StatusTransport(SMPBLETransport):
    """The SMP transport, with the debug service discovered as well.

    smpclient narrows the service discovery to SMP, and the status line lives in another
    service: without this, reading it costs a connection of its own, 5 s from this PC.
    """

    async def _connect(self, address, timeout_s):
        def with_debug_service(device, services=(), **kwargs):
            return BleakClient(device, services=(*services, DEBUG_SERVICE), **kwargs)

        smp_ble.BleakClient = with_debug_service
        try:
            await super()._connect(address, timeout_s)
        finally:
            smp_ble.BleakClient = BleakClient


async def run_steps(address, steps, stats=None, attempts=ATTEMPTS, on_failure=None):
    """Run the steps, in order, on one connection: False if one of them said STOP.

    Each step is (name for the messages, key of the statistics, coroutine taking the
    client). A step that fails is tried again, followed by the remaining ones, on a fresh
    connection: a connection costs 3 to 6 s from this PC, the commands a few tens of ms.
    """
    todo = list(steps)
    failures = 0
    while todo:
        what, step, _ = todo[0]
        client = SMPClient(StatusTransport(), address, timeout_s=REQUEST_TIMEOUT_S)
        try:
            connecting = time.monotonic()
            await client.connect(CONNECT_TIMEOUT_S)
            if stats:
                stats.connect_s += time.monotonic() - connecting
            try:
                while todo:
                    what, step, action = todo[0]
                    if await action(client) is STOP:
                        return False
                    todo.pop(0)
            finally:
                try:
                    await asyncio.wait_for(client.disconnect(), 10)
                except Exception:  # noqa: BLE001 - the link may already be gone
                    pass
        except (UploadCut, Refused):
            raise
        except Exception as exc:  # noqa: BLE001 - every failure means "try again"
            failures += 1
            if _log_file:
                # Where it failed, in the log only: a library error can hide a lost link
                _log_file.write(traceback.format_exc())
                _log_file.flush()
            if stats:
                stats.retries[step] += 1
            out(f"  {what}: attempt {failures}/{attempts} failed ({type(exc).__name__}: {exc})")
            if failures >= attempts:
                raise RuntimeError(f"{what} failed after {attempts} attempts: {exc}") from exc
            if on_failure and step in on_failure:
                await on_failure[step]()
            await asyncio.sleep(2.0)
    return True


async def read_states(client):
    response = await client.request(ImageStatesRead())
    if error(response):
        raise RuntimeError(f"image state read refused: {response}")
    return response.images


async def read_status(client, address):
    """The status line on the connection of `client`, or else on one of its own."""
    try:
        value = await client._transport._client.read_gatt_char(DEBUG_CHARACTERISTIC)
        return bytes(value).decode(errors="replace")
    except Exception:  # noqa: BLE001 - smpclient internals moved, or the service was not found
        return await read_status_line(address)


async def read_status_line(address, attempts=3):
    """The debug characteristic of the self-test; reading it also confirms a test image.

    On a connection of its own, for when no SMP connection is at hand.
    """
    last = None
    for _ in range(attempts):
        try:
            device = await BleakScanner.find_device_by_address(address, timeout=CONNECT_TIMEOUT_S)
            if device is None:
                raise RuntimeError("not seen advertising")
            async with BleakClient(device, timeout=CONNECT_TIMEOUT_S) as bleak_client:
                value = await bleak_client.read_gatt_char(DEBUG_CHARACTERISTIC)
                return bytes(value).decode(errors="replace")
        except Exception as exc:  # noqa: BLE001 - diagnostic only
            last = exc
            await asyncio.sleep(2.0)
    return f"(status line not read: {type(last).__name__}: {last})"


def describe(images):
    for image in images:
        flags = [name for name in ("active", "confirmed", "pending", "permanent", "bootable")
                 if getattr(image, name, False)]
        out(f"  slot {image.slot}: {image.version}  {bytes(image.hash).hex()[:16]}...  "
            f"{' '.join(flags)}")


async def show_tasks(client):
    """The stack of each thread, used and size, where the image answers taskstat
    (the watch application in debug.conf): a sealed watch gives no other way to
    measure it."""
    response = await client.request(TaskStatisticsRead())
    if error(response):
        out(f"  tasks: not answered ({response}): an image without taskstat")
        return
    out("  thread           stack used / size, in bytes")
    for name, task in sorted(response.tasks.items(), key=lambda item: item[1].prio):
        if task.stksiz:
            used, size = task.stkuse * 4, task.stksiz * 4  # Zephyr counts in words
            out(f"  {name:<16} {used:5} / {size:5}  {100 * used // size:3} %   prio {task.prio}")
        else:
            out(f"  {name:<16} no stack information   prio {task.prio}")


async def reset_board(client):
    try:
        await client.request(ResetWrite(), timeout_s=4.0)
    except (TimeoutError, asyncio.TimeoutError):
        pass  # the board may reset before its answer gets through


# ---- One update -----------------------------------------------------------------


async def update(address, provide, args, stats):
    """One update with the image `provide` picks; the exit code, the cost left in `stats`.

    Two connections in all: slots, upload, mark and reset on the first one, then, once
    the new image runs, check, confirmation and status line on the second.
    """
    started = time.monotonic()
    # The image the previous update left in the free slot may be this one: in a series,
    # or when asked, send it anyway, the erase and the upload are what is being tried
    always_upload = args.loop or args.force_upload or args.cut_upload is not None
    found = {}

    async def slots(client):
        if args.loop and "before" not in found:
            found["before"] = await read_status(client, address)
            out(f"  status before: {found['before']}")
        out("Current slots:")
        images = await read_states(client)
        describe(images)
        active = next((i for i in images if getattr(i, "active", False)), images[0])
        running = version_tuple(active.version)
        if "image" not in found:  # a retry sends the same image again
            image = provide(1 - active.slot, running)
            info = ImageInfo.load_file(str(image))
            found.update(image=image, info=info, data=image.read_bytes(),
                         hash=info.get_tlv(IMAGE_TLV.SHA256).value)
            stats.label = image.name
        info = found["info"]
        out(f"image {found['image'].name}: version {info.header.ver}, {len(found['data'])} bytes, "
            f"linked for {info.header.load_addr:#x}, hash {found['hash'].hex()[:16]}...")
        if bytes(active.hash) == found["hash"]:
            out("this image is already running: nothing to do")
            return STOP
        if image_version(found["image"]) <= running:
            out(f"version {version_text(image_version(found['image']))} is not higher than the "
                f"running {version_text(running)}: MCUboot would not start it. Raise VERSION "
                "(VERSION_TWEAK is enough) and build again, or use --next-build")
            found["code"] = 5
            return STOP
        found["in_free_slot"] = any(bytes(i.hash) == found["hash"] for i in images)
        found["free_slot"] = 1 - active.slot
        # `sp=k` in the status line: an upload of an earlier run was left half done
        if "sp=k" in (await read_status(client, address)).split():
            found["dirty"] = True

    async def erase_free_slot(client):
        """Erase the free slot, which also resets the upload state of the board.

        An upload is never resumed after an interruption: on 2026-09-23 the two uploads
        resumed after chunks had gone unanswered gave images MCUboot refused, and the
        clean one passed. The image manager of this firmware does not check the hash of
        what it received, so a bad image only shows at the reset.
        """
        out(f"  erasing slot {found['free_slot']} first: an interrupted upload starts over")
        response = await client.request(ImageErase(slot=found["free_slot"]),
                                        timeout_s=ERASE_TIMEOUT_S)
        if error(response):
            raise RuntimeError(f"slot erase refused: {response}")

    async def upload(client):
        if found.get("in_free_slot") and not always_upload and not found.get("dirty"):
            out("this image is already in the free slot: upload skipped")
            return
        if found.get("dirty"):
            await erase_free_slot(client)
            found["dirty"] = False
        data = found["data"]
        found["dirty"] = True  # until the last chunk is acknowledged
        out("Upload to the free slot:")
        start = time.monotonic()
        first = None  # (time, offset) of the first answer, which waits for the slot erase
        last_percent = -10
        try:
            # A chunk can wait seconds for its answer on a weak link, the board writing
            # flash between radio events: 20 s rather than the 6 s of the other requests
            # (2026-09-23, the watch worn at the desk: stalls over 6 s, several per upload)
            async for offset in client.upload(data, first_timeout_s=40.0,
                                              subsequent_timeout_s=CHUNK_TIMEOUT_S):
                if first is None:
                    first = (time.monotonic(), offset)
                percent = offset * 100 // len(data)
                if args.cut_upload is not None and percent >= args.cut_upload:
                    raise UploadCut(f"link dropped on purpose at {percent} % ({offset} bytes)")
                if percent >= last_percent + 10:
                    last_percent = percent
                    # Rate of this connection: a resumed upload does not start at zero
                    out(f"  {percent:3d} %  {offset / 1000:7.1f} kB  "
                        f"{(offset - first[1]) / 1000 / max(time.monotonic() - start, 0.1):5.1f} kB/s")
        except SMPUploadError as exc:
            reason = getattr(getattr(exc.args[0] if exc.args else None, "err", None), "rc", None)
            if reason is None:
                # A refusal from an MCUmgr hook comes back as a legacy `rc` in an
                # answer without offset, which smpclient only reports as text
                legacy = re.search(r"off=None, .*rc=(\d+)\)$", str(exc))
                if legacy and int(legacy.group(1)) != 0:
                    rc = int(legacy.group(1))
                    hint = (": up to 0.1.43 the self-test refuses an update on a cell below "
                            "2.8 V (EF-63), use a fresh cell or USB" if rc == 6 else "")
                    raise Refused(f"the board refused the image (rc={rc}){hint}") from exc
                raise  # not an answer of the board: a failure like any other
            hint = (": this image is linked for the other slot"
                    if getattr(reason, "name", "") == "INVALID_FLASH_ADDRESS" else "")
            raise Refused(f"the board refused the image ({getattr(reason, 'name', reason)}){hint}")
        found["dirty"] = False
        seconds = time.monotonic() - start
        stats.erase_s = first[0] - start
        stats.kbps = (len(data) - first[1]) / 1000 / max(time.monotonic() - first[0], 0.1)
        out(f"  uploaded in {seconds:.1f} s: {stats.erase_s:.1f} s for the first chunk "
            f"(slot erase), then {stats.kbps:.1f} kB/s")

    async def after_failed_upload():
        # A reset shows as a short `up=` with its cause in `rst=` (10: watchdog)
        out(f"  status after the failure: {await read_status_line(address)}")

    async def mark(client):
        response = await client.request(ImageStatesWrite(hash=found["hash"], confirm=False))
        if error(response):
            raise RuntimeError(f"mark for test refused: {response}")
        if not any(bytes(i.hash) == found["hash"] and i.pending for i in response.images):
            raise RuntimeError("image not pending after the state write")
        out("Marked for test: MCUboot starts it at the next reset")
        if args.no_reset:
            return STOP

    async def reset(client):
        out("Reset (the glass shows UPd until the new image starts)")
        await reset_board(client)
        found["reset_at"] = time.monotonic()

    try:
        complete = await run_steps(address, [("image state read", "state", slots),
                                             ("upload", "upload", upload),
                                             ("mark for test", "mark", mark),
                                             ("reset", "reset", reset)],
                                   stats, on_failure={"upload": after_failed_upload})
    except Refused as refusal:
        out(f"  {refusal}")
        return 6
    except UploadCut as cut:
        # EV-08: the running image must not care, and the next update must go through
        out(f"  {cut}")
        out("Slots after the cut:")

        async def after_cut(client):
            describe(await read_states(client))
            out(f"  status: {await read_status(client, address)}")

        await run_steps(address, [("image state read", "state", after_cut)], stats)
        return 0
    if not complete:
        return found.get("code", 0)

    out("Waiting for MCUboot to check the image and start it...")
    await asyncio.sleep(REBOOT_WAIT_S)

    async def check(client):
        images = await read_states(client)
        describe(images)
        running = next((i for i in images if getattr(i, "active", False)), images[0])
        found["ok"] = bytes(running.hash) == found["hash"]
        if found["ok"] and not args.no_confirm:
            if not running.confirmed:
                response = await client.request(ImageStatesWrite(hash=None, confirm=True))
                if error(response):
                    raise RuntimeError(f"confirm refused: {response}")
                out("  image confirmed")
            out(f"  status: {await read_status(client, address)}")

    deadline = time.monotonic() + BACK_TIMEOUT_S
    while True:
        try:
            await run_steps(address, [("check after the reset", "check", check)], stats, attempts=2)
            break
        except RuntimeError as exc:
            if time.monotonic() > deadline:
                out(f"the board did not come back in {BACK_TIMEOUT_S} s: {exc}")
                return 2
            await asyncio.sleep(3.0)
    stats.back_s = time.monotonic() - found["reset_at"]
    if not found["ok"]:
        out("the previous image is running: MCUboot refused the new one, or gave it up")
        return 3
    stats.total_s = time.monotonic() - started
    out(f"update done in {stats.total_s:.0f} s, {stats.connect_s:.0f} s of them getting connected: "
        f"{found['info'].header.ver} is running"
        + (", in test: confirm it with --confirm once tried (the self-test confirms itself "
           "after 2 min; the watch application never does, and reboots to the previous image "
           "after 10 min)" if args.no_confirm else " and confirmed"))
    return 0


def summarize(series):
    def cell(value, fmt):
        return format(value, fmt) if value is not None else "-".rjust(len(format(0, fmt)))

    out("")
    out("  #  image                                 result  " + "  ".join(f"{s:>6}" for s in STEPS)
        + "  conn s  erase s   kB/s  back s  total s")
    for number, stats in enumerate(series, 1):
        out(f" {number:2d}  {stats.label[:36]:36s}  "
            f"{'ok' if stats.code == 0 else f'code {stats.code}':>6}  "
            + "  ".join(f"{stats.retries[s]:6d}" for s in STEPS)
            + f"  {stats.connect_s:6.1f}  {cell(stats.erase_s, '7.1f')}  {cell(stats.kbps, '5.1f')}"
              f"  {cell(stats.back_s, '6.1f')}  {cell(stats.total_s, '7.1f')}")
    done = [s for s in series if s.code == 0]
    clean = [s for s in done if s.retry_count == 0]
    out(f"{len(done)}/{len(series)} updates done, {len(clean)} without any retry; failed attempts: "
        + ", ".join(f"{step} {sum(s.retries[step] for s in series)}" for step in STEPS))


async def run(args):
    address = args.address or await find_address(args.name)
    if not address:
        out("no CB-91AI found: is it powered, in range, and not connected elsewhere? "
            "A button press brings fast advertising back for 30 s.")
        return 1

    if args.status or args.reset or args.confirm or args.tasks:
        async def status(client):
            images = await read_states(client)
            describe(images)
            if args.tasks:
                await show_tasks(client)
            if args.confirm:
                # The running image, over SMP: the one confirmation the watch
                # application accepts (it never confirms itself, watch/src/update.c)
                running = next((i for i in images if getattr(i, "active", False)), None)
                if running is None:
                    raise RuntimeError("the board names no running image: nothing confirmed")
                if running.confirmed:
                    out("  the running image is confirmed already")
                else:
                    response = await client.request(ImageStatesWrite(hash=None, confirm=True))
                    if error(response):
                        raise RuntimeError(f"confirm refused: {response}")
                    out("  running image confirmed")
                    describe(await read_states(client))
            if args.reset:
                # Not the status line: reading it confirms an image left in test
                out("Reset")
                await reset_board(client)
            else:
                out(f"  status: {await read_status(client, address)}")

        out(f"Image slots of {address}:")
        await run_steps(address, [("status", "state", status)])
        return 0

    provide = next_build if (args.loop or args.next_build) else from_files(args.image)
    if not args.loop:
        return await update(address, provide, args, UpdateStats())

    series = []
    code = 0
    try:
        for number in range(1, args.loop + 1):
            out("")
            out(f"=== update {number}/{args.loop}, {datetime.datetime.now():%H:%M:%S} ===")
            if args.idle_wait:
                out(f"Leaving the board alone for {args.idle_wait} s, until it is at rest...")
                await asyncio.sleep(args.idle_wait)
            stats = UpdateStats()
            series.append(stats)
            stats.code = code = await update(address, provide, args, stats)
            if code:
                break
    finally:
        summarize(series)
    return code


def main():
    global _log_file

    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("image", nargs="*", type=pathlib.Path,
                        help="signed MCUboot image (.bin), or the two variants of one build: the "
                             "one linked for the free slot is sent; default: the local build")
    parser.add_argument("--address", help="Bluetooth address; default: first board advertising SMP")
    parser.add_argument("--name", default="CB-91AI", help="advertised name accepted by the scan")
    parser.add_argument("--status", action="store_true", help="only show the slots and the status line")
    parser.add_argument("--reset", action="store_true",
                        help="show the slots, then reset the board")
    parser.add_argument("--tasks", action="store_true",
                        help="show the slots and the stack used by each thread (taskstat, "
                             "watch application in debug.conf)")
    parser.add_argument("--confirm", action="store_true",
                        help="confirm the running image over SMP, after an update with "
                             "--no-confirm, once it has been tried")
    parser.add_argument("--next-build", action="store_true",
                        help="sign the local build again with the build number of the running "
                             "image plus one, and send that")
    parser.add_argument("--no-confirm", action="store_true",
                        help="leave the new image in test, to try it before --confirm: the "
                             "self-test confirms itself after 2 min, the watch application "
                             "never, and reboots to the previous image after 10 min (EF-62)")
    parser.add_argument("--no-reset", action="store_true", help="stop after marking the image")
    parser.add_argument("--force-upload", action="store_true",
                        help="send the image even when the free slot already holds it")
    parser.add_argument("--cut-upload", type=int, metavar="PERCENT",
                        help="EV-08: drop the link once PERCENT of the image is sent, show the "
                             "slots and stop; the next update then resumes the upload")
    parser.add_argument("--loop", type=int, metavar="N",
                        help="chain N updates of the local build, signed with growing build numbers")
    parser.add_argument("--idle-wait", type=int, default=IDLE_WAIT_S, metavar="S",
                        help="with --loop: seconds left to the board before each update, so that "
                             f"it is back at rest (default {IDLE_WAIT_S})")
    args = parser.parse_args()

    if args.loop is not None and (args.loop < 1 or args.image or args.no_reset or args.no_confirm
                                  or args.cut_upload is not None):
        parser.error("--loop takes a count of at least 1 and the local build, "
                     "without --no-reset, --no-confirm or --cut-upload")
    if args.next_build and args.image:
        parser.error("--next-build signs the local build: no image argument")
    if len(args.image) > 2:
        parser.error("one image, or the two variants of one build")
    if not (args.status or args.reset or args.confirm or args.tasks):
        args.image = args.image or local_build()
        if not args.image:
            parser.error(f"no local build in {BUILD_DIR}: build first, or name an image")
        for image in args.image:
            if not image.is_file():
                parser.error(f"image not found: {image}")

    LOG_DIR.mkdir(exist_ok=True)
    log = LOG_DIR / f"bleupdate-{datetime.datetime.now():%Y%m%d-%H%M%S}.txt"
    with log.open("w", encoding="utf-8") as _log_file:
        try:
            code = asyncio.run(run(args))
        except (RuntimeError, subprocess.CalledProcessError, KeyboardInterrupt) as exc:
            out(f"stopped: {exc}")
            code = 1
    _log_file = None
    print(f"log saved to {log}")
    return code


if __name__ == "__main__":
    sys.exit(main())
