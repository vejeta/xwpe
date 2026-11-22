"""A wait is stretched by XWPE_TEST_WAIT_SCALE in one place.

A row delays a mock reply by `5000 * scale` ms and then waits for the screen
that shows the reply.  If the wait itself is not stretched, it ends before
the reply comes and the row fails for the time and not for the editor: the
two assistant rows of the Linux job (the late reply, the edit undone) did,
at scale 3.  wpe_driver.wait_screen and wait_deadline are the one place a
wait's seconds become a deadline; these rows pin that with a clock that
moves only when the driver drains, so they take no real time.
"""
import os
import subprocess
import sys

import pytest

import gui_owner
import wpe_driver

DRAIN_TURN_S = 0.4          # the turn wait_screen drains the editor for
NEVER = "a needle that is never on the screen"


class ClockSession:
    """A session whose clock moves by what the driver drains, and whose
    screen never shows the needle."""

    def __init__(self):
        self.now = 1000.0
        self.waited = 0.0

    def _drain(self, seconds):
        self.now += seconds
        self.waited += seconds

    def display(self, drain=0.6):
        return [""]


@pytest.fixture
def clock(monkeypatch):
    """wpe_driver's clock is the session's: time.time() reads `now`."""
    session = ClockSession()

    class Clock:
        @staticmethod
        def time():
            return session.now

    monkeypatch.setattr(wpe_driver, "time", Clock)
    return session


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
