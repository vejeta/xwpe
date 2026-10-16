"""Preflight, the Claude CLI engine's argv, every spawn path's descriptors,
and the hosted engine's end-of-session lines.

Before a run starts the editor checks the backend can be reached at all: an
unreachable Ollama endpoint or a missing `claude` binary is refused with a
named line and no stream is opened. Driving the Claude CLI as the built-in
engine (XWPE_AI_BACKEND=claudecli) sends it a text-only prompt whatever the
dial, with the destructive tools disallowed and a one-line tip shown only
under auto, where full autonomy is available but not yet asked for. Every
child the agent spawns -- the CLI, the post-edit hook, git for the
checkpoint, and the hosted engine -- runs detached from the editor's own
terminal: its own session, no controlling tty, stdin/stdout piped (stderr
piped or /dev/null). The hosted engine (XWPE_AI_AGENT_ENGINE=claude-code)
maps the permission dial onto the CLI's own flags at spawn time; ending the
session reviews whatever it wrote and then says so, and a command that
cannot even be executed is reported through the same send-failure path a
dead pipe takes, not a fork/pipe "could not start" (that needs the pipe
setup itself to fail, which an unexecutable path never provokes).

The mock backend cannot show argv or process facts, so these tests put a
fake `claude`/`git`/hook on PATH and read what reached them.
"""
import os
import subprocess
import textwrap
import time

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, pane_text_with_history

SEED = "int main(void){return 0;}\n"
FOLLOWUP_LINE = "type a follow-up"
REVIEW_PROMPT = "a = keep all"
TIP_LINE = "[tip] For full autonomy"
TURN_POLL_S = 20
# The hosted session's end-of-run review names the file by its full path in
# the Messages window; see _host_run.
WIDE_COLS = 160
WIDE_ROWS = 30

PROCFACTS = (
    'echo "pid=$$ sid=$(ps -o sid= -p $$ | tr -d \' \') ppid=$PPID" >> "%(out)s"\n'
    'for fd in 0 1 2; do echo "fd$fd=$(readlink /proc/$$/fd/$fd)" >> "%(out)s"; done\n'
    'echo "tty=$(tty 2>&1)" >> "%(out)s"\n'
    'echo "argv=$*" >> "%(out)s"\n'
)

HOST_MOCK = textwrap.dedent('''\
    import sys, json, os
    facts = os.environ.get("HOST_FACTS")
    if facts:
        with open(facts, "a") as fh:
            fh.write("pid=%d sid=%d\\n" % (os.getpid(), os.getsid(0)))
            for fd in (0, 1, 2):
                try: fh.write("fd%d=%s\\n" % (fd, os.readlink("/proc/self/fd/%d" % fd)))
                except OSError as e: fh.write("fd%d=ERR %s\\n" % (fd, e))
            fh.write("argv=%s\\n" % " ".join(sys.argv[1:]))
    def emit(o):
        sys.stdout.write(json.dumps(o) + "\\n"); sys.stdout.flush()
    while True:
        line = sys.stdin.readline()
        if not line:
            break
        with open("t.c", "w") as fh:
            fh.write("int main(void){return 0;} /* HOSTWROTE */\\n")
        emit({"type": "system", "subtype": "init", "session_id": "mock-1"})
        emit({"type": "assistant", "message": {"content": [
            {"type": "text", "text": "Editing t.c now."},
            {"type": "tool_use", "name": "Write", "input": {"file_path": "t.c"}}]}})
        emit({"type": "result", "subtype": "success", "is_error": False, "result": "edited t.c"})
''')


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


def _host_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AIHostAdapter text needs AIHostCommand" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _trace(tmp_path):
    t = tmp_path / "ai.trace"
    return t.read_text() if t.exists() else ""


def _screen(s, drain=0.2):
    return "\n".join(s.display(drain=drain))


