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

Only /proc/<pid>/stat and /proc/<pid>/cwd of other processes are read,
never their command lines or environments.
"""
import ctypes
import os
import re
import select
import signal
import subprocess
import sys
import tempfile
import threading
import time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import procinfo  # noqa: E402  (needs HERE on the path)

IS_LINUX = sys.platform.startswith("linux")
IS_MACOS = sys.platform == "darwin"

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
# The terminal size variables would override the window's own size.
NEVER_PASSED = ("LINES", "COLUMNS")


class OwnerTimeout(AssertionError):
    """A named event did not happen within its scaled deadline."""


class OwnerRefused(RuntimeError):
    """The launcher refused to start something it could not own safely."""


class LeakedProcesses(AssertionError):
    """A row ended and left processes behind; the sweep ended them."""


class EndFailed(AssertionError):
    """A process the owner started did not end even after SIGKILL."""


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


def now_ticks():
    """The current time in the same clock ticks since boot (Linux)."""
    return int(time.clock_gettime(time.CLOCK_BOOTTIME) * os.sysconf("SC_CLK_TCK"))


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


def _child_setup(parent_pid):
    """Return the function a new child runs before exec.

    On Linux it asks for SIGTERM when pytest's main thread dies, so a
    pytest that was killed takes its displays and editors with it, and it
    gives up at once if pytest died before the request was in place.  When
    the request is refused the launch fails: a process that would outlive a
    killed pytest is never started."""
    prctl = _LIBC.prctl if IS_LINUX else None

    def setup():
        if prctl is not None \
                and prctl(PR_SET_PDEATHSIG, signal.SIGTERM, 0, 0, 0) != 0:
            raise OSError(ctypes.get_errno(), "prctl(PR_SET_PDEATHSIG)")
        if os.getppid() != parent_pid:
            os._exit(1)
    return setup


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


def watch_process(pid):
    """A watch on pid for this platform (pidfd, else kqueue)."""
    if hasattr(os, "pidfd_open"):
        return _PidfdWatch(pid)
    return _KqueueWatch(pid)


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


def _check_home(home):
    """Refuse the machine's own home folder as a process's HOME.

    With XDG_CONFIG_HOME empty the editor reads its settings and keys from
    HOME, so a row given the real one would run on the user's configuration
    and could change it."""
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
        _check_home(extra_env["HOME"])
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
    """The row's own variables last, never the terminal size ones."""
    for name, value in (extra_env or {}).items():
        if name not in NEVER_PASSED:
            env[name] = value
    for name in NEVER_PASSED:
        env.pop(name, None)
    return env


def _editor_env(env):
    """What every editor gets on top of the base: the checkout's data, a
    known font size, no eager language server."""
    env.update(XWPE_LIB=TOP, XWPE_FONT_SIZE=FONT_SIZE, XWPE_LSP_NO_EAGER="1")
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

    def __init__(self, out_dir):
        self.out_dir = out_dir
        self.ledger = Ledger()
        self.leaks = []
        self._x_displays = {}
        self._wayland = {}
        self._runtime_dirs = []
        self._x_runtime_dir = None
        self._ended_trees = []
        self._begun = False
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
        late = []
        for owned in self.ledger.entries():
            late.extend(owned.end_collect())
        late.extend(self._final_sweep())
        self._remove_runtime_dirs()
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
               detached_from_terminal=False, terminal_fd=None):
        """Start argv as an owned process and return it.

        With terminal_fd (the slave side of a pty the row reads) the
        process's standard streams are that terminal, as when it is
        started from a shell, so the row sees every byte it writes there;
        otherwise its output goes to its log and its input is empty.

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
        log_path = _free_log_path(log_dir, kind)
        log = open(log_path, "wb")
        stdin_arg, stdout_arg, stderr_arg = (subprocess.DEVNULL, log,
                                             subprocess.STDOUT)
        if terminal_fd is not None:
            stdin_arg = stdout_arg = stderr_arg = terminal_fd
        try:
            proc = subprocess.Popen(argv, env=env, cwd=cwd, stdin=stdin_arg,
                                    stdout=stdout_arg, stderr=stderr_arg,
                                    start_new_session=detached_from_terminal,
                                    process_group=None if detached_from_terminal else 0,
                                    pass_fds=pass_fds,
                                    preexec_fn=_child_setup(os.getpid()))
        except subprocess.SubprocessError as error:
            raise OwnerRefused("%s for %s was not started: its death signal "
                               "(PR_SET_PDEATHSIG) could not be set"
                               % (kind, label)) from error
        finally:
            log.close()
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
    one of its sessions, and not a process the owner itself started."""
    if pid in owner.ledger.pids():
        return False
    return tree.holds(pid, fields)


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
    watch = _PidfdWatch(pid)
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
        self.tree = Tree(self.pid, label)

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

    def _ladder(self):
        """SIGTERM, the grace, then SIGKILL; True when SIGKILL was needed."""
        self.watch.signal(signal.SIGTERM)
        if self.watch.wait(scaled(self.ladder_s)):
            return False
        self.watch.signal(signal.SIGKILL)
        if not self.watch.wait(scaled(KILL_WAIT_S)):
            raise EndFailed("%s did not end after SIGKILL" % self.describe())
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
            self.abrupt = self._ladder()
            self.proc.wait()
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
        undeclared = [leak for leak in self.end_collect() if not leak.declared]
        if undeclared:
            raise LeakedProcesses("%s left %d process(es) behind, now ended:\n  %s"
                                  % (self.label, len(undeclared),
                                     "\n  ".join(l.describe() for l in undeclared)))


# ---------------------------------------------------------------------------
# The launchers rows call


class Editor:
    """The one way a graphical row starts xwpe."""

    @staticmethod
    def start(owner, binary, args, backend, display, cwd, home, log_dir=None,
              extra_env=None, label="", request=None, daemons=None, dump=None,
              detached_from_terminal=False, terminal_fd=None):
        """Start the editor on a display this owner started.

        With terminal_fd the editor's standard streams are that pty's slave
        side (see Owner.launch), so a row can read what a graphical editor
        writes to the terminal it was started from.

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
        _check_extra_displays(owner, extra_env)
        log_dir = log_dir or home
        if dump is False and backend != "x11":
            raise OwnerRefused("a Wayland editor needs its frame dump")
        env = _editor_env(_backend_env(owner, backend, display, home))
        if dump is not False:
            _add_dump(env, backend, dump or os.path.join(log_dir, "frame.ppm"))
        _apply_extra(env, extra_env)
        env["XWPE_BACKEND"] = backend
        owned = owner.launch([binary] + list(args), env, cwd, log_dir, "xwpe",
                             label, request=request, daemons=daemons,
                             ladder_s=editor_end_ladder_s(),
                             detached_from_terminal=detached_from_terminal,
                             terminal_fd=terminal_fd)
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
# Fixtures and the configure helper


OUT_DIR = os.path.join(HERE, "harness", "out")


@pytest.fixture(scope="session")
def gui_owner():
    """The session's owner; errors the session on leaks no row reported."""
    owner = Owner(OUT_DIR)
    owner.begin()
    yield owner
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


if __name__ == "__main__":
    if sys.argv[1:] == ["--hog"]:
        _busy_loop()
    elif sys.argv[1:] == ["--report-subreaper"]:
        _report_subreaper()
    else:
        sys.stderr.write("usage: gui_owner.py --hog | --report-subreaper\n")
        sys.exit(2)
