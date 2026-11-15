"""The debugger side of the editor holds a path whole, and looks for a program
on a PATH it does not control without running past a buffer.

Rows here were red because a path was cut or copied into a fixed array:

- the compile for a debug session (Ctrl-G R on a Rust file) told the compiler
  the first 1023 bytes of the binary's and the source's path when the folder
  was deeper than that;
- the lookup of a program on PATH (a debugger, a compiler, a language server)
  copied each PATH entry into 256 bytes and appended the program's name with
  strcat, so a PATH entry of 256 bytes or more, or a compiler name of about
  that length from a folder's option file, wrote past the array when a file
  was opened or a debug session was asked for.

Each row asserts the effect (the paths the compiler was given, the editor
alive, no sanitizer report), so it is red on the commit before the lookup
and the compile went through the path owner.

Run: WPE_BIN=$PWD/wpe pytest tests/test_path_debugger.py, and with
WPE_BIN=$PWD/wpe-asan ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 (the
binary must carry a wpe-prefixed name) for the sanitizer build, where the
stack overflows are visible.
"""
import os
import stat
import time

import pytest

import deep_folder
from wpe_driver import WpeSession, WAIT_SCALE
from test_find_regex_owner import _grep
from test_legacy_debuggers import _screen, _wait_for
from test_signal_save import DAP_RUST_PROG, never_stopping_adapter_env

CTRL_G = "\x07"            # the debugger's prefix
RUN_IT = "\033r"           # Alt-R: Run it, in the box a folder's compiler asks
SANITIZER = "AddressSanitizer"
ASAN_ENV = {"ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1"}
DEEP_FOLDER = 1100         # over the 1023 bytes the compile for debug kept
LONG_ENTRY = 300           # a PATH entry over the 255 bytes the lookup held
LONG_COMMAND = 300         # a compiler name over what the lookup held
RECORD_S = 15              # time for the stand-in compiler to be started
ARGS_FILE = "rustc.args"

# A compiler that records every argument it is given, one per line, and
# writes the empty binary the -o option names, so no toolchain is needed.
RECORDING_RUSTC = ("#!/bin/sh\n"
                   "for a in \"$@\"; do printf '%%s\\n' \"$a\" >> %(args)s; done\n"
                   "while [ $# -gt 0 ]; do\n"
                   "  if [ \"$1\" = -o ]; then : > \"$2\"; fi\n"
                   "  shift\n"
                   "done\n")


def _raw(w):
    return bytes(w.raw).decode("utf-8", "replace")


def _intact(w, what):
    """The editor is alive and no sanitizer report was printed."""
    assert w.alive(), "the editor died: %s\n%s" % (what, _screen(w))
    assert SANITIZER not in _raw(w), "sanitizer report: %s\n%s" % (what, _raw(w)[-1500:])


def _recording_env(tmp_path, front):
    """gdb as the adapter and a recording rustc first on PATH, @front before
    them (PATH entries the editor cannot make sense of)."""
    env = never_stopping_adapter_env(tmp_path)
    rustc = tmp_path / "bin" / "rustc"
    rustc.write_text(RECORDING_RUSTC % {"args": str(tmp_path / ARGS_FILE)})
    rustc.chmod(rustc.stat().st_mode | stat.S_IXUSR)
    env["PATH"] = front + env["PATH"]
    env.update(ASAN_ENV)
    return env


def _recorded_arguments(tmp_path):
    path = tmp_path / ARGS_FILE
    if not path.exists():
        return []
    return path.read_text().splitlines()


def _compiler_started(w, tmp_path):
    """Wait until the stand-in compiler has recorded its arguments, or the
    editor is gone; the caller says which it was."""
    return _wait_for(w, lambda: len(_recorded_arguments(tmp_path)) >= 4 or not w.alive(),
                     RECORD_S)


def _path_prefix(kind, base):
    """What goes in front of PATH: nothing, an entry over the old buffer, or
    one over what the system holds under @base."""
    if kind == "none":
        return ""
    if kind == "long":
        return "/" + "a" * LONG_ENTRY + ":"
    return "/" + "a" * (deep_folder.over_the_limit(base) - 1) + ":"


