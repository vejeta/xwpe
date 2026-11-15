"""The process owner of the graphical suites, checked without a display.

Every row here starts stand-ins instead of an editor and a server: this
file run as a program is the stand-in.  The rows show what a graphical row
gets from the owner: what an editor leaves behind is found, ended and
named; what the row started itself is left alone; a display the owner did
not start is refused; a wait that runs out names its event and its scale.
"""
import ctypes
import errno
import fcntl
import os
import pty
import re
import select
import shlex
import signal
import struct
import subprocess
import sys
import termios
import threading
import time

import pytest

import gui_owner
import procinfo
from host_tools import needs

THIS = os.path.abspath(__file__)
# The stand-in runs with the interpreter of the suite, ignoring the
# machine's PYTHON* variables and the user's site folder.
STANDIN = [sys.executable, "-E", "-s", THIS]

PR_SET_NAME = 15
LEAK_COMM = "standin-leak"
DAEMON_COMM = "standin-daemon"
FAKE_X_DISPLAY = ":4242"
FAKE_SOCKET = "wl-standin"
# The stand-ins announce themselves in their log; a row waits for that.
READY_S = 10.0
GO_POLL_S = 0.05
SLOW_EXIT_S = 0.8
GRANDCHILD_LINE = "grandchild "
READY_LINE = "ready"

pytestmark = pytest.mark.skipif(
    not hasattr(os, "pidfd_open") or not gui_owner.IS_LINUX,
    reason="the sweep rows need a Linux pidfd and a child subreaper")


# ---------------------------------------------------------------------------
# The stand-ins (this file run as a program)


def _set_comm(name):
    """Give this process the kernel command name the rows look for."""
    ctypes.CDLL(None).prctl(PR_SET_NAME, name.encode(), 0, 0, 0)


def _say(text):
    """One line on stdout, which the owner sends to the row's log."""
    sys.stdout.write(text + "\n")
    sys.stdout.flush()


def _wait_forever():
    """Sleep until a signal ends the process."""
    while True:
        signal.pause()


def _standin_leaker(comm):
    """An editor that leaves a child behind: the child moves to its own
    session, ignores SIGTERM and outlives the editor."""
    if os.fork() == 0:
        os.setsid()
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        _set_comm(comm)
        _say(GRANDCHILD_LINE + str(os.getpid()))
        _wait_forever()
    _wait_forever()


def _standin_crasher(comm):
    """An editor that ends on its own and leaves a child in its session
    that ignores SIGTERM, as a crashed editor leaves its language server."""
    if os.fork() == 0:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        _set_comm(comm)
        _say(GRANDCHILD_LINE + str(os.getpid()))
        _wait_forever()
    os._exit(0)


def _standin_crasher_apart(comm, go):
    """An editor that starts a child in a session of its own, which ignores
    SIGTERM, and ends on its own once the file `go` exists, as an editor
    that crashes leaves the language server it moved out of its session."""
    if os.fork() == 0:
        os.setsid()
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        _set_comm(comm)
        _say(GRANDCHILD_LINE + str(os.getpid()))
        _wait_forever()
    while not os.path.exists(go):
        time.sleep(GO_POLL_S)
    os._exit(0)


def _standin_slow_leaver(comm):
    """An editor whose child is still on its way out when the editor is
    ended: it ends by itself a moment later."""
    if os.fork() == 0:
        _set_comm(comm)
        _say(GRANDCHILD_LINE + str(os.getpid()))
        time.sleep(SLOW_EXIT_S)
        os._exit(0)
    _wait_forever()


def _standin_orphan_maker(seen, go):
    """An editor whose child A has a child B and then ends once the file
    `seen` exists, which leaves B an orphan (it comes to pytest, the
    subreaper); B ends, and becomes a zombie of pytest, once `go` exists."""
    if os.fork() == 0:
        if os.fork() == 0:
            _say(GRANDCHILD_LINE + str(os.getpid()))
            while not os.path.exists(go):
                time.sleep(GO_POLL_S)
            os._exit(0)
        while not os.path.exists(seen):
            time.sleep(GO_POLL_S)
        os._exit(0)
    _wait_forever()


def _standin_orphan_in_group(comm):
    """An editor whose child starts a program in the editor's group and
    ends at once (a double fork): the program, which ignores SIGTERM, comes
    to pytest while the editor still runs, and no snapshot ever sees it."""
    if os.fork() == 0:
        if os.fork() == 0:
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            _set_comm(comm)
            _say(GRANDCHILD_LINE + str(os.getpid()))
            _wait_forever()
        os._exit(0)
    _wait_forever()


def _standin_env_names():
    """An editor that writes the names of its environment and the display
    it was given, then waits to be ended."""
    for name in sorted(os.environ):
        _say("env " + name)
    for name in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XWPE_BACKEND"):
        _say(name + "=" + os.environ.get(name, ""))
    _say(READY_LINE)
    _wait_forever()


# What the recording stand-in shows the value of: where it may draw and how
# it was set up.  Every other variable is recorded by name only.
RECORD_SHOWN = ("DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "HOME",
                "XWPE_BACKEND", "XDG_CONFIG_HOME", "TERM",
                "XWPE_GDB_INTERFACE", "XWPE_KEYS")
STAT_TTY_NR = 4         # field of /proc/<pid>/stat after the state: tty_nr
WINCH_LINE = "winch"


def _stdio_size():
    """The window size of the terminal on stdout (else stdin), or None."""
    for fd in (1, 0):
        try:
            packed = fcntl.ioctl(fd, termios.TIOCGWINSZ, b"\0" * 8)
        except OSError:
            continue
        rows, cols = struct.unpack("HHHH", packed)[:2]
        return cols, rows
    return None


def _record_header(args):
    """The facts a recording stand-in writes about how it was started."""
    fields = gui_owner._stat_fields(os.getpid())
    size = _stdio_size()
    lines = ["pid %d ppid %d sid %d pgid %d" % (os.getpid(), os.getppid(),
                                                os.getsid(0), os.getpgid(0)),
             "tty_nr %s" % fields[STAT_TTY_NR],
             "stdin_tty %d" % os.isatty(0),
             "size %s" % ("none" if size is None else "%dx%d" % size),
             "argv %r" % (args,)]
    for name in sorted(os.environ):
        if name in RECORD_SHOWN:
            lines.append("env %s=%s" % (name, os.environ[name]))
        else:
            lines.append("env " + name)
    return lines


def _standin_record(out_dir, args):
    """A stand-in for the editor binary: write <out_dir>/<pid>.txt (the pid,
    how its terminal is set up, the names of its environment, the value only
    of the display and style ones), note each SIGWINCH in it, and wait to be
    ended.  The file appears whole or not at all."""
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "%d.txt" % os.getpid())

    def note_winch(signum, frame):
        with open(path, "a", encoding="utf-8") as fh:
            fh.write(WINCH_LINE + "\n")
    # Before the record exists: a row that sees it may resize at once.
    signal.signal(signal.SIGWINCH, note_winch)
    with open(path + ".part", "w", encoding="utf-8") as fh:
        fh.write("\n".join(_record_header(args)) + "\n")
    os.replace(path + ".part", path)
    _wait_forever()


def _standin_print_env(names):
    """Print NAME=value for each variable that is set, NAME unset for the
    rest, then end."""
    for name in names:
        if name in os.environ:
            _say("%s=%s" % (name, os.environ[name]))
        else:
            _say(name + " unset")
    sys.exit(0)


def _run_standin(argv):
    """Dispatch this file run as a program."""
    if argv[:1] == ["--record"] and len(argv) >= 2:
        _standin_record(argv[1], argv[2:])
    elif argv[:1] == ["--print-env"]:
        _standin_print_env(argv[1:])
    elif argv[:1] == ["--leaker"] and len(argv) == 2:
        _standin_leaker(argv[1])
    elif argv[:1] == ["--crasher"] and len(argv) == 2:
        _standin_crasher(argv[1])
    elif argv[:1] == ["--crasher-apart"] and len(argv) == 3:
        _standin_crasher_apart(argv[1], argv[2])
    elif argv[:1] == ["--slow-leaver"] and len(argv) == 2:
        _standin_slow_leaver(argv[1])
    elif argv[:1] == ["--orphan-maker"] and len(argv) == 3:
        _standin_orphan_maker(argv[1], argv[2])
    elif argv[:1] == ["--orphan-in-group"] and len(argv) == 2:
        _standin_orphan_in_group(argv[1])
    elif argv == ["--sleeper"]:
        _wait_forever()
    elif argv == ["--env-names"]:
        _standin_env_names()
    sys.stderr.write("usage: test_gui_owner.py --leaker COMM | --crasher COMM "
                     "| --crasher-apart COMM GO | --orphan-maker SEEN GO "
                     "| --orphan-in-group COMM "
                     "| --sleeper | --env-names | --record DIR [ARG...] "
                     "| --print-env NAME...\n")
    sys.exit(2)


# ---------------------------------------------------------------------------
# Helpers for the rows


