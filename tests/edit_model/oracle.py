"""A session with the editor that knows when the editor has taken its keys.

The editor started with ``XWPE_TEST_DUMP_FD`` writes its state to a pipe
just before it blocks for a key (``we_testdump.c``): how many keys it has
taken, where the next one is read, every text window's lines, cursor,
marks, modified count and undo depth, and the search strings, ended by
``E <seq>``.  ``EditSession.send`` writes an operation's keys, then waits
on the terminal and the pipe together until the dump that counts all of
them arrives: no fixed wait, no polling.  The screen of the same moment is
already in the terminal by then (the editor refreshes before it dumps), so
the session reads it at once.
"""
import os
import pty
import select
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
if TESTS not in sys.path:
    sys.path.insert(0, TESTS)

import pyte  # noqa: E402

from test_utf8_border import SafeScreen  # noqa: E402
from wpe_driver import (WPE_BIN, XWPE_LIB_DIR, _RepStream,  # noqa: E402
                        adopt_pty_as_controlling_tty, set_pty_size,
                        strip_size_env)

# ---- keys, as an xterm sends them -----------------------------------------

ESC = "\033"
ENTER = "\r"
TAB = "\t"
BACKSPACE = "\x7f"
DELETE = "\033[3~"
INSERT = "\033[2~"
UP, DOWN, RIGHT, LEFT = "\033[A", "\033[B", "\033[C", "\033[D"
HOME, END = "\033[H", "\033[F"
PGUP, PGDN = "\033[5~", "\033[6~"
CTRL_HOME, CTRL_END = "\033[1;5H", "\033[1;5F"
CTRL_PGUP, CTRL_PGDN = "\033[5;5~", "\033[6;5~"
CTRL_LEFT, CTRL_RIGHT = "\033[1;5D", "\033[1;5C"
SHIFT_UP, SHIFT_DOWN = "\033[1;2A", "\033[1;2B"
SHIFT_RIGHT, SHIFT_LEFT = "\033[1;2C", "\033[1;2D"
SHIFT_HOME, SHIFT_END = "\033[1;2H", "\033[1;2F"
SHIFT_DEL = "\033[3;2~"
CTRL_DEL = "\033[3;5~"
CTRL_INS = "\033[2;5~"
SHIFT_INS = "\033[2;2~"
F2, F3, F4 = "\033OQ", "\033OR", "\033OS"
ALT_BACKSPACE = ESC + BACKSPACE


def ctrl(letter):
    """The byte Ctrl-<letter> sends."""
    return chr(ord(letter.upper()) - ord("@"))


def alt(letter):
    """The keys Alt-<letter> sends: Esc, then the letter."""
    return ESC + letter


# The xwperc keys (we_opt.c) a session may set; Options2 153 is the CUA
# key style with the defaults, 156 the WordStar block mode.
OPTIONS2_CUA = 153
OPTIONS2_WORDSTAR = 156

# ---- waits -----------------------------------------------------------------

# How long an editor may take to answer one operation, or to start, before
# the session calls it a hang.  The channel ends every wait as soon as the
# answer is there, so this only bounds a failure.
ANSWER_TIMEOUT_S = 10.0
START_TIMEOUT_S = 15.0
READ_CHUNK = 65536
# The context the editor reports while it waits for something other than a
# key (a command, a program): a dump of that context is not an answer.
CTX_WAIT = "wait"


class ChannelTimeout(AssertionError):
    """The editor did not answer an operation within ANSWER_TIMEOUT_S."""


class ChannelMiscount(AssertionError):
    """The editor counted more keys than were sent: a key was split."""


# ---- the dump --------------------------------------------------------------

def unescape(text):
    """The bytes a dump field stands for (``\\xHH`` back to the byte)."""
    out = bytearray()
    i = 0
    raw = text.encode("latin-1")
    while i < len(raw):
        if raw[i:i + 2] == b"\\x" and i + 4 <= len(raw):
            out.append(int(raw[i + 2:i + 4], 16))
            i += 4
        else:
            out.append(raw[i])
            i += 1
    return bytes(out)


def _pair(value):
    y, x = value.split(",")
    return int(y), int(x)


