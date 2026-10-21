"""The AI pane under the File keys, and the size of a request.

The pane intercepts only its input keys (we_ai_ui.c: e_ai_chat_key); a
function key goes back to the editor.  Alt-F2 is the Window menu's
"Size/Move" in the default key set (we_menue.c, we_edit.c: e_tst_dfkt), and
with the pane focused it moves the pane like any other window: the frame
loses its buttons while the move runs, Enter ends it, and the input row then
takes keys again without the Enter having sent anything.

F2, File > Save and File > Save As on the pane write its transcript
(we_ai_ui.c: wpe_ai_pane_save; we_fl_unix.c: the Save As dialog under the
title "Save transcript").  A request holds AI_PROMPT_MAX bytes in the input
row, the prompt field and the composer, and each says "Input at its limit"
at the byte past it (we_e_aus.c: e_input_limit_words); a pane line cut for
display ends with "..." (we_ai_ui.c: ai_mark_display_cut).  What the
backend received is read from the mock backend's "mock request" trace line.

The mock backend makes every reply deterministic; nothing here reaches a
live provider.
"""
import os
import subprocess
import time

import pytest
from wpe_driver import ALT, WPE_BIN, WpeSession, pane_rows

SEED = "int main(void){return 0;}\n"
ALT_F2 = "\033\033OQ"                  # Esc prefix + F2: the terminal's Alt-F2
CURSOR_UP = "\033[A"
PANE_TITLE = " AI "
FRAME_BUTTONS = "□ ✕"        # the zoom and close glyphs on the frame


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "REPLYONE",
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }


def _trace(tmp_path):
    t = tmp_path / "ai.trace"
    return t.read_text() if t.exists() else ""


def _pane_top(rows):
    """The screen row of the pane's top frame, -1 when it is not shown."""
    return next((i for i, r in enumerate(rows) if PANE_TITLE in r and "⚙" in r), -1)


# @scenario SC-ai-chat-59
def test_alt_f2_moves_the_focused_pane_and_enter_ends_the_move(tmp_path):
    """Alt-F2 with the pane focused is Size/Move; Enter ends it, nothing is sent.

    After Alt-F2 and one Up the pane's top frame is one row higher and has
    no buttons (the move is running).  Enter gives the buttons back, the
    trace holds no "chat prompt=", and typed letters land in the input row.
    """
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        s.key(ALT.AI)
        s.key("a", delay=0.8)
        start = s.display()
        s.key(ALT_F2, delay=1.0)
        s.key(CURSOR_UP, delay=0.6)
        moving = s.display()
        s.key("\r", delay=0.8)
        ended = s.display()
        s.key("zz", delay=0.6)
        typed = s.display()
    top0 = _pane_top(start)
    top1 = _pane_top(moving)
    assert top0 >= 0, "the pane did not open:\n" + "\n".join(start)
    assert top1 == top0 - 1, \
        "Alt-F2 + Up did not move the pane up one row (%d -> %d)" % (top0, top1)
    assert FRAME_BUTTONS not in moving[top1], \
        "the frame kept its buttons, so no move was running: %r" % moving[top1]
    assert FRAME_BUTTONS in ended[_pane_top(ended)], \
        "Enter did not end the move: %r" % ended[_pane_top(ended)]
    assert "chat prompt=" not in _trace(tmp_path), \
        "the Enter that ended the move sent a question:\n" + _trace(tmp_path)
    assert any(r[1:].startswith("> zz") for r in typed), \
        "the input row did not take keys after the move:\n" + "\n".join(typed)


# ---- saving the transcript ----------------------------------------------

F2 = "\033OQ"
F5 = "\033[15~"                         # Window > Zoom: the pane covers the editor
ALT_NAME = "\033n"                      # the Save As dialog's Name field
SAVE_TITLE = "Save transcript"
SAVE_FAILED = "File failed to save"
# A reply longer than the pane is wide, with a character outside ASCII: the
# pane wraps it over rows, the file must hold it as the one line it is.
LONG_REPLY = ("The transcript keeps every word of this reply on one line "
              "even though the pane wraps it over two rows: café au lait.")
PLACEMENTS = ["docked", "zoomed"]


def _save_env(tmp_path):
    env = _env(tmp_path)
    env["XWPE_AI_MOCK_REPLY"] = LONG_REPLY
    return env


def _ask(s, question):
    """Type a question into the armed input row and let the reply land."""
    s.key(question, delay=0.3)
    s.key("\r", delay=1.5)


def _open_chat_with_turn(s, placement):
    s.key(ALT.AI)
    s.key("a", delay=0.8)
    _ask(s, "hello")
    if placement == "zoomed":
        s.key(F5, delay=0.8)


