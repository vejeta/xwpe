"""What a command the assistant starts is given, and what it is not.

A model's command used to inherit the editor's whole environment: the
provider keys, the agent sockets, the loader's variables.  Now every
assistant child is started through its containment profile, which builds the
environment from a list, sets a file-size limit and the no-new-privileges
flag, and refuses to start when the user asked for Strict and this system
cannot hold the files and the network yet (it cannot, so Strict refuses).

Each row sets its own markers in the editor's environment and reads what the
command wrote on disk.  The engines (the `claude` backend and a hosted
command) keep their own login variables; a program the workspace planted is
never the engine that runs.

Stand-ins only: the mock backend for the agent, a fake `claude` that records
its environment, and a fake hosted engine.  The editor's HOME is the
workspace in every pyte session, which is why the rows here read only what
the commands wrote, never what is under HOME.
"""
import os
import re
import subprocess
import textwrap
import time

import pytest
from host_tools import needs
from wpe_driver import WpeSession, ALT, WAIT_SCALE, WPE_BIN, assistant_build, wait_screen, \
    pane_text_with_history

pytestmark = pytest.mark.skipif(not assistant_build(),
                                reason="wpe built without --enable-ai")

SEED = "int main(void){return 0;}\n"
MARKER = "xwpe-contain-marker-4c1e"
LIMIT_S = 10 * WAIT_SCALE
# `ulimit -f` counts 512-byte blocks in a POSIX shell and 1024-byte ones in
# others; the documented limit is 8 GiB either way.
FSIZE_BLOCKS = {8 * 1024 ** 3 // 512, 8 * 1024 ** 3 // 1024}


def _agent_env(tmp_path, reply, **extra):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": reply, "XWPE_AI_POLICY": "auto",
           "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
           "XWPE_UI_TRACE": str(tmp_path / "ui.trace")}
    env.update(extra)
    return env


def _read(path):
    return path.read_text() if path.exists() else ""


def _wait_file(s, path, limit=LIMIT_S):
    end = time.time() + limit
    while time.time() < end:
        if path.exists() and path.read_text().strip():
            return True
        s._drain(0.1)
    return False


def _wait_text(s, path, needle, limit=LIMIT_S):
    end = time.time() + limit
    while time.time() < end:
        if needle in _read(path):
            return True
        s._drain(0.1)
    return needle in _read(path)


def _run_agent(s, task):
    s.key(ALT.AI); s.key("g")
    s.key(task); s.key("\r", delay=0.3)


def _env_names(text):
    return {line.split("=", 1)[0] for line in text.splitlines() if "=" in line}


# @scenario SC-ai-contain-20
def test_a_run_command_never_sees_the_editors_keys_or_loader_variables(tmp_path):
    out = tmp_path / "cmd.env"
    reply = "TOOL run_command env > %s@@TURN@@DONE listed" % out
    env = _agent_env(tmp_path, reply,
                     ANTHROPIC_API_KEY=MARKER, OPENAI_API_KEY=MARKER + "-oa",
                     XWPE_AI_KEY=MARKER + "-xk", SSH_AUTH_SOCK="/run/" + MARKER,
                     LD_LIBRARY_PATH="/nonexistent/" + MARKER,
                     GIT_SSH_COMMAND=MARKER, ANTHROPIC_API_KEY_FILE="/nonexistent/" + MARKER)
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _run_agent(s, "list the environment")
        ran = _wait_file(s, out)
        s._drain(0.5)
    text = _read(out)
    assert ran, "the command never ran:\n" + _read(tmp_path / "ai.trace")
    assert MARKER not in text, "a marker reached the command:\n" + text
    names = _env_names(text)
    for name in ("ANTHROPIC_API_KEY", "OPENAI_API_KEY", "XWPE_AI_KEY", "SSH_AUTH_SOCK",
                 "LD_LIBRARY_PATH", "GIT_SSH_COMMAND", "ANTHROPIC_API_KEY_FILE"):
        assert name not in names, "%s reached the command:\n%s" % (name, text)


