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
import base64
import fcntl
import os
import pty
import select
import struct
import termios
import shutil
import subprocess
import sys
import time

import pytest
import pyte

from test_utf8_border import SafeScreen

WPE_BIN = os.environ.get("WPE_BIN") or os.path.join(os.path.dirname(__file__), "..", "wpe")

# The data files xwpe loads at runtime (the in-app help, syntax_def) live under
# $XWPE_LIB when set, else the compiled-in install prefix. Point it at the
# checkout so the in-tree help.xwpe_eng is found via the editor's "_eng"
# fallback: the help/manual scenarios then pass from a plain build tree on any
# OS, instead of silently depending on a system-installed copy that happens to
# exist on the Linux dev box but not on a fresh macOS runner.
XWPE_LIB_DIR = os.path.dirname(os.path.abspath(WPE_BIN))

# macOS reserves the F1-F12 keys at the OS / terminal layer, so they never reach
# wpe -- macOS users drive those actions from the menu instead, which is the
# supported path there. Map each advertised function-key accelerator the tests
# send to its menu route, applied ONLY on macOS so every Linux scenario stays
# byte-for-byte unchanged. (The letters match we_menue.c in programming mode.)
MACOS = sys.platform == "darwin" or os.environ.get("XWPE_TEST_OS") == "macos"

_MACOS_KEY_ROUTE = {
    "\033OP": ["\033h", "e"],    # F1 -> Help -> Editor (manual viewer)
    "\033OQ": ["\033f", "s"],    # F2 -> File -> Save
    "\033OR": ["\033f", "m"],    # F3 -> File -> File-Manager
    "\033OS": ["\033s", "f"],    # F4 -> Search -> Find
    "\033[20~": ["\033r", "m"],  # F9 -> Run -> Make
    "\x0c": ["\033s", "s"],      # ^L -> Search -> Search again
}


def macos_key_route(k):
    """The keystrokes to actually send for one logical key. Identity on Linux;
    on macOS a dead function-key accelerator becomes its equivalent menu route."""
    if MACOS and k in _MACOS_KEY_ROUTE:
        return _MACOS_KEY_ROUTE[k]
    return [k]

# A loaded CI runner is 3-5x slower than a dev box, so the fixed wait/timeout
# budgets that pass in-tree flake under autopkgtest. XWPE_TEST_WAIT_SCALE
# stretches every wait without slowing the local run (default 1.0; the Debian
# autopkgtest sets it to 3). All waits funnel through _drain(), so scaling there
# scales the startup wait AND every per-key delay.
WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1") or 1)
COLS, ROWS = 80, 30


class _RepStream(pyte.Stream):
    """A pyte Stream that also handles REP (CSI Pn b).

    ncurses emits REP to compress long identical runs (window borders) on a
    non-UTF-8 terminal; without it pyte drops the repeats and a full-width
    border looks short.  Extending the class-level ``csi`` map is the
    version-stable way to add a control: the parser binds its CSI dispatcher
    from ``csi`` when the Stream is constructed, so no private re-init is
    needed.  (Older pyte exposed ``_initialize_parser`` for this; newer pyte --
    the one in Debian -- removed it, which broke the previous approach.)
    """
    csi = {**pyte.Stream.csi, "b": "repeat_last"}


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


def tool_usable(name, *args):
    """True only if `name` is on PATH AND actually runs (exit 0).

    shutil.which() finds the executable file, but a rustup / asdf / mise shim
    can sit on PATH and still fail at *runtime* -- no default toolchain set, a
    rust-toolchain.toml pinning a toolchain that is not installed, a component
    (e.g. rust-analyzer) not added.  A which()-only skip guard then lets the
    test RUN and FAIL with a confusing toolchain error instead of self-skipping.
    Probing `name --version` (or the given args) and checking the exit status is
    what distinguishes "installed and usable" from "a shim that errors", so a
    misconfigured toolchain skips with a precise reason -- matching the rest of
    the suite, which self-skips when a tool is absent."""
    path = shutil.which(name)
    if path is None:
        return False
    probe = list(args) if args else ["--version"]
    try:
        return subprocess.run(
            [path, *probe],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            timeout=30,
        ).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


