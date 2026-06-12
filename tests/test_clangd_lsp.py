"""C IDE features via the LSP bridge (clangd), in wpe.

Drives the editor's LSP entry point for a C file: opening a .c is recognised as
an LSP-backed language (the status bar advertises "clangd"), and Alt-Q (the
"Query the language server" prefix) then E starts clangd and surfaces compiler
diagnostics in Messages.

This is the EDITOR-bridge counterpart to tests/test_lsp_clangd.c: the C test
exercises the protocol engine (we_lsp.c) directly; this one checks the glue in
we_debug.c that the engine test bypasses -- extension -> language detection, the
eager start on open, the status-bar label, and the Alt-Q key dispatch.

clangd needs no JVM and no build server, so unlike the Metals bridge test this
runs in a few seconds -- fast enough for CI where clangd is packaged.  Skips
when clangd is absent.
"""
import shutil
import time

import pytest

from wpe_driver import WpeSession

# A deliberate undeclared identifier on the last line guarantees clangd reports
# at least one diagnostic, so the assertion is deterministic.
PROG = (
    "#include <stdio.h>\n"
    "\n"
    "int add(int a, int b) { return a + b; }\n"
    "\n"
    "int main(void) {\n"
    "    int total = add(2, 3);\n"
    "    printf(\"%d\\n\", total);\n"
    "    return bad_symbol;\n"          # undeclared -> a clangd diagnostic
    "}\n"
)

ALT_Q = "\033q"     # the LSP prefix (Alt-Q)
ALT_S = "\033s"     # Search menu (for Find -- arrow keys are not decoded under pyte)
LOCK = "\U0001f512" # the read-only padlock glyph

pytestmark = pytest.mark.skipif(
    shutil.which("clangd") is None, reason="clangd required")


def _text(w):
    return "\n".join(w.display())


def _find(w, term):
    """Position the cursor on the first match of `term` via Search > Find."""
    w.key(ALT_S, delay=0.6)            # Search menu
    w.key("f", delay=0.6)              # Find -> dialog, focus in the text field
    for ch in term:
        w.key(ch, delay=0.05)
    w.key("\r", delay=1.0)             # OK -> jump to the match


def test_clangd_lsp_bridge(tmp_path):
    """Opening a .c is recognised (the bar names clangd), and Alt-Q E starts
    clangd and surfaces diagnostics -- the editor bridge wired end to end."""
    with WpeSession(str(tmp_path), PROG, filename="demo.c", wait=2.0) as w:
        time.sleep(1.0)
        w._drain(1.0)
        # extension -> language -> server: the bottom bar advertises the server
        # that backs this file (proves e_lsp_lang_for + e_lsp_server_label for C).
        assert "clangd" in _text(w), \
            "status bar should advertise the clangd LSP for a .c file:\n%s" % _text(w)

        # Alt-Q E starts clangd and reports diagnostics (fast: no JVM/build
        # server -- and the eager start already began clangd when we opened).
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=6.0)
        time.sleep(1.5)
        w._drain(2.0)
        assert w.alive(), "wpe died starting clangd"
        text = _text(w)
        assert ("LSP:" in text or "error" in text or "Language server" in text), \
            "no clangd diagnostics/status in Messages:\n%s" % text


def test_clangd_definition_into_readonly_header(tmp_path):
    """Alt-Q D on a libc symbol jumps into its system header, opened READ-ONLY.

    /usr/include is not the user's to edit, so the window carries the padlock in
    its title.  This is the clangd-specific navigation path -- a real file locked
    by its own permissions -- distinct from Metals' extracted .metals/readonly
    sources, so it earns its own assertion.
    """
    with WpeSession(str(tmp_path), PROG, filename="demo.c", wait=2.0) as w:
        time.sleep(1.0)
        w._drain(1.0)
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=6.0)              # start clangd
        w._drain(2.0)
        assert w.alive(), "wpe died starting clangd"

        _find(w, "printf")                 # cursor onto the printf call
        w.key(ALT_Q, delay=0.4)
        w.key("d", delay=5.0)              # go to definition -> system header
        w._drain(2.0)
        assert w.alive(), "wpe died on go-to-definition into a system header"

        rows = w.display()
        # the header opened: a title row names the libc header it jumped into...
        header_rows = [r for r in rows if "stdio.h" in r]
        assert header_rows, \
            "Alt-Q D did not open the system header:\n%s" % "\n".join(rows)
        # ...and it is READ-ONLY: the padlock is on that same title row (the
        # Messages pane may carry its own padlock -- match the header's row, not
        # just "a padlock somewhere").
        assert any(LOCK in r for r in header_rows), \
            "the system header was not opened read-only (no padlock):\n%s" \
            % "\n".join(rows)


