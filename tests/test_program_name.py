"""One rule names the program a build makes from a source or a project.

F9's link, Alt-C's compile, Ctrl-F9's Run and a project's link used to work
the name out each by its own rule: the last dot or the first, Exedir or not,
the user's EXENAME or the path it had been turned into.  Now the owner
(we_progname.c) works it out once, and every command names the same file.

A stand-in `gcc` (written here, first on PATH) logs its words and writes its
-o target: an empty file for an object, a script that logs the word it was
started as for anything else.  So the log says, per row, which file F9
compiled and linked and which file Ctrl-F9 started.  Rows compare the FILE
named, resolved from the build folder, except where the row is about the word
itself (04, 06b).

The debugger's side of the same rule (the program it loads, the Rust debug
binary) is the debugger pass's rows, not these.

Run: WPE_BIN=$PWD/wpe pytest tests/test_program_name.py (and wpe-asan).
"""
import os
import shutil
import time

import pytest
from wpe_driver import ALT, WAIT_SCALE, WpeSession, tool_usable

F9 = "\033[20~"             # Make: compile and link
CTRL_F9 = "\033[20;5~"      # Run
ENTER = "\r"
ALT_S = "\033s"
BOX = "Command from p.prj"  # the box a folder's Project key puts before F9

SANITIZER = "AddressSanitizer"
ASAN_ENV = {"ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1"}
SEED = "int main(void){return 0;}\n"
BUILD_WAIT = 30             # seconds a small build and run has to finish in
STEM_BYTES = 250            # a stem that fits with ".c" in a 255 byte name

needs_sh = pytest.mark.skipif(not tool_usable("sh", "-c", "exit 0"), reason="sh not usable")

# The stand-in compiler.  It never compiles: the name it was given is the
# whole observation.
STAND_IN_GCC = """#!/bin/sh
echo "gcc $*" >> "$STANDIN_LOG"
out=
prev=
for a in "$@"; do
  if [ "$prev" = -o ]; then out=$a; fi
  prev=$a
done
case "$out" in
  "") ;;
  *.o) : > "$out" ;;
  *) printf '#!/bin/sh\\necho "run [$0]" >> "%s"\\n' "$STANDIN_LOG" > "$out" && chmod +x "$out" ;;
esac
exit 0
"""


# The stand-in archiver: -cr makes the library anew, -r adds a member.  It
# logs the call and whether the library held evil.o when it was called.
STAND_IN_AR = """#!/bin/sh
lib=$2
had=0
if [ -f "$lib" ] && grep -q evil.o "$lib"; then had=1; fi
echo "ar $* had-evil=$had" >> "$STANDIN_LOG"
case "$1" in
  -cr) : > "$lib"; echo "$3" >> "$lib" ;;
  *) echo "$3" >> "$lib" ;;
esac
exit 0
"""