@pytest.fixture
def owner(tmp_path):
    """An owner of the row's own, begun as a session fixture would, whose
    leak file goes to the row's folder; it must end with nothing left."""
    own = gui_owner.Owner(str(tmp_path / "out"))
    own.begin()
    yield own
    late = own.finish()
    assert not late, "left at the owner's end: %s" % [l.describe() for l in late]


def _fake_x_display(owner, tmp_path):
    """A display name the owner accepts, served by a stand-in it started."""
    server = owner.start_service(STANDIN + ["--sleeper"], "fake X server",
                                 str(tmp_path), str(tmp_path), kind="server")
    return owner.register_x_display(FAKE_X_DISPLAY, server)


def _start_standin(owner, tmp_path, request, mode, daemons=None, detached=False):
    """Start a stand-in editor through the launcher on the fake display."""
    display = _fake_x_display(owner, tmp_path)
    return gui_owner.Editor.start(owner, sys.executable,
                                  ["-E", "-s", THIS] + mode, "x11", display,
                                  str(tmp_path), str(tmp_path),
                                  label=request.node.nodeid, request=request,
                                  daemons=daemons,
                                  detached_from_terminal=detached)


def _grandchild_pid(editor):
    """The pid the stand-in's grandchild wrote to the log, or None yet."""
    for line in editor.log_text().splitlines():
        if line.startswith(GRANDCHILD_LINE):
            return int(line[len(GRANDCHILD_LINE):])
    return None


def _wait_grandchild(editor):
    return gui_owner.wait_for(lambda: _grandchild_pid(editor),
                              "the stand-in's grandchild to announce its pid",
                              READY_S, state_fn=editor.log_text)


def _env_lines(editor):
    """The stand-in's environment listing, once it is complete."""
    gui_owner.wait_for(lambda: READY_LINE in editor.log_text().splitlines(),
                       "the stand-in to list its environment", READY_S,
                       state_fn=editor.log_text)
    return editor.log_text().splitlines()


def _ended(pidfd):
    """True when the process behind pidfd has ended."""
    return bool(select.select([pidfd], [], [], 0)[0])


def _end_leftover(pidfd):
    """End and reap a process a failed row left (red run), by its pidfd."""
    try:
        if not _ended(pidfd):
            signal.pidfd_send_signal(pidfd, signal.SIGKILL)
            select.select([pidfd], [], [], gui_owner.scaled(gui_owner.KILL_WAIT_S))
        os.waitid(os.P_PIDFD, pidfd, os.WEXITED)
    except ChildProcessError:
        pass
    finally:
        os.close(pidfd)


# ---------------------------------------------------------------------------
# The rows


# @scenario SC-harness-owner-01
def test_end_names_and_ends_a_grandchild_left_in_its_own_session(owner, tmp_path, request):
    editor = _start_standin(owner, tmp_path, request, ["--leaker", LEAK_COMM])
    grandchild = _wait_grandchild(editor)
    pidfd = os.pidfd_open(grandchild)
    try:
        with pytest.raises(gui_owner.LeakedProcesses) as raised:
            editor.end()
        assert "pid %d comm %s" % (grandchild, LEAK_COMM) in str(raised.value)
        assert request.node.nodeid in str(raised.value)
        assert _ended(pidfd), "the grandchild survived its row"
        assert procinfo.run_state(grandchild) == procinfo.GONE
    finally:
        _end_leftover(pidfd)
    leak_file = owner.write_leak_file()
    with open(leak_file, encoding="utf-8") as fh:
        assert "pid %d comm %s" % (grandchild, LEAK_COMM) in fh.read()


# @scenario SC-harness-owner-02
def test_a_process_the_row_started_itself_is_not_swept(owner, tmp_path, request):
    apart = subprocess.Popen(STANDIN + ["--sleeper"], start_new_session=True)
    alongside = subprocess.Popen(STANDIN + ["--sleeper"])
    pidfd = None
    try:
        editor = _start_standin(owner, tmp_path, request, ["--leaker", LEAK_COMM])
        grandchild = _wait_grandchild(editor)
        pidfd = os.pidfd_open(grandchild)
        with pytest.raises(gui_owner.LeakedProcesses) as raised:
            editor.end()
        assert "pid %d " % grandchild in str(raised.value)
        assert "pid %d " % apart.pid not in str(raised.value)
        assert "pid %d " % alongside.pid not in str(raised.value)
        assert apart.poll() is None, "the sweep ended a process in its own session"
        assert alongside.poll() is None, "the sweep ended a process in pytest's session"
    finally:
        for proc in (apart, alongside):
            proc.kill()
            proc.wait()
        if pidfd is not None:
            _end_leftover(pidfd)


# @scenario SC-harness-owner-03
def test_a_declared_daemon_is_ended_without_an_error(owner, tmp_path, request):
    editor = _start_standin(owner, tmp_path, request, ["--leaker", DAEMON_COMM],
                            daemons=[DAEMON_COMM])
    grandchild = _wait_grandchild(editor)
    pidfd = os.pidfd_open(grandchild)
    try:
        editor.end()
        assert _ended(pidfd), "the declared daemon was left running"
    finally:
        _end_leftover(pidfd)
    declared = [l for l in owner.leaks if l.pid == grandchild]
    assert declared and declared[0].declared


# @scenario SC-harness-owner-04
def test_an_editor_is_refused_a_display_the_owner_did_not_start(owner, tmp_path, monkeypatch):
    monkeypatch.setenv("DISPLAY", ":77")
    for display in (":77", ":88", FAKE_X_DISPLAY):
        with pytest.raises(gui_owner.OwnerRefused):
            gui_owner.Editor.start(owner, sys.executable, [THIS, "--sleeper"],
                                   "x11", display, str(tmp_path), str(tmp_path))
    real = gui_owner.WaylandDisplay("/run/user/%d" % os.getuid(), "wayland-0",
                                    None, None)
    with pytest.raises(gui_owner.OwnerRefused):
        gui_owner.Editor.start(owner, sys.executable, [THIS, "--sleeper"],
                               "wayland", real, str(tmp_path), str(tmp_path))
    compositor = owner.start_service(STANDIN + ["--sleeper"], "fake compositor",
                                     str(tmp_path), str(tmp_path), kind="compositor")
    with pytest.raises(gui_owner.OwnerRefused):
        owner.register_wayland(str(tmp_path), FAKE_SOCKET, compositor, None)
    assert not owner.ledger.pids() - {compositor.pid}, \
        "a refused start still started something"


# @scenario SC-harness-owner-05
def test_wait_for_names_the_event_and_the_scale(monkeypatch):
    monkeypatch.setenv(gui_owner.WAIT_SCALE_VAR, "1.5")
    with pytest.raises(gui_owner.OwnerTimeout) as raised:
        gui_owner.wait_for(lambda: None, "the stand-in event", 0.2,
                           state_fn=lambda: "nothing there")
    assert str(raised.value) == ("waited 0.3 s (scale 1.5) for the stand-in "
                                 "event; last state: nothing there")


# @scenario SC-harness-owner-06
def test_the_editor_gets_the_whitelist_and_the_owners_display(owner, tmp_path, request, monkeypatch):
    monkeypatch.setenv("WAYLAND_DISPLAY", "wayland-0")
    monkeypatch.setenv("XWPE_TEST_MACHINE_ONLY", "1")
    monkeypatch.setenv("LINES", "40")
    editor = _start_standin(owner, tmp_path, request, ["--env-names"])
    lines = _env_lines(editor)
    names = {line[len("env "):] for line in lines if line.startswith("env ")}
    pinned = {"LC_ALL", "LANG", "SHELL", "HOME", "XDG_CONFIG_HOME",
              "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "DISPLAY", "XWPE_BACKEND",
              "XWPE_LIB", "XWPE_FONT_SIZE", "XWPE_LSP_NO_EAGER", "XWPE_X_DUMP",
              "XWPE_KEYS"}
    # The interpreter may add its own (LC_CTYPE when it coerces a locale).
    interpreter_own = {"LC_CTYPE"}
    assert names - pinned - interpreter_own - set(gui_owner.PASS_THROUGH) == set()
    assert "DISPLAY=%s" % FAKE_X_DISPLAY in lines
    assert editor.env["XWPE_BACKEND"] == "x11"
    # A new profile is asked which keys it wants and nobody answers in a row.
    assert editor.env["XWPE_KEYS"] == "borland"
    assert not editor.env["XDG_RUNTIME_DIR"].startswith(gui_owner.REAL_SESSION_RUNTIME)
    editor.end()


# @scenario SC-harness-owner-10
def test_a_wayland_editor_gets_the_owners_socket_and_x_display(owner, tmp_path, request):
    x_display = _fake_x_display(owner, tmp_path)
    compositor = owner.start_service(STANDIN + ["--sleeper"], "fake compositor",
                                     str(tmp_path), str(tmp_path), kind="compositor")
    runtime = owner.make_runtime_dir()
    wayland = owner.register_wayland(runtime, FAKE_SOCKET, compositor, x_display)
    editor = gui_owner.Editor.start(owner, sys.executable,
                                    ["-E", "-s", THIS, "--env-names"], "wayland",
                                    wayland, str(tmp_path), str(tmp_path),
                                    label=request.node.nodeid, request=request)
    lines = _env_lines(editor)
    assert "WAYLAND_DISPLAY=%s" % FAKE_SOCKET in lines
    assert "XDG_RUNTIME_DIR=%s" % runtime in lines
    assert "DISPLAY=%s" % FAKE_X_DISPLAY in lines, "a fallback would not land on the owner's display"
    assert "XWPE_BACKEND=wayland" in lines
    assert "env XWPE_WL_DUMP" in lines and "env XWPE_X_DUMP" not in lines
    editor.end()
    owner.finish()
    assert not os.path.exists(runtime), "the owner left its runtime folder"


