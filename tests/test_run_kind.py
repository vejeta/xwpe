"""What Ctrl-F9 starts after the build is a kind of the language's entry.

Run used to read the compiler style (CompilerSwitch) as "the compiler is an
interpreter": for Python, Perl and Algol 68 that is right, and for Rust,
Pascal, Go, Java, Scala and LaTeX it made Ctrl-F9 run the compiler a second
time and never the program.  Now the entry says what it starts, in a field
read as `Run` in an xwperc: `interpreter` (the Compiler runs the source),
`program` (the program the build made), `vm` (java runs the class the build
made) or `none` (LaTeX).  An entry that names none takes it from its
compiler style, and a folder's xwperc that chooses what starts is asked about
before it runs, like its Compiler, Exedir and Executable.

The tools are stand-ins written here and put first on PATH.  Each logs its
words in brackets; a compiler writes what the real one writes (the program
`./t`, or `T.class`); the program and the VM log the word they were started
as and their arguments.  So the log says, per row, what Ctrl-F9 started.
The Java rows with real tools (35, 36) run the system's JDK.

Run: WPE_BIN=$PWD/wpe pytest tests/test_run_kind.py (and wpe-asan).
"""
import os
import shutil
import time

import pytest
from wpe_driver import ALT, WpeSession, tool_usable, wait_screen

import test_program_name as pn

F9 = pn.F9
CTRL_F9 = pn.CTRL_F9
ESC = "\033"
RUN_IT = "\033r"
USE_MINE = "\033u"
CTRL_G = "\x07"
ALT_O = ALT.OPTIONS
ASK_WAIT = 12               # seconds the box has to come up in
SEED_J = "class T { public static void main(String[] a) { } }\n"
ARGS = "[Programming]\nArguments : A1\n"
BOX = "Command from ./xwperc"
RUN_BOX = "sets the program to run to"   # Run's own box, not the build's

needs_sh = pn.needs_sh

# What a started program, or a VM, logs: its word and its arguments.
STAND_IN_PROGRAM = r"""#!/bin/sh
{ printf 'run [%s]' "$0"; for a in "$@"; do printf ' [%s]' "$a"; done; echo; } >> "$STANDIN_LOG"
if [ -n "$STANDIN_STDIN" ]; then read x; echo "stdin [$x]" >> "$STANDIN_LOG"; fi
"""

LOG_WORDS = '{ printf "%s" "NAME"; for a in "$@"; do printf " [%s]" "$a"; done; echo; } >> "$STANDIN_LOG"\n'

STAND_IN_VM = r"""#!/bin/sh
{ printf "NAME"; for a in "$@"; do printf " [%s]" "$a"; done; echo; } >> "$STANDIN_LOG"
echo "cwd [$PWD]" >> "$STANDIN_LOG"
echo "classpath [$CLASSPATH]" >> "$STANDIN_LOG"
if [ -n "$STANDIN_STDIN" ]; then read x; echo "stdin [$x]" >> "$STANDIN_LOG"; fi
"""


class Kit(pn.Build):
    """A start folder, a HOME of its own, and stand-in tools."""

    def tool(self, name, text, beside=False):
        """A stand-in on PATH.  With @beside it lives in a folder of its own
        and only a link to it is on PATH, so a java next to it is found by
        following the link and is on no PATH."""
        path = self.bin / name
        if beside:
            home = self.tmp / "jdk"
            home.mkdir(exist_ok=True)
            path = home / name
        path.write_text(text)
        path.chmod(0o755)
        if beside:
            (self.bin / name).symlink_to(path)

    def compiler(self, name, writes, beside=False):
        """A compiler that logs its words and writes what the real one writes
        in the folder it ran in: the program (as ./<stem>), a class, or nothing."""
        text = "#!/bin/sh\n" + LOG_WORDS.replace("NAME", name)
        text += 'for a in "$@"; do src=$a; done\nstem=${src##*/}\nstem=${stem%.*}\n'
        if writes == "program":
            text += 'cat > "$stem" <<\'EOF\'\n' + STAND_IN_PROGRAM + 'EOF\nchmod +x "$stem"\n'
        elif writes == "class":
            text += ': > "$stem.class"\n'
        self.tool(name, text + "exit 0\n", beside)

    def vm(self, name):
        """The VM next to the compiler, in the folder the compiler's link
        points into, never on PATH itself."""
        home = self.tmp / "jdk"
        home.mkdir(exist_ok=True)
        (home / name).write_text(STAND_IN_VM.replace("NAME", name))
        (home / name).chmod(0o755)

    def interpreter(self, name):
        self.tool(name, "#!/bin/sh\n" + LOG_WORDS.replace("NAME", name) + "exit 0\n")

    def session(self, filename="t.c", seed=pn.SEED, env=None, workdir=None):
        extra = {"XWPE_UI_TRACE": str(self.tmp / "ui.trace")}
        if env:
            extra.update(env)
        return pn.Build.session(self, filename=filename, seed=seed, env=extra, workdir=workdir)

    def trace(self):
        try:
            return (self.tmp / "ui.trace").read_text()
        except OSError:
            return ""

    def box_lines(self):
        return [l.split("startplan line ", 1)[1] for l in self.trace().splitlines()
                if "startplan line " in l]

    def starts(self, word):
        """The lines the log holds that begin with @word."""
        return [l for l in self.lines() if l == word or l.startswith(word + " ")]

    def ship(self, name, text=STAND_IN_PROGRAM):
        """A program the folder ships, dated like a fresh clone's files."""
        path = self.ws / name
        path.write_text(text)
        path.chmod(0o755)

    def folder_rc(self, text):
        (self.ws / "xwperc").write_text(text)


def _until_line(s, build, prefix, timeout=pn.BUILD_WAIT):
    return pn._until(s, lambda: any(l.startswith(prefix) for l in build.lines()), timeout)