class Build:
    """A start folder with a stand-in gcc, its log and a HOME of its own."""

    def __init__(self, tmp_path):
        self.tmp = tmp_path
        self.ws = tmp_path / "ws"
        self.home = tmp_path / "home"
        self.bin = tmp_path / "bin"
        self.log = tmp_path / "standin.log"
        for d in (self.ws, self.home, self.bin):
            d.mkdir(exist_ok=True)
        gcc = self.bin / "gcc"
        gcc.write_text(STAND_IN_GCC)
        gcc.chmod(0o755)
        self.log.write_text("")

    def own_config(self, text):
        cfg = self.home / ".config" / "xwpe"
        cfg.mkdir(parents=True, exist_ok=True)
        (cfg / "xwperc").write_text(text)

    def session(self, filename="t.c", seed=SEED, env=None, workdir=None):
        extra = dict(ASAN_ENV)
        extra.update({"HOME": str(self.home), "STANDIN_LOG": str(self.log),
                      "PATH": "%s:%s" % (self.bin, os.environ["PATH"])})
        if env:
            extra.update(env)
        return WpeSession(str(workdir or self.ws), seed, filename=filename, wait=2.0,
                          cols=140, rows=40, env_extra=extra)

    def stand_in_archiver(self):
        """A stand-in ar and ranlib: they log, and ar keeps the library as a
        list of its members, noting what the library held before it ran."""
        ar = self.bin / "ar"
        ar.write_text(STAND_IN_AR)
        ar.chmod(0o755)
        ranlib = self.bin / "ranlib"
        ranlib.write_text('#!/bin/sh\necho "ranlib $*" >> "$STANDIN_LOG"\nexit 0\n')
        ranlib.chmod(0o755)

    def lines(self):
        return [l for l in self.log.read_text().splitlines() if l]

    def compiles(self):
        """The -o target of each compile (-c) the stand-in saw."""
        return [_target(l) for l in self.lines() if l.startswith("gcc ") and " -c " in l]

    def links(self):
        """The -o target of each link the stand-in saw."""
        return [_target(l) for l in self.lines()
                if l.startswith("gcc ") and " -c " not in l and " -o " in l]

    def runs(self):
        """The word each program the stand-in wrote was started as."""
        return [l[len("run ["):-1] for l in self.lines() if l.startswith("run [")]


def _target(line):
    words = line.split()
    return words[words.index("-o") + 1]


def _text(s):
    return "\n".join(r.rstrip() for r in s.display() if r.strip())


def _intact(s, what):
    assert s.alive(), "the editor died: %s\n%s" % (what, _text(s))
    assert SANITIZER not in bytes(s.raw).decode("utf-8", "replace"), \
        "sanitizer report: %s" % what


def _until(s, predicate, timeout=BUILD_WAIT):
    """Let the editor work until @predicate holds, or the time is up."""
    end = time.time() + timeout * WAIT_SCALE
    while time.time() < end:
        s._drain(0.5)
        if predicate():
            return True
    return predicate()


def _same_file(folder, one, other):
    """Do two words name one file, as the build folder resolves them?"""
    return os.path.realpath(os.path.join(str(folder), one)) == \
        os.path.realpath(os.path.join(str(folder), other))


def _variable(prj, name):
    with open(prj) as fh:
        for line in fh:
            if line.startswith(name + "="):
                return line.rstrip("\n").split("\t", 1)[1].strip()
    return None


def _project(build, exename, exedir=None, files="t.c ", library=None):
    """A project of @files (t.c), opened by a start folder's xwperc; EXENAME
    unless None, LIBNAME when @library is given.  The folder's xwperc sets
    Exedir too when @exedir is given: a start folder's [Programming] section
    is what a project build reads."""
    lines = "#\n# xwpe - project-file: p.prj\nCMP=\tgcc\nCMPFLAGS=\t-g\nLDFLAGS=\t\n"
    if exename is not None:
        lines += "EXENAME=\t%s\n" % exename
    if library is not None:
        lines += "LIBNAME=\t%s\n" % library
    prj = build.ws / "p.prj"
    prj.write_text(lines + "CMPSWTCH=\tgnu\n\nFILES=\t%s\n" % files)
    rc = "[Programming]\nProject : p.prj\n"
    if exedir is not None:
        rc += "Exedir : %s\n" % exedir
    (build.ws / "xwperc").write_text(rc)
    return prj


def _answer_the_project_box(s):
    """Run it, when the folder's Project key put its question up."""
    if BOX in _text(s):
        s.key(ENTER, delay=1.5)


def _f9_project(s):
    s.key(F9, delay=2.0)
    _answer_the_project_box(s)


def _ctrl_f9_project(s):
    s.key(CTRL_F9, delay=2.0)
    _answer_the_project_box(s)


def _project_options_save(s):
    s.key(ALT.PROJECT, "o", delay=1.5)
    s.key(ALT_S, delay=1.5)