# @scenario SC-harness-owner-07
def test_the_subreaper_is_set_only_while_an_owner_is_begun():
    # Another suite's session owner may be begun in this same pytest: the
    # flag must then be set, and unset otherwise.  The child process below
    # proves that importing alone sets nothing.
    assert gui_owner.subreaper_set() == (gui_owner.owners_begun() > 0), \
        "the subreaper flag does not follow the owners begun (%d)" \
        % gui_owner.owners_begun()
    report = subprocess.run([sys.executable, "-E", "-s", gui_owner.__file__,
                             "--report-subreaper"], capture_output=True,
                            text=True, timeout=gui_owner.scaled(READY_S))
    assert report.stdout.strip() == "import 0 begin 1 finish 0", report.stderr


# @scenario SC-harness-owner-08
def test_launching_off_the_main_thread_is_refused(owner, tmp_path):
    refused = []

    def launch():
        try:
            owner.start_service(STANDIN + ["--sleeper"], "off-thread",
                                str(tmp_path), str(tmp_path))
        except gui_owner.OwnerRefused as error:
            refused.append(error)

    thread = threading.Thread(target=launch)
    thread.start()
    thread.join()
    assert refused and not owner.ledger.pids()


# @scenario SC-harness-owner-09
def test_load_processes_are_owned_busy_and_ended_by_their_pid(owner, tmp_path, monkeypatch):
    monkeypatch.setenv(gui_owner.LOAD_VAR, "2")
    hogs = owner.start_hogs(gui_owner.load_count(), str(tmp_path), "load")
    assert len(hogs) == 2 and {h.pid for h in hogs} <= owner.ledger.pids()
    for hog in hogs:
        gui_owner.wait_for(lambda: procinfo.cpu_ticks(hog.pid),
                           "a load process to use the CPU", READY_S)
    pidfds = [os.pidfd_open(h.pid) for h in hogs]
    try:
        for hog in hogs:
            hog.end()
        assert all(_ended(fd) for fd in pidfds)
        assert not owner.ledger.pids()
    finally:
        for fd in pidfds:
            os.close(fd)


def _end_own(proc):
    """End and reap a process the row itself started."""
    if proc.poll() is None:
        proc.kill()
    proc.wait()


def _start_env_standin(owner, display, tmp_path, request, extra_env):
    """Start the environment-listing stand-in with the row's own variables."""
    return gui_owner.Editor.start(owner, sys.executable,
                                  ["-E", "-s", THIS, "--env-names"], "x11",
                                  display, str(tmp_path), str(tmp_path),
                                  label=request.node.nodeid, request=request,
                                  extra_env=extra_env)


def _leave_a_tree_behind(owner, tmp_path, request):
    """Start a stand-in that leaks a grandchild and end it, so the owner
    keeps the ended row's tree for its last sweep.  Returns that tree."""
    editor = _start_standin(owner, tmp_path, request, ["--leaker", LEAK_COMM])
    pidfd = os.pidfd_open(_wait_grandchild(editor))
    try:
        with pytest.raises(gui_owner.LeakedProcesses):
            editor.end()
    finally:
        _end_leftover(pidfd)
    return owner._ended_trees[-1]


# @scenario SC-harness-owner-11
def test_a_process_started_during_the_editors_end_is_not_swept(owner, tmp_path, request):
    """A row that starts a helper of its own in a session of its own, while
    its editor is being ended, keeps that helper: the sweep names only what
    the editor left."""
    editor = _start_standin(owner, tmp_path, request, ["--leaker", LEAK_COMM])
    grandchild = _wait_grandchild(editor)
    pidfd = os.pidfd_open(grandchild)
    sibling = subprocess.Popen(STANDIN + ["--sleeper"], start_new_session=True)
    try:
        with pytest.raises(gui_owner.LeakedProcesses) as raised:
            editor.end()
        assert "pid %d " % grandchild in str(raised.value)
        assert "pid %d " % sibling.pid not in str(raised.value)
        assert sibling.poll() is None, "the sweep ended a process the row started"
    finally:
        _end_own(sibling)
        _end_leftover(pidfd)


# @scenario SC-harness-owner-12
def test_a_reused_session_id_does_not_make_a_row_process_a_leftover(owner, tmp_path, request):
    """The kernel may hand a freed session id to a process the row starts
    later.  The owner's last sweep must not take that process for something
    an ended editor left, or the row's own process would be killed and its
    exit status stolen.  The reuse is stood in for by writing the number into
    the ended row's tree."""
    tree = _leave_a_tree_behind(owner, tmp_path, request)
    sibling = subprocess.Popen(STANDIN + ["--sleeper"], start_new_session=True)
    tree.sids.add(sibling.pid)
    try:
        owner.finish()
        assert sibling.poll() is None, "the last sweep ended a process the row started"
    finally:
        _end_own(sibling)


# @scenario SC-harness-owner-13
def test_a_reused_pid_does_not_make_a_row_process_a_leftover(owner, tmp_path, request):
    """Same protection as the session id row, for a process number the ended
    editor's snapshot held: a number alone does not name a process, its
    start time must match too."""
    tree = _leave_a_tree_behind(owner, tmp_path, request)
    sibling = subprocess.Popen(STANDIN + ["--sleeper"])
    tree.pids.add(sibling.pid)
    try:
        owner.finish()
        assert sibling.poll() is None, "the last sweep ended a process the row started"
    finally:
        _end_own(sibling)


# @scenario SC-harness-owner-14
def test_extra_env_cannot_point_an_editor_off_the_owners_displays(owner, tmp_path, request, monkeypatch):
    """A row's own variables are refused when they would lead the editor to
    the machine's session: its DISPLAY, a display the owner did not start, a
    runtime folder the owner did not make, a Wayland socket given as a path.
    A bare socket name stays allowed, for a row that names a missing
    compositor."""
    monkeypatch.setenv("DISPLAY", ":77")
    display = _fake_x_display(owner, tmp_path)
    before = owner.ledger.pids()
    for extra in ({"DISPLAY": ":77"}, {"DISPLAY": ":88"},
                  {"XDG_RUNTIME_DIR": "/run/user/%d" % os.getuid()},
                  {"WAYLAND_DISPLAY": "/run/user/%d/wayland-0" % os.getuid()}):
        try:
            editor = _start_env_standin(owner, display, tmp_path, request, extra)
        except gui_owner.OwnerRefused:
            continue
        lines = _env_lines(editor)
        editor.end()
        pytest.fail("extra_env %r was accepted: %s"
                    % (extra, [l for l in lines if "=" in l]))
    assert owner.ledger.pids() == before, "a refused start still started something"
    editor = _start_env_standin(owner, display, tmp_path, request,
                                {"WAYLAND_DISPLAY": "wl-missing",
                                 "DISPLAY": display.name})
    _env_lines(editor)
    editor.end()


# @scenario SC-harness-owner-15
def test_extra_env_cannot_point_a_service_at_the_machines_session(owner, tmp_path, monkeypatch):
    """A window manager started with the machine's DISPLAY would manage the
    user's own desktop: the service launcher refuses it before it starts."""
    monkeypatch.setenv("DISPLAY", ":77")
    before = owner.ledger.pids()
    with pytest.raises(gui_owner.OwnerRefused):
        service = owner.start_service(STANDIN + ["--env-names"], "wm",
                                      str(tmp_path), str(tmp_path),
                                      extra_env={"DISPLAY": ":77"})
        service.end()
    assert owner.ledger.pids() == before, "a refused start still started something"


# @scenario SC-harness-owner-16
def test_a_wayland_socket_given_as_a_path_is_refused(owner, tmp_path):
    """libwayland connects to a name with a slash as a path and ignores the
    owner's runtime folder, so a socket such as the user's own
    /run/user/<uid>/wayland-0 must not be registered."""
    x_display = _fake_x_display(owner, tmp_path)
    compositor = owner.start_service(STANDIN + ["--sleeper"], "fake compositor",
                                     str(tmp_path), str(tmp_path), kind="compositor")
    runtime = owner.make_runtime_dir()
    with pytest.raises(gui_owner.OwnerRefused):
        owner.register_wayland(runtime, "/run/user/%d/wayland-0" % os.getuid(),
                               compositor, x_display)


class _LibcRefusingDeathSignal:
    """A C library whose prctl refuses PR_SET_PDEATHSIG and passes the rest."""

    def __init__(self, real):
        self.real = real

    def prctl(self, option, *args):
        """Fail the death signal request, as a kernel without it would."""
        if option == gui_owner.PR_SET_PDEATHSIG:
            return -1
        return self.real.prctl(option, *args)


