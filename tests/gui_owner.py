"""The one owner of every process a graphical test starts.

A graphical row starts an X server or a compositor, a window manager, the
editor, sometimes a helper client.  Before this module each of those was
started by hand: with the whole environment of the machine (so an editor
that lost its compositor opened on the user's own desktop), under fixed
display and socket names (so two suites broke each other), waited for by
fixed sleeps, and ended by one SIGTERM (so what the editor left behind
lived on).  This module is where all of that is decided once:

- ``Editor.start`` / ``Client.start`` / ``Owner.start_service``: one
  launcher.  The environment is built from a whitelist, never copied from
  the machine; the display must be one this owner started; every process
  runs in its own session, ends with pytest (PR_SET_PDEATHSIG on Linux),
  logs into the row's own folder, and is recorded in the ``Ledger`` with a
  pidfd (a kqueue watch on macOS) before anything waits on it.
- ``wait_for``: every wait is for a named event, stretched by
  XWPE_TEST_WAIT_SCALE, and a timeout says which event and which scale.
- ``OwnedProcess.end``: SIGTERM, the editor's own end ladder, SIGKILL, then
  the sweep: what the process left behind is found, ended, reaped and
  reported, and the row errors.  On Linux the owner is a child subreaper
  (set by the session fixture, never at import), so orphans of an editor
  come back to pytest; only processes that were in the editor's tree when
  it was ended, or in one of the sessions that tree used, are candidates.
  A process the test body started itself is never touched.
- the ``load`` fixture: XWPE_TEST_LOAD=<n> busy processes, owned and ended
  like any other, so a row can be run under a known CPU load.
- ``reject_reruns``: a graphical conftest calls it from
  ``pytest_configure`` so a run cannot hide a flaky row behind a rerun.
- ``Terminal.start`` / ``Terminal.run``: the editor on a pty, or run to
  completion, with the same whitelist and no display variable at all.
- ``tool_env`` / ``run_tool`` / ``run_nested``: a short tool, bounded, on the
  ledger, with an environment built from the whitelist and never a copy of the
  machine's; ``inherit=`` names the one more variable a row needs.
- ``current`` / ``install``: the process's owner, for code that is not a
  fixture.
- ``OwnedProcess.end_for_row``: the end a driver calls where an error would
  hide the row's own failure (the exit of a ``with`` block); what the process
  left is held and the row's last word (``Owner.end_since``, run by the root
  fixtures) names it.
- ``is_shared_server``: the build server an editor starts for a language
  server is shared with the user's tools and the next row, so the sweep never
  signals it.

Only /proc/<pid>/stat and /proc/<pid>/cwd of other processes are read,
never their command lines or environments.
"""
import atexit
import ctypes
import errno
import fcntl
import os
import pwd
import re
import resource
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import engine_guard  # noqa: E402
import procinfo  # noqa: E402  (needs HERE on the path)

IS_LINUX = procinfo.IS_LINUX
IS_MACOS = procinfo.IS_MACOS

# prctl(2) options this module uses (linux/prctl.h).
PR_SET_PDEATHSIG = 1
PR_SET_CHILD_SUBREAPER = 36
PR_GET_CHILD_SUBREAPER = 37

PROC_ROOT = "/proc"
# Fields of /proc/<pid>/stat after the command name: the state is the first
# (field 3 of the whole line), then ppid, pgrp and session.
STAT_STATE, STAT_PPID, STAT_PGRP, STAT_SESSION = 0, 1, 2, 3
# Field 22 of the whole line: when the process started, in clock ticks
# since boot.  A pid and its start time name one process for good.
STAT_STARTTIME = 19
ZOMBIE = "Z"

WAIT_SCALE_VAR = "XWPE_TEST_WAIT_SCALE"
LOAD_VAR = "XWPE_TEST_LOAD"
# Set to 0 only to show that a leaking row is caught by the sweep and by
# nothing else: with it off the stand-in's grandchild survives its row.
# The rows of test_gui_owner.py end their own grandchild by its pidfd; any
# other row run with it off leaves what its editor left running, so after
# such a run look for leftovers and end each one by its exact pid.
SWEEP_VAR = "XWPE_TEST_OWNER_SWEEP"

DEFAULT_POLL_S = 0.05
# After SIGKILL a process ends as soon as the kernel delivers it; this is
# only the bound on that, before the scale.
KILL_WAIT_S = 2.0
# How long a process the owner started without its own end ladder (a
# server, a client, a busy process) gets after SIGTERM, before the scale.
SERVICE_GRACE_S = 2.0
# How long what an editor left is given to end by itself before it counts as
# a leak, before the scale.
LEFTOVER_GRACE_S = 2.0
# Every sweep round ends one level of what was left: a killed process's
# own children come back to the subreaper and are found by the next round.
SWEEP_MAX_ROUNDS = 16
# The path of a Unix socket must fit sun_path (108 bytes with its NUL);
# a runtime folder deeper than this leaves no room for the socket name.
SOCKET_PATH_MAX = 100
# A runtime folder is made under the system temporary folder when its path
# is at most this long, else under SHORT_TMP_BASE.
TMP_BASE_MAX = 40
SHORT_TMP_BASE = "/tmp"
REAL_SESSION_RUNTIME = "/run/user/"
MS_PER_S = 1000.0

UTF8_LOCALE = "en_US.UTF-8"
TEST_SHELL = "/bin/sh"
FONT_SIZE = "10"
WL_WIDTH, WL_HEIGHT = "1024", "768"
BACKENDS = ("x11", "wayland")

# Passed through from the machine when set: where the tools are, and the
# options of a sanitizer build.  Nothing else of the machine's environment
# reaches a process this owner starts.
PASS_THROUGH = ("PATH", "TMPDIR", "RUSTUP_HOME", "CARGO_HOME", "JAVA_HOME",
                "GOPATH", "GOMODCACHE", "LD_LIBRARY_PATH", "ASAN_OPTIONS",
                "UBSAN_OPTIONS", "LSAN_OPTIONS")
# The editor's trace files, passed to an editor on a terminal when set in the
# machine's environment (CI and developers trace a whole run that way) and not
# set by the row itself.
TRACE_PASS_THROUGH = ("XWPE_UI_TRACE", "XWPE_LSP_TRACE", "XWPE_AI_TRACE",
                      "WPE_TRACE_FILE")
# The terminal size variables would override the window's own size.
NEVER_PASSED = ("LINES", "COLUMNS")
# Names a row may never inherit from the machine, whatever it asks for: the
# display, bus, authority and agent sockets lead a process to the user's own
# session, and HOME is the row's folder.
NEVER_INHERITED = ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET",
                   "XDG_RUNTIME_DIR", "HOME", "XAUTHORITY",
                   "DBUS_SESSION_BUS_ADDRESS", "SSH_AUTH_SOCK", "GPG_AGENT_INFO",
                   engine_guard.REAL_ENGINE_VAR, engine_guard.TOOLS_VAR)
# The machine's PATH as it was when this module was imported, before any row
# changed it: the tool farm of every owner is made from it (engine_guard.py).
MACHINE_TOOLS = engine_guard.MachineTools.from_path(os.environ.get("PATH"))
# What a nested run of the harness needs from the machine: which editor it
# tests and how it scales its waits.
HARNESS_VARS = ("WPE_BIN", "XWPE_BIN", "XWPE_TEST_WAIT_SCALE",
                "XWPE_TEST_SETTLE_MS", "XWPE_TEST_OS")
# The row's folder holds the names (never the values) it inherited.
INHERITED_FILE = "env-inherited.txt"

TERM_NAME = "xterm-256color"
COLS, ROWS = 80, 30
# struct winsize: rows, cols, xpixel, ypixel.
PTY_WINSIZE_FORMAT = "HHHH"
STDIN_FD, STDOUT_FD = 0, 1

# How a launched process's standard streams are wired (see Owner.launch);
# the third way is an int: the slave side of a pty the row reads.
LOG = "log"          # stdout and stderr to <log_dir>/<kind>.log, stdin empty
PIPES = "pipes"      # stdin and stdout pipes for the row, stderr to the log

# Which step of the child's setup failed, written by the child to a pipe the
# launcher reads, so each failure is reported in its own words.
STAGE_DEATH_SIGNAL = b"d"
STAGE_CONTROLLING_TTY = b"t"

SCREEN = "1024x768x24"
SERVER_START_S = 20.0
COMPOSITOR_START_S = 20.0
# How many bytes the server's display-number pipe is read in.
DISPLAY_LINE_MAX = 64
# Errors of pidfd_open that mean "not here" (an old kernel, a container's
# filter), not "that process is gone".
PIDFD_UNAVAILABLE = (errno.ENOSYS, errno.EPERM, errno.EACCES, errno.EINVAL)
POLL_WATCH_INTERVAL_S = 0.01
TOOL_HOME_PREFIX = "xwpe-tool-"
# How often an editor that is being ended has its terminal read, so what it
# writes on its way out never fills the terminal and blocks it.
TERMINAL_DRAIN_POLL_S = 0.02
TERMINAL_READ_CHUNK = 65536
# After a process has ended, what it wrote last is read for at most this long
# (before the scale).
FINAL_DRAIN_S = 1.0
# A build server the editor starts for a language server or a program (the
# Scala build server, Bloop) is shared with the user's own tools and with the
# next row; it is not what a row leaves behind and the sweep never signals
# it.  It is the process with this command name whose working directory is
# under this folder of the user's real home (Java ignores HOME, so the folder
# is the same whatever HOME a row has).
SHARED_SERVER_COMM = "java"
SHARED_SERVER_FOLDER = os.path.join(".local", "share", "scalacli", "bloop")


class OwnerTimeout(AssertionError):
    """A named event did not happen within its scaled deadline."""


class OwnerRefused(RuntimeError):
    """The launcher refused to start something it could not own safely."""


class EngineReached(AssertionError):
    """A row ran a program the tool farm denies (engine_guard.py)."""


class LeakedProcesses(AssertionError):
    """A row ended and left processes behind; the sweep ended them."""


def is_account_data(path):
    """True when the realpath of @path is the home folder of the account
    running the tests (what the password file says) or inside it: the user's
    own engine logins are there, whatever link reaches them."""
    own = os.path.realpath(pwd.getpwuid(os.getuid()).pw_dir)
    real = os.path.realpath(path)
    if _SESSION_BASETEMP is not None and _is_inside(real, _SESSION_BASETEMP):
        # The session's own scratch is never the user's login, even when the
        # temporary folder is inside the home; a link from there to the real
        # login resolves outside it and is judged below.
        return False
    return _is_inside(real, own)


def _is_inside(real, folder):
    """True when the realpath @real is the realpath of @folder or below it."""
    folder = os.path.realpath(folder)
    return real == folder or real.startswith(folder.rstrip(os.sep) + os.sep)


# The session's pytest basetemp, noted by Owner.use_farm_base.
_SESSION_BASETEMP = None


def session_error(stray_hits, late):
    """The error that fails the session, or None.

    It collects both what the tool farm denied when no row was running
    (@stray_hits, lines) and the processes left at the session's end (@late,
    leaks) before raising either, so one never hides the other.  It is an
    EngineReached when a denied program ran, else LeakedProcesses."""
    problems = []
    if stray_hits:
        problems.append("a program the tool farm denies ran when no row was "
                        "running:\n  %s" % "\n  ".join(stray_hits))
    if late:
        problems.append("left at the session's end, now ended "
                        "(LeakedProcesses):\n  %s"
                        % "\n  ".join(leak.describe() for leak in late))
    if not problems:
        return None
    kind = EngineReached if stray_hits else LeakedProcesses
    return kind("\n".join(problems))


class OutputHandlerFailed(AssertionError):
    """A driver's handler for what an editor wrote raised an error while the
    owner was ending the editor; reported at the row's end."""


class EndFailed(AssertionError):
    """A process the owner started did not end even after SIGKILL."""


class DisplayProtocolError(AssertionError):
    """An editor's log holds a display protocol error or a lost-display line."""


class UnexpectedExit(AssertionError):
    """An editor ended by itself with a crash signal or a non-zero code."""


# Lines that say an editor lost its display, as the display libraries and
# the editor itself print them, at the start of a line of the editor's log.
# The libwayland text is pinned to libwayland-client 1.26.0
# (display_handle_error: "%s#%u: error %d: %s", and
# "[destroyed object]: error %d: %s" when the object is gone; the send
# failures "Error sending request for wl_surface.damage_buffer: Broken
# pipe" and "Error marshalling request for ...", which are all a client
# prints when the compositor cut it off mid-request); Xlib's are
# "X connection to %s broken" and "XIO:  fatal IO error"; the last is the
# editor's own line when it ends because the display went away.  Check them
# again when the libwayland major or minor version changes.
DISPLAY_ERROR_LINE = re.compile(
    r"^(?:(?:\S+#\d+|\[destroyed object\]): error \d+: "
    r"|Error (?:sending|marshalling) request for \S+: "
    r"|X connection to .* broken"
    r"|XIO: "
    r"|xwpe: connection to .* lost: )")

