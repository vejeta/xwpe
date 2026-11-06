"""The gdb on this machine speaks the MI the editor will drive.

The editor reads gdb's machine interface (we_gdbmi.c) instead of its
sentences.  Before it trusts a gdb with that, it needs to know the gdb is
new enough and answers the few commands a session starts with.  Older gdbs
(and a gdb that is not there) keep the line interface.

The commands go in on gdb's input.  `-batch -ex '-list-features'` does not
work with gdb 18: -ex runs its argument as an ordinary command and gdb
answers 'Undefined command: "-list-features"'.
"""
import re
import shutil
import subprocess

import pytest

# The oldest gdb the editor drives over MI: fullname in frames,
# -inferior-tty-set, -break-insert -f and =breakpoint-modified all exist
# from here on.
MI_FLOOR = (7, 6)

# What -list-features must name for the editor's session to work.
FEATURES_THE_SESSION_NEEDS = ("pending-breakpoints", "breakpoint-notifications")

# The commands a session starts with, each answered ^done.
START_COMMANDS = ("-gdb-set confirm off", "-gdb-set mi-async off",
                  "-inferior-tty-set /dev/null")

GDB_TIMEOUT_S = 30
PROBE_FIRST_TOKEN = 1


def _need_gdb():
    if shutil.which("gdb") is None:
        pytest.skip("gdb is not installed: nothing to probe")


def _probe_input():
    """The probe's commands, numbered from PROBE_FIRST_TOKEN, ending gdb."""
    commands = ["-list-features", "-gdb-version"] + list(START_COMMANDS)
    commands.append("-gdb-exit")
    return "".join("%d%s\n" % (PROBE_FIRST_TOKEN + k, c)
                   for k, c in enumerate(commands))


def _ask_gdb():
    """Everything gdb --interpreter=mi2 printed for the probe's commands."""
    done = subprocess.run(["gdb", "--interpreter=mi2", "-nx", "-q"],
                          input=_probe_input(), capture_output=True,
                          text=True, timeout=GDB_TIMEOUT_S)
    return done.stdout


def _answer_to(output, token):
    """The result record that carries @token, or None."""
    for line in output.splitlines():
        if re.match(r"%d\^" % token, line):
            return line
    return None


def _banner(output):
    """The first line of -gdb-version's console records, decoded."""
    for line in output.splitlines():
        found = re.match(r'~"(GNU gdb[^"]*?)\\n"', line)
        if found:
            return found.group(1)
    return None


def _version_of(banner):
    """(major, minor) from "GNU gdb (Debian 18.1-3) 18.1": the last number
    pair on the line, which is gdb's own version whatever the vendor wrote
    in the parentheses."""
    pairs = re.findall(r"\b(\d+)\.(\d+)", banner)
    return (int(pairs[-1][0]), int(pairs[-1][1]))


# @scenario SC-debug-63
def test_gdb_speaks_the_mi_the_editor_will_drive():
    """The gdb is at or above the MI floor, names the features a session
    needs, and answers the start-up commands with ^done."""
    _need_gdb()
    output = _ask_gdb()

    banner = _banner(output)
    assert banner, "no version banner in:\n" + output
    assert _version_of(banner) >= MI_FLOOR, banner

    features = _answer_to(output, PROBE_FIRST_TOKEN)
    assert features and features.startswith("1^done,features=["), output
    for name in FEATURES_THE_SESSION_NEEDS:
        assert '"%s"' % name in features, name + " missing from " + features

    first_start = PROBE_FIRST_TOKEN + 2
    for k, command in enumerate(START_COMMANDS):
        answer = _answer_to(output, first_start + k)
        assert answer == "%d^done" % (first_start + k), (command, answer)
