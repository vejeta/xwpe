"""The AI pane tells who wrote a line by its colour.

Every transcript line used to paint in the editor text colour, and the lines
that await a key were red on the Options-text background, drawn over the
pane's left border.  The pane now colours a line by its role, from the colour
scheme: the user's own lines, the assistant's reply (the editor text entry),
the machinery lines ("[agent] ...", "[AI edit] ...", the header), the
attention lines (">> ...") and the errors each have an entry a user can change
in Options > Colors or in xwperc.  The pane also names the dialog that enables
the assistant, and sends the file it was opened from -- not its own transcript
-- as the chat's context.  Its placement next to Messages and under Tile is in
test_ai_pane_placement.py.

Scenarios: SC-ai-chat-36, SC-ai-chat-37, SC-ai-chat-18, SC-ai-chat-43,
SC-ai-chat-11b.
"""
import os
import subprocess
import time
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

# The default (Borland) scheme as the terminal reports it: the pane background
# is blue; a colour index above 7 shows as its base colour plus bold.
EDITOR_TEXT = ("brown", True, "blue")      # et = 11 on 4: the assistant's lines
USER_TEXT = ("white", True, "blue")        # 15 on 4
MACHINERY_TEXT = ("white", False, "blue")  # 7 on 4: light grey
ATTENTION_TEXT = ("white", True, "green")  # 15 on 2
ERROR_TEXT = ("red", True, "blue")         # 9 on 4
FRAME = ("white", True, "blue")            # er = 15 on 4

# A seeded scheme with the four pane entries set to other values.
SEEDED_USER = ("magenta", True, "blue")    # 13 on 4
SEEDED_MACHINERY = ("green", True, "blue")  # 10 on 4
SEEDED_ATTENTION = ("black", False, "cyan")  # 0 on 6
SEEDED_ERROR = ("magenta", False, "blue")  # 5 on 4

AGENT_ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
             "XWPE_AI_MOCK_REPLY": "DONE ok"}


def _cell(s, y, x):
    """(fg, bold, bg) of the cell at row y, column x."""
    c = s.screen.buffer[y][x]
    return (c.fg, c.bold, c.bg)


def _row_of(disp, needle):
    for y, r in enumerate(disp):
        if needle in r:
            return y
    return -1


def _text_cells(s, disp, needle):
    """The (fg, bold, bg) of the first three cells of `needle` on its row."""
    y = _row_of(disp, needle)
    assert y >= 0, "no line %r in:\n%s" % (needle, "\n".join(disp))
    x = disp[y].index(needle)
    return y, [_cell(s, y, x + i) for i in range(3)]


def _agent_then_question(s):
    """An agent run (machinery, the answer, an attention line) followed by a
    chat question (the user's line and the reply)."""
    s.key(ALT.AI); s.key("g")
    s.key("do it"); s.key("\r", delay=1.2)
    s._drain(1.0)
    s.key("hello"); s.key("\r", delay=1.2)
    s._drain(1.0)
    return s.display()


def _write_xwperc_colours(home, entries):
    d = os.path.join(home, ".config", "xwpe")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "xwperc"), "w") as fh:
        fh.write("[Color/Term]\n")
        for key, fg, bg in entries:
            fh.write("%s : %d %d\n" % (key, fg, bg))


# @scenario SC-ai-chat-36
def test_roles_paint_from_the_default_scheme(tmp_path):
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=AGENT_ENV, filename="t.c") as s:
        disp = _agent_then_question(s)
        _, user = _text_cells(s, disp, "You: hello")
        _, reply = _text_cells(s, disp, "AI: DONE ok")
        _, machinery = _text_cells(s, disp, "[agent] task: do it")
        _, attention = _text_cells(s, disp, ">> done")
        # "main" is plain program text; "int" is a keyword in its own colour.
        ed_y = _row_of(disp, "int main")
        editor = _cell(s, ed_y, disp[ed_y].index("main"))
    assert all(c == USER_TEXT for c in user), \
        "the user's line should be white on blue, got %r" % user
    assert all(c == EDITOR_TEXT for c in reply), \
        "the reply should be in the editor text colour, got %r" % reply
    assert all(c == editor for c in reply), \
        "the reply %r differs from the editor's text %r" % (reply, editor)
    assert all(c == MACHINERY_TEXT for c in machinery), \
        "the agent's line should be light grey on blue, got %r" % machinery
    assert all(c == ATTENTION_TEXT for c in attention), \
        "the attention line should be white on green, got %r" % attention


# @scenario SC-ai-chat-36
def test_error_line_is_light_red(tmp_path):
    # A backend that cannot start reports "[AI unavailable] ..." in the pane:
    # the claudecli backend with no `claude` on PATH does so without a network.
    nobin = tmp_path / "nobin"
    nobin.mkdir()
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "claudecli",
           "PATH": str(nobin)}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("a", delay=0.8)
        s._drain(0.5)
        disp = s.display()
        _, cells = _text_cells(s, disp, "[AI unavailable]")
    assert all(c == ERROR_TEXT for c in cells), \
        "an error line should be light red on blue, got %r" % cells


