"""The contextual Metals / language-server menu, grouped by surface.

xwpe surfaces the LSP actions the Borland way: a per-window bottom-bar entry
("Metals") -- shown only while a language-server file is the active window --
that opens a popup of every Alt-Q action.  This mirrors how the Messages bar
shows Compile/Run and the Debug bar shows Trace/Step.

Groups (like the other menu tests, by shortcut AND by mouse, plus variations):

  1. Visibility      -- a .scala window shows "Metals"; a .c window does not.
  2. Focus switching -- open a non-LSP file, the entry disappears; switch back
                        to the Scala window and it returns (the user's ask).
  3. Mouse           -- click the "Metals" bar entry -> the action popup opens
                        (titled after the server) listing every action.
  4. Keyboard        -- Alt-Q + letter runs an action (Metals-gated, slow).

Groups 1-3 need NO running server: the bar keys off the filename extension and
the popup is built locally; only SELECTING an action talks to Metals.  Must run
the `wpe` (programming-mode) binary so the bar action dispatches.

Run: WPE_BIN=../wpe python -m pytest -v tests/test_lsp_menu.py
"""
import os
import pty
import select
import shutil
import subprocess
import time

import pytest
import pyte

from test_utf8_border import SafeScreen

WPE_BIN = os.environ.get("WPE_BIN") or os.path.join(os.path.dirname(__file__), "..", "wpe")
COLS, ROWS = 80, 30
ESC = "\033"


def _sgr_press(col, row):
    return "\033[<0;%d;%dM" % (col, row)      # SGR-1006 left press, 1-based


def _sgr_release(col, row):
    return "\033[<0;%d;%dm" % (col, row)


