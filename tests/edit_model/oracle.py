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
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
if TESTS not in sys.path:
    sys.path.insert(0, TESTS)

import pyte  # noqa: E402

import gui_owner  # noqa: E402
import procinfo  # noqa: E402

from test_utf8_border import SafeScreen  # noqa: E402
from wpe_driver import WPE_BIN, XWPE_LIB_DIR, _RepStream  # noqa: E402

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


class EndTimeout(subprocess.TimeoutExpired):
    """The editor did not end when asked to: the text says where it is
    stuck (see EditSession.wait_exit), and is part of the message."""

    def __init__(self, cmd, timeout, why):
        super().__init__(cmd, timeout)
        self.why = why

    def __str__(self):
        return ("the editor did not end within %s s\n%s"
                % (self.timeout, self.why))


# How often the end of the editor is looked for while its output is read, and
# how long a second SIGTERM is given to end it (both stretched by the scale).
EXIT_POLL_S = 0.1
SECOND_SIGNAL_S = 2


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
        # the first position (0 cursor, 1 block begin, 2 block end, 3 find
        # start, 4 find end, 5 anchor) that is not a place of the text; -1
        # for none (an editor without the field says none)
        self.pos = int(kv.get("pos", "-1"))
        # the file's line-ending style (lf or crlf) and whether it ended on
        # a line break (1) or not (0); an editor without the fields says
        # an LF file that did
        self.eol = kv.get("eol", "lf")
        self.final = int(kv.get("final", "1"))
        # how many NUL bytes of the file the reader left out (a row cannot
        # hold one); 0 for none
        self.nul = int(kv.get("nul", "0"))
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
        self.says = int(kv.get("says", 0))    # status lines said since the dump before
        # what the question read now ignored before it was shown: the key and
        # button presses, and whether pasted text was among them (an editor
        # that reads no question here says neither)
        self.dropped = int(kv.get("dropped", 0))
        self.pasted = int(kv.get("pasted", 0))
        self.windows = {}
        self.find = None
        self.menu = None         # (bar entry, [row text]) while a dropdown is open
        self.raw = "\n".join(lines)
        for line in lines[1:]:
            if line.startswith("M "):
                _, bar, count, texts = line.split(" ", 3)
                rows = texts.split("|")
                assert len(rows) == int(count), line
                self.menu = (int(bar), rows)
            elif line.startswith("W "):
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
        # seed None: the file does not exist, as when the user names a new file
        if isinstance(seed, str):
            seed = seed.encode("utf-8")
        if seed is not None:
            with open(self.path, "wb") as fh:
                fh.write(seed)
        if file_mode is not None:
            os.chmod(self.path, file_mode)
        # The session's HOME has a configuration folder, so it is an existing
        # profile: a new one is asked which keys it wants (and these rows have
        # no one to answer).  The editor runs the Borland keys unless the
        # xwperc says another style.
        cfg = os.path.join(workdir, ".config", "xwpe")
        os.makedirs(cfg, exist_ok=True)
        if xwperc is not None:
            with open(os.path.join(cfg, "xwperc"), "w") as fh:
                fh.write(xwperc)
        self.screen = SafeScreen(cols, rows)
        self.stream = _RepStream(self.screen)
        self.reader = DumpReader()
        self.keys_sent = 0
        self.answers_ms = []
        self.trace_offset = 0
        self._fds_closed = False
        self.master_fd, slave_fd = pty.openpty()
        dump_r, dump_w = os.pipe()
        self.dump_fd = dump_r
        # The owner gives the editor its allow-list, no key of the machine's
        # and no assistant.  XWPE_KEYS is removed: the row's xwperc names its
        # key style, and a session style would override it (the owner runs
        # the Borland keys by default, which would run every "cua" row as
        # Borland).
        extra_env = {"XWPE_KEYS": None, "XWPE_LIB": XWPE_LIB_DIR,
                     "XWPE_UI_TRACE": self.trace_path,
                     "XWPE_TEST_DUMP_FD": str(dump_w)}
        extra_env.update(env_extra or {})
        start = time.monotonic()
        self.owned = gui_owner.Terminal.start(
            gui_owner.current(), binary or WPE_BIN, [filename], workdir,
            workdir, self.master_fd, slave_fd, (cols, rows),
            extra_env=extra_env, pass_fds=(dump_w,),
            on_output=self._feed_screen,
            extra_drains=((dump_r, self.reader.feed),),
            label="%s %s" % (os.path.basename(workdir), filename))
        self.proc = self.owned.proc
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

    def _pump(self, fds, seconds, counts):
        """Read the terminal and the dump pipe for at most @seconds, into the
        screen and the reader, adding the bytes taken to @counts (a dict with
        "pty" and "dump").  A descriptor that reached its end leaves @fds."""
        r, _, _ = select.select(fds, [], [], seconds)
        for fd in r:
            try:
                data = os.read(fd, READ_CHUNK)
            except OSError:
                data = b""
            if not data:
                fds.remove(fd)
                continue
            if fd == self.dump_fd:
                counts["dump"] += len(data)
                self.reader.feed(data)
            else:
                counts["pty"] += len(data)
                self._feed_screen(data)

    def _pump_until_ended(self, fds, seconds, counts):
        """Keep reading until the editor has ended or @seconds have passed;
        True when it has ended."""
        deadline = time.monotonic() + seconds
        while self.proc.poll() is None and time.monotonic() < deadline:
            self._pump(fds, EXIT_POLL_S, counts)
        return self.proc.poll() is not None

    def _why_it_did_not_end(self, fds, counts):
        """The facts that tell where an editor that was asked to end is
        stuck: whether its emergency save ran, what it has been writing,
        its run state, and whether a second signal ends it (the second takes
        the shortest way out, so it does only if the first was handled)."""
        saved = os.path.exists(self.path + ".ESV")
        state = procinfo.run_state(self.proc.pid)
        self.proc.send_signal(signal.SIGTERM)
        waited = gui_owner.scaled(SECOND_SIGNAL_S)
        second = self._pump_until_ended(fds, waited, counts)
        return (".ESV present: %s   drained during the wait: %d bytes (pty) "
                "%d bytes (dump)\nrun state: %s\nsecond SIGTERM: %s"
                % ("yes" if saved else "no", counts["pty"], counts["dump"],
                   state, "ended within %d s" % waited if second
                   else "still alive"))

    def wait_exit(self, timeout):
        """Wait for the editor to end after a signal, reading its terminal
        and its dump pipe the whole time, and return its exit status.

        An editor that has written more than the terminal holds blocks in
        that write and cannot end; reading while waiting is what lets a
        healthy editor finish its emergency save and leave.  When it does not
        end within @timeout (stretched by the wait scale), the error says
        where it is stuck: see _why_it_did_not_end."""
        fds = [self.master_fd, self.dump_fd]
        counts = {"pty": 0, "dump": 0}
        if self._pump_until_ended(fds, gui_owner.scaled(timeout), counts):
            return self.proc.returncode
        raise EndTimeout(self.proc.args, timeout,
                         self._why_it_did_not_end(fds, counts))

    def close(self):
        """End the editor through the owner (SIGTERM, its own ladder, SIGKILL,
        its terminal read meanwhile) and close the descriptors, once.  What
        the editor left errors the row when the row ends."""
        self.owned.end_for_row()
        if self._fds_closed:
            return
        self._fds_closed = True
        for fd in (self.master_fd, self.dump_fd):
            try:
                os.close(fd)
            except OSError:
                pass