def _quiet_run(s, build, key=CTRL_F9):
    """Ctrl-F9 and let the build and the run, if any, finish."""
    s.key(key, delay=2.0)
    pn._until(s, lambda: any(l.startswith("run [") for l in build.lines()), timeout=10)
    s._drain(1.5)


def _asked(s, timeout=ASK_WAIT):
    return wait_screen(s, BOX, tmo=timeout)


def _flat(s):
    return "".join(pn._text(s).split())


# ---- Rows 20, 21, 23: a compiler that writes its own program runs it ----

def _runs_its_program(tmp_path, tool, seed, filename, options):
    kit = Kit(tmp_path)
    kit.compiler(tool, "program")
    kit.own_config(ARGS)
    with kit.session(filename=filename, seed=seed) as s:
        s.key(CTRL_F9, delay=2.0)
        assert pn._until(s, lambda: kit.starts("run"), 20), \
            "the program never ran: %r\n%s" % (kit.lines(), pn._text(s))
        pn._intact(s, "Ctrl-F9 on a %s file" % tool)
    assert len(kit.starts(tool)) == 1, "the compiler ran again: %r" % kit.lines()
    assert kit.starts("run") == ["run [./t] [A1]"], kit.lines()


# @scenario SC-program-name-20
@needs_sh
def test_rust_runs_the_program_it_wrote(tmp_path):
    """t.rs, Ctrl-F9: rustc once, then ./t with the Arguments.  Ctrl-F9 used
    to run rustc a second time and never the program."""
    _runs_its_program(tmp_path, "rustc", "fn main() {}\n", "t.rs", None)


# @scenario SC-program-name-21
@needs_sh
def test_pascal_runs_the_program_it_wrote(tmp_path):
    """t.pas, Ctrl-F9: fpc once, then ./t."""
    _runs_its_program(tmp_path, "fpc", "begin end.\n", "t.pas", None)


# @scenario SC-program-name-23
@needs_sh
def test_go_runs_the_program_it_wrote(tmp_path):
    """t.go, Ctrl-F9: go build once, then ./t.  Run used to start `go t.go`,
    which exits 2."""
    _runs_its_program(tmp_path, "go", "package main\nfunc main() {}\n", "t.go", None)


# ---- Row 22, 24: the VM runs the class the build made ----

def _vm_kit(tmp_path, compiler, runner):
    kit = Kit(tmp_path)
    kit.compiler(compiler, "class", beside=True)
    kit.vm(runner)
    return kit


# @scenario SC-program-name-22
@needs_sh
def test_java_runs_its_class_on_the_vm_beside_javac(tmp_path):
    """T.java, Ctrl-F9: javac once, then the java beside javac starts the
    class T in the build folder with no class path of its own."""
    kit = _vm_kit(tmp_path, "javac", "java")
    kit.own_config(ARGS)
    with kit.session(filename="T.java", seed=SEED_J) as s:
        s.key(CTRL_F9, delay=2.0)
        assert pn._until(s, lambda: kit.starts("java"), 20), \
            "no java: %r\n%s" % (kit.lines(), pn._text(s))
        pn._intact(s, "Ctrl-F9 on T.java")
    assert len(kit.starts("javac")) == 1, kit.lines()
    assert kit.starts("java") == ["java [T] [A1]"], kit.lines()
    assert any(os.path.realpath(l[len("cwd ["):-1]) == os.path.realpath(str(kit.ws))
               for l in kit.lines() if l.startswith("cwd [")), kit.lines()
    assert "java [T] [A1]" in kit.lines() and not any(
        "-cp" in l or "-classpath" in l for l in kit.starts("java")), kit.lines()


# @scenario SC-program-name-22b
@needs_sh
def test_a_folders_java_is_shown_with_its_class_in_the_box(tmp_path):
    """A folder sets Arguments: the box shows the java as found (the stand-in
    beside javac) and the class T, so the user reads which VM would start."""
    kit = _vm_kit(tmp_path, "javac", "java")
    kit.folder_rc(ARGS)
    with kit.session(filename="T.java", seed=SEED_J) as s:
        s.key(CTRL_F9, delay=2.0)
        assert _asked(s), pn._text(s)
        flat = _flat(s)
        s.key(ESC, delay=1.0)
        pn._intact(s, "the box of a Java Run")
    assert "jdk/java" in flat and "TA1" in flat, flat
    assert not kit.starts("java"), "java started after Esc: %r" % kit.lines()


# @scenario SC-program-name-22c
@needs_sh
def test_a_folders_java_compiler_asks_again_before_its_java_starts(tmp_path):
    """Folder [Language/Java] Compiler : jdk/javac, with a java the folder
    ships beside it: the build asks (Run it), and Run asks again before the
    folder's java starts, since the java is found beside that Compiler.  Esc:
    the folder's java never starts."""
    kit = Kit(tmp_path)
    jdk = kit.ws / "jdk"
    jdk.mkdir()
    (jdk / "javac").write_text("#!/bin/sh\n" + LOG_WORDS.replace("NAME", "fjavac")
                               + ': > T.class\nexit 0\n')
    (jdk / "java").write_text(STAND_IN_VM.replace("NAME", "fjava"))
    (jdk / "javac").chmod(0o755)
    (jdk / "java").chmod(0o755)
    kit.folder_rc("[Language/Java]\nCompiler : jdk/javac\n")
    with kit.session(filename="T.java", seed=SEED_J) as s:
        s.key(CTRL_F9, delay=2.0)
        assert _asked(s), "the folder's compiler was not asked:\n" + pn._text(s)
        s.key(RUN_IT, delay=2.0)
        assert pn._until(s, lambda: kit.starts("fjavac"), 20), kit.lines()
        asked_run = wait_screen(s, RUN_BOX, tmo=ASK_WAIT)
        s.key(ESC, delay=1.0)
        s._drain(1.0)
        pn._intact(s, "a folder's javac with its own java")
    assert asked_run, "the folder's java was not asked about: %r" % kit.lines()
    assert not kit.starts("fjava"), "the folder's java started: %r" % kit.lines()


