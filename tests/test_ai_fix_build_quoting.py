"""Alt-G B on a file with no makefile: the file's name reaches /bin/sh as one word.

With no makefile, Build & fix checks the current C file with
`gcc -fsyntax-only -Wall <file>`, shows that line in the pane and hands it to
the agent.  The agent runs it through /bin/sh -c, so a file called
"a'; touch PWNED; '.c" must not end its quote and run a second command.

Each row opens such a file, starts Build & fix on the mock backend (the agent
answers DONE at once, so only the editor's own command is judged), reads the
command from the trace and runs it here under a stand-in `gcc` that records
what it was given.  Asserts: the shell saw the one file, nothing else ran
(no PWNED), and the command is the quoted one.

Red on the unfixed tree: only the three names with a single quote (the two
injections and it's.c).  The newline, backquote and ok.c rows are controls:
they pass before and after.  A path too long for the command is refused with
a pane line and nothing runs.
"""
import os
import stat
import subprocess
import time
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

CMD_TAG = "fix-build cmd="

NAMES = [
    "a'; touch PWNED; '.c",
    "a'$(touch PWNED)'.c",
    "it's.c",
    "a\nb.c",
    "a`touch PWNED`.c",
    "ok.c",
]

FAKE_GCC = ("#!/bin/sh\n"
            "# stand-in compiler: one line per argument it was given\n"
            "for a in \"$@\"; do printf '%s\\n' \"$a\" >> \"$GCC_ARGS\"; done\n")


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _always_quoted(s):
    """One single-quoted word, an embedded quote written '\\'' (even for a safe name)."""
    return "'" + s.replace("'", "'\\''") + "'"


def _command_from(trace_text):
    """The command the editor traced: from the tag to the first `.c'` that ends a line."""
    start = trace_text.index(CMD_TAG) + len(CMD_TAG)
    end = trace_text.index(".c'\n", start) + len(".c'")
    return trace_text[start:end]


def _start_fix_build(workdir, name):
    trace = os.path.join(workdir, "b.trace")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "auto",
           "XWPE_AI_MOCK_REPLY": "DONE nothing to do", "XWPE_AI_TRACE": trace,
           "XDG_STATE_HOME": os.path.join(workdir, "state")}
    with WpeSession(workdir, "int main(void){return 0;}\n",
                    env_extra=env, filename=name) as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("b"); s._drain(0.5)
        end = time.time() + 15
        while time.time() < end:
            s._drain(0.5)
            t = open(trace).read() if os.path.exists(trace) else ""
            if CMD_TAG in t and ".c'\n" in t[t.index(CMD_TAG):]:
                break
        s.key("a", delay=0.4); s.key("\033", delay=0.3)
        s._drain(0.3)
    return open(trace).read()


def _run_under_fake_gcc(cmd, workdir):
    """Run `cmd` through /bin/sh -c as the agent would; return (argv lines, files made)."""
    run = os.path.join(workdir, "run")
    bindir = os.path.join(workdir, "fakebin")
    os.makedirs(run)
    os.makedirs(bindir)
    gcc = os.path.join(bindir, "gcc")
    with open(gcc, "w") as fh:
        fh.write(FAKE_GCC)
    os.chmod(gcc, os.stat(gcc).st_mode | stat.S_IXUSR)
    args = os.path.join(workdir, "gcc.args")
    env = {"PATH": bindir + ":/usr/bin:/bin", "GCC_ARGS": args}
    subprocess.run(["/bin/sh", "-c", cmd], cwd=run, env=env, timeout=20,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    lines = open(args).read().split("\n")[:-1] if os.path.exists(args) else []
    return lines, os.listdir(run)


# @scenario SC-ai-runtime-18
@pytest.mark.parametrize("name", NAMES, ids=["quote-semicolon", "quote-subst", "apostrophe",
                                             "newline", "backquote", "plain"])
def test_fix_build_command_quotes_the_file_name(tmp_path, name):
    workdir = str(tmp_path)
    trace = _start_fix_build(workdir, name)
    assert CMD_TAG in trace, "Build & fix did not start:\n" + trace
    cmd = _command_from(trace)
    full = os.path.join(workdir, name)
    assert cmd == "gcc -fsyntax-only -Wall " + _always_quoted(full), \
        "the command shown is not the quoted one: %r" % cmd
    argv, made = _run_under_fake_gcc(cmd, workdir)
    assert "PWNED" not in made and not os.path.exists(os.path.join(workdir, "PWNED")), \
        "the file name ran a command of its own"
    assert "\n".join(argv) == "-fsyntax-only\n-Wall\n" + full, \
        "the shell did not hand the compiler the one file: %r" % argv


# @scenario SC-ai-runtime-18
def test_too_long_path_runs_nothing(tmp_path):
    """A path that does not fit in the command is refused, never cut."""
    deep = str(tmp_path)
    while len(deep) < 1100:
        deep = os.path.join(deep, "d" * 60)
    os.makedirs(deep)
    trace_path = os.path.join(deep, "b.trace")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "auto",
           "XWPE_AI_MOCK_REPLY": "DONE nothing to do", "XWPE_AI_TRACE": trace_path,
           "XDG_STATE_HOME": os.path.join(str(tmp_path), "state")}
    with WpeSession(deep, "int main(void){return 0;}\n", env_extra=env,
                    filename="x.c") as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("b"); s._drain(2.0)
        screen = "\n".join(s.display())
        s.key("\033", delay=0.3)
    t = open(trace_path).read() if os.path.exists(trace_path) else ""
    assert "fix-build path too long" in t, t
    assert CMD_TAG not in t, "a command was built for a path that does not fit:\n" + t
    assert "too long" in screen, screen
