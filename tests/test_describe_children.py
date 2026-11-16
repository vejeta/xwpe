"""procinfo.describe_children: the listing a failed check shows when it did not
find the child of the editor it looked for.

It must say which program was asked for (and the file that came to), what the
process table holds below a process (pid, parent, state, the executable, the
script, whether the child is the program asked for and why not, what ps
prints), and it must never print more of a command line than the program's
own name, nor any of its environment.
"""
import inspect
import os
import shutil
import subprocess
import time

import procinfo

STARTED_WAIT_S = 5
STARTED_POLL_S = 0.02

# A time no pid, path or state letter can hold: a decimal point and digits
# in an order only this argument has.
SLEEP_S = "31.7311"
SECRET = "describe-children-secret-value"


def _sleep_child(env=None):
    """A sleeping child, returned once the process table shows its command
    line: for an instant after exec it shows an empty one."""
    child = subprocess.Popen(["sleep", SLEEP_S], env=env)
    deadline = time.time() + STARTED_WAIT_S
    facts = procinfo.child_facts(child.pid)
    while not (facts and facts.argv) and time.time() < deadline:
        time.sleep(STARTED_POLL_S)
        facts = procinfo.child_facts(child.pid)
    return child


def _child_line(listing, pid):
    for line in listing.splitlines():
        if line.startswith("pid=%d " % pid):
            return line
    raise AssertionError("no line for pid %d:\n%s" % (pid, listing))


def test_the_listing_names_a_child_and_shows_nothing_of_its_arguments_or_environment():
    env = dict(os.environ, DESCRIBE_CHILDREN_SECRET=SECRET)
    child = _sleep_child(env)
    try:
        listing = procinfo.describe_children(os.getpid())
    finally:
        child.kill()
        child.wait()
    assert "pid=%d ppid=%d" % (child.pid, os.getpid()) in listing, listing
    assert "argv0='sleep'" in listing, listing
    assert "exe=" in listing and "state=" in listing, listing
    line = _child_line(listing, child.pid)
    assert SLEEP_S not in line, \
        "the arguments of the command line must not be shown:\n" + listing
    assert SECRET not in listing, "the environment must never be shown"


def test_a_process_with_no_children_says_so():
    listing = procinfo.describe_children(2 ** 22 + 1)
    lines = listing.splitlines()
    assert lines[0] == "asked: nothing yet", listing
    assert lines[1].startswith("descendants of"), listing
    assert ": 0" in lines[1], listing


def test_the_listing_names_the_asked_program_beside_each_child():
    asked = procinfo.program("sleep")
    child = _sleep_child()
    try:
        listing = procinfo.describe_children(os.getpid(), asked)
    finally:
        child.kill()
        child.wait()
    real = os.path.realpath(shutil.which("sleep"))
    assert "asked: sleep -> exe " + real in listing.splitlines(), listing
    line = _child_line(listing, child.pid)
    assert "exe=%r" % real in line, line
    assert line.endswith("runs_asked=yes"), line


def test_the_listing_says_why_a_child_is_not_the_asked_program(tmp_path):
    asked = procinfo.program("nosuch", path=str(tmp_path))
    child = _sleep_child()
    try:
        listing = procinfo.describe_children(os.getpid(), asked)
    finally:
        child.kill()
        child.wait()
    assert "asked: nosuch -> not on the editor's PATH" in listing, listing
    assert "runs_asked=no: asked program not on PATH" in \
        _child_line(listing, child.pid), listing


def test_the_listing_reads_no_environment(monkeypatch):
    """The environment of a process is never read: no ps argv built while
    listing carries an option that prints it, and no owner function names
    one."""
    seen = []
    real_run = subprocess.run

    def recording_run(argv, *args, **kwargs):
        seen.append(list(argv))
        return real_run(argv, *args, **kwargs)

    monkeypatch.setattr(procinfo.subprocess, "run", recording_run)
    monkeypatch.setattr(procinfo, "has_procfs", lambda: False)
    asked = procinfo.program("sleep")
    child = _sleep_child(dict(os.environ, DESCRIBE_CHILDREN_SECRET=SECRET))
    try:
        listing = procinfo.describe_children(os.getpid(), asked)
        procinfo.children_running(os.getpid(), asked)
    finally:
        child.kill()
        child.wait()
    assert SECRET not in listing
    ps_calls = [argv for argv in seen if argv and argv[0] == "ps"]
    assert ps_calls, "the listing read no ps table: this row proves nothing"
    for argv in ps_calls:
        for option in procinfo.PS_FORBIDDEN_OPTIONS:
            assert option not in argv, argv
    for owner in (procinfo.describe_children, procinfo.child_facts,
                  procinfo.children_running, procinfo.descendants_running,
                  procinfo._exe_of_pid, procinfo._lsof_name):
        source = inspect.getsource(owner)
        for option in procinfo.PS_FORBIDDEN_OPTIONS:
            assert '"%s"' % option not in source, owner.__name__