# @scenario SC-program-name-24
@needs_sh
def test_scala_runs_its_class_on_the_vm_beside_scalac(tmp_path):
    """T.scala, Ctrl-F9: scalac once, then the scala runner beside scalac
    starts the class T, never java (java alone cannot find scala-library:
    NoClassDefFoundError scala/Predef$).  Runner: scala, found beside scalac
    as java is beside javac."""
    kit = _vm_kit(tmp_path, "scalac", "scala")
    kit.vm("java")   # a java beside scalac must not be the one started
    kit.own_config(ARGS)
    with kit.session(filename="T.scala", seed="object T { def main(a: Array[String]) = () }\n") as s:
        s.key(CTRL_F9, delay=2.0)
        assert pn._until(s, lambda: kit.starts("scala"), 20), \
            "no scala: %r\n%s" % (kit.lines(), pn._text(s))
        pn._intact(s, "Ctrl-F9 on T.scala")
    assert len(kit.starts("scalac")) == 1, kit.lines()
    assert kit.starts("scala") == ["scala [T] [A1]"], kit.lines()
    assert not kit.starts("java"), "java started: %r" % kit.lines()


# @scenario SC-program-name-24c
@needs_sh
def test_a_folders_scala_compiler_asks_again_before_its_scala_starts(tmp_path):
    """Folder [Language/Scala] Compiler : sdk/scalac, with a scala the folder
    ships beside it: the build asks (Run it), and Run asks again before the
    folder's scala starts, since the runner is found beside that Compiler.
    Esc: the folder's scala never starts."""
    kit = Kit(tmp_path)
    sdk = kit.ws / "sdk"
    sdk.mkdir()
    (sdk / "scalac").write_text("#!/bin/sh\n" + LOG_WORDS.replace("NAME", "fscalac")
                                + ': > T.class\nexit 0\n')
    (sdk / "scala").write_text(STAND_IN_VM.replace("NAME", "fscala"))
    (sdk / "scalac").chmod(0o755)
    (sdk / "scala").chmod(0o755)
    kit.folder_rc("[Language/Scala]\nCompiler : sdk/scalac\n")
    with kit.session(filename="T.scala", seed="object T { def main(a: Array[String]) = () }\n") as s:
        s.key(CTRL_F9, delay=2.0)
        assert _asked(s), "the folder's compiler was not asked:\n" + pn._text(s)
        s.key(RUN_IT, delay=2.0)
        assert pn._until(s, lambda: kit.starts("fscalac"), 20), kit.lines()
        asked_run = wait_screen(s, RUN_BOX, tmo=ASK_WAIT)
        flat = _flat(s)
        s.key(ESC, delay=1.0)
        s._drain(1.0)
        pn._intact(s, "a folder's scalac with its own scala")
    assert asked_run, "the folder's scala was not asked about: %r" % kit.lines()
    assert "sdk/scala" in flat, flat
    assert not kit.starts("fscala"), "the folder's scala started: %r" % kit.lines()


real_scala = pytest.mark.skipif(
    not (tool_usable("scalac", "-version") and tool_usable("scala", "-version")),
    reason="scalac and scala are not usable")


# @scenario SC-program-name-24b
@needs_sh
@real_scala
def test_scala_runs_a_real_object_with_the_scala_runner(tmp_path):
    """T.scala with the real scalac and scala, Ctrl-F9: the object prints its
    line.  java T would end in NoClassDefFoundError scala/Predef$."""
    kit = Kit(tmp_path)
    seed = 'object T { def main(a: Array[String]): Unit = println("hello from T") }\n'
    with kit.session(filename="T.scala", seed=seed) as s:
        s.key(CTRL_F9, delay=3.0)
        assert pn._until(s, lambda: "hello from T" in _flat_gap(s), 60), pn._text(s)
        assert "NoClassDefFoundError" not in pn._text(s), pn._text(s)
        pn._intact(s, "Ctrl-F9 on a real Scala object")


# ---- Row 25: LaTeX makes no program ----

# @scenario SC-program-name-25
@needs_sh
def test_latex_says_it_makes_no_program_to_run(tmp_path):
    """t.tex, Ctrl-F9: one pdflatex (F9's own step) and the status row says
    there is no program.  Ctrl-F9 ran pdflatex a second time and said nothing."""
    kit = Kit(tmp_path)
    kit.interpreter("pdflatex")
    with kit.session(filename="t.tex", seed="\\documentclass{article}\n") as s:
        s.key(CTRL_F9, delay=2.0)
        assert pn._until(s, lambda: "LaTeX makes no program to run" in pn._text(s), 20), \
            "nothing said:\n%s\n%r" % (pn._text(s), kit.lines())
        pn._intact(s, "Ctrl-F9 on t.tex")
    assert len(kit.starts("pdflatex")) == 1, kit.lines()


# ---- Rows 26, 33, 33b, 33c, 34: what a folder's xwperc chose is asked ----