def _seen_within(s, needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        s._drain(0.25)
        if needle in "\n".join(s.display(drain=0)):
            return True
    return False


def _seed_xwperc(tmp_path, lines):
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text("[Programming]\n" + "".join(l + "\n" for l in lines))


def _start_agent(s, task, delay=1.0):
    s.key(ALT.AI); s.key("g")
    s.key(task); s.key("\r", delay=delay)


def _stub_claude(dir_, capture, reply, facts=None):
    """A fake `claude` on PATH: saves its argv to `capture`.argv, its stdin
    to `capture`, optionally its process facts to `facts`, and answers a
    success envelope whose result is `reply`."""
    d = os.path.join(dir_, "bin")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "claude")
    envelope = ('{"type":"result","subtype":"success","is_error":false,'
                '"result":"%s","session_id":"stub"}' % reply.replace("\n", "\\n"))
    body = "#!/bin/sh\n" + 'printf \'%s\\n\' "$@" > \'' + capture + ".argv'\n"
    if facts:
        body += PROCFACTS % {"out": facts}
    body += "cat > '" + capture + "'\n" + "printf '%s' '" + envelope + "'\n"
    with open(p, "w") as f:
        f.write(body)
    os.chmod(p, 0o755)
    return d


def _stub_git(dir_, facts):
    """A fake `git` on PATH that records its process facts then runs the
    real git, so the checkpoint behaves as usual and the spawn is seen."""
    d = os.path.join(dir_, "bin")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "git")
    with open(p, "w") as f:
        f.write("#!/bin/sh\n" + PROCFACTS % {"out": facts} + 'exec /usr/bin/git "$@"\n')
    os.chmod(p, 0o755)
    return d


def _stub_claude_as_host(tmp_path):
    """A `claude` on PATH that records argv (what the editor spawned for the
    hosted engine) and then speaks stream-json like HOST_MOCK."""
    mock = tmp_path / "host_mock.py"
    mock.write_text(HOST_MOCK)
    d = tmp_path / "bin"
    d.mkdir(exist_ok=True)
    p = d / "claude"
    p.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\" > '" + str(tmp_path / "host.argv") + "'\n"
                 "exec python3 '" + str(mock) + "' \"$@\"\n")
    p.chmod(0o755)
    return str(d)


def _facts_fields(facts):
    d = {}
    for line in facts.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            d.setdefault(k.split()[0], v)
    return d


def _command_facts_ok(facts, want_fd0):
    """Own session (sid in {pid, ppid} -- a shell-run command's child sees its
    parent's pid as sid), no tty, fd0 as wanted, fd1 a pipe, fd2 pipe or
    /dev/null.  For the PROCFACTS format (a shell script), which names pid,
    sid AND ppid on one line."""
    d = _facts_fields(facts)
    pid_sid = next(l for l in facts.splitlines() if l.startswith("pid="))
    pid = int(pid_sid.split()[0].split("=")[1])
    sid = int(pid_sid.split()[1].split("=")[1])
    ppid = int(pid_sid.split()[2].split("=")[1])
    probs = []
    if sid not in (pid, ppid):
        probs.append("not its own session: pid=%d sid=%d ppid=%d" % (pid, sid, ppid))
    if "tty" in d and "not a tty" not in d["tty"]:
        probs.append("has a terminal: %s" % d["tty"])
    if want_fd0 not in d.get("fd0", ""):
        probs.append("fd0=%s" % d.get("fd0"))
    if "pipe" not in d.get("fd1", ""):
        probs.append("fd1=%s" % d.get("fd1"))
    if "pipe" not in d.get("fd2", "") and "/dev/null" not in d.get("fd2", ""):
        probs.append("fd2=%s" % d.get("fd2"))
    return probs