# Signals that mean the editor crashed, whoever sent the end.  The editor's
# crash handler catches every one of them, writes its crash file and raises
# the signal again with the default action, so each leaves a core.
CRASH_SIGNALS = (signal.SIGSEGV, signal.SIGABRT, signal.SIGBUS,
                 signal.SIGFPE, signal.SIGILL, signal.SIGQUIT,
                 signal.SIGTRAP, signal.SIGSYS)
# The only signals a row or the user sends to end an editor on purpose; an
# editor found dead by any other signal before the row ended it is flagged.
QUIET_SIGNALS = (signal.SIGTERM, signal.SIGHUP)


def display_errors(text):
    """The lines of an editor's log that report a lost display.

    A graphical row is only honest when its editor never had its display
    taken away by a protocol error or a broken connection: such an editor
    lingers with no window and a row that only checks "still running" or
    "text saved" passes over it.  This reads text, the editor's own log,
    and returns the matching lines in order; a process that only inherited
    the editor's standard error (a clipboard helper) writes into the same
    file and is not told apart, so a row whose child is expected to print
    such a line says so with ``expect_display_loss``.  A line of a log the
    owner keeps for another process is never given to this function.  A row
    whose editor writes to a pty (stdio=<pty>, the XWPE_TEST_PTY rows) has
    an empty log, so nothing is scanned for it."""
    return [line for line in text.splitlines()
            if DISPLAY_ERROR_LINE.match(line)]


def unexpected_exit_text(returncode, by_ladder=False):
    """A phrase naming the exit when a row did not ask for it, else None.

    An editor found dead before the row ended it is flagged for a non-zero
    code and for every signal but SIGTERM and SIGHUP; exit 0 is a normal
    quit.  An editor the owner's ladder ended (by_ladder) is flagged only
    for a crash signal, since SIGKILL is the ladder's own last step."""
    if returncode is None or returncode == 0:
        return None
    if returncode < 0:
        number = -returncode
        name = signal.Signals(number).name
        if by_ladder:
            return "killed by %s" % name if number in CRASH_SIGNALS else None
        if number in QUIET_SIGNALS:
            return None
        return "killed by %s" % name
    if by_ladder:
        return None
    return "exited with code %d" % returncode

# ---------------------------------------------------------------------------
# Waiting by event


def wait_scale():
    """The factor every deadline is stretched by on a slow or loaded host.

    A row run on a sanitizer build or a busy CI runner sets
    XWPE_TEST_WAIT_SCALE instead of each row growing its own margins."""
    text = os.environ.get(WAIT_SCALE_VAR, "") or "1"
    scale = float(text)
    if scale <= 0:
        raise ValueError("%s must be positive, not %r" % (WAIT_SCALE_VAR, text))
    return scale


def scaled(seconds):
    """seconds stretched by the wait scale."""
    return seconds * wait_scale()


def _timeout_message(waited, scale, what, state):
    """The text of an OwnerTimeout: the event, how long, at which scale."""
    return "waited %.1f s (scale %.1f) for %s; last state: %s" % (
        waited, scale, what, state)


def wait_for(event_fn, what, timeout, poll=DEFAULT_POLL_S, state_fn=None):
    """Return event_fn's first true value, polling until timeout * scale.

    A row uses it to wait for the thing it needs (a window, a frame, a log
    line) instead of sleeping, so it goes on as soon as the event happens
    and, when the event never does, fails with a message naming the event,
    the deadline and the scale, plus state_fn's picture of what was there
    instead."""
    scale = wait_scale()
    deadline_s = timeout * scale
    start = time.monotonic()
    while True:
        value = event_fn()
        if value:
            return value
        if time.monotonic() - start >= deadline_s:
            break
        time.sleep(poll)
    state = state_fn() if state_fn is not None else repr(value)
    raise OwnerTimeout(_timeout_message(deadline_s, scale, what, state))


# ---------------------------------------------------------------------------
# The editor's own end ladder, read from the C headers


# The C files that define the editor's end ladder, one name leading to the
# next (E_EXIT_CHILDREN_MAX_MS to E_DAP_CLOSE_MAX_MS to the graces).
LADDER_SOURCES = ("we_unix.c", "we_dap.h", "we_child.h")
C_COMMENT = re.compile(r"/\*.*?\*/")


def _define_body(name):
    """The text after `#define name` in the ladder's C files, with its
    continuation lines joined and its comments removed, or None."""
    pattern = re.compile(r"^#define\s+%s\b(.*)$" % re.escape(name))
    for source in LADDER_SOURCES:
        with open(os.path.join(TOP, source), encoding="utf-8",
                  errors="replace") as fh:
            lines = fh.read().splitlines()
        for index, line in enumerate(lines):
            match = pattern.match(line)
            if match is None:
                continue
            body = match.group(1)
            while body.endswith("\\") and index + 1 < len(lines):
                index += 1
                body = body[:-1] + " " + lines[index]
            return C_COMMENT.sub(" ", body).strip()
    return None


def _define_ms(name):
    """The value of `#define name <expr>` in the ladder's C files, where expr
    is a sum of numbers and of other such names (parentheses allowed)."""
    body = _define_body(name)
    if body is None:
        raise RuntimeError("%s is defined in none of %s"
                           % (name, ", ".join(LADDER_SOURCES)))
    total = 0
    for term in body.replace("(", " ").replace(")", " ").split("+"):
        term = term.strip()
        total += int(term) if term.isdigit() else _define_ms(term)
    return total


def editor_end_ladder_s():
    """How long an editor may take to end after SIGTERM, before the scale.

    The editor gives each child its polite signal and grace, then SIGKILL
    and its grace (we_child.h), and waits for its children for at most
    E_EXIT_CHILDREN_MAX_MS (we_unix.c), read from the C files so it cannot
    drift.  A row that ends its editor waits that long before calling the
    end abrupt."""
    grace = _define_ms("WPE_CHILD_END_GRACE_MS")
    kill = _define_ms("WPE_CHILD_END_KILL_MS")
    return (grace + kill + _define_ms("E_EXIT_CHILDREN_MAX_MS")) / MS_PER_S


# ---------------------------------------------------------------------------
# The process table, read only through stat and cwd


def _stat_fields(pid):
    """/proc/<pid>/stat after the command name (state first), or None."""
    try:
        with open(os.path.join(PROC_ROOT, str(pid), "stat")) as fh:
            text = fh.read()
    except OSError:
        return None
    return text[text.rfind(")") + 2:].split()


def _stat_comm(pid):
    """The kernel's command name of pid, or None once it is gone."""
    try:
        with open(os.path.join(PROC_ROOT, str(pid), "stat")) as fh:
            text = fh.read()
    except OSError:
        return None
    return text[text.find("(") + 1:text.rfind(")")]


def session_of(pid):
    """The session id of pid, or None once it is gone."""
    if IS_LINUX:
        fields = _stat_fields(pid)
        if fields is None:
            return None
        return int(fields[STAT_SESSION])
    try:
        return os.getsid(pid)
    except OSError:
        return None


def start_ticks(pid):
    """When pid started, in clock ticks since boot (Linux), or None.

    The sweep compares it with what its snapshot recorded, so a number the
    kernel handed to a new process since then is never taken for the
    process the snapshot saw."""
    if not IS_LINUX:
        return None
    fields = _stat_fields(pid)
    if fields is None:
        return None
    return int(fields[STAT_STARTTIME])


def _boot_seconds():
    """Seconds since boot on the clock /proc start times count from.

    CLOCK_BOOTTIME where Python has it; /proc/uptime otherwise (the same
    kernel clock, so the two stay comparable)."""
    if hasattr(time, "CLOCK_BOOTTIME"):
        return time.clock_gettime(time.CLOCK_BOOTTIME)
    with open(os.path.join(PROC_ROOT, "uptime")) as uptime:
        return float(uptime.read().split()[0])


def now_ticks():
    """The current time in the clock ticks start_ticks counts in, or None.

    Only Linux has such a pair: /proc start times are ticks since boot, so
    the mark must be read on the boot clock.  Elsewhere start_ticks gives
    None (no process start time is compared), so the mark is None too and
    no other clock is guessed at.  Callers never pick a clock of their own."""
    if not IS_LINUX:
        return None
    return int(_boot_seconds() * os.sysconf("SC_CLK_TCK"))


def group_of(pid):
    """The process group of pid, or None once it is gone."""
    try:
        return os.getpgid(pid)
    except OSError:
        return None


def working_dir(pid):
    """Where pid runs, or None when it is gone or not readable."""
    try:
        return os.readlink(os.path.join(PROC_ROOT, str(pid), "cwd"))
    except OSError:
        return None


def command_name(pid):
    """The command name pid runs under (the kernel's, on Linux)."""
    if IS_LINUX:
        return _stat_comm(pid)
    return procinfo.command_run(pid)


def _direct_children_linux(ppid):
    """(pid, fields) of every direct child of ppid, from /proc."""
    found = []
    for entry in os.listdir(PROC_ROOT):
        if not entry.isdigit():
            continue
        fields = _stat_fields(int(entry))
        if fields is not None and int(fields[STAT_PPID]) == ppid:
            found.append((int(entry), fields))
    return found


def shared_server_folder():
    """The folder a shared build server runs in: under the home the system
    names for this user, not the HOME of the environment, which a row
    changes."""
    return os.path.join(pwd.getpwuid(os.getuid()).pw_dir, SHARED_SERVER_FOLDER)


def is_shared_server(pid):
    """True when pid is the shared build server, which the sweep leaves
    running.

    An editor that starts the Scala build server leaves it orphaned, and
    the next row (and the user's own scala-cli) uses it again; ending it
    after every row would make each Scala row start it cold and end the
    user's server with the test run.  The programs the server runs for a
    row are not it: their working directory is the row's folder, and the
    driver ends them by that."""
    if command_name(pid) != SHARED_SERVER_COMM:
        return False
    folder = shared_server_folder()
    where = working_dir(pid)
    return where is not None and (where == folder
                                  or where.startswith(folder + os.sep))


# ---------------------------------------------------------------------------
# prctl and the subreaper


def _libc():
    """The C library, for prctl(2); resolved before any fork."""
    return ctypes.CDLL(None, use_errno=True)


_LIBC = _libc() if IS_LINUX else None
_subreaper_lock = threading.Lock()
_subreaper_users = 0


def subreaper_set():
    """True when this process is a child subreaper (Linux), else False.

    A row checks it to prove that importing or collecting the module
    changes nothing about the pytest process."""
    if not IS_LINUX:
        return False
    value = ctypes.c_int(0)
    _LIBC.prctl(PR_GET_CHILD_SUBREAPER, ctypes.byref(value), 0, 0, 0)
    return value.value != 0


def owners_begun():
    """How many owners of this process are begun and not finished.

    The subreaper is set exactly while this is above 0, so a row that runs
    beside a suite whose session owner is live still checks the flag
    against what it should be, not against unset."""
    with _subreaper_lock:
        return _subreaper_users


def _set_subreaper(on):
    """Make this process a child subreaper, or stop being one."""
    if _LIBC.prctl(PR_SET_CHILD_SUBREAPER, 1 if on else 0, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), "prctl(PR_SET_CHILD_SUBREAPER)")


def _acquire_subreaper():
    """Count one more owner that needs orphans to come back to pytest."""
    global _subreaper_users
    if not IS_LINUX:
        return
    with _subreaper_lock:
        if _subreaper_users == 0:
            _set_subreaper(True)
        _subreaper_users += 1


def _release_subreaper():
    """Count one owner less; the last one stops the subreaper."""
    global _subreaper_users
    if not IS_LINUX:
        return
    with _subreaper_lock:
        _subreaper_users -= 1
        if _subreaper_users == 0:
            _set_subreaper(False)


def _note_stage(stage_fd, stage):
    """Tell the launcher which step of the child's setup failed."""
    if stage_fd is None:
        return
    try:
        os.write(stage_fd, stage)
    except OSError:
        pass


def _take_controlling_tty():
    """Make the terminal on stdin (else on stdout) this session's controlling
    terminal, so TIOCSWINSZ on its master reaches the process as SIGWINCH.

    The caller is already a session leader (start_new_session); the pty is
    what the standard streams were set to.  stdout is the fallback for a row
    whose stdin is a pipe (an input that ends)."""
    if not hasattr(termios, "TIOCSCTTY"):
        return
    try:
        fcntl.ioctl(STDIN_FD, termios.TIOCSCTTY, 0)
    except OSError:
        fcntl.ioctl(STDOUT_FD, termios.TIOCSCTTY, 0)


