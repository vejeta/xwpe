"""A build from a long path must not hand the compiler a cut path.

The source path and the object path were each formatted into 128 bytes, so a
file more than 127 characters deep reached the compiler as the piece that fit.
What the user saw was the compiler complaining about a linker input file it
could not find, naming a path ending mid-word: the editor had truncated the
name and the compiler was blamed for it. Paths that long are ordinary under a
cache or a per-session scratch directory, which is where this was found.

The contract is not that every path compiles: a path can always be longer than
any buffer. It is that the editor never substitutes a different file name for
the one it was given, and says so when the name does not fit.
"""
import os
import shutil

import pytest
from wpe_driver import WpeSession

PROG = "int main(void){return 0;}\n"
# Deep enough that dir + name passes 127 characters, in named pieces rather
# than one unreadable string, and short enough to stay well inside PATH_MAX.
DEEP = os.path.join(*["a_directory_name_of_some_length"] * 4)
CUT_PATH_ERROR = "linker input file not found"
F9 = "\033[20~"          # Make


@pytest.mark.skipif(shutil.which("gcc") is None, reason="needs gcc")
def test_a_build_from_a_long_path_does_not_report_a_cut_path(tmp_path):
    deep = tmp_path / DEEP
    deep.mkdir(parents=True)
    assert len(str(deep)) > 127, \
        "the test's own path is too short to reach the old limit: %d" % len(str(deep))

    with WpeSession(str(deep), PROG, filename="t.c", wait=2.0, cols=200) as w:
        w.key(F9, delay=3.0)
        w._drain(2.0)
        screen = "\n".join(w.display())
        assert w.alive(), "wpe died building from a long path"
        assert CUT_PATH_ERROR not in screen, \
            "the compiler was handed a cut path:\n" + screen
        # The object lands beside the source, which also proves the path that
        # reached the compiler was the real one.
        built = list(deep.glob("t.o")) + list(deep.glob("t"))
        assert built, "nothing was built from a long path:\n" + screen
