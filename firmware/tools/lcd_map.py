#!/usr/bin/env python3
"""Expected glass element for each (COM, SEG) pixel of the F-91W display, from
the Sensor Watch tables used by firmware/app/src/lcd.c. Prints the order of
`cb91ai walk start` (pixel mode: COM0 SEG0..23, COM1 SEG0..23, COM2 SEG0..23)
so that one photo per step can be checked against the map.

    python firmware/tools/lcd_map.py            # the 72 pixel steps
    python firmware/tools/lcd_map.py seg        # the 24 SEG steps (3 pixels each)
    python firmware/tools/lcd_map.py 1 16       # one pixel: COM1 SEG16
"""
import sys

SEGMENT_MAP = [
    0x4E4F0E8E8F8D4D0D, 0x0C8C4C4C8B4B4B0B, 0xC049C00A49890949, 0xC048088886874707,
    0xC053921252139352, 0xC054511415559594, 0xC057965616179716, 0xC041804000018A81,
    0xC043420203048382, 0xC045440506468584,
]
POSITION_NAMES = ["weekday 1", "weekday 2", "date tens", "date ones", "hour tens",
                  "hour ones", "min tens", "min ones", "sec tens", "sec ones"]
INDICATORS = {(0, 17): "signal", (0, 16): "bell", (2, 17): "PM", (2, 16): "24H",
              (1, 10): "LAP", (1, 16): "colon"}


def describe(com, seg):
    items = []
    for pos, entry in enumerate(SEGMENT_MAP):
        for i in range(8):
            byte = (entry >> (8 * i)) & 0xFF
            if byte >> 6 == com and byte & 0x3F == seg:
                letter = chr(ord("A") + i) if i < 7 else "X"
                items.append(f"P{pos}:{letter} ({POSITION_NAMES[pos]} seg {letter})")
    if (com, seg) in INDICATORS:
        items.append(INDICATORS[(com, seg)])
    return ", ".join(items) if items else "nothing in the map"


def main(argv):
    if len(argv) == 3:
        com, seg = int(argv[1]), int(argv[2])
        print(f"COM{com} SEG{seg}: {describe(com, seg)}")
    elif len(argv) == 2 and argv[1] == "seg":
        for seg in range(24):
            print(f"step {seg + 1:2d}: SEG{seg:<3d} " +
                  " | ".join(f"COM{com} {describe(com, seg)}" for com in range(3)))
    else:
        step = 0
        for com in range(3):
            for seg in range(24):
                step += 1
                print(f"step {step:2d}: COM{com} SEG{seg:<3d} {describe(com, seg)}")


if __name__ == "__main__":
    main(sys.argv)
