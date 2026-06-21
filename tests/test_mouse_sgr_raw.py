"""Terminfo-independent SGR (1006) console-mouse decoding.

On a terminal whose terminfo lacks an SGR-capable ``kmous`` -- FreeBSD's
termcap console, OpenBSD's base curses -- ncurses does NOT fold an
``ESC [ < b ; x ; y M`` mouse report into a ``KEY_MOUSE`` event; the raw
bytes reach the application instead.  Older wpe ignored them, so the console
mouse silently did nothing on those systems even though the terminal was
emitting perfectly good reports (confirmed by capturing the raw bytes).

wpe now decodes the report itself (we_term.c: e_t_sgr_mouse, reached from
e_t_csi_key's ``<`` branch) and applies it exactly as the KEY_MOUSE path
would, so the mouse works regardless of terminfo.

These tests force the raw path on Linux by running under TERM=vt220 (no SGR
mouse in its terminfo), which is what FreeBSD/OpenBSD effectively present.
Under TERM=xterm-256color ncurses folds the report and this code never runs,
so the no-SGR TERM is essential to exercise it.
"""
import time

from wpe_driver import WpeSession, ALT

# A terminfo with no SGR mouse capability, so ncurses passes the raw
# "ESC [ < ..." through to wpe instead of synthesising KEY_MOUSE.
NO_SGR_TERM = {"TERM": "vt220"}


def _sgr(button, col, row, press=True):
    """An SGR 1006 mouse report at 1-based screen (col, row)."""
    return "\x1b[<%d;%d;%d%s" % (button, col, row, "M" if press else "m")


def test_sgr_click_opens_menu_on_no_sgr_terminal(tmp_path):
    """A left click on the 'File' entry of the menu bar opens the File menu,
    decoded purely from the raw SGR report (no KEY_MOUSE from ncurses)."""
    with WpeSession(str(tmp_path), "AAAAA\nBBBBB\nCCCCC\n",
                    env_extra=NO_SGR_TERM) as w:
        bar = w.display()[0]
        assert "File" in bar, "menu bar not drawn: %r" % bar
        col = bar.index("File") + 1          # 1-based SGR column of 'F'
        w.key(_sgr(0, col, 1, True), _sgr(0, col, 1, False), delay=0.5)
        screen = "\n".join(w.display())
        assert w.alive(), "wpe died decoding the SGR mouse report"
        assert "Save" in screen or "New" in screen, \
            "SGR click on 'File' did not open the File menu:\n" + screen


def test_sgr_report_bytes_do_not_leak_into_buffer(tmp_path):
    """The introducer and digits of the report are fully consumed: none of
    'M', the coordinates, or '<' leak into the edit buffer as typed text."""
    with WpeSession(str(tmp_path), "AAAAA\nBBBBB\nCCCCC\n",
                    env_extra=NO_SGR_TERM) as w:
        bar = w.display()[0]
        col = bar.index("File") + 1
        w.key(_sgr(0, col, 1, True), _sgr(0, col, 1, False), delay=0.5)
        w.key("\033", delay=0.4)             # Esc: close the menu
        w.menu(ALT.FILE, "s")                # Save
        time.sleep(0.3)
        assert w.alive(), "wpe died after the SGR click"
        assert w.text() == "AAAAA\nBBBBB\nCCCCC\n", \
            "SGR report bytes leaked into the buffer: %r" % w.text()
