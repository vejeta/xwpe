"""engine_guard.py -- a test row sees only the tools it is meant to use.

A row that runs the editor with an AI backend starts programs by name
(`claude -p`, an `ollama` client, whatever XWPE_AI_HOST_CMD says).  The
machine of the person or the runner may have the real program on its PATH,
and new AI tools appear all the time, so a list of "bad names" is always
short.  The guard works the other way round: the PATH of every process a row
starts is built from a tool farm, a folder of symlinks to an allowed
baseline (BASELINE_TOOLS) and nothing else of the machine's PATH.

tests/gui_owner.py (Owner.launch) is the one place that applies it, so every
process of a row, on every backend, gets the same treatment.

What a row gets:

* PATH = the row's own folders (its stand-ins, in its scratch), then the
  farm's tool folder, then the deny folder.  The machine's PATH folders
  (~/.local/bin, /usr/local/bin, ~/.cargo/bin, npm and go bins, anything
  under the real HOME, /usr/bin itself) are never on it.
* A program that is neither in the baseline nor declared is not found, or
  it is found as a stub of the deny folder: one stub for every executable
  name the machine's PATH has that the farm does not.  The stub writes a
  line to the blocker log and exits 127, so a row that reaches anything
  unexpected fails by name when it ends (Owner.end_since).
* A row adds a tool of the machine to its farm by naming it in
  TOOLS_VAR of its environment (only that one symlink is added).
* A row that removes PATH, or empties it, is refused: the editor then
  falls back to built-in lists of folders, which the farm cannot sit in.
* XWPE_AI_HOST_CMD naming a program by an absolute path must name one inside
  the row's scratch or the farm (or a baseline tool): a PATH does nothing
  against an absolute path.
* The live opt-out (REAL_ENGINE_VAR) skips the farm for that row, keeps the
  machine's PATH, and is allowed only to rows of a live file
  (LIVE_FILE_PATTERN) when the person running the suite also set
  LIVE_GATE_VAR=1 in the environment of the test run.  So are inheriting an
  API key and `real_login`.  Without the gate a live file is not even
  imported (tests/live_gate.py).
* A launch is refused when an argument, a word of an environment value, or a
  file in a folder of the row's own PATH is the realpath of a program of the
  machine that the farm does not provide (check_no_unprovided_program): a
  PATH does nothing against an absolute path, a symlink or a value the
  program later runs.

Out of scope, on purpose:
* A shell inside the row that rebuilds its own PATH (`env -i PATH=/usr/local/bin
  sh -c claude`), where the program is named by a bare name.  Revisit when a
  row is seen doing it; the owner would then be the shells a row starts.
* A launch with PATH removed, where dash falls back to its built-in
  /usr/local/bin:/usr/bin:/bin.  Launch refuses a missing or empty PATH
  (check_path_present), so only a program that unsets PATH itself reaches it.
  Revisit when a stand-in does; the owner would then be the stand-ins' scripts.
* The HTTP route.  A real Ollama listens on
127.0.0.1:11434 on a developer's machine and the editor's default endpoint
is that address; a farm does not cover a socket.  Every row seen that uses
the ollama or claude backend sets XWPE_AI_ENDPOINT to a stub server or a
refused port, and no row has been seen talking to 11434 or to a provider
host.  Revisit when one is: the owner of that concern would be the endpoint
default, set in Owner.launch for rows with the AI on.
Revisit also when a containment profile holds the file system (Strict, fs
none): the stubs must stay runnable and the log writable inside the profile,
or the detector goes blind without an error; that pass adds its own canary.
"""
import fnmatch
import os
import re
import shlex