# A vector + `auto` deduced variable guarantees clangd offers at least one
# inlay hint (the deduced type), so the toggle has something to show.
INLAY_PROG = (
    "#include <vector>\n"
    "\n"
    "int main() {\n"
    "    std::vector<int> v{1, 2, 3};\n"
    "    auto count = v.size();\n"        # inlay -> the deduced type, e.g. : size_type
    "    return (int) count;\n"
    "}\n"
)


def _chip_cells(w):
    """Editor cells drawn in the inlay 'chip' attribute (black on light grey).

    The chip is palette 7-on-0 (Light Gray bg, Black fg); pyte names index 7
    'white'.  Restrict to the editor body (rows 2..18) so the status bar -- which
    also uses light-on-dark chrome -- cannot be mistaken for a hint."""
    out = []
    for y in range(2, 19):
        for x in range(len(w.screen.buffer[y])):
            c = w.screen.buffer[y][x]
            if c.fg == "black" and c.bg == "white" and c.data not in (" ", ""):
                out.append((y, x, c.data))
    return out


def test_clangd_inlay_hint_is_a_distinct_chip(tmp_path):
    """Alt-Q Y renders inlay hints as a distinct black-on-light-grey 'pill'.

    The hint used to be dim cyan on the line's own background, which blended into
    green comments and the Borland-blue editor -- so users could not tell the
    server annotation from the code (it read as invisible).  This asserts the
    chip attribute is actually painted, not just that the toggle fired.

    Green-or-skip: if clangd has not produced a hint yet (cold start), the toggle
    reports 'none' and we skip rather than fail -- the rendering, not the server's
    timing, is under test here."""
    (tmp_path / "compile_flags.txt").write_text("-std=c++17 -Wall\n")
    with WpeSession(str(tmp_path), INLAY_PROG, filename="demo.cpp", wait=2.0) as w:
        time.sleep(1.0)
        w._drain(1.0)
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=8.0)              # start clangd + first diagnostics
        w._drain(2.0)
        assert w.alive(), "wpe died starting clangd"

        w.key(ALT_Q, delay=0.4)
        w.key("y", delay=3.0)              # toggle inlay hints ON
        w._drain(1.5)
        assert w.alive(), "wpe died toggling inlay hints"

        text = _text(w)
        if "Inlay hints: none" in text:
            pytest.skip("clangd returned no inlay hints yet (cold start)")
        assert "Inlay hints: ON" in text, \
            "inlay toggle did not turn on:\n%s" % text

        chip = _chip_cells(w)
        assert chip, \
            "inlay hint was not painted as the black-on-light-grey chip:\n%s" \
            % text


