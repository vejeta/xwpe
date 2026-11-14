"""The process owner of the graphical suites, checked without a display.

Every row here starts stand-ins instead of an editor and a server: this
file run as a program is the stand-in.  The rows show what a graphical row
gets from the owner: what an editor leaves behind is found, ended and
named; what the row started itself is left alone; a display the owner did
not start is refused; a wait that runs out names its event and its scale.
"""
import ctypes
import os
import re
import select
import signal
import subprocess
import sys
import threading
import time

import pytest

import gui_owner
import procinfo

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


def _run_standin(argv):
    """Dispatch this file run as a program."""
    if argv[:1] == ["--leaker"] and len(argv) == 2:
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
                     "| --sleeper | --env-names\n")
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
              "XWPE_LIB", "XWPE_FONT_SIZE", "XWPE_LSP_NO_EAGER", "XWPE_X_DUMP"}
    # The interpreter may add its own (LC_CTYPE when it coerces a locale).
    interpreter_own = {"LC_CTYPE"}
    assert names - pinned - interpreter_own - set(gui_owner.PASS_THROUGH) == set()
    assert "DISPLAY=%s" % FAKE_X_DISPLAY in lines
    assert editor.env["XWPE_BACKEND"] == "x11"
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


X11_DIR = os.path.join(gui_owner.HERE, "x11")
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
    for name in sorted(os.listdir(X11_DIR)):
        if not name.endswith(".py"):
            continue
        with open(os.path.join(X11_DIR, name), encoding="utf-8") as fh:
            for number, line in enumerate(fh, 1):
                if BARE_DISPLAY.search(line):
                    found.append("tests/x11/%s:%d: %s" % (name, number, line.strip()))
    assert not found, "a constant display is still read:\n  " + "\n  ".join(found)
    for suite in CONFTESTS:
        path = os.path.join(gui_owner.HERE, suite, "conftest.py")
        with open(path, encoding="utf-8") as fh:
            assert "def display_name():" in fh.read(), \
                "tests/%s/conftest.py has no display_name()" % suite


if __name__ == "__main__":
    _run_standin(sys.argv[1:])
