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
import re
import shutil
import signal
import stat
import subprocess
import sys

PROC_ROOT = "/proc"

# The system the tests run on: the one place a platform name is read.
# IS_LINUX and IS_MACOS are the real system.  MACOS also answers true on any
# system when XWPE_TEST_OS=macos, so a row that depends on macOS (the name of
# a locale, say) can be exercised on Linux; no key and no process is ever
# treated differently because of it.
IS_LINUX = sys.platform.startswith("linux")
IS_MACOS = sys.platform == "darwin"
MACOS = IS_MACOS or os.environ.get("XWPE_TEST_OS") == "macos"

GONE = "gone"           # run state of a process no longer in the table
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


def _build_config_h():
    """The config.h of the build the tests run: the one beside the editor
    binary (WPE_BIN, or ../wpe).  None for an installed binary (autopkgtest),
    which has no build folder."""
    binary = os.environ.get("WPE_BIN") or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "wpe")
    path = os.path.join(os.path.dirname(os.path.abspath(binary)), "config.h")
    return path if os.path.isfile(path) else None


def build_has(name):
    """Whether the build has the system fact @name (a WPE_HAS_* or
    WPE_REGEX_* line of config.h, the words configure's summary prints).

    True when the fact is on, False when configure found it missing, None
    when there is no config.h beside the binary: the caller then runs the
    row instead of skipping it.  A name config.h does not define is a
    misspelling and raises, never a silent False."""
    path = _build_config_h()
    if path is None:
        return None
    with open(path) as fh:
        for line in fh:
            words = line.split(None, 2)
            if len(words) >= 2 and words[0] == "#define" and words[1] == name:
                value = words[2].strip() if len(words) > 2 else "1"
                return value not in ("0", "")
    raise KeyError("config.h defines no %s" % name)


def parent_death_signal():
    """Whether the system ends a child when its parent dies (prctl on Linux,
    procctl on FreeBSD): the fact the editor was built with, or, with no
    build folder, what those two systems are known to have."""
    built = build_has("WPE_HAS_PARENT_DEATH_SIGNAL")
    if built is not None:
        return built
    return sys.platform.startswith(("linux", "freebsd"))


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


def _ps_table_rows():
    """Every row of the process table as ps(1) prints it: a list of
    (pid, ppid, state, comm) with the pids as integers; [] when ps fails."""
    result = subprocess.run(["ps", "-A", "-o", "pid=,ppid=,state=,comm="],
                            capture_output=True, text=True)
    rows = []
    for line in result.stdout.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4 and parts[0].isdigit() and parts[1].isdigit():
            rows.append((int(parts[0]), int(parts[1]), parts[2], parts[3]))
    return rows


def _first_word(text):
    """The first blank-separated word of text, or '' for None or blank."""
    words = (text or "").split(None, 1)
    return words[0] if words else ""


# ------------------------------------------------------ program identity

# What a test asks of the process table is "is this child the program P the
# editor would have started", and P is a file: the one the editor's execvp
# reaches on the PATH the editor was started with.  Program holds that file;
# ChildFacts holds what the process table says about one child.  The two
# meet in Program.verdict, the only place the question is decided, so the
# program asked for and the process reported cannot be named two ways.

SCRIPT = "script"
EXECUTABLE = "executable"
MISSING = "missing"
VERDICT_YES = "yes"
VERDICT_ENDED = "no: ended"
VERDICT_NOT_ON_PATH = "no: asked program not on PATH"
VERDICT_NO_SCRIPT = "no: script path not in argv"
VERDICT_NO_EXE = "no: exe not in the asked set"
VERDICT_NO_EITHER = "no: script path not in argv and exe not in the asked set"
NOT_ASKED = "n/a (nothing asked)"

# The options no ps(1) read of this module may carry: -E prints the
# environment of the process, which a listing must never show.
PS_FORBIDDEN_OPTIONS = ("-E",)