def _host_facts_ok(facts, want_fd0):
    """Like _command_facts_ok but for HOST_MOCK's own facts format
    ("pid=X sid=Y", no ppid token and no tty line): the hosted engine is a
    python child, not a shell script, so its facts dump differs."""
    d = _facts_fields(facts)
    pid_sid = next(l for l in facts.splitlines() if l.startswith("pid="))
    pid = int(pid_sid.split()[0].split("=")[1])
    sid = int(pid_sid.split()[1].split("=")[1])
    probs = []
    if sid != pid:
        probs.append("not its own session: pid=%d sid=%d" % (pid, sid))
    if want_fd0 not in d.get("fd0", ""):
        probs.append("fd0=%s" % d.get("fd0"))
    if "pipe" not in d.get("fd1", ""):
        probs.append("fd1=%s" % d.get("fd1"))
    if "pipe" not in d.get("fd2", "") and "/dev/null" not in d.get("fd2", ""):
        probs.append("fd2=%s" % d.get("fd2"))
    return probs


# @scenario SC-ai-agent-04
def test_preflight_ollama_unreachable_refuses_to_start(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "ollama",
           "XWPE_AI_ENDPOINT": "http://127.0.0.1:9/", "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "x", delay=2.0)
        s._drain(1.5)
        rows = s.display()
    txt = _trace(tmp_path)
    text = "\n".join(rows)
    assert "Ollama not reachable at http://127.0.0.1:9/" in text, text
    assert "stream start" not in txt, txt


# @scenario SC-ai-agent-04
def test_preflight_claudecli_missing_refuses_to_start(tmp_path):
    empty = tmp_path / "emptybin"
    empty.mkdir()
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "claudecli", "PATH": str(empty),
           "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "x", delay=2.0)
        rows = s.display()
    txt = _trace(tmp_path)
    text = "\n".join(rows)
    assert "Claude Code CLI not found - install `claude` and log in (claude auth)" in text, text
    assert "stream start" not in txt, txt


def _claudecli_agent(tmp_path, policy):
    capture = str(tmp_path / "request.txt")
    facts = str(tmp_path / "claude.facts")
    bindir = _stub_claude(str(tmp_path), capture, "DONE ok", facts=facts)
    # The web switch is answered on: these cases assert what the dial puts on
    # the CLI's command line, and the web tools are pre-approved only while the
    # switch is on (tests/test_ai_claudecli_web.py owns both of its states).
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "claudecli", "XWPE_AI_MODEL": "sonnet",
           "XWPE_AI_POLICY": policy, "PATH": bindir + ":" + os.environ.get("PATH", ""),
           "XWPE_AI_WEB": "1",
           "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "look", delay=1.0)
        # Under auto the run is checkpointed and the review lists the stub's
        # own capture files (they live in the workdir): keep them and go on.
        if _seen_within(s, REVIEW_PROMPT, 6):
            s.key("a", delay=1.0)
        assert _seen_within(s, FOLLOWUP_LINE, 20), _screen(s)
        rows = _rows_with_history(s)
    argv = open(capture + ".argv").read().split("\n")
    return rows, argv, open(facts).read()


def _rows_with_history(s):
    """The pane's lines at the end of a run: the view pinned on the input
    row, then the pages PgUp brings back while the tip is not among them.
    The tip shown before the run, the header and the run's own lines are
    more than the pane holds once the hint lines are written at the first
    chat arm (and the review's Messages window has halved the pane), so the
    oldest lines are read the way a user reads them, with PgUp from the
    input row."""
    return pane_text_with_history(s, [TIP_LINE])


# @scenario SC-ai-agent-23
def test_claudecli_text_only_under_auto_with_tip(tmp_path):
    rows, argv, facts = _claudecli_agent(tmp_path, "auto")
    text = "\n".join(rows)
    assert "--disallowedTools" in argv and "Bash" in argv and "Write" in argv, argv
    assert "--allowedTools" in argv and "WebSearch" in argv and "WebFetch" in argv, argv
    assert "--dangerously-skip-permissions" not in argv, argv
    assert "--permission-mode" not in argv, argv
    assert TIP_LINE in text, text


# @scenario SC-ai-agent-23
def test_claudecli_text_only_under_ask_no_tip(tmp_path):
    rows, argv, facts = _claudecli_agent(tmp_path, "ask")
    text = "\n".join(rows)
    assert "--disallowedTools" in argv and "Bash" in argv, argv
    assert "[tip]" not in text, text


# @scenario SC-ai-agent-47
def test_claudecli_spawn_is_its_own_session_with_no_terminal(tmp_path):
    rows, argv, facts = _claudecli_agent(tmp_path, "ask")
    probs = _command_facts_ok(facts, "pipe")
    assert not probs, probs


# @scenario SC-ai-agent-47
def test_hook_and_git_checkpoint_spawns_are_their_own_session(tmp_path):
    hookfacts = str(tmp_path / "hook.facts")
    gitfacts = str(tmp_path / "git.facts")
    hook = tmp_path / "hook.sh"
    hook.write_text("#!/bin/sh\n" + (PROCFACTS % {"out": hookfacts}))
    hook.chmod(0o755)
    bindir = _stub_git(str(tmp_path), gitfacts)
    _seed_xwperc(tmp_path, ["AIEditHook : %s" % hook])
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "TOOL write_file a.txt\nAAA\n@@END@@TURN@@DONE ok",
           "XWPE_AI_POLICY": "auto", "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
           "PATH": bindir + ":" + os.environ.get("PATH", "")}
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        _start_agent(s, "write", delay=1.0)
        assert _seen_within(s, REVIEW_PROMPT, 20), _screen(s)
        s.key("a", delay=1.0)
        _seen_within(s, FOLLOWUP_LINE, 10)
    hf = open(hookfacts).read() if os.path.exists(hookfacts) else ""
    gf = open(gitfacts).read() if os.path.exists(gitfacts) else ""
    assert hf, "the hook never ran"
    assert gf, "git was never spawned for the checkpoint"
    assert not _command_facts_ok(hf, "/dev/null"), _command_facts_ok(hf, "/dev/null")
    assert not _command_facts_ok(gf, "/dev/null"), _command_facts_ok(gf, "/dev/null")


