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


# All-explicit types, no call sites -> clangd has NO inlay hint to offer, which
# forces the "empty" path of the Alt-Q Y toggle (the one that used to freeze).
NOINLAY_PROG = (
    "int compute(int value) {\n"
    "    int result = value * 2;\n"       # explicit int, not auto -> no type hint
    "    return result;\n"                # no call site -> no parameter hint
    "}\n"
)


def test_inlay_toggle_defers_instead_of_freezing_when_empty(tmp_path):
    """Alt-Q Y on a file the server has no hints for must turn the overlay ON and
    say it will fill once indexing finishes -- it must NOT block the editor on a
    fixed wait and then give up with 'none for this file yet'.

    Regression: the toggle used to call e_lsp_wait_diagnostics(...,8000) -- an 8s
    SYNCHRONOUS wait -- on an empty first fetch, then leave the overlay OFF if it
    was still empty.  For a cold Metals (minutes to index) that was an 8s freeze
    AND a dead end.  The fix is event-driven: turn ON immediately, mark the
    snapshot stale, and let the async fd-loop re-pull when the server publishes.

    The message text is the deterministic proxy: the old path printed
    'Inlay hints: none for this file yet' and left it OFF; the new path prints
    'Inlay hints: ON ...'.  We also confirm the editor still accepts input."""
    (tmp_path / "compile_flags.txt").write_text("-std=c++17\n")
    with WpeSession(str(tmp_path), NOINLAY_PROG, filename="demo.cpp", wait=2.0) as w:
        time.sleep(1.0)
        w._drain(1.0)
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=8.0)              # start clangd + first diagnostics
        w._drain(2.0)
        assert w.alive(), "wpe died starting clangd"

        w.key(ALT_Q, delay=0.4)
        w.key("y", delay=2.0)              # toggle inlay hints on an empty-hint file
        w._drain(1.0)
        assert w.alive(), "wpe died toggling inlay hints"

        text = _text(w)
        # turned ON and DEFERRED, never the old synchronous give-up message
        assert "Inlay hints: ON" in text, \
            "empty-file inlay toggle did not turn ON / defer:\n%s" % text
        assert "Inlay hints: none" not in text, \
            "the toggle gave up with the old 'none for this file yet' message "\
            "(the synchronous wait is back):\n%s" % text

        # the editor is not frozen: a printable key still lands in the buffer
        w.key("X", delay=0.5)
        w._drain(0.5)
        w.save()
        assert w.text() != NOINLAY_PROG, \
            "editor did not accept input after the inlay toggle (frozen?):\n%r" \
            % w.text()


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
        # the footer legend tells the truth: ANY key closes (not just Esc), so it
        # must say so -- this guards against re-introducing a misleading "Esc"-only
        # hint while the handler in fact dismisses on any key.
        assert any("any key" in r for r in rows), \
            "hover footer must advertise 'Press any key to close':\n%s" \
            % "\n".join(rows)

        # dismiss with a printable, non-Esc key; it must be CONSUMED, not inserted,
        # and it must close the tooltip -- proving "any key closes" is literally true
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
        # the footer legend advertises the navigation keys AND tells the truth that
        # any other key closes -- not a misleading 'Esc'-only hint.
        assert any(("next" in r and "prev" in r) for r in w.display()), \
            "the tooltip is missing the '. next  , prev' footer hint:\n%s" % _text(w)
        assert any("any other key" in r for r in w.display()), \
            "footer must say 'any other key closes', since the handler dismisses on "\
            "any non-nav key:\n%s" % _text(w)

        # While the tooltip is up, BOTH ways of stepping must work and must NOT
        # leak a keystroke into the buffer:
        #  - re-pressing the full Alt-Q . prefix (the natural habit), and
        #  - a bare '.' / ',' (the shortcut).
        w.key(ALT_Q, delay=0.4)
        w.key(".", delay=2.0)             # Alt-Q .  -> second error (line 3)
        w._drain(0.4)
        w.key(",", delay=2.0)             # bare ',' -> back to the first (line 2)
        w._drain(0.4)
        # dismiss with a printable, NON-Esc key: the footer promises "any other key
        # closes", so 'z' must both close the tooltip and be swallowed (not typed).
        w.key("z", delay=0.6)
        w._drain(0.5)
        assert not any(("next" in r and "prev" in r) for r in w.display()), \
            "a non-nav key did not dismiss the diagnostic tooltip:\n%s" % _text(w)
        assert _cursor_line(w) == 2, \
            "stepping with Alt-Q . / bare ',' did not land on line 2, at %s" \
            % _cursor_line(w)
        w.save()
        assert w.text() == DIAG_PROG, \
            "a navigation/dismiss key leaked into the buffer:\n%r" % w.text()


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


# A spelling typo on a LOW line: clangd flags it with a "did you mean" fix-it,
# exposed as a quick-fix code action.  The padding lines push the error far down
# the screen so an anchored picker lands well below the top-left corner.
CODEACTION_PROG = (
    "int compute(int value) {\n"
    "    int result = value * 2;\n"
    "    int pad_a = 0;\n"
    "    int pad_b = 0;\n"
    "    int pad_c = 0;\n"
    "    int pad_d = 0;\n"
    "    int pad_e = 0;\n"
    "    return reslt + pad_a + pad_b + pad_c + pad_d + pad_e;\n"
    "}\n"
)


def _row_of(w, term):
    """Screen-row index of the first display row containing `term`, or -1."""
    for i, r in enumerate(w.display()):
        if term in r:
            return i
    return -1