# A framework Python's python3 is a launcher that re-executes Python.app
# beside it, so a process running it has the app as its executable.
FRAMEWORK_PYTHON = re.compile(
    r"^(.*/Python\.framework/Versions/[^/]+)/bin/[^/]+$")
FRAMEWORK_APP_TAIL = "/Resources/Python.app/Contents/MacOS/Python"
# A shell wrapper that execs the real program (pkgsrc's python3): the word
# after "exec" names the program a process running it ends up as.
WRAPPER_SHELLS = ("sh", "bash", "dash", "ksh")
WRAPPER_EXEC = re.compile(r'^\s*exec\s+"?([^\s"$`]+)"?(?:\s|$)', re.MULTILINE)
DELETED_SUFFIX = " (deleted)"
TESTS_DIR = os.path.dirname(os.path.abspath(__file__))


def _is_script(path):
    """True when the file at @path starts with "#!"."""
    try:
        with open(path, "rb") as f:
            return f.read(2) == b"#!"
    except OSError:
        return False


def _which_real(word, path):
    """The real path of what execvp reaches for @word on @path, or None."""
    found = shutil.which(word, path=path)
    return os.path.realpath(found) if found else None


def _require_absolute_path(path):
    """Refuse a PATH with a relative or empty entry: such an entry means a
    different folder for every process, so the file the editor would have
    run cannot be named from outside it."""
    for entry in path.split(os.pathsep):
        if not os.path.isabs(entry):
            raise ValueError(
                "PATH entry %r is not absolute; a program asked for on it "
                "cannot be told from another of the same name" % entry)


def _framework_executables(real):
    """The paths a process running the framework Python at @real may have as
    its executable: the launcher itself and the Python.app it re-executes."""
    match = FRAMEWORK_PYTHON.match(real)
    if match is None:
        return set()
    return {match.group(1) + FRAMEWORK_APP_TAIL}


def _wrapper_targets(file, path):
    """The real paths a shell wrapper at @file execs, or an empty set when
    @file is not a wrapper of the system (a script under tests/ is a stub,
    known by its own path) or the word after exec is not a plain path."""
    words = _shebang_words(file)
    if not words or os.path.basename(words[0]) not in WRAPPER_SHELLS:
        return set()
    real = os.path.realpath(file)
    if real.startswith(TESTS_DIR + os.sep):
        return set()
    try:
        with open(file, "r", errors="replace") as f:
            text = f.read()
    except OSError:
        return set()
    targets = set()
    for word in WRAPPER_EXEC.findall(text):
        found = _wrapper_word_path(word, file, path)
        if found is not None:
            targets.add(found)
    return targets


def _wrapper_word_path(word, file, path):
    """The real path an exec @word of the wrapper @file names, or None."""
    if "/" in word:
        candidate = os.path.join(os.path.dirname(os.path.realpath(file)), word)
        return os.path.realpath(candidate) if os.path.exists(candidate) else None
    return _which_real(word, path)