# @scenario SC-harness-owner-17
def test_a_launch_whose_death_signal_is_refused_starts_nothing(owner, tmp_path, monkeypatch):
    """A process that would outlive a killed pytest is never started, and
    the row is told why in the owner's own words, not by a bare error from
    inside the child."""
    children_before = {pid for pid, _ in gui_owner._direct_children_linux(os.getpid())}
    monkeypatch.setattr(gui_owner, "_LIBC",
                        _LibcRefusingDeathSignal(gui_owner._LIBC))
    with pytest.raises(gui_owner.OwnerRefused) as raised:
        owner.start_service(STANDIN + ["--sleeper"], "refused", str(tmp_path),
                            str(tmp_path))
    monkeypatch.undo()
    assert "PR_SET_PDEATHSIG" in str(raised.value)
    assert not owner.ledger.pids(), "a refused start is still in the ledger"
    children = {pid for pid, _ in gui_owner._direct_children_linux(os.getpid())}
    assert children <= children_before, "a refused start left a child behind"


# @scenario SC-harness-owner-61
def test_a_launch_that_cannot_take_its_terminal_says_so(owner, tmp_path):
    """A process asked to have a controlling terminal whose streams are no
    terminal is not started, and the row is told it was the terminal, not
    the death signal; one asked to be detached too is refused before any
    start."""
    with pytest.raises(gui_owner.OwnerRefused) as raised:
        owner.launch(STANDIN + ["--sleeper"], {"PATH": os.environ["PATH"]},
                     str(tmp_path), str(tmp_path), "probe", "no terminal",
                     controlling_tty=True)
    assert "controlling terminal" in str(raised.value)
    assert "PR_SET_PDEATHSIG" not in str(raised.value)
    with pytest.raises(gui_owner.OwnerRefused) as raised:
        owner.launch(STANDIN + ["--sleeper"], {"PATH": os.environ["PATH"]},
                     str(tmp_path), str(tmp_path), "probe", "both",
                     controlling_tty=True, detached_from_terminal=True)
    assert "detached" in str(raised.value)
    assert not owner.ledger.pids(), "a refused start is still in the ledger"


# @scenario SC-harness-owner-18
def test_a_row_cannot_hand_a_process_the_machines_home_or_compositor(owner, tmp_path, request):
    """The editor reads its settings and keys from HOME, and libwayland
    takes WAYLAND_SOCKET as an open connection to a compositor: a row
    giving either from the machine would run on the user's own session."""
    display = _fake_x_display(owner, tmp_path)
    before = owner.ledger.pids()
    real_home = os.path.expanduser("~")
    for extra in ({"HOME": real_home}, {"WAYLAND_SOCKET": "3"}):
        with pytest.raises(gui_owner.OwnerRefused):
            editor = _start_env_standin(owner, display, tmp_path, request, extra)
            editor.end()
    with pytest.raises(gui_owner.OwnerRefused):
        editor = gui_owner.Editor.start(owner, sys.executable,
                                        ["-E", "-s", THIS, "--sleeper"], "x11",
                                        display, str(tmp_path), real_home,
                                        log_dir=str(tmp_path))
        editor.end()
    assert owner.ledger.pids() == before, "a refused start still started something"


# @scenario SC-harness-owner-19
def test_an_editor_that_ended_on_its_own_is_still_swept(owner, tmp_path, request):
    """An editor that crashes leaves its children in its session.  A row
    that saw it end through running() must still have them found, named
    and ended when it ends the editor.  The editor is detached from the
    terminal, so it has a session of its own to leave them in; an editor
    started as a terminal starts it has none, and its children are found
    through the snapshot instead (the rows below)."""
    editor = _start_standin(owner, tmp_path, request, ["--crasher", LEAK_COMM],
                            detached=True)
    grandchild = _wait_grandchild(editor)
    pidfd = os.pidfd_open(grandchild)
    try:
        gui_owner.wait_for(lambda: not editor.running(),
                           "the stand-in editor to end on its own", READY_S,
                           state_fn=editor.log_text)
        with pytest.raises(gui_owner.LeakedProcesses) as raised:
            editor.end()
        assert "pid %d comm %s" % (grandchild, LEAK_COMM) in str(raised.value)
        assert _ended(pidfd), "the crashed editor's child survived its row"
    finally:
        _end_leftover(pidfd)


def _start_apart(owner, tmp_path, request):
    """A stand-in editor with a child in a session of its own, and the
    file that makes it end on its own."""
    go = tmp_path / "go"
    editor = _start_standin(owner, tmp_path, request,
                            ["--crasher-apart", LEAK_COMM, str(go)])
    return editor, go, _wait_grandchild(editor)


def _crash(editor, go):
    """Make the stand-in editor end on its own and wait until it has."""
    go.write_text("")
    gui_owner.wait_for(lambda: not editor.running(),
                       "the stand-in editor to end on its own", READY_S,
                       state_fn=editor.log_text)


# @scenario SC-harness-owner-20
def test_a_child_moved_to_its_own_session_is_found_when_the_editor_crashed(
        owner, tmp_path, request):
    """A row notes what its editor has started while the editor runs; when
    the editor then crashes, a child it had moved to a session of its own is
    still found, named and ended."""
    editor, go, grandchild = _start_apart(owner, tmp_path, request)
    pidfd = os.pidfd_open(grandchild)
    try:
        editor.refresh_tree()
        _crash(editor, go)
        with pytest.raises(gui_owner.LeakedProcesses) as raised:
            editor.end()
        assert "pid %d comm %s" % (grandchild, LEAK_COMM) in str(raised.value)
        assert _ended(pidfd), "the crashed editor's child survived its row"
    finally:
        _end_leftover(pidfd)


# @scenario SC-harness-owner-21
def test_a_child_in_its_own_session_is_known_only_from_the_refresh(
        owner, tmp_path, request):
    """The control of the row above: with no refresh before the crash the
    child in a session of its own is unknown to the owner, so the row's
    result comes from the refresh and from nothing else."""
    editor, go, grandchild = _start_apart(owner, tmp_path, request)
    pidfd = os.pidfd_open(grandchild)
    try:
        _crash(editor, go)
        editor.end()
        assert not _ended(pidfd), "the child was found without a refresh"
    finally:
        _end_leftover(pidfd)


def _orphan_zombie(owner, tmp_path, request):
    """An editor stand-in whose orphan B, known to the snapshot, has ended
    and is a zombie of pytest.  Returns (editor, B's pid)."""
    seen, go = tmp_path / "seen", tmp_path / "go"
    display = _fake_x_display(owner, tmp_path)
    editor = gui_owner.Editor.start(
        owner, sys.executable,
        ["-E", "-s", THIS, "--orphan-maker", str(seen), str(go)], "x11",
        display, str(tmp_path), str(tmp_path), label=request.node.nodeid,
        request=request)
    orphan = _wait_grandchild(editor)
    editor.refresh_tree()
    seen.write_text("")
    gui_owner.wait_for(lambda: procinfo.run_state(orphan) is not None
                       and _parent_of(orphan) == os.getpid(),
                       "the orphan to come to pytest", READY_S)
    go.write_text("")
    gui_owner.wait_for(lambda: procinfo.run_state(orphan) == procinfo.ZOMBIE,
                       "the orphan to end", READY_S)
    return editor, orphan


def _parent_of(pid):
    """The parent pid of pid, or None once it is gone."""
    fields = gui_owner._stat_fields(pid)
    if fields is None:
        return None
    return int(fields[gui_owner.STAT_PPID])


# @scenario SC-harness-owner-26
def test_an_orphan_zombie_in_the_snapshot_is_reaped(owner, tmp_path, request):
    editor, orphan = _orphan_zombie(owner, tmp_path, request)
    assert editor.reap_zombies() == [orphan]
    assert procinfo.run_state(orphan) == procinfo.GONE, \
        "the ended orphan is still a zombie"


# @scenario SC-harness-owner-27
def test_a_zombie_outside_the_snapshot_is_left_alone(owner, tmp_path, request):
    editor, orphan = _orphan_zombie(owner, tmp_path, request)
    own = subprocess.Popen(["true"])
    try:
        gui_owner.wait_for(lambda: procinfo.run_state(own.pid) == procinfo.ZOMBIE,
                           "the row's own child to end", READY_S)
        assert editor.reap_zombies() == [orphan]
        assert procinfo.run_state(own.pid) == procinfo.ZOMBIE, \
            "the owner reaped a child the row started itself"
        assert own.wait() == 0, "the row's own child could not be collected"
    finally:
        own.poll()


# @scenario SC-harness-owner-31
def test_a_child_still_on_its_way_out_when_the_editor_ends_is_no_leak(
        owner, tmp_path, request):
    """A debugger the editor told to stop may need a moment after the
    editor is gone; only what is still running after the grace is a leak."""
    editor = _start_standin(owner, tmp_path, request, ["--slow-leaver", LEAK_COMM])
    grandchild = _wait_grandchild(editor)
    editor.refresh_tree()
    editor.end()
    assert procinfo.run_state(grandchild) == procinfo.GONE
    assert not [l for l in owner.leaks if l.pid == grandchild]


