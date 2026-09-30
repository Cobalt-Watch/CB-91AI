#!/usr/bin/env python3
"""Prepare a watch update for the website, where the Cobalt app looks for it (lot N3).

The app reads https://cobalt-watch.com/firmware/manifest.json at most once a day, compares the
version of its channel with the watch's (the first word of the watch's status line, with its
build number since 0.2.1+31), and offers the update: it downloads the ZIP, checks its size and
SHA-256, and sends it at the watch's next session like any other update (test, then confirm).
MCUboot refuses any image the project's key did not sign and any lower version: a hijacked
website could only withhold updates.

On the bench PC, once an image has been tried and confirmed on a watch:

    python firmware/tools/publish_update.py firmware/buildw --channel dev --notes "..." --release
    python firmware/tools/publish_update.py firmware/buildr --channel produit --notes "..." --release

Each run builds the ZIP of both slot variants (dfu_zip.py) of a sysbuild build directory and
writes into firmware/logs/site/firmware (kept out of git) the ZIP, a manifest.json (the channel
updated, the others kept as they were in that folder) and a .htaccess that keeps browsers and
phones from caching the manifest. With --release, it also opens a draft release of
Cobalt-Watch/CB-91AI, tagged watch-<channel>-<version>, the ZIP attached: publishing that
draft on GitHub is the maintainers' approval, and the "Site" workflow (.github/workflows/site.yml) then
puts the ZIP and the manifest on the website over SFTP. The images are signed here, with the
project's key, which never leaves this PC: nothing is built in the CI.

In the CI, from a release's ZIP (the workflow's own use):

    python firmware/tools/publish_update.py --zip release/cb91ai-watch-dev-0.2.1+33.zip \
        --channel dev --notes-file notes.txt --manifest-url https://cobalt-watch.com/firmware/manifest.json -o out

checks both variants (same version, two addresses, the project's key), reads the manifest the
website serves, updates the channel and writes out/ (ZIP, manifest.json, .htaccess). It refuses
a version lower than the one the website offers on that channel, and another ZIP under the
same version.

Channels: "dev" for development images (debug.conf: the harness's keys, SMP without a bond),
"produit" for release images. A development watch takes the dev channel, a product the other.
No password ever goes through this tool nor the repository: the website's key lives in the
repository's Actions secrets, set by the maintainers.
"""

import argparse
import datetime
import hashlib
import io
import json
import pathlib
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile

FIRMWARE_DIR = pathlib.Path(__file__).resolve().parents[1]
TOOLS = pathlib.Path(__file__).resolve().parent
DEFAULT_OUT = FIRMWARE_DIR / "logs" / "site" / "firmware"
BASE_URL = "https://cobalt-watch.com/firmware/"
REPO = "Cobalt-Watch/CB-91AI"

HTACCESS = """# The Cobalt app reads manifest.json once a day: never from a cache
<Files "manifest.json">
  Header set Cache-Control "no-cache, must-revalidate"
</Files>
AddType application/json .json
AddType application/zip .zip
"""


# SHA-256 of the project's public signing key (imgtool getpubhash):
# a public value, what MCUboot on the watches compares each image's KEYHASH with
PROJECT_KEY_HASH = "84b8710c7222ea548c5642ff698bd7faa42d585e27384a2e47c534d7061d71f1"

IMAGE_MAGIC = 0x96F3B83D
TLV_INFO_MAGIC = 0x6907
TLV_PROT_INFO_MAGIC = 0x6908
TLV_KEYHASH = 0x01


def key_hash(data):
    """The KEYHASH TLV of a signed MCUboot image (bytes or a path): the hash of the key that signed it, or None"""
    if isinstance(data, pathlib.Path):
        data = data.read_bytes()
    header_size = struct.unpack_from("<H", data, 8)[0]
    image_size = struct.unpack_from("<I", data, 12)[0]
    at = header_size + image_size
    magic, length = struct.unpack_from("<HH", data, at)
    if magic == TLV_PROT_INFO_MAGIC:
        at += length
        magic, length = struct.unpack_from("<HH", data, at)
    if magic != TLV_INFO_MAGIC:
        return None
    end, at = at + length, at + 4
    while at + 4 <= end:
        kind, size = struct.unpack_from("<HH", data, at)
        if kind == TLV_KEYHASH:
            return data[at + 4:at + 4 + size].hex()
        at += 4 + size
    return None