def _name_in_dialog(s, name):
    """Type ``name`` into the Save As dialog's Name field and confirm it."""
    s.key(ALT_NAME, delay=0.4)
    s.key(name, delay=0.4)
    s.key("\r", delay=1.5)


def _transcript_shown(s):
    """The pane's rows above its input row, as the pane shows them."""
    out = []
    for row in pane_rows(s.display()):
        if row.startswith(">"):
            break
        out.append(row.rstrip())
    return out


def _glyphs(lines):
    """The text of ``lines`` with every blank taken out: the same for the
    rows a line was wrapped into and for the one line they were cut from."""
    return "".join("".join(l.split()) for l in lines)


def _file_lines(path):
    with open(path, "rb") as fh:
        text = fh.read().decode("utf-8")
    lines = text.split("\n")
    assert lines[-1] == "", "the file does not end with a line break: %r" % text[-40:]
    lines = lines[:-1]
    while lines and not lines[-1]:
        lines.pop()
    return lines


# @scenario SC-ai-chat-60
@pytest.mark.parametrize("placement", PLACEMENTS)
def test_f2_saves_the_transcript_and_a_second_f2_asks_nothing(tmp_path, placement):
    """F2 in the pane asks a name under "Save transcript", writes, then rewrites.

    The file holds the logical lines the pane shows, one per line and
    unwrapped (the long reply on one line, its accented character as UTF-8),
    without the input row; the status row says "saved <name>".  A second F2
    after another question opens no dialog and the file then holds the new
    question too.  The pane stays the read-only pane: nothing says it failed.
    """
    target = tmp_path / "pane.txt"
    with WpeSession(str(tmp_path), SEED, env_extra=_save_env(tmp_path)) as s:
        _open_chat_with_turn(s, placement)
        shown = _transcript_shown(s)
        s.key(F2, delay=1.0)
        asking = "\n".join(s.display())
        _name_in_dialog(s, "pane.txt")
        after_first = s.display()
        first = _file_lines(target) if target.exists() else None
        if placement == "zoomed":
            s.key(F5, delay=0.8)
        s.key(ALT.AI)
        s.key("a", delay=0.8)
        _ask(s, "again")
        s.key(F2, delay=1.2)
        after_second = s.display()
        second = _file_lines(target) if target.exists() else None
    assert SAVE_FAILED not in asking, "F2 answered with an error box:\n" + asking
    assert SAVE_TITLE in asking, "F2 did not ask a name under %r:\n%s" % (SAVE_TITLE, asking)
    assert first is not None, "F2 + a name wrote no file"
    assert "AI: " + LONG_REPLY in first, \
        "the reply is not one unwrapped line in the file: %r" % first
    assert "You: hello" in first, first
    assert not any(l.startswith(">") for l in first), "the input row was saved: %r" % first
    assert _glyphs(first) == _glyphs(shown), \
        "the file is not the text the pane shows:\nfile  %r\npane  %r" % (first, shown)
    assert len(first) < len(shown), \
        "the file has as many lines as the pane has rows: %r" % first
    assert "saved pane.txt" in after_first[-1], \
        "the status row does not say the save: %r" % after_first[-1]
    assert SAVE_TITLE not in "\n".join(after_second), \
        "the second F2 asked a name again:\n" + "\n".join(after_second)
    assert second is not None and "You: again" in second, \
        "the second F2 did not rewrite the file: %r" % second
    assert "saved pane.txt" in after_second[-1], after_second[-1]


# @scenario SC-ai-chat-61
@pytest.mark.parametrize("placement", PLACEMENTS)
def test_save_as_writes_the_named_file_every_time(tmp_path, placement):
    """File > Save As asks every time and writes the name typed.

    The first Save As writes one.txt; a second Save As asks again (the
    dialog is back) and writes two.txt; the status row names each file.
    """
    with WpeSession(str(tmp_path), SEED, env_extra=_save_env(tmp_path)) as s:
        _open_chat_with_turn(s, placement)
        s.key(ALT.FILE, delay=0.6)
        s.key("a", delay=1.2)
        first_dialog = "\n".join(s.display())
        _name_in_dialog(s, "one.txt")
        after_one = s.display()
        s.key(ALT.FILE, delay=0.6)
        s.key("a", delay=1.2)
        second_dialog = "\n".join(s.display())
        _name_in_dialog(s, "two.txt")
        after_two = s.display()
    assert SAVE_TITLE in first_dialog, first_dialog
    assert (tmp_path / "one.txt").exists(), "Save As + a name wrote nothing"
    assert "saved one.txt" in after_one[-1], after_one[-1]
    assert SAVE_TITLE in second_dialog, "the second Save As did not ask:\n" + second_dialog
    assert (tmp_path / "two.txt").exists(), "the second Save As wrote nothing"
    assert "AI: " + LONG_REPLY in _file_lines(tmp_path / "two.txt")
    assert "saved two.txt" in after_two[-1], after_two[-1]
    assert SAVE_TITLE not in "\n".join(after_two), "the dialog stayed open"