def _child_setup(parent_pid, controlling_tty=False, stage_fd=None,
                 core_limit=None):
    """Return the function a new child runs before exec.

    With core_limit (a byte count, 0 for none) the child's core file size
    limit is set to it before exec, and the process and everything it starts
    inherit it: a row that kills its editor with a crash signal on purpose
    gives the machine no core file to keep and no journal record to read.

    On Linux it asks for SIGTERM when pytest's main thread dies, so a
    pytest that was killed takes its displays and editors with it, and it
    gives up at once if pytest died before the request was in place.  When
    the request is refused the launch fails: a process that would outlive a
    killed pytest is never started.  With controlling_tty it then takes the
    terminal on its standard streams as its controlling terminal; a failure
    of either step is written to stage_fd before it is raised."""
    prctl = _LIBC.prctl if IS_LINUX else None

    def setup():
        if prctl is not None \
                and prctl(PR_SET_PDEATHSIG, signal.SIGTERM, 0, 0, 0) != 0:
            _note_stage(stage_fd, STAGE_DEATH_SIGNAL)
            raise OSError(ctypes.get_errno(), "prctl(PR_SET_PDEATHSIG)")
        if os.getppid() != parent_pid:
            os._exit(1)
        if core_limit is not None:
            resource.setrlimit(resource.RLIMIT_CORE, (core_limit, core_limit))
        if controlling_tty:
            try:
                _take_controlling_tty()
            except OSError:
                _note_stage(stage_fd, STAGE_CONTROLLING_TTY)
                raise
    return setup


def _read_stage(stage_fd):
    """The step the child said failed, or b"" when it said nothing."""
    os.set_blocking(stage_fd, False)
    try:
        return os.read(stage_fd, 1)
    except (BlockingIOError, OSError):
        return b""


def _start_failure_text(kind, label, stage):
    """The launcher's words for a child that could not be set up."""
    if stage == STAGE_CONTROLLING_TTY:
        return ("%s for %s was not started: its terminal could not be made "
                "its controlling terminal (TIOCSCTTY on stdin, then stdout)"
                % (kind, label))
    return ("%s for %s was not started: its death signal "
            "(PR_SET_PDEATHSIG) could not be set" % (kind, label))


def process_running(pid, started=None):
    """True while pid is a live process, False once it has ended.

    A zombie has ended: kill(pid, 0) finds it, which is why a row must ask
    here.  With started (the start time start_ticks(pid) gave when the row
    first saw the process) a pid the kernel has handed to a newer process is
    not taken for the old one, and an ended orphan that pytest adopted (it is
    the subreaper while an owner is begun) is collected by its exact pid;
    without it nothing is collected, because the zombie may be the exit
    status of a child the row's own Popen still owes a wait.  A pid the
    current owner started is never collected here either; pid must not be a
    child the row started with its own Popen."""
    if IS_LINUX:
        fields = _stat_fields(pid)
        if fields is None:
            return False
        if started is not None and int(fields[STAT_STARTTIME]) != started:
            return False
        if fields[STAT_STATE] != ZOMBIE:
            return True
        _collect_adopted_zombie(pid, fields, started)
        return False
    return procinfo.run_state(pid) not in (procinfo.GONE, procinfo.ZOMBIE)


def process_gone(pid):
    """True when nothing of pid is left that a row still has to wait for.

    The process is absent, or it is a zombie that pytest itself adopted: an
    orphan whose parent ended first (the editor killed the server a helper
    belonged to), which pytest, the child subreaper, collects when the row
    ends.  A zombie whose parent is another process is NOT gone: it is an
    editor that never waited for a child it ended, which is the defect a row
    that asks this is looking for.  Without a subreaper an orphan goes to the
    system's init and is collected at once, so the two cases read the same on
    every system."""
    if IS_LINUX:
        fields = _stat_fields(pid)
        if fields is None:
            return True
        if fields[STAT_STATE] != ZOMBIE:
            return False
        return int(fields[STAT_PPID]) == os.getpid()
    return procinfo.run_state(pid) == procinfo.GONE