class _Wpe:
    """A wpe session that can open SEVERAL files, screenshot, click and type."""

    def __init__(self, workdir, files):
        """files: list of (name, body); all are created and opened together."""
        for name, body in files:
            with open(os.path.join(workdir, name), "w") as fh:
                fh.write(body)
        self.screen = SafeScreen(COLS, ROWS)
        self.stream = pyte.Stream(self.screen)
        self.master_fd, slave = pty.openpty()
        env = os.environ.copy()
        env.update(TERM="xterm-256color", COLUMNS=str(COLS), LINES=str(ROWS),
                   LC_ALL="en_US.UTF-8", HOME=workdir)
        self.proc = subprocess.Popen(
            [WPE_BIN] + [n for n, _ in files], stdin=slave, stdout=slave,
            stderr=slave, cwd=workdir, env=env, preexec_fn=os.setsid)
        os.close(slave)
        self.drain(1.6)

    def drain(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            r, _, _ = select.select([self.master_fd], [], [], 0.1)
            if r:
                try:
                    data = os.read(self.master_fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                self.stream.feed(data.decode("utf-8", "replace"))

    def key(self, s, delay=0.4):
        os.write(self.master_fd, s.encode())
        self.drain(delay)

    def click(self, col, row):
        os.write(self.master_fd, _sgr_press(col, row).encode())
        self.drain(0.2)
        os.write(self.master_fd, _sgr_release(col, row).encode())
        self.drain(0.6)

    def display(self):
        return self.screen.display

    def text(self):
        return "\n".join(self.screen.display)

    def find(self, needle):
        """1-based (col,row) of the first cell of `needle`, or None."""
        for y, line in enumerate(self.screen.display):
            x = line.find(needle)
            if x >= 0:
                return (x + 1, y + 1)
        return None

    def status_bar(self):
        return self.screen.display[ROWS - 1]

    def title(self):
        return self.screen.display[1]

    def alive(self):
        return self.proc.poll() is None

    def close(self):
        try:
            os.killpg(os.getpgid(self.proc.pid), 9)
        except Exception:
            pass
        try:
            os.close(self.master_fd)
        except Exception:
            pass
        self.proc.wait()


# --------------------------------------------------------------------------
# Group 1: contextual visibility
# --------------------------------------------------------------------------
def test_scala_window_shows_metals_entry(tmp_path):
    w = _Wpe(str(tmp_path), [("Demo.scala", "object Demo\n")])
    try:
        assert w.alive(), "wpe died opening a .scala file"
        assert "Metals" in w.status_bar(), \
            "Scala window should show the 'Metals' status-bar entry\n%s" % w.text()
    finally:
        w.close()


def test_c_window_has_no_metals_entry(tmp_path):
    w = _Wpe(str(tmp_path), [("t.c", "int main(void){return 0;}\n")])
    try:
        assert w.alive(), "wpe died opening a .c file"
        assert "Metals" not in w.status_bar(), \
            "Non-LSP window must NOT show the 'Metals' entry\n%s" % w.text()
    finally:
        w.close()


# --------------------------------------------------------------------------
# Group 2: focus switching -- the entry follows the active window
# --------------------------------------------------------------------------
def test_metals_entry_follows_active_window(tmp_path):
    """Open a .scala and a .c together; the 'Metals' entry must be present for
    exactly ONE of the two windows (the Scala one -- a .c never shows it, see
    test_c_window_has_no_metals_entry) and must RETURN when we switch back to it.
    Window identity by title is unreliable with tiled windows, so we assert the
    contextual toggle directly: the two windows differ, and the entry reappears."""
    w = _Wpe(str(tmp_path), [("Main.scala", "object Main\n"),
                             ("plain.c", "int x;\n")])
    try:
        assert w.alive(), "wpe died opening two files"
        w.key("\0331", delay=0.6)               # Alt-1: jump to window 1
        m1 = "Metals" in w.status_bar()
        w.key("\0332", delay=0.6)               # Alt-2: jump to window 2
        m2 = "Metals" in w.status_bar()
        assert m1 != m2, (
            "the 'Metals' entry must be contextual: present for the Scala window "
            "only, not for both/neither (win1=%s win2=%s)" % (m1, m2))
        # switch back to whichever window had it -> it must come back
        back = "\0331" if m1 else "\0332"
        w.key(back, delay=0.6)
        assert "Metals" in w.status_bar(), \
            "the 'Metals' entry did not return when switching back to the Scala window"
    finally:
        w.close()


# --------------------------------------------------------------------------
# Group 3: mouse -- click the bar entry, the action popup opens
# --------------------------------------------------------------------------
def test_mouse_click_opens_action_dropdown(tmp_path):
    """Clicking the 'Metals' bar entry opens a top-menu-style dropdown that
    unfolds UPWARD from the bar (not a centered dialog) listing every action --
    no running server needed to display it."""
    w = _Wpe(str(tmp_path), [("Demo.scala", "object Demo\n")])
    try:
        pos = w.find("Metals")
        assert pos, "no 'Metals' entry on the bar\n%s" % w.text()
        w.click(pos[0] + 1, pos[1])             # click inside the label
        rows = w.display()
        body = "\n".join(rows)
        for action in ("Diagnostics", "Go to Definition", "Hover", "References",
                       "Rename", "Format"):
            assert action in body, \
                "action %r missing from the menu dropdown\n%s" % (action, body)
        # it is a pull-UP anchored at the bar: the LAST item ("Format") sits just
        # above the status bar (lower screen), not centered like the old dialog
        last_row = next(i for i, r in enumerate(rows) if "Format" in r)
        assert last_row >= ROWS - 4, \
            "dropdown should bottom-anchor at the bar (last item row=%d of %d)\n%s" \
            % (last_row, ROWS, body)
        w.key(ESC, delay=0.4)                   # close it cleanly
        assert w.alive(), "wpe died after opening/closing the menu"
    finally:
        w.close()


# --------------------------------------------------------------------------
# Group 4: keyboard -- Alt-Q + letter runs an action (needs a real Metals)
# --------------------------------------------------------------------------
@pytest.mark.skipif(shutil.which("metals") is None or shutil.which("scala-cli") is None,
                    reason="metals and scala-cli required")
def test_keyboard_altq_definition_jumps(tmp_path):
    """The keyboard equivalent of the menu: Alt-Q E to start Metals, then Alt-Q D
    on a use of a symbol jumps to its definition (same file here, for speed)."""
    (tmp_path / "project.scala").write_text("//> using jvm temurin:21\n")
    w = _Wpe(str(tmp_path), [("Demo.scala",
        "object Demo:\n  val z = greeting(\"x\")\n  def greeting(n: String): String = n\n")])
    try:
        w.key("\033q", delay=0.4)
        w.key("e", delay=150.0)                 # start Metals + first compile
        w.drain(3.0)
        assert w.alive(), "wpe died starting Metals"
        # position on the use of `greeting` via Find (Alt-S f), prefix so the
        # cursor lands inside the identifier
        w.key("\033s", delay=0.6)
        w.key("f", delay=0.6)
        for ch in "greet":
            w.key(ch, delay=0.05)
        w.key("\r", delay=1.0)
        w.key("\033q", delay=0.4)
        w.key("d", delay=4.0)
        w.drain(2.0)
        cur = w.screen.cursor
        rows = w.display()
        row = rows[cur.y] if 0 <= cur.y < len(rows) else ""
        assert "def greeting" in row, \
            "Alt-Q D did not jump to the definition (row=%r)\n%s" % (row, w.text())
    finally:
        w.close()