# @scenario SC-program-name-26
@needs_sh
def test_a_folders_run_key_is_asked_and_shown(tmp_path):
    """Folder [Language/C] Run : interpreter, t.c, Ctrl-F9: the box names
    ./xwperc: Run = interpreter; Esc starts nothing (the compiler would have
    been handed the source as its program)."""
    kit = Kit(tmp_path)
    kit.folder_rc("[Language/C]\nRun : interpreter\n")
    with kit.session() as s:
        s.key(CTRL_F9, delay=2.0)
        asked = _asked(s)
        lines = kit.box_lines()
        s.key(ESC, delay=1.0)
        s._drain(1.0)
        pn._intact(s, "a folder's Run key")
    assert asked, "no question: %r" % kit.lines()
    assert any("Run = interpreter" in l for l in lines), lines
    assert not kit.starts("run"), kit.lines()
    assert len(kit.starts("gcc")) == 2, "gcc started as the interpreter: %r" % kit.lines()


# @scenario SC-program-name-33
@needs_sh
def test_a_folders_compiler_switch_never_runs_a_shipped_program_unasked(tmp_path):
    """Folder [Language/C] CompilerSwitch : 1 and a program `t` the folder
    ships: Ctrl-F9 asks, names CompilerSwitch = 1 and the word ./t; Esc runs
    nothing, Run it starts ./t.  The switch makes the compiler write its own
    program, so the word is a file the folder ships."""
    kit = Kit(tmp_path)
    kit.folder_rc("[Language/C]\nCompilerSwitch : 1\n")
    kit.ship("t")
    with kit.session() as s:
        s.key(CTRL_F9, delay=2.0)
        asked = _asked(s)
        box = pn._text(s)
        lines = kit.box_lines()
        s.key(ESC, delay=1.0)
        s._drain(1.0)
        ran_after_esc = kit.starts("run")
        pn._intact(s, "a folder's CompilerSwitch")
    assert asked, "no question: %r" % kit.lines()
    assert any("CompilerSwitch = 1" in l for l in lines), lines
    assert "./t" in box, box
    assert not ran_after_esc, "the shipped program ran after Esc: %r" % ran_after_esc


# @scenario SC-program-name-33a
@needs_sh
def test_a_folders_compiler_switch_then_run_it_starts_the_program(tmp_path):
    """The same folder, answered Run it: ./t starts."""
    kit = Kit(tmp_path)
    kit.folder_rc("[Language/C]\nCompilerSwitch : 1\n")
    kit.ship("t")
    with kit.session() as s:
        s.key(CTRL_F9, delay=2.0)
        assert _asked(s), pn._text(s)
        s.key(RUN_IT, delay=1.0)
        assert pn._until(s, lambda: kit.starts("run"), 15), kit.lines()
    assert kit.starts("run") == ["run [./t]"], kit.lines()


# @scenario SC-program-name-33b
@needs_sh
def test_a_folders_executable_for_a_self_written_program_is_asked(tmp_path):
    """Folder [Language/Rust] Executable : evil and a shipped `evil`, Ctrl-F9:
    the box names Executable = evil and the word ./evil; Esc: evil does not
    run.  rustc never writes that file."""
    kit = Kit(tmp_path)
    kit.compiler("rustc", "none")
    kit.folder_rc("[Language/Rust]\nExecutable : evil\n")
    kit.ship("evil")
    with kit.session(filename="t.rs", seed="fn main() {}\n") as s:
        s.key(CTRL_F9, delay=2.0)
        asked = _asked(s)
        box = pn._text(s)
        lines = kit.box_lines()
        s.key(ESC, delay=1.0)
        s._drain(1.0)
        pn._intact(s, "a folder's Executable for rustc")
    assert asked, "no question: %r" % kit.lines()
    assert any("Executable = evil" in l for l in lines), lines
    assert "./evil" in box, box
    assert not kit.starts("run"), "evil ran: %r" % kit.lines()


# @scenario SC-program-name-33c
@needs_sh
def test_a_folders_exedir_for_a_linked_program_still_asks_nothing(tmp_path):
    """Control: folder [Programming] Exedir : out, t.c, Ctrl-F9: no box, and
    the program the link wrote there runs.  The link writes that file in this
    very build."""
    kit = Kit(tmp_path)
    (kit.ws / "out").mkdir()
    kit.folder_rc("[Programming]\nExedir : out\n")
    with kit.session() as s:
        s.key(CTRL_F9, delay=2.0)
        assert pn._until(s, lambda: kit.starts("run"), 20), \
            "no run:\n%s\n%r" % (pn._text(s), kit.lines())
        box = BOX in pn._text(s)
        pn._intact(s, "a folder's Exedir")
    assert not box, "a question for a program this build wrote"
    assert kit.starts("run") == ["run [out/t.e]"], kit.lines()


# @scenario SC-program-name-34
@needs_sh
def test_a_folders_run_program_for_python_is_asked(tmp_path):
    """Folder [Language/Python] Run : program and a shipped `t`: Ctrl-F9
    asks, and Esc runs nothing."""
    kit = Kit(tmp_path)
    kit.interpreter("python3")
    kit.folder_rc("[Language/Python]\nRun : program\n")
    kit.ship("t")
    with kit.session(filename="t.py", seed="print(1)\n") as s:
        s.key(CTRL_F9, delay=2.0)
        asked = _asked(s)
        s.key(ESC, delay=1.0)
        s._drain(1.0)
        pn._intact(s, "a folder's Run : program")
    assert asked, "no question: %r" % kit.lines()
    assert not kit.starts("run"), kit.lines()


# ---- Rows 27, 30: what ran before still does ----

# @scenario SC-program-name-27
@needs_sh
def test_python_and_a68g_still_run_as_interpreters(tmp_path):
    """Controls: Python runs `python3 <source> A1` and Algol 68 (a68g)
    `a68g <source> A1`, the source as its absolute path."""
    for tool, filename, seed in (("python3", "t.py", "print(1)\n"),
                                 ("a68g", "t.a68", "# a #\nprint(1)\n")):
        sub = tmp_path / tool
        sub.mkdir()
        kit = Kit(sub)
        kit.interpreter(tool)
        kit.own_config(ARGS)
        with kit.session(filename=filename, seed=seed) as s:
            s.key(CTRL_F9, delay=2.0)
            src = str(kit.ws / filename)
            assert pn._until(s, lambda: "%s [%s] [A1]" % (tool, src) in kit.lines(), 20), \
                "%s: %r\n%s" % (tool, kit.lines(), pn._text(s))
            pn._intact(s, "Ctrl-F9 on " + filename)