# What the suite executes by name, from the editor's own source (we_*.c: the
# compiler, the debuggers, the language servers, man, make) and from the rows
# (run_tool/Popen argv, the display and clipboard tools of the x11 and
# wayland rows).  A name that the machine does not have is skipped.  A tool
# missing here shows up as a deny-folder hit in the log of the row that
# needed it; it is added with the reason.
BASELINE_TOOLS = (
    # shell and interpreters the editor's /bin/sh -c lines and the stand-ins use
    "sh", "dash", "bash", "env", "python3", "python", "perl", "awk", "mawk",
    "gawk", "sed", "grep", "egrep", "fgrep", "find", "xargs", "diff", "cmp",
    "patch", "tar", "cpio", "gzip", "gunzip", "bzip2", "xz", "less", "more",
    "man", "col", "script", "scriptreplay",
    # coreutils
    "ls", "cat", "cp", "mv", "rm", "mkdir", "rmdir", "touch", "chmod", "chown",
    "ln", "head", "tail", "sort", "uniq", "wc", "tr", "cut", "paste", "tee",
    "true", "false", "echo", "printf", "sleep", "date", "dirname", "basename",
    "readlink", "realpath", "stat", "mktemp", "id", "whoami", "uname",
    "hostname", "test", "[", "expr", "seq", "yes", "od", "md5sum", "sha256sum",
    "timeout", "nice", "nohup", "kill", "ps", "pgrep", "stty", "tty", "clear",
    "reset", "tput", "infocmp", "tic", "locale", "true", "pwd", "which",
    "command", "touch", "du", "df", "ldd", "strings", "nm", "objdump", "file",
    "sync", "install", "tac", "rev", "fold", "fmt", "nl", "comm", "join",
    "split", "shuf", "numfmt", "cksum", "base64", "xxd", "hexdump", "iconv",
    # compilers and build tools (a row's Make, Compile and Run menus)
    "cc", "gcc", "c++", "g++", "clang", "clang++", "tcc", "ld", "as", "ar",
    "ranlib", "make", "gmake", "pkg-config", "cpp", "indent", "ctags",
    "cscope", "git", "rg", "cobc", "scalac", "scala-cli", "javac", "go",
    "cargo", "rustc", "ghc", "ocaml",
    # the compilers and runtimes of the Run menu's other languages (the rows
    # of test_ga68, test_run_kind and the Compile menu run them), and the
    # option and system probes of the debugger stand-ins
    "ga68", "fpc", "gfortran", "gnat", "gnatmake", "gccgo", "gm2", "gdc",
    "node", "nodejs", "ruby", "php", "lua", "tclsh", "kotlinc", "scala",
    "cs", "coursier", "bloop", "sbt", "mvn", "gradle", "swift", "zig", "nim",
    "julia", "Rscript", "erl", "racket", "sbcl", "getopt", "getconf",
    # the manual formatter behind `man` (the help rows read a rendered page)
    "nroff", "tbl", "troff", "groff", "eqn", "pic", "preconv", "soelim",
    "refer", "zcat", "bzcat", "xzcat", "manpath",
    # debuggers and their stand-in helpers
    "gdb", "lldb", "lldb-dap", "jdb", "java", "a68g", "dbx", "sdb", "dlv",
    "pdb", "valgrind", "strace",
    # language servers the LSP and DAP rows use
    "clangd", "rust-analyzer", "gopls", "metals", "pylsp", "pyright-langserver",
    # display, clipboard and terminal tools of the x11 and wayland rows
    "xdotool", "xwd", "xclip", "xsel", "xprop", "xmodmap", "xwininfo", "xlogo",
    "xterm", "convert", "import", "Xvfb", "Xwayland", "weston", "weston-info",
    "matchbox-window-manager", "wl-copy", "wl-paste", "xauth", "setxkbmap",
    "screen", "tmux", "openssl",
    # what the graphical editor asks for its font and display, with the
    # row's private HOME: gsettings reads the font name, fc-* the fonts
    "gsettings", "fc-match", "fc-list", "xrdb", "xrandr", "xset", "xdpyinfo",
)
# Names by pattern: the compiler driver runs its target's binutils and its
# own versions by name from PATH (x86_64-linux-gnu-as, gcc-14, python3.13).
BASELINE_PATTERNS = ("*-linux-gnu-*", "gcc-[0-9]*", "g++-[0-9]*", "cpp-[0-9]*",
                     "python3.[0-9]*", "clang-[0-9]*", "ld.*")