def group_running(pgid):
    """True while any member of process group pgid is still running.

    A member that has ended and waits to be collected (a zombie) is not
    running, though kill(-pgid, 0) still finds it: under the subreaper the
    orphans of an ended process group stay zombies of pytest until the row's
    sweep, so a row that asks whether a group is gone asks here."""
    if not IS_LINUX:
        try:
            os.killpg(pgid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True
    for entry in os.listdir(PROC_ROOT):
        if not entry.isdigit():
            continue
        fields = _stat_fields(int(entry))
        if fields is not None and int(fields[STAT_PGRP]) == pgid \
                and fields[STAT_STATE] != ZOMBIE:
            return True
    return False


def _collect_adopted_zombie(pid, fields, started):
    """Reap one zombie that is a child of pytest and is, by start time, the
    process the caller named; never waitpid(-1).

    A pid in the current owner's ledger is never reaped: its own Popen owes
    the wait and would get 0 instead of the exit status.  The caller must
    not pass a child the row started with its own Popen either; this cannot
    tell it from an adopted orphan (it is exact once the row has no Popen of
    its own)."""
    if started is None or int(fields[STAT_PPID]) != os.getpid():
        return
    if _CURRENT is not None and pid in _CURRENT.ledger.pids():
        return
    try:
        os.waitpid(pid, os.WNOHANG)
    except ChildProcessError:
        pass


# ---------------------------------------------------------------------------
# Watching one process: pidfd on Linux, kqueue on macOS


class _PidfdWatch:
    """A pidfd on one process: signals and waits that cannot hit a reused
    pid, and the reap of a process this pytest adopted."""

    def __init__(self, pid):
        self.pid = pid
        self.fd = os.pidfd_open(pid)

    def signal(self, sig):
        """Send sig; a process that has already ended is not an error."""
        try:
            signal.pidfd_send_signal(self.fd, sig)
        except ProcessLookupError:
            pass

    def wait(self, timeout_s):
        """True once the process has ended, waiting at most timeout_s."""
        poller = select.poll()
        poller.register(self.fd, select.POLLIN)
        return bool(poller.poll(max(0, int(timeout_s * MS_PER_S))))

    def reap(self):
        """Collect the ended process's status (only for an adopted one)."""
        try:
            os.waitid(os.P_PIDFD, self.fd, os.WEXITED)
        except ChildProcessError:
            pass

    def close(self):
        """Release the pidfd; calling it again does nothing."""
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


class _KqueueWatch:
    """A kqueue NOTE_EXIT watch on one process (macOS, the BSDs)."""

    def __init__(self, pid):
        self.pid = pid
        self.kq = select.kqueue()
        event = select.kevent(pid, filter=select.KQ_FILTER_PROC,
                              flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
                              fflags=select.KQ_NOTE_EXIT)
        self.kq.control([event], 0, 0)
        self.ended = False

    def signal(self, sig):
        """Send sig while the watch holds; an ended process is not an error."""
        if self.ended:
            return
        try:
            os.kill(self.pid, sig)
        except ProcessLookupError:
            pass

    def wait(self, timeout_s):
        """True once the process has ended, waiting at most timeout_s."""
        if not self.ended:
            self.ended = bool(self.kq.control(None, 1, max(0.0, timeout_s)))
        return self.ended

    def reap(self):
        """Nothing to collect: without a subreaper launchd adopts orphans."""

    def close(self):
        """Release the kqueue; calling it again does nothing."""
        if self.kq is not None:
            self.kq.close()
            self.kq = None


class _PollWatch:
    """A watch that looks at the process table: for a kernel without
    pidfd_open or a container whose filter refuses it, where nothing better
    is there.  A pid is only trusted while its start time is the one seen
    first, so a number the kernel hands to a new process is never signalled."""

    def __init__(self, pid):
        self.pid = pid
        self.started = start_ticks(pid)
        self.ended = False

    def _same_and_alive(self):
        """True while the process is the one first seen and has not ended."""
        if self.ended:
            return False
        if procinfo.run_state(self.pid) in (procinfo.GONE, procinfo.ZOMBIE) \
                or start_ticks(self.pid) != self.started:
            self.ended = True
        return not self.ended

    def signal(self, sig):
        """Send sig while the process is the one watched; an ended process is
        not an error."""
        if not self._same_and_alive():
            return
        try:
            os.kill(self.pid, sig)
        except ProcessLookupError:
            pass

    def wait(self, timeout_s):
        """True once the process has ended, waiting at most timeout_s."""
        deadline = time.monotonic() + max(0.0, timeout_s)
        while self._same_and_alive():
            if time.monotonic() >= deadline:
                return False
            time.sleep(POLL_WATCH_INTERVAL_S)
        return True

    def reap(self):
        """Collect the ended process's status (only for an adopted one)."""
        try:
            os.waitpid(self.pid, os.WNOHANG)
        except ChildProcessError:
            pass

    def close(self):
        """Nothing is held: calling it again does nothing."""


def _open_pidfd_watch(pid):
    """A pidfd watch, or None where this host does not give pidfds (the
    error says "not here", not "that process is gone")."""
    if not hasattr(os, "pidfd_open"):
        return None
    try:
        return _PidfdWatch(pid)
    except OSError as error:
        if error.errno in PIDFD_UNAVAILABLE:
            return None
        raise


def watch_process(pid):
    """A watch on pid for this platform: pidfd, else kqueue, else a poll of
    the process table (a host that filters pidfd_open), never an error in
    every row."""
    watch = _open_pidfd_watch(pid)
    if watch is not None:
        return watch
    if hasattr(select, "kqueue"):
        return _KqueueWatch(pid)
    return _PollWatch(pid)


# ---------------------------------------------------------------------------
# The ledger


class Ledger:
    """Every process the owner started and has not ended yet.

    The sweep leaves these out (each is ended by its own row or fixture),
    and the session's end ends whatever is still in it, each by its own
    pidfd."""

    def __init__(self):
        self._owned = {}

    def add(self, owned):
        """Record a process the owner has just started."""
        self._owned[owned.pid] = owned

    def forget(self, pid):
        """Drop a process that has ended; an unknown pid is ignored."""
        self._owned.pop(pid, None)

    def pids(self):
        """The pids of everything still owned, a copy the caller may keep."""
        return set(self._owned)

    def entries(self):
        """The owned processes, a copy the caller may end while it loops."""
        return list(self._owned.values())


class Leak:
    """One process a row left behind, as the sweep found it."""

    def __init__(self, pid, comm, cwd, state, label, declared):
        self.pid = pid
        self.comm = comm
        self.cwd = cwd
        self.state = state
        self.label = label
        self.declared = declared

    def describe(self):
        """One line for the leak file and the row's error: who, what it was
        doing and which row left it."""
        note = " (declared daemon)" if self.declared else ""
        return "pid %d comm %s state %s cwd %s left by %s%s" % (
            self.pid, self.comm, self.state, self.cwd, self.label, note)


# ---------------------------------------------------------------------------
# Displays the owner started


class XDisplay:
    """An X display this owner started (its server is in the ledger)."""

    def __init__(self, name, server):
        self.name = name
        self.server = server


class WaylandDisplay:
    """A compositor socket this owner started, in its private runtime
    folder, with the owner's X display for any fallback to land on."""

    def __init__(self, runtime_dir, socket, compositor, x_display):
        self.runtime_dir = runtime_dir
        self.socket = socket
        self.compositor = compositor
        self.x_display = x_display

    def socket_path(self):
        """The full path of the compositor's socket."""
        return os.path.join(self.runtime_dir, self.socket)


# ---------------------------------------------------------------------------
# The environment


def _base_env(home, runtime_dir):
    """The whitelisted environment every owned process starts from.

    A row then sees the same locale, shell and empty configuration on
    every machine, and nothing of the user's desktop, keys or settings."""
    _check_home(home)
    env = {}
    for name in PASS_THROUGH:
        value = os.environ.get(name)
        if value is not None:
            env[name] = value
    real_home = os.path.expanduser("~")
    # Under the row's HOME the rustup shims would not find a toolchain;
    # point them at the machine's, as the terminal driver does.
    env.setdefault("RUSTUP_HOME", os.path.join(real_home, ".rustup"))
    env.setdefault("CARGO_HOME", os.path.join(real_home, ".cargo"))
    env.update(LC_ALL=UTF8_LOCALE, LANG=UTF8_LOCALE, SHELL=TEST_SHELL,
               HOME=home, XDG_CONFIG_HOME="", XDG_STATE_HOME="")
    if runtime_dir is not None:
        env["XDG_RUNTIME_DIR"] = runtime_dir
    return env


NO_PASSWD_HOME = "no-passwd-home"
TEST_FAIL_VARIABLE = "XWPE_TEST_FAIL"


def _home_removal_is_safe(extra_env):
    """May this row run with no HOME at all?

    With no HOME the editor asks the password file for the home of the user
    running it, which is the machine's own home folder.  The seam
    XWPE_TEST_FAIL=no-passwd-home makes that step answer "none", so the row
    can never reach the tester's real home; without it a row that removes
    HOME is refused."""
    value = (extra_env or {}).get(TEST_FAIL_VARIABLE) or ""
    return NO_PASSWD_HOME in value.split(",")


def _check_home(home, extra_env=None):
    """Refuse the machine's own home folder as a process's HOME.

    With XDG_CONFIG_HOME empty the editor reads its settings and keys from
    HOME, so a row given the real one would run on the user's configuration
    and could change it.  A row that removes HOME (@home is None) is allowed
    only with the password-file seam, named above."""
    if home is None:
        if not _home_removal_is_safe(extra_env):
            raise OwnerRefused("a row that removes HOME must also set %s=%s, "
                               "or the editor resolves this machine's own "
                               "home folder from the password file"
                               % (TEST_FAIL_VARIABLE, NO_PASSWD_HOME))
        return
    if os.path.realpath(home) == os.path.realpath(os.path.expanduser("~")):
        raise OwnerRefused("HOME %s is this machine's own home folder" % home)


def _check_extra_displays(owner, extra_env):
    """Refuse a row's variables that would lead a process off the owner's
    displays: an X display it did not start, a runtime folder it did not
    make, a Wayland socket named by a path (which ignores the runtime
    folder) or handed down as an open connection, or the machine's home.
    A bare socket name stays allowed: it resolves inside the owner's
    runtime folder, as a row that names a missing compositor needs."""
    extra_env = extra_env or {}
    if "WAYLAND_SOCKET" in extra_env:
        raise OwnerRefused("WAYLAND_SOCKET hands down a compositor connection "
                           "the owner did not open")
    if "HOME" in extra_env:
        _check_home(extra_env["HOME"], extra_env)
    if "DISPLAY" in extra_env:
        owner.x_display(extra_env["DISPLAY"])
    if "XDG_RUNTIME_DIR" in extra_env \
            and extra_env["XDG_RUNTIME_DIR"] not in owner.runtime_dirs():
        raise OwnerRefused("runtime folder %s was not made by this owner"
                           % extra_env["XDG_RUNTIME_DIR"])
    _check_socket_name(extra_env.get("WAYLAND_DISPLAY", ""))


def _check_socket_name(socket):
    """Refuse a Wayland socket given as a path instead of a name."""
    if "/" in socket:
        raise OwnerRefused("Wayland socket %s is a path, not a name in the "
                           "owner's runtime folder" % socket)


def _apply_extra(env, extra_env):
    """The row's own variables last, never the terminal size ones.  A value
    of None removes the variable (a row that asks the first-run question)."""
    for name, value in (extra_env or {}).items():
        if name in NEVER_PASSED:
            continue
        if value is None:
            env.pop(name, None)
        else:
            env[name] = value
    for name in NEVER_PASSED:
        env.pop(name, None)
    return env


def _editor_env(env):
    """What every editor gets on top of the base: the checkout's data, a
    known font size, no eager language server."""
    env.update(XWPE_FONT_SIZE=FONT_SIZE)
    return _common_editor_env(env)


def _common_editor_env(env):
    """What every editor gets, graphical or on a terminal: the checkout's
    data and no eager language server."""
    env.update(XWPE_LIB=TOP, XWPE_LSP_NO_EAGER="1")
    # A row starts in an empty HOME, a new profile, which is asked which keys
    # it wants; nobody answers there, so rows run the Borland keys unless the
    # row's own environment names another style (or removes this variable).
    env.setdefault("XWPE_KEYS", "borland")
    return env


def _terminal_env(env):
    """What an editor on a terminal gets on top of the base: the terminal
    type, and no display variable (there is none in the base)."""
    env["TERM"] = TERM_NAME
    return _common_editor_env(env)


def _inherit(owner, env, names, log_dir):
    """Copy the machine's variables a row named, when they are set.

    A row that needs one more variable of the machine (an API key for a live
    row) names it here; the names in NEVER_INHERITED and the terminal size
    are refused.  Only the names are recorded, in the row's folder, never a
    value."""
    taken = []
    for name in names:
        if name in NEVER_INHERITED or name in NEVER_PASSED:
            raise OwnerRefused("%s is never inherited from the machine" % name)
        if name in engine_guard.API_KEY_NAMES and not owner.row_is_live():
            raise OwnerRefused("%s is inherited only by a row of a live file "
                               "(%s) with %s=1 in the environment of the test "
                               "run" % (name, engine_guard.LIVE_FILE_PATTERN,
                                        engine_guard.LIVE_GATE_VAR))
        value = os.environ.get(name)
        if value is not None:
            env[name] = value
            taken.append(name)
    if taken:
        with open(os.path.join(log_dir, INHERITED_FILE), "a",
                  encoding="utf-8") as fh:
            fh.write("\n".join(taken) + "\n")
    return env


def _display_env(owner, env, display):
    """Point env at one display the owner started: an XDisplay gives DISPLAY,
    a WaylandDisplay its socket, runtime folder and the X display behind it."""
    if display is None:
        return env
    if isinstance(display, WaylandDisplay):
        wayland = owner.wayland_display(display)
        env.update(DISPLAY=wayland.x_display.name,
                   WAYLAND_DISPLAY=wayland.socket,
                   XDG_RUNTIME_DIR=wayland.runtime_dir)
        return env
    env["DISPLAY"] = owner.x_display(display).name
    return env


# ---------------------------------------------------------------------------
# The owner


def _sweep_enabled():
    return os.environ.get(SWEEP_VAR, "1") != "0"


def _short_tmp_base():
    """A temporary folder short enough for a socket path under it."""
    base = tempfile.gettempdir()
    if len(base) <= TMP_BASE_MAX:
        return base
    return SHORT_TMP_BASE


class Owner:
    """The session's owner of displays, editors and their lifetimes.

    One per pytest process: the graphical conftests create it in a session
    fixture, so every row's processes are started, watched and ended the
    same way, and what a row leaves behind is found and named."""

    def __init__(self, out_dir, machine=None):
        self.out_dir = out_dir
        # What the machine's PATH holds (engine_guard.MachineTools); a test
        # of the guard passes a folder of its own.
        self._machine = machine or MACHINE_TOOLS
        self._farm = None
        self._farm_base = None
        self._row_node = None
        self._row_file = None
        self._row_log_start = 0
        self._charged = []
        self.stray_engine_hits = []
        self.ledger = Ledger()
        self.leaks = []
        self._x_displays = {}
        self._wayland = {}
        self._runtime_dirs = []
        self._x_runtime_dir = None
        self._ended_trees = []
        self._begun = False
        self._finished = False
        self._tool_home = None
        self._row_env = {}
        self._row_env_node = None
        self._row_leaks = []
        self._row_errors = []
        self.sweep_enabled = _sweep_enabled()

    # -- session lifetime ---------------------------------------------------

    def begin(self):
        """Start owning: orphans of this owner's processes come back here."""
        _acquire_subreaper()
        self._begun = True

    def finish(self):
        """End what is still owned, sweep once more, write the leak file.

        Returns the leaks found only now, after every row has ended: those
        no row reported, so the session fixture errors on them."""
        if self._finished:
            return []
        self._finished = True
        late = []
        for owned in self.ledger.entries():
            late.extend(owned.end_collect())
        late.extend(self._final_sweep())
        self._collect_stray_hits()
        self._remove_farm()
        self._remove_runtime_dirs()
        self._remove_tool_home()
        if self._begun:
            _release_subreaper()
            self._begun = False
        self.write_leak_file()
        return [leak for leak in late if not leak.declared]

    def write_leak_file(self):
        """Write tests/harness/out/leaks-<ts>.txt when anything leaked."""
        if not self.leaks:
            return None
        os.makedirs(self.out_dir, exist_ok=True)
        path = os.path.join(self.out_dir, "leaks-%s.txt"
                            % time.strftime("%Y%m%dT%H%M%S"))
        with open(path, "w", encoding="utf-8") as fh:
            for leak in self.leaks:
                fh.write(leak.describe() + "\n")
        return path

    # -- displays -----------------------------------------------------------

    def register_x_display(self, name, server):
        """Accept name as a display for editors, because server (a process
        this owner started) serves it."""
        self._require_owned(server)
        display = XDisplay(name, server)
        self._x_displays[name] = display
        return display

    def make_runtime_dir(self):
        """A private, short XDG_RUNTIME_DIR for one compositor (0700)."""
        path = tempfile.mkdtemp(prefix="xwpe-wl-", dir=_short_tmp_base())
        self._runtime_dirs.append(path)
        return path

    def runtime_dirs(self):
        """The runtime folders this owner made and has not removed yet."""
        return list(self._runtime_dirs)

    def register_wayland(self, runtime_dir, socket, compositor, x_display):
        """Accept the socket for editors: in a runtime folder this owner
        made, served by a compositor it started, with its own X display
        for a fallback."""
        self._require_owned(compositor)
        _check_socket_name(socket)
        if runtime_dir not in self._runtime_dirs:
            raise OwnerRefused("runtime folder %s was not made by this owner"
                               % runtime_dir)
        display = WaylandDisplay(runtime_dir, socket, compositor,
                                 self.x_display(x_display))
        if len(display.socket_path()) >= SOCKET_PATH_MAX:
            raise OwnerRefused("socket path %s is %d bytes, the limit is %d"
                               % (display.socket_path(),
                                  len(display.socket_path()), SOCKET_PATH_MAX))
        self._wayland[display.socket_path()] = display
        return display

    def x_display(self, display):
        """The XDisplay this owner started for display (object or name).

        Refuses anything else: the user's own session above all, but also a
        display some other suite or person started, which only looks free."""
        name = display.name if isinstance(display, XDisplay) else display
        if name and name == os.environ.get("DISPLAY"):
            raise OwnerRefused("display %s is this machine's session" % name)
        found = self._x_displays.get(name)
        if found is None or (isinstance(display, XDisplay) and found is not display):
            raise OwnerRefused("display %s was not started by this owner" % name)
        return found

    def wayland_display(self, display):
        """The WaylandDisplay this owner started, or a refusal."""
        if not isinstance(display, WaylandDisplay):
            raise OwnerRefused("%r is not a compositor this owner started"
                               % (display,))
        if display.runtime_dir.startswith(REAL_SESSION_RUNTIME):
            raise OwnerRefused("socket %s is in the machine's runtime folder"
                               % display.socket_path())
        if self._wayland.get(display.socket_path()) is not display:
            raise OwnerRefused("socket %s was not started by this owner"
                               % display.socket_path())
        return display

    # -- launching ----------------------------------------------------------

    def launch(self, argv, env, cwd, log_dir, kind, label, request=None,
               daemons=None, ladder_s=SERVICE_GRACE_S, pass_fds=(),
               detached_from_terminal=False, stdio=LOG, stdin_fd=None,
               controlling_tty=False, core_limit=None):
        """Start argv as an owned process and return it.

        core_limit sets the process's core file size limit before it runs
        (0: no core file); a row that ends a process with a crash signal on
        purpose passes 0 so the machine keeps no core of it.

        stdio says where the standard streams go.  LOG (the default): the
        output goes to the process's log and its input is empty.  PIPES:
        stdin and stdout are pipes the row talks to (proc.stdin,
        proc.stdout), stderr goes to the log.  An int: the slave side of a
        pty the row reads, as when it is started from a shell, so the row
        sees every byte it writes there.  stdin_fd replaces the standard
        input alone (a row that feeds a pipe and closes it).

        controlling_tty makes the pty the process's controlling terminal,
        as a shell does: the process is a session leader and takes the
        terminal on stdin (else stdout) with TIOCSCTTY, so a size set on the
        pty's master reaches it as SIGWINCH.  It keeps the session basis of
        the sweep, and it excludes detached_from_terminal.

        Every process of a row goes through here: its own process group
        (as a job started from a shell has), the death signal, its log in
        the row's folder, a watch and a ledger entry, and its end
        registered with the row before anything waits on it, so a failed
        start still ends what was started.

        It is NOT a session leader unless detached_from_terminal is true:
        a session leader has no controlling terminal and is the one that
        gets one by opening a terminal, which is how an editor started
        from a desktop launcher behaves, not how one started from a
        terminal does.  What the default loses: the sweep can no longer
        take a leftover by the editor's session id, only by the snapshot
        of its descendants and by the editor's own group, so a child that
        moved to a group of its own and was orphaned between two refreshes
        (or after the last one, before a crash) is not found.  A detached
        editor keeps the session basis.  A double-forked child that made a
        session of its own is found only from a snapshot, either way."""
        if threading.current_thread() is not threading.main_thread():
            raise OwnerRefused("the owner launches only from the main thread: "
                               "the death signal follows the thread that forks")
        if controlling_tty and detached_from_terminal:
            raise OwnerRefused("%s for %s: a process cannot be both detached "
                               "from the terminal and have one as its "
                               "controlling terminal" % (kind, label))
        self._guard_tools(argv, env, cwd, kind, label)
        log_path = _free_log_path(log_dir, kind)
        log = open(log_path, "wb")
        stage_read, stage_write = os.pipe()
        new_session = detached_from_terminal or controlling_tty
        try:
            stdin_arg, stdout_arg, stderr_arg = _stream_args(stdio, stdin_fd, log)
            proc = subprocess.Popen(argv, env=env, cwd=cwd, stdin=stdin_arg,
                                    stdout=stdout_arg, stderr=stderr_arg,
                                    start_new_session=new_session,
                                    process_group=None if new_session else 0,
                                    pass_fds=pass_fds,
                                    preexec_fn=_child_setup(os.getpid(),
                                                            controlling_tty,
                                                            stage_write,
                                                            core_limit))
        except subprocess.SubprocessError as error:
            raise OwnerRefused(_start_failure_text(kind, label,
                                                   _read_stage(stage_read))) from error
        finally:
            log.close()
            os.close(stage_read)
            os.close(stage_write)
        owned = OwnedProcess(self, proc, kind, label, log_path, daemons, ladder_s)
        self.ledger.add(owned)
        if request is not None:
            request.addfinalizer(owned.end)
        return owned

    def start_service(self, argv, label, log_dir, home, kind="service",
                      extra_env=None, request=None, pass_fds=(),
                      runtime_dir=None):
        """Start a display server, window manager or compositor.

        It gets the same whitelist as an editor without the editor's own
        variables, and a private HOME, so no user configuration (a
        weston.ini, a window manager's rc file) changes what rows see."""
        _check_extra_displays(self, extra_env)
        if runtime_dir is not None and runtime_dir not in self._runtime_dirs:
            raise OwnerRefused("runtime folder %s was not made by this owner"
                               % runtime_dir)
        env = _apply_extra(_base_env(home, runtime_dir), extra_env)
        return self.launch(argv, env, home, log_dir, kind, label,
                           request=request, pass_fds=pass_fds)

    def start_hogs(self, count, log_dir, label):
        """Start count busy processes, each owned and ended by its pid."""
        argv = [sys.executable, "-E", "-s", os.path.abspath(__file__), "--hog"]
        env = _base_env(log_dir, None)
        return [self.launch(argv, env, log_dir, log_dir, "hog", label)
                for _ in range(count)]

    # -- the row's view of the ledger ---------------------------------------

    def running(self, kind):
        """The owned processes of that kind not yet ended by their row.

        A row checks it before it starts its own editor, so one the row
        before left (running, or crashed and never ended) is named instead
        of found by accident."""
        return [owned for owned in self.ledger.entries() if owned.kind == kind]

    def entries_since(self, pids):
        """The owned processes that are not in pids (a set taken from
        ledger.pids() before a row): what the row started."""
        return [owned for owned in self.ledger.entries()
                if owned.pid not in pids]

    def end_since(self, pids):
        """End every process the row started since pids was taken, and error
        the row, naming them, when any left something behind.

        It is the row's last word for a process started outside a fixture
        (a driver built in the test body): the row's end is registered
        nowhere else.  What a driver's own close left (hold_for_row) is
        reported here too."""
        leaks = self._row_leaks
        self._row_leaks = []
        errors = self._row_errors
        self._row_errors = []
        for owned in self.entries_since(pids):
            found, exit_error = owned.end_and_judge()
            leaks.extend(found)
            if exit_error is not None:
                errors.append(exit_error)
        errors.extend(self._row_errors)
        self._row_errors = []
        self._raise_engine_hits()
        if errors:
            raise errors[0]
        if leaks:
            raise LeakedProcesses("the row left %d process(es) behind, now "
                                  "ended:\n  %s" % (len(leaks), "\n  ".join(
                                      leak.describe() for leak in leaks)))

    # -- the tool farm (engine_guard.py) -------------------------------------

    def begin_row(self, nodeid, path):
        """Say which row (and which file) runs now, before its fixtures start
        a process.  The live opt-out and the inheriting of an API key are
        allowed by the file's name, and the deny log is read from here."""
        self._row_node = nodeid
        self._row_file = str(path)
        self._row_log_start = self._farm.size() if self._farm else 0

    def row_is_live(self):
        """True when the running row is of a live file and the person running
        the suite opened the live gate in the test run's own environment."""
        return engine_guard.live_row_allowed(self._row_file, os.environ)

    def use_farm_base(self, folder):
        """Make the farm under @folder (the session's pytest basetemp: it is
        removed with the session and is outside every row's working folder).
        Takes effect only before the first launch made the farm.  The session's
        own working folder does not matter, so a --basetemp inside it is
        fine: a row's folder is its own tmp_path, a sibling of the farm, and
        launch refuses an explicit working folder that holds the farm
        (engine_guard.check_farm_outside)."""
        global _SESSION_BASETEMP
        _SESSION_BASETEMP = folder
        if self._farm is None:
            self._farm_base = folder

    def _farm_made(self):
        """The tool farm, made on first use in a private folder."""
        if self._farm is None:
            root = tempfile.mkdtemp(prefix="xwpe-farm-",
                                    dir=self._farm_base or _short_tmp_base())
            self._farm = engine_guard.ToolFarm(root, self._machine)
        return self._farm

    def _remove_farm(self):
        """Remove the farm, if one was made, by its exact path."""
        if self._farm is not None:
            _remove_tree(self._farm.root)
            self._farm = None

    def _guard_tools(self, argv, env, cwd, kind, label):
        """Give a process the PATH of the farm, in place in @env, or refuse
        the row (engine_guard.py says what is refused and why)."""
        try:
            self._apply_farm(argv, env, cwd)
        except engine_guard.GuardRefused as error:
            raise OwnerRefused("%s for %s: %s" % (kind, label, error)) from None

    def _apply_farm(self, argv, env, cwd):
        """The guard itself; it raises engine_guard.GuardRefused.  A row of a
        live file that sets REAL_ENGINE_VAR keeps the machine's PATH."""
        real_engine = env.pop(engine_guard.REAL_ENGINE_VAR, None)
        extras = engine_guard.take_tools(env)
        if real_engine is not None:
            if not self.row_is_live():
                raise engine_guard.GuardRefused(
                    "%s is allowed only to a row of a live file (%s) with %s=1 "
                    "in the environment of the test run"
                    % (engine_guard.REAL_ENGINE_VAR,
                       engine_guard.LIVE_FILE_PATTERN,
                       engine_guard.LIVE_GATE_VAR))
            return
        engine_guard.check_path_present(env)
        farm = self._farm_made()
        engine_guard.check_farm_outside(farm, cwd)
        scratch = [cwd or os.getcwd()]
        if env.get("HOME"):
            scratch.append(env["HOME"])
        engine_guard.check_host_command(env, farm, scratch)
        engine_guard.check_no_unprovided_program(argv, env, farm, extras)
        env["PATH"] = engine_guard.farm_path(env["PATH"], self._machine, farm,
                                             extras, scratch)

    def _charge_row_hits(self):
        """The lines the deny stubs wrote since the row began; they are
        charged to the row, so the session does not count them again."""
        if self._farm is None:
            return []
        lines, end = self._farm.read(self._row_log_start)
        self._charged.append((self._row_log_start, end))
        self._row_log_start = end
        return lines

    def _raise_engine_hits(self):
        """Fail the row that ran a program the farm denies, naming it."""
        lines = self._charge_row_hits()
        if not lines:
            return
        names = sorted(set(line.split("\t")[0] for line in lines))
        raise EngineReached(
            "row %s ran a program the tool farm denies (%s).  Put a stand-in "
            "first on the row's PATH, or declare the tool with %s.  Lines "
            "written while a module or session fixture's process ran count "
            "against the row running then.\n  %s"
            % (self._row_node, ", ".join(names), engine_guard.TOOLS_VAR,
               "\n  ".join(lines)))

    def _collect_stray_hits(self):
        """Keep the log lines no row was charged with (a stub run between two
        rows, or after the last), for the session fixture to fail on."""
        if self._farm is None:
            return
        with open(self._farm.log, "rb") as fh:
            data = fh.read()
        offset = 0
        for raw in data.splitlines(keepends=True):
            start, offset = offset, offset + len(raw)
            if raw.strip() and not self._is_charged(start):
                self.stray_engine_hits.append(
                    raw.decode("utf-8", "replace").rstrip())

    def _is_charged(self, offset):
        """True when a row was charged with the log byte at @offset."""
        return any(low <= offset < high for low, high in self._charged)

    def hold_error(self, error):
        """Keep an error found while a process was being ended (a driver's
        output handler raised), so the row's last word reports it; raising
        it where it was found would leave the process running."""
        self._row_errors.append(error)

    def hold_for_row(self, leaks):
        """Keep the leaks a driver found when it ended its editor, so the
        row's last word (end_since) reports them.

        A driver ends its editor inside a `with` block's exit, where a raised
        error would replace the failure the row itself had; it records here
        and the row errors, naming the leaks, when it ends."""
        self._row_leaks.extend(leaks)

    # -- the row's environment ----------------------------------------------

    def row_env(self, request):
        """The variables the row's fixtures give every editor it starts.

        A fixture writes here what it used to put in os.environ (a debugger
        interface, a trace file), and the launchers apply it under the row's
        own extra_env; the dict is emptied when the row ends, so nothing
        reaches the next row or the machine's environment."""
        if self._row_env_node != request.node.nodeid:
            self._row_env.clear()
            self._row_env_node = request.node.nodeid
            request.addfinalizer(self._clear_row_env)
        return self._row_env

    def _clear_row_env(self):
        """Forget the row's variables."""
        self._row_env.clear()
        self._row_env_node = None

    def merged_env(self, extra_env):
        """The row's variables (row_env) with extra_env over them."""
        merged = dict(self._row_env)
        merged.update(extra_env or {})
        return merged

    def tool_home(self):
        """A private folder, made on first use, that is the HOME and the log
        folder of every short tool a row runs."""
        if self._tool_home is None:
            self._tool_home = tempfile.mkdtemp(prefix=TOOL_HOME_PREFIX,
                                               dir=_short_tmp_base())
        return self._tool_home

    def _remove_tool_home(self):
        """Remove the tool folder, if one was made, by its exact path."""
        if self._tool_home is not None:
            _remove_tree(self._tool_home)
            self._tool_home = None

    # -- ending -------------------------------------------------------------

    def _require_owned(self, owned):
        """Refuse a server that is not a live process of this owner."""
        if not isinstance(owned, OwnedProcess) or owned.owner is not self \
                or owned.pid not in self.ledger.pids():
            raise OwnerRefused("%r is not a live process of this owner" % (owned,))

    def x_runtime_dir(self):
        """The private XDG_RUNTIME_DIR every X11 editor of the session uses."""
        if self._x_runtime_dir is None:
            self._x_runtime_dir = self.make_runtime_dir()
        return self._x_runtime_dir

    def note_ended(self, tree, leaks):
        """Keep an ended process's tree for the last sweep, and its leaks."""
        self._ended_trees.append(tree)
        self.leaks.extend(leaks)

    def _final_sweep(self):
        """Sweep every tree an ended row had, once more, at the end."""
        found = []
        for tree in self._ended_trees:
            found.extend(_sweep(self, tree, []))
        self.leaks.extend(found)
        return found

    def _remove_runtime_dirs(self):
        """Remove the runtime folders this owner made, and nothing else."""
        for path in self._runtime_dirs:
            _remove_tree(path)
        self._runtime_dirs = []
        self._x_runtime_dir = None

    def is_begun(self):
        """True between begin and finish: orphans come back to pytest."""
        return self._begun


def _remove_tree(path):
    """Remove one folder this owner made, by its exact path."""
    for root, dirs, files in os.walk(path, topdown=False):
        for name in files:
            os.unlink(os.path.join(root, name))
        for name in dirs:
            os.rmdir(os.path.join(root, name))
    os.rmdir(path)


def _stream_args(stdio, stdin_fd, log):
    """(stdin, stdout, stderr) for Popen, from the launcher's stdio."""
    if stdio == PIPES:
        stdin_arg, stdout_arg, stderr_arg = subprocess.PIPE, subprocess.PIPE, log
    elif stdio == LOG:
        stdin_arg, stdout_arg, stderr_arg = (subprocess.DEVNULL, log,
                                             subprocess.STDOUT)
    elif isinstance(stdio, int):
        stdin_arg = stdout_arg = stderr_arg = stdio
    else:
        raise OwnerRefused("stdio must be LOG, PIPES or a descriptor, not %r"
                           % (stdio,))
    if stdin_fd is not None:
        stdin_arg = stdin_fd
    return stdin_arg, stdout_arg, stderr_arg


def _free_log_path(log_dir, kind):
    """<log_dir>/<kind>.log, or <kind>-<n>.log when a row starts several."""
    path = os.path.join(log_dir, kind + ".log")
    count = 1
    while os.path.exists(path):
        count += 1
        path = os.path.join(log_dir, "%s-%d.log" % (kind, count))
    return path


# ---------------------------------------------------------------------------
# The tree a process had, and the sweep


class Tree:
    """What a process had under it right before it was ended: the pids of
    its descendants and every session among them (its own included)."""

    def __init__(self, root_pid, label):
        self.label = label
        self.pids = set(procinfo.descendants(root_pid))
        self.sids = set()
        for pid in self.pids | {root_pid}:
            sid = session_of(pid)
            if sid is not None:
                self.sids.add(sid)
        self.groups = {}
        self.started = {}
        for pid in self.pids:
            self.groups[pid] = (group_of(pid), session_of(pid))
            self.started[pid] = start_ticks(pid)
        # pytest's own session is never a candidate's, whatever a tree held.
        self.sids.discard(os.getsid(0))
        # The process's own group: an editor started as a terminal starts it
        # shares pytest's session, so a child it left in its group (a script
        # that asks the user something) is found by the group instead.
        self.pgids = set()
        root_group = group_of(root_pid)
        if root_group is not None and root_group != os.getpgid(0):
            self.pgids.add(root_group)
        self.closed_at = None

    def extend(self, pid):
        """Add what pid has under it now, before pid is killed."""
        for child in procinfo.descendants(pid):
            self.pids.add(child)
            self.groups[child] = (group_of(child), session_of(child))
            self.started[child] = start_ticks(child)
            sid = session_of(child)
            if sid is not None and sid != os.getsid(0):
                self.sids.add(sid)

    def close(self):
        """Mark the row's own sweep over.

        Nothing the tree left can start after this (its sessions were
        emptied), so a later sweep takes no process younger than the mark:
        such a process got a recorded number or session id only because the
        kernel reused it."""
        if self.closed_at is None:
            self.closed_at = now_ticks()

    def holds(self, pid, fields):
        """True when the process (pid, fields) is one this tree left.

        A recorded pid counts only with the start time the snapshot saw; a
        recorded session or the process's own group only for a process
        that started before the mark
        (the row's own sweep ended every member there was then)."""
        started = int(fields[STAT_STARTTIME])
        if pid in self.pids:
            return self.started.get(pid) == started
        if (int(fields[STAT_SESSION]) not in self.sids
                and int(fields[STAT_PGRP]) not in self.pgids):
            return False
        # Strictly before: a tick is 10 ms, and a process started in the
        # mark's own tick may already be one the kernel gave a reused number.
        return self.closed_at is None or started < self.closed_at


def _is_candidate(owner, tree, pid, fields):
    """A direct child of pytest that this tree left: in the snapshot or in
    one of its sessions, and neither a process the owner itself started nor
    the shared build server."""
    if pid in owner.ledger.pids():
        return False
    if not tree.holds(pid, fields):
        return False
    return not is_shared_server(pid)


def _candidates_linux(owner, tree):
    """The adopted processes a tree left, as (pid, fields)."""
    return [(pid, fields) for pid, fields in _direct_children_linux(os.getpid())
            if _is_candidate(owner, tree, pid, fields)]


def _end_adopted(tree, pid, fields, daemons):
    """SIGKILL one adopted leftover through its pidfd, reap it, describe it.

    A zombie (it ended on its own after it was adopted) is reaped and not
    reported; anything that was still running is."""
    comm = _stat_comm(pid)
    cwd = working_dir(pid)
    state = fields[STAT_STATE]
    watch = watch_process(pid)
    try:
        tree.extend(pid)
        watch.signal(signal.SIGKILL)
        if not watch.wait(scaled(KILL_WAIT_S)):
            raise EndFailed("pid %d (%s) left by %s survived SIGKILL"
                            % (pid, comm, tree.label))
        watch.reap()
    finally:
        watch.close()
    if state == ZOMBIE:
        return None
    return Leak(pid, comm, cwd, state, tree.label, comm in daemons)


def _let_leftovers_exit(owner, tree):
    """Give what a tree left a moment to end by itself.

    An editor that has just ended may have told its debugger or a child to
    stop, and that process is still on its way out when the sweep looks: it
    is no leak.  A process that is still running after LEFTOVER_GRACE_S
    (before the scale) is."""
    deadline = time.monotonic() + scaled(LEFTOVER_GRACE_S)
    while time.monotonic() < deadline:
        running = [pid for pid, fields in _candidates_linux(owner, tree)
                   if fields[STAT_STATE] != ZOMBIE]
        if not running:
            return
        time.sleep(DEFAULT_POLL_S)


def _sweep_linux(owner, tree, daemons):
    """End, level by level, everything a tree left with pytest."""
    _let_leftovers_exit(owner, tree)
    found = []
    for _ in range(SWEEP_MAX_ROUNDS):
        candidates = _candidates_linux(owner, tree)
        if not candidates:
            return found
        for pid, fields in candidates:
            leak = _end_adopted(tree, pid, fields, daemons)
            if leak is not None:
                found.append(leak)
    raise EndFailed("the sweep for %s did not converge in %d rounds"
                    % (tree.label, SWEEP_MAX_ROUNDS))


def _still_same(tree, pid):
    """True while pid is the process the snapshot saw (same group and
    session), so a reused pid is never signalled."""
    if procinfo.run_state(pid) in (procinfo.GONE, procinfo.ZOMBIE):
        return False
    return (group_of(pid), session_of(pid)) == tree.groups.get(pid)


def _sweep_ppid_walk(owner, tree, daemons):
    """Without a subreaper (macOS): end the snapshot's survivors by group.

    Orphans go to launchd, so they are found from the snapshot taken
    before the end, ended with killpg per group and watched by kqueue;
    launchd reaps them."""
    found = []
    for pid in sorted(tree.pids):
        if pid in owner.ledger.pids() or not _still_same(tree, pid):
            continue
        if is_shared_server(pid):
            continue
        comm = command_name(pid)
        state = procinfo.run_state(pid)
        watch = watch_process(pid)
        try:
            pgid = tree.groups[pid][0]
            if pgid is not None and pgid != os.getpgid(0):
                os.killpg(pgid, signal.SIGKILL)
            watch.signal(signal.SIGKILL)
            if not watch.wait(scaled(KILL_WAIT_S)):
                raise EndFailed("pid %d (%s) left by %s survived SIGKILL"
                                % (pid, comm, tree.label))
        finally:
            watch.close()
        found.append(Leak(pid, comm, None, state, tree.label, comm in daemons))
    return found


def _sweep(owner, tree, daemons):
    """The sweep for this platform; nothing when it was turned off."""
    if not owner.sweep_enabled:
        return []
    if IS_LINUX and owner.is_begun():
        return _sweep_linux(owner, tree, daemons)
    return _sweep_ppid_walk(owner, tree, daemons)


# ---------------------------------------------------------------------------
# An owned process


class OwnedProcess:
    """A process the owner started, as its ledger entry records it: pid,
    group, session, watch, kind, the row's label, start time and log; and
    how it is ended and swept."""

    def __init__(self, owner, proc, kind, label, log, daemons, ladder_s):
        self.owner = owner
        self.proc = proc
        self.pid = proc.pid
        self.pgid = group_of(proc.pid)
        self.sid = session_of(proc.pid)
        self.kind = kind
        self.label = label
        self.log = log
        self.started = time.time()
        self.watch = watch_process(proc.pid)
        self.daemons = daemons if daemons is not None else []
        self.ladder_s = ladder_s
        self.ended = False
        self.abrupt = False
        self.env = None
        # What is read while the editor is ended: (fd, callback) pairs, the
        # master side of the terminal an editor runs on first (Terminal.start).
        # The callback gets every chunk, so no byte is thrown away.
        self.drain_sources = []
        self.tree = Tree(self.pid, label)
        self.display_loss_expected = False
        self.exit_declared = False
        self.exit_code_declared = None
        self.crash_declared = None
        self.early_returncode = None
        self.final_returncode = None

    def expect_display_loss(self):
        """Declare that this row ends the editor's display on purpose.

        A row that stops its compositor or X server to see the editor save
        and exit has a display-loss line in the log by design; without this
        the log rule would fail it."""
        self.display_loss_expected = True

    def expect_exit(self, code=None):
        """Declare that this editor ends by itself with a non-zero exit.

        With code the exit must be exactly that code; without it any
        non-zero exit code is accepted.  A signal is never accepted, so a
        declared row still fails when the editor crashes.  A row that has an editor leave on its own with a
        failure code (a lost display, a refused save) says so here, so
        that a crash or an exit nobody asked for still fails the row."""
        self.exit_declared = True
        self.exit_code_declared = code

    def expect_crash(self, sig):
        """Declare that this editor is made to die of the signal sig.

        The row passes only when the editor was found dead of exactly that
        signal before the row ended it; an editor still running, or one that
        ended any other way, fails the row, so a crash a row provokes is
        proven to have happened.  The declaration must be made at start
        (Terminal.start, Editor.start: expect_crash=) so the editor is also
        started with no core file allowed."""
        self.crash_declared = signal.Signals(sig)

    def refresh_tree(self):
        """Note what the process has started so far, while it runs.

        An editor that crashes takes its own list of children with it, and
        one that moved a child to a session of its own is no longer found
        through the editor's session: a row calls this after each input it
        sends, so the sweep still knows that child when the editor is
        gone.  It does nothing once the process has ended."""
        if not self.ended and not self.watch.wait(0):
            self.tree.extend(self.pid)

    def reap_zombies(self):
        """Reap the ended processes of this editor that pytest adopted.

        pytest is the child subreaper, so a program the editor started and
        left orphaned comes to pytest, which must collect it when it ends:
        left alone it stays a zombie and a row that asks whether it is
        still there is told yes.  Only a zombie that is a direct child of
        pytest and is, by pid and start time, in this process's snapshot is
        collected, by its exact pid; nothing else is, and never with
        waitpid(-1), which would take the children of the row's own
        Popen.  Returns the pids reaped."""
        reaped = []
        if not (IS_LINUX and self.owner.is_begun()):
            return reaped
        for pid in sorted(self.tree.pids):
            if self._is_adopted_zombie(pid):
                try:
                    os.waitpid(pid, os.WNOHANG)
                except ChildProcessError:
                    continue
                reaped.append(pid)
        return reaped

    def _is_adopted_zombie(self, pid):
        """True when pid is a zombie child of pytest from this snapshot."""
        fields = _stat_fields(pid)
        if fields is None or fields[STAT_STATE] != ZOMBIE:
            return False
        if int(fields[STAT_PPID]) != os.getpid():
            return False
        return self.tree.started.get(pid) == int(fields[STAT_STARTTIME])

    def describe(self):
        """One line naming the process for a failure message."""
        return "%s pid %d started %s for %s, log %s" % (
            self.kind, self.pid,
            time.strftime("%H:%M:%S", time.localtime(self.started)),
            self.label, self.log)

    def running(self):
        """True until the process has ended.

        It looks through the watch and leaves an ended process unreaped, so
        its session id is still there when the row ends it: an editor that
        crashed is still known by the children it left in its session."""
        return not self.ended and not self.watch.wait(0)

    def log_text(self):
        """What the process has written to its log so far."""
        with open(self.log, "rb") as fh:
            return fh.read().decode("utf-8", "replace")

    def add_drain_source(self, fd, on_output=None):
        """Have fd read, and each chunk handed to on_output (when given),
        while the process is ended."""
        self.drain_sources.append([fd, on_output])

    def _hand_over(self, source, data):
        """Give a chunk to the source's callback.  A callback that raises is
        not called again, and its first error is held for the row's end
        (Owner.hold_error): the ladder, the sweep and the ledger must still
        finish, and an error here must not leave the editor running."""
        if source[1] is None:
            return
        try:
            source[1](data)
        except Exception as error:      # held, reported by the row's end
            source[1] = None
            self.owner.hold_error(OutputHandlerFailed(
                "the output handler for %s raised %s: %s"
                % (self.describe(), type(error).__name__, error)))

    def _drain_source(self, source, deadline, to_end=False):
        """Read the source until the deadline; True while it can still be
        read.  By default it stops when nothing is ready.  With to_end it
        waits for more, up to the deadline, until the end of the stream
        (EOF, or EIO on a terminal whose last writer left), so data still on
        its way from the writer to the reader is not missed."""
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return True
            try:
                ready = select.select([source[0]], [], [],
                                      left if to_end else 0)[0]
                data = os.read(source[0], TERMINAL_READ_CHUNK) if ready else None
            except (OSError, ValueError):
                return False
            if data is None:
                return True
            if not data:
                return False
            self._hand_over(source, data)

    def _drain_terminal(self, deadline, to_end=False):
        """Read what the editor has written to its terminal (and any other
        source the row named), handing every byte to its owner.

        An editor ending writes (it releases the screen, reports an
        emergency save), and a terminal nobody reads holds only so much: on
        a system whose terminals hold little the editor then blocks in that
        write and never reaches its end, and the ladder kills a healthy
        editor.  Reading while it ends lets it finish.  A source that
        cannot be read any more is dropped.  The reading stops at the
        ladder's deadline, so a flood cannot hold the ladder."""
        self.drain_sources = [
            source for source in self.drain_sources
            if self._drain_source(source, deadline, to_end)]

    def _drain_last(self):
        """Read each source once more, now that the process has ended.

        What an editor writes just before it exits (the escape that leaves
        the alternate screen, the last dump) is still in the terminal or the
        pipe when the process is seen to end; this takes it, until the end
        of the stream or a short deadline (a grandchild that still holds the
        terminal costs that deadline and no more), so the driver that keeps
        the screen has all of it."""
        deadline = time.monotonic() + scaled(FINAL_DRAIN_S)
        self._drain_terminal(deadline)      # everything ready, from every source
        self._drain_terminal(deadline, to_end=True)

    def _wait_ended(self, timeout_s):
        """True when the process ended within timeout_s, its terminal read
        meanwhile (see _drain_terminal) and once more after it ended."""
        ended = self._wait_ended_reading(timeout_s)
        if ended:
            self._drain_last()
        return ended

    def _wait_ended_reading(self, timeout_s):
        """The wait itself: True once the process ended, its sources read
        every TERMINAL_DRAIN_POLL_S."""
        if not self.drain_sources:
            return self.watch.wait(timeout_s)
        deadline = time.monotonic() + timeout_s
        while True:
            self._drain_terminal(deadline)
            left = deadline - time.monotonic()
            if left <= 0:
                return self.watch.wait(0)
            if self.watch.wait(min(left, TERMINAL_DRAIN_POLL_S)):
                return True

    def _ladder(self):
        """SIGTERM, the grace, then SIGKILL; True when SIGKILL was needed."""
        self.watch.signal(signal.SIGTERM)
        if self._wait_ended(scaled(self.ladder_s)):
            return False
        self.watch.signal(signal.SIGKILL)
        if not self.watch.wait(scaled(KILL_WAIT_S)):
            raise EndFailed("%s did not end after SIGKILL" % self.describe())
        self._drain_last()
        return True

    def end_collect(self):
        """End the process and sweep what it left; return the leaks.

        The snapshot of the tree is taken right before SIGTERM, so a child
        the process started in its own session is still known after the
        process is gone."""
        if self.ended:
            return []
        self.ended = True
        tree = self.tree
        tree.extend(self.pid)
        try:
            if self.watch.wait(0):
                self.early_returncode = self.proc.wait()
            self.abrupt = self._ladder()
            self.final_returncode = self.proc.wait()
        finally:
            self.watch.close()
            self.owner.ledger.forget(self.pid)
        self.reap_zombies()
        leaks = _sweep(self.owner, tree, self.daemons)
        tree.close()
        self.owner.note_ended(tree, leaks)
        return leaks

    def end(self):
        """End the process and error the row when it left anything.

        Registered as the row's finalizer by the launcher, so it runs even
        when the row failed before its first wait was over.  Calling it a
        second time does nothing."""
        first = not self.ended
        leaks, error = self.end_and_judge()
        if leaks:
            raise LeakedProcesses("%s left %d process(es) behind, now ended:\n  %s"
                                  % (self.label, len(leaks),
                                     "\n  ".join(l.describe() for l in leaks)))
        if first and self.kind == "xwpe":
            self._check_display_log()
        if error is not None:
            raise error

    def end_for_row(self):
        """End the process and leave what it left to the row's own end.

        For a driver that ends its editor where an error would hide the
        row's own failure (the exit of a `with` block): the leaks are kept
        (Owner.hold_for_row) and a bad exit is kept too (Owner.hold_error),
        and the row errors, naming them, when it ends.  Returns the leaks."""
        leaks, error = self.end_and_judge()
        self.owner.hold_for_row(leaks)
        if error is not None:
            self.owner.hold_error(error)
        return leaks

    def end_and_judge(self):
        """End the process once; return (undeclared leaks, exit error or None).

        The one place an end is judged, for every way a row ends an editor
        (end, end_for_row, Owner.end_since): the exit is read only on the
        first end, so a second call returns nothing to report."""
        first = not self.ended
        leaks = [leak for leak in self.end_collect() if not leak.declared]
        error = self.exit_error() if first else None
        return leaks, error

    def exit_error(self):
        """The error for how this editor ended, or None when it ended well.

        A declared crash (expect_crash) is judged by that alone: the editor
        must have been found dead of exactly that signal.  Otherwise a
        graphical editor (kind xwpe) fails for a crash signal, a failure
        code nobody declared, or any signal but SIGTERM and SIGHUP while it
        was found already dead.  A terminal editor (kind wpe) fails for a
        crash signal only, whether it died by itself or while the owner's
        ladder ended it.  The terminal rule is narrower on purpose: terminal
        rows end the editor with SIGKILL, and with the loss of its input
        (exit code 1), as a way of testing it, and no row has been audited
        for its exit codes.  Out of scope, its own class: auditing terminal
        rows for exit codes; revisit with the first terminal row that hides a
        wrong exit code.  A process of any other kind is not judged."""
        if self.crash_declared is not None:
            return self._declared_crash_error()
        if self.kind == "xwpe":
            return self._graphical_exit_error()
        if self.kind == "wpe":
            return self._terminal_exit_error()
        return None

    def _declared_crash_error(self):
        """The error when the editor did not die of the declared signal
        before the row ended it, else None."""
        wanted = self.crash_declared
        if self.early_returncode == -wanted:
            return None
        if self.early_returncode is None:
            what = "was still running when the row ended it"
        else:
            what = unexpected_exit_text(self.early_returncode) or "exited by itself"
        return UnexpectedExit("%s: expected %s, the editor %s (log %s)"
                              % (self.label, wanted.name, what, self.log))

    def _terminal_exit_error(self):
        """The error when a terminal editor died of a crash signal."""
        for code in (self.early_returncode, self.final_returncode):
            if code is not None and code < 0 and -code in CRASH_SIGNALS:
                return UnexpectedExit("%s: the editor killed by %s (log %s)"
                                      % (self.label,
                                         signal.Signals(-code).name, self.log))
        return None

    def _graphical_exit_error(self):
        """The error when a graphical editor ended by itself and badly.

        An editor found already ended before the row ended it crashed or
        left with a failure code unless the row declared that exit (a
        declaration accepts a non-zero code, never a signal); a quiet exit
        0 or a SIGTERM the row sent is how rows end it.  An editor the
        owner's ladder ended is flagged only when it died of a crash
        signal."""
        if self.early_returncode is not None:
            return self._early_exit_error()
        what = unexpected_exit_text(self.final_returncode, by_ladder=True)
        if what is None:
            return None
        return UnexpectedExit("%s: the editor %s while the owner "
                              "ended it (log %s)" % (self.label, what, self.log))

    def _check_display_log(self):
        """Error the row when the editor's log reports a lost display.

        It protects a row from passing over an editor whose compositor or
        X server cut it off: that editor has no window and a row that only
        looks at the file or at "still running" would be green."""
        if self.display_loss_expected:
            return
        try:
            lines = display_errors(self.log_text())
        except OSError:
            return
        if lines:
            raise DisplayProtocolError(
                "%s: the editor's log reports a lost display:\n  %s\n(log %s)"
                % (self.label, "\n  ".join(lines), self.log))

    def _early_exit_error(self):
        """The rule for an editor that was already dead when the row ended it."""
        code = self.early_returncode
        what = unexpected_exit_text(code)
        if what is None:
            return None
        if self.exit_declared and code > 0 and self.exit_code_declared in (
                None, code):
            return None
        return UnexpectedExit("%s: the editor %s before the row ended it "
                              "(log %s)" % (self.label, what, self.log))


# ---------------------------------------------------------------------------
# The launchers rows call


SELF_CRASH_VAR = "XWPE_TEST_CRASH"
NO_CORE_BYTES = 0


def _core_limit_for(expect_crash):
    """The core file limit an editor starts with: none when the row makes
    it crash on purpose, else the machine's own."""
    return NO_CORE_BYTES if expect_crash is not None else None


def _declare_crash(owned, expect_crash):
    """Record on the owned editor the crash its row declared, if any."""
    if expect_crash is not None:
        owned.expect_crash(expect_crash)


def _refuse_undeclared_self_crash(env, expect_crash, kind, label):
    """Refuse to start an editor told to crash itself by a row that did not
    declare the crash, so a self-crash can never leave a core nobody
    expected."""
    if env.get(SELF_CRASH_VAR) and expect_crash is None:
        raise OwnerRefused("%s for %s: %s makes the editor crash itself; "
                           "the row must start it with expect_crash="
                           % (kind, label, SELF_CRASH_VAR))


class Editor:
    """The one way a graphical row starts xwpe."""

    @staticmethod
    def start(owner, binary, args, backend, display, cwd, home, log_dir=None,
              extra_env=None, label="", request=None, daemons=None, dump=None,
              detached_from_terminal=False, stdio=LOG, inherit=(),
              expect_crash=None):
        """Start the editor on a display this owner started.

        expect_crash (a signal) declares that the row makes the editor die
        of it; the editor is then started with no core file allowed, and the
        row fails unless it died of exactly that signal (see
        OwnedProcess.expect_crash).  The variable that makes the editor
        crash itself (XWPE_TEST_CRASH) is refused without it.

        With stdio an int (the slave side of a pty, see Owner.launch) the
        editor's standard streams are that terminal, so a row can read what
        a graphical editor writes to the terminal it was started from.

        With detached_from_terminal the editor is a session leader with no
        controlling terminal, as when a desktop launcher starts it; by
        default it is started as a terminal would start it.  The backend is pinned (x11 or wayland), the display and the
        fallback display are the owner's, the environment is the whitelist
        plus extra_env, and the editor's end ladder is waited for before
        it is called abrupt.  A row benefits by never opening on the
        user's desktop and never leaving a process behind unnoticed."""
        if backend not in BACKENDS:
            raise OwnerRefused("backend must be one of %s, not %r"
                               % (", ".join(BACKENDS), backend))
        extra_env = owner.merged_env(extra_env)
        _check_extra_displays(owner, extra_env)
        log_dir = log_dir or home
        if dump is False and backend != "x11":
            raise OwnerRefused("a Wayland editor needs its frame dump")
        env = _editor_env(_backend_env(owner, backend, display, home))
        if dump is not False:
            _add_dump(env, backend, dump or os.path.join(log_dir, "frame.ppm"))
        _inherit(owner, env, inherit, log_dir)
        _apply_extra(env, extra_env)
        env["XWPE_BACKEND"] = backend
        _refuse_undeclared_self_crash(env, expect_crash, "xwpe", label)
        owned = owner.launch([binary] + list(args), env, cwd, log_dir, "xwpe",
                             label, request=request, daemons=daemons,
                             ladder_s=editor_end_ladder_s(),
                             detached_from_terminal=detached_from_terminal,
                             stdio=stdio,
                             core_limit=_core_limit_for(expect_crash))
        _declare_crash(owned, expect_crash)
        owned.env = env
        return owned


class Client:
    """The one way a graphical row starts a helper client (xlogo, a probe)."""

    @staticmethod
    def start(owner, argv, display, cwd, home, log_dir=None, extra_env=None,
              label="", request=None, backend="x11"):
        """Start a client on a display this owner started, with the same
        whitelist as an editor but none of the editor's own variables."""
        _check_extra_displays(owner, extra_env)
        env = _apply_extra(_backend_env(owner, backend, display, home), extra_env)
        owned = owner.launch(list(argv), env, cwd, log_dir or home, "client",
                             label, request=request)
        owned.env = env
        return owned


def _backend_env(owner, backend, display, home):
    """The base environment pinned to one of the owner's displays."""
    if backend == "x11":
        x_display = owner.x_display(display)
        env = _base_env(home, owner.x_runtime_dir())
        env["DISPLAY"] = x_display.name
        return env
    wayland = owner.wayland_display(display)
    env = _base_env(home, wayland.runtime_dir)
    env["WAYLAND_DISPLAY"] = wayland.socket
    env["DISPLAY"] = wayland.x_display.name
    return env


def _add_dump(env, backend, dump):
    """Where the editor mirrors its frames, and the Wayland window size."""
    if backend == "wayland":
        env.update(XWPE_WL_WIDTH=WL_WIDTH, XWPE_WL_HEIGHT=WL_HEIGHT,
                   XWPE_WL_DUMP=dump)
    else:
        env["XWPE_X_DUMP"] = dump


# ---------------------------------------------------------------------------
# The editor on a terminal, and the short tools


def set_pty_size(master_fd, cols, rows):
    """Set the window size of the pty behind master_fd (TIOCSWINSZ), so a
    process started on it afterwards inherits that size.  The terminal's own
    ioctl number is used: they differ by architecture."""
    winsize = struct.pack(PTY_WINSIZE_FORMAT, rows, cols, 0, 0)
    fcntl.ioctl(master_fd, termios.TIOCSWINSZ, winsize)


def _terminal_editor_env(owner, home, log_dir, extra_env, inherit):
    """The environment of an editor with no display: the whitelist, the
    terminal's own variables, what the row inherited, then the row's own."""
    merged = owner.merged_env(extra_env)
    _check_extra_displays(owner, merged)
    env = _terminal_env(_base_env(home, owner.x_runtime_dir()))
    _inherit(owner, env, inherit, log_dir)
    _pass_traces(env, merged)
    return _apply_extra(env, merged)


def _pass_traces(env, merged):
    """Give the editor the trace files the machine's environment names, for
    a developer or a CI step that asks for a trace of a whole run; a trace
    the row sets itself (row_env or extra_env) wins."""
    for name in TRACE_PASS_THROUGH:
        value = os.environ.get(name)
        if value is not None and name not in merged:
            env[name] = value


class Terminal:
    """The one way a test starts the editor on a terminal."""

    @staticmethod
    def start(owner, binary, args, cwd, home, master_fd, slave_fd,
              size=(COLS, ROWS), stdin_fd=None, extra_env=None, inherit=(),
              label="", request=None, daemons=None, log_dir=None,
              pass_fds=(), on_output=None, extra_drains=(),
              expect_crash=None):
        """Start the editor on the pty (master_fd, slave_fd) and return the
        owned process; its .proc is the Popen a driver exposes.

        expect_crash (a signal) declares that the row makes the editor die
        of it: the editor is started with no core file allowed (the machine
        keeps no dump of a crash the row provoked), and the row fails unless
        the editor died of exactly that signal before the row ended it.
        Without it the row fails when the editor dies of any crash signal,
        and the variable that makes the editor crash itself (XWPE_TEST_CRASH)
        is refused.

        While the editor is ended the owner reads the pty's master, and each
        (fd, callback) of extra_drains, so a full terminal or pipe never
        blocks an editor that is leaving; on_output gets every chunk read
        from the master, so a driver that keeps the screen or the raw bytes
        loses none of them.

        The pty gets its size before the start so the editor inherits it
        (a row never sizes a terminal by LINES or COLUMNS), the slave side
        is the editor's standard streams and its controlling terminal (a
        later resize reaches it as SIGWINCH), and the environment is the
        whitelist plus TERM, the checkout's data and the row's own variables.
        There is no display variable, so nothing the editor starts (a
        debugger, a shell, a terminal window) can reach the user's session.
        A row that needs one more variable of the machine names it in
        inherit."""
        log_dir = log_dir or home
        env = _terminal_editor_env(owner, home, log_dir, extra_env, inherit)
        set_pty_size(master_fd, *size)
        _refuse_undeclared_self_crash(env, expect_crash, "wpe", label)
        owned = owner.launch([binary] + list(args), env, cwd, log_dir, "wpe",
                             label, request=request, daemons=daemons,
                             ladder_s=editor_end_ladder_s(), pass_fds=pass_fds,
                             stdio=slave_fd, stdin_fd=stdin_fd,
                             controlling_tty=True,
                             core_limit=_core_limit_for(expect_crash))
        _declare_crash(owned, expect_crash)
        owned.env = env
        owned.add_drain_source(master_fd, on_output)
        for fd, callback in extra_drains:
            owned.add_drain_source(fd, callback)
        return owned

    @staticmethod
    def run(owner, binary, args, timeout, extra_env=None, inherit=(),
            cwd=None, home=None, label="", input=None):
        """Run the editor to completion (a `--version`, a batch mode) with
        the editor's environment and no terminal; return a CompletedProcess
        whose stderr is what the editor wrote to its log.  A run that
        outlives its deadline is ended and the row fails naming it."""
        home = home or owner.tool_home()
        env = _terminal_editor_env(owner, home, home, extra_env, inherit)
        return _run_bounded(owner, [binary] + list(args), env, cwd, home,
                            "wpe-run", label or os.path.basename(binary),
                            timeout, input, False)


def tool_env(display=None, inherit=(), home=None, extra_env=None):
    """The environment of a short tool a row runs to completion.

    The whitelist with the locale, HOME the owner's private tool folder
    (or the home given) and, for a display the owner started, DISPLAY (and
    for a Wayland one its socket and runtime folder).  Never a copy of
    os.environ: a row that needs one more variable of the machine names it
    in inherit."""
    owner = current()
    home = home or owner.tool_home()
    merged = dict(extra_env or {})
    _check_extra_displays(owner, merged)
    env = _display_env(owner, _base_env(home, None), display)
    _inherit(owner, env, inherit, home)
    return _apply_extra(env, merged)


def _run_bounded(owner, argv, env, cwd, log_dir, kind, label, timeout, input,
                 check):
    """Start argv as an owned process, wait for it at most timeout (scaled),
    end it and return what it printed.  A tool that outlives its deadline is
    ended and the row fails naming the tool and the deadline; one that left
    a process behind errors the row, as an editor does."""
    owned = owner.launch(argv, env, cwd, log_dir, kind, label, stdio=PIPES)
    deadline = scaled(timeout)
    try:
        out, _ = owned.proc.communicate(input=input, timeout=deadline)
    except subprocess.TimeoutExpired:
        owned.end_collect()
        _close_pipes(owned.proc)
        raise OwnerTimeout("%s ran past its deadline of %.1f s (scale %.1f)"
                           % (label, deadline, wait_scale())) from None
    except BaseException:
        owned.end_collect()
        _close_pipes(owned.proc)
        raise
    owned.end()
    return _finish_run(argv, owned, out, check)


def _close_pipes(proc):
    """Close the row's ends of a process's pipes."""
    for pipe in (proc.stdin, proc.stdout):
        if pipe is not None:
            pipe.close()


def _finish_run(argv, owned, out, check):
    """The CompletedProcess of an ended tool; its log (stderr) is kept only
    when it holds something."""
    code = owned.proc.returncode
    with open(owned.log, "rb") as fh:
        errors = fh.read()
    if not errors:
        os.unlink(owned.log)
    if check and code != 0:
        raise subprocess.CalledProcessError(code, argv, out, errors)
    return subprocess.CompletedProcess(argv, code, out, errors)


def run_tool(argv, timeout, display=None, input=None, cwd=None, inherit=(),
             check=False, home=None, extra_env=None):
    """Run a short tool (xdotool, xwd, git, a probe) to completion.

    The one door for tools: the tool is on the owner's ledger, bounded by
    timeout (scaled), its environment comes from tool_env (home= and
    extra_env= for a probe that sets HOME or PATH for a canary), stdout is
    returned and stderr goes to the row's log.  A row benefits by never
    hanging on a tool and by never leaking the machine's session to one."""
    owner = current()
    env = tool_env(display, inherit, home, extra_env)
    return _run_bounded(owner, list(argv), env, cwd, owner.tool_home(), "tool",
                        os.path.basename(str(argv[0])), timeout, input, check)


def run_nested(argv, timeout, input=None, cwd=None, check=False,
               extra_env=None):
    """Run another python or pytest of the harness to completion: like
    run_tool, with the names of HARNESS_VARS passed from the machine, which
    the inner run needs to find the editor and scale its waits."""
    return run_tool(argv, timeout, input=input, cwd=cwd, check=check,
                    inherit=HARNESS_VARS, extra_env=extra_env)


# ---------------------------------------------------------------------------
# The display servers, moved here from the graphical conftests


def _display_number(fd, chunks):
    """The display number the server wrote to fd, once its line is whole."""
    if not select.select([fd], [], [], 0)[0]:
        return None
    data = os.read(fd, DISPLAY_LINE_MAX)
    chunks.append(data)
    text = b"".join(chunks)
    if not data or not text.endswith(b"\n"):
        return None
    return int(text.strip())


def start_x_server(owner, request, log_dir, screen=SCREEN):
    """Start Xvfb on the display number it picks; return the owner's record
    of that display.

    The server writes its number to the pipe once it is listening, so the
    read is the ready event: no fixed number, no lock file looked at or
    removed, and two runs at once never meet on one display."""
    read_fd, write_fd = os.pipe()
    try:
        server = owner.start_service(
            ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", screen,
             "-nolisten", "tcp"],
            "Xvfb of the test run", log_dir, log_dir, kind="server",
            request=request, pass_fds=(write_fd,))
    finally:
        os.close(write_fd)
    chunks = []
    try:
        number = wait_for(lambda: _display_number(read_fd, chunks),
                          "Xvfb to print its display number", SERVER_START_S,
                          state_fn=server.log_text)
    finally:
        os.close(read_fd)
    return owner.register_x_display(":%d" % number, server)