# @scenario SC-ai-chat-37
def test_scheme_keys_in_xwperc_recolour_the_roles(tmp_path):
    _write_xwperc_colours(str(tmp_path), [("aiu", 13, 4), ("aim", 10, 4),
                                          ("aia", 0, 6), ("aie", 5, 4)])
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=AGENT_ENV, filename="t.c") as s:
        disp = _agent_then_question(s)
        _, user = _text_cells(s, disp, "You: hello")
        _, reply = _text_cells(s, disp, "AI: DONE ok")
        _, machinery = _text_cells(s, disp, "[agent] task: do it")
        _, attention = _text_cells(s, disp, ">> done")
    assert all(c == SEEDED_USER for c in user), \
        "the user's line should follow the aiu key, got %r" % user
    assert all(c == SEEDED_MACHINERY for c in machinery), \
        "the agent's line should follow the aim key, got %r" % machinery
    assert all(c == SEEDED_ATTENTION for c in attention), \
        "the attention line should follow the aia key, got %r" % attention
    assert all(c == EDITOR_TEXT for c in reply), \
        "the reply stays in the editor text colour, got %r" % reply


# @scenario SC-ai-chat-37
def test_error_key_in_xwperc_recolours_the_error_line(tmp_path):
    _write_xwperc_colours(str(tmp_path), [("aie", 5, 4)])
    nobin = tmp_path / "nobin"
    nobin.mkdir()
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "claudecli",
           "PATH": str(nobin)}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("a", delay=0.8)
        s._drain(0.5)
        disp = s.display()
        _, cells = _text_cells(s, disp, "[AI unavailable]")
    assert all(c == SEEDED_ERROR for c in cells), \
        "the error line should follow the aie key, got %r" % cells


# @scenario SC-ai-chat-18
def test_attention_line_stays_inside_the_pane(tmp_path):
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=AGENT_ENV, filename="t.c") as s:
        s.key(ALT.AI); s.key("g")
        s.key("do it"); s.key("\r", delay=1.2)
        s._drain(1.0)
        disp = s.display()
        y = _row_of(disp, ">> done")
        assert y >= 0, "no attention line:\n" + "\n".join(disp)
        border_above = disp[y - 1][0]
        border_cell_above = _cell(s, y - 1, 0)
        border = disp[y][0]
        border_cell = _cell(s, y, 0)
        first = _cell(s, y, 1)
        # The pane's bottom frame row (it starts with the corner glyph and
        # carries the focused window's status fields) must hold no attention
        # colour: the highlight used to run one row past the interior.
        bottom = y + 1
        while bottom < len(disp) - 1 and not disp[bottom].startswith("m"):
            bottom += 1
        bottom_cells = [_cell(s, bottom, x) for x in range(0, 79)]
    assert border == border_above and border_cell == border_cell_above, \
        "the left border on the attention row was overwritten: %r %r" % (border, border_cell)
    assert border_cell == FRAME, "the border lost the frame colour: %r" % (border_cell,)
    assert first == ATTENTION_TEXT, \
        "the attention line should start at the first interior column in white " \
        "on green, got %r" % (first,)
    assert disp[bottom].startswith("m") and _cell(s, bottom, 0) == FRAME, \
        "the pane's bottom border is gone:\n" + "\n".join(disp)
    assert not any(c[2] == "green" for c in bottom_cells), \
        "the pane's bottom border was painted in the attention colour: %r" % bottom_cells


# @scenario SC-ai-chat-43
def test_assistant_off_line_names_options_ai(tmp_path):
    env = {"XWPE_AI_ENABLE": "0"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s.key(ALT.AI, delay=0.8)
        s._drain(0.5)
        disp = s.display()
    text = " ".join(r.strip("x ").strip() for r in disp)
    assert "AI assistant is off" in text, "no off line:\n" + "\n".join(disp)
    assert "Options > AI" in text, \
        "the off line should name Options > AI:\n" + "\n".join(disp)
    assert "Options > Editor" not in text, \
        "the off line still names Options > Editor:\n" + "\n".join(disp)


def _stub_claude_capturing(dir_, capture, reply):
    """A fake `claude` CLI that saves its stdin (the request body) to `capture`
    and answers with a success envelope whose result is `reply`."""
    d = os.path.join(dir_, "bin")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "claude")
    envelope = ('{"type":"result","subtype":"success","is_error":false,'
                '"result":"%s","session_id":"stub"}' % reply)
    with open(p, "w") as f:
        f.write("#!/bin/sh\n"
                "cat > '" + capture + "'\n"
                "printf '%s' '" + envelope + "'\n")
    os.chmod(p, 0o755)
    return d


def _wait_screen(s, needle, tmo=15):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in "\n".join(s.display()):
            return True
    return False


# @scenario SC-ai-chat-11b
def test_chat_context_is_the_file_not_the_transcript(tmp_path):
    orig = "int unique_marker_fn(void){return 7;}\n"
    capture = str(tmp_path / "request.txt")
    bindir = _stub_claude_capturing(str(tmp_path), capture, "STUBREPLY")
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "claudecli",
        "XWPE_AI_MODEL": "sonnet",
        "PATH": bindir + ":" + os.environ.get("PATH", ""),
        "XWPE_AI_TRACE": str(tmp_path / "c.trace"),
    }
    with WpeSession(str(tmp_path), orig, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("a", delay=0.8)
        s._drain(0.5)
        s.key("what is this"); s.key("\r", delay=0.8)
        assert _wait_screen(s, "STUBREPLY"), \
            "the chat turn did not complete:\n" + "\n".join(s.display())
    body = open(capture).read()
    assert "--- current file ---\n" + orig in body, \
        "the request should carry the file the chat was opened from:\n" + body
    assert "You: what is this" not in body, \
        "the request carries the pane's own transcript as the current file:\n" + body
