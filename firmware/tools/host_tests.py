#!/usr/bin/env python3
"""Run the host tests of the firmware's pure logic (firmware/tests/host).

    python firmware/tools/host_tests.py
    python firmware/tools/host_tests.py clean

The parts of the firmware that decide without touching the hardware (the gestures of
the buttons, lot D3; the format of the notes later, lot E2) are tested on the PC, where a
failure costs a second rather than a flash. This PC has no C compiler for Windows itself,
only the cross compilers of the board, but its WSL Ubuntu has gcc and make: the tool runs
the Makefile there. On Linux (the CI of lot A5) it runs make directly. The WSL
distribution is "Ubuntu" unless CB91AI_WSL_DISTRO says otherwise. Extra arguments go to
make.
"""

import os
import pathlib
import subprocess
import sys

TESTS_DIR = pathlib.Path(__file__).resolve().parents[1] / "tests" / "host"


def wsl_path(path):
    """C:\\Users\\... as WSL sees it: /mnt/c/Users/..."""
    posix = path.as_posix()
    return f"/mnt/{posix[0].lower()}{posix[2:]}"


def main():
    if os.name == "nt":
        distro = os.environ.get("CB91AI_WSL_DISTRO", "Ubuntu")
        command = ["wsl", "-d", distro, "--", "make", "-C", wsl_path(TESTS_DIR)]
    else:
        command = ["make", "-C", str(TESTS_DIR)]
    return subprocess.call(command + sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