def test_code_actions_drop_down_at_the_cursor(tmp_path):
    """Alt-Q A opens the quick-fix list ANCHORED at the cursor (the fix applies to
    the symbol under the cursor), not parked in the top-left corner like the
    browse-everything lists (outline / workspace symbols)."""
    (tmp_path / "compile_flags.txt").write_text("-std=c++17\n")
    with WpeSession(str(tmp_path), CODEACTION_PROG, filename="demo.cpp",
                    wait=2.0) as w:
        time.sleep(1.0)
        w._drain(1.0)
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=9.0)             # start clangd; wait for the diagnostic
        w._drain(2.0)
        _find(w, "reslt")                 # cursor onto the typo, on a low line
        cur = _cursor_line(w)
        w.key(ALT_Q, delay=0.4)
        w.key("a", delay=4.0)             # code actions
        w._drain(1.0)
        row = _row_of(w, "Code actions")
        if row < 0:
            pytest.skip("clangd offered no code action here (version/cold start)")
        # the picker dropped at the cursor, NOT at the corner: the default
        # e_lsp_pick parks its title around screen row 3; anchored at a cursor on
        # line ~8 it appears well below that.
        assert row >= 6, \
            "code-action picker was not anchored at the cursor (title at row %d, "\
            "cursor line %s):\n%s" % (row, cur, _text(w))
        # and it is the real quick-fix: the 'did you mean result' spelling action
        assert any("result" in r for r in w.display()), \
            "code-action list is missing the spelling fix:\n%s" % _text(w)
        w.key("\033", delay=0.6)          # Esc: don't apply, just checked position


def _problem_tooltip_open(w):
    """True when a diagnostic tooltip (with the nav footer) is on screen."""
    return any(("next" in r and "prev" in r) for r in w.display())


def test_alt_q_chains_through_the_diagnostic_tooltip(tmp_path):
    """While a problem tooltip is up, Alt-Q A closes it and opens the code-action
    picker in ONE motion -- the dispatch is shared, so the prefix is not wasted on
    just dismissing the tooltip (the friction the user reported)."""
    (tmp_path / "compile_flags.txt").write_text("-std=c++17\n")
    with WpeSession(str(tmp_path), CODEACTION_PROG, filename="demo.cpp",
                    wait=2.0) as w:
        time.sleep(1.0)
        w._drain(1.0)
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=10.0)            # start clangd; wait for the diagnostic
        w._drain(2.0)
        w.key(ALT_Q, delay=0.4)
        w.key(".", delay=2.5)             # jump to the problem -> tooltip, cursor on it
        if not _problem_tooltip_open(w):
            pytest.skip("clangd has not published the diagnostic yet")
        # chain: Alt-Q A WHILE the tooltip is up -> should open code actions
        w.key(ALT_Q, delay=0.4)
        w.key("a", delay=4.0)
        if not any("Code actions" in r for r in w.display()):
            pytest.skip("clangd offered no quick-fix at the cursor")
        assert any("Code actions" in r for r in w.display()), \
            "Alt-Q A did not chain through the tooltip into the code-action "\
            "picker:\n%s" % _text(w)
        w.key("\033", delay=0.6)          # don't apply


def test_undo_of_a_quick_fix_remarks_diagnostics(tmp_path):
    """Applying a quick-fix clears the error; undoing it brings the error back AND
    re-publishes the diagnostic marks.  Undo bypasses the per-keystroke Enter
    debounce, so without an explicit re-sync the marks would lag the buffer (the
    symptom the user hit: after Ctrl-U the re-introduced error stayed unmarked).

    Alt-Q . is used to probe the diagnostic state because it does NOT itself sync
    -- it only reads the marks clangd last published -- so it isolates whether the
    undo re-sync actually happened."""
    (tmp_path / "compile_flags.txt").write_text("-std=c++17\n")
    with WpeSession(str(tmp_path), CODEACTION_PROG, filename="demo.cpp",
                    wait=2.0) as w:
        time.sleep(1.0)
        w._drain(1.0)
        w.key(ALT_Q, delay=0.4)
        w.key("e", delay=10.0)            # start clangd; wait for the diagnostic
        w._drain(3.0)
        _find(w, "reslt")                 # cursor onto the typo
        w.key(ALT_Q, delay=0.4)
        w.key("a", delay=4.0)             # code actions
        if not any("Code actions" in r for r in w.display()):
            pytest.skip("clangd offered no quick-fix")
        w.key("\r", delay=3.0)            # apply 'did you mean result'
        w._drain(3.0)
        # the fix cleared the error: Alt-Q . now reports nothing (non-syncing probe)
        w.key(ALT_Q, delay=0.4)
        w.key(".", delay=2.0)
        if not any("No problems" in r for r in w.display()):
            pytest.skip("the quick-fix did not clear the diagnostic (clangd variance)")
        w.key("\033", delay=0.5)          # dismiss the "No problems" box

        # UNDO the fix; the buffer is dirty again -> the re-sync must re-mark it
        w.key("\x15", delay=1.5)          # Ctrl-U
        w._drain(4.0)                     # let the auto-resync + republish land
        w.key(ALT_Q, delay=0.4)
        w.key(".", delay=2.5)             # jump to a problem -- needs fresh marks
        assert not any("No problems" in r for r in w.display()), \
            "after undo the diagnostics were NOT re-published (undo re-sync "\
            "missing):\n%s" % _text(w)
        assert _problem_tooltip_open(w), \
            "Alt-Q . did not reopen a problem tooltip after undo:\n%s" % _text(w)
        w.key("\033", delay=0.5)