# @scenario SC-ai-chat-61
def test_esc_in_the_save_dialog_writes_nothing(tmp_path):
    """Esc in the "Save transcript" dialog closes it and writes no file."""
    with WpeSession(str(tmp_path), SEED, env_extra=_save_env(tmp_path)) as s:
        _open_chat_with_turn(s, "docked")
        before = sorted(p.name for p in tmp_path.iterdir())
        s.key(F2, delay=1.0)
        asking = "\n".join(s.display())
        s.key(ALT_NAME, delay=0.4)
        s.key("never.txt", delay=0.4)
        s.key("\033", delay=1.0)
        s.key("\033", delay=1.0)
        after = "\n".join(s.display())
        files = sorted(p.name for p in tmp_path.iterdir())
    assert SAVE_TITLE in asking, asking
    assert SAVE_TITLE not in after, "Esc did not close the dialog:\n" + after
    assert files == before, "Esc wrote a file: %r -> %r" % (before, files)


# ---- the size of a request ------------------------------------------------

PROMPT_MAX = 32768                      # AI_PROMPT_MAX in we_ai_ui.c
LIMIT_WORDS = "Input at its limit (%d bytes)" % PROMPT_MAX
PASTE_BEG = "\033[200~"
PASTE_END = "\033[201~"
PASTE_CHUNK = 512                       # bytes per pty write, so the pty keeps up
ALT_MULTILINE = "\033m"                 # the prompt popup's Multi-line button
COMPOSER_HINT = "[Esc = done"           # the composer window's title
# The trace events a multi-line value ends at.
TRACE_HEADS = ("chat ", "stream ", "edit ", "agent ", "session ", "mock ",
               "ai ", "tool ", "prompt ", "project ", "plan ", "policy ",
               "menu ", "input ", "pane ", "transcript ", "host ")


def _paste(s, text, settle=2.0):
    """Send ``text`` as one bracketed terminal paste."""
    data = PASTE_BEG + text + PASTE_END
    for i in range(0, len(data), PASTE_CHUNK):
        os.write(s.master_fd, data[i:i + PASTE_CHUNK].encode())
        s._drain(0.15)
    s._drain(settle)


def _lines_payload(nbytes):
    """``nbytes`` of numbered 100-byte lines joined by line breaks, no final
    break: the text a user copies from a file or a log."""
    lines = []
    total = 0
    i = 0
    while total < nbytes:
        line = ("L%05d" % i + "x" * 100)[:100]
        lines.append(line)
        total += len(line) + 1
        i += 1
    return "\n".join(lines)[:nbytes]


def _traced_value(tmp_path, head, nth=0):
    """The value of the ``nth`` trace event starting ``head``, its
    continuation lines included; None when there is no such event."""
    text = _trace(tmp_path)
    found = -1
    out = None
    for line in text.split("\n"):
        if out is not None and line.startswith(TRACE_HEADS):
            if found == nth:
                break
            out = None
        if line.startswith(head):
            found += 1
            out = [line[len(head):]]
        elif out is not None:
            out.append(line)
    if found < nth or out is None:
        return None
    return "\n".join(out).rstrip("\n")


def _unescape_trace(text):
    """Undo the trace's one-line escaping ("\\\\" and "\\n")."""
    out = []
    i = 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            out.append("\n" if text[i + 1] == "n" else text[i + 1])
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def _mock_request(tmp_path, nth=0):
    """The last user message of the ``nth`` request the mock backend got."""
    value = _traced_value(tmp_path, "mock request ", nth)
    if value is None:
        return None
    return _unescape_trace(value.split("last=", 1)[1])