# @scenario SC-ai-contain-21
def test_a_run_command_still_has_what_a_build_needs(tmp_path):
    out = tmp_path / "cmd.env"
    reply = ("TOOL run_command (echo HOME=$HOME TERM=$TERM; cc --version; "
             "make --version) > %s@@TURN@@DONE listed" % out)
    env = _agent_env(tmp_path, reply, JAVA_HOME="/opt/some-jdk")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _run_agent(s, "show the environment")
        ran = _wait_file(s, out)
        s._drain(1.5)
    text = _read(out)
    assert ran, "the command never ran:\n" + _read(tmp_path / "ai.trace")
    assert "HOME=" + str(tmp_path) in text, text
    # What a build needs is that the compiler and make are found and run, not
    # which folders PATH names: each answers `--version` with its own name.
    assert "cc" in text.lower() and "GNU Make" in text, \
        "the compiler or make did not run:\n" + text
    assert "TERM=dumb" in text, text


# @scenario SC-ai-contain-22
def test_a_run_command_has_the_file_size_limit_and_no_new_privileges(tmp_path):
    out = tmp_path / "limits.txt"
    probe = ("echo FSIZE=$(ulimit -f); echo CORE=$(ulimit -c); "
             "if [ -r /proc/self/status ]; then grep '^NoNewPrivs' /proc/self/status; "
             "else echo NoNewPrivs: none; fi")
    reply = "TOOL run_command (%s) > %s@@TURN@@DONE probed" % (probe, out)
    with WpeSession(str(tmp_path), SEED, env_extra=_agent_env(tmp_path, reply)) as s:
        _run_agent(s, "probe the limits")
        ran = _wait_file(s, out)
        s._drain(0.5)
    text = _read(out)
    assert ran, "the command never ran:\n" + _read(tmp_path / "ai.trace")
    fsize = re.search(r"FSIZE=(\w+)", text)
    assert fsize and fsize.group(1).isdigit() and int(fsize.group(1)) in FSIZE_BLOCKS, \
        "the file-size limit is not the documented 8 GiB:\n" + text
    assert "CORE=0" in text, "a killed command may leave a core file:\n" + text
    if os.path.exists("/proc/self/status"):
        assert re.search(r"NoNewPrivs:\s*1", text), \
            "the command can still gain privileges:\n" + text


