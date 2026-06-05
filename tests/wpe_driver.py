"""Shared pyte driver for the per-menu-section wpe (ncurses) test suite.

Each menu section gets a `tests/test_menu_<section>.py` that drives wpe
through pyte, performs menu operations, saves, and asserts on the file
written to disk -- ground truth (the on-screen render can be captured
mid-repaint; the saved file cannot).

Usage:
    from wpe_driver import WpeSession, ALT, incoherence

    def test_xxx(tmp_path):
        with WpeSession(str(tmp_path), "AAA\\nBBB\\n") as w:
            w.menu(ALT.BLOCK, "o")     # Block -> Mark WhOle
            w.menu(ALT.EDIT, "c")      # Edit  -> Copy
            w.save()
            assert w.alive(), "wpe died"
            assert w.text().count("AAA") == 2

Incoherence flagging:
    @incoherence("Undo does not revert an XBuffer paste")
    def test_yyy(tmp_path): ...
    -> recorded as xfail; `pytest -rxX` lists every INCOHERENCE for a later
       manual review (turns green/XPASS when the behaviour is fixed).
"""
import os
import pty
import select
import subprocess
import time

import pytest
import pyte

from test_utf8_border import SafeScreen

WPE_BIN = os.environ.get("WPE_BIN") or os.path.join(os.path.dirname(__file__), "..", "wpe")
COLS, ROWS = 80, 30


class ALT:
    """Alt-<key> sequences that open each top-level menu."""
    SYSTEM = "\033#"
    FILE = "\033f"
    EDIT = "\033e"
    SEARCH = "\033s"
    BLOCK = "\033b"
    RUN = "\033r"
    DEBUG = "\033d"
    PROJECT = "\033p"
    OPTIONS = "\033o"
    WINDOW = "\033w"
    HELP = "\033h"


def incoherence(reason):
    """Mark a test that asserts behaviour we believe SHOULD hold but which
    currently does not (an incoherence found during coverage).  Recorded as
    xfail so CI stays green; `pytest -rxX` prints the review queue."""
    return pytest.mark.xfail(reason="INCOHERENCE: " + reason, strict=False)


class WpeSession:
    """A live wpe instance under a pyte-backed pty, with helpers to send
    keys, drive menus, save, and read back the file or the screen."""

    def __init__(self, workdir, seed, filename="t.c", cols=COLS, rows=ROWS,
                 wait=1.3):
        self.workdir = workdir
        self.path = os.path.join(workdir, filename)
        with open(self.path, "w") as fh:
            fh.write(seed)
        self.screen = SafeScreen(cols, rows)
        self.stream = pyte.Stream(self.screen)
        self.master_fd, slave_fd = pty.openpty()
        env = os.environ.copy()
        env.update(TERM="xterm-256color", COLUMNS=str(cols), LINES=str(rows),
                   LC_ALL="en_US.UTF-8", HOME=workdir)
        self.proc = subprocess.Popen(
            [WPE_BIN, filename], stdin=slave_fd, stdout=slave_fd,
            stderr=slave_fd, cwd=workdir, env=env, preexec_fn=os.setsid)
        os.close(slave_fd)
        self._drain(wait)

    # -- context manager -----------------------------------------------------
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # -- internals -----------------------------------------------------------
    def _drain(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            r, _, _ = select.select([self.master_fd], [], [], 0.1)
            if r:
                try:
                    data = os.read(self.master_fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                self.stream.feed(data.decode("utf-8", "replace"))

    # -- input ---------------------------------------------------------------
    def key(self, *keys, delay=0.45):
        """Send raw key strings, draining wpe output after each."""
        for k in keys:
            os.write(self.master_fd, k.encode())
            self._drain(delay)
        return self

    def menu(self, alt_seq, item, delay=0.45):
        """Open a top-level menu (ALT.<X>) and pick an item by its letter."""
        return self.key(alt_seq, item, delay=delay)

    def save(self):
        """File -> Save."""
        return self.key(ALT.FILE, "s", delay=0.5)

    # -- output --------------------------------------------------------------
    def text(self):
        """The file as written to disk (call save() first)."""
        with open(self.path) as fh:
            return fh.read()

    def display(self):
        """The current pyte screen as a list of row strings."""
        return self.screen.display

    def alive(self):
        """True while wpe is still running healthily."""
        return self.proc.poll() is None

    def exit_code(self):
        return self.proc.poll()

    def close(self):
        code = self.proc.poll()
        try:
            os.killpg(os.getpgid(self.proc.pid), 9)
        except Exception:
            pass
        try:
            os.close(self.master_fd)
        except Exception:
            pass
        self.proc.wait()
        return code