def _wait_traced(tmp_path, needle, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        if needle in _trace(tmp_path):
            return True
        time.sleep(0.2)
    return False


def _open_edit_prompt(s):
    s.key(ALT.AI)
    s.key("e", delay=0.8)


# @scenario SC-ai-chat-62
def test_a_30000_byte_paste_into_the_input_row_is_sent_whole(tmp_path):
    """A 30000-byte paste lands whole in the input row and is sent whole.

    The row is painted once, when the paste ends (one "input paste painted
    once" line in the trace), not after every byte; nothing is refused, and
    Enter sends the 30000 bytes: the chat prompt and the request the mock
    backend receives both hold the whole text.
    """
    text = _lines_payload(30000)
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        s.key(ALT.AI)
        s.key("a", delay=0.8)
        _paste(s, text)
        status = s.display()[-1]
        s.key("\r", delay=1.0)
        assert _wait_traced(tmp_path, "chat done"), _trace(tmp_path)[-2000:]
    trace = _trace(tmp_path)
    assert LIMIT_WORDS not in status, "a paste under the limit was refused: %r" % status
    assert trace.count("input paste painted once") == 1, \
        "the paste was not painted exactly once at its end"
    sent = _traced_value(tmp_path, "chat prompt=")
    assert sent == text, "the chat sent %s of %d bytes" % (
        None if sent is None else len(sent), len(text))
    assert _mock_request(tmp_path) == text, "the request did not carry the whole paste"


# @scenario SC-ai-chat-62
def test_the_byte_past_32768_is_refused_with_the_limit_words(tmp_path):
    """A 32769-byte paste keeps 32768 bytes and says "Input at its limit (32768 bytes)"."""
    text = "a" * PROMPT_MAX + "Z"
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        s.key(ALT.AI)
        s.key("a", delay=0.8)
        _paste(s, text)
        status = s.display()[-1]
        s.key("\r", delay=1.0)
        assert _wait_traced(tmp_path, "chat done"), _trace(tmp_path)[-2000:]
    assert LIMIT_WORDS in status, "the status row does not say the limit: %r" % status
    sent = _traced_value(tmp_path, "chat prompt=")
    assert sent == "a" * PROMPT_MAX, "the chat sent %s bytes, the last %r" % (
        None if sent is None else len(sent), None if sent is None else sent[-3:])


# @scenario SC-ai-chat-63
def test_the_composer_sends_a_5000_byte_paste_whole(tmp_path):
    """The composer sends exactly the 5000 pasted bytes it shows."""
    text = _lines_payload(5000)
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _open_edit_prompt(s)
        s.key(ALT_MULTILINE, delay=1.2)
        _paste(s, text)
        s.key("\033", delay=1.5)               # Esc: done
        s.key("y", delay=2.0)
        assert _wait_traced(tmp_path, "edit instr="), _trace(tmp_path)[-2000:]
    sent = _traced_value(tmp_path, "edit instr=")
    assert sent == text, "the composer sent %s of %d bytes" % (
        None if sent is None else len(sent), len(text))


# @scenario SC-ai-chat-63
def test_the_composer_refuses_the_byte_past_the_limit(tmp_path):
    """Past 32768 bytes the composer refuses with the input row's words.

    The status row says "Input at its limit (32768 bytes)" while the paste
    ends, and what is sent is the 32768 bytes the composer shows, never a
    shorter cut of them.
    """
    text = "a" * PROMPT_MAX + "Z"
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _open_edit_prompt(s)
        s.key(ALT_MULTILINE, delay=1.2)
        _paste(s, text, settle=4.0)
        status = s.display()[-1]
        s.key("\033", delay=1.5)
        s.key("y", delay=2.0)
        assert _wait_traced(tmp_path, "edit instr="), _trace(tmp_path)[-2000:]
    assert LIMIT_WORDS in status, "the composer did not say the limit: %r" % status
    sent = _traced_value(tmp_path, "edit instr=")
    assert sent == "a" * PROMPT_MAX, "the composer sent %s bytes" % (
        None if sent is None else len(sent))


# @scenario SC-ai-chat-63
def test_a_composer_seed_over_16000_bytes_is_sent_whole(tmp_path):
    """A 20000-byte prompt carried into the composer is sent whole.

    The composer's text is a request, not file context: it is not cut at the
    16000-byte context cap, so the 20000 bytes typed in the prompt field and
    carried over by Multi-line are the bytes sent.
    """
    text = "s" * 20000
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _open_edit_prompt(s)
        _paste(s, text)
        s.key(ALT_MULTILINE, delay=1.5)
        s.key("\033", delay=1.5)
        s.key("y", delay=2.0)
        assert _wait_traced(tmp_path, "edit instr="), _trace(tmp_path)[-2000:]
    sent = _traced_value(tmp_path, "edit instr=")
    assert sent == text, "the composer sent %s of %d bytes" % (
        None if sent is None else len(sent), len(text))


CTRL_K = "\x0b"
CTRL_PGUP = "\033[5;5~"                 # start of the buffer
CTRL_PGDN = "\033[6;5~"                 # end of the buffer


# @scenario SC-ai-chat-63
def test_a_block_pasted_whole_past_the_limit_is_caught_before_the_composer_closes(tmp_path):
    """A block duplicated with Ctrl-K C arrives in one piece and can take the
    composer's text past the limit, which a typed key cannot.  Esc then asks,
    with the composer still open, whether to go back and shorten it; N gives
    the text up, and nothing is sent."""
    text = "b" * 20000
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _open_edit_prompt(s)
        s.key(ALT_MULTILINE, delay=1.2)
        _paste(s, text, settle=3.0)
        s.key(CTRL_K, "x", delay=0.8)        # mark the whole text as the block
        s.key(CTRL_PGDN, delay=0.5)
        s.key("\r", delay=0.5)               # a new line below it: the cursor leaves the block
        s.key(CTRL_K, "c", delay=2.5)        # duplicate the block there: about 40000 bytes
        s.key("\033", delay=2.0)             # Esc: done, but the text is over
        asked = "\n".join(s.display())
        s.key("n", delay=2.0)
        sent = _traced_value(tmp_path, "edit instr=")
        assert s.alive()
    assert "over the limit (32768)" in asked, \
        "Esc did not ask about the text over the limit:\n" + asked
    assert "shorten" in asked, asked
    assert sent is None, "a text over the limit was sent (%d bytes)" % len(sent)


# @scenario SC-ai-chat-64
def test_a_multi_line_paste_into_the_prompt_field_opens_the_composer(tmp_path):
    """A paste with line breaks into Alt-G e lands in the composer whole.

    The one-line field cannot hold lines, so the paste opens the composer
    with the pasted text as its content; Esc and Y then send every line,
    each with its line break, nothing run together.
    """
    text = _lines_payload(5000)
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _open_edit_prompt(s)
        _paste(s, text, settle=3.0)
        composer = "\n".join(s.display())
        s.key("\033", delay=1.5)
        s.key("y", delay=2.0)
        assert _wait_traced(tmp_path, "edit instr="), _trace(tmp_path)[-2000:]
    assert COMPOSER_HINT in composer, "the paste did not open the composer:\n" + composer
    sent = _traced_value(tmp_path, "edit instr=")
    assert sent == text, "the composer sent %s of %d bytes, lines kept: %s" % (
        None if sent is None else len(sent), len(text),
        None if sent is None else "\n" in sent)


# @scenario SC-ai-chat-64
def test_a_one_line_paste_past_the_field_cap_says_the_limit(tmp_path):
    """A one-line paste past 32768 bytes into Alt-G e says the limit words."""
    text = "f" * PROMPT_MAX + "Z"
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _open_edit_prompt(s)
        _paste(s, text, settle=3.0)
        status = s.display()[-1]
        s.key("\r", delay=2.0)
        assert _wait_traced(tmp_path, "edit instr="), _trace(tmp_path)[-2000:]
    assert LIMIT_WORDS in status, "the field did not say the limit: %r" % status
    sent = _traced_value(tmp_path, "edit instr=")
    assert sent == "f" * PROMPT_MAX, "the field sent %s bytes" % (
        None if sent is None else len(sent))


# @scenario SC-ai-chat-65
def test_a_long_agent_task_is_marked_cut_and_sent_whole(tmp_path):
    """A 1500-byte agent task shows a cut line ending in "..." and is sent whole.

    The pane's task line has room for about 1100 bytes; the saved transcript
    shows that line ending in the mark, while the trace's task and the
    request the mock backend receives hold all 1500 bytes.
    """
    task = "t" * 1499 + "E"
    env = _env(tmp_path)
    env["XWPE_AI_MOCK_REPLY"] = "DONE all good"
    env["XWPE_AI_POLICY"] = "ask"
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI)
        s.key("g", delay=0.8)
        _paste(s, task)
        s.key("\r", delay=2.0)
        assert _wait_traced(tmp_path, "agent task="), _trace(tmp_path)[-2000:]
        s._drain(2.0)
        s.key(ALT.AI)
        s.key("a", delay=0.8)
        s.key(F2, delay=1.0)
        _name_in_dialog(s, "agent.txt")
    lines = _file_lines(tmp_path / "agent.txt")
    shown = [l for l in lines if l.startswith("[agent] task: ")]
    assert shown, "no task line in the transcript: %r" % lines
    assert shown[0].endswith("..."), "the cut task line has no mark: %r" % shown[0][-20:]
    assert "E" not in shown[0], "the task line was not cut"
    assert _traced_value(tmp_path, "agent task=") == task, "the trace lost part of the task"
    request = _mock_request(tmp_path)
    assert request is not None and task in request, "the request did not carry the whole task"