def image_facts(data):
    """(load address, "major.minor.revision+build") of a signed MCUboot image, from its header"""
    magic, load = struct.unpack_from("<II", data, 0)
    if magic != IMAGE_MAGIC:
        raise SystemExit("not an MCUboot image (magic)")
    major, minor, revision, build = struct.unpack_from("<BBHI", data, 20)
    return load, f"{major}.{minor}.{revision}+{build}"


def check_variants(variants):
    """The version of two signed variants, one per slot, after the checks that keep a wrong
    image off the website: same version, two addresses, the project's key"""
    if len(variants) != 2:
        raise SystemExit(f"{len(variants)} image(s): one per slot is needed")
    facts = [image_facts(data) for _, data in variants]
    if facts[0][1] != facts[1][1]:
        raise SystemExit(f"the two variants differ in version: {facts[0][1]} and {facts[1][1]}")
    if facts[0][0] == facts[1][0]:
        raise SystemExit(f"both variants are linked at {facts[0][0]:#x}: one per slot is needed")
    # Offered to every watch, an image signed with another key (MCUboot's development key,
    # CB91AI_SIGNING_KEY unset) would be sent and refused by each of them (review of 29/09)
    for name, data in variants:
        found = key_hash(data)
        if found != PROJECT_KEY_HASH:
            raise SystemExit(f"{name}: signed with another key ({found}), not the project's: "
                             "set CB91AI_SIGNING_KEY and build again")
    return facts[0][1]


def images(build):
    """The two signed variants of a sysbuild build: <app>/zephyr and <app>_slot1_variant/zephyr"""
    found = sorted(build.glob("*_slot1_variant/zephyr/zephyr.signed.bin"))
    if len(found) != 1:
        raise SystemExit(f"{build}: no slot 1 variant (a sysbuild build of the watch is needed)")
    slot1 = found[0]
    app = slot1.parents[1].name.removesuffix("_slot1_variant")
    slot0 = build / app / "zephyr" / "zephyr.signed.bin"
    if not slot0.is_file():
        raise SystemExit(f"{build}: no {slot0.relative_to(build)}")
    return slot0, slot1


def zip_name(channel, version):
    return f"cb91ai-watch-{channel}-{version}.zip"


def tag_name(channel, version):
    return f"watch-{channel}-{version}"


def version_key(text):
    return tuple(int(n) for n in text.replace("+", ".").split("."))


def entry(channel, version, data, notes):
    return {
        "version": version,
        "zip": zip_name(channel, version),
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "date": datetime.date.today().isoformat(),
        "notes": notes,
    }


def update_manifest(manifest, channel, new, strict=True):
    """[manifest] with [new] as its [channel]'s offer; never a lower version than the one it
    offers, nor ([strict], the website's) another ZIP under the same version: a watch that
    took the first would never take the second, MCUboot only booting a higher version"""
    manifest = dict(manifest)
    manifest.setdefault("v", 1)
    manifest["watch"] = "CB-91AI"
    channels = dict(manifest.get("channels", {}))
    old = channels.get(channel)
    if old is not None:
        if version_key(new["version"]) < version_key(old["version"]):
            raise SystemExit(f"{channel}: the manifest offers {old['version']}, higher than {new['version']}")
        if strict and new["version"] == old["version"] and new["sha256"] != old.get("sha256"):
            raise SystemExit(f"{channel}: {new['version']} is already on the website with another ZIP")
        if new["version"] == old["version"] and new["sha256"] == old.get("sha256"):
            new = dict(new, date=old.get("date", new["date"]))
    channels[channel] = new
    manifest["channels"] = channels
    return manifest


def served_manifest(url):
    """The manifest the website serves, {} when it has none yet"""
    request = urllib.request.Request(f"{url}?t={int(time.time() * 1000)}",
                                     headers={"Cache-Control": "no-cache"})
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return {}
        raise