# Ctrl-G R on a Rust file in a folder deeper than 1023 bytes.
# @scenario SC-path-debug-01
def test_compile_for_debug_in_a_deep_folder_gives_the_compiler_whole_paths(tmp_path):
    """The compiler is started with the whole binary path, the whole source
    path and an error file path, each the folder's own and 1100 bytes deep.
    The three were kept in 1024 bytes each, so the compiler was told a cut
    path and the binary was written nowhere the session looked.  A system
    that holds no path of that length cannot have the cut and the row skips."""
    deep_folder.held(tmp_path, DEEP_FOLDER)
    folder = deep_folder.folder_of_length(tmp_path / "work", DEEP_FOLDER)
    env = _recording_env(tmp_path, "")
    with WpeSession(folder, DAP_RUST_PROG, filename="main.rs", wait=1.5,
                    env_extra=env) as w:
        w.key(CTRL_G, "r", delay=0.2)
        _compiler_started(w, tmp_path)
        _intact(w, "Ctrl-G R in a %d byte folder" % DEEP_FOLDER)
        assert len(_recorded_arguments(tmp_path)) >= 4, \
            "the compiler was not started:\n%s" % _screen(w)
    args = _recorded_arguments(tmp_path)
    binary = args[args.index("-o") + 1]
    source = args[-1]
    assert binary == os.path.join(folder, "main.dbg"), \
        "the binary named to the compiler is not whole (%d bytes): %r" % (len(binary), binary)
    assert source == os.path.join(folder, "main.rs"), \
        "the source named to the compiler is not whole (%d bytes): %r" % (len(source), source)
    assert len(binary) > 1023


# A PATH with an entry the old lookup could not hold, then the real compiler.
# @scenario SC-path-debug-02
@pytest.mark.parametrize("kind", ["long", "huge"])
def test_an_unusable_path_entry_is_skipped_and_the_compiler_still_found(tmp_path, kind):
    """A PATH whose first entry is 300 bytes (a folder a system can hold but
    the lookup's array could not) or one over the system's limit (which no
    system can hold) does not end
    the editor when a Rust debug session looks for its compiler and adapter:
    the entry is skipped, the next one finds them and the compiler runs.
    The lookup copied each entry into 256 bytes and wrote past them."""
    env = _recording_env(tmp_path, _path_prefix(kind, tmp_path))
    with WpeSession(str(tmp_path), DAP_RUST_PROG, filename="main.rs", wait=1.5,
                    env_extra=env) as w:
        _intact(w, "start with a %s PATH entry" % kind)
        w.key(CTRL_G, "r", delay=0.2)
        _compiler_started(w, tmp_path)
        _intact(w, "Ctrl-G R with a %s PATH entry" % kind)
        assert len(_recorded_arguments(tmp_path)) >= 4, \
            "the compiler was not found:\n%s" % _screen(w)


# A PATH entry longer than the old buffer, with no debug session at all.
# @scenario SC-path-debug-03
@pytest.mark.parametrize("kind", ["long", "huge"])
def test_a_long_path_entry_does_not_end_the_editor_at_start(tmp_path, kind):
    """Opening a C file starts the language-server probe, which looks the
    server up on PATH.  A 300 byte entry wrote past the lookup's 256 byte
    array and ended the editor before the first key, so a PATH set by a
    wrapper script (or a hostile environment) made the editor unusable."""
    env = {"PATH": _path_prefix(kind, tmp_path) + os.environ.get("PATH", "")}
    env.update(ASAN_ENV)
    with WpeSession(str(tmp_path), "int main(void) { return 0; }\n", filename="t.c",
                    wait=2.0, env_extra=env) as w:
        _intact(w, "start with a %s PATH entry" % kind)
        w.key("\033", delay=0.3)
        _intact(w, "first key with a %s PATH entry" % kind)


