"""What the process table says about the editor's children, on any Unix.

The debugger tests ask the same few questions of the process table: which
of the editor's direct children is the debugger it started, how much CPU a
process has used, whether a process is running, a zombie or gone, and what
its command line is.  Linux answers them from /proc; systems without /proc
(macOS, the BSDs) answer them from ps(1).

The same on both: the set of direct children, the run state letter (R, S,
Z, ...; GONE once reaped), CPU time in clock ticks, and that an ended or
zombie child is never reported as running.  Different: the name a child
runs under is the kernel's command name on Linux, cut to 15 characters,
but the full base name of what was executed elsewhere (see command_run);
and ps(1) prints a command line with its words joined by blanks, so
without /proc a word that holds a blank is taken as two words when the
name is worked out from it.
"""
import os
import subprocess
import sys

PROC_ROOT = "/proc"
GONE = "gone"            # run state of a process no longer in the table
ZOMBIE = "Z"             # run state of a child that ended but is not reaped
RUNNING = "R"            # run state of a process on a CPU or ready for one

# The interpreter path a "#!" line names is at most this many words before
# the script's own path on the command line: the interpreter and one option.
SHEBANG_MAX_WORDS = 2
# The macOS framework Python's `python3` is a launcher that re-executes
# Python.app under this path and records the path it was run as in
# LAUNCHER_VAR, so the command the editor ran is found there.
FRAMEWORK_PYTHON_SUFFIX = "/Python.app/Contents/MacOS/Python"
LAUNCHER_VAR = "__PYVENV_LAUNCHER__"
SECONDS_PER_MINUTE = 60
SECONDS_PER_HOUR = 3600
HOURS_PER_DAY = 24


def has_procfs():
    """True where /proc describes processes the Linux way, i.e. on Linux;
    elsewhere the answers come from ps(1).  NetBSD mounts a /proc with a
    Linux-like stat file, but its command name for a "#!" script is the
    interpreter's and its status file has no signal masks."""
    return (sys.platform.startswith("linux")
            and os.path.exists(os.path.join(PROC_ROOT, "self", "stat")))


def _proc_stat_fields(pid):
    """The fields of /proc/<pid>/stat after the command name, starting with
    the run state, or None once the process is gone.  The name is skipped
    by its closing parenthesis, so a name with blanks does not shift them."""
    try:
        with open(os.path.join(PROC_ROOT, str(pid), "stat")) as f:
            stat = f.read()
    except OSError:
        return None
    return stat[stat.rfind(")") + 2:].split()


def _proc_comm(pid):
    """The command name the kernel keeps for pid in /proc (what Linux ps
    prints as comm), or None once the process is gone."""
    try:
        with open(os.path.join(PROC_ROOT, str(pid), "stat")) as f:
            stat = f.read()
    except OSError:
        return None
    return stat[stat.find("(") + 1:stat.rfind(")")]


def _ps_field(pid, field, *options):
    """One ps(1) column for pid, stripped, or None once pid is gone."""
    argv = ["ps"] + list(options) + ["-p", str(pid), "-o", field + "="]
    result = subprocess.run(argv, capture_output=True, text=True)
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def run_state(pid):
    """The single-letter run state of pid (R, S, D, T, I, Z, ...), or GONE.

    A test needs it to tell a process that still runs from one that ended:
    a zombie (ZOMBIE) has ended although it is still listed."""
    if has_procfs():
        fields = _proc_stat_fields(pid)
        if fields is None:
            return GONE
        return fields[0]
    state = _ps_field(pid, "state")
    if not state:
        return GONE
    return state[:1]


def _ticks_from_ps_time(text):
    """Clock ticks in a ps(1) cumulative CPU time: [[dd-]hh:]mm:ss[.cc]."""
    days = 0
    if "-" in text:
        day_text, text = text.split("-", 1)
        days = int(day_text)
    parts = text.split(":")
    hours = days * HOURS_PER_DAY
    if len(parts) == 3:
        hours += int(parts[0])
        parts = parts[1:]
    seconds = (hours * SECONDS_PER_HOUR + int(parts[0]) * SECONDS_PER_MINUTE
               + float(parts[1]))
    return int(seconds * os.sysconf("SC_CLK_TCK"))


def cpu_ticks(pid):
    """User plus system CPU time of pid in clock ticks, or None once it is
    gone.

    A test samples it twice around a wait to show the editor sat idle
    instead of spinning in a busy loop."""
    if has_procfs():
        fields = _proc_stat_fields(pid)
        if fields is None:
            return None
        return int(fields[11]) + int(fields[12])
    text = _ps_field(pid, "time")
    if not text:
        return None
    try:
        return _ticks_from_ps_time(text)
    except ValueError:
        return None