def start_compositor(owner, request, x_display, log_dir, width, height):
    """Start weston on x_display (the owner's X server) in a runtime folder
    of its own, and return the registered WaylandDisplay once its socket is
    there.

    weston's x11 backend takes its keyboard and pointer from the X server,
    so a synthetic key sent to that server reaches the editor as a wl_keyboard
    event.  kiosk-shell fullscreens the single client at the output's (0,0)
    with no panel, so the editor's surface origin is the weston window's
    origin and a click relative to that window lands on the matching cell
    (the default desktop shell centres the surface and breaks that).  The
    socket is named after this process, in a short folder the owner removes."""
    runtime = owner.make_runtime_dir()
    socket = "wl-%d" % os.getpid()
    compositor = owner.start_service(
        ["weston", "--backend=x11-backend.so", "--shell=kiosk-shell.so",
         "--width=%d" % width, "--height=%d" % height, "--socket=" + socket,
         "--idle-time=0"],
        "weston of the test run", log_dir, log_dir, kind="compositor",
        extra_env={"DISPLAY": x_display.name}, request=request,
        runtime_dir=runtime)
    path = os.path.join(runtime, socket)
    wait_for(lambda: os.path.exists(path),
             "weston's socket %s to appear" % path, COMPOSITOR_START_S,
             state_fn=compositor.log_text)
    return owner.register_wayland(runtime, socket, compositor, x_display)


