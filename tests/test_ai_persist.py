"""AI working mode persists automatically, and the quick toggle is discreet.

Cycling the permission level (Alt-G y) must:
  - flash a one-shot status-line confirmation that shows all three levels
    (ask / edits / auto) with the ACTIVE one highlighted in a distinct colour
    (so you can see which is selected, and the highlight moves as you cycle),
    plus a short plain-language description of what the active level does,
  - not pop the AI pane up for a settings change,
  - not nag about "Save Options",
  - stick across sessions on its own (no manual save).

HOME is redirected to a tmp dir so the config is isolated.
"""
import os
import subprocess
import tempfile

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

GEAR = "⚙"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _word_col(row, word):
    """Column of `word` as a standalone token in the bar row (space-delimited)."""
    i = row.find(" " + word + " ")
    return i + 1 if i >= 0 else row.find(word)


def _fg(s, y, x):
    return s.screen.buffer[y][x].fg


def test_policy_toggle_is_quiet_and_persists(tmp_path):
    home = tempfile.mkdtemp()
    work = str(tmp_path)
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "HOME": home}
    bottom = None  # last screen row index

    # session 1: cycle ask -> edits from the menu
    with WpeSession(work, "int main(void){return 0;}\n",
                    env_extra=dict(env), filename="t.c") as s:
        bottom = s.screen.lines - 1
        s.key(ALT.AI); s._drain(0.4)
        s.key("y"); s._drain(0.6)
        disp = s.display()
        bar = disp[-1]
        # the flash shows ALL THREE levels and describes the active one, so the
        # user can see what is selected and what it means -- no bare "edits".
        for w in ("Permissions", "ask", "edits", "auto"):
            assert w in bar, "flash is missing %r:\n%s" % (w, "\n".join(disp))
        assert "auto-accept edits, ask before commands" in bar, \
            "flash does not explain what 'edits' does:\n" + "\n".join(disp)
        # the ACTIVE level (edits) is coloured differently from the inactive ones
        c_ask   = _fg(s, bottom, _word_col(bar, "ask"))
        c_edits = _fg(s, bottom, _word_col(bar, "edits"))
        c_auto  = _fg(s, bottom, _word_col(bar, "auto"))
        assert c_edits != c_ask and c_edits != c_auto, (
            "active level 'edits' is not highlighted (ask=%r edits=%r auto=%r):\n%s"
            % (c_ask, c_edits, c_auto, "\n".join(disp)))
        # quiet: no "Save Options" nag, no AI pane, no modal box
        assert not any("Save Options" in r for r in disp), \
            "the toggle still nags about Save Options:\n" + "\n".join(disp)
        assert not any(GEAR in r for r in disp), \
            "the toggle opened the AI pane (should be a status-line flash):\n" + "\n".join(disp)

        # cycling again moves the highlight edits -> auto (the colour follows)
        s.key(ALT.AI); s._drain(0.4)
        s.key("y"); s._drain(0.6)
        disp = s.display(); bar = disp[-1]
        assert "run edits and commands unattended" in bar, \
            "second toggle did not advance to 'auto':\n" + "\n".join(disp)
        assert _fg(s, bottom, _word_col(bar, "auto")) != _fg(s, bottom, _word_col(bar, "edits")), \
            "the highlight did not move to 'auto':\n" + "\n".join(disp)

        # the next keystroke restores the key hints
        s.key("\033[B"); s._drain(0.4)
        assert "F1 Help" in s.display()[-1], \
            "the flash did not clear on the next key:\n" + "\n".join(s.display())

    # session 2: same HOME -> the change stuck without a manual save.  auto was
    # the last level set, so the menu row must show it.
    with WpeSession(work, "int main(void){return 0;}\n",
                    env_extra=dict(env), filename="t.c") as s:
        s.key(ALT.AI); s._drain(0.5)
        disp = "\n".join(s.display())
    assert "Permissions: auto" in disp, \
        "the permission level did not persist across sessions:\n" + disp
