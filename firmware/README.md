# CB-91AI firmware

Zephyr v4.4.2 firmware for the CB-91AI board (nRF52840). Two applications share
the board definition (`boards/`) and the drivers (`lib/`): `watch/`, the watch,
and `app/`, the bench self-test.

## Build

Zephyr v4.4.2 with the modules listed in `west.yml`, and the `arm-zephyr-eabi`
toolchain of the Zephyr SDK 1.0.1. A workspace from this repository:

```
west init -l firmware
west update -o=--depth=1 -n
pip3 install -r bootloader/mcuboot/scripts/requirements.txt
```

Then build with sysbuild, which builds MCUboot and both slot variants too:

```
west build --sysbuild -b cb91ai -s firmware/watch -d build/watch -p always -- -DBOARD_ROOT=$PWD/firmware -DEXTRA_CONF_FILE=debug.conf
```

- `debug.conf` gives the development image, `release.conf` the product image;
  `-s firmware/app` builds the self-test.
- `BOARD_ROOT` must point at `firmware/`: MCUboot, built apart, needs it.
- On Windows, keep the build folder's path short: beyond 260 characters, `ar`
  fails.

**Signing.** The environment variable `CB91AI_SIGNING_KEY` names the private key
that signs the images. Without it, sysbuild signs with MCUboot's development key
and warns: a board running the bootloader of `prebuilt/` refuses such images. The
project's key never lives in a repository.

**Versions.** MCUboot runs the image in place and goes back to the previous one
if the new image is not confirmed. Each build therefore gives two variants, one
per slot (`<build>/<app>` and `<build>/<app>_slot1_variant`), and each update
must carry a higher version, `VERSION_TWEAK` included, or MCUboot will not boot
it. Both applications share one sequence of versions on a given board.

## Flash through SWD

With a debug probe and pyOCD:

```
pip install pyocd
python firmware/tools/bringup.py
```

`bringup.py` programs the bootloader of `prebuilt/` and the confirmed image
(`zephyr.signed.confirmed.hex`), then saves the RTT log. With `--no-erase` it
keeps the settings and the other slot. Some rules:

- Always pass `-O auto_unlock=false` to pyOCD: otherwise a locked chip is erased
  without a word.
- While Bluetooth runs, attach with `-O connect_mode=attach` and never halt the
  core.
- Never write `UICR.APPROTECT` on the V1 boards: any value but `0xFF` locks the
  debug port.
- **Never power the board from USB or from the probe with a coin cell in
  place**: take the cell out before any wired work.

## Update over Bluetooth LE

```
python firmware/tools/ble_update.py --address AA:BB:CC:DD:EE:FF
```

The tool sends the local build: it picks the variant of the free slot, uploads
it, marks it for test, resets the board, checks it and confirms it, with
retries. An interrupted upload is never resumed: the tool erases the free slot
and sends everything again. `--status` shows the slots, `--no-confirm` leaves a
new image in test.

From a phone, always use the ZIP of `tools/dfu_zip.py`, which carries both
variants and their slot, never a lone `.bin`.

## Tools

| Tool | What it does |
|---|---|
| `bringup.py` | flash through pyOCD, and save the RTT log |
| `rtt_shell.py` | the shell through the debug probe |
| `usb_console.py`, `usb_update.py` | the shell and an update over USB, on the bench board |
| `ble_update.py` | update over Bluetooth LE |
| `dfu_zip.py` | the ZIP of both variants, for an update from a phone |
| `publish_update.py` | prepare an update for the website, where the app looks for it |
| `ble_pair.py` | pair a PC with a watch, as a phone does |
| `cobalt_link.py` | Cobalt Link on the PC side: the test harness of the protocol |
| `cobalt_time.py` | read, set and check the watch's clock |
| `ble_shell.py` | the self-test's commands over Bluetooth LE, on a sealed watch |
| `ble_test.py`, `ble_rssi.py`, `ble_trial.py` | scan, measure the radio link, advertising trials |
| `lfxo_drift.py` | measure the 32.768 kHz crystal against network time |
| `codec_eval.py`, `pcm_from_log.py` | codec evaluation: coding, transcription, speech dumps |
| `lcd_map.py` | the glass element behind each pixel of the display |
| `host_tests.py` | run the host tests (in WSL on Windows) |

## Tests

The host tests cover the logic that needs no hardware: button gestures,
sessions, notes, the calendar, the wrist raise, the cell. Run them with
`make -C firmware/tests/host` (gcc, with the sanitizers).

`tests/vectors/` holds the vectors of Cobalt Link and of the note format; the
apps check them too.

## Licence

Apache-2.0: see [LICENSE](../LICENSE) at the root of the repository.