def command_line(pid):
    """The command line of pid, its words joined by blanks, or None once it
    is gone.

    A test needs it to know a PID it recorded still belongs to the process
    it started (its command line names the test's own directory) and was
    not reused by another."""
    if has_procfs():
        try:
            with open(os.path.join(PROC_ROOT, str(pid), "cmdline"), "rb") as f:
                raw = f.read()
        except OSError:
            return None
        return raw.replace(b"\0", b" ").decode("utf-8", "surrogateescape")
    return _ps_field(pid, "args", "-ww")


def _child_pids(ppid):
    """PIDs of every direct child of ppid, whatever it runs."""
    if has_procfs():
        return _proc_child_pids(ppid)
    return _ps_child_pids(ppid)


def _proc_child_pids(ppid):
    """Direct children of ppid, read from /proc."""
    pids = []
    for entry in os.listdir(PROC_ROOT):
        if not entry.isdigit():
            continue
        fields = _proc_stat_fields(int(entry))
        if fields is not None and int(fields[1]) == ppid:
            pids.append(int(entry))
    return pids


def _ps_child_pids(ppid):
    """Direct children of ppid, read from ps(1)."""
    out = subprocess.run(["ps", "-A", "-o", "pid=", "-o", "ppid="],
                         capture_output=True, text=True).stdout
    pids = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 2 and int(parts[1]) == ppid:
            pids.append(int(parts[0]))
    return pids


def _shebang_words(path):
    """The interpreter and option a "#!" file names, as words; [] for a
    file that is not a script or cannot be read."""
    try:
        with open(path, "rb") as f:
            first = f.readline()
    except OSError:
        return []
    if not first.startswith(b"#!"):
        return []
    return first[2:].decode("utf-8", "replace").strip().split(None, 1)


def _script_run(argv):
    """The script's path when argv is the kernel's rewrite of a "#!" script
    (interpreter first, then the script), else None."""
    for words in range(1, SHEBANG_MAX_WORDS + 1):
        if len(argv) > words and _shebang_words(argv[words]) == argv[:words]:
            return argv[words]
    return None


def _framework_launcher_path(pid, argv):
    """The path the macOS framework Python launcher was run as, when pid is
    the Python.app it re-executed, else None."""
    if not argv[0].endswith(FRAMEWORK_PYTHON_SUFFIX):
        return None
    words = _ps_field(pid, "args", "-E", "-ww")
    if not words:
        return None
    prefix = LAUNCHER_VAR + "="
    for word in words.split():
        if word.startswith(prefix):
            return word[len(prefix):]
    return None


def _ps_command_run(pid):
    """The name of the command that was run to start pid, from ps(1)."""
    words = command_line(pid)
    if not words:
        return None
    argv = words.split()
    launcher = _framework_launcher_path(pid, argv)
    if launcher is not None:
        return os.path.basename(launcher)
    script = _script_run(argv)
    if script is not None:
        return os.path.basename(script)
    return os.path.basename(argv[0])


def command_run(pid):
    """The name pid runs under, or None once it is gone.

    On Linux it is the kernel's command name: the base name of the program
    or script that was executed, cut to its first 15 characters.  Without
    /proc it is the base name of what was passed to exec (the script's own
    name for a "#!" script, the launcher's for the framework Python), not
    cut.  The names the debugger tests look for are shorter than 15
    characters, so both give the same answer for them."""
    if has_procfs():
        return _proc_comm(pid)
    return _ps_command_run(pid)


def _live_and_named(pid, name):
    """True when pid has not ended (neither gone nor a zombie) and runs as
    `name`: an ended debugger the editor has not reaped yet is not there."""
    if run_state(pid) in (GONE, ZOMBIE):
        return False
    return command_run(pid) == name


def children_named(ppid, name):
    """PIDs of ppid's direct children started as the command `name` that
    have not ended.

    A test needs it to find the debugger the editor started (and only that
    child: not another child, not a grandchild), or to see it has ended."""
    return [pid for pid in _child_pids(ppid) if _live_and_named(pid, name)]


def descendants(ppid):
    """PIDs of every child of ppid, of its children, and so on, that have
    not ended.

    A test needs it to see a debugger's whole tree (the debugger, the
    program it launched, what that started) and to check, once a session is
    over, that none of it is left behind."""
    found = []
    pending = [ppid]
    while pending:
        for pid in _child_pids(pending.pop()):
            if run_state(pid) in (GONE, ZOMBIE):
                continue
            found.append(pid)
            pending.append(pid)
    return found


def all_ended(pids):
    """True when none of pids is still alive: each is gone or a zombie."""
    return all(run_state(pid) in (GONE, ZOMBIE) for pid in pids)
