"""The Options menu offers the assistant's row exactly when the build has it.

The menu table holds the row in every build (so the key reference and the row
counts read the same everywhere); the editor decides what to show from the one
fact configure gave it.  A build without the assistant must not offer an AI
row that opens nothing, and a build with it must.  The expectation is read from
the binary's own account of what it contains (`--version`), so the same row
runs on every configure and fails when the editor's fact and the build say
different things.
"""
import re

from build_config import built
from wpe_driver import WpeSession, ALT

SEED = "int main(void){return 0;}\n"
# A dropdown row, between the frame's vertical bars, as the screen dump shows it.
AI_ROW = re.compile(r"x AI +x")
FIRST_ROW = "Adjust Colors"


# @scenario SC-config-03b
def test_options_menu_shows_the_ai_row_exactly_when_the_assistant_is_built(tmp_path):
    with WpeSession(str(tmp_path), SEED) as s:
        s.key(ALT.OPTIONS, delay=0.6)
        disp = s.display()
        joined = "\n".join(disp)
        assert FIRST_ROW in joined, "the Options menu did not open:\n" + joined
        shown = any(AI_ROW.search(line) for line in disp)
    assert shown == built("enable-ai"), (
        "the Options menu %s an AI row but the build says enable-ai=%s:\n%s"
        % ("shows" if shown else "has no", "yes" if built("enable-ai") else "no", joined))