# @scenario SC-harness-owner-32
def test_an_orphan_left_in_the_editors_group_is_found(owner, tmp_path, request):
    """An editor started as a terminal starts it shares pytest's session, so
    a program it orphaned before any snapshot saw it is found by the
    editor's own group: named, ended, and the row errors."""
    editor = _start_standin(owner, tmp_path, request,
                            ["--orphan-in-group", LEAK_COMM])
    orphan = _wait_grandchild(editor)
    gui_owner.wait_for(lambda: _parent_of(orphan) == os.getpid(),
                       "the orphan to come to pytest", READY_S)
    pidfd = os.pidfd_open(orphan)
    try:
        editor.refresh_tree()
        with pytest.raises(gui_owner.LeakedProcesses) as raised:
            editor.end()
        assert "pid %d comm %s" % (orphan, LEAK_COMM) in str(raised.value)
        assert _ended(pidfd), "the orphan in the editor's group survived its row"
    finally:
        _end_leftover(pidfd)


# @scenario SC-harness-owner-28
def test_an_editor_is_not_a_session_leader_unless_detached(owner, tmp_path, request):
    """A terminal starts an editor without a session of its own; a desktop
    launcher does.  The default is the first, the option the second."""
    attached = _start_standin(owner, tmp_path, request, ["--sleeper"])
    detached = _start_standin(owner, tmp_path, request, ["--sleeper"],
                              detached=True)
    assert gui_owner.session_of(attached.pid) != attached.pid
    assert gui_owner.session_of(attached.pid) == os.getsid(0)
    assert gui_owner.session_of(detached.pid) == detached.pid


# A bare DISPLAY: not inside quotes (an environment variable's name) and not
# the start of a longer name.
BARE_DISPLAY = re.compile(r"(?<![\"\w])DISPLAY(?![\"\w])")
CONFTESTS = ("x11", "wayland")


# @scenario SC-harness-owner-23
def test_no_x11_module_reads_a_display_constant_and_both_conftests_have_the_accessor():
    """The X server picks its display number, so a module asks for it when
    it needs it, by the one accessor both suites share, and imports no
    constant."""
    found = []
    for suite in CONFTESTS:
        folder = os.path.join(gui_owner.HERE, suite)
        for name in sorted(os.listdir(folder)):
            if not name.endswith(".py"):
                continue
            with open(os.path.join(folder, name), encoding="utf-8") as fh:
                for number, line in enumerate(fh, 1):
                    if BARE_DISPLAY.search(line):
                        found.append("tests/%s/%s:%d: %s"
                                     % (suite, name, number, line.strip()))
    assert not found, "a constant display is still read:\n  " + "\n  ".join(found)
    for suite in CONFTESTS:
        path = os.path.join(gui_owner.HERE, suite, "conftest.py")
        with open(path, encoding="utf-8") as fh:
            assert "def display_name():" in fh.read(), \
                "tests/%s/conftest.py has no display_name()" % suite


# ---------------------------------------------------------------------------
# What a test may start, and what an editor is given (the process owner)

# The terminal driver still starts its editor by hand; the rows that show it
# are red there until it is moved onto the owner, and they are marked so the
# run is green and the move turns each into an unexpected pass that must have
# its mark removed.
PASS_C = pytest.mark.xfail(
    strict=True, raises=AssertionError,
    reason="WpeSession still starts its editor by hand (moved onto the "
           "owner by the terminal pass)")
SUITES = ["x11", "wayland"]

CANARY_VALUES = {"DISPLAY": ":99", "WAYLAND_DISPLAY": "wayland-9",
                 "DBUS_SESSION_BUS_ADDRESS": "unix:path=/nonexistent/bus",
                 "XAUTHORITY": "/nonexistent/Xauthority",
                 "SSH_AUTH_SOCK": "/nonexistent/agent",
                 "TPAS_FAKE_API_KEY": "canary-value"}
# The names a graphical editor, or one on a terminal, may be given: the
# owner's own, what it passes through, and what the interpreter or the shell
# of the stand-in's launcher adds by itself.
EDITOR_PINNED = {"LC_ALL", "LANG", "SHELL", "HOME", "XDG_CONFIG_HOME",
                 "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "XWPE_LIB",
                 "XWPE_LSP_NO_EAGER", "XWPE_KEYS", "TERM"}
LAUNCHER_OWN = {"LC_CTYPE", "PWD", "OLDPWD", "SHLVL", "_"}
INNER_START_S = 90.0
INNER_END_S = 120.0
SETTLE_S = 8.0
SERVER_FILES = ("/tmp/.X%d-lock", "/tmp/.X11-unix/X%d")
INNER_ROW = "test_block_marking.py"
RUNTIME_PREFIX = "xwpe-wl-"


def _write_launcher(folder, record_dir):
    """An executable the suites take for the editor binary: it records how
    it was started in record_dir and waits to be ended."""
    path = os.path.join(str(folder), "standin-editor")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("#!/bin/sh\nexec %s -E -s %s --record %s \"$@\"\n" % (
            shlex.quote(sys.executable), shlex.quote(THIS),
            shlex.quote(str(record_dir))))
    os.chmod(path, 0o755)
    return path


def _parse_record(text):
    """The facts one recording stand-in wrote, as a dict."""
    rec = {"names": set(), "values": {}, "winch": 0}
    for line in text.splitlines():
        words = line.split()
        if line.startswith("pid "):
            rec.update(pid=int(words[1]), ppid=int(words[3]),
                       sid=int(words[5]), pgid=int(words[7]))
        elif line.startswith("tty_nr "):
            rec["tty_nr"] = int(words[1])
        elif line.startswith("stdin_tty "):
            rec["stdin_tty"] = int(words[1])
        elif line.startswith("size "):
            rec["size"] = words[1]
        elif line.startswith("env "):
            name, equals, value = line[len("env "):].partition("=")
            rec["names"].add(name)
            if equals:
                rec["values"][name] = value
        elif line == WINCH_LINE:
            rec["winch"] += 1
    return rec


def _read_record(record_dir):
    """The first record in record_dir, or None while there is none."""
    try:
        names = sorted(n for n in os.listdir(str(record_dir)) if n.endswith(".txt"))
    except OSError:
        return None
    if not names:
        return None
    with open(os.path.join(str(record_dir), names[0]), encoding="utf-8") as fh:
        return _parse_record(fh.read())


def _wait_record(record_dir):
    """Wait for the stand-in's record and return it parsed."""
    return gui_owner.wait_for(lambda: _read_record(record_dir),
                              "the recording stand-in to start", READY_S)


def _under(folder, cwd):
    """True when cwd is folder or inside it."""
    return cwd is not None and (cwd == folder or cwd.startswith(folder + os.sep))


def _processes_under(folder):
    """(pid, command name) of every process whose working directory is
    under folder: what a run started there and left.  Only stat and cwd
    are read."""
    found = []
    for entry in os.listdir(gui_owner.PROC_ROOT):
        if entry.isdigit() and int(entry) != os.getpid() \
                and _under(folder, gui_owner.working_dir(int(entry))):
            found.append((int(entry), gui_owner.command_name(int(entry))))
    return found


def _kill_by_pid(pid, folder):
    """SIGKILL pid through a pidfd, after its working directory was seen
    under folder again, so a pid the kernel reused is never touched."""
    try:
        pidfd = os.pidfd_open(pid)
    except OSError:
        return
    try:
        if _under(folder, gui_owner.working_dir(pid)):
            signal.pidfd_send_signal(pidfd, signal.SIGKILL)
            select.select([pidfd], [], [], gui_owner.scaled(gui_owner.KILL_WAIT_S))
    except ProcessLookupError:
        pass
    finally:
        os.close(pidfd)


def _running(pid):
    """True while the editor the stand-in recorded is still there."""
    return gui_owner.process_running(pid)