# @scenario SC-program-name-30
@needs_sh
def test_a_users_own_interpreter_entry_keeps_running(tmp_path):
    """Own [Language/Lua] Compiler : lua, CompilerSwitch : 1, no Run: Ctrl-F9
    runs `lua <source>` as it did before the key existed."""
    kit = Kit(tmp_path)
    kit.interpreter("lua")
    kit.own_config(ARGS + LUA)
    with kit.session(filename="t.lua", seed="print(1)\n") as s:
        s.key(CTRL_F9, delay=2.0)
        src = str(kit.ws / "t.lua")
        assert pn._until(s, lambda: len(kit.starts("lua")) >= 2, 20), \
            "%r\n%s" % (kit.lines(), pn._text(s))
        pn._intact(s, "Ctrl-F9 on t.lua")
    assert kit.starts("lua")[-1] == "lua [%s] [A1]" % src, kit.lines()
    assert not kit.starts("run"), kit.lines()


# ---- Rows 31, 37: Algol 68 by toolchain, and a project's program ----

# @scenario SC-program-name-31
@needs_sh
@pytest.mark.skipif(not (tool_usable("ga68", "--version")),
                    reason="ga68 not usable")
def test_ga68_runs_the_program_it_linked(tmp_path):
    """Control, with the real ga68: a ga68 dialect file (a { } comment) builds
    ./t.e and Ctrl-F9 starts it, where a68g would have been handed the
    source.  The dialect picks the kind per file, as it picks the compiler."""
    kit = Kit(tmp_path)
    env = {"PATH": os.environ["PATH"]}
    src = "{ ga68 }\nbegin\n  puts (\"hello\\n\")\nend\n"
    with WpeSession(str(kit.ws), src, filename="t.a68", wait=2.0, cols=140, rows=40,
                    env_extra={"HOME": str(kit.home)}) as s:
        s.key(CTRL_F9, delay=3.0)
        assert pn._until(s, lambda: "Return-Code: 0" in pn._text(s), 60), pn._text(s)
        shown = pn._text(s)
        pn._intact(s, "Ctrl-F9 on a ga68 file")
    assert "hello" in shown, shown
    assert os.path.exists(str(kit.ws / "t.e")), os.listdir(str(kit.ws))


# @scenario SC-program-name-31b
@needs_sh
@pytest.mark.skipif(not (tool_usable("ga68", "--version") and tool_usable("a68g", "--version")),
                    reason="ga68 and a68g not both usable")
@pytest.mark.xfail(strict=True, reason="the per-file Algol 68 toolchain's values are judged "
                   "against the default toolchain's, so a folder with any xwperc is said to "
                   "have chosen them (Run = program, CompilerSwitch = 0); its owner is "
                   "the user's own copy, which needs a design")
def test_a_ga68_file_in_a_folder_with_an_xwperc_is_not_said_to_be_the_folders_choice(tmp_path):
    """Both toolchains installed (a68g is the default), a ga68 dialect file,
    and a folder xwperc that sets only a default value: Run's box must not say
    the folder chose the program, since the editor's dialect check did."""
    kit = Kit(tmp_path)
    kit.folder_rc("[Language/C]\nCompilerOptions : -g\n")
    src = "{ ga68 }\nbegin\n  puts (\"hello\\n\")\nend\n"
    with kit.session(filename="t.a68", seed=src, env={"PATH": os.environ["PATH"]}) as s:
        s.key(CTRL_F9, delay=3.0)
        for _ in range(2):
            if not _asked(s):
                break
            s.key(RUN_IT, delay=3.0)
            s._drain(2.0)
        pn._until(s, lambda: "Return-Code" in pn._text(s), 30)
        pn._intact(s, "a ga68 file in a folder with an xwperc")
    assert not any("Program chosen by" in l for l in kit.box_lines()), kit.box_lines()


# @scenario SC-program-name-37
@needs_sh
def test_a_projects_run_starts_its_program_whatever_cmpswtch_says(tmp_path):
    """Project p.prj with CMPSWTCH=other, F9 then Ctrl-F9: Run starts ./a.out
    once.  It started the compiler again on the file, as an interpreter
    (the build of such a project writes no program of its own, so the
    program here is one the folder holds)."""
    kit = Kit(tmp_path)
    prj = pn._project(kit, None)
    prj.write_text(prj.read_text().replace("CMPSWTCH=\tgnu", "CMPSWTCH=\tother"))
    kit.ship("a.out")
    with kit.session() as s:
        pn._f9_project(s)
        pn._until(s, lambda: kit.compiles(), 20)
        s._drain(2.0)
        s.key(CTRL_F9, delay=2.0)
        # The style links nothing, so the program is the folder's: Run asks
        # (row 37b); answered Run it, it starts that program once.
        assert _asked(s), "no question for a program the build never wrote:\n" + pn._text(s)
        s.key(RUN_IT, delay=1.5)
        assert pn._until(s, lambda: kit.starts("run"), 20), \
            "%r\n%s" % (kit.lines(), pn._text(s))
        pn._intact(s, "Run of a project with CMPSWTCH=other")
    assert kit.starts("run") == ["run [./a.out]"], kit.lines()
    assert not [l for l in kit.starts("gcc") if " -c " not in l], \
        "the compiler ran as an interpreter: %r" % kit.lines()