def test_altq_menu_brackets_the_lsp_modal_guard(tmp_path):
    """Opening the Alt-Q action menu suspends the async LSP fd-loop's painting.

    The menu (and every dialog/picker/rename) is an e_opt_kst popup that draws a
    box over the editor but pushes NO window, so e_lsp_ui_safe's window check
    cannot see it.  e_opt_kst therefore brackets its input loop with
    e_lsp_modal_enter/leave, and e_lsp_ui_safe returns false while the depth is
    non-zero -- that is what stops a streamed Metals diagnostic / status repaint
    from drawing under the open box and corrupting it.

    The race itself (a server message landing in the exact window the box is up)
    is timing-sensitive and not reliably reproducible headless, so this asserts
    the *mechanism* deterministically: opening the menu enters the guard and
    closing it leaves, balanced -- via the XWPE_UI_TRACE hook."""
    trace = tmp_path / "ui.trace"
    with WpeSession(str(tmp_path), PROG, filename="demo.c", wait=2.0,
                    env_extra={"XWPE_UI_TRACE": str(trace)}) as w:
        time.sleep(0.8)
        w._drain(1.0)
        w.key(ALT_Q, delay=0.4)
        w.key("?", delay=0.8)              # Alt-Q ? -> the action menu (WpeHandleSubmenu)
        w._drain(0.6)
        assert w.alive(), "wpe died opening the Alt-Q menu"
        rows = w.display()
        assert any("Diagnostics" in r for r in rows) and any("Rename" in r for r in rows), \
            "the Alt-Q action menu did not render:\n%s" % "\n".join(rows)
        w.key("\033", delay=0.5)           # Esc closes the menu
        w._drain(0.5)

        log = trace.read_text() if trace.exists() else ""
        enters = log.count("lsp modal enter")
        leaves = log.count("lsp modal leave")
        assert enters >= 1, \
            "opening the Alt-Q menu did not enter the LSP modal guard:\n%s" % log
        assert enters == leaves, \
            "LSP modal guard not balanced (enter=%d leave=%d):\n%s" \
            % (enters, leaves, log)


HOVER_PROG = (
    "#include <vector>\n"
    "int compute(const std::vector<int>& v) { return (int) v.size(); }\n"
    "int main() { std::vector<int> v; return compute(v); }\n"
)


def test_hover_is_a_cursor_tooltip_not_a_modal_box(tmp_path):
    """Alt-Q H shows hover as a small box anchored at the cursor, dismissed by any
    key (consumed -- it must NOT edit the buffer), replacing the old centered
    one-button e_message dialog.

    Asserts: a 'Hover' box appears with the symbol's type; it carries NO ' OK '
    button (the modal box did); and the dismiss key is swallowed, not typed."""
    (tmp_path / "compile_flags.txt").write_text("-std=c++17\n")
    with WpeSession(str(tmp_path), HOVER_PROG, filename="demo.cpp", wait=2.0) as w:
        time.sleep(1.0)
        w._drain(1.0)
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=8.0)              # start clangd
        w._drain(2.0)
        assert w.alive(), "wpe died starting clangd"

        _find(w, "compute")               # cursor onto the function name
        w.key(ALT_Q, delay=0.4)
        w.key("h", delay=3.0)             # hover -> tooltip
        w._drain(1.0)
        assert w.alive(), "wpe died on hover"

        rows = w.display()
        if not any("Hover" in r for r in rows):
            pytest.skip("clangd returned no hover yet (cold start)")
        assert any("compute" in r or "int" in r for r in rows), \
            "hover tooltip did not show the symbol type:\n%s" % "\n".join(rows)
        # a tooltip, not the old modal box: no centered " OK " button
        assert not any(" OK " in r for r in rows), \
            "hover still renders the old modal message box (has an OK button):\n%s" \
            % "\n".join(rows)

        # dismiss with a printable key; it must be CONSUMED, not inserted
        w.key("z", delay=0.5)
        w._drain(0.5)
        assert not any("Hover" in r for r in w.display()), \
            "hover tooltip did not dismiss on a keypress"
        w.save()
        assert w.text() == HOVER_PROG, \
            "the dismiss key edited the buffer (tooltip must consume it):\n%r" \
            % w.text()


# Two undeclared identifiers => two clangd errors, on known lines, to navigate.
DIAG_PROG = (
    "int main() {\n"
    "    int a = first_undeclared;\n"
    "    return second_undeclared;\n"
    "}\n"
)


def _cursor_line(w):
    """The 1-based line number from the editor status bar ('A q  L:C ...')."""
    import re
    for r in w.display():
        m = re.search(r"A q\s+(\d+):(\d+)", r)
        if m:
            return int(m.group(1))
    return None


