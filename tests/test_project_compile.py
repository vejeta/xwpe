"""F9 (Make) on a malformed project must report an error, not crash.

Regression guard for a heap abort found in the field (core dump): e_c_project
allocates its argument vectors with MALLOC (uninitialised) and, when a project
variable such as CMP is missing, bails straight into e_free_arg -- which FREEs
every non-NULL slot.  On a dirty heap slot 0 held a stale pointer, so the free
aborted (glibc "free(): invalid pointer").  The fix zero-initialises slot 0 so
the early-exit free skips it.

This drives F9 on a project whose CMP variable is absent and asserts xwpe stays
alive and shows the error.  Run under MALLOC_PERTURB_ so a reintroduced
uninitialised free is more likely to fault here too; the deterministic proof of
the original crash is the core dump (e_c_project -> e_free_arg), which this path
exercises.
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, WPE_BIN


def _prog_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"No Files to compile" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _prog_build(), reason="wpe built without the programming layer")


def test_f9_on_project_without_cmp_does_not_crash(tmp_path):
    (tmp_path / "main.c").write_text("#include <stdio.h>\nint main(void){return 0;}\n")
    # a .prj with no CMP line: e_p_get_var("CMP") returns NULL -> the early-exit
    # free path that aborted in the field.
    (tmp_path / "p.prj").write_text("CMPFLAGS=\t-g\nEXENAME=\tp\nFILES=\tmain.c\n")
    env = {
        "XWPE_LIB": os.path.abspath(os.path.join(os.path.dirname(WPE_BIN))),
        "MALLOC_PERTURB_": "165",
    }
    with WpeSession(str(tmp_path), (tmp_path / "p.prj").read_text(),
                    env_extra=env, filename="p.prj") as s:
        s._drain(1.0)
        assert s.proc.poll() is None, "xwpe died opening the project"
        s.key("\033[20~", delay=3.0)          # F9 -> Run -> Make
        s._drain(1.5)
        assert s.proc.poll() is None, \
            "xwpe crashed compiling a project with a missing CMP variable"
        disp = "\n".join(s.display())
    assert "compile" in disp.lower() or "error" in disp.lower(), \
        "expected a compile error dialog, not a silent/again state:\n" + disp