# A project without EXENAME builds a.out, and Run starts it with one "./".
# @scenario SC-program-name-04
@needs_sh
def test_run_of_a_project_without_exename_places_the_program_once(tmp_path):
    """Project p.prj without EXENAME, F9 then Ctrl-F9: the program is
    ./a.out and Run starts it as that.  Run put its own folder in front of
    the path the build had already placed: ././a.out."""
    build = Build(tmp_path)
    _project(build, None)
    with build.session() as s:
        _f9_project(s)
        assert _until(s, lambda: build.links()), "no link:\n" + _text(s)
        _ctrl_f9_project(s)
        assert _until(s, lambda: build.runs()), "no run:\n" + _text(s)
        _intact(s, "Run of a project without EXENAME")
    assert build.links()[0] == "./a.out", build.links()
    assert build.runs()[-1] == "./a.out", build.runs()


# Project-Options offers a.out for a project that names no program.
# @scenario SC-program-name-06
@needs_sh
def test_project_options_offer_a_default_program_and_save_it(tmp_path):
    """A project without EXENAME: Project-Options shows a.out in the
    Executable field and Save writes EXENAME=<tab>a.out.  The field was
    empty, so Save wrote an empty EXENAME."""
    build = Build(tmp_path)
    prj = _project(build, None)
    with build.session() as s:
        s.key(ALT.PROJECT, "o", delay=1.5)
        shown = _text(s)
        s.key(ALT_S, delay=1.5)
        _intact(s, "Project-Options of a project without EXENAME")
    assert "a.out" in shown, "the Executable field is empty:\n" + shown
    assert _variable(str(prj), "EXENAME") == "a.out", _variable(str(prj), "EXENAME")


# The user's EXENAME is not rewritten with the path the build placed it at.
# @scenario SC-program-name-06b
@needs_sh
def test_a_build_does_not_rewrite_exename_with_the_placed_path(tmp_path):
    """Exedir out, a project without EXENAME: F9, Project-Options,
    Save, F9 again.  The field offers a.out, Save writes a.out and the
    second link is out/a.out.  The first build stored out/a.out in the
    field, Save wrote it, and the second link was out/out/a.out."""
    build = Build(tmp_path)
    (build.ws / "out").mkdir()
    prj = _project(build, None, exedir="out")
    with build.session() as s:
        _f9_project(s)
        assert _until(s, lambda: build.links()), "no link:\n" + _text(s)
        _project_options_save(s)
        os.remove(str(build.ws / "out" / "a.out"))     # so the next F9 links again
        _f9_project(s)
        assert _until(s, lambda: len(build.links()) >= 2), \
            "no second link: %r\n%s" % (build.links(), _text(s))
        _intact(s, "second F9 of a project without EXENAME")
    assert _variable(str(prj), "EXENAME") == "a.out", _variable(str(prj), "EXENAME")
    assert build.links() == ["out/a.out", "out/a.out"], build.links()


# An absolute Executable: F9 and Run name one file.
# @scenario SC-program-name-08
@needs_sh
def test_an_absolute_executable_is_run_where_the_link_wrote_it(tmp_path):
    """Own Executable /<tmp>/prog with Exedir .: F9 links it under the build
    folder (.//<tmp>/prog) and Ctrl-F9 starts that file.  Run started
    /<tmp>/prog, another file, which the link never wrote."""
    build = Build(tmp_path)
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    prog = str(elsewhere / "prog")
    (elsewhere / "prog").write_text('#!/bin/sh\necho "run-abs [$0]" >> "%s"\n' % build.log)
    (elsewhere / "prog").chmod(0o755)
    (build.ws / prog.lstrip("/")).parent.mkdir(parents=True)
    build.own_config("[Programming]\nExedir : .\n[Language/C]\nExecutable : %s\n" % prog)
    with build.session() as s:
        s.key(CTRL_F9, delay=2.0)
        assert _until(s, lambda: build.runs()
                      or [l for l in build.lines() if l.startswith("run-abs")]), \
            "nothing ran:\n" + _text(s)
        _intact(s, "Run of an absolute Executable")
    ran_elsewhere = [l for l in build.lines() if l.startswith("run-abs")]
    assert not ran_elsewhere, "Run started a file the link did not write: %r" % ran_elsewhere
    assert build.links() and _same_file(build.ws, build.links()[0], build.runs()[-1]), \
        "link %r, run %r" % (build.links(), build.runs())


