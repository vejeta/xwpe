"""Regression: an LSP edit must NOT collapse the file's blank lines (wpe).

Applying a code action / format / rename rebuilds the whole editor buffer via
e_lsp_replace_buffer -> e_buffer_set_text.  That rebuild used to delegate each
split line to print_to_end_of_buffer, which DELIBERATELY drops blank lines (the
right thing for compiler-output appends, the wrong thing for a buffer rebuild):
every empty line in the user's file vanished, and because whole-buffer Undo
rebuilds through the same path, Ctrl-U did not bring them back.

This drives the real editor through clangd's formatter (fast, no JVM): the input
is misindented so clang-format rewrites it -- forcing the rebuild -- while the
single blank line between the two functions is preserved by clang-format
(MaxEmptyLinesToKeep defaults to 1).  After the format, that blank line must
still be there.

The buffer-rebuild logic is shared with rename/code-action (the user hit it via
Alt-Q A on Scala); clangd's format is just the cheapest deterministic trigger.

Self-skips when clangd is absent OR when the Alt-Q format did not actually
rewrite the buffer in this environment (so it asserts only when the rebuild path
really ran -- it never goes spuriously red).  The buffer-rebuild logic itself is
proven independently by the e_buffer_set_text round-trip red-green.
"""
import shutil
import time

import pytest

from wpe_driver import WpeSession

# Misindented on purpose (the `return`s sit in column 0) so clang-format changes
# the text and the buffer is rebuilt; the ONE blank line between alpha and beta
# is preserved by clang-format, so it is a clean probe for the drop-blank bug.
PROG = (
    "int alpha(void) {\n"
    "return 11;\n"
    "}\n"
    "\n"
    "int beta(void) {\n"
    "return 22;\n"
    "}\n"
)

ALT_Q = "\033q"     # the LSP prefix (Alt-Q)

pytestmark = pytest.mark.skipif(
    shutil.which("clangd") is None, reason="clangd required")


def _editor_rows(lines):
    """The editor text columns, stripped of the window frame (left border in
    column 0, scrollbar in the last column), so a blank editor line reads as ''."""
    return [l[1:-1].rstrip() if len(l) > 2 else "" for l in lines]


def test_clangd_format_keeps_blank_lines(tmp_path):
    """Alt-Q F reformats via clangd; the rebuilt buffer keeps the blank line
    between the two functions instead of collapsing them together."""
    with WpeSession(str(tmp_path), PROG, filename="blanks.c", wait=2.0) as w:
        time.sleep(1.5)
        w._drain(1.5)
        w.key(ALT_Q, delay=0.4)
        w.key("f", delay=10.0)            # clangd format + buffer rebuild
        time.sleep(1.5)
        w._drain(2.0)
        assert w.alive(), "wpe died applying the format"

        rows = _editor_rows(w.display())
        # The rebuild path only ran if clang-format actually reindented the
        # bodies.  If it did not engage in this environment, skip rather than
        # fail -- the rebuild logic is covered by the C round-trip test.
        if not any(r == "  return 11;" for r in rows):
            pytest.skip("clangd format did not rewrite the buffer here "
                        "(Alt-Q dispatch / formatter unavailable)")

        body = [i for i, r in enumerate(rows) if r == "  return 11;"]
        beta = [i for i, r in enumerate(rows) if r.startswith("int beta")]
        assert body and beta, "both functions should be in the rebuilt buffer"
        between = rows[body[0] + 1:beta[0]]
        assert any(r == "" for r in between), \
            "the blank line between the two functions was collapsed by the " \
            "buffer rebuild: %r" % rows