# @scenario SC-program-name-37b
@needs_sh
def test_a_folders_project_of_style_other_never_runs_a_shipped_program_unasked(tmp_path):
    """The folder's xwperc opens p.prj with CMPSWTCH=other, and the folder
    ships a.out: F9 compiles and links nothing (that style writes its own
    program), so Ctrl-F9 would start the shipped a.out.  Run asks, names
    CMPSWTCH = other, and Esc starts nothing."""
    kit = Kit(tmp_path)
    prj = pn._project(kit, None)
    prj.write_text(prj.read_text().replace("CMPSWTCH=\tgnu", "CMPSWTCH=\tother"))
    kit.ship("a.out")
    with kit.session() as s:
        pn._f9_project(s)
        assert pn._until(s, lambda: kit.compiles(), 20), kit.lines()
        s._drain(2.0)
        s.key(CTRL_F9, delay=2.0)
        asked = _asked(s)
        lines = kit.box_lines()
        if asked:
            s.key(ESC, delay=1.0)
        s._drain(1.5)
        pn._intact(s, "Run of a project with CMPSWTCH=other")
    assert asked, "no question: %r" % kit.lines()
    assert any("CMPSWTCH = other" in l for l in lines), lines
    assert not kit.starts("run"), "the shipped a.out ran: %r" % kit.lines()


# ---- Row 32: Options > Save keeps the Run key ----

# @scenario SC-program-name-32
@needs_sh
def test_options_save_keeps_a_run_key_and_the_next_session_uses_it(tmp_path):
    """Own [Language/Python] Run : program: Options > Save writes the line
    back, and the next session's Ctrl-F9 starts ./t."""
    kit = Kit(tmp_path)
    kit.interpreter("python3")
    kit.own_config("[Language/Python]\nRun : program\n")
    own = kit.home / ".config" / "xwpe" / "xwperc"
    with kit.session(filename="t.py", seed="print(1)\n") as s:
        s.key(ALT_O)
        s.key("s", delay=0.8)
        s.key("\r", delay=1.5)
        pn._intact(s, "Options > Save")
    saved = own.read_text()
    section = saved.split("[Language/Python]", 1)[1].split("[", 1)[0]
    assert "Run : program" in section, saved
    kit.ship("t")
    with kit.session(filename="t.py", seed="print(1)\n") as s:
        s.key(CTRL_F9, delay=2.0)
        assert pn._until(s, lambda: kit.starts("run"), 20), "%r\n%s" % (kit.lines(), pn._text(s))
    assert kit.starts("run") == ["run [./t]"], kit.lines()


# @scenario SC-program-name-32b
@needs_sh
def test_a_folder_file_that_repeats_his_own_run_asks_nothing(tmp_path):
    """His own [Language/Python] Run : program, and a folder xwperc that says
    the same: his own Run line is his in that folder too, so Ctrl-F9 asks
    nothing and starts ./t, as it does without the folder's file."""
    kit = Kit(tmp_path)
    kit.interpreter("python3")
    kit.own_config("[Language/Python]\nRun : program\n")
    kit.folder_rc("[Language/Python]\nRun : program\n")
    kit.ship("t")
    with kit.session(filename="t.py", seed="print(1)\n") as s:
        s.key(CTRL_F9, delay=2.0)
        pn._until(s, lambda: kit.starts("run") or BOX in pn._text(s), 20)
        asked = BOX in pn._text(s)
        if asked:
            s.key(ESC, delay=1.0)
        pn._intact(s, "a folder that repeats his own Run")
    assert not asked, "a question about his own Run line: %r" % kit.box_lines()
    assert kit.starts("run") == ["run [./t]"], kit.lines()


# ---- Rows 39, 40, 41: the kind an entry has when a file names none ----

SWITCHES = [("C", 0), ("C++", 0), ("Fortran", 0), ("Pascal", 1), ("Java", 1), ("Python", 1),
            ("LaTeX", 1), ("Perl", 1), ("COBOL", 0), ("Go", 1), ("Rust", 1), ("Scala", 1)]


def _saved_style_file():
    """Every language with its CompilerSwitch and no Run, as Options > Save
    wrote it before the key existed."""
    return "".join("[Language/%s]\nCompilerSwitch : %d\n" % (n, v) for n, v in SWITCHES)


# @scenario SC-program-name-39
@needs_sh
@pytest.mark.parametrize("filename, seed, compiler, writes, expect", [
    ("t.rs", "fn main() {}\n", "rustc", "program", ["run [./t]"]),
    ("T.java", SEED_J, "javac", "class", None),
    ("t.tex", "\\documentclass{article}\n", "pdflatex", "none", []),
], ids=["rust", "java", "latex"])
def test_a_saved_xwperc_keeps_the_built_in_kinds(tmp_path, filename, seed, compiler, writes,
                                                 expect):
    """An xwperc as Options > Save wrote it (every language, CompilerSwitch,
    no Run): Rust still runs its program, Java its class and LaTeX nothing.
    Reading the switch must not turn them into interpreters."""
    kit = Kit(tmp_path)
    if compiler == "pdflatex":
        kit.interpreter(compiler)
    else:
        kit.compiler(compiler, writes, beside=(compiler == "javac"))
    if compiler == "javac":
        kit.vm("java")
    kit.own_config(_saved_style_file())
    with kit.session(filename=filename, seed=seed) as s:
        s.key(CTRL_F9, delay=2.0)
        if compiler == "javac":
            assert pn._until(s, lambda: kit.starts("java"), 20), kit.lines()
        elif compiler == "pdflatex":
            assert pn._until(s, lambda: "makes no program" in pn._text(s), 20), pn._text(s)
        else:
            assert pn._until(s, lambda: kit.starts("run"), 20), kit.lines()
        pn._intact(s, "Ctrl-F9 on " + filename)
    assert len(kit.starts(compiler)) == 1, kit.lines()
    if expect is not None:
        assert kit.starts("run") == expect, kit.lines()
    else:
        assert kit.starts("java") == ["java [T]"], kit.lines()


