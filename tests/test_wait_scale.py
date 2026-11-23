"""A wait is stretched by XWPE_TEST_WAIT_SCALE in one place.

A row delays a mock reply by `5000 * scale` ms and then waits for the screen
that shows the reply.  If the wait itself is not stretched, it ends before
the reply comes and the row fails for the time and not for the editor: the
two assistant rows of the Linux job (the late reply, the edit undone) did,
at scale 3.  wpe_driver.wait_screen and wait_deadline are the one place a
wait's seconds become a deadline; these rows pin that with a clock that
moves only when the driver reads the editor, so they take no real time.
The scale stretches the ceiling of a wait and never the grain of looking,
so a row here also pins the turn wait_until reads for.
"""
import os
import subprocess
import sys

import pytest

import gui_owner
import wpe_driver

DRAIN_TURN_S = 0.4          # the turn wait_until reads the editor for
NEVER = "a needle that is never on the screen"
TRANSIENT = "a needle the screen takes back while the editor works"
# How long the transient needle stays up: longer than one turn of wait_until,
# shorter than one turn stretched by the scale of three the Linux job runs
# at.  A wait that looks with the stretched turn steps over it.
TRANSIENT_SHOWN_S = 0.8
TRANSIENT_SCALE = 3


class ClockSession:
    """A session whose clock moves by what the driver reads, and whose
    screen never shows the needle.

    The driver reads a session two ways: `_read_for(seconds, settle)`, which
    takes the seconds as they are, and `_drain(seconds)`, which stretches
    them by the wait scale first.  Both move this clock by what the real one
    would spend, so a row pins the grain of a wait without waiting."""

    def __init__(self):
        self.now = 1000.0
        self.waited = 0.0

    def _read_for(self, seconds, settle=0.0):
        self.now += seconds
        self.waited += seconds

    def _drain(self, seconds):
        self._read_for(gui_owner.scaled(seconds))

    def display(self, drain=0.6):
        return [""]


class TransientSession(ClockSession):
    """A session whose screen shows one needle for a window of its own clock.

    The window stands for a state the editor paints and takes back while it
    works: the "waiting for the debugger" line, a spinner, a notice that the
    next paint overwrites.  Such a state is seen only by a wait that looks
    often enough."""

    def __init__(self, shown_for):
        ClockSession.__init__(self)
        self.shown_for = shown_for
        self.start = self.now

    def display(self, drain=0.6):
        if self.now - self.start < self.shown_for:
            return [TRANSIENT]
        return [""]


def use_clock(monkeypatch, session):
    """Make wpe_driver's clock the session's: time.time() reads `now`."""
    class Clock:
        @staticmethod
        def time():
            return session.now

    monkeypatch.setattr(wpe_driver, "time", Clock)
    return session


@pytest.fixture
def clock(monkeypatch):
    return use_clock(monkeypatch, ClockSession())


def at_scale(monkeypatch, scale):
    monkeypatch.setenv(gui_owner.WAIT_SCALE_VAR, str(scale))


def test_a_screen_wait_lasts_its_seconds_times_the_scale(monkeypatch, clock):
    at_scale(monkeypatch, 3)
    assert wpe_driver.wait_screen(clock, NEVER, 1.0) is False
    # 3 s are asked for; the last turn may run past the end by one turn
    assert 3.0 <= clock.waited < 3.0 + DRAIN_TURN_S + 1e-9, clock.waited


def test_a_screen_wait_at_scale_one_lasts_its_seconds(monkeypatch, clock):
    at_scale(monkeypatch, 1)
    assert wpe_driver.wait_screen(clock, NEVER, 1.0) is False
    assert 1.0 <= clock.waited < 1.0 + DRAIN_TURN_S + 1e-9, clock.waited


def test_a_state_shorter_than_a_scaled_turn_is_still_seen(monkeypatch):
    """The grain of looking is never stretched, only the ceiling is.

    A state the editor shows for longer than one turn and takes back before
    a stretched turn would end is still seen.  Red when wait_until goes back
    to `_drain(TURN_S)`, whose turn is the scale times longer and so steps
    over the state: the "waiting for gdb" line the deadline rows wait for was
    missed that way at a scale of three."""
    at_scale(monkeypatch, TRANSIENT_SCALE)
    session = use_clock(monkeypatch, TransientSession(TRANSIENT_SHOWN_S))
    turn = wpe_driver.TURN_S
    assert turn < TRANSIENT_SHOWN_S < turn * gui_owner.wait_scale(), \
        "the window must sit between one turn (%s s) and a scaled one" % turn
    assert wpe_driver.wait_until(
        session, lambda: TRANSIENT in "\n".join(session.display(drain=0)),
        8, what="the line the screen takes back") is True


def test_a_deadline_is_its_seconds_times_the_scale_ahead(monkeypatch, clock):
    at_scale(monkeypatch, 2.5)
    assert wpe_driver.wait_deadline(4) == pytest.approx(clock.now + 10.0)


def test_the_drivers_scale_is_the_owners():
    """The module constant the rows multiply their mock delays by is the
    owner's value, so it cannot differ from what the owner stretches the
    waits by (a value the owner refuses is refused at import)."""
    for text, want in (("3", 3.0), ("0.5", 0.5), ("", 1.0)):
        env = dict(os.environ, **{gui_owner.WAIT_SCALE_VAR: text})
        out = subprocess.run(
            [sys.executable, "-c",
             "import gui_owner, wpe_driver;"
             "print(wpe_driver.WAIT_SCALE, gui_owner.wait_scale())"],
            cwd=gui_owner.HERE, env=env, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            timeout=gui_owner.scaled(60))
        assert out.stdout.split() == [str(want), str(want)], out.stdout


def test_a_scale_of_zero_is_refused_where_the_driver_reads_it(monkeypatch):
    at_scale(monkeypatch, 0)
    with pytest.raises(ValueError):
        wpe_driver.wait_deadline(1)