class WpeSession:
    """A live wpe instance under a pyte-backed pty, with helpers to send
    keys, drive menus, save, and read back the file or the screen."""

    def __init__(self, workdir, seed, filename="t.c", cols=COLS, rows=ROWS,
                 wait=1.3, env_extra=None):
        self.workdir = workdir
        self.path = os.path.join(workdir, filename)
        with open(self.path, "w") as fh:
            fh.write(seed)
        self.screen = SafeScreen(cols, rows)
        self.stream = _RepStream(self.screen)   # Stream + REP (CSI Pn b) handler
        self.raw = bytearray()                  # every byte wpe emits, pre-parse
                                                # (pyte swallows OSC 52, so the
                                                # clipboard test scans this)
        self.master_fd, slave_fd = pty.openpty()
        env = os.environ.copy()
        env.update(TERM="xterm-256color", COLUMNS=str(cols), LINES=str(rows),
                   LC_ALL="en_US.UTF-8", HOME=workdir)
        # Load help/data from the checkout (hermetic), not a system-installed
        # copy; an explicit caller-set XWPE_LIB still wins.
        env.setdefault("XWPE_LIB", XWPE_LIB_DIR)
        # HOME=workdir gives xwpe a clean ~/.xwpe, but it also HIDES per-HOME
        # toolchain config: the rustup `rustc` / `cargo` / `rust-analyzer` shims
        # resolve the default toolchain from $HOME/.rustup, so under workdir they
        # die with "rustup could not choose a version of rustc ... one wasn't
        # configured" and every Rust compile/debug/LSP test breaks -- even though
        # the toolchain is installed and works in a normal shell. Point the
        # toolchain managers back at the real home (only if the user has not set
        # them) so the shims keep resolving under the test HOME.
        _real_home = os.path.expanduser("~")
        env.setdefault("RUSTUP_HOME", os.path.join(_real_home, ".rustup"))
        env.setdefault("CARGO_HOME", os.path.join(_real_home, ".cargo"))
        # The menu/file/etc. tests do not exercise the LSP, but opening t.c
        # on a box with clangd installed (e.g. Homebrew on macOS) pops up a
        # "Starting language server..." Messages window that overlays the
        # editor and breaks tests that screenshot the chrome (Help viewer,
        # window operations, ...).  Opt out of the eager-on-open boot here;
        # the LSP-specific tests use their own driver and override this.
        env.setdefault("XWPE_LSP_NO_EAGER", "1")
        # Tests that exercise locale-dependent behaviour (e.g. the non-UTF-8
        # chrome fallback) pass env_extra to override LC_ALL/LANG; applied last
        # so the override wins over the UTF-8 default above.
        if env_extra:
            env.update(env_extra)
        # Make the editor under test use THIS repo's syntax_def, not whatever
        # stale copy is installed on the machine: it reads $HOME/.xwpe/syntax_def
        # first and we set HOME=workdir, so drop the build-tree copy there.
        _synt = os.path.join(os.path.dirname(os.path.abspath(WPE_BIN)), "syntax_def")
        if os.path.exists(_synt):
            _xd = os.path.join(workdir, ".xwpe")
            os.makedirs(_xd, exist_ok=True)
            shutil.copyfile(_synt, os.path.join(_xd, "syntax_def"))
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
        deadline = time.time() + timeout * WAIT_SCALE
        while time.time() < deadline:
            r, _, _ = select.select([self.master_fd], [], [], 0.1)
            if r:
                try:
                    data = os.read(self.master_fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                self.raw.extend(data)
                self.stream.feed(data.decode("utf-8", "replace"))

    def osc52_payload(self):
        """Decode the most recent OSC 52 "set clipboard" sequence wpe emitted
        (ESC ] 52 ; c ; <base64> BEL), or None. Used to assert that a plain
        Copy pushed the selection to the OS clipboard."""
        raw = bytes(self.raw)
        marker = b"\033]52;c;"
        i = raw.rfind(marker)
        if i < 0:
            return None
        j = raw.find(b"\a", i)
        if j < 0:
            return None
        try:
            return base64.b64decode(raw[i + len(marker):j]).decode("utf-8", "replace")
        except Exception:
            return None

    # -- input ---------------------------------------------------------------
    def key(self, *keys, delay=0.45):
        """Send raw key strings, draining wpe output after each. On macOS a
        function-key accelerator is replaced by its menu route (see
        macos_key_route); on Linux every key is sent verbatim."""
        for k in keys:
            for stroke in macos_key_route(k):
                os.write(self.master_fd, stroke.encode())
                self._drain(delay)
        return self

    def resize(self, cols, rows, delay=0.6):
        """Resize the terminal: set the pty window size (the kernel sends
        SIGWINCH to wpe) and the pyte screen to match, then drain the repaint.
        Drives the ncurses KEY_RESIZE -> resizeterm -> e_relayout_windows path,
        the terminal peer of an X11 ConfigureNotify / a Wayland xdg configure."""
        winsize = struct.pack("HHHH", rows, cols, 0, 0)
        fcntl.ioctl(self.master_fd, termios.TIOCSWINSZ, winsize)
        try:
            self.screen.resize(rows, cols)
        except Exception:
            pass
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

    def display(self, drain=0.6):
        """The current pyte screen as a list of row strings.

        Drains pending pty bytes for up to ``drain`` seconds first so a caller
        that did `time.sleep(N)` after a key (instead of `key()`'s built-in
        drain) still sees output that arrived during the sleep.  Without this
        the slower-to-compile/run macOS pipeline left the run's "XQZ42",
        "End output" and "Return-Code" lines stuck in the kernel pty buffer
        and the display showed the pre-run snapshot."""
        if drain > 0:
            self._drain(drain)
        return self.screen.display

    def cell_bg(self, y, x):
        """Background colour name of the cell at row y, col x (pyte parses the
        SGR colour the terminal emitted, e.g. 'red').  'default' if unstyled."""
        return self.screen.buffer[y][x].bg

    def row_has_bg(self, y, bg):
        """True if any cell on row y has background colour `bg` -- used to find
        an inline mark without pinning the exact column."""
        row = self.screen.buffer[y]
        return any(row[x].bg == bg for x in range(self.screen.columns))

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