# @scenario SC-program-name-40
@needs_sh
def test_a_folder_file_that_repeats_the_defaults_asks_nothing(tmp_path):
    """A folder's xwperc with every language as Options > Save wrote it:
    Ctrl-F9 on t.rs starts the program and asks nothing, since each value is
    the built-in one."""
    kit = Kit(tmp_path)
    kit.compiler("rustc", "program")
    kit.folder_rc(_saved_style_file())
    with kit.session(filename="t.rs", seed="fn main() {}\n") as s:
        s.key(CTRL_F9, delay=2.0)
        assert pn._until(s, lambda: kit.starts("run"), 20), \
            "%r\n%s" % (kit.lines(), pn._text(s))
        box = BOX in pn._text(s)
        pn._intact(s, "a folder that repeats the defaults")
    assert not box, "a question about built-in values"
    assert kit.starts("run") == ["run [./t]"], kit.lines()


# A language a user adds, as Options > Save writes it: with its message
# string (an entry without one is a message parser's input it cannot read).
LUA = ("[Language/Lua]\nCompiler : lua\nFileExtension : .lua\n"
       "MessageString : ${FILE}:${LINE}:*\nCompilerSwitch : 1\n")


# @scenario SC-program-name-41
@needs_sh
def test_use_mine_keeps_the_users_interpreter(tmp_path):
    """Folder Arguments : A1, the same own and folder [Language/Lua] with
    CompilerSwitch : 1 and no Run, and a shipped `t`: the box asks about the
    Arguments, and Use mine runs `lua <source>`, never ./t."""
    kit = Kit(tmp_path)
    kit.interpreter("lua")
    kit.own_config(LUA)
    kit.folder_rc(ARGS + LUA)
    kit.ship("t")
    with kit.session(filename="t.lua", seed="print(1)\n") as s:
        s.key(CTRL_F9, delay=2.0)
        assert _asked(s), "no question:\n" + pn._text(s)
        s.key(USE_MINE, delay=1.5)
        assert pn._until(s, lambda: len(kit.starts("lua")) >= 2, 15), \
            "%r\n%s" % (kit.lines(), pn._text(s))
        pn._intact(s, "Use mine for a user's interpreter")
    src = str(kit.ws / "t.lua")
    assert kit.starts("lua")[-1] == "lua [%s]" % src, kit.lines()
    assert not kit.starts("run"), "the shipped program ran: %r" % kit.lines()


# A language only a folder's xwperc makes (he has no entry of his for it).
EVIL = ("[Language/Evil]\nCompiler : evilc\nFileExtension : .ev\n"
        "MessageString : ${FILE}:${LINE}:*\nCompilerSwitch : 1\n")


def _folder_language_run(kit, second_key):
    """Ctrl-F9 on t.ev: Run it for the folder's compiler, then @second_key on
    Run's own box.  Returns that box's text."""
    with kit.session(filename="t.ev", seed="x\n") as s:
        s.key(CTRL_F9, delay=2.0)
        assert _asked(s), "the folder's compiler was not asked:\n" + pn._text(s)
        s.key(RUN_IT, delay=2.0)
        assert pn._until(s, lambda: kit.starts("evilc"), 20), kit.lines()
        assert wait_screen(s, RUN_BOX, tmo=ASK_WAIT), "Run was not asked:\n" + pn._text(s)
        box = pn._text(s)
        s.key(second_key, delay=2.0)
        s._drain(1.5)
        s.key(ESC, delay=1.0)
        pn._intact(s, "Run of a language only the folder makes")
    return box


# @scenario SC-program-name-41b
@needs_sh
def test_a_folder_only_interpreter_offers_no_use_mine(tmp_path):
    """A language only the folder makes, an interpreter (CompilerSwitch : 1,
    no Run): Run's box offers no Use mine, since he has no interpreter of his
    own for it, and the compiler runs once (F9's step)."""
    kit = Kit(tmp_path)
    kit.interpreter("evilc")
    kit.own_config("[Programming]\nArguments : \n")
    kit.folder_rc(EVIL)
    box = _folder_language_run(kit, USE_MINE)
    assert "Use mine" not in box, box
    assert len(kit.starts("evilc")) == 1, kit.lines()


# @scenario SC-program-name-41c
@needs_sh
def test_use_mine_never_starts_a_program_a_folder_only_language_chose(tmp_path):
    """A language only the folder makes, with Run : program and a compiler
    that writes its own program, and a shipped `t`: Run asks, and Use mine is
    not offered, since the kind and the style that name ./t are the folder's,
    not his.  Pressing it starts nothing."""
    kit = Kit(tmp_path)
    kit.interpreter("evilc")
    kit.own_config("[Programming]\nArguments : \n")
    kit.folder_rc(EVIL + "Run : program\n")
    kit.ship("t")
    box = _folder_language_run(kit, USE_MINE)
    assert "Use mine" not in box, box
    assert not kit.starts("run"), "the shipped program ran as his: %r" % kit.lines()


# ---- Rows 38, 42: the VM's shell line and a class java would read as an option ----

# @scenario SC-program-name-38
@needs_sh
def test_the_vm_line_takes_the_users_own_shell_text(tmp_path):
    """T.java, own Arguments `< in.txt` and in.txt holding X: the VM reads X
    on its input, so the java and the class are the line's first two words and
    his text follows."""
    kit = _vm_kit(tmp_path, "javac", "java")
    (kit.ws / "in.txt").write_text("X\n")
    kit.own_config("[Programming]\nArguments : < in.txt\n")
    with kit.session(filename="T.java", seed=SEED_J, env={"STANDIN_STDIN": "1"}) as s:
        s.key(CTRL_F9, delay=2.0)
        assert pn._until(s, lambda: "stdin [X]" in kit.lines(), 20), \
            "%r\n%s" % (kit.lines(), pn._text(s))
        pn._intact(s, "the VM line with his own text")
    assert kit.starts("java") == ["java [T]"], kit.lines()


