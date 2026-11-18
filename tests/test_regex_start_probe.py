"""Evidence: what this system's regexec does with the calls Find makes.

This is a probe, not a verdict.  It compiles tests/regex-start-probe.c with
the system C compiler, runs it, and writes its output to
tests/regex-start-probe.log, which the macOS workflow uploads with the other
tests/*.log files.  The row asserts only that the probe ran and printed at
least one line for each shape; it never judges the numbers.  A person reads
the log.

It can be removed once the regex start-context pass lands.
"""
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

TESTS = Path(__file__).resolve().parent
SOURCE = TESTS / "regex-start-probe.c"
LOG = TESTS / "regex-start-probe.log"

# Every shape that must have a line, whichever way \b compiles.
ALWAYS_SHAPES = ("env:", "bracket-nul:", "group-backref:", "unmatched-paren:")
BACKSLASH_SHAPES = ("gt-dot-1:", "gt-dot-2:", "b-a-from1:", "B-a-from1:",
                    "bracket-lf:", "icase-b:")
BRACKET_SHAPES = ("br-gt-dot-1:", "br-gt-dot-2:", "br-lt-a-from1:",
                  "br-bracket-lf:", "br-icase:")


# @scenario SC-regex-start-context-probe
def test_regex_start_probe_runs_and_logs():
    """Run the probe and keep its output in tests/regex-start-probe.log."""
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        pytest.skip("no C compiler (cc, gcc or clang) on this host")
    work = Path(tempfile.mkdtemp(prefix="rxprobe."))
    try:
        exe = work / "regex-start-probe"
        build = subprocess.run([cc, "-o", str(exe), str(SOURCE)],
                               capture_output=True, text=True, timeout=120)
        assert build.returncode == 0, build.stderr
        run = subprocess.run([str(exe)], capture_output=True, text=True,
                             timeout=60)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    LOG.write_text(run.stdout + run.stderr)
    assert run.returncode == 0, run.stdout + run.stderr
    lines = run.stdout.splitlines()
    assert lines and lines[-1] == "done", run.stdout
    for shape in ALWAYS_SHAPES:
        assert any(ln.startswith(shape) for ln in lines), shape
    group = BACKSLASH_SHAPES if "backslash-b compiles=yes" in run.stdout \
        else BRACKET_SHAPES
    for shape in group:
        assert any(ln.startswith(shape) for ln in lines), shape