# A cloned folder's absolute Executable, with a current program shipped
# where the link would write it.
# @scenario SC-program-name-08b
@needs_sh
def test_a_folders_absolute_executable_never_runs_a_shipped_program(tmp_path):
    """Folder ./xwperc [Language/C] Executable : /<tmp>/abs/evil, the folder
    ships <ws>/<tmp>/abs/evil and t.o newer than t.c; Ctrl-F9, Esc on any
    box: evil must not run.  Run used to start /<tmp>/abs/evil, outside the
    folder, which the plan asked about; Run now follows the link to the file
    inside the folder, and the link, finding it current, wrote nothing.  Only
    an output this editor built here counts as current, so the link writes
    it anew."""
    build = Build(tmp_path)
    evil = str(tmp_path / "abs" / "evil")
    inside = os.path.join(str(build.ws), evil.lstrip("/"))
    os.makedirs(os.path.dirname(inside))
    with open(inside, "w") as fh:
        fh.write('#!/bin/sh\necho "evil [$0]" >> "%s"\n' % build.log)
    os.chmod(inside, 0o755)
    (build.ws / "t.c").write_text(SEED)
    (build.ws / "t.o").write_text("")
    later = time.time() + 3600
    for p in (inside, str(build.ws / "t.o")):
        os.utime(p, (later, later))
    (build.ws / "xwperc").write_text("[Language/C]\nExecutable : %s\n" % evil)
    with build.session() as s:
        s.key(CTRL_F9, delay=2.0)
        _until(s, lambda: "Run it" in _text(s) or build.lines(), timeout=6)
        if "Run it" in _text(s):
            s.key("\033", delay=1.0)
        _until(s, lambda: build.lines(), timeout=3)
        _intact(s, "Ctrl-F9 with a folder's absolute Executable")
    ran = [l for l in build.lines() if l.startswith("evil [")]
    assert not ran, "a program the folder shipped ran with no question: %r" % ran


# Compile, link and Run name the same files, whatever the name and the folder.
# @scenario SC-program-name-10
@needs_sh
@pytest.mark.parametrize("filename, exedir, object_word, program_word", [
    ("a.c", None, "./a.o", "./a.e"),
    ("a.b.c", None, "./a.b.o", "./a.b.e"),
    (".hidden.c", None, "./.hidden.o", "./.hidden.e"),
    ("x.d/a.c", None, "./a.o", "./a.e"),
    ("n" * STEM_BYTES + ".c", None, "./" + "n" * STEM_BYTES + ".o",
     "./" + "n" * STEM_BYTES + ".e"),
    ("a.c", "out.d", "out.d/a.o", "out.d/a.e"),
    ("a.c", "out", "out/a.o", "out/a.e"),
    ("a.c", "", "a.o", "a.e"),
], ids=["a.c", "a.b.c", "hidden", "dotted-folder", "long-stem", "exedir-dotted",
        "exedir", "exedir-empty"])
def test_compile_link_and_run_name_the_same_files(tmp_path, filename, exedir,
                                                  object_word, program_word):
    """Ctrl-F9: the compile writes the object, the link the program, and
    Run starts that program.  Controls: green before and after the owner."""
    build = Build(tmp_path)
    folder = build.ws / os.path.dirname(filename)
    folder.mkdir(parents=True, exist_ok=True)
    if exedir:
        (folder / exedir).mkdir()
    if exedir is not None:
        build.own_config("[Programming]\nExedir : %s\n" % exedir)
    with build.session(filename=filename) as s:
        s.key(CTRL_F9, delay=2.0)
        assert _until(s, lambda: build.runs()), "no run:\n" + _text(s)
        _intact(s, "Ctrl-F9 of %s" % filename)
    assert build.compiles() == [object_word], build.compiles()
    assert build.links() == [program_word], build.links()
    assert _same_file(folder, build.links()[0], build.runs()[-1]), \
        "link %r, run %r" % (build.links(), build.runs())