# @scenario SC-ai-contain-23
def test_the_user_can_name_a_file_size_limit_for_a_session(tmp_path):
    out = tmp_path / "limit.txt"
    reply = "TOOL run_command echo FSIZE=$(ulimit -f) > %s@@TURN@@DONE ok" % out
    env = _agent_env(tmp_path, reply, XWPE_AI_CONTAIN_FSIZE="64M")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _run_agent(s, "show the limit")
        ran = _wait_file(s, out)
        s._drain(0.5)
    text = _read(out)
    assert ran, "the command never ran:\n" + _read(tmp_path / "ai.trace")
    want = {64 * 1024 ** 2 // 512, 64 * 1024 ** 2 // 1024}
    got = re.search(r"FSIZE=(\w+)", text)
    assert got and got.group(1).isdigit() and int(got.group(1)) in want, text


# ------------------------------------------------------ Strict and its gate --

def _seed_config(home, lines):
    cfg = home / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text("[Programming]\n" + "".join(l + "\n" for l in lines))


# @scenario SC-ai-contain-24
def test_strict_refuses_the_assistants_command_while_nothing_is_held(tmp_path):
    marker = tmp_path / "ran.marker"
    reply = "TOOL run_command touch %s@@TURN@@DONE tried" % marker
    env = _agent_env(tmp_path, reply, XWPE_AI_CONTAIN="strict")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _run_agent(s, "touch the marker")
        refused = _wait_text(s, tmp_path / "ui.trace", "child uncontained")
        s._drain(1.0)
    assert refused, "the start gate said nothing:\n" + _read(tmp_path / "ui.trace")
    assert not marker.exists(), "the command ran under Strict"


# @scenario SC-ai-contain-25
def test_strict_in_the_users_config_cannot_be_loosened_by_the_start_folder(tmp_path):
    marker = tmp_path / "ran.marker"
    _seed_config(tmp_path, ["AIContain : strict"])
    (tmp_path / "xwperc").write_text("[Programming]\nAIContain : off\n")
    reply = "TOOL run_command touch %s@@TURN@@DONE tried" % marker
    with WpeSession(str(tmp_path), SEED, env_extra=_agent_env(tmp_path, reply)) as s:
        _run_agent(s, "touch the marker")
        refused = _wait_text(s, tmp_path / "ui.trace", "child uncontained")
        s._drain(1.0)
    assert refused, "Strict from the user's config did not hold:\n" + _read(tmp_path / "ui.trace")
    assert not marker.exists(), "a start folder's xwperc loosened the containment"


# @scenario SC-ai-contain-26
def test_a_names_list_in_the_users_config_is_kept_for_every_command(tmp_path):
    out = tmp_path / "cmd.env"
    _seed_config(tmp_path, ["AIContainEnv : MY_TOOL_HOME"])
    reply = "TOOL run_command env > %s@@TURN@@DONE listed" % out
    env = _agent_env(tmp_path, reply, MY_TOOL_HOME="/opt/mytool", OTHER_NAME="x")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _run_agent(s, "list the environment")
        ran = _wait_file(s, out)
        s._drain(0.5)
    names = _env_names(_read(out))
    assert ran, "the command never ran"
    assert "MY_TOOL_HOME" in names, "AIContainEnv did not keep its name:\n" + _read(out)
    assert "OTHER_NAME" not in names, "a name nobody listed came through:\n" + _read(out)


# @scenario SC-ai-contain-33
def test_a_start_folder_cannot_add_names_to_the_environment_or_say_which(tmp_path):
    out = tmp_path / "cmd.env"
    (tmp_path / "xwperc").write_text(
        "[Programming]\nAIContainEnv : ANTHROPIC_API_KEY,FOLDER_CHOSEN\nAIHostEnv : FOLDER_CHOSEN\n")
    reply = "TOOL run_command env > %s@@TURN@@DONE listed" % out
    env = _agent_env(tmp_path, reply, ANTHROPIC_API_KEY=MARKER, FOLDER_CHOSEN="x")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _run_agent(s, "list the environment")
        ran = _wait_file(s, out)
        s._drain(0.5)
    names = _env_names(_read(out))
    assert ran, "the command never ran"
    assert "FOLDER_CHOSEN" not in names, "a start folder named an environment variable:\n" + _read(out)
    assert MARKER not in _read(out), "a start folder let the key through:\n" + _read(out)


# ------------------------------------------------------------ the engines --

def _fake_claude(bindir, env_out, argv_out=None):
    """A fake `claude` that records its environment and answers once."""
    bindir.mkdir(exist_ok=True)
    fake = bindir / "claude"
    fake.write_text(
        "#!/bin/sh\n"
        "env > %s\n"
        "echo '{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
        "\"result\":\"engine ran\"}'\n" % env_out)
    fake.chmod(0o755)


def _next_claude(tmp_path_factory):
    """A stand-in `claude` in a folder outside the workspace: the next one on
    PATH once the workspace's own folder is dropped.  Returns the file its run
    writes and its folder."""
    elsewhere = tmp_path_factory.mktemp("elsewhere")
    seen = elsewhere / "claude.env"
    _fake_claude(elsewhere / "bin", seen)
    return seen, elsewhere / "bin"


def _claude_env(tmp_path, bindir, **extra):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "claudecli", "XWPE_AI_MODEL": "default",
           "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
           "XWPE_UI_TRACE": str(tmp_path / "ui.trace"),
           "PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
           "XWPE_AI_CONTAIN_PATH_ALLOW": str(bindir)}
    env.update(extra)
    return env


# @scenario SC-ai-contain-27
def test_the_claude_backend_keeps_its_login_variables_and_no_others(tmp_path):
    seen = tmp_path / "claude.env"
    _fake_claude(tmp_path / "bin", seen)
    env = _claude_env(tmp_path, tmp_path / "bin",
                      ANTHROPIC_API_KEY=MARKER, ANTHROPIC_BASE_URL="http://127.0.0.1:9",
                      CLAUDE_CODE_TEST_FLAG="1", OPENAI_API_KEY=MARKER + "-oa",
                      SSH_AUTH_SOCK="/run/" + MARKER, LD_LIBRARY_PATH="/nonexistent/" + MARKER)
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("a")
        s.key("hello"); s.key("\r", delay=1.0)
        ran = _wait_file(s, seen)
        s._drain(0.5)
    names = _env_names(_read(seen))
    assert ran, "the engine never ran:\n" + _read(tmp_path / "ai.trace")
    for name in ("ANTHROPIC_API_KEY", "ANTHROPIC_BASE_URL", "CLAUDE_CODE_TEST_FLAG"):
        assert name in names, "the engine lost its own %s:\n%s" % (name, _read(seen))
    for name in ("OPENAI_API_KEY", "SSH_AUTH_SOCK", "LD_LIBRARY_PATH"):
        assert name not in names, "%s reached the engine:\n%s" % (name, _read(seen))