# ---------------------------------------------------------------------------
# The process's owner, for code that is not a fixture

_CURRENT = None


class PreviousRowLeft(AssertionError):
    """A row started while the one before it had left something behind."""


def install(owner):
    """Make owner the one current() returns (the session fixture does it);
    None puts nothing there, so the next current() begins a new owner."""
    global _CURRENT
    _CURRENT = owner


def _finish_at_exit(owner):
    """End what a lazily made owner still owns when the process ends."""
    late = owner.finish()
    if late:
        sys.stderr.write("left at the end, now ended:\n  %s\n"
                         % "\n  ".join(leak.describe() for leak in late))


def current():
    """The process's owner: the one a session fixture installed, or, outside
    pytest (a script, the standalone smoke), one made and begun now and
    finished when the process ends.  A driver built in a test body uses it,
    so the files that build one themselves need no fixture."""
    if _CURRENT is None:
        owner = Owner(OUT_DIR)
        owner.begin()
        atexit.register(_finish_at_exit, owner)
        install(owner)
    return _CURRENT


# ---------------------------------------------------------------------------
# Fixtures and the configure helper


OUT_DIR = os.path.join(HERE, "harness", "out")


@pytest.fixture(scope="session")
def gui_owner():
    """The session's owner; errors the session on leaks no row reported.

    It is the owner current() returns while the session runs; when another
    fixture already installed one, that one is used and left to its maker."""
    if _CURRENT is not None:
        yield _CURRENT
        return
    owner = Owner(OUT_DIR)
    owner.begin()
    install(owner)
    yield owner
    install(None)
    late = owner.finish()
    if late:
        raise LeakedProcesses("left at the session's end, now ended:\n  %s"
                              % "\n  ".join(l.describe() for l in late))


