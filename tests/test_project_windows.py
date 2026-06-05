"""Project-window rendering regression tests for wpe (terminal mode).

Covers the 2026 Borland-audit fixes to the project subsystem:
  - the modal pickers and the project window show a visible TITLE
    ("Select Project", "Project: <name>.prj");
  - that title is NOT white-on-white (fb->nr regression guard -- pyte shows
    a glyph regardless of colour, so we assert fg != bg explicitly);
  - the project window STATUS BAR is packed compactly and the
    "Project: <name>" label does not overwrite the rightmost button.

Run: tests/.venv/bin/python -m pytest -v tests/test_project_windows.py
"""

import os
import pty
import select
import subprocess
import time

import pyte
import pytest

from test_utf8_border import SafeScreen

WPE_BIN = os.environ.get('WPE_BIN') or os.path.join(os.path.dirname(__file__), '..', 'wpe')

KEY_ALT_P = '\033p'
KEY_RETURN = '\r'
COLS, ROWS = 80, 30


def run_wpe(workdir, keys, wait=1.5, key_delay=0.5, settle=0.7):
    """Run wpe on a demo project, send keys, return the settled SafeScreen."""
    screen = SafeScreen(COLS, ROWS)
    stream = pyte.Stream(screen)
    master_fd, slave_fd = pty.openpty()

    env = os.environ.copy()
    env['TERM'] = 'xterm-256color'
    env['COLUMNS'] = str(COLS)
    env['LINES'] = str(ROWS)
    env['LC_ALL'] = 'en_US.UTF-8'
    env['HOME'] = workdir          # isolate from the user's ~/.xwpe config

    proc = subprocess.Popen(
        [WPE_BIN, 'main.c'],
        stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
        cwd=workdir, env=env, preexec_fn=os.setsid)
    os.close(slave_fd)

    def drain(timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            r, _, _ = select.select([master_fd], [], [], 0.1)
            if r:
                try:
                    data = os.read(master_fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                stream.feed(data.decode('utf-8', 'replace'))

    try:
        drain(wait)
        for k in keys:
            os.write(master_fd, k.encode())
            drain(key_delay)
        drain(settle)
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), 9)
        except Exception:
            pass
        os.close(master_fd)
        proc.wait()
    return screen


@pytest.fixture
def project_dir(tmp_path):
    """A workdir with main.c and a demo.prj listing it."""
    (tmp_path / 'main.c').write_text('int main(void){return 0;}\n')
    (tmp_path / 'demo.prj').write_text(
        'CMP=\tgcc\nEXENAME=\tdemo\nFILES=\tmain.c\n')
    return str(tmp_path)


def row_with(screen, needle):
    for y in range(ROWS):
        if needle in screen.display[y]:
            return y
    return None


def open_project(workdir):
    """Alt-P P, Enter (focus Files), Enter (open demo.prj)."""
    return run_wpe(workdir, [KEY_ALT_P, 'p', KEY_RETURN, KEY_RETURN])


def test_select_project_picker_has_title(project_dir):
    screen = run_wpe(project_dir, [KEY_ALT_P, 'p'])
    assert row_with(screen, 'Select Project') is not None, \
        'open-project picker must show the "Select Project" title'


def test_project_window_title_visible(project_dir):
    screen = open_project(project_dir)
    y = row_with(screen, 'Project: demo.prj')
    assert y is not None, 'project window must show "Project: demo.prj" title'

    # Regression guard for fb->nr white-on-white: the title glyphs must have
    # a foreground distinct from their background (pyte shows the char anyway).
    x = screen.display[y].index('Project: demo.prj')
    cell = screen.buffer[y][x]
    assert cell.fg != cell.bg, \
        'project title is invisible (fg == bg == %r)' % cell.fg


def test_window_project_with_no_project_open(tmp_path):
    # Guard (e_project_is_open): Window->Project with no project open must NOT
    # synthesise an empty project window; it reports "No project open".
    (tmp_path / 'main.c').write_text('int main(void){return 0;}\n')
    screen = run_wpe(str(tmp_path), ['\033w', 'p'])   # Alt-W P, no project
    assert row_with(screen, 'No project open') is not None, \
        'Window->Project with no project open must report "No project open"'


