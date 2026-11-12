"""The buffer owner, driven by lines and compared with a model of it.

``tests/edit_driver.c`` links ``we_buffer.c`` alone, the way ``test_splice``
does, and answers one command per line with the rows, breakpoints, marks and
undo depths it holds.  The walks here send it thousands of operations (a
splice, a delete, a split, a copy, Undo, Redo, new marks and breakpoints)
and an ``OwnerModel`` the same, and compare after each.  The short rows
below must agree exactly; the longer walks list what differs.
"""
import os

import pytest

pytest.importorskip("pyte")

from edit_model import cdriver  # noqa: E402

STEPS = int(os.environ.get("EDIT_MODEL_C_STEPS", "3000"))
AGREE_STEPS = 1500
WIDTHS = (20, 40)


@pytest.fixture(scope="module")
def binary(tmp_path_factory):
    path = cdriver.build(str(tmp_path_factory.mktemp("edit_driver")))
    if path is None:
        pytest.skip("edit_driver is older than its sources and could not be "
                    "compiled again (no C compiler)")
    return path


@pytest.fixture
def drv(binary):
    d = cdriver.Driver(binary)
    yield d
    d.close()


# @scenario SC-edit-model-34
def test_strings_survive_the_driver_protocol():
    """A string with line breaks, blanks and bytes past ASCII is written
    without a blank and read back the same."""
    data = b"a b\n\\ \x00\xff caf\xc3\xa9"
    assert " " not in cdriver.esc(data)
    assert cdriver.unesc(cdriver.esc(data)) == data


# @scenario SC-edit-model-34
def test_a_driver_older_than_its_sources_is_never_used(tmp_path, monkeypatch):
    """A binary made before the owner changed describes an owner that no
    longer exists: it counts as current only when it is at least as new as
    every source it is made of, and a stale one is replaced by a driver
    compiled again, never returned."""
    old = tmp_path / "edit_driver"
    old.write_bytes(b"")
    os.utime(old, (0, 0))
    assert not cdriver.is_current(str(old))
    new = tmp_path / "newer"
    new.write_bytes(b"")
    future = os.path.getmtime(os.path.join(cdriver.TOP, "we_buffer.c")) + 100
    os.utime(new, (future, future))
    assert cdriver.is_current(str(new))
    assert not cdriver.is_current(str(tmp_path / "missing"))
    monkeypatch.setattr(cdriver, "is_current", lambda _path: False)
    rebuilt = cdriver.build(str(tmp_path))
    if rebuilt is None:
        pytest.skip("no C compiler to build the driver again")
    assert os.path.dirname(rebuilt) == str(tmp_path)


# @scenario SC-edit-model-34
def test_the_driver_holds_what_it_was_given_and_says_what_each_command_did(drv):
    drv.run("open 20 10")
    r = drv.run("text one\\ntwo\\nthree\\n")
    assert r.rows == [("H", b"one"), ("H", b"two"), ("H", b"three")] and (r.ud, r.rd) == (0, 0)
    r = drv.run("brp 1")
    assert r.brp == [1]
    r = drv.run("splice 1 0 xx\\n")
    assert r.rows[1] == ("H", b"xx") and r.brp == [2] and (r.ud, r.rd) == (1, 0)
    r = drv.run("undo")
    assert r.rows == [("H", b"one"), ("H", b"two"), ("H", b"three")] and r.brp == [1]
    assert (r.ud, r.rd) == (0, 1)
    assert drv.run("undo").rc == cdriver.RC_NOTHING
    assert drv.run("redo").rc == cdriver.RC_OK
    assert drv.run("delete 9 1").rc == cdriver.RC_RANGE


# @scenario SC-edit-model-34
@pytest.mark.parametrize("width", WIDTHS)
@pytest.mark.parametrize("seed", [1, 2, 3])
def test_the_owner_agrees_with_its_model_over_the_first_steps(binary, seed, width):
    """In the first 1500 operations of a seeded walk the rows with their
    line ends, the breakpoint rows, the marks and the undo depths are the
    model's after every one: a splice wraps as the file reader wraps, a
    row put where a row stood keeps its breakpoint, a breakpoint comes back
    with Undo, one operation is one step."""
    drv = cdriver.Driver(binary)
    try:
        w = cdriver.OwnerWalk(drv, seed, AGREE_STEPS, width).run()
    finally:
        drv.close()
    assert w.findings == [], w.findings[:3]