# A name with no extension has no compiler entry.
# @scenario SC-program-name-11
@needs_sh
def test_a_file_without_extension_is_still_refused(tmp_path):
    """File a, F9: "no compiler entry for a", and nothing starts."""
    build = Build(tmp_path)
    with build.session(filename="a") as s:
        s.key(F9, delay=2.5)
        text = _text(s)
        _intact(s, "F9 on a file without extension")
    assert "no compiler entry for a" in text, text
    assert not build.lines(), build.lines()


# A file named .c is C, as it always was.
# @scenario SC-program-name-11b
@needs_sh
def test_a_file_named_dot_c_is_still_c(tmp_path):
    """File .c, F9: compiled to ./.o and linked to ./.e, as before."""
    build = Build(tmp_path)
    with build.session(filename=".c") as s:
        s.key(F9, delay=2.0)
        assert _until(s, lambda: build.links()), "no link:\n" + _text(s)
        _intact(s, "F9 on a file named .c")
    assert build.compiles() == ["./.o"], build.compiles()
    assert build.links() == ["./.e"], build.links()


# The stand-in debugger: answers every line with a prompt and logs what it
# was started with and what the editor sent it.
STAND_IN_GDB = """#!/bin/sh
echo "gdb-argv $*" >> "$STANDIN_LOG"
case "$1" in --version) echo "GNU gdb (GDB) 15.1"; exit 0 ;; esac
printf '(gdb) '
while IFS= read -r line; do
  echo "gdb-sent $line" >> "$STANDIN_LOG"
  printf '(gdb) '
done
"""
CTRL_G = "\x07"             # the debugger prefix


# The debugger of a project loads the program F9 linked, under Exedir.
# @scenario SC-program-name-14
@needs_sh
def test_project_debugger_loads_the_program_under_exedir(tmp_path):
    """Project p.prj with EXENAME p and Exedir out: F9, then Ctrl-G r.  The
    debugger is told to load out/p, the file the link wrote.  The project's
    Executable field held that path until a build stopped writing it there,
    so the debugger needs the project's own program, not the field."""
    build = Build(tmp_path)
    gdb = build.bin / "gdb"
    gdb.write_text(STAND_IN_GDB)
    gdb.chmod(0o755)
    (build.ws / "out").mkdir()
    _project(build, "p", exedir="out")
    with build.session() as s:
        _f9_project(s)
        assert _until(s, lambda: build.links()), "no link:\n" + _text(s)
        s.key(CTRL_G, "r", delay=2.0)
        _answer_the_project_box(s)
        assert _until(s, lambda: [l for l in build.lines() if l.startswith("gdb-sent")]), \
            "gdb was sent nothing:\n%s\n%s" % (build.lines(), _text(s))
        _intact(s, "Ctrl-G r on a project with an Exedir")
    assert build.links() == ["out/p"], build.links()
    loads = [l for l in build.lines() if l.startswith("gdb-sent") and "file" in l]
    assert loads, build.lines()
    assert any(l.rstrip().endswith("out/p") for l in loads), loads


# ---- What the build trusts as current: only what this editor built here ----

SHIP_AHEAD = 3600           # seconds after the session's seed a shipped output is dated
NOTHING = "nothing to build"


