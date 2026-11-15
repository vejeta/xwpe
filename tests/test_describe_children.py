"""procinfo.describe_children: the listing a failed check shows when it did not
find the child of the editor it looked for.

It must say what the process table holds below a process (pid, parent, state,
the name the helpers conclude, the program it was executed as, what ps prints),
and it must never print more of a command line than the program's own name,
nor any of its environment.
"""
import os
import subprocess

import procinfo

SLEEP_S = "30"
SECRET = "describe-children-secret-value"


def test_the_listing_names_a_child_and_shows_nothing_of_its_arguments_or_environment():
    env = dict(os.environ, DESCRIBE_CHILDREN_SECRET=SECRET)
    child = subprocess.Popen(["sleep", SLEEP_S, "--"], env=env)
    try:
        listing = procinfo.describe_children(os.getpid())
    finally:
        child.kill()
        child.wait()
    assert "pid=%d ppid=%d" % (child.pid, os.getpid()) in listing, listing
    assert "argv0='sleep'" in listing, listing
    assert "run_as=" in listing and "state=" in listing, listing
    assert SLEEP_S not in listing.split("argv0=", 1)[1].split("ps_comm", 1)[0], \
        "the arguments of the command line must not be shown:\n" + listing
    assert SECRET not in listing, "the environment must never be shown"


def test_a_process_with_no_children_says_so():
    listing = procinfo.describe_children(2 ** 22 + 1)
    assert listing.startswith("descendants of"), listing
    assert ": 0" in listing.splitlines()[0], listing
