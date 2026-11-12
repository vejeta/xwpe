"""The model's self-check as a stateful Hypothesis machine.

Hypothesis draws sequences of the model's own actions (typing, Enter, the
deletions, blocks, the clipboard, Change All, Undo and Redo) and after each
one the rules the harness relies on must hold: Ctrl-U gives back the text
before a changing action and Ctrl-R the text after it, one changing action is
one undo step, and the cursor and the marks stay inside the text.

Hypothesis is a test dependency, not a build one: without it the machine is
not defined, its one configuration row skips naming what it needs, and
``tests/run-tests.sh`` stays green.  The settings are the gate's: no
randomness (derandomize), no deadline, no example database.
"""
import pytest

pytest.importorskip("pyte")

from edit_model import model  # noqa: E402

try:
    from hypothesis import HealthCheck, settings
    from hypothesis import strategies as st
    from hypothesis.stateful import RuleBasedStateMachine, invariant, rule
    HAVE_HYPOTHESIS = True
except ImportError:
    HAVE_HYPOTHESIS = False

A = model.Action
WIDTH = 40
SEED_ROWS = [("H", b"int main(void)"), ("H", b"{"), ("H", b"\tif (a) {"),
             ("H", b"    int a = 1;"), ("H", b""), ("H", "café au lait".encode()),
             ("H", b" ".join(b"w%03d" % i for i in range(12)) + b"xy"), ("H", b"last")]
MAX_EXAMPLES = 25
STEP_COUNT = 30


def _fresh(numundo):
    s = model.State()
    for end, data in SEED_ROWS:
        s.rows.append(model.Row(data, end, s.next_ident))
        s.next_ident += 1
    s.width, s.flg = WIDTH, model.FLG_AUTOINDENT
    s.options.numundo = numundo
    s.mb = s.me = (len(SEED_ROWS) - 1, len(SEED_ROWS[-1][1]))
    return s


if HAVE_HYPOTHESIS:
    TYPED = st.sampled_from([b"x", b" ", b"-", b"{", "é".encode()])

    class EditMachine(RuleBasedStateMachine):
        """One window of the model, driven by drawn actions."""

        def __init__(self):
            super().__init__()
            self.st = _fresh(10)

        def _act(self, action):
            before = self.st
            after = model.apply(before, action)
            changed = after.lines() != before.lines()
            if changed and action.name not in ("undo", "redo"):
                assert after.ud == min(before.ud + 1, before.options.numundo + 1) \
                    or action.name == "type", (action, "one change is one step")
                back = model.apply(after, A("undo"))
                assert [(r.data, r.end, r.ident) for r in back.rows] == \
                    [(r.data, r.end, r.ident) for r in before.rows], action
                assert model.apply(back, A("redo")).lines() == after.lines(), action
            self.st = after

        @rule(data=TYPED)
        def type_bytes(self, data):
            self._act(A("type", data))

        @rule(name=st.sampled_from(["enter", "tab", "backspace", "delete", "delete_word",
                                    "kill_to_eol", "delete_line"]))
        def edit(self, name):
            self._act(A(name))

        @rule(name=st.sampled_from(["left", "right", "up", "down", "home", "end",
                                    "file_start", "file_end", "word_left", "word_right"]))
        def move(self, name):
            self._act(A(name))

        @rule(name=st.sampled_from(["block_begin", "block_end", "block_line", "block_all",
                                    "block_hide", "block_goto_begin", "block_goto_end"]))
        def mark(self, name):
            self._act(A(name))

        @rule(name=st.sampled_from(["block_copy", "block_move", "block_delete",
                                    "block_indent", "block_unindent", "edit_copy",
                                    "edit_cut", "edit_paste", "help_button"]))
        def block(self, name):
            self._act(A(name))

        @rule(search=st.sampled_from([b"a", b"e", b"line", b"w0"]),
              repl=st.sampled_from([b"ZZ", b"Q", b"##"]))
        def replace(self, search, repl):
            self._act(A("replace_all", (search, repl)))

        @rule(name=st.sampled_from(["undo", "redo"]))
        def history(self, name):
            self._act(A(name))

        @invariant()
        def marks_and_cursor_are_inside_the_text(self):
            rows = self.st.rows
            s = self.st
            assert 0 <= s.y < len(rows) and 0 <= s.x <= len(rows[s.y].data)
            for y, x in (s.mb, s.me):
                assert 0 <= y < len(rows) and 0 <= x <= len(rows[y].data) + 1

    EditMachine.TestCase.settings = settings(
        derandomize=True, deadline=None, database=None, max_examples=MAX_EXAMPLES,
        stateful_step_count=STEP_COUNT, suppress_health_check=list(HealthCheck))
    TestEditMachine = EditMachine.TestCase


# @scenario SC-edit-model-38
def test_the_machine_is_configured_for_the_gate():
    """The settings are fixed: the same machines every run, no database
    written next to the tests, no timing.  Needs hypothesis."""
    if not HAVE_HYPOTHESIS:
        pytest.skip("hypothesis is not installed (python3-hypothesis)")
    s = EditMachine.TestCase.settings
    assert s.derandomize and s.deadline is None and s.database is None
    assert s.stateful_step_count == STEP_COUNT and s.max_examples == MAX_EXAMPLES
