#!/usr/bin/env python3
"""Shell of the CB-91AI through the debug probe (Segger RTT): send commands,
capture what comes back, no USB port needed.

`pyocd rtt` only takes its input from the keyboard of a real console, so a
script cannot drive it. This tool does the same job through the pyOCD Python
API: it attaches to the running target, finds the RTT control block, writes
each command to down channel 0 (the Zephyr RTT shell) and logs up channel 0
(shell output and firmware log) to firmware/logs/rtt-<date>.txt.

Same rules as bringup.py: the target is attached to, never halted nor reset
(the Bluetooth controller asserts when the core stops), and `auto_unlock` is
off so that a locked chip is never erased behind our back. One pyOCD session
at a time: do not run it together with bringup.py.

Usage:
    python firmware/tools/rtt_shell.py --cmd "cb91ai codec all"
    python firmware/tools/rtt_shell.py --cmd "kernel uptime" --cmd "cb91ai time"
    python firmware/tools/rtt_shell.py --seconds 30          # just listen

A command is over when the shell prompt is back and the line has been quiet
for a moment, or after --timeout seconds. The firmware polls its RTT input
every 250 ms and its down buffer holds 15 characters: a command goes through
in a few slices, which is why sending one takes about a second.

Requirements: `pip install pyocd`, a probe on the SWD pads.
"""

import argparse
import datetime
import pathlib
import re
import sys
import time

TARGET = "nrf52840"
LOG_DIR = pathlib.Path(__file__).resolve().parent.parent / "logs"
PROMPT = b"cb91ai:~$"
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
SHELL_PROMPT = re.compile(r"cb91ai:~\$ ?")
CONTROL_CHARS = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")


class Console:
    """Up channel 0 to the terminal and the log, line by line."""

    def __init__(self, up_chan, log):
        self.up_chan = up_chan
        self.log = log
        self.pending = b""
        self.since_mark = b""
        self.lines = 0
        self.last_data = time.time()

    def pump(self):
        """Read what the target has written since the last call."""
        data = self.up_chan.read()
        if not data:
            return
        self.last_data = time.time()
        self.pending += data
        self.since_mark += data
        while b"\n" in self.pending:
            raw, self.pending = self.pending.split(b"\n", 1)
            self.emit(raw)

    def mark(self):
        self.since_mark = b""

    def finished(self, cmd, sent_at):
        """The shell only shows its prompt when idle: a prompt that comes after
        the echo of the command means the command is over. Should a log line
        have cut the echo in two, any prompt a second after the send will do.
        """
        text = ANSI_ESCAPE.sub("", self.since_mark.decode("utf-8", "replace"))
        echo = text.find(cmd)
        if echo >= 0:
            return PROMPT.decode() in text[echo + len(cmd):]
        return time.time() - sent_at > 1.0 and text.rstrip().endswith(PROMPT.decode())

    def emit(self, raw):
        text = raw.decode("utf-8", "replace")
        text = CONTROL_CHARS.sub("", SHELL_PROMPT.sub("", ANSI_ESCAPE.sub("", text)))
        if text.strip():
            print(text, flush=True)
            self.log.write(text + "\n")
            self.lines += 1

    def flush(self):
        if self.pending:
            self.emit(self.pending)
            self.pending = b""


def send(down_chan, console, text, deadline):
    """Write one command, as fast as the 15-character down buffer drains."""
    data = (text + "\n").encode()
    while data and time.time() < deadline:
        data = data[down_chan.write(data):]
        console.pump()
        time.sleep(0.01)
    return not data


def attach(connect_helper, rtt_control_block, attempts):
    """Open a session on the running target and find its RTT control block.

    The probe loses the target while the firmware erases internal flash (the
    spare slot, about a minute after each start, or an incoming update): try
    again a little later instead of dying with a traceback.
    """
    for attempt in range(1, attempts + 1):
        session = connect_helper.session_with_chosen_probe(
            blocking=False,
            target_override=TARGET,
            connect_mode="attach",
            options={"auto_unlock": False},
        )
        if session is None:
            print("No debug probe found (or another pyOCD session holds it).", file=sys.stderr)
            return None, None
        try:
            session.open()
            control_block = rtt_control_block.from_target(session.board.target)
            control_block.start()
            return session, control_block
        except Exception as exc:  # pyOCD errors, and plain ones from the probe layer
            session.close()
            print(f"attach failed ({exc}); a flash erase on the target? attempt {attempt} of {attempts}",
                  file=sys.stderr, flush=True)
            if attempt < attempts:
                time.sleep(10)
    return None, None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cmd", action="append", default=[], help="shell command to send (repeatable, run in order)")
    parser.add_argument("--timeout", type=float, default=60, help="seconds a command may take (default 60)")
    parser.add_argument("--quiet", type=float, default=0.6, help="silence after the prompt that ends a command")
    parser.add_argument("--seconds", type=float, default=2, help="seconds to keep listening after the last command")
    parser.add_argument("--attempts", type=int, default=3, help="attach attempts, 10 s apart (default 3)")
    args = parser.parse_args()

    try:
        from pyocd.core.helpers import ConnectHelper
        from pyocd.debug.rtt import RTTControlBlock
    except ImportError:
        print("pyOCD is missing: pip install pyocd", file=sys.stderr)
        return 1

    LOG_DIR.mkdir(exist_ok=True)
    log_path = LOG_DIR / f"rtt-{datetime.datetime.now():%Y%m%d-%H%M%S}.txt"
    session, control_block = attach(ConnectHelper, RTTControlBlock, args.attempts)
    if session is None:
        return 1

    with session, open(log_path, "w", encoding="utf-8") as log:
        log.write(f"# rtt shell {datetime.datetime.now():%Y-%m-%d %H:%M:%S}\n")
        if not control_block.up_channels or not control_block.down_channels:
            print("RTT control block without an up and a down channel.", file=sys.stderr)
            return 1
        console = Console(control_block.up_channels[0], log)
        down_chan = control_block.down_channels[0]

        # What was waiting in the buffer belongs to the past, not to a command
        time.sleep(0.3)
        console.pump()
        for cmd in args.cmd:
            print(f"> {cmd}", flush=True)
            log.write(f"> {cmd}\n")
            sent_at = time.time()
            deadline = sent_at + args.timeout
            console.mark()
            if not send(down_chan, console, cmd, deadline):
                print("(command not taken by the target)", flush=True)
                log.write("# command not taken by the target\n")
                break
            while time.time() < deadline:
                console.pump()
                if time.time() - console.last_data > args.quiet and console.finished(cmd, sent_at):
                    break
                time.sleep(0.005)
            else:
                print(f"(no prompt after {args.timeout:.0f} s)", flush=True)
                log.write(f"# no prompt after {args.timeout:.0f} s\n")
        end = time.time() + args.seconds
        while time.time() < end:
            console.pump()
            time.sleep(0.005)
        console.flush()

    print(f"\n{console.lines} line(s), log saved to {log_path}")
    return 0 if console.lines or not args.cmd else 1


if __name__ == "__main__":
    sys.exit(main())