def test_diagnostic_navigation_jumps_between_problems(tmp_path):
    """Alt-Q . / Alt-Q , move the cursor to the next / previous diagnostic and
    show its message in the cursor tooltip, wrapping around the file."""
    (tmp_path / "compile_flags.txt").write_text("-std=c++17\n")
    with WpeSession(str(tmp_path), DIAG_PROG, filename="demo.cpp", wait=2.0) as w:
        time.sleep(1.0)
        w._drain(1.0)
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=10.0)            # start clangd; wait for diagnostics
        w._drain(3.0)
        if "2 error" not in _text(w):
            pytest.skip("clangd has not published both diagnostics yet")

        # Alt-Q . -> first error (line 2); its message shows in the tooltip
        w.key(ALT_Q, delay=0.4)
        w.key(".", delay=2.5)
        w._drain(0.8)
        assert any("first_undeclared" in r for r in w.display()), \
            "the tooltip did not show the first problem's message:\n%s" % _text(w)
        # the footer legend advertises the navigation keys
        assert any(("next" in r and "prev" in r) for r in w.display()), \
            "the tooltip is missing the '. next  , prev' footer hint:\n%s" % _text(w)

        # While the tooltip is up, BOTH ways of stepping must work and must NOT
        # leak a keystroke into the buffer:
        #  - re-pressing the full Alt-Q . prefix (the natural habit), and
        #  - a bare '.' / ',' (the shortcut).
        w.key(ALT_Q, delay=0.4)
        w.key(".", delay=2.0)             # Alt-Q .  -> second error (line 3)
        w._drain(0.4)
        w.key(",", delay=2.0)             # bare ',' -> back to the first (line 2)
        w._drain(0.4)
        w.key("\033", delay=0.6)          # dismiss
        w._drain(0.5)
        assert _cursor_line(w) == 2, \
            "stepping with Alt-Q . / bare ',' did not land on line 2, at %s" \
            % _cursor_line(w)
        w.save()
        assert w.text() == DIAG_PROG, \
            "a navigation key leaked into the buffer:\n%r" % w.text()


COMPLETE_PROG = (
    "#include <vector>\n"
    "int main() {\n"
    "    std::vector<int> v;\n"
    "    v.\n"
    "    return 0;\n"
    "}\n"
)


def test_completion_drops_down_at_the_cursor(tmp_path):
    """Alt-Q C opens the completion list as a compact dropdown ANCHORED at the
    cursor (capped to ~10 visible so it fits the editor, not a full-height wall at
    the corner), and selecting an entry inserts it."""
    (tmp_path / "compile_flags.txt").write_text("-std=c++17\n")
    with WpeSession(str(tmp_path), COMPLETE_PROG, filename="demo.cpp", wait=2.0) as w:
        time.sleep(1.0)
        w._drain(1.0)
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=9.0)             # start clangd
        w._drain(2.0)
        # cursor onto the end of 'v.' (line 4, after the dot)
        _find(w, "v.")
        w.key(ALT_Q, delay=0.4)
        w.key("c", delay=3.0)             # complete
        w._drain(1.0)
        rows = w.display()
        if not any("Complete" in r for r in rows):
            pytest.skip("clangd returned no completions yet (cold start)")
        # member names of std::vector show up in the list...
        assert any(("begin" in r or "assign" in r or "push_back" in r) for r in rows), \
            "completion dropdown is missing the member list:\n%s" % "\n".join(rows)
        # ...and it is the COMPACT anchored dropdown (caps visible rows), not the
        # full 16-row default list -- "first 10 of N" proves the cursor anchor path
        assert any("first 10 of" in r for r in rows), \
            "completion list was not capped to a compact dropdown:\n%s" \
            % "\n".join(rows)

        w.key("\r", delay=1.0)            # Enter inserts the focused completion
        w._drain(0.6)
        w.save()
        line4 = w.text().split("\n")[3]
        assert line4.strip().startswith("v.") and len(line4.strip()) > 2, \
            "selecting a completion did not insert it (line 4 = %r)" % line4