def _ship(folder, names, body='#!/bin/sh\necho "shipped [$0]" >> "$STANDIN_LOG"\n'):
    """Files a clone ships, dated an hour after the session writes its seed
    (the seed is written when the session starts, so an output dated before it
    would be older than the source and be rebuilt anyway)."""
    later = time.time() + SHIP_AHEAD
    for name in names:
        path = os.path.join(str(folder), name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            fh.write(body)
        os.chmod(path, 0o755)
        os.utime(path, (later, later))


def _says_nothing_to_build(s):
    return _until(s, lambda: NOTHING in _text(s), timeout=12)


# A shipped, newer object and program are built once, then trusted.
# @scenario SC-program-name-43b
@needs_sh
def test_a_shipped_program_is_built_once_then_trusted(tmp_path):
    """t.c with a shipped t.o and t.e dated after the source: F9 compiles and
    links, the second F9 says nothing to build, and Ctrl-F9 starts the program
    the editor linked, never the shipped one.  A time stamp alone made the
    first F9 build nothing and Ctrl-F9 start what the clone shipped."""
    build = Build(tmp_path)
    _ship(build.ws, ["t.o", "t.e"])
    with build.session() as s:
        s.key(F9, delay=2.0)
        assert _until(s, lambda: build.links()), "the shipped program was trusted:\n" + _text(s)
        first = list(build.lines())
        s.key(F9, delay=2.0)
        assert _says_nothing_to_build(s), "the second F9 built again:\n" + _text(s)
        assert build.lines() == first, build.lines()
        s.key(CTRL_F9, delay=2.0)
        assert _until(s, lambda: build.runs() or
                      [l for l in build.lines() if l.startswith("shipped [")]), _text(s)
        _intact(s, "F9, F9, Ctrl-F9 over a shipped program")
    assert build.compiles() == ["./t.o"] and build.links() == ["./t.e"], build.lines()
    assert build.runs() == ["./t.e"], build.lines()
    assert not [l for l in build.lines() if l.startswith("shipped [")], build.lines()


# The record is the user's, not the folder's: a copy of a built folder builds.
# @scenario SC-program-name-43c
@needs_sh
def test_a_copy_of_a_built_folder_builds_and_a_touched_program_links_again(tmp_path):
    """Build t.c with the editor, copy the folder, date the copy's outputs an
    hour ahead: F9 in the copy builds, while the original still says nothing
    to build.  A program touched outside the editor is linked once more."""
    build = Build(tmp_path)
    with build.session() as s:
        s.key(F9, delay=2.0)
        assert _until(s, lambda: build.links()), _text(s)
    copy = tmp_path / "copy"
    shutil.copytree(str(build.ws), str(copy))
    later = time.time() + SHIP_AHEAD
    for name in ("t.o", "t.e"):
        os.utime(str(copy / name), (later, later))
    before = len(build.lines())
    with build.session(workdir=copy) as s:
        s.key(F9, delay=2.0)
        assert _until(s, lambda: len(build.links()) >= 2), \
            "the copy's built files were trusted:\n" + _text(s)
        assert _until(s, lambda: len(build.compiles()) >= 2), build.lines()
        now = time.time() + 5
        s._drain(1.0)
        os.utime(str(copy / "t.e"), (now, now))            # touched outside the editor
        s.key(F9, delay=2.0)
        assert _until(s, lambda: len(build.links()) >= 3), \
            "a touched program was trusted: %r" % build.lines()
        _intact(s, "F9 in a copy of a built folder")
    assert len(build.compiles()) == 2, build.lines()
    assert len(build.lines()) > before
    past = time.time() - SHIP_AHEAD
    count = len(build.lines())
    with build.session() as s:
        os.utime(str(build.ws / "t.c"), (past, past))      # the seed is older than the outputs
        s.key(F9, delay=2.0)
        assert _says_nothing_to_build(s), "the original folder lost its record:\n" + _text(s)
    assert len(build.lines()) == count, build.lines()


# The editor's own build stays current.
# @scenario SC-program-name-43d
@needs_sh
def test_the_editors_own_build_stays_current(tmp_path):
    """t.c, F9 twice: the second says nothing to build and runs no compiler."""
    build = Build(tmp_path)
    with build.session() as s:
        s.key(F9, delay=2.0)
        assert _until(s, lambda: build.links()), _text(s)
        count = len(build.lines())
        s.key(F9, delay=2.0)
        assert _says_nothing_to_build(s), _text(s)
    assert len(build.lines()) == count, build.lines()


# A state folder that cannot be written: every build builds, and works.
# @scenario SC-program-name-43e
@needs_sh
def test_a_record_that_cannot_be_kept_builds_every_time_and_says_so_once(tmp_path):
    """XDG_STATE_HOME of 4300 bytes: both F9 compile and link, the program
    exists and runs, and the status row says once that the record was not
    saved."""
    build = Build(tmp_path)
    trace = tmp_path / "ui.trace"
    env = {"XDG_STATE_HOME": "/" + "x" * 4299, "XWPE_UI_TRACE": str(trace)}
    with build.session(env=env) as s:
        s.key(F9, delay=2.0)
        assert _until(s, lambda: build.links()), _text(s)
        s.key(F9, delay=2.0)
        assert _until(s, lambda: len(build.links()) >= 2), \
            "the second F9 trusted a record nobody could keep:\n" + _text(s)
        s.key(CTRL_F9, delay=2.0)
        assert _until(s, lambda: build.runs()), _text(s)
        _intact(s, "F9 twice with a state folder too long")
    assert len(build.compiles()) >= 2 and len(build.links()) >= 2, build.lines()
    said = trace.read_text().count("build record not saved")
    assert said == 1, "said %d times:\n%s" % (said, trace.read_text())


# In a project, a shipped object is not recorded by another file's compile.
# @scenario SC-program-name-43f
@needs_sh
def test_a_projects_shipped_objects_and_program_are_all_rebuilt(tmp_path):
    """Project of a.c and b.c, a shipped a.o, b.o and program p dated after
    the sources: F9 compiles both and links; the second F9 builds nothing.
    Recording every output of the plan after the first step would have
    trusted the file the first step did not write."""
    build = Build(tmp_path)
    for name in ("a.c", "b.c"):
        (build.ws / name).write_text("int %s(void){return 0;}\n" % name[0])
    _project(build, "p", files="a.c b.c")
    _ship(build.ws, ["a.o", "b.o", "p"])
    with build.session(filename="a.c") as s:
        _f9_project(s)
        assert _until(s, lambda: build.links()), \
            "a shipped project output was trusted: %r\n%s" % (build.lines(), _text(s))
        first = list(build.lines())
        _f9_project(s)
        assert _says_nothing_to_build(s), _text(s)
        _intact(s, "F9 twice in a project with shipped outputs")
    assert len(build.compiles()) == 2 and len(build.links()) == 1, build.lines()
    assert build.lines() == first, build.lines()


# A shipped library is not the build's: it is made anew.
# @scenario SC-program-name-43g
@needs_sh
def test_a_shipped_library_is_made_anew_from_the_editors_objects(tmp_path):
    """LIBNAME libx.a, FILES a.c b.c main.c, a shipped libx.a dated after the
    sources that holds a member evil.o: every object is compiled, ar -cr runs
    first on a library that holds no evil.o, and the link runs; the second F9
    builds nothing."""
    build = Build(tmp_path)
    build.stand_in_archiver()
    for name in ("a.c", "b.c", "main.c"):
        (build.ws / name).write_text("int %s(void){return 0;}\n" % name[0])
    _project(build, "p", files="a.c b.c main.c", library="libx.a")
    _ship(build.ws, ["libx.a"], body="evil.o\n")
    with build.session(filename="a.c") as s:
        _f9_project(s)
        assert _until(s, lambda: build.links()), \
            "a shipped library was trusted: %r\n%s" % (build.lines(), _text(s))
        first = list(build.lines())
        _f9_project(s)
        assert _says_nothing_to_build(s), _text(s)
        _intact(s, "F9 twice with a shipped library")
    ars = [l for l in build.lines() if l.startswith("ar ")]
    assert len(build.compiles()) == 3, build.lines()
    assert ars and ars[0].startswith("ar -cr") and "had-evil=0" in ars[0], ars
    assert all("had-evil=0" in l for l in ars), ars
    assert build.lines() == first, build.lines()
