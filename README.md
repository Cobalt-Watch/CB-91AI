# CB-91AI

**Open firmware for the CB-91AI, a replacement board for the Casio F-91W built
around a Nordic nRF52840.** The watch keeps its case, its glass, its buttons and
its CR2016 coin cell; the board adds voice notes, Bluetooth LE and updates over
the air.

> Not affiliated with Casio. "F-91W" only says which watch the board fits.

This repository holds the firmware. The companion app, Cobalt, is published
separately; firmware updates reach the watch through the app, from
[cobalt-watch.com](https://cobalt-watch.com/).

## What the watch does

- **The time** on the original glass, in 12 or 24 hours, kept by a 32.768 kHz
  crystal corrected in software.
- **Voice notes**: press and speak. The microphone records, the notes are coded
  in LC3 at 16 kbit/s and stored in flash, then delivered to the phone, which
  acknowledges each one only once it has written it.
- **Cobalt Link**: the radio stays off; the watch opens a short Bluetooth LE
  session when it has something to deliver, or when asked with a click. Pairing
  is authenticated by a code shown on the glass.
- **Updates over Bluetooth LE** with MCUboot, which runs each image in place and
  goes back to the previous one if a new image is not confirmed. Images are
  signed with the project's key.
- **Also**: the wrist raise, button gestures, a light behind the glass, and a
  journal of the temperature and of the cell.

## Status

Prototype V1 boards are in service. The V2 board is in design; its schematic
will be published here.

## Layout

| Path | What |
|---|---|
| `firmware/boards/` | the Zephyr board definition, `cb91ai` |
| `firmware/lib/` | a Zephyr module: the drivers (display, microphone, accelerometer, clock) and the logic that needs no hardware |
| `firmware/watch/` | the watch application |
| `firmware/app/` | the bench self-test |
| `firmware/tools/` | PC tools: flashing, updates over Bluetooth LE, the Cobalt Link harness, shells |
| `firmware/tests/host/` | host tests of the pure logic |
| `firmware/tests/vectors/` | test vectors of Cobalt Link and of the note format, shared with the apps |
| `firmware/prebuilt/` | the MCUboot bootloader, ready to flash; it carries the project's public key only |

Build, flash and update: [firmware/README.md](firmware/README.md).

## Licence

[Apache-2.0](LICENSE). Zephyr, MCUboot, liblc3 and the other modules are fetched
by west, each under its own licence.