def _assert_web_denied(argv):
    """The hosted CLI must be told not to use its web tools while the web
    switch is not on.  Its permission callback cannot carry the switch at every
    dial -- the auto dial asks the CLI to run unattended and routes no prompt
    back to the editor -- so this list is the lever that holds everywhere.
    @argv is one argument per element, as the stub recorded them."""
    shown = " ".join(argv)
    assert "--disallowedTools" in argv, \
        "no web denial with the switch unanswered:\n" + shown
    after = argv[argv.index("--disallowedTools") + 1:]
    assert "WebSearch" in after and "WebFetch" in after, shown


def _host_run(tmp_path, policy, end_session=False, web=None):
    bindir = _stub_claude_as_host(tmp_path)
    facts = str(tmp_path / "host.facts")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_AGENT_ENGINE": "claude-code",
           "XWPE_AI_POLICY": policy, "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
           "PATH": bindir + ":" + os.environ.get("PATH", ""), "HOST_FACTS": facts}
    # No XWPE_AI_WEB unless a case asks for one, so the default run has the
    # switch unanswered, which is what a user meets on a first launch.
    if web is not None:
        env["XWPE_AI_WEB"] = web
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("edit t.c"); s.key("\r", delay=1.0)
        assert _seen_within(s, "edited t.c", 15), _screen(s)
        s._drain(1.0)
        rows = s.display()
        review = None; ended = None
        if end_session:
            s.key(ALT.AI); s.key("g"); s._drain(0.5)
            s.key(chr(27), delay=1.0)      # Esc at the prompt ends the session
            if _seen_within(s, REVIEW_PROMPT, 8):
                # The review lists the written file by its full path in the
                # Messages window, an ordinary editor window cut at its border
                # once it shares the strip with the pane: read it with the
                # terminal widened, as a user with a long path would widen it.
                s.resize(WIDE_COLS, WIDE_ROWS)
                review = s.display()
                s.key("a", delay=1.0)
            _seen_within(s, "session ended", 8)
            ended = s.display()
    argv = open(str(tmp_path / "host.argv")).read().split("\n")
    hf = open(facts).read() if os.path.exists(facts) else ""
    return rows, argv, hf, review, ended