class _InnerRun:
    """One pytest of its own, started by a row, running one graphical row of
    a suite against the recording stand-in, with the caller's session and
    some keys in its environment.  The row ends what a red run leaves by the
    pids it finds, so a red run of the row itself leaves nothing."""

    def __init__(self, root, suite):
        self.suite = suite
        self.work = os.path.join(str(root), "work")
        self.records = os.path.join(str(root), "records")
        for folder in (self.work, self.records, os.path.join(str(root), "home")):
            os.makedirs(folder)
        self.exit_code = None
        env = {"PATH": os.environ.get("PATH", ""),
               "HOME": os.path.join(str(root), "home"), "TERM": "xterm",
               "XWPE_BIN": _write_launcher(root, self.records)}
        env["WPE_BIN"] = env["XWPE_BIN"]
        env.update(CANARY_VALUES)
        self.log = os.path.join(str(root), "inner.log")
        argv = [sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
                "--basetemp", os.path.join(self.work, "base"), "-x",
                os.path.join(gui_owner.HERE, suite, INNER_ROW)]
        with open(self.log, "wb") as log:
            self.proc = subprocess.Popen(argv, cwd=self.work, env=env,
                                         stdin=subprocess.DEVNULL, stdout=log,
                                         stderr=subprocess.STDOUT,
                                         start_new_session=True)

    def text(self):
        """The end of what the inner pytest printed, for a failure message."""
        with open(self.log, encoding="utf-8", errors="replace") as fh:
            return "exit %s:\n%s" % (self.proc.poll(), fh.read()[-1500:])

    def wait_record(self):
        """The stand-in's record, once it exists; fails if the run ended
        without starting one."""
        return gui_owner.wait_for(
            lambda: _read_record(self.records) or self.proc.poll() is not None,
            "the inner run to start its editor", INNER_START_S,
            state_fn=self.text) and _read_record(self.records)

    def wait_exit(self):
        """Wait for the inner pytest to end, and note how."""
        try:
            self.exit_code = self.proc.wait(timeout=gui_owner.scaled(INNER_END_S))
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.exit_code = self.proc.wait()
        return self.exit_code

    def settle(self, editor_pid):
        """What is left SETTLE_S (scaled) after the inner pytest ended: the
        editor still running, other processes it started and left, and the
        X server files of its display."""
        def state():
            return {"editor_alive": _running(editor_pid),
                    "leftovers": [p for p in _processes_under(self.work)
                                  if p[0] != editor_pid],
                    "server_files": self.server_files()}

        def settled():
            now = state()
            return not now["editor_alive"] and not now["leftovers"] \
                and not now["server_files"]
        try:
            gui_owner.wait_for(settled, "the run's processes and display to be gone",
                               SETTLE_S)
        except gui_owner.OwnerTimeout:
            pass
        return state()

    def server_files(self):
        """The lock file and socket of the run's display that still exist
        (known from the editor's record, which names the run's X display)."""
        rec = _read_record(self.records)
        display = (rec or {}).get("values", {}).get("DISPLAY", "")
        number = int(display[1:]) if re.fullmatch(r":\d+", display) else None
        if number is None:
            return []
        return [path % number for path in SERVER_FILES
                if os.path.exists(path % number)]

    def end(self):
        """End what the run left: the inner pytest, then every process still
        under its folder, each by its pid, then the files its servers and
        its runtime folder left on a hard kill."""
        rec = _read_record(self.records)
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=gui_owner.scaled(INNER_END_S))
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        for pid, _ in _processes_under(self.work):
            _kill_by_pid(pid, self.work)
        for path in self.server_files():
            os.unlink(path)
        runtime = (rec or {"values": {}})["values"].get("XDG_RUNTIME_DIR", "")
        if os.path.basename(runtime).startswith(RUNTIME_PREFIX) and os.path.isdir(runtime):
            gui_owner._remove_tree(runtime)


class _InnerRuns:
    """The plain failing run of each suite, made once for the rows that
    only read what it left."""

    def __init__(self, root):
        self.root = root
        self.runs = {}

    def get(self, suite):
        """The finished run of suite (its editor never showed a window), with
        what was left after it."""
        if suite not in self.runs:
            run = _InnerRun(self.root / suite, suite)
            self.runs[suite] = run
            run.record = run.wait_record()
            run.wait_exit()
            run.after = run.settle(run.record["pid"]) if run.record else None
        return self.runs[suite]

    def end(self):
        """End everything the runs left."""
        for run in self.runs.values():
            run.end()


@pytest.fixture(scope="module")
def inner_runs(tmp_path_factory):
    """The failing runs of the graphical suites, for the rows below."""
    runs = _InnerRuns(tmp_path_factory.mktemp("inner"))
    yield runs
    runs.end()


def _signalled_run(tmp_path, request, suite, sig):
    """A run of suite, sent sig once its editor has started, and what it
    left after it was gone."""
    run = _InnerRun(tmp_path, suite)
    request.addfinalizer(run.end)
    run.record = run.wait_record()
    assert run.record, "the editor never started: " + run.text()
    run.proc.send_signal(sig)
    run.wait_exit()
    run.after = run.settle(run.record["pid"])
    return run


def _describe_after(run):
    """What the run left, for a failure message."""
    return "editor %d %s, other processes %s, server files %s; inner run %s" % (
        run.record["pid"], "STILL RUNNING" if run.after["editor_alive"] else "gone",
        run.after["leftovers"], run.after["server_files"], run.text())


# @scenario SC-harness-owner-33
@pytest.mark.parametrize("suite", SUITES)
def test_a_graphical_row_that_fails_before_yield_leaves_no_editor(suite, inner_runs):
    """A row whose fixture fails before its first yield (the window never
    appears) must still have its editor ended."""
    run = inner_runs.get(suite)
    assert run.record, "the editor never started: " + run.text()
    assert run.exit_code != 0, "the inner row did not fail: " + run.text()
    assert not run.after["editor_alive"], \
        "the editor outlived its failed row: " + _describe_after(run)


# @scenario SC-harness-owner-34
@pytest.mark.parametrize("suite", SUITES)
def test_a_gui_editor_never_sees_the_callers_display(suite, inner_runs):
    """The caller's display and session variables, and a key in its
    environment, do not reach the editor; the display it has is the run's
    own."""
    rec = inner_runs.get(suite).record
    assert rec, "the editor never started: " + inner_runs.get(suite).text()
    names, values = rec["names"], rec["values"]
    assert values.get("DISPLAY") != CANARY_VALUES["DISPLAY"], \
        "the editor was given the caller's DISPLAY"
    assert values.get("WAYLAND_DISPLAY") != CANARY_VALUES["WAYLAND_DISPLAY"], \
        "the editor was given the caller's WAYLAND_DISPLAY"
    carried = sorted(set(CANARY_VALUES) - {"DISPLAY", "WAYLAND_DISPLAY"} & names)
    assert not carried, "the machine's variables reached the editor: %s" % carried


# @scenario SC-harness-owner-35
@pytest.mark.parametrize("suite", SUITES)
def test_a_killed_run_leaves_no_server_and_no_editor(suite, tmp_path, request):
    """A pytest that is killed outright takes its display server, its
    compositor and its editor along, and the display files with them."""
    run = _signalled_run(tmp_path, request, suite, signal.SIGKILL)
    assert not run.after["editor_alive"] and not run.after["leftovers"] \
        and not run.after["server_files"], _describe_after(run)


# @scenario SC-harness-owner-36
@pytest.mark.parametrize("suite", SUITES)
def test_an_interrupted_run_leaves_no_editor(suite, tmp_path, request):
    """An interrupt that lands while the fixture waits for the window ends
    the editor the fixture had started."""
    run = _signalled_run(tmp_path, request, suite, signal.SIGINT)
    assert not run.after["editor_alive"] and not run.after["leftovers"], \
        _describe_after(run)


def _scan(folder, pattern, skip=()):
    """The lines of the .py files under folder that match pattern, as
    "path:line: text"; folders in skip are not entered."""
    found = []
    for root, dirs, files in os.walk(folder):
        dirs[:] = [d for d in dirs if d not in skip and not d.startswith(".")]
        for name in sorted(files):
            if not name.endswith(".py") or os.path.join(root, name) == THIS:
                continue
            with open(os.path.join(root, name), encoding="utf-8") as fh:
                for number, line in enumerate(fh, 1):
                    if pattern.search(line):
                        found.append("%s:%d: %s" % (os.path.relpath(
                            os.path.join(root, name), gui_owner.HERE), number,
                            line.strip()))
    return found


WL_DISPLAY_VARIABLE = re.compile(r"XWPE_TEST_WL_DISPLAY")


# @scenario SC-harness-owner-37
@pytest.mark.parametrize("suite", SUITES)
def test_a_fixed_display_number_is_not_used(suite):
    """No suite reads a constant display number, and none takes one from the
    environment: the X server picks its own."""
    found = _scan(os.path.join(gui_owner.HERE, suite), BARE_DISPLAY)
    if suite == "wayland":
        found += _scan(gui_owner.HERE, WL_DISPLAY_VARIABLE, ("harness",))
    assert not found, "a fixed display is still used:\n  " + "\n  ".join(found)


@pytest.fixture
def current_owner(owner):
    """The row's owner as the process's current one, for the module-level
    helpers (tool_env, run_tool), put back afterwards."""
    previous = gui_owner._CURRENT
    gui_owner.install(owner)
    yield owner
    gui_owner.install(previous)


def _pty_pair(request):
    """A pty whose master the row reads or sizes; both ends are closed with
    the row."""
    master, slave = pty.openpty()
    request.addfinalizer(lambda: _close_quietly(master))
    request.addfinalizer(lambda: _close_quietly(slave))
    return master, slave


def _close_quietly(fd):
    """Close fd; closing it twice is not an error."""
    try:
        os.close(fd)
    except OSError:
        pass


def _start_terminal_standin(owner, tmp_path, request, **kwargs):
    """Start the recording stand-in as a terminal editor; return the owned
    process, the pty master and the parsed record."""
    master, slave = _pty_pair(request)
    records = tmp_path / "records"
    home = tmp_path / "home"
    home.mkdir()
    owned = gui_owner.Terminal.start(
        owner, _write_launcher(tmp_path, records), [], str(tmp_path), str(home),
        master, slave, label=request.node.nodeid, request=request, **kwargs)
    return owned, master, _wait_record(records)


