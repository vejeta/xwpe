"""The console test, on a real virtual console (layer: console).

What an ssh session cannot show: on a Linux VT the kernel answers the VT
ioctls, so the editor takes the GPM console path -- which is the path that was
proved by hand on a bare console and must keep working.  On either end of a pty
it does not, so a terminal emulator's own mouse reports are used instead.  Both
halves are asserted here, in one boot of a micro-VM, because a fix that only
ever ran on a pty could not tell them apart.

Scenario ids: SC-dialog-engine-45 (a pty is not the console).
"""

import pytest


VT_LABELS = ["vt0", "vt1", "vt63"]
PTY_LABELS = ["pty_slave", "pty_master"]


def _case(cases, label):
    assert label in cases, (
        "the console probe reported no case for %r; it reported: %s"
        % (label, sorted(cases)))
    return cases[label]


@pytest.mark.parametrize("label", VT_LABELS)
def test_a_real_virtual_console_keeps_the_gpm_path(vt_cases, label):
    """The console that was proved by hand still classifies as a console: the
    kernel answers both VT ioctls, so the GPM path is taken exactly as before.
    """
    c = _case(vt_cases, label)
    assert c["isatty"] == "1", c
    assert c["ttyname"].startswith("/dev/tty"), c
    assert c["kdgetmode"] == "ok", (
        "a real VT must answer KDGETMODE; without it the console mouse would be "
        "handed to the terminal-report path and the bare console would lose the "
        "mouse that was proved working: %s" % c)
    assert c["vtgetstate"] == "ok", c
    assert c["predicate"] == "1", (
        "a real virtual console must take the GPM console path: %s" % c)


@pytest.mark.parametrize("label", PTY_LABELS)
def test_neither_end_of_a_pty_is_a_console(vt_cases, label):
    """A pseudo-terminal gets the terminal's own mouse reports, whatever the
    machine's gpm situation is.  Both ends: the slave is what an editor reads,
    and the master is what a driver or a multiplexer holds, so a test that only
    covered the slave would leave the master free to be mistaken for a console.
    """
    c = _case(vt_cases, label)
    assert c["isatty"] == "1", c
    assert c["kdgetmode"] == "enotty", c
    assert c["vtgetstate"] == "enotty", c
    assert c["predicate"] == "0", (
        "a pty must not take the GPM console path, or its menus wait for a "
        "console event that can never arrive: %s" % c)


def test_the_slave_is_named_as_a_pts(vt_cases):
    c = _case(vt_cases, "pty_slave")
    assert c["ttyname"].startswith("/dev/pts/"), c


def test_the_master_is_named_ptmx(vt_cases):
    c = _case(vt_cases, "pty_master")
    assert c["ttyname"] == "/dev/ptmx", c


def test_a_descriptor_that_is_not_a_terminal_is_not_a_console(vt_cases):
    c = _case(vt_cases, "not_a_tty")
    assert c["isatty"] == "0", c
    assert c["predicate"] == "0", c


@pytest.mark.parametrize("label", ["console", "serial"])
def test_a_serial_console_is_left_as_it_was(vt_cases, label):
    """Pinned on purpose.  A serial line answers neither VT ioctl, and the
    fallback deliberately keeps it on the console path rather than taking a
    mouse away that may well work: gpm can be what serves it.  Only
    pseudo-terminals are excluded by name.  If this ever changes, it is a
    decision about serial consoles, not a side effect of a pty fix.
    """
    c = _case(vt_cases, label)
    assert c["kdgetmode"] == "enotty", c
    assert c["predicate"] == "1", (
        "a serial console changed classification; that is a decision of its "
        "own and must not happen as a side effect: %s" % c)


def test_the_console_test_never_reads_term(predicate_source):
    """The whole defect was a mouse backend chosen by an environment variable:
    Gpm_Open refuses xterm-like TERM values, so xterm escaped and screen, tmux
    and linux did not.  The replacement asks the kernel.  Reading TERM here
    would reintroduce the accident, so the source is asserted, not just the
    behaviour.
    """
    assert "TERM" not in predicate_source, (
        "the console test consults TERM again:\n%s" % predicate_source)
    assert "KDGETMODE" in predicate_source and "VT_GETSTATE" in predicate_source, (
        "the console test no longer asks the kernel:\n%s" % predicate_source)