class Program:
    """The program P the editor would exec for the word @name, found on
    @path (the PATH the editor was started with) the way execvp finds it.

    A test asks `runs(facts)` of a child and nothing else.  The identity is
    a file, never a name string: a script is known by its own path (the
    interpreter that runs it does not matter), a binary by the executable
    its process has."""

    def __init__(self, name, path, file):
        self.name = name
        self.path = path
        self.file = file
        self.script = None
        self.executables = set()
        if file is None:
            self.kind = MISSING
        elif _is_script(file):
            self.kind = SCRIPT
            self.script = os.path.realpath(file)
            self.executables = _wrapper_targets(file, path)
        else:
            self.kind = EXECUTABLE
            real = os.path.realpath(file)
            self.executables = {real} | _framework_executables(real)

    def _exe_of(self, facts):
        """The real path of the executable of the child @facts describes:
        what the system says it is, else the path its argv[0] names (a bare
        word is looked up on this program's PATH as execvp did; a word with
        a slash that could not be resolved against the child's folder is
        unknown, never looked up against this process's folder)."""
        if facts.exe is not None:
            return facts.exe
        if facts.argv0_file is not None:
            return facts.argv0_file
        if facts.argv0_word and "/" not in facts.argv0_word:
            return _which_real(facts.argv0_word, self.path)
        return None

    def verdict(self, facts):
        """VERDICT_YES when the child @facts describes runs this program,
        else the reason it does not.  A child that ended (gone, or a zombie
        the editor has not reaped) runs nothing."""
        if facts is None or facts.state in (GONE, ZOMBIE):
            return VERDICT_ENDED
        if self.kind == MISSING:
            return VERDICT_NOT_ON_PATH
        if self.script is not None and self.script in facts.argv_files:
            return VERDICT_YES
        if self.executables and self._exe_of(facts) in self.executables:
            return VERDICT_YES
        if self.kind == SCRIPT and self.executables:
            return VERDICT_NO_EITHER
        if self.kind == SCRIPT:
            return VERDICT_NO_SCRIPT
        return VERDICT_NO_EXE

    def runs(self, facts):
        """True when the child @facts describes is this program and has not
        ended: the one way a row decides a debugger it asked for is there."""
        return self.verdict(facts) == VERDICT_YES

    def describe(self):
        """One line saying what was asked for and the file it came to, for
        the message of a row that found no such child."""
        if self.kind == MISSING:
            return "%s -> not on the editor's PATH" % self.name
        if self.kind == SCRIPT:
            return "%s -> script %s" % (self.name, self.script)
        return "%s -> exe %s" % (self.name, os.path.realpath(self.file))

    def other_executables(self):
        """The paths besides its own file a process running this program may
        have as its executable, sorted."""
        own = os.path.realpath(self.file) if self.file else None
        return sorted(self.executables - {own})


def program(name, path=None):
    """The Program the editor would exec for @name on @path (default: this
    process's PATH, which a session passes on to the editor unchanged).

    The one constructor: the lookup on PATH is made here and nowhere else.
    Raises ValueError for a PATH with a relative entry."""
    if path is None:
        path = os.environ.get("PATH", "")
    _require_absolute_path(path)
    return Program(name, path, shutil.which(name, path=path))


class ChildFacts:
    """What the process table says about one child, read once: its run
    state, the real path of its executable (None when this system cannot
    say), its argument words, and the real paths of the first few that name
    an existing file (the script path of a "#!" child is among them)."""

    def __init__(self, pid, state, exe, argv, argv_files, argv0_file):
        self.pid = pid
        self.state = state
        self.exe = exe
        self.argv = argv
        self.argv_files = argv_files
        self.argv0_file = argv0_file
        self.argv0_word = argv[0] if argv else ""

    @property
    def argv0(self):
        """The base name of argv[0], for reports only."""
        return os.path.basename(self.argv0_word)

    @property
    def script(self):
        """The first of argv_files that is a "#!" file, or None: the script
        a report shows beside the executable."""
        for path in self.argv_files:
            if _is_script(path):
                return path
        return None


def _proc_argv(pid):
    """The argument words of pid from /proc, or [] when not readable."""
    try:
        with open(os.path.join(PROC_ROOT, str(pid), "cmdline"), "rb") as f:
            raw = f.read()
    except OSError:
        return []
    words = raw.split(b"\0")
    if words and words[-1] == b"":
        words.pop()
    return [w.decode("utf-8", "surrogateescape") for w in words]


def _argv_of(pid):
    """The argument words of pid: exact from /proc, else ps(1)'s line split
    on blanks (a word with a blank is then two words)."""
    if has_procfs():
        return _proc_argv(pid)
    return (command_line(pid) or "").split()


def _lsof_name(pid, descriptor):
    """The path lsof(8) gives for the @descriptor ("txt" the program, "cwd"
    the working folder) of pid, or None when lsof is not installed or says
    nothing absolute."""
    out = _tool_output(["lsof", "-p", str(pid), "-a", "-d", descriptor, "-Fn"])
    for line in (out or "").splitlines():
        if line.startswith("n") and os.path.isabs(line[1:]):
            return line[1:]
    return None