def write_out(out, manifest, zip_bytes, name):
    out.mkdir(parents=True, exist_ok=True)
    (out / name).write_bytes(zip_bytes)
    (out / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (out / ".htaccess").write_text(HTACCESS, encoding="utf-8")


def from_zip(args):
    """The CI's part: a release's ZIP checked, the website's manifest updated"""
    data = args.zip.read_bytes()
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        variants = [(n, archive.read(n)) for n in sorted(archive.namelist()) if n.endswith(".bin")]
    version = check_variants(variants)
    name = zip_name(args.channel, version)
    if args.zip.name != name:
        raise SystemExit(f"{args.zip.name}: holds {version}, so it should be named {name}")
    notes = args.notes_file.read_text(encoding="utf-8").strip() if args.notes_file else args.notes
    # One line in the app: the release's first paragraph
    notes = notes.split("\n\n")[0].replace("\n", " ").strip()
    manifest = update_manifest(served_manifest(args.manifest_url), args.channel, entry(args.channel, version, data, notes))
    write_out(args.output, manifest, data, name)
    print(f"{args.channel}: {version}, {len(data)} bytes, sha256 {manifest['channels'][args.channel]['sha256']}")
    return 0


def from_build(args):
    """The bench PC's part: the ZIP of a build, the local folder, and the draft release"""
    slot0, slot1 = images(args.build)
    version = check_variants([(str(slot0), slot0.read_bytes()), (str(slot1), slot1.read_bytes())])
    args.output.mkdir(parents=True, exist_ok=True)
    name = zip_name(args.channel, version)
    zip_path = args.output / name
    subprocess.run([sys.executable, str(TOOLS / "dfu_zip.py"), str(slot0), str(slot1), "-o", str(zip_path)], check=True)
    data = zip_path.read_bytes()
    manifest_path = args.output / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.is_file() else {}
    # The local folder only stages: the same version built again replaces its ZIP there
    manifest = update_manifest(manifest, args.channel, entry(args.channel, version, data, args.notes), strict=False)
    write_out(args.output, manifest, data, name)
    print(f"{args.channel}: {version}, {len(data)} bytes -> {zip_path}")
    if not args.release:
        print(f"to upload by hand to public_html/firmware: manifest.json, {name}, .htaccess (from {args.output}),")
        print(f"or again with --release for the Site workflow; then check {BASE_URL}manifest.json")
        return 0
    head = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    tag = tag_name(args.channel, version)
    kind = "de développement" if args.channel == "dev" else "produit"
    notes = (f"{args.notes}\n\nImage {kind} de la montre CB-91AI, {version}, les deux variantes (une par slot) "
             f"signées par la clé du projet. Publier ce brouillon la met sur cobalt-watch.com/firmware "
             f"(workflow « Site »), où l'app Cobalt la trouve.")
    subprocess.run(["gh", "release", "create", tag, str(zip_path), "--repo", REPO, "--draft", "--target", head,
                    "--title", f"Montre {version} ({args.channel})", "--notes", notes], check=True)
    print(f"draft release {tag} on {REPO}, the tag at {head[:7]} once published (the commit must be pushed):")
    print("publish it on GitHub to put it on the website")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build", type=pathlib.Path, nargs="?",
                        help="sysbuild build directory (firmware/buildw, firmware/buildr)")
    parser.add_argument("--zip", type=pathlib.Path, help="a release's ZIP, instead of a build (the CI)")
    parser.add_argument("--channel", required=True, choices=("dev", "produit"))
    parser.add_argument("--notes", default="", help="what the update brings, in a line, in French")
    parser.add_argument("--notes-file", type=pathlib.Path, help="the notes from a file (the release's text)")
    parser.add_argument("--manifest-url", default=BASE_URL + "manifest.json",
                        help="with --zip: the manifest to update, as the website serves it")
    parser.add_argument("--release", action="store_true", help="also open a draft release of " + REPO)
    parser.add_argument("-o", "--output", type=pathlib.Path, default=DEFAULT_OUT, help="folder to upload")
    args = parser.parse_args()
    if (args.build is None) == (args.zip is None):
        parser.error("a build directory or --zip, one of the two")
    return from_zip(args) if args.zip else from_build(args)


if __name__ == "__main__":
    sys.exit(main())