# A compiler named by a folder's option file, longer than the old buffer.
# @scenario SC-path-debug-04
def test_a_compiler_name_longer_than_the_lookup_buffer_is_refused_not_overrun(tmp_path):
    """A folder's xwperc names the compiler of Rust with 300 bytes.  Ctrl-G R
    looks it up on PATH: the name is appended to each PATH entry with strcat
    in 256 bytes.  The editor says the compiler is not in PATH and stays
    alive, with no session started."""
    (tmp_path / "xwperc").write_text("[Language/Rust]\nCompiler : %s\n" % ("z" * LONG_COMMAND))
    env = _recording_env(tmp_path, "")
    with WpeSession(str(tmp_path), DAP_RUST_PROG, filename="main.rs", wait=1.5,
                    env_extra=env) as w:
        w.key(CTRL_G, "r", delay=1.0)
        w.key(RUN_IT, delay=1.0)           # the box a folder's compiler asks
        w._drain(2.0)
        _intact(w, "Ctrl-G R with a %d byte compiler name" % LONG_COMMAND)
        screen = _screen(w)
        assert "Compiler '" in screen or "not in PATH" in screen or "z" * 20 in screen, \
            "the editor should say the compiler cannot be found:\n" + screen
    assert _recorded_arguments(tmp_path) == [], "no compiler should have been started"


# Grep reads a file's lines whole.
# @scenario SC-path-debug-06
@pytest.mark.parametrize("seed,text,opts,listed", [
    ("a" * 252 + "needle" + "b" * 150 + "\n", "needle", [], True),
    ("a" * 255 + "needle" + "b" * 150 + "\n", "^needle", ["\033r"], False),
    ("b" * 250 + "x\n" + "a" * 255 + "needle\n", "^needle", ["\033r"], False),
    ("short line\n" + "n" * 300 + "needle\n", "needle$", ["\033r"], True),
], ids=["spanning-the-old-piece", "anchor-inside-a-line", "anchor-after-a-line",
        "end-of-a-long-line"])
def test_grep_reads_a_long_line_whole(tmp_path, seed, text, opts, listed):
    """File > Grep on a file with a line of 400 bytes.  The line was read in
    pieces of 255 bytes and each piece searched as a line of its own, so a
    match across two pieces was missed and ^ and $ matched inside the line."""
    screen, found = _grep(tmp_path, seed, text, *opts)
    assert found == listed, screen


# Project-Options saves a project value its field could not hold, untouched.
# @scenario SC-path-debug-05
@pytest.mark.parametrize("variable,size", [("EXENAME", 4200), ("LIBNAME", 4200),
                                           ("EXENAME", 4096), ("CMPFLAGS", 300),
                                           ("LDFLAGS", 300)])
def test_project_options_save_keeps_a_value_longer_than_its_field(tmp_path, variable, size):
    """A .prj whose EXENAME, LIBNAME, CMPFLAGS or LDFLAGS is longer than the
    field that shows it: Project-Options, Save writes the value back whole.
    The field cut it to its capacity (4095 bytes for a path, 256 for options)
    and Save wrote the cut over the user's file."""
    if variable in ("EXENAME", "LIBNAME"):
        value = "/" + "v" * (size - 1)
    else:
        value = "-" + "f" * (size - 1)
    lines = {"CMP": "gcc", "CMPFLAGS": "-g", "LDFLAGS": "", "EXENAME": "p", "LIBNAME": "lib"}
    lines[variable] = value
    prj = tmp_path / "p.prj"
    body = "".join("%s=\t%s\n" % (k, lines[k])
                   for k in ("CMP", "CMPFLAGS", "LDFLAGS", "EXENAME", "LIBNAME"))
    prj.write_text("#\n# xwpe - project-file: p.prj\n" + body + "CMPSWTCH=\tgnu\n\nFILES=\tt.c \n")
    (tmp_path / "xwperc").write_text("[Programming]\nProject : p.prj\n")
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n", filename="t.c",
                    wait=2.0, cols=120, rows=40, env_extra=ASAN_ENV) as w:
        w.key("\033p", delay=1.0)
        w.key("o", delay=1.5)
        w.key("\033s", delay=1.5)
        _intact(w, "Project-Options Save with a %d byte %s" % (size, variable))
    saved = {}
    for line in prj.read_text().splitlines():
        if "=\t" in line:
            name, text = line.split("=\t", 1)
            saved[name] = text.strip()
    assert saved.get(variable) == value, \
        "%s on disk is %d bytes, not %d" % (variable, len(saved.get(variable, "")), size)