def test_status_bar_compact_with_project_label(project_dir):
    screen = open_project(project_dir)
    bar = screen.display[ROWS - 1]
    # The project name is shown in the status bar (so you know a project is
    # open without opening a window).
    assert 'Project: demo.prj' in bar, 'status bar must show the project name'
    # The rightmost button must survive (label must not overwrite it).
    assert 'Quit' in bar, 'status bar Quit button must not be overwritten'
    # Buttons are packed left: the project label sits clear of the last button.
    label_col = bar.index('Project: demo.prj')
    quit_col = bar.index('Quit')
    assert quit_col + len('Quit') < label_col, \
        'Quit button and project label must not overlap'


# -- #142: New Project vs Open Project ---------------------------------------

CLEAR_NAME = '\x7f' * 5   # erase the "*.prj" filter in the Name field


def test_project_menu_has_new_and_open(tmp_path):
    (tmp_path / 'main.c').write_text('int main(void){return 0;}\n')
    screen = run_wpe(str(tmp_path), ['\033p'])    # open the Project menu
    assert row_with(screen, 'New Project') is not None, \
        'Project menu must offer "New Project"'
    assert row_with(screen, 'Open Project') is not None, \
        'Project menu must offer "Open Project"'


def test_new_project_creates_prj(tmp_path):
    (tmp_path / 'main.c').write_text('int main(void){return 0;}\n')
    # Project -> New Project, type a fresh name, Enter.
    run_wpe(str(tmp_path), ['\033p', 'n', CLEAR_NAME, 'fresh.prj', '\r'])
    assert (tmp_path / 'fresh.prj').exists(), \
        'New Project must create the .prj the user typed'


def test_open_project_is_strict(tmp_path):
    (tmp_path / 'main.c').write_text('int main(void){return 0;}\n')
    # Project -> Open Project, type a name that does not exist.
    screen = run_wpe(str(tmp_path), ['\033p', 'p', CLEAR_NAME, 'ghost.prj', '\r'])
    assert not (tmp_path / 'ghost.prj').exists(), \
        'Open Project must NOT create a missing project (that is New Project)'
    assert row_with(screen, 'not found') is not None, \
        'Open Project on a missing file must report "Project file not found"'


def test_delete_removes_last_file(tmp_path):
    # Regression: e_p_del_df refused to delete the last entry (guard
    # nf > anz-2), so the last/only project member could never be removed.
    for fn in ('extra.c', 'main.c', 'util.c'):
        (tmp_path / fn).write_text('int x;\n')
    (tmp_path / 'd.prj').write_text(
        'CMP=\tgcc\nEXENAME=\td\nFILES=\textra.c main.c util.c\n')
    (tmp_path / 'main.c').write_text('int main(void){return 0;}\n')
    # Open project, then Delete three times (Alt-D).
    run_wpe(str(tmp_path), ['\033p', 'p', '\r', '\r', '\033d', '\033d', '\033d'])
    files = _prj_files(tmp_path / 'd.prj')
    assert files == [], \
        'all project members (incl. the last) must be deletable; FILES=%r' % files


def _prj_files(prj_path):
    for line in prj_path.read_text().splitlines():
        if line.startswith('FILES'):
            return line.split('\t', 1)[1].split() if '\t' in line else []
    return []


def test_long_libname_does_not_overflow(tmp_path):
    # T4 regression: LIBNAME= is copied into the fixed library[80] buffer.
    # A long value must be truncated, not overflow the global (which corrupted
    # adjacent state / crashed). Opening such a project must stay alive.
    (tmp_path / 'main.c').write_text('int main(void){return 0;}\n')
    long_lib = 'libwaytoolongname' * 12          # ~200 chars, well over 80
    (tmp_path / 'big.prj').write_text(
        'CMP=\tgcc\nEXENAME=\tp\nLIBNAME=\t%s\nFILES=\tmain.c\n' % long_lib)
    screen = run_wpe(str(tmp_path), ['\033p', 'p', '\r', '\r'])
    assert row_with(screen, 'Project: big.prj') is not None, \
        'project with an over-long LIBNAME must open without crashing'