def _exe_of_pid(pid):
    """The real path of the executable of pid, or None when this system
    cannot say.  Linux reads it from /proc; elsewhere ps(1)'s command column
    when it is an absolute path (macOS prints the executable there), else
    the program file lsof(8) lists.  Never the environment."""
    if has_procfs():
        try:
            link = os.readlink(os.path.join(PROC_ROOT, str(pid), "exe"))
        except OSError:
            return None
        if link.endswith(DELETED_SUFFIX):
            link = link[:-len(DELETED_SUFFIX)]
        return os.path.realpath(link)
    comm = _ps_field(pid, "comm")
    if comm and os.path.isabs(comm):
        return os.path.realpath(comm)
    found = _lsof_name(pid, "txt")
    return os.path.realpath(found) if found else None


def _cwd_of_pid(pid):
    """The working folder of pid: /proc, else lsof(8); None when unknown."""
    if has_procfs():
        return _proc_cwd(pid)
    return _lsof_name(pid, "cwd")


def _resolved_file(word, cwd):
    """The real path of the argument @word when it names an existing file,
    a relative word being taken relative to the child's folder @cwd (the
    process's own, not this one's); else None."""
    if os.path.isabs(word):
        candidate = word
    elif cwd is not None:
        candidate = os.path.join(cwd, word)
    else:
        return None
    return os.path.realpath(candidate) if os.path.exists(candidate) else None


def child_facts(pid):
    """What the process table says about pid, or None once it is gone.

    A test reads it once per child and hands it to Program.runs, so the
    decision and the report that explains it are made from the same
    facts."""
    state = run_state(pid)
    if state == GONE:
        return None
    argv = _argv_of(pid)
    cwd = _cwd_of_pid(pid)
    words = argv[:SHEBANG_MAX_WORDS + 1]
    files = [f for f in (_resolved_file(w, cwd) for w in words) if f]
    first = None
    if argv and "/" in argv[0]:
        first = _resolved_file(argv[0], cwd)
    return ChildFacts(pid, state, _exe_of_pid(pid), argv, files, first)


def children_running(ppid, prog):
    """PIDs of ppid's direct children that run the Program @prog and have
    not ended.

    A test needs it to find the debugger the editor started (and only that
    child: not another child, not a grandchild), or to see it has ended."""
    return [pid for pid in _child_pids(ppid) if prog.runs(child_facts(pid))]


def descendants_running(ppid, prog):
    """PIDs of every live process below ppid that runs the Program @prog.

    A test needs it to find a debugger that is not a direct child (the
    editor started a shell that started it) or to see none is left."""
    return [pid for pid in descendants(ppid) if prog.runs(child_facts(pid))]


def _asked_lines(asked):
    """The lines that say which program the report was asked about."""
    if asked is None:
        return ["asked: nothing yet"]
    lines = ["asked: " + asked.describe()]
    for other in asked.other_executables():
        lines.append("       also runs as " + other)
    return lines


def _child_line(pid, parent, asked):
    """The report line for one child: its state, executable, script, the
    base name of argv[0], and whether it is the program asked for."""
    facts = child_facts(pid)
    if facts is None:
        return "pid=%d ppid=%d state=%s" % (pid, parent, GONE)
    answer = NOT_ASKED if asked is None else asked.verdict(facts)
    return ("pid=%d ppid=%d state=%s exe=%r script=%r argv0=%r runs_asked=%s"
            % (pid, parent, facts.state, facts.exe, facts.script,
               facts.argv0, answer))