# Advisory only, for the static check: programs that run a model or act on the
# user's accounts off this machine.  The guard does not depend on it.
KNOWN_ENGINE_NAMES = ("claude", "qwen", "codex", "gemini", "opencode", "aider",
                      "ollama", "cursor-agent", "agent", "cursor", "bl",
                      "bailian", "lumo-serve", "proton", "crush", "goose",
                      "kimi", "amp", "copilot", "llm")
# The files whose rows may reach a real engine (they skip without keys).
LIVE_FILE_PATTERN = "test_*_live*.py"
# A live row sets this in its own environment to keep the farm away.
REAL_ENGINE_VAR = "XWPE_TEST_REAL_ENGINE"
# The person running the suite sets this to 1, in the environment of the test
# run, to let live files be imported and their rows reach real engines.
LIVE_GATE_VAR = "XWPE_TEST_LIVE"
LIVE_GATE_ON = "1"
# A row lists, here, tools of the machine it needs beyond the baseline.
TOOLS_VAR = "XWPE_TEST_TOOLS"
HOST_CMD_VAR = "XWPE_AI_HOST_CMD"
# Keys a row may inherit from the machine only in a live file.
API_KEY_NAMES = ("ANTHROPIC_API_KEY", "OPENAI_API_KEY")
BLOCKER_LOG_NAME = "blocked.log"
STUB_NAME = "deny-stub"
STUB_EXIT_CODE = 127


class GuardRefused(RuntimeError):
    """A row asked for something the guard never allows."""


def is_live_file(path):
    """True when the test file at @path is a live file (it may reach a real
    engine)."""
    return fnmatch.fnmatch(os.path.basename(str(path or "")), LIVE_FILE_PATTERN)


def is_baseline_tool(name):
    """True when @name is one of the programs the farm provides (the
    baseline names and the baseline patterns)."""
    return name in BASELINE_TOOLS or any(
        fnmatch.fnmatchcase(name, pattern) for pattern in BASELINE_PATTERNS)


def live_gate_open(environ):
    """True when the environment of the test run has opened the live gate."""
    return environ.get(LIVE_GATE_VAR) == LIVE_GATE_ON


def live_row_allowed(path, environ):
    """True when a row of the test file at @path may reach real engines: the
    file is a live file and the person running the suite opened the gate."""
    return is_live_file(path) and live_gate_open(environ)


def _executable_file(path):
    """True when @path is a file this process may execute."""
    return os.path.isfile(path) and os.access(path, os.X_OK)


def _usable_entry(entry):
    """True for a PATH entry that names a folder by an absolute path (an empty
    entry and a relative one mean the working folder)."""
    return bool(entry) and os.path.isabs(entry)


def _inside(path, folder):
    """True when the realpath of @path is @folder or below it."""
    path, folder = os.path.realpath(path), os.path.realpath(folder)
    return path == folder or path.startswith(folder.rstrip(os.sep) + os.sep)