# @scenario SC-program-name-42
@needs_sh
@pytest.mark.parametrize("stem", ["-x", "@x"], ids=["option", "argfile"])
def test_a_class_named_like_an_option_is_refused_and_said(tmp_path, stem):
    """-x.java or @x.java, Ctrl-F9: java never starts, and the status row says
    why (java reads -x as an option and @x as a file of options)."""
    kit = _vm_kit(tmp_path, "javac", "java")
    name = "./%s.java" % stem   # "-x.java" alone is an option of the editor
    with kit.session(filename=name, seed=SEED_J) as s:
        s.key(CTRL_F9, delay=2.0)
        assert pn._until(s, lambda: "read by java as an option" in pn._text(s), 20), \
            "%r\n%s" % (kit.lines(), pn._text(s))
        pn._intact(s, "Ctrl-F9 on " + name)
    assert len(kit.starts("javac")) == 1, kit.lines()
    assert not kit.starts("java"), "java started: %r" % kit.lines()


# @scenario SC-program-name-42b
@needs_sh
@pytest.mark.parametrize("stem", ["-Dx", "@x"], ids=["option", "argfile"])
def test_a_scala_object_named_like_an_option_is_refused_and_said(tmp_path, stem):
    """-Dx.scala or @x.scala, Ctrl-F9: the scala runner never starts, and the
    status row names scala as the tool that would read the class as an
    option (it hands -D to the VM and reads @x as a file of options)."""
    kit = _vm_kit(tmp_path, "scalac", "scala")
    name = "./%s.scala" % stem
    with kit.session(filename=name, seed="object T { def main(a: Array[String]) = () }\n") as s:
        s.key(CTRL_F9, delay=2.0)
        assert pn._until(s, lambda: "read by scala as an option" in pn._text(s), 20), \
            "%r\n%s" % (kit.lines(), pn._text(s))
        pn._intact(s, "Ctrl-F9 on " + name)
    assert len(kit.starts("scalac")) == 1, kit.lines()
    assert not kit.starts("scala"), "scala started: %r" % kit.lines()


# ---- Row 43: a shipped class is built again, then trusted ----

# @scenario SC-program-name-43
@needs_sh
def test_a_shipped_class_is_compiled_and_then_run(tmp_path):
    """T.java with a shipped T.class dated after the source: Ctrl-F9 starts
    javac (the editor built no class here), then java runs T; the second
    Ctrl-F9 starts no javac and runs java again."""
    kit = _vm_kit(tmp_path, "javac", "java")
    kit.ship("T.class", text="")
    later = time.time() + pn.SHIP_AHEAD
    os.utime(str(kit.ws / "T.class"), (later, later))
    with kit.session(filename="T.java", seed=SEED_J) as s:
        s.key(CTRL_F9, delay=2.0)
        assert pn._until(s, lambda: kit.starts("java"), 20), \
            "%r\n%s" % (kit.lines(), pn._text(s))
        s.key(CTRL_F9, delay=2.0)
        assert pn._until(s, lambda: len(kit.starts("java")) >= 2, 20), \
            "%r\n%s" % (kit.lines(), pn._text(s))
        pn._intact(s, "a shipped class")
    assert len(kit.starts("javac")) == 1, kit.lines()
    assert kit.starts("java") == ["java [T]", "java [T]"], kit.lines()


# ---- Rows 35, 36: Run and the Java debugger share one VM start (real JDK) ----

real_jdk = pytest.mark.skipif(
    not (tool_usable("javac", "-version") and tool_usable("java", "-version")
         and tool_usable("jdb", "-version")),
    reason="a JDK with javac, java and jdb is not usable")

NO_CLASS = "Could not find or load main class T"


def _real_kit(tmp_path):
    kit = Kit(tmp_path)
    return kit


# @scenario SC-program-name-35
@needs_sh
@real_jdk
def test_run_and_the_debugger_share_the_class_path_rule(tmp_path):
    """T.java with a CLASSPATH that holds no `.`, Ctrl-F9: the VM starts in
    the build folder with no -cp of its own, honours CLASSPATH, and reports
    the class not found, as the Java debugger's VM does.  Both report it."""
    kit = _real_kit(tmp_path)
    jar = str(tmp_path / "none.jar")
    with kit.session(filename="T.java", seed=SEED_J, env={"CLASSPATH": jar}) as s:
        s.key(CTRL_F9, delay=3.0)
        assert pn._until(s, lambda: NO_CLASS in pn._text(s), 40), pn._text(s)
        pn._intact(s, "Run with a CLASSPATH without .")


# @scenario SC-program-name-36
@needs_sh
@real_jdk
def test_the_class_a_source_makes_is_its_stem_for_run_as_for_the_debugger(tmp_path):
    """Finding F5 pinned: p/T.java with `package p;`, Ctrl-F9: Run starts
    class T in the folder p and the VM refuses it ("wrong name: p/T"), as the
    Java debugger's VM does today.  A later fix moves both."""
    kit = _real_kit(tmp_path)
    (kit.ws / "p").mkdir()
    with kit.session(filename="p/T.java", seed="package p;\n" + SEED_J) as s:
        s.key(CTRL_F9, delay=3.0)
        assert pn._until(s, lambda: "wrong name: p/T" in _flat_gap(s), 40), pn._text(s)
        pn._intact(s, "Run of a class in a package")


def _flat_gap(s):
    return " ".join(pn._text(s).split())
