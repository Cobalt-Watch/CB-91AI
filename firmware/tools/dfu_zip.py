#!/usr/bin/env python3
"""Package the firmware for an update from a phone: one ZIP, both slot variants.

MCUboot executes the images in place (direct execution with revert): every build
comes in two variants, one linked for each slot, and an update must carry the
variant of the slot that the running image does not occupy. ble_update.py makes
that choice itself. Nordic's MCUmgr libraries for Android and iOS, and nRF Connect
Device Manager built on them, make it only when they get a ZIP whose
manifest.json gives the slot of each file, as in the dfu_application.zip of the
nRF Connect SDK. Given a lone .bin, they take it for a slot 1 image; when slot 1
holds the running, confirmed image, they skip the upload without a word and
report it complete (B4, 2026-09-20: the board ran from slot 1).

    python firmware/tools/dfu_zip.py                       # the local build, as it is
    python firmware/tools/dfu_zip.py --version 0.1.43+1    # the local build signed again, higher version
    python firmware/tools/dfu_zip.py a.signed.bin b.signed.bin -o update.zip

The ZIP is written to firmware/logs/phone/cb91ai-<version>.zip unless -o says
otherwise. On the phone: nRF Connect Device Manager, Image tab, select the ZIP
(the app lists one image per slot), mode "Test and confirm", start. The version
rule is the one of ble_update.py: higher than the running image, build number
included. --version signs with the command of the build itself, like
ble_update.py --next-build, and keeps the signed images in firmware/logs/series/
so that both tools send the very same files.

The manifest follows the one of the nRF Connect SDK, numbers written as strings
where the SDK writes strings: the iOS library reads "slot" and "image_index" only
as strings (an integer "slot" makes it take every file for slot 1), and requires
"format-version", "time", "modtime", "board" and "soc"; the Android library
takes either form.

Requirements: those of ble_update.py (`pip install smpclient bleak`), whose
image helpers this tool shares.
"""

import argparse
import json
import pathlib
import sys
import time
import zipfile

from ble_update import LOG_DIR, local_build, sign_again, version_text, version_tuple
from smpclient.mcuboot import ImageInfo

BOARD = "cb91ai"
SOC = "nRF52840_xxAA"


def header(path):
    """(load address, version) of a signed image, from its MCUboot header."""
    info = ImageInfo.load_file(str(path)).header
    return info.load_addr, (info.ver.major, info.ver.minor, info.ver.revision, info.ver.build_num)


def manifest_entry(slot, load_addr, version, name, size, now):
    return {
        "type": "application",
        "board": BOARD,
        "soc": SOC,
        "load_address": load_addr,
        "image_index": "0",
        "slot": str(slot),
        "version_MCUBOOT": version_text(version),
        "size": size,
        "file": name,
        "modtime": now,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", nargs="*", type=pathlib.Path,
                        help="signed images, one per slot (default: the local build)")
    parser.add_argument("--version", help="sign the local build again under this version, e.g. 0.1.43+1")
    parser.add_argument("-o", "--output", type=pathlib.Path, help="ZIP to write")
    args = parser.parse_args()

    if args.version and args.image:
        parser.error("--version signs the local build: no image argument")
    if args.version:
        images = [sign_again(slot, version_tuple(args.version)) for slot in (0, 1)]
    else:
        images = args.image or local_build()
    if len(images) != 2:
        parser.error(f"one image per slot is needed, {len(images)} given: build with sysbuild first")

    # The variant linked at the lower address is the one of slot 0, as in ble_update.py.
    variants = sorted((header(path), path) for path in images)
    (load0, version), (load1, version1) = variants[0][0], variants[1][0]
    if version != version1:
        print(f"the two images differ in version: {version_text(version)} and {version_text(version1)}")
        return 2
    if load0 == load1:
        print(f"both images are linked for the same address ({load0:#x}): one variant per slot is needed")
        return 2

    now = int(time.time())
    files = []
    output = args.output or LOG_DIR / "phone" / f"{BOARD}-{version_text(version)}.zip"
    output.parent.mkdir(parents=True, exist_ok=True)
    # Flat archive: the Android library refuses a ZIP with directory entries.
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for slot, ((load_addr, _), path) in enumerate(variants):
            name = f"{BOARD}-slot{slot}.bin"
            data = path.read_bytes()
            archive.writestr(name, data)
            files.append(manifest_entry(slot, load_addr, version, name, len(data), now))
            print(f"slot {slot}: {path} ({len(data)} bytes, linked at {load_addr:#x})")
        manifest = {"format-version": 1, "time": now, "name": f"{BOARD}-{version_text(version)}",
                    "files": files}
        archive.writestr("manifest.json", json.dumps(manifest, indent=2))
    print(f"version {version_text(version)} -> {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