# @scenario SC-ai-contain-28
def test_a_program_the_workspace_planted_is_never_the_engine(tmp_path, tmp_path_factory):
    seen = tmp_path / "claude.env"
    _fake_claude(tmp_path / "bin", seen)
    next_seen, next_bin = _next_claude(tmp_path_factory)
    env = _claude_env(tmp_path, tmp_path / "bin")
    del env["XWPE_AI_CONTAIN_PATH_ALLOW"]          # no seam: the workspace's bin is not trusted
    env["PATH"] = "%s:%s:/usr/bin:/bin" % (tmp_path / "bin", next_bin)
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("a")
        s.key("hello"); s.key("\r", delay=1.0)
        ran_next = _wait_file(s, next_seen)
        s._drain(1.0)
    assert not seen.exists(), "an engine named claude ran out of the workspace's own folder"
    assert ran_next, "the next claude on PATH, outside the workspace, never ran"


HOST_MOCK = textwrap.dedent('''\
    import json, os, sys
    def emit(o):
        sys.stdout.write(json.dumps(o) + "\\n"); sys.stdout.flush()
    sys.stdin.readline()
    with open(%r, "w") as fh:
        for k, v in sorted(os.environ.items()):
            fh.write("%%s=%%s\\n" %% (k, v))
    emit({"type": "system", "subtype": "init", "session_id": "m"})
    emit({"type": "assistant", "message": {"content": [{"type": "text", "text": "listed"}]}})
    emit({"type": "result", "subtype": "success", "is_error": False, "result": "listed"})
''')


def _host_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AIHostAdapter text needs AIHostCommand" in out
    except Exception:
        return False


# @scenario SC-ai-contain-29
@pytest.mark.skipif(not _host_build(), reason="built without --enable-ai-agent-host")
def test_a_hosted_command_keeps_its_openai_login_and_the_names_the_user_lists(tmp_path):
    seen = tmp_path / "host.env"
    mock = tmp_path / "mock_host.py"
    mock.write_text(HOST_MOCK % str(seen))
    _seed_config(tmp_path, ["AIHostEnv : MY_ENGINE_TOKEN"])
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_AGENT_ENGINE": "claude-code",
           "XWPE_AI_HOST_CMD": "python3 %s" % mock,
           "XWPE_AI_TRACE": str(tmp_path / "h.trace"),
           "OPENAI_API_KEY": MARKER, "OPENAI_BASE_URL": "http://127.0.0.1:9",
           "OPENAI_MODEL": "m", "MY_ENGINE_TOKEN": "tok",
           "ANTHROPIC_API_KEY": MARKER + "-an", "SSH_AUTH_SOCK": "/run/" + MARKER,
           "UNLISTED_NAME": "x"}
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("list it"); s.key("\r", delay=1.0)
        ran = _wait_file(s, seen)
        s._drain(0.5)
    names = _env_names(_read(seen))
    assert ran, "the hosted command never ran:\n" + _read(tmp_path / "h.trace")
    for name in ("OPENAI_API_KEY", "OPENAI_BASE_URL", "OPENAI_MODEL", "MY_ENGINE_TOKEN"):
        assert name in names, "the hosted command lost %s:\n%s" % (name, _read(seen))
    for name in ("ANTHROPIC_API_KEY", "SSH_AUTH_SOCK", "UNLISTED_NAME"):
        assert name not in names, "%s reached the hosted command:\n%s" % (name, _read(seen))


# ---------------------------------------------------- the header and the line --

# @scenario SC-ai-contain-30
def test_the_header_says_uncontained_and_the_line_is_said_once(tmp_path):
    reply = "DONE nothing to do"
    env = _agent_env(tmp_path, reply, XWPE_AI_MOCK_DELAY_MS="4000")   # the run is still going
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _run_agent(s, "first task")
        assert wait_screen(s, "[agent] task: first task", 8 * WAIT_SCALE), \
            "the run never started:\n" + "\n".join(s.display())
        rows = s.display()
        header = next(i for i, r in enumerate(rows) if "[AI: mock" in r)
        notice = next(i for i, r in enumerate(rows) if "[AI] containment on this system" in r)
        assert "| UNCONTAINED]" in rows[header], rows[header]
        task = next(i for i, r in enumerate(rows) if "[agent] task:" in r)
        assert task == header + 1, "the notice came between the header and the task:\n" + "\n".join(rows)
        assert notice < header, "the notice must come before the header"
        col = rows[notice].index("[AI] containment")
        notice_cell = s.screen.buffer[notice][col]
        header_cell = s.screen.buffer[header][rows[header].index("[AI:")]
        assert (notice_cell.fg, notice_cell.bg) != (header_cell.fg, header_cell.bg), \
            "the notice is not in the error colour"
        text = "\n".join(pane_text_with_history(s, ["containment on this system"]))
        s._drain(6.0)                    # the first run ends after the mock's delay
        _run_agent(s, "second task")
        assert wait_screen(s, "[agent] task: second task", 8 * WAIT_SCALE), \
            "the second run never started:\n" + "\n".join(s.display())
    assert "fs=none" in text and "net=uncontrolled" in text, text
    assert "NOT held" in text and "files you can read" in text, text
    trace = _read(tmp_path / "ai.trace")
    assert trace.count("agent containment=UNCONTAINED") == 2, \
        "both runs must name the word:\n" + trace
    assert trace.count("agent containment notice said") == 1, \
        "the notice was said again:\n" + trace