class Window:
    """One text window of a dump."""

    def __init__(self, fields):
        self.n = int(fields[1])
        self.name = unescape(fields[2]).decode("utf-8", "replace")
        kv = dict(f.split("=", 1) for f in fields[3:])
        self.ro = int(kv["ro"])
        self.mx = int(kv["mx"])
        self.y = int(kv["y"])
        self.x = int(kv["x"])
        self.mb = _pair(kv["mb"])
        self.me = _pair(kv["me"])
        self.ks = _pair(kv["ks"])
        self.ins = int(kv["ins"])
        self.flg = int(kv["flg"])
        self.save = int(kv["save"])
        self.view = tuple(int(v) for v in kv["view"].split(","))
        self.pt = [tuple(int(v) for v in p.split(",")) for p in kv["pt"].split("/")]
        self.brp = [] if kv["brp"] == "-" else [int(v) for v in kv["brp"].split(",")]
        self.ud = int(kv["ud"])
        self.rd = int(kv["rd"])
        # the first row wider than Max. Columns or not ended as a row ends;
        # -1 for none (an editor without the field says none)
        self.rows = int(kv.get("rows", "-1"))
        self.lines = []          # [(end 'H'|'S', bytes)]

    @property
    def cursor(self):
        return (self.y, self.x)

    def text_rows(self):
        """The lines as (end, bytes) tuples, for comparison."""
        return list(self.lines)

    def disk(self):
        """The file this window writes on Save, from its lines."""
        out = bytearray()
        for end, data in self.lines:
            out += data
            if end == "H":
                out += b"\n"
        return bytes(out)


class Dump:
    """One dump: from ``S <seq>`` to ``E <seq>``."""

    def __init__(self, lines):
        head = lines[0].split(" ")
        self.seq = int(head[1])
        kv = dict(f.split("=", 1) for f in head[2:])
        self.ctx = kv["ctx"]
        self.top = int(kv["top"])
        self.autosaved = int(kv["autosaved"])
        self.windows = {}
        self.find = None
        self.raw = "\n".join(lines)
        for line in lines[1:]:
            if line.startswith("W "):
                w = Window(line.split(" "))
                self.windows[w.n] = w
            elif line.startswith("L "):
                n, end, rest = _split_l(line)
                self.windows[n].lines.append((end, unescape(rest)))
            elif line.startswith("F "):
                f = line.split(" ")
                self.find = (unescape(f[1]), unescape(f[2]), int(f[3]))

    @property
    def win(self):
        """The top window, if it is a text window."""
        return self.windows.get(self.top)


def _split_l(line):
    """``L <n> <end> <bytes>`` -> (n, end, bytes text); bytes keep spaces."""
    rest = line[2:]
    n, rest = rest.split(" ", 1)
    end, data = rest[0], rest[2:]
    return int(n), end, data


class DumpReader:
    """Collects whole dumps from the bytes of the pipe."""

    def __init__(self):
        self.pending = b""
        self.current = None
        self.dumps = []

    def feed(self, data):
        self.pending += data
        while b"\n" in self.pending:
            line, self.pending = self.pending.split(b"\n", 1)
            text = line.decode("latin-1")
            if text.startswith("S "):
                self.current = [text]
            elif self.current is not None:
                self.current.append(text)
                if text.startswith("E "):
                    self.dumps.append(Dump(self.current))
                    self.current = None


# ---- the session -----------------------------------------------------------

def segments(keys):
    """Split an operation's keys where a lone Esc must reach the editor on
    its own (followed at once, it would read as Alt with the next key)."""
    out, cur = [], []
    for k in keys:
        cur.append(k)
        if k == ESC:
            out.append(cur)
            cur = []
    if cur:
        out.append(cur)
    return out