# @scenario SC-ai-agent-47
def test_hosted_engine_spawn_is_its_own_session_with_no_terminal(tmp_path):
    if not _host_build():
        pytest.skip("built without --enable-ai-agent-host")
    rows, argv, hf, _, _ = _host_run(tmp_path, "ask")
    probs = _host_facts_ok(hf, "pipe")
    assert not probs, probs


# @scenario SC-ai-agent-50
def test_hosted_engine_argv_under_ask(tmp_path):
    if not _host_build():
        pytest.skip("built without --enable-ai-agent-host")
    rows, argv, hf, _, _ = _host_run(tmp_path, "ask")
    assert "--permission-mode" in argv and "default" in argv, argv
    assert "--permission-prompts" in argv and "host" in argv, argv
    assert "--permission-prompt-tool" in argv and "stdio" in argv, argv
    assert "--dangerously-skip-permissions" not in argv, argv
    _assert_web_denied(argv)


# @scenario SC-ai-agent-50
def test_hosted_engine_argv_under_edits(tmp_path):
    if not _host_build():
        pytest.skip("built without --enable-ai-agent-host")
    rows, argv, hf, _, _ = _host_run(tmp_path, "edits")
    assert "--permission-mode" in argv and "acceptEdits" in argv, argv
    assert "--permission-prompts" in argv and "--permission-prompt-tool" in argv, argv
    assert "--dangerously-skip-permissions" not in argv
    _assert_web_denied(argv)


# @scenario SC-ai-agent-50
def test_hosted_engine_argv_under_auto(tmp_path):
    if not _host_build():
        pytest.skip("built without --enable-ai-agent-host")
    rows, argv, hf, _, _ = _host_run(tmp_path, "auto")
    assert "--dangerously-skip-permissions" in argv, argv
    assert "--permission-mode" not in argv, argv
    # This dial routes no prompt back to the editor, so the deny list is the
    # only place the web switch can speak here.
    _assert_web_denied(argv)


# @scenario SC-ai-agent-50
def test_hosted_engine_argv_opens_the_web_once_the_switch_is_on(tmp_path):
    """The other side of the switch: with it answered on, the hosted CLI is
    launched with no web denial, and the dial alone decides whether the editor
    is asked first."""
    if not _host_build():
        pytest.skip("built without --enable-ai-agent-host")
    rows, argv, hf, _, _ = _host_run(tmp_path, "auto", web="1")
    assert "--disallowedTools" not in argv and "WebFetch" not in argv, \
        "the web stayed denied with the switch on:\n" + " ".join(argv)


# @scenario SC-ai-agent-53
def test_hosted_session_end_at_the_prompt_reviews_then_says_so(tmp_path):
    if not _host_build():
        pytest.skip("built without --enable-ai-agent-host")
    rows, argv, hf, review, ended = _host_run(tmp_path, "ask", end_session=True)
    assert review is not None, "no review although the engine wrote t.c"
    assert "AI changeset:" in "\n".join(review) and "/t.c:1:" in "\n".join(review), "\n".join(review)
    assert "[agent] Claude Code session ended" in "\n".join(ended), "\n".join(ended)


# @scenario SC-ai-agent-53
def test_hosted_unexecutable_command_reports_send_failed(tmp_path):
    if not _host_build():
        pytest.skip("built without --enable-ai-agent-host")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_AGENT_ENGINE": "claude-code",
           "XWPE_AI_POLICY": "ask", "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
           "XWPE_AI_HOST_CMD": "/nonexistent/agent-binary"}
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s._drain(0.5)
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("hello"); s.key("\r", delay=1.5)
        s._drain(2.0)
        rows = s.display()
    text = "\n".join(rows)
    # An exec() failure inside the already-forked child surfaces at the first
    # send, not as a spawn failure: fork()/pipe() themselves did not fail, so
    # "[agent] could not start ..." (which needs THAT) never shows here.
    assert "[agent] send failed - session ended" in text, \
        "an unexecutable host command was not reported as a send failure:\n" + text
