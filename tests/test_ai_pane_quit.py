"""The AI pane is a tool/output window (like Messages), not a document.

The quit-time "save this file?" prompt fires for a window only when
`save != 0 && ins != 8` (we_wind.c).  Messages avoids it by being a tool pane
(ins == 8); the AI pane used to be a plain document, so once its buffer looked
modified, quitting asked to "save AI".  It is now a tool pane too.

That is observable in the title bar: a tool pane wears the gear marker (U+2699),
a document wears none.  Asserting the gear proves ins == 8, which is exactly what
exempts the pane from the save-on-quit prompt.

Red/green: without ins == 8 the pane is a WIN_DOCUMENT and shows no gear.
"""
import os
import subprocess
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


def test_ai_pane_is_a_tool_pane(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": "ok"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("a"); s._drain(0.6)      # open the AI pane
        rows = s.display()
    ai_titles = [r for r in rows if " AI " in r and ("q" in r or "-" in r)]
    assert ai_titles, "AI pane title bar not found:\n" + "\n".join(rows)
    assert any(GEAR in r for r in ai_titles), \
        "AI pane is not marked a tool pane (no gear -> ins != 8 -> would prompt to " \
        "save on quit):\n" + "\n".join(ai_titles)