# @scenario SC-edit-model-34
@pytest.mark.parametrize("seed", [1, 2, 3])
def test_a_long_walk_lists_where_the_owner_and_its_model_part(binary, seed, capsys):
    """The same walk, longer: what differs is listed (the owner and its
    model are written from different texts, so a difference is a question
    for the user, not a failure here).  The driver answers every command."""
    drv = cdriver.Driver(binary)
    try:
        w = cdriver.OwnerWalk(drv, seed, STEPS).run()
        sent = drv.sent
    finally:
        drv.close()
    with capsys.disabled():
        print("\n[owner walk seed %d] %d steps, %d differences" % (seed, STEPS, len(w.findings)))
        for step, cmd, field, mine, theirs in w.findings[:5]:
            print("   step %d %s: %s model %.70r driver %.70r" % (step, cmd[:40], field,
                                                                   str(mine), str(theirs)))
    assert sent > STEPS
    assert all(f[2] in ("text", "breakpoints", "marks", "steps", "rc") for f in w.findings)


# @scenario SC-edit-model-35
@pytest.mark.parametrize("op", [
    "splice 1 1 new\\nrows\\n", "delete 0 2", "blank 1", "split 1 2", "copy 0 2 1",
    "undo", "redo"])
def test_a_refused_allocation_leaves_the_text_whole_or_the_operation_made(drv, op):
    """Memory refused at the k-th owner allocation of an operation, for each
    k until the operation fits: the answer is out-of-memory with the rows,
    breakpoints, marks and undo lists exactly as they were, or the
    operation made; never half of it."""
    drv.run("open 20 10")
    drv.run("text one\\ntwo\\nthree\\nfour\\n")
    drv.run("brp 1 3")
    drv.run("splice 0 0 head\\n")
    drv.run("mark 1 1 3 2")
    if op == "redo":
        drv.run("undo")
    before = drv.run("dump")
    refused = 0
    for k in range(1, 60):
        drv.run("fail %d" % k)
        res = drv.run(op)
        drv.run("fail 0")
        if res.rc == cdriver.RC_NOMEM:
            refused += 1
            same = drv.run("dump")
            assert (same.rows, same.brp, same.marks, same.ud, same.rd) == \
                (before.rows, before.brp, before.marks, before.ud, before.rd), (op, k)
        else:
            assert res.rc == cdriver.RC_OK, (op, k, res.rc)
            break
    else:
        pytest.fail("%s never fitted in 59 allocations" % op)
    assert refused > 0 or op in ("undo", "redo"), \
        "no allocation was refused: the hook is not reached"


# @scenario SC-edit-model-36
def test_a_move_carries_the_breakpoints_of_its_rows_and_a_copy_carries_none(drv):
    """Moving rows to above another row takes their breakpoints with them
    and leaves the breakpoint rows of the rest on their code; a copy of rows
    has none of its own.  (The owner's move makes no record: the editor's
    block command reserves one, so Undo is not asked here.)"""
    drv.run("open 20 10")
    drv.run("text a\\nb\\nc\\nd\\n")
    drv.run("brp 0 2")
    r = drv.run("move 0 2 2")
    assert [d for _e, d in r.rows] == [b"c", b"d", b"a", b"b"] and sorted(r.brp) == [0, 2]
    r = drv.run("copy 0 1 4")
    assert [d for _e, d in r.rows][-1] == b"c" and sorted(r.brp) == [0, 2]


# @scenario SC-edit-model-37
def test_rows_moved_into_a_narrower_buffer_are_wrapped_to_its_width(drv):
    """A line of 60 bytes moved to a buffer 30 wide arrives wrapped at that
    width with every byte and its line break, as a line read from a file."""
    drv.run("open 80 10")
    drv.run("text " + cdriver.esc(b" ".join([b"w%02d" % i for i in range(15)]) + b"\nlast\n"))
    drv.run("open2 30")
    r = drv.run("xmove 0 1 0")
    assert all(len(d) <= 29 for _e, d in r.rows2), [len(d) for _e, d in r.rows2]
    assert b"".join(d for _e, d in r.rows2).startswith(b"w00 w01")