# @scenario SC-harness-owner-38
def test_a_terminal_editor_gets_only_the_allow_list(owner, tmp_path, request, monkeypatch):
    for name, value in CANARY_VALUES.items():
        monkeypatch.setenv(name, value)
    monkeypatch.setenv("LINES", "40")
    monkeypatch.setenv("COLUMNS", "100")
    owned, master, rec = _start_terminal_standin(owner, tmp_path, request)
    allowed = EDITOR_PINNED | LAUNCHER_OWN | set(gui_owner.PASS_THROUGH)
    assert rec["names"] - allowed == set(), \
        "names outside the allow-list: %s" % sorted(rec["names"] - allowed)
    assert not rec["names"] & {"DISPLAY", "WAYLAND_DISPLAY", "LINES", "COLUMNS"}
    assert rec["values"]["TERM"] == gui_owner.TERM_NAME
    assert rec["values"]["XWPE_KEYS"] == "borland"
    assert not os.path.exists(tmp_path / "home" / gui_owner.INHERITED_FILE)


# @scenario SC-harness-owner-39
def test_a_terminal_editor_has_the_pty_as_its_size_and_controlling_terminal(
        owner, tmp_path, request):
    """The pty gets its size before the start, the editor leads a session of
    its own with the pty as its controlling terminal, and a size set later
    reaches it as SIGWINCH."""
    owned, master, rec = _start_terminal_standin(owner, tmp_path, request,
                                                 size=(100, 40))
    assert rec["size"] == "100x40"
    assert rec["sid"] == rec["pid"] and rec["tty_nr"] != 0
    gui_owner.set_pty_size(master, 90, 25)
    gui_owner.wait_for(lambda: _read_record(tmp_path / "records")["winch"],
                       "the editor to see the new size", READY_S)


# @scenario SC-harness-owner-40
def test_a_terminal_editor_whose_input_is_a_pipe_still_has_its_terminal(
        owner, tmp_path, request):
    """A row that feeds the editor a pipe keeps the pty for the screen: the
    terminal is then taken from stdout."""
    reader, writer = os.pipe()
    request.addfinalizer(lambda: _close_quietly(writer))
    try:
        owned, master, rec = _start_terminal_standin(owner, tmp_path, request,
                                                     stdin_fd=reader)
    finally:
        os.close(reader)
    assert rec["stdin_tty"] == 0 and rec["tty_nr"] != 0 and rec["size"] == "80x30"


# @scenario SC-harness-owner-41
def test_a_row_env_reaches_the_editor_under_the_rows_own_extra_env(
        owner, tmp_path, request):
    """A fixture's variables (a debugger interface) reach every editor of
    the row, the row's own extra_env wins over them, and they are gone
    when the row ends."""
    owner.row_env(request)["XWPE_GDB_INTERFACE"] = "mi"
    owned, master, rec = _start_terminal_standin(owner, tmp_path, request)
    assert rec["values"]["XWPE_GDB_INTERFACE"] == "mi"
    other = tmp_path / "second"
    other.mkdir()
    owned, master, rec = _start_terminal_standin(
        owner, other, request, extra_env={"XWPE_GDB_INTERFACE": "cli"})
    assert rec["values"]["XWPE_GDB_INTERFACE"] == "cli"


class _FakeRow:
    """What the owner takes from a pytest request: its id and finalizers."""

    class node:
        nodeid = "tests/test_fake.py::row"

    def __init__(self):
        self.finalizers = []

    def addfinalizer(self, function):
        """Record a finalizer, as pytest does."""
        self.finalizers.append(function)


# @scenario SC-harness-owner-42
def test_the_row_env_is_cleared_when_the_row_ends(owner):
    row = _FakeRow()
    owner.row_env(row)["XWPE_UI_TRACE"] = "/x"
    assert owner.merged_env({"A": "1"}) == {"XWPE_UI_TRACE": "/x", "A": "1"}
    for function in row.finalizers:
        function()
    assert owner.merged_env(None) == {}


# @scenario SC-harness-owner-43
def test_a_row_can_inherit_one_more_variable_and_only_its_name_is_recorded(
        owner, tmp_path, monkeypatch):
    monkeypatch.setenv("TPAS_KEY", "s3cret-value")
    home = tmp_path / "home"
    home.mkdir()
    done = gui_owner.Terminal.run(
        owner, sys.executable, ["-E", "-s", THIS, "--print-env", "TPAS_KEY",
                                "TPAS_UNSET", "DISPLAY"], READY_S,
        inherit=("TPAS_KEY", "TPAS_UNSET"), home=str(home))
    assert done.stdout.decode().split("\n")[:3] == [
        "TPAS_KEY=s3cret-value", "TPAS_UNSET unset", "DISPLAY unset"]
    with open(home / gui_owner.INHERITED_FILE, encoding="utf-8") as fh:
        recorded = fh.read()
    assert recorded.split() == ["TPAS_KEY"] and "s3cret" not in recorded


# @scenario SC-harness-owner-44
def test_a_row_cannot_inherit_the_machines_session_or_home(current_owner):
    for name in gui_owner.NEVER_INHERITED + gui_owner.NEVER_PASSED:
        with pytest.raises(gui_owner.OwnerRefused):
            gui_owner.tool_env(inherit=(name,))
        with pytest.raises(gui_owner.OwnerRefused):
            gui_owner.run_tool(STANDIN + ["--sleeper"], READY_S, inherit=(name,))
    assert not current_owner.ledger.pids(), "a refused run still started something"