def describe_children(ppid, asked=None):
    """A listing of the processes below ppid, for the message of a check
    that found none of the child it was looking for.

    When a debugger row fails on a system that prints its process table
    differently, the question is whether the child never ran, ran as
    another file, or ran and was not recognised.  The first lines say which
    Program @asked was looked for (its file); then one line per live
    descendant with its state, executable, script and whether it is that
    program, and why not.  Only the program's own path is shown, never the
    rest of its command line and never its environment."""
    below = descendants(ppid)
    lines = _asked_lines(asked)
    lines.append("descendants of %d: %d" % (ppid, len(below)))
    for pid, parent, state, comm in _ps_table_rows():
        if pid in below:
            lines.append(_child_line(pid, parent, asked))
    kernel = ["%d %d %s %s" % row for row in _ps_table_rows()
              if row[1] == ppid or row[0] in below]
    lines.append("ps rows in the tree (pid ppid state comm): "
                 + ("; ".join(kernel) if kernel else "none"))
    return "\n".join(lines)


def all_ended(pids):
    """True when none of pids is still alive: each is gone or a zombie."""
    return all(run_state(pid) in (GONE, ZOMBIE) for pid in pids)


def parent_of(pid):
    """The parent PID of pid (from /proc), or None once it is gone."""
    fields = _proc_stat_fields(pid)
    if fields is None:
        return None
    return int(fields[1])


def _proc_cwd(pid):
    """The working directory of pid (from /proc), or None when it is gone
    or not readable."""
    try:
        return os.readlink(os.path.join(PROC_ROOT, str(pid), "cwd"))
    except OSError:
        return None


def _live_comm_in(pid, comm, workdir):
    """True when pid has not ended, its kernel command name is comm and its
    working directory is workdir."""
    if run_state(pid) in (GONE, ZOMBIE) or _proc_comm(pid) != comm:
        return False
    return _proc_cwd(pid) == os.path.realpath(workdir)


def processes_in(workdir, comm):
    """PIDs of live processes named comm (the kernel's command name) whose
    working directory is workdir; Linux only, [] elsewhere.

    A test needs it to find a program a shared server started for the
    session, which is not a descendant of the editor: a Scala program runs
    as a child of the Bloop build server, in the directory of the source.
    Only the command name and the directory are read, never a command
    line."""
    if not has_procfs():
        return []
    return [int(e) for e in os.listdir(PROC_ROOT)
            if e.isdigit() and _live_comm_in(int(e), comm, workdir)]


def children_in(ppid, workdir, comm):
    """PIDs of ppid's live direct children named comm whose working
    directory is workdir; Linux only, [] elsewhere.

    A test needs it to count what a shared server still runs for one
    session (its programs), leaving the server itself and the programs of
    other sessions out."""
    if not has_procfs():
        return []
    return [pid for pid in _proc_child_pids(ppid)
            if _live_comm_in(pid, comm, workdir)]


# ---------------------------------------------------------------- path limit

# What the smallest system (macOS and the BSDs) holds, used when the system
# will not say.
PATH_MAX_FALLBACK = 1024


def _existing_ancestor(path):
    """The deepest folder of @path that exists, so pathconf has something to
    ask about when the row has not made its folder yet."""
    path = os.path.abspath(str(path))
    while not os.path.exists(path) and path != os.path.dirname(path):
        path = os.path.dirname(path)
    return path


def path_max(path):
    """The longest path, in bytes with its end mark, that the system holds
    under @path: 4096 on Linux, 1024 on macOS and the BSDs.

    A row that builds a deep folder asks it, so the folder is as deep as
    this system can make it instead of as deep as Linux can, and a row about
    the editor's own old cut at N bytes knows it cannot exist where N is
    over the limit."""
    try:
        limit = os.pathconf(_existing_ancestor(path), "PC_PATH_MAX")
    except (OSError, ValueError):
        return PATH_MAX_FALLBACK
    return limit if limit > 0 else PATH_MAX_FALLBACK


# ------------------------------------------------------- open descriptors