class MachineTools:
    """What the machine's PATH holds, taken once from a PATH value.

    folders: the realpaths of its folders; programs: {name: the path the
    machine's PATH finds first}, by the path as found (a symlink keeps its
    own name, which a multi-call program reads from argv[0])."""

    def __init__(self, folders, programs, also=None):
        self.folders = frozenset(folders)
        self.programs = dict(programs)
        # {name: [path, ...]}: the later files of the same name that are not
        # the first one (a rustup shim first, the real tool in /usr/bin: the
        # shim falls back to the next `rust-analyzer` on PATH).
        self.also = dict(also or {})
        self._real = None

    @classmethod
    def from_path(cls, path_text):
        """The folders and programs of @path_text (a PATH value)."""
        folders, found = [], {}
        for entry in (path_text or "").split(os.pathsep):
            if not _usable_entry(entry) or not os.path.isdir(entry):
                continue
            folders.append(os.path.realpath(entry))
            _add_programs(entry, found)
        programs = {name: paths[0] for name, paths in found.items()}
        also = {name: paths[1:] for name, paths in found.items()
                if len(paths) > 1}
        return cls(folders, programs, also)

    def is_machine_folder(self, entry):
        """True when @entry is one of the machine's PATH folders."""
        return _usable_entry(entry) and os.path.realpath(entry) in self.folders

    def real_programs(self):
        """The realpath of every program file of the machine's PATH."""
        if self._real is None:
            files = list(self.programs.values())
            for paths in self.also.values():
                files.extend(paths)
            self._real = frozenset(os.path.realpath(p) for p in files)
        return self._real


def _add_programs(entry, found):
    """Add the executables of folder @entry to @found ({name: [path, ...]}
    in PATH order, a file already there by its realpath not twice)."""
    try:
        names = os.listdir(entry)
    except OSError:
        return
    for name in names:
        path = os.path.join(entry, name)
        if not _executable_file(path):
            continue
        paths = found.setdefault(name, [])
        if all(os.path.realpath(path) != os.path.realpath(p) for p in paths):
            paths.append(path)


def _is_script(path):
    """True when the file at @path starts with a #! line."""
    try:
        with open(path, "rb") as fh:
            return fh.read(2) == b"#!"
    except OSError:
        return False


def _write_wrapper(path, target):
    """Write, at @path, a script that runs the script @target.  A symlink
    would make `dirname $0` the farm's folder, and a launcher that looks for
    its own files beside itself (scala-cli's `.scala-cli.aux`) would not find
    them; a compiled program is linked instead, because a multi-call program
    (a rustup shim) reads its name from argv[0]."""
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("#!/bin/sh\nexec %s \"$@\"\n" % shlex.quote(target))
    os.chmod(path, 0o755)


def stub_text(log_path):
    """The text of the deny stub: it appends one line (the name it was called
    by, HOME, PWD and its arguments) to @log_path and exits.  Only sh
    builtins are used, because a contained program's PATH holds only what
    the profile kept; the log's path is written here, never passed by
    variable, because a contained program's environment is built from a list
    and would drop it."""
    return ("#!/bin/sh\n"
            "printf '%%s\\t%%s\\n' \"${0##*/}\" "
            "\"HOME=$HOME PWD=$PWD argv=$*\" >> %s\n"
            "exit %d\n" % (shlex.quote(log_path), STUB_EXIT_CODE))