# @scenario SC-harness-owner-45
def test_tool_env_is_built_from_the_allow_list_with_the_owners_home(
        current_owner, tmp_path, monkeypatch):
    monkeypatch.setenv("DISPLAY", ":77")
    for name, value in CANARY_VALUES.items():
        monkeypatch.setenv(name, value)
    env = gui_owner.tool_env()
    assert set(env) - set(gui_owner.PASS_THROUGH) <= {
        "LC_ALL", "LANG", "SHELL", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME"}
    assert env["HOME"] == current_owner.tool_home()
    assert os.path.realpath(env["HOME"]) != os.path.realpath(os.path.expanduser("~"))
    display = _fake_x_display(current_owner, tmp_path)
    assert gui_owner.tool_env(display)["DISPLAY"] == FAKE_X_DISPLAY
    for extra in ({"HOME": os.path.expanduser("~")}, {"DISPLAY": ":77"},
                  {"WAYLAND_SOCKET": "3"}):
        with pytest.raises(gui_owner.OwnerRefused):
            gui_owner.tool_env(extra_env=extra)
    with pytest.raises(gui_owner.OwnerRefused):
        gui_owner.tool_env(":77")


# @scenario SC-harness-owner-46
def test_run_tool_returns_the_output_and_takes_a_home_and_extra_variables(
        current_owner, tmp_path):
    home = tmp_path / "home"
    home.mkdir()
    done = gui_owner.run_tool(STANDIN + ["--print-env", "HOME", "TPAS_X"],
                              READY_S, home=str(home),
                              extra_env={"TPAS_X": "1"})
    assert done.returncode == 0
    assert done.stdout.decode().split("\n")[:2] == ["HOME=%s" % home, "TPAS_X=1"]
    assert not current_owner.ledger.pids(), "an ended tool is still on the ledger"


# @scenario SC-harness-owner-47
def test_run_tool_ends_a_tool_that_outlives_its_deadline_and_names_it(current_owner):
    with pytest.raises(gui_owner.OwnerTimeout) as raised:
        gui_owner.run_tool(STANDIN + ["--sleeper"], 0.5)
    assert "python" in str(raised.value) and "0.5 s" in str(raised.value)
    assert not current_owner.ledger.pids(), "the late tool is still running"


# @scenario SC-harness-owner-48
def test_run_tool_check_raises_on_a_failing_tool(current_owner):
    with pytest.raises(subprocess.CalledProcessError) as raised:
        gui_owner.run_tool(STANDIN + ["--no-such-mode"], READY_S, check=True)
    assert raised.value.returncode == 2 and b"usage" in raised.value.stderr


# @scenario SC-harness-owner-49
def test_run_nested_passes_the_harness_variables_and_nothing_of_the_session(
        current_owner, monkeypatch):
    monkeypatch.setenv("XWPE_BIN", "/the/editor")
    monkeypatch.setenv("XWPE_TEST_OS", "somewhere")
    monkeypatch.setenv("DISPLAY", ":77")
    done = gui_owner.run_nested(
        STANDIN + ["--print-env", "XWPE_BIN", "XWPE_TEST_OS", "DISPLAY"], READY_S)
    assert done.stdout.decode().split("\n")[:3] == [
        "XWPE_BIN=/the/editor", "XWPE_TEST_OS=somewhere", "DISPLAY unset"]


# @scenario SC-harness-owner-50
def test_process_running_says_ended_for_a_zombie_and_a_reused_pid(tmp_path):
    live = subprocess.Popen(STANDIN + ["--sleeper"])
    done = subprocess.Popen(["true"])
    try:
        started = gui_owner.start_ticks(live.pid)
        assert gui_owner.process_running(live.pid)
        assert gui_owner.process_running(live.pid, started)
        assert not gui_owner.process_running(live.pid, started + 1), \
            "a pid with another start time was taken for the same process"
        gui_owner.wait_for(lambda: procinfo.run_state(done.pid) == procinfo.ZOMBIE,
                           "the short process to end", READY_S)
        assert not gui_owner.process_running(done.pid), "a zombie counts as running"
        assert procinfo.run_state(done.pid) == procinfo.ZOMBIE, \
            "a zombie was reaped though the caller did not name its start time"
        assert done.wait() == 0, "the row's own child lost its exit status"
    finally:
        live.kill()
        live.wait()
        done.wait()
    assert not gui_owner.process_running(live.pid)


# @scenario SC-harness-owner-51
def test_process_running_reaps_an_adopted_orphan_by_its_pid_and_start_time(
        owner, tmp_path, request):
    editor, orphan = _orphan_zombie(owner, tmp_path, request)
    started = editor.tree.started[orphan]
    own = subprocess.Popen(["true"])
    try:
        gui_owner.wait_for(lambda: procinfo.run_state(own.pid) == procinfo.ZOMBIE,
                           "the row's own child to end", READY_S)
        assert not gui_owner.process_running(orphan)
        assert procinfo.run_state(orphan) == procinfo.ZOMBIE
        assert not gui_owner.process_running(orphan, started)
        assert procinfo.run_state(orphan) == procinfo.GONE
        assert procinfo.run_state(own.pid) == procinfo.ZOMBIE, \
            "another child of pytest was reaped"
    finally:
        own.wait()


# @scenario SC-harness-owner-52
def test_a_host_without_pidfd_gets_a_poll_watch_and_the_same_sweep(
        owner, tmp_path, request, monkeypatch):
    """A container may filter pidfd_open: no row errors with an OSError, the
    owner watches by the process table, and a leftover is found, named and
    ended as with a pidfd."""
    def refused(pid, flags=0):
        raise OSError(errno.ENOSYS, "pidfd_open filtered")
    grandchild = None
    try:
        with monkeypatch.context() as filtered:
            filtered.setattr(os, "pidfd_open", refused)
            assert isinstance(gui_owner.watch_process(os.getpid()),
                              gui_owner._PollWatch)
            editor = _start_standin(owner, tmp_path, request,
                                    ["--leaker", LEAK_COMM])
            assert isinstance(editor.watch, gui_owner._PollWatch)
            grandchild = _wait_grandchild(editor)
            with pytest.raises(gui_owner.LeakedProcesses) as raised:
                editor.end()
            assert "pid %d comm %s" % (grandchild, LEAK_COMM) in str(raised.value)
        assert procinfo.run_state(grandchild) == procinfo.GONE, \
            "the grandchild survived its row"
        assert not editor.running()
    finally:
        if grandchild is not None and procinfo.run_state(grandchild) != procinfo.GONE:
            _end_leftover(os.pidfd_open(grandchild))


# @scenario SC-harness-owner-53
def test_a_pidfd_error_that_is_not_unavailability_is_still_an_error(monkeypatch):
    def gone(pid, flags=0):
        raise OSError(errno.ESRCH, "no such process")
    monkeypatch.setattr(os, "pidfd_open", gone)
    with pytest.raises(OSError):
        gui_owner.watch_process(os.getpid())


# @scenario SC-harness-owner-54
def test_current_is_the_installed_owner_and_a_script_gets_one_of_its_own(owner):
    previous = gui_owner._CURRENT
    try:
        gui_owner.install(owner)
        assert gui_owner.current() is owner
    finally:
        gui_owner.install(previous)
    report = subprocess.run([sys.executable, "-E", "-s", gui_owner.__file__,
                             "--report-current"], capture_output=True,
                            text=True, timeout=gui_owner.scaled(READY_S))
    assert report.stdout.strip() == "current begun 1 same 1", report.stderr


# @scenario SC-harness-owner-62
def test_process_running_does_not_reap_a_process_the_owner_started(
        owner, tmp_path, request):
    previous = gui_owner._CURRENT
    gui_owner.install(owner)
    try:
        owned = owner.start_service(
            [sys.executable, "-E", "-s", "-c", "raise SystemExit(3)"],
            "exits 3", str(tmp_path), str(tmp_path), request=request)
        started = gui_owner.start_ticks(owned.pid)
        gui_owner.wait_for(
            lambda: procinfo.run_state(owned.pid) == procinfo.ZOMBIE,
            "the owned process to end", READY_S)
        assert not gui_owner.process_running(owned.pid, started)
        assert procinfo.run_state(owned.pid) == procinfo.ZOMBIE, \
            "the owner's own child was reaped by process_running"
        assert owned.proc.wait() == 3, "the owned process lost its exit status"
    finally:
        gui_owner.install(previous)


# @scenario SC-harness-owner-55
def test_running_and_entries_since_name_what_a_row_started_and_end_since_ends_it(
        owner, tmp_path, request):
    before = owner.ledger.pids()
    first = _start_standin(owner, tmp_path, request, ["--sleeper"])
    second = _start_standin(owner, tmp_path, request, ["--sleeper"])
    assert {o.pid for o in owner.entries_since(before)} >= {first.pid, second.pid}
    assert {first.pid, second.pid} <= {o.pid for o in owner.running("xwpe")}
    owner.end_since(before)
    assert not first.running() and not second.running()
    assert not owner.entries_since(before)


# @scenario SC-harness-owner-56
def test_end_since_errors_the_row_naming_what_a_process_left(owner, tmp_path, request):
    before = owner.ledger.pids()
    editor = _start_standin(owner, tmp_path, request, ["--leaker", LEAK_COMM])
    grandchild = _wait_grandchild(editor)
    pidfd = os.pidfd_open(grandchild)
    try:
        with pytest.raises(gui_owner.LeakedProcesses) as raised:
            owner.end_since(before)
        assert "pid %d comm %s" % (grandchild, LEAK_COMM) in str(raised.value)
        assert _ended(pidfd)
    finally:
        _end_leftover(pidfd)


# @scenario SC-harness-owner-57
def test_the_root_fixtures_are_written_and_not_installed():
    """The owner's session and row fixtures exist in tests/conftest.py but
    are not autouse: turning them on puts every terminal row under the
    subreaper, which the terminal suite's pass does after its liveness
    checks moved to process_running."""
    with open(os.path.join(gui_owner.HERE, "conftest.py"), encoding="utf-8") as fh:
        text = fh.read()
    assert "def process_owner(" in text and "def end_row_processes(" in text
    assert not re.search(r"@pytest\.fixture\([^)]*autouse", text)


# @scenario SC-harness-owner-58
@needs("Xvfb", "weston", "xdotool")
def test_the_owner_starts_an_x_server_and_a_compositor_it_registers(
        current_owner, tmp_path, request):
    """The starters both graphical suites use: Xvfb on a number of its own,
    weston on it in a runtime folder the owner made, both registered and
    ended with the row."""
    display = gui_owner.start_x_server(current_owner, request, str(tmp_path))
    assert re.fullmatch(r":\d+", display.name)
    assert current_owner.x_display(display.name) is display
    done = gui_owner.run_tool(["xdotool", "getdisplaygeometry"], READY_S,
                              display=display)
    assert done.stdout.split() == [b"1024", b"768"]
    wayland = gui_owner.start_compositor(current_owner, request, display,
                                         str(tmp_path), 640, 480)
    assert os.path.exists(wayland.socket_path())
    assert wayland.runtime_dir in current_owner.runtime_dirs()
    assert current_owner.wayland_display(wayland) is wayland
    env = gui_owner.tool_env(wayland)
    assert env["WAYLAND_DISPLAY"] == wayland.socket
    assert env["DISPLAY"] == display.name


# @scenario SC-harness-owner-60
def test_the_x11_conftest_takes_the_starters_from_the_owner():
    """The X11 suite keeps its names for the moved code, so a row that
    imports PreviousRowLeft from it still gets the one class."""
    with open(os.path.join(gui_owner.HERE, "x11", "conftest.py"), encoding="utf-8") as fh:
        text = fh.read()
    assert "owner_api.start_x_server(" in text
    assert "from gui_owner import PreviousRowLeft" in text
    assert "subprocess.Popen" not in text


# @scenario SC-harness-owner-59
@PASS_C
def test_a_wpe_session_editor_gets_only_the_allow_list(tmp_path, monkeypatch):
    """The terminal driver gives its editor the allow-list and no display."""
    wpe_driver = pytest.importorskip("wpe_driver")
    records = tmp_path / "records"
    monkeypatch.setattr(wpe_driver, "WPE_BIN", _write_launcher(tmp_path, records))
    for name, value in CANARY_VALUES.items():
        monkeypatch.setenv(name, value)
    workdir = tmp_path / "work"
    workdir.mkdir()
    with wpe_driver.WpeSession(str(workdir), "int main(){}\n", wait=0.2):
        rec = _wait_record(records)
        allowed = EDITOR_PINNED | LAUNCHER_OWN | set(gui_owner.PASS_THROUGH)
        assert rec["names"] - allowed == set(), \
            "names outside the allow-list: %s" % sorted(rec["names"] - allowed)
        assert not rec["names"] & {"DISPLAY", "WAYLAND_DISPLAY"}


if __name__ == "__main__":
    _run_standin(sys.argv[1:])