TOOL_TIMEOUT_S = 30
LSOF_NUMBER_FIELD = "f"     # lsof -F f: one "f<fd>" line per descriptor
LSOF_PROCESS_FIELD = "p"    # lsof -F f: one "p<pid>" line per process


def _tool_output(argv):
    """What the tool printed, or None when it is not installed or hung."""
    try:
        result = subprocess.run(argv, capture_output=True, text=True,
                                timeout=TOOL_TIMEOUT_S)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.stdout


def _proc_descriptors(pid):
    """The descriptor numbers of pid from /proc, or None once it is gone."""
    try:
        names = os.listdir(os.path.join(PROC_ROOT, str(pid), "fd"))
    except OSError:
        return None
    return sorted(int(name) for name in names if name.isdigit())


def _lsof_descriptors(pid):
    """The descriptor numbers of pid from lsof(8), or None when lsof is not
    installed or knows no such process.  The current directory, the program
    and the mapped files lsof also lists have no number and are left out."""
    out = _tool_output(["lsof", "-p", str(pid), "-F", LSOF_NUMBER_FIELD])
    if not out:
        return None
    lines = out.splitlines()
    if not any(line.startswith(LSOF_PROCESS_FIELD) for line in lines):
        return None
    return sorted(int(line[1:]) for line in lines
                  if line.startswith(LSOF_NUMBER_FIELD) and line[1:].isdigit())


def _fstat_descriptors(pid):
    """The descriptor numbers of pid from fstat(1), the tool the BSDs ship
    in their base system (lsof is not there), or None when fstat is not
    installed or knows no such process.  A row reads
    "USER CMD PID FD MOUNT INUM MODE ..."; the working folder and the
    program have a word in the FD column, not a number."""
    out = _tool_output(["fstat", "-p", str(pid)])
    if not out:
        return None
    row = re.compile(r"^\S+\s+.*?\s%d\s+(\S+?)\*?\s" % pid)
    found = [row.match(line) for line in out.splitlines()]
    found = [match.group(1) for match in found if match]
    if not found:
        return None
    return sorted(int(fd) for fd in found if fd.isdigit())


def open_descriptors(pid):
    """The numbers of the descriptors pid holds open, sorted, or None when
    this system cannot say.

    A test that shows an error path leaves no descriptor behind lists them
    before and after.  Linux answers from /proc; elsewhere lsof(8), then the
    BSDs' own fstat(1).  None (no tool answers, or the process is gone) is
    for the row to skip by name: "needs a descriptor table: /proc, lsof or
    fstat"."""
    if has_procfs():
        return _proc_descriptors(pid)
    for ask in (_lsof_descriptors, _fstat_descriptors):
        found = ask(pid)
        if found is not None:
            return found
    return None


def cpu_seconds(pid):
    """User plus system CPU time of pid in seconds, or None once it is gone
    or this system cannot say.

    A test samples it twice around a wait to show the editor did not spin
    while it copied a file."""
    ticks = cpu_ticks(pid)
    if ticks is None:
        return None
    return ticks / os.sysconf("SC_CLK_TCK")


# --------------------------------------------------------- signal states

SIGSTATE_IGNORED = "IGNORED"
SIGSTATE_BLOCKED = "BLOCKED"
SIGSTATE_SOURCE = "sigstate.c"
STATUS_MASK_LINES = {"SigIgn": SIGSTATE_IGNORED, "SigBlk": SIGSTATE_BLOCKED}
SIGNAL_NUMBERS = range(1, 65)