@pytest.fixture
def owner_daemons():
    """The command names a row expects to leave running (a build server).

    A row appends to it; its editor's sweep then ends those without an
    error, and still errors on anything else."""
    return []


def load_count():
    """How many busy processes XWPE_TEST_LOAD asks for (0 when unset)."""
    text = os.environ.get(LOAD_VAR, "") or "0"
    return int(text)


@pytest.fixture
def load(gui_owner, tmp_path):
    """XWPE_TEST_LOAD=<n> busy processes for the row's duration.

    A row that has failed only on a loaded host is run with it to fail
    the same way here, and again after its fix to show it holds."""
    hogs = gui_owner.start_hogs(load_count(), str(tmp_path), "load")
    yield hogs
    for hog in hogs:
        hog.end()


def reject_reruns(config):
    """Refuse a run that would rerun failed rows.

    A graphical conftest calls it from pytest_configure: a rerun turns a
    row that fails under load into a pass, which hides the very failure
    the owner exists to name."""
    if (config.getoption("reruns", 0) or 0) > 0:
        raise pytest.UsageError("--reruns is refused: a failing graphical row "
                                "must fail, not be retried")


# ---------------------------------------------------------------------------
# Running the module itself


def _busy_loop():
    """Spin until killed: one CPU's worth of load."""
    while True:
        pass


def _report_subreaper():
    """Print the subreaper flag after import, after begin, after finish."""
    after_import = subreaper_set()
    owner = Owner(tempfile.gettempdir())
    owner.begin()
    after_begin = subreaper_set()
    owner.finish()
    print("import %d begin %d finish %d"
          % (after_import, after_begin, subreaper_set()))


def _report_current():
    """Print whether current() made a begun owner outside pytest and gives
    the same one again; the owner is finished when the process ends."""
    first = current()
    print("current begun %d same %d" % (first.is_begun(), current() is first))


if __name__ == "__main__":
    if sys.argv[1:] == ["--hog"]:
        _busy_loop()
    elif sys.argv[1:] == ["--report-subreaper"]:
        _report_subreaper()
    elif sys.argv[1:] == ["--report-current"]:
        _report_current()
    else:
        sys.stderr.write("usage: gui_owner.py --hog | --report-subreaper "
                         "| --report-current\n")
        sys.exit(2)
