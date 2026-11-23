"""A window the editor writes itself has no file behind it (X11 twin of
tests/test_window_no_file.py).

The sentence is on the status row, which a screenshot cannot read; what the
GUI row pins is the effect on the disk: F2 in the assistant's composer writes
nothing under its title, and a file of that name keeps its text, in the X11 and
the Wayland windows as in the terminal.
"""
import os

import pytest

TITLE = "AI agent task  [Esc = done, then Y sends, N cancels]"
AI_ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": "ok"}


def _folder(xwpe):
    return os.path.dirname(xwpe.srcfile)


# @scenario SC-window-no-file-06
@pytest.mark.parametrize("xwpe", [AI_ENV], indirect=True)
def test_f2_in_the_composer_writes_nothing_under_its_title(xwpe):
    """F2 in the composer leaves a file called like its title as it was and
    makes none when there is none."""
    mine = os.path.join(_folder(xwpe), TITLE)
    with open(mine, "w") as fh:
        fh.write("my own text\n")
    xwpe.key("alt+g", delay=0.6)
    xwpe.key("g", delay=0.8)
    xwpe.type("hello")
    xwpe.key("alt+m", delay=1.0)
    xwpe.key("F2", delay=1.0)
    assert xwpe.proc.poll() is None, "xwpe died"
    with open(mine) as fh:
        assert fh.read() == "my own text\n"
    assert not os.path.exists(mine + ".bak")