class EditSession:
    """A live ``wpe`` whose every answer is a dump.

    ``xwperc`` is the text of a ``[General]`` options file written to the
    session's ``.config/xwpe/xwperc`` before start, or None.  The session's
    environment has the assistant off and the language servers' eager start
    off, and a fresh HOME.
    """

    def __init__(self, workdir, seed, filename="t.c", cols=80, rows=30,
                 xwperc=None, env_extra=None, binary=None, file_mode=None):
        self.workdir = workdir
        self.path = os.path.join(workdir, filename)
        self.trace_path = os.path.join(workdir, "ui.trace")
        if isinstance(seed, str):
            seed = seed.encode("utf-8")
        with open(self.path, "wb") as fh:
            fh.write(seed)
        if file_mode is not None:
            os.chmod(self.path, file_mode)
        if xwperc is not None:
            cfg = os.path.join(workdir, ".config", "xwpe")
            os.makedirs(cfg, exist_ok=True)
            with open(os.path.join(cfg, "xwperc"), "w") as fh:
                fh.write(xwperc)
        self.screen = SafeScreen(cols, rows)
        self.stream = _RepStream(self.screen)
        self.reader = DumpReader()
        self.keys_sent = 0
        self.answers_ms = []
        self.trace_offset = 0
        self.master_fd, slave_fd = pty.openpty()
        set_pty_size(self.master_fd, cols, rows)
        dump_r, dump_w = os.pipe()
        self.dump_fd = dump_r
        env = strip_size_env(os.environ.copy())
        for name in ("OPENAI_API_KEY", "ANTHROPIC_API_KEY", "XWPE_AI_ENABLE",
                     "XWPE_AI_BACKEND", "WAYLAND_DISPLAY", "DISPLAY"):
            env.pop(name, None)
        env.update(TERM="xterm-256color", LC_ALL="en_US.UTF-8", HOME=workdir,
                   XDG_CONFIG_HOME="", XDG_STATE_HOME="", XWPE_LIB=XWPE_LIB_DIR,
                   XWPE_LSP_NO_EAGER="1", XWPE_UI_TRACE=self.trace_path,
                   XWPE_TEST_DUMP_FD=str(dump_w))
        if env_extra:
            env.update(env_extra)
        start = time.monotonic()
        self.proc = subprocess.Popen(
            [binary or WPE_BIN, filename], stdin=slave_fd, stdout=slave_fd,
            stderr=slave_fd, cwd=workdir, env=env, pass_fds=(dump_w,),
            preexec_fn=adopt_pty_as_controlling_tty)
        os.close(slave_fd)
        os.close(dump_w)
        try:
            self.last = self._wait_for(0, START_TIMEOUT_S)
        except BaseException:
            self.close()          # an editor that never answered is not left running
            raise
        self.start_ms = (time.monotonic() - start) * 1000.0

    # -- context manager -----------------------------------------------------
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # -- reading -------------------------------------------------------------
    def _feed_screen(self, data):
        self.stream.feed(data.decode("utf-8", "replace"))

    def _drain_screen_now(self):
        """Take what the terminal holds already, without waiting."""
        while True:
            r, _, _ = select.select([self.master_fd], [], [], 0)
            if not r:
                return
            try:
                data = os.read(self.master_fd, READ_CHUNK)
            except OSError:
                return
            if not data:
                return
            self._feed_screen(data)

    def _take_answer(self, seq):
        """The first dump counting ``seq`` keys that is an answer, or None."""
        while self.reader.dumps:
            d = self.reader.dumps.pop(0)
            if d.seq > seq:
                raise ChannelMiscount("the editor counted %d keys, %d were sent"
                                      % (d.seq, seq))
            if d.seq == seq and d.ctx != CTX_WAIT:
                return d
        return None

    def _wait_for(self, seq, timeout):
        """Wait on the terminal and the pipe until the dump for ``seq``."""
        deadline = time.monotonic() + timeout
        fds = [self.master_fd, self.dump_fd]
        while True:
            d = self._take_answer(seq)
            if d is not None:
                self._drain_screen_now()
                return d
            left = deadline - time.monotonic()
            if left <= 0 or not fds:
                raise ChannelTimeout("no answer for key %d within %.0f s" % (seq, timeout))
            r, _, _ = select.select(fds, [], [], left)
            for fd in r:
                try:
                    data = os.read(fd, READ_CHUNK)
                except OSError:
                    data = b""
                if not data:
                    fds.remove(fd)
                    continue
                if fd == self.dump_fd:
                    self.reader.feed(data)
                else:
                    self._feed_screen(data)

    # -- writing -------------------------------------------------------------
    def send(self, keys, timeout=ANSWER_TIMEOUT_S):
        """Type ``keys`` (a list, one entry per key) and return the dump
        that answers the last of them."""
        if isinstance(keys, str):
            keys = [keys]
        start = time.monotonic()
        for seg in segments(keys):
            os.write(self.master_fd, "".join(seg).encode("utf-8"))
            self.keys_sent += len(seg)
            self.last = self._wait_for(self.keys_sent, timeout)
        self.answers_ms.append((time.monotonic() - start) * 1000.0)
        return self.last

    # -- what else there is to read ------------------------------------------
    def display(self):
        return list(self.screen.display)

    def disk(self, name=None):
        path = self.path if name is None else os.path.join(self.workdir, name)
        try:
            with open(path, "rb") as fh:
                return fh.read()
        except OSError:
            return None

    def trace_since_last(self):
        """The trace lines the editor wrote since the previous call."""
        try:
            with open(self.trace_path, "rb") as fh:
                fh.seek(self.trace_offset)
                data = fh.read()
        except OSError:
            return []
        self.trace_offset += len(data)
        return data.decode("utf-8", "replace").splitlines()

    def alive(self):
        return self.proc.poll() is None

    def ended_within(self, seconds):
        """Has the editor ended, allowing @seconds for a crash to finish (a
        process that dumps core stays until the dump is written, which takes
        long on a loaded machine)?"""
        try:
            self.proc.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            return False
        return True

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        for fd in (self.master_fd, self.dump_fd):
            try:
                os.close(fd)
            except OSError:
                pass