def sigstate_program(tmp_path):
    """The path of tests/sigstate, which prints "IGNORED: INT PIPE ..." and
    "BLOCKED: CHLD ..." for the signals the process it is started as ignores
    and blocks.

    A test that asks whether a program the editor started inherited an
    ignored or blocked signal runs this as that program, on any system, by
    names instead of the mask bits a /proc status shows (only Linux has them,
    and bit numbers differ).  `make check` builds it beside the sources;
    when it is missing it is compiled into @tmp_path with the build's
    config.h, and the row skips by name when there is no C compiler."""
    import pytest
    tests = os.path.dirname(os.path.abspath(__file__))
    top = os.path.join(tests, "..")
    built = os.path.join(top, "sigstate")
    source = os.path.join(tests, SIGSTATE_SOURCE)
    if os.path.exists(built) and os.path.getmtime(built) >= os.path.getmtime(source):
        return built
    out = str(tmp_path / "sigstate")
    cmd = [os.environ.get("CC", "cc"), "-o", out]
    config_h = os.path.join(top, "config.h")
    if os.path.exists(config_h):
        cmd += ["-include", config_h]
    cmd.append(source)
    try:
        built_ok = subprocess.run(cmd, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL).returncode == 0
    except OSError:
        built_ok = False
    if not built_ok:
        pytest.skip("tests/sigstate.c does not build here: no C compiler")
    return out


def signal_names(text, which):
    """The signal names on the @which line (SIGSTATE_IGNORED or
    SIGSTATE_BLOCKED) of what sigstate printed.  A text with no such line is
    a defect of the row, not an empty set, so it asserts."""
    for line in text.splitlines():
        head, _, names = line.partition(":")
        if head == which:
            return names.split()
    raise AssertionError("no %s line in the signal state:\n%s" % (which, text))


def _names_in_mask(mask):
    """The names (without SIG) of the signals whose bit is set in a /proc
    status mask."""
    names = []
    for number in SIGNAL_NUMBERS:
        if mask >> (number - 1) & 1:
            try:
                names.append(signal.Signals(number).name[len("SIG"):])
            except ValueError:
                continue
    return names


def signal_state_of(pid):
    """The signal state of a running pid, in the text sigstate prints, or
    None once it is gone or when this system has no /proc to read it from.

    Only a process that is already running needs it (gdb, jdb and the VM
    under it); a program the editor starts is asked with sigstate_program,
    which answers on every system."""
    if not has_procfs():
        return None
    try:
        with open(os.path.join(PROC_ROOT, str(pid), "status")) as fh:
            lines = fh.read().splitlines()
    except OSError:
        return None
    out = []
    for line in lines:
        key, _, value = line.partition(":")
        if key in STATUS_MASK_LINES:
            names = _names_in_mask(int(value.strip(), 16))
            out.append("%s: %s" % (STATUS_MASK_LINES[key], " ".join(names)))
    return "\n".join(out) + "\n"


def self_memory_file():
    """The path of a file whose read fails with an input/output error (the
    process's own memory at its start), or None where this system has none.

    A copy of it shows the file manager ends a failed read with a box
    instead of spinning."""
    path = os.path.join(PROC_ROOT, "self", "mem")
    return path if has_procfs() and os.path.exists(path) else None


# ------------------------------------------------------------ the pty name

# The names a pty's slave has: /dev/pts/N (Linux, FreeBSD), /dev/ttysNNN
# (macOS), /dev/tty[p-z]N and /dev/tty[P-T]N (NetBSD, OpenBSD).  Not
# /dev/ttyS0, a serial line.  The letters are the ones OpenBSD and NetBSD give
# their ptys; on FreeBSD /dev/ttyu0 and /dev/ttyv0 are serial lines and the
# console, but FreeBSD's ptys are /dev/pts/N, so those two are never passed as
# a pty's name there.
PTY_NAME_PREFIXES = ("/dev/pts/",) + tuple("/dev/tty" + letter
                                           for letter in "pqrstuvwxyzPQRT")


def is_pty_path(path):
    """True when @path names a pty's slave on some system.

    A test that shows a program was given a terminal of its own asks it of
    the name the editor passed, instead of expecting Linux's /dev/pts/N.  A
    name that exists must also be a character device, so an ordinary file
    called like one is not taken for it."""
    if not path.startswith(PTY_NAME_PREFIXES):
        return False
    if not os.path.exists(path):
        return True
    return stat.S_ISCHR(os.stat(path).st_mode)