# --------------------------------------- a hard limit the editor inherited --

LOW_FSIZE_BYTES = 1000000000
LOW_FSIZE_ENV = "XWPE_TEST_LOW_FSIZE"       # set only for the inner row, under prlimit


@pytest.mark.skipif(LOW_FSIZE_ENV not in os.environ,
                    reason="the inner row: run by the one under prlimit")
def test_inner_a_command_starts_under_a_lower_hard_limit(tmp_path):
    out = tmp_path / "limit.txt"
    reply = "TOOL run_command echo FSIZE=$(ulimit -f) > %s@@TURN@@DONE ok" % out
    with WpeSession(str(tmp_path), SEED, env_extra=_agent_env(tmp_path, reply)) as s:
        _run_agent(s, "show the limit")
        ran = _wait_file(s, out)
        s._drain(0.5)
    text = _read(out)
    assert ran, "no command ran under the lower hard limit:\n" + _read(tmp_path / "ai.trace") + \
        "\n" + _read(tmp_path / "ui.trace")
    got = re.search(r"FSIZE=(\w+)", text)
    ceiling = {LOW_FSIZE_BYTES // 512, LOW_FSIZE_BYTES // 1024}
    assert got and got.group(1).isdigit() and int(got.group(1)) in ceiling, \
        "the command did not get the inherited limit:\n" + text


# @scenario SC-ai-contain-34
@pytest.mark.skipif(LOW_FSIZE_ENV in os.environ, reason="the outer row")
@needs("prlimit")
def test_every_command_still_starts_when_the_editor_inherits_a_lower_hard_limit(tmp_path):
    """A hard file-size limit below the 8 GiB default (a login shell's, a
    container's) used to make setrlimit fail in every assistant child, so no
    tool, git or engine command started.  The row runs the inner row, with
    its editor, under `prlimit --fsize=1000000000:1000000000`."""
    import sys
    env = dict(os.environ)
    env[LOW_FSIZE_ENV] = "1"
    inner = tmp_path / "inner"
    here = os.path.abspath(__file__)
    res = subprocess.run(
        ["prlimit", "--fsize=%d:%d" % (LOW_FSIZE_BYTES, LOW_FSIZE_BYTES), sys.executable, "-m",
         "pytest", "-q", "-p", "no:cacheprovider", "--basetemp=%s" % inner, "-rs",
         here + "::test_inner_a_command_starts_under_a_lower_hard_limit"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=170)
    assert res.returncode == 0 and "1 passed" in res.stdout, res.stdout[-3000:]


# -------------------------------------- an engine in a folder PATH drops --

# @scenario SC-ai-contain-35
def test_an_engine_in_a_dropped_workspace_folder_is_named_in_the_pane(tmp_path, tmp_path_factory):
    seen = tmp_path / "claude.env"
    _fake_claude(tmp_path / "bin", seen)
    # The pane names the dropped folder only when no other `claude` is left:
    # the rest of PATH is a folder outside the workspace with no engine in it
    # (none of the machine's folders, so no stub of the tool farm either).
    no_engine = tmp_path_factory.mktemp("noengine")
    env = _claude_env(tmp_path, tmp_path / "bin")
    del env["XWPE_AI_CONTAIN_PATH_ALLOW"]
    env["PATH"] = "%s:%s" % (tmp_path / "bin", no_engine)
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("a")
        s.key("hello"); s.key("\r", delay=1.0)
        s._drain(3.0)
        text = "\n".join(pane_text_with_history(s, ["inside the workspace"]))
    assert str(tmp_path / "bin") in text and "inside the workspace" in text, text
    assert "AIContainPathAllow" in text, text
    assert not seen.exists()


# @scenario SC-ai-contain-36
def test_the_users_own_config_can_allow_the_folder_an_engine_sits_in(tmp_path):
    seen = tmp_path / "claude.env"
    _fake_claude(tmp_path / "bin", seen)
    _seed_config(tmp_path, ["AIContainPathAllow : %s" % (tmp_path / "bin")])
    env = _claude_env(tmp_path, tmp_path / "bin")
    del env["XWPE_AI_CONTAIN_PATH_ALLOW"]
    env["PATH"] = str(tmp_path / "bin") + ":/usr/bin:/bin"    # no real claude behind the stand-in
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("a")
        s.key("hello"); s.key("\r", delay=1.0)
        ran = _wait_file(s, seen)
    assert ran, "the folder the user allowed was not kept on the engine's PATH"


# ------------------------------------- the commit that needs the agent --

GIT_STUB = """#!/bin/sh
case " $* " in
  *" commit "*) env > %s; exec /usr/bin/git -c commit.gpgsign=false "$@" ;;
esac
exec /usr/bin/git "$@"
"""


def _signing_repo(tmp_path, sign_lines):
    """A repository with a tracked file whose config holds @sign_lines."""
    def g(*a):
        subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t",
                        "-c", "commit.gpgsign=false", *a], cwd=tmp_path, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    g("init", "-q")
    (tmp_path / "t.c").write_text(SEED)
    g("add", "t.c")
    g("commit", "-q", "-m", "seed")
    with open(tmp_path / ".git" / "config", "a") as fh:
        fh.write("".join(l + "\n" for l in sign_lines))


def _commit_env_of_review(tmp_path, sign_lines):
    """Run an agent that writes a file, commit it from the review with the
    stub git on PATH, and return the environment the commit's git saw."""
    seen = tmp_path / "commit.env"
    gbin = tmp_path / "gbin"
    gbin.mkdir()
    (gbin / "git").write_text(GIT_STUB % seen)
    (gbin / "git").chmod(0o755)
    _signing_repo(tmp_path, sign_lines)
    trace = tmp_path.parent / (tmp_path.name + ".trace")
    state = tmp_path.parent / (tmp_path.name + ".state")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "auto",
           "XWPE_AI_MOCK_REPLY": "TOOL write_file a.txt\nhello\n@@END@@TURN@@DONE wrote it",
           "XWPE_AI_TRACE": str(trace), "XDG_STATE_HOME": str(state),
           "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t",
           "SSH_AUTH_SOCK": "/run/" + MARKER,
           "PATH": str(gbin) + ":" + os.environ.get("PATH", "")}
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("g")
        s.key("write a file"); s.key("\r", delay=1.0)
        end = time.time() + 60
        while time.time() < end and "changeset n=" not in _read(trace):
            s._drain(0.2)
        s.key("c", delay=0.8)
        s.key("\r", delay=1.0)
        _wait_file(s, seen, 30)
    return _read(seen)