class ToolFarm:
    """One owner's farm: the tool folders and the log of the deny stubs."""

    def __init__(self, root, machine):
        self.root = root
        self.machine = machine
        self.log = os.path.join(root, BLOCKER_LOG_NAME)
        open(self.log, "ab").close()
        self._stub = os.path.join(root, STUB_NAME)
        with open(self._stub, "w", encoding="utf-8") as fh:
            fh.write(stub_text(self.log))
        os.chmod(self._stub, 0o755)
        self._tools = {}
        self._provided = {}
        self._deny = None

    def tools_folders(self, extras=()):
        """The folders of symlinks to the baseline and to @extras (names of
        machine tools), made on first use: the first holds the file the
        machine's PATH finds first for each name, the next ones the later
        files of the same name, in order."""
        key = tuple(sorted(set(extras)))
        if key not in self._tools:
            names = sorted(set(BASELINE_TOOLS) | set(key)
                           | self._pattern_names())
            levels = 1 + max([len(self.machine.also.get(n, ())) for n in names]
                             or [0])
            self._tools[key] = [self._make_level(len(self._tools), level, names)
                                for level in range(levels)]
        return self._tools[key]

    def _make_level(self, serial, level, names):
        """One tools folder: the @level'th file of each of @names."""
        folder = os.path.join(self.root, "tools%d-%d" % (serial, level))
        os.makedirs(folder)
        for name in names:
            self._link(folder, name, level)
        return folder

    def _pattern_names(self):
        """The machine's programs that match BASELINE_PATTERNS."""
        return {name for name in self.machine.programs
                if any(fnmatch.fnmatchcase(name, pattern)
                       for pattern in BASELINE_PATTERNS)}

    def _link(self, folder, name, level):
        """Link @name in @folder to the @level'th file of that name on the
        machine's PATH, if the machine has one."""
        files = [self.machine.programs.get(name)]
        files.extend(self.machine.also.get(name, ()))
        if level >= len(files) or not files[level]:
            return
        if _is_script(files[level]):
            _write_wrapper(os.path.join(folder, name), files[level])
        else:
            os.symlink(files[level], os.path.join(folder, name))

    def provided_realpaths(self, extras=()):
        """The realpath of every file the farm provides for these @extras."""
        key = tuple(sorted(set(extras)))
        if key not in self._provided:
            names = (set(BASELINE_TOOLS) | set(key) | self._pattern_names())
            files = []
            for name in names:
                if name in self.machine.programs:
                    files.append(self.machine.programs[name])
                files.extend(self.machine.also.get(name, ()))
            self._provided[key] = frozenset(os.path.realpath(p) for p in files)
        return self._provided[key]

    def deny_folder(self):
        """The folder with one stub for every program name of the machine
        (it comes after the tools folder, so a tool of the farm wins)."""
        if self._deny is None:
            folder = os.path.join(self.root, "deny")
            os.makedirs(folder)
            for name in self.machine.programs:
                os.symlink(self._stub, os.path.join(folder, name))
            self._deny = folder
        return self._deny

    def size(self):
        """How many bytes the log holds now."""
        return os.path.getsize(self.log)

    def read(self, start, end=None):
        """The lines of the log between byte offsets @start and @end, and the
        offset where the reading stopped."""
        with open(self.log, "rb") as fh:
            fh.seek(start)
            data = fh.read() if end is None else fh.read(end - start)
        text = data.decode("utf-8", "replace")
        return [line for line in text.splitlines() if line], start + len(data)


def farm_path(path_text, machine, farm, extras, scratch):
    """The PATH of a row's process: the row's own folders, the farm's tool
    folder, the deny folder.

    A folder of @path_text is the machine's (and dropped) when it is one of
    the machine's PATH folders, or when it is under the real HOME and not
    under one of the @scratch folders (the row's working folder, its HOME).
    The others are the row's own (its stand-ins) and keep their order."""
    real_home = os.path.realpath(os.path.expanduser("~"))
    own, dropped = [], False
    for entry in path_text.split(os.pathsep):
        if not entry or machine.is_machine_folder(entry):
            dropped = dropped or bool(entry)
            continue
        if _usable_entry(entry) and _inside(entry, real_home) \
                and not any(_inside(entry, folder) for folder in scratch):
            dropped = True
            continue
        own.append(entry)
    check_own_folders(own, farm, extras)
    if not dropped:
        # The row gave a PATH with none of the machine's folders (a PATH of
        # its own stand-ins only, to see the editor with no `claude`): a farm
        # added to it would make absent programs look installed.
        return path_text
    return os.pathsep.join(own + farm.tools_folders(extras)
                           + [farm.deny_folder()])


# Separators inside a word that may hold a path: spaces, the `=` of NAME=path,
# the `:` of a list of folders, quotes, and the shell's own punctuation
# (`;` `&` `|` `(` `)` `<` `>` `{` `}` `$` backtick backslash `!` and `,`), so a
# path in `true;/p/eng`, `$(/p/eng)`, `(/p/eng)` or `x|/p/eng` is a word.
WORD_SPLIT = re.compile(r"[\s=:'\"`;&|()<>{}$\\!,]+")


