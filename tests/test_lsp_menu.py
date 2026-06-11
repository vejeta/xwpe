"""The contextual Metals / language-server menu, grouped by surface.

xwpe surfaces the LSP actions the Borland way: a per-window bottom-bar entry
("Metals", named after the server) -- shown only while a language-server file is
the active window -- that opens a drop-up menu of every action.  Reach it by
clicking the entry OR with Alt-Q (which opens the same menu, like Alt-F opens
File).  This mirrors how the Messages bar shows Compile/Run and the Debug bar
shows Trace/Step.

Groups (like the other menu tests, by shortcut AND by mouse, plus variations):

  1. Visibility      -- a .scala window shows "Metals"; a .c window does not.
  2. Focus switching -- open a non-LSP file, the entry disappears; switch back
                        to the Scala window and it returns (the user's ask).
  3. Mouse           -- click the "Metals" bar entry -> the action popup opens
                        (titled after the server) listing every action.
  4. Keyboard        -- Alt-Q opens the menu; its letter then runs an action
                        (e.g. Alt-Q D = Definition; Metals-gated, slow).

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

    def __init__(self, workdir, files, eager=False):
        """files: list of (name, body); all are created and opened together.
        eager=False disables the LSP eager-start-on-open so the no-server tests
        (and the Alt-Q-driven ones) keep their old behaviour; the dedicated
        eager test passes eager=True."""
        for name, body in files:
            with open(os.path.join(workdir, name), "w") as fh:
                fh.write(body)
        self.screen = SafeScreen(COLS, ROWS)
        self.stream = pyte.Stream(self.screen)
        self.master_fd, slave = pty.openpty()
        env = os.environ.copy()
        env.update(TERM="xterm-256color", COLUMNS=str(COLS), LINES=str(ROWS),
                   LC_ALL="en_US.UTF-8", HOME=workdir)
        if not eager:
            env["XWPE_LSP_NO_EAGER"] = "1"
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
                       "Incoming calls", "Outgoing calls", "Supertypes",
                       "Subtypes", "Expand selection", "Inlay hints",
                       "Semantic colours", "Rename", "Format"):
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


@pytest.mark.skipif(shutil.which("metals") is None or shutil.which("scala-cli") is None,
                    reason="metals and scala-cli required")
def test_keyboard_altq_inlay_hints(tmp_path):
    """Alt-Q Y toggles end-of-line inlay hints: the inferred type of an
    un-annotated `val` shows up at the end of its line.  Self-contained file
    (no cross-file indexing) pinned to the 3.3 LTS PC so the type resolves
    cleanly.  `: Int` appears ONLY as the hint -- the source never writes it --
    so its presence proves the overlay rendered.  Needs a real Metals (slow)."""
    (tmp_path / "project.scala").write_text(
        "//> using scala 3.3.7\n//> using jvm temurin:21\n")
    w = _Wpe(str(tmp_path), [("Demo.scala",
        "object Demo:\n"
        "  def main(a: Array[String]): Unit =\n"
        "    val n = 1 + 1\n"
        "    println(n)\n")])
    try:
        w.key("\033q", delay=0.4)
        w.key("e", delay=150.0)                 # Alt-Q E: start Metals + compile
        w.drain(3.0)
        assert w.alive(), "wpe died starting Metals"
        w.key("\033q", delay=0.4)
        w.key("y", delay=10.0)                  # Alt-Q Y: toggle inlay hints on
        w.drain(3.0)
        body = "\n".join(w.display())
        assert ": Int" in body, \
            "inlay hint ': Int' for `val n` did not render at end-of-line\n%s" \
            % w.text()
    finally:
        w.close()


@pytest.mark.skipif(shutil.which("metals") is None or shutil.which("scala-cli") is None,
                    reason="metals and scala-cli required")
def test_worksheet_shows_evaluation_results(tmp_path):
    """#187: open a .worksheet.sc, start Metals, and the per-line EVALUATION
    results appear at end-of-line -- automatic, no Alt-Q Y.  Metals delivers
    them as inlay hints (the decoration protocol is gone since Metals 1.3), so
    xwpe auto-enables the inlay overlay for a worksheet.  `val a = 5 + 7` has no
    `12` in the source, so seeing `12` at a line end proves the evaluation hint
    rendered through the end-of-line overlay.  Needs a real Metals (slow)."""
    (tmp_path / "project.scala").write_text(
        "//> using scala 3.3.7\n//> using jvm temurin:21\n")
    w = _Wpe(str(tmp_path), [("ex.worksheet.sc",
        "val a = 5 + 7\n"
        "val b = a * 2\n")])
    try:
        w.key("\033q", delay=0.4)
        w.key("e", delay=160.0)                 # Alt-Q E: start Metals + evaluate
        w.drain(15.0)                           # eval + the fd-loop fetch/repaint
        assert w.alive(), "wpe died starting Metals"
        # NO edits: the results must appear on their own (the fd-loop fetches the
        # inlay hints when Metals evaluates and repaints+flushes the screen).
        body = "\n".join(w.display())
        assert "12" in body, \
            "worksheet result '12' (5 + 7) did not render at end-of-line\n%s" \
            % w.text()
    finally:
        w.close()


@pytest.mark.skipif(shutil.which("metals") is None or shutil.which("scala-cli") is None,
                    reason="metals and scala-cli required")
def test_async_start_keeps_editor_responsive(tmp_path):
    """#196 (the headline): the first LSP action starts Metals in the BACKGROUND
    and returns at once -- the editor must keep accepting keystrokes while the
    JVM boots, not freeze for the ~1-3 min cold start.  Kick the start off, then
    type into the buffer DURING the boot and confirm the text lands well before
    the server could possibly be ready.  Under the old synchronous start the
    keys would queue behind the frozen call and not appear until ~150s later, so
    seeing them within seconds proves the freeze is gone."""
    (tmp_path / "project.scala").write_text(
        "//> using scala 3.3.7\n//> using jvm temurin:21\n")
    w = _Wpe(str(tmp_path), [("Demo.scala", "object Demo\n")])
    try:
        w.key("\033q", delay=0.4)
        w.key("e", delay=2.0)                   # Alt-Q E: kick off the async start
        w.drain(1.0)
        assert w.alive(), "wpe died kicking off the async start"
        # Metals is booting now (not ready for ~30-60s).  Type during the boot:
        for ch in "ZZZTOP":
            w.key(ch, delay=0.2)
        w.drain(12.0)                           # generous: allow brief JVM-boot silence
        body = "\n".join(w.display())
        assert "ZZZTOP" in body, \
            "editor did not accept keystrokes during the Metals cold start " \
            "(the async start is not keeping the input loop alive)\n%s" % w.text()
        assert w.alive(), "wpe died while Metals was starting"
    finally:
        w.close()


def test_dead_server_does_not_spin(tmp_path, monkeypatch):
    """Robustness: if the language server exits immediately (crash, bad setup,
    an unsaved worksheet with no build), xwpe must NOT spin the input loop at
    100% CPU on the dead fd -- it detects the EOF, tears the session down and
    stays responsive.  PATH points at a fake 'metals' that exits at once;
    opening a .scala (eager on) spawns it, it dies, and the editor must still
    accept keystrokes (under the old bug the spin starved stdin and 'HELLO'
    would never appear).  Needs no real Metals."""
    fake = tmp_path / "bin"
    fake.mkdir()
    (fake / "metals").write_text("#!/bin/sh\nexit 0\n")
    (fake / "metals").chmod(0o755)
    monkeypatch.setenv("PATH", str(fake) + os.pathsep + os.environ["PATH"])
    w = _Wpe(str(tmp_path), [("Demo.scala", "object Demo\n")], eager=True)
    try:
        w.drain(5.0)                            # the fake server spawned + died by now
        for ch in "HELLO":
            w.key(ch, delay=0.15)
        w.drain(1.5)
        body = "\n".join(w.display())
        assert "HELLO" in body, \
            "editor did not accept keys after the server died -- it is spinning\n%s" \
            % w.text()
        assert w.alive(), "wpe died after the language server exited"
    finally:
        w.close()


def test_eager_start_skips_non_lsp_file(tmp_path):
    """#210 gating: with eager-start ENABLED, opening a file whose language has
    no server (a .c) must spawn nothing and stay instant -- the bar shows no
    Metals entry and no 'Starting language server' appears.  Needs no Metals:
    .c never maps to a server, so this proves the language gate."""
    w = _Wpe(str(tmp_path), [("main.c", "int main(void){ return 0; }\n")], eager=True)
    try:
        w.drain(2.5)
        body = "\n".join(w.display())
        assert "Starting language server" not in body, \
            "eager start fired for a non-LSP .c file\n%s" % w.text()
        assert "Metals" not in body, \
            "a .c window must not show the Metals bar\n%s" % w.text()
        assert w.alive(), "wpe died opening a .c file"
    finally:
        w.close()


@pytest.mark.skipif(shutil.which("metals") is None or shutil.which("scala-cli") is None,
                    reason="metals and scala-cli required")
def test_eager_start_on_open_boots_metals(tmp_path):
    """#210 (the '3' of 3+1): just OPENING a .scala boots Metals in the
    background -- NO Alt-Q.  We press no LSP key at all; the 'Starting language
    server ...' notice appearing on its own proves the open alone kicked off the
    server (full readiness, the slow part, is covered by the other Metals
    tests).  Needs a real Metals."""
    (tmp_path / "project.scala").write_text(
        "//> using scala 3.3.7\n//> using jvm temurin:21\n")
    w = _Wpe(str(tmp_path), [("Demo.scala", "object Demo:\n  val x = 1\n")],
             eager=True)
    try:
        w.drain(20.0)                           # no keypress -- the open kicked it off
        body = "\n".join(w.display())
        assert ("Starting language server" in body) or ("LSP:" in body) \
            or ("Metals:" in body), \
            "opening a .scala did not start Metals on its own\n%s" % w.text()
        assert w.alive(), "wpe died during the eager Metals start"
    finally:
        w.close()