SIGNING_CASES = [
    (["[commit]", "\tgpgsign = true", "[gpg]", "\tformat = ssh"], True),
    (["[commit]", "\tgpgsign = yes", "[gpg]", "\tformat = ssh"], True),
    (["[commit]", "\tgpgsign = 1", "[gpg]", "\tformat = ssh"], True),
    (["[commit]", "\tgpgsign = on", "[gpg]", "\tformat = ssh"], True),
    (["[commit]", "\tgpgsign", "[gpg]", "\tformat = ssh"], True),
    (["[commit]", "\tgpgsign = false", "[gpg]", "\tformat = ssh"], False),
    (["[commit]", "\tgpgsign = true", "[gpg]", "\tformat = openpgp"], False),
    (["[commit]", "\tgpgsign = true"], False),
]


# @scenario SC-ai-contain-37
@pytest.mark.parametrize("sign_lines,keeps", SIGNING_CASES,
                         ids=["true", "yes", "1", "on", "bare", "false", "openpgp", "no-format"])
def test_only_a_commit_git_signs_with_ssh_keeps_the_agent_socket(tmp_path, sign_lines, keeps):
    text = _commit_env_of_review(tmp_path, sign_lines)
    assert text, "the commit never ran through the stub git"
    names = _env_names(text)
    assert ("SSH_AUTH_SOCK" in names) == keeps, text