def words_of(text):
    """The pieces of @text that may be a path: the whole text and each word
    of it split on WORD_SPLIT."""
    return [text] + [word for word in WORD_SPLIT.split(text) if word]


def _unprovided_program(path, farm, extras):
    """True when @path is, by realpath, a program of the machine's PATH that
    the farm does not provide (for these @extras)."""
    if not os.path.isabs(path):
        return False
    real = os.path.realpath(path)
    return (real in farm.machine.real_programs()
            and real not in farm.provided_realpaths(extras))


def check_no_unprovided_program(argv, env, farm, extras):
    """Refuse a launch whose argument vector or environment names a program
    of the machine that the farm does not provide, by the path of the file.

    Every argument and every value of @env (PATH apart) is split on
    WORD_SPLIT; a word that is an absolute path whose realpath is such a
    program refuses the launch.  A stand-in of the row in its scratch is not a
    machine program and passes; a link in the scratch to one is its realpath
    and does not."""
    texts = [str(word) for word in argv]
    texts.extend(value for name, value in env.items()
                 if name != "PATH" and isinstance(value, str))
    for text in texts:
        for word in words_of(text):
            if _unprovided_program(word, farm, extras):
                raise GuardRefused(
                    "%s is a program of the machine's PATH that the tool farm "
                    "does not provide: a row reaches it by a bare name only "
                    "through the farm; declare it with %s or use a stand-in"
                    % (word, TOOLS_VAR))


def check_own_folders(own, farm, extras):
    """Refuse a PATH folder of the row's own that holds a file which is, by
    realpath, a program of the machine that the farm does not provide (a row
    that links the real binary into its own bin)."""
    for entry in own:
        if not _usable_entry(entry) or not os.path.isdir(entry):
            continue
        for name in os.listdir(entry):
            if _unprovided_program(os.path.join(entry, name), farm, extras):
                raise GuardRefused(
                    "%s in the row's own PATH folder %s is a program of the "
                    "machine's PATH that the tool farm does not provide"
                    % (name, entry))


def check_path_present(env):
    """Refuse a row whose process has no PATH or an empty one."""
    if not env.get("PATH"):
        raise GuardRefused("a row that removes PATH or empties it is refused: "
                           "the editor falls back to built-in folders the "
                           "farm cannot sit in")


def check_farm_outside(farm, cwd):
    """Refuse a working folder that holds the farm: the product drops PATH
    folders inside the workspace from an engine's PATH.

    Only a working folder the launch names is a workspace.  A launch with no
    cwd (a short tool of the session, xdotool) inherits the session's own,
    which may hold the pytest basetemp and so the farm, and is no row's
    workspace."""
    if cwd is None:
        return
    if _inside(farm.root, cwd):
        raise GuardRefused("the tool farm %s is inside the working folder %s"
                           % (farm.root, cwd))


def check_host_command(env, farm, scratch):
    """Refuse XWPE_AI_HOST_CMD that names a program by an absolute path
    outside the row's scratch, the farm and the baseline."""
    command = env.get(HOST_CMD_VAR)
    if not command:
        return
    try:
        words = shlex.split(command)
    except ValueError:
        words = command.split()
    if not words or not os.path.isabs(words[0]):
        return
    program = words[0]
    if any(_inside(program, folder) for folder in scratch + [farm.root]):
        return
    if _is_baseline_file(program, farm.machine):
        return
    raise GuardRefused("%s names the program %s by an absolute path outside "
                       "the row's scratch and the farm" % (HOST_CMD_VAR, program))


def _is_baseline_file(program, machine):
    """True when @program is the real file of a baseline tool."""
    real = os.path.realpath(program)
    return any(os.path.realpath(machine.programs[name]) == real
               for name in BASELINE_TOOLS if name in machine.programs)


def take_tools(env):
    """Remove TOOLS_VAR from @env and return the names it listed."""
    return (env.pop(TOOLS_VAR, "") or "").split()
