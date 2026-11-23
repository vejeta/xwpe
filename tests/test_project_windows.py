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
import re
import select
import time

import pyte
import pytest

import gui_owner
from mode_premise import file_mode_is_obeyed, folder_mode_is_obeyed
from test_utf8_border import SafeScreen
from wpe_driver import WpeSession

WPE_BIN = os.environ.get('WPE_BIN') or os.path.join(os.path.dirname(__file__), '..', 'wpe')

KEY_ALT_P = '\033p'
KEY_RETURN = '\r'
COLS, ROWS = 80, 30


def run_wpe(workdir, keys, wait=1.5, key_delay=0.5, settle=0.7):
    """Run wpe on a demo project, send keys, return the settled SafeScreen."""
    screen = SafeScreen(COLS, ROWS)
    stream = pyte.Stream(screen)
    master_fd, slave_fd = pty.openpty()

    # The owner starts the editor: the whitelisted environment (HOME is the
    # row's folder, never the user's ~/.xwpe configuration), the tool farm's
    # PATH, and its end and leftovers are the owner's.  Nothing is given to
    # on_output: the owner reads what the editor writes while it ends it
    # (its teardown clears the last screen row), and the screen this row
    # returns is the one the row's own reads built.
    owned = gui_owner.Terminal.start(
        gui_owner.current(), WPE_BIN, ['main.c'], workdir, workdir,
        master_fd, slave_fd, (COLS, ROWS), label='project windows')
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
        owned.end_for_row()
        os.close(master_fd)
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


# -- the project's file refuses: the system's reason, never a guess -----------

PRJ_TEXT = 'CMP=\tgcc\nEXENAME=\td\nFILES=\textra.c t.c util.c\n'
COLS_ROWS = dict(cols=100, rows=30)


def _flat(text):
    """The words of a screen with the box borders gone and the white space
    collapsed: a box wraps its sentence at a width that depends on the length
    of the scratch path in it, so a phrase may be split over two rows."""
    rows = []
    for row in text.splitlines():
        row = re.sub(r'^(x\s+)+', '', row)
        row = re.sub(r'(\s+x)+(\s+\S)?\s*$', '', row)
        rows.append(row.strip())
    return re.sub(r'\s+', ' ', ' '.join(rows))


def _screen_text(w):
    return _flat('\n'.join(r.rstrip() for r in w.display() if r.strip()))


def _open_d_prj(w):
    w.key(KEY_ALT_P, delay=1.0)
    w.key('p', delay=1.5)
    w.key(CLEAR_NAME + 'd.prj' + KEY_RETURN, delay=2.0)


def _session(tmp_path, args=()):
    for fn in ('extra.c', 'util.c'):
        (tmp_path / fn).write_text('int x;\n')
    (tmp_path / 'd.prj').write_text(PRJ_TEXT)
    return WpeSession(str(tmp_path), 'int main(void){return 0;}\n', filename='t.c',
                      wait=2.0, args=args, **COLS_ROWS)


# @scenario SC-unsaved-text-17
def test_project_save_onto_a_read_only_prj_says_why(tmp_path):
    """Delete a member while the .prj is 0400: the box names the file and the
    system's reason and says the list is still in its window.  The old box
    was "Can't open File <path>" with no reason.  (Add goes through the same
    writer; it says nothing more than this box.)"""
    file_mode_is_obeyed(str(tmp_path), 0o400, os.O_WRONLY)
    prj = tmp_path / 'd.prj'
    with _session(tmp_path) as w:
        _open_d_prj(w)
        os.chmod(str(prj), 0o400)
        try:
            w.key('\033d', delay=1.5)
            text = _screen_text(w)
        finally:
            os.chmod(str(prj), 0o600)
        assert w.alive(), text
    assert 'Cannot write' in text and 'Permission denied' in text, text
    assert 'The project list' in text and 'still in its window' in text, text
    assert "Can't open" not in text, text
    assert 'Project: d.prj' in text, text
    assert prj.read_text() == PRJ_TEXT


# Escape closes the project window from inside its own key loop; Ctrl-W goes
# through the desktop's close, which writes the list too.
CLOSE_KEYS = [pytest.param('\x1b', id='escape'), pytest.param('\x17', id='ctrl-w')]


# @scenario SC-unsaved-text-17
@pytest.mark.parametrize('close_key', CLOSE_KEYS)
def test_closing_a_project_the_file_refuses_keeps_it_open_and_says_why_once(tmp_path, close_key):
    """Delete a member, the .prj goes read-only, Close: one box, and the
    project window is still there with its list.  With the file writable
    again Close writes the list once and the window closes."""
    file_mode_is_obeyed(str(tmp_path), 0o400, os.O_WRONLY)
    prj = tmp_path / 'd.prj'
    with _session(tmp_path) as w:
        _open_d_prj(w)
        os.chmod(str(prj), 0o400)
        try:
            w.key('\033d', delay=1.5)       # the refusal, said once
            w.key('\x1b', delay=1.0)        # dismiss the box
            w.key(close_key, delay=1.5)     # Close
            closing = _screen_text(w)
            w.key('\x1b', delay=1.0)        # dismiss the box
            still_open = _screen_text(w)
        finally:
            os.chmod(str(prj), 0o600)
        w.key(close_key, delay=1.5)         # Close, the file writable again
        closed = _screen_text(w)
        assert w.alive(), closed
    assert closing.count('Cannot write') == 1, closing
    assert 'still in its window' in closing, closing
    assert 'Project: d.prj' in still_open, still_open
    assert 'Project: d.prj' not in closed.split('F1 Help')[0], closed
    assert 'extra.c' not in prj.read_text(), prj.read_text()


# @scenario SC-path-cuts-16
def test_open_project_in_a_folder_closed_to_reading_says_why(tmp_path):
    """Open Project on a .prj the user cannot read: the box says the system's
    reason, not "Project file not found" (the file is there) and not a silent
    empty project."""
    file_mode_is_obeyed(str(tmp_path), 0o000, os.O_RDONLY)
    locked = tmp_path / 'locked.prj'
    locked.write_text(PRJ_TEXT)
    os.chmod(str(locked), 0o000)
    try:
        with WpeSession(str(tmp_path), 'int main(void){return 0;}\n', filename='t.c',
                        wait=2.0, **COLS_ROWS) as w:
            w.key(KEY_ALT_P, delay=1.0)
            w.key('p', delay=1.5)
            w.key(CLEAR_NAME + 'locked.prj' + KEY_RETURN, delay=2.0)
            text = _screen_text(w)
            assert w.alive(), text
    finally:
        os.chmod(str(locked), 0o600)
    assert 'Cannot open project' in text and 'Permission denied' in text, text
    assert 'not found' not in text, text


# -- the project list the file refuses, and what comes next -------------------

B_TEXT = 'CMP=\tgcc\nEXENAME=\tb\nFILES=\tutil.c\n'
KEY_ALT_1 = '\0331'     # the first window: the Project menu is reachable from it
KEY_F9 = '\033[20~'


def _need_unwritable_prj(tmp_path):
    file_mode_is_obeyed(str(tmp_path), 0o400, os.O_WRONLY)


def _open_b_while_d_refuses(tmp_path):
    """d.prj is open and dirty (a Delete the 0400 file refused), then Open
    Project b.prj.  Returns the first box, the second box and the screen
    after both."""
    (tmp_path / 'b.prj').write_text(B_TEXT)
    prj = tmp_path / 'd.prj'
    with _session(tmp_path) as w:
        _open_d_prj(w)
        os.chmod(str(prj), 0o400)
        try:
            w.key('\033d', delay=1.5)       # the refusal, said
            w.key('\x1b', delay=1.0)        # dismiss the box
            w.key(KEY_ALT_1, delay=1.0)
            w.key(KEY_ALT_P, delay=1.0)
            w.key('p', delay=1.5)
            w.key(CLEAR_NAME + 'b.prj' + KEY_RETURN, delay=2.0)
            first = _screen_text(w)
            w.key('\x1b', delay=1.5)        # dismiss it
            second = _screen_text(w)
            w.key('\x1b', delay=1.0)
            after = _screen_text(w)
        finally:
            os.chmod(str(prj), 0o600)
        assert w.alive(), after
    return first, second, after


# @scenario SC-unsaved-text-17
def test_open_project_while_the_old_one_cannot_be_written_keeps_the_old_one(tmp_path):
    """Open b.prj while d.prj's list is refused by its file: the writer's box,
    then "b.prj was not opened."; one project window, d.prj's, and no window
    for b.prj.  The old close used to leave two project windows."""
    _need_unwritable_prj(tmp_path)
    first, second, after = _open_b_while_d_refuses(tmp_path)
    assert 'Cannot write' in first and 'still in its window' in first, first
    assert 'b.prj was not opened.' in second, second
    assert 'Project: b.prj' not in after.split('F1 Help')[0], after
    assert after.split('F1 Help')[0].count('Project: d.prj') == 1, after


# @scenario SC-unsaved-text-17
def test_open_project_while_the_old_one_cannot_be_written_leaves_the_new_file_alone(tmp_path):
    """The old project's list is written into the old project's file or
    nowhere, never into the project being opened: b.prj is as it was.  On the
    old code the project was already b.prj when d.prj's window closed, so
    d.prj's list went into b.prj without a word."""
    _need_unwritable_prj(tmp_path)
    _open_b_while_d_refuses(tmp_path)
    assert (tmp_path / 'b.prj').read_text() == B_TEXT
    assert (tmp_path / 'd.prj').read_text() == PRJ_TEXT


# @scenario SC-path-cuts-18
def test_a_failed_open_project_leaves_the_project_option_as_it_was(tmp_path):
    """Open ghost.prj while d.prj is open: the box says not found, and the
    Project option Options > Save writes is still d.prj, not the typed name."""
    with _session(tmp_path) as w:
        _open_d_prj(w)
        w.key(KEY_ALT_1, delay=1.0)
        w.key(KEY_ALT_P, delay=1.0)
        w.key('p', delay=1.5)
        w.key(CLEAR_NAME + 'ghost.prj' + KEY_RETURN, delay=2.0)
        box = _screen_text(w)
        w.key(KEY_RETURN, delay=1.0)        # dismiss the box
        w.key('\033o', delay=1.0)
        w.key('s', delay=1.5)
        w.key(KEY_RETURN, delay=1.5)        # Options > Save
        assert w.alive(), box
    assert 'not found' in box, box
    value = None
    with open(os.path.join(str(tmp_path), '.config', 'xwpe', 'xwperc')) as fh:
        for line in fh:
            if line.startswith('Project'):
                value = line.split(':', 1)[1].strip()
    assert value is not None and value.endswith('d.prj'), value


# @scenario SC-unsaved-text-17
def test_build_does_not_run_on_a_project_list_the_file_refused(tmp_path):
    """F9 with d.prj's list refused by its file: the box says the list is kept
    and the build does not start on the old list (no compiler output in
    Messages)."""
    _need_unwritable_prj(tmp_path)
    prj = tmp_path / 'd.prj'
    with _session(tmp_path) as w:
        _open_d_prj(w)
        os.chmod(str(prj), 0o400)
        try:
            w.key('\033d', delay=1.5)
            w.key('\x1b', delay=1.0)
            w.key(KEY_F9, delay=3.0)
            box = _screen_text(w)
            w.key('\x1b', delay=3.0)
            after = _screen_text(w)
        finally:
            os.chmod(str(prj), 0o600)
        assert w.alive(), after
    assert 'Cannot write' in box and 'still in its window' in box, box
    assert 'Success' not in after and 'Compiling' not in after and 'cc ' not in after, after


# @scenario SC-unsaved-text-17
def test_add_onto_a_read_only_prj_says_only_the_refusal(tmp_path):
    """Add a file while the .prj is 0400: the refusal, and nothing after it.
    "Added to project: ..." used to follow the refusal and contradict it."""
    _need_unwritable_prj(tmp_path)
    prj = tmp_path / 'd.prj'
    with _session(tmp_path) as w:
        _open_d_prj(w)
        os.chmod(str(prj), 0o400)
        try:
            w.key('\033a', delay=1.5)       # Add: the file manager
            w.key(KEY_RETURN, delay=1.5)
            w.key(KEY_RETURN, delay=1.5)    # add the file under the cursor
            refusal = _screen_text(w)
            w.key('\x1b', delay=1.5)        # dismiss the box
            after = _screen_text(w)
        finally:
            os.chmod(str(prj), 0o600)
        assert w.alive(), after
    assert 'Cannot write' in refusal and 'Permission denied' in refusal, refusal
    assert 'Added to project' not in refusal + after, after
    assert prj.read_text() == PRJ_TEXT


# -- a close the project's file refuses stops every loop that closes ----------

KEY_ALT_X = '\033x'
KEY_ESC = '\x1b'
KEY_ALT_SYSTEM = '\033#'
SEED_TEXT = 'int main(void){return 0;}\n'


def _refuse_d_list(w, prj):
    """d.prj is open; its file goes read-only; a Delete is refused (said once
    and dismissed), so the list is dirty and the file will not take it."""
    _open_d_prj(w)
    os.chmod(str(prj), 0o400)
    w.key('\033d', delay=1.5)
    w.key(KEY_ESC, delay=1.0)


def _project_windows(text):
    """How many project windows are drawn: their title sits between frame
    lines, unlike the status bar's label."""
    return len(re.findall(r'q Project: d\.prj q', text))


# @scenario SC-unsaved-text-17
def test_quit_with_a_refused_project_list_keeps_the_editor_and_the_text(tmp_path):
    """Alt-X with a project list the file refuses: the writer's box, and
    the editor goes on with the text window's typed text.  The refusal used to
    read as "closed", so the loop went on and the editor ended without ever
    asking about t.c."""
    _need_unwritable_prj(tmp_path)
    prj = tmp_path / 'd.prj'
    with _session(tmp_path) as w:
        w.key('Q', delay=0.5)               # unsaved text in t.c
        _refuse_d_list(w, prj)
        try:
            w.key(KEY_ALT_X, delay=2.0)
            box = _screen_text(w)
            alive_after_box = w.alive()
            w.key(KEY_ESC, delay=1.5)
            after = _screen_text(w)
            alive = w.alive()
        finally:
            os.chmod(str(prj), 0o600)
        assert alive_after_box and alive, after
        w.key(KEY_ALT_1, delay=1.0)
        text_window = _screen_text(w)
        w.key(KEY_ALT_X, delay=2.0)         # the file takes the list now
        question = _screen_text(w)
        w.key('n', delay=2.0)
        for _ in range(20):                 # the editor takes a moment to leave
            if not w.alive():
                break
            time.sleep(0.5)
        ended = not w.alive()
    assert 'Cannot write' in box and 'still in its window' in box, box
    assert 'NOT saved' not in box, box
    assert _project_windows(after) == 1, after
    assert 'Qint main' in text_window, text_window
    assert (tmp_path / 't.c').read_text() == SEED_TEXT
    assert 'NOT saved' in question, question
    assert ended
    assert 'extra.c' not in prj.read_text(), prj.read_text()


# @scenario SC-unsaved-text-17
def test_clear_desktop_with_a_refused_project_list_keeps_the_window(tmp_path):
    """Clear Desktop stops at a project list the file refuses: the
    project window is still there, the desktop is not orphaned."""
    _need_unwritable_prj(tmp_path)
    prj = tmp_path / 'd.prj'
    with _session(tmp_path) as w:
        w.key('Q', delay=0.5)
        _refuse_d_list(w, prj)
        try:
            w.key(KEY_ALT_1, delay=1.0)
            w.key(KEY_ALT_SYSTEM, delay=1.0)
            w.key('c', delay=2.0)
            question = _screen_text(w)
            w.key('y', delay=2.0)           # t.c is writable: saved
            box = _screen_text(w)
            w.key(KEY_ESC, delay=1.5)
            after = _screen_text(w)
            alive = w.alive()
        finally:
            os.chmod(str(prj), 0o600)
        assert alive, after
        w.key('\x17', delay=2.0)            # Ctrl-W: the file takes the list now
        cleared = _screen_text(w)
        assert w.alive(), cleared
    assert 'NOT saved' in question, question
    assert 'Cannot write' in box and 'still in its window' in box, box
    assert _project_windows(after) == 1, after
    assert (tmp_path / 't.c').read_text().startswith('Q'), (tmp_path / 't.c').read_text()
    assert _project_windows(cleared) == 0, cleared
    assert 'extra.c' not in prj.read_text(), prj.read_text()


def _open_via_file_manager(w, name):
    w.key(KEY_ALT_1, delay=1.0)
    w.key('\033[13~', delay=1.5)            # F3: the File Manager
    w.key(CLEAR_NAME + name + KEY_RETURN, delay=2.0)


# @scenario SC-unsaved-text-17
def test_open_prj_from_the_file_manager_around_a_refused_close_keeps_the_old(tmp_path):
    """F3 on b.prj while d.prj's list is refused: the writer's box, then
    "b.prj was not opened."; one project window, d.prj; b.prj and d.prj as
    they were; the Project option still d.prj."""
    _need_unwritable_prj(tmp_path)
    (tmp_path / 'b.prj').write_text(B_TEXT)
    prj = tmp_path / 'd.prj'
    with _session(tmp_path) as w:
        _refuse_d_list(w, prj)
        try:
            _open_via_file_manager(w, 'b.prj')
            first = _screen_text(w)
            w.key(KEY_ESC, delay=1.5)
            second = _screen_text(w)
            w.key(KEY_ESC, delay=1.0)
            after = _screen_text(w)
            w.key(KEY_ALT_1, delay=1.0)
            w.key('\033o', delay=1.0)
            w.key('s', delay=1.5)
            w.key(KEY_RETURN, delay=1.5)
        finally:
            os.chmod(str(prj), 0o600)
        assert w.alive(), after
    assert 'Cannot write' in first, first
    assert 'b.prj was not opened.' in second, second
    assert _project_windows(after) == 1 and 'Project: b.prj' not in after.split('F1 Help')[0], after
    assert (tmp_path / 'b.prj').read_text() == B_TEXT
    assert prj.read_text() == PRJ_TEXT
    assert _saved_option(tmp_path, 'Project').endswith('d.prj')


def _saved_option(tmp_path, key):
    with open(os.path.join(str(tmp_path), '.config', 'xwpe', 'xwperc')) as fh:
        for line in fh:
            if line.startswith(key):
                return line.split(':', 1)[1].strip()
    return None


# @scenario SC-unsaved-text-17
def test_open_prj_from_the_file_manager_when_the_old_one_is_clean(tmp_path):
    """The control: F3 on b.prj with d.prj clean opens b.prj."""
    (tmp_path / 'b.prj').write_text(B_TEXT)
    (tmp_path / 'util.c').write_text('int x;\n')
    with _session(tmp_path) as w:
        _open_d_prj(w)
        _open_via_file_manager(w, 'b.prj')
        text = _screen_text(w)
        assert w.alive(), text
    assert 'Project: b.prj' in text.split('F1 Help')[0], text
    assert 'Project: d.prj' not in text.split('F1 Help')[0], text


# @scenario SC-unsaved-text-17
def test_a_missing_prj_on_the_command_line_is_refused_not_made_up(tmp_path):
    """`wpe missing.prj t.c`: "Project file not found: <path>" and no
    project window; t.c is open.  Control: an existing d.prj opens."""
    with _session(tmp_path, args=('missing.prj',)) as w:
        text = _screen_text(w)
        w.key(KEY_RETURN, delay=1.5)        # dismiss the box
        after = _screen_text(w)
        assert w.alive(), text
    assert 'Project file not found' in text and 'missing.prj' in text, text
    assert 'Project: missing.prj' not in text + after, text
    assert 't.c' in after, after
    assert not (tmp_path / 'missing.prj').exists()
    with _session(tmp_path, args=('d.prj',)) as w:
        text = _screen_text(w)
    assert 'Project: d.prj' in text, text


# @scenario SC-unsaved-text-17
def test_opening_the_open_project_brings_it_forward_without_a_release(tmp_path):
    """Open Project d.prj while d.prj is open with a list its file
    refuses: no "was not opened", no second refusal; the project window is in
    front and, once the file takes it, closing writes the list."""
    _need_unwritable_prj(tmp_path)
    prj = tmp_path / 'd.prj'
    with _session(tmp_path) as w:
        _refuse_d_list(w, prj)
        try:
            w.key(KEY_ALT_1, delay=1.0)
            w.key(KEY_ALT_P, delay=1.0)
            w.key('p', delay=1.5)
            w.key(CLEAR_NAME + 'd.prj' + KEY_RETURN, delay=2.0)
            text = _screen_text(w)
        finally:
            os.chmod(str(prj), 0o600)
        w.key('\x17', delay=1.5)            # Ctrl-W: close, writing the list
        assert w.alive(), text
    assert 'was not opened' not in text and 'Cannot write' not in text, text
    assert _project_windows(text) == 1, text
    assert 'extra.c' not in prj.read_text(), prj.read_text()


# @scenario SC-unsaved-text-17
def test_opening_the_open_project_after_its_file_was_replaced(tmp_path):
    """d.prj is replaced on disk by a rename (a new inode) while it is
    open and dirty; Open Project d.prj brings the open project forward and
    does not say it was not opened."""
    _need_unwritable_prj(tmp_path)
    prj = tmp_path / 'd.prj'
    with _session(tmp_path) as w:
        _refuse_d_list(w, prj)
        new = tmp_path / 'd.prj.new'
        new.write_text(PRJ_TEXT)
        os.chmod(str(new), 0o400)
        os.rename(str(new), str(prj))       # a new inode under the same name
        try:
            w.key(KEY_ALT_1, delay=1.0)
            w.key(KEY_ALT_P, delay=1.0)
            w.key('p', delay=1.5)
            w.key(CLEAR_NAME + 'd.prj' + KEY_RETURN, delay=2.0)
            text = _screen_text(w)
        finally:
            os.chmod(str(prj), 0o600)
        assert w.alive(), text
    assert 'was not opened' not in text and 'Cannot write' not in text, text
    assert _project_windows(text) == 1, text


# @scenario SC-unsaved-text-17
def test_new_project_that_cannot_be_written_leaves_the_open_project(tmp_path):
    """New Project in a folder closed to writing while d.prj is open and
    clean: the box says no project was created; d.prj's window is still
    there and the Project option is still d.prj."""
    (tmp_path / 'extra.c').write_text('int x;\n')
    with _session(tmp_path) as w:
        _open_d_prj(w)
        folder_mode_is_obeyed(str(tmp_path), 0o500, create=True)
        os.chmod(str(tmp_path), 0o500)
        try:
            w.key(KEY_ALT_1, delay=1.0)
            w.key(KEY_ALT_P, delay=1.0)
            w.key('n', delay=1.5)
            w.key(CLEAR_NAME + 'new.prj' + KEY_RETURN, delay=2.0)
            box = _screen_text(w)
        finally:
            os.chmod(str(tmp_path), 0o700)
        w.key(KEY_RETURN, delay=1.0)
        w.key('\0332', delay=1.0)           # the second window: the project's
        after = _screen_text(w)
        w.key(KEY_ALT_1, delay=1.0)
        w.key('\033o', delay=1.0)
        w.key('s', delay=1.5)
        w.key(KEY_RETURN, delay=1.5)
        assert w.alive(), after
    assert 'Cannot write' in box and 'No project was created.' in box, box
    assert _project_windows(after) == 1, after
    assert not (tmp_path / 'new.prj').exists()
    assert _saved_option(tmp_path, 'Project').endswith('d.prj')


# @scenario SC-unsaved-text-17
def test_close_project_with_a_refused_list_keeps_the_project(tmp_path):
    """Close Project while the file refuses the list: the box, and the
    project stays open: the window and the status label are both still
    there.  With the file writable again Close Project closes it."""
    _need_unwritable_prj(tmp_path)
    prj = tmp_path / 'd.prj'
    with _session(tmp_path) as w:
        _refuse_d_list(w, prj)
        try:
            w.key(KEY_ALT_1, delay=1.0)
            w.key(KEY_ALT_P, delay=1.0)
            w.key('c', delay=2.0)
            box = _screen_text(w)
            w.key(KEY_ESC, delay=1.5)
            after = _screen_text(w)
        finally:
            os.chmod(str(prj), 0o600)
        w.key(KEY_ALT_1, delay=1.0)
        w.key(KEY_ALT_P, delay=1.0)
        w.key('c', delay=2.0)
        closed = _screen_text(w)
        assert w.alive(), closed
    assert 'Cannot write' in box, box
    assert after.count('Project: d.prj') == 2, after
    assert 'Project: d.prj' not in closed, closed


def _two_folders(tmp_path):
    """T with extra.c, util.c and d.prj (three members), and T/sub/d.prj with
    one: two projects of the same name in two folders."""
    for fn in ('extra.c', 'util.c'):
        (tmp_path / fn).write_text('int x;\n')
    (tmp_path / 'd.prj').write_text(PRJ_TEXT)
    (tmp_path / 'sub').mkdir()
    (tmp_path / 'sub' / 'd.prj').write_text('CMP=\tgcc\nEXENAME=\ts\nFILES=\tutil.c\n')


# @scenario SC-path-cuts-19
def test_a_project_of_the_same_name_in_another_folder_is_another_project(tmp_path):
    """`wpe d.prj t.c` in T, then F3 and sub/d.prj: the project of sub/ opens
    (one member).  The first project had been kept as the bare name d.prj and
    was read against the folder the File Manager had moved to, so T's own
    project was taken for the one asked for and sub/d.prj never opened."""
    _two_folders(tmp_path)
    with WpeSession(str(tmp_path), SEED_TEXT, filename='t.c', wait=2.0, args=('d.prj',),
                    **COLS_ROWS) as w:
        w.key(KEY_ALT_1, delay=1.0)
        w.key('\033[13~', delay=1.5)        # F3
        w.key(CLEAR_NAME + 'sub/d.prj' + KEY_RETURN, delay=2.5)    # into sub/
        w.key(KEY_RETURN, delay=2.5)        # open it
        text = _screen_text(w)
        assert w.alive(), text
    assert 'util.c' in text, text
    assert 'extra.c' not in text, text


# @scenario SC-path-cuts-19
def test_a_project_opened_by_name_is_the_same_one_from_another_folder(tmp_path):
    """d.prj opened from the File Manager by its bare name, its list refused
    by the file; the File Manager goes to sub/ and asks for ../d.prj: the same
    project, brought forward, with no second refusal.  Kept as the bare name,
    it was read against sub/ and taken for another project."""
    _need_unwritable_prj(tmp_path)
    _two_folders(tmp_path)
    prj = tmp_path / 'd.prj'
    with WpeSession(str(tmp_path), SEED_TEXT, filename='t.c', wait=2.0, **COLS_ROWS) as w:
        w.key('\033[13~', delay=1.5)
        w.key(CLEAR_NAME + 'd.prj' + KEY_RETURN, delay=2.5)        # by its bare name
        os.chmod(str(prj), 0o400)
        try:
            w.key('\033d', delay=1.5)       # a Delete the file refuses
            w.key(KEY_ESC, delay=1.0)
            w.key('\033[13~', delay=1.5)
            w.key(CLEAR_NAME + 'sub/' + KEY_RETURN, delay=2.5)     # the manager moves
            w.key('\x7f' * 8 + '../d.prj' + KEY_RETURN, delay=2.5)
            w.key(KEY_RETURN, delay=2.5)
            text = _screen_text(w)
        finally:
            os.chmod(str(prj), 0o600)
        assert w.alive(), text
    assert 'Cannot write' not in text and 'was not opened' not in text, text
    assert 'extra.c' not in text and 'util.c' in text, text


def _seed_project_key(tmp_path, value):
    """The user's xwperc names the project by a relative name."""
    folder = tmp_path / '.config' / 'xwpe'
    folder.mkdir(parents=True)
    (folder / 'xwperc').write_text('[Programming]\nProject : %s\n' % value)


# @scenario SC-path-cuts-19
def test_a_project_from_the_xwperc_key_is_not_another_folders_project(tmp_path):
    """xwperc says `Project : d.prj`; Window > Project opens it; the File
    Manager goes to sub/ and opens d.prj: that is sub/d.prj, the project of
    that folder (one member).  The stored relative name was read against the
    folder the File Manager had moved to, and T's project was taken for it."""
    _two_folders(tmp_path)
    _seed_project_key(tmp_path, 'd.prj')
    with WpeSession(str(tmp_path), SEED_TEXT, filename='t.c', wait=2.0, **COLS_ROWS) as w:
        w.key('\033w', delay=1.0)
        w.key('p', delay=2.0)               # Window > Project
        w.key('\033[13~', delay=1.5)        # F3
        w.key(CLEAR_NAME + 'sub/d.prj' + KEY_RETURN, delay=2.5)
        w.key(KEY_RETURN, delay=2.5)
        text = _screen_text(w)
        assert w.alive(), text
    assert 'util.c' in text, text
    assert 'extra.c' not in text, text


# @scenario SC-path-cuts-19
def test_a_relative_project_is_still_the_same_one_from_its_own_window_folder(tmp_path):
    """The reverse: xwperc names `d.prj`, Window > Project opens T/d.prj, and
    the File Manager, moved into sub/ where another d.prj lives, asks for
    ../d.prj (T's): the open project, brought forward, with its list kept."""
    _two_folders(tmp_path)
    _seed_project_key(tmp_path, 'd.prj')
    with WpeSession(str(tmp_path), SEED_TEXT, filename='t.c', wait=2.0, **COLS_ROWS) as w:
        w.key('\033w', delay=1.0)
        w.key('p', delay=2.0)
        w.key('\033d', delay=1.5)           # drop extra.c from the open list
        w.key('\033[13~', delay=1.5)
        w.key(CLEAR_NAME + 'sub/' + KEY_RETURN, delay=2.5)
        w.key('\x7f' * 8 + '../d.prj' + KEY_RETURN, delay=2.5)
        w.key(KEY_RETURN, delay=2.5)
        text = _screen_text(w)
        assert w.alive(), text
    assert 'was not opened' not in text, text
    assert 'extra.c' not in text and 't.c' in text, text


# @scenario SC-unsaved-text-17
def test_a_prj_member_opened_from_the_project_window_opens_as_text(tmp_path):
    """Enter on a .prj member inside the project window opens it as a text
    window and leaves the project and its window alone (it used to release
    the window that was still running its own key loop: a use after free,
    seen under the sanitizer)."""
    (tmp_path / 'sub.prj').write_text(B_TEXT)
    (tmp_path / 'extra.c').write_text('int x;\n')
    (tmp_path / 'd.prj').write_text('CMP=\tgcc\nEXENAME=\td\nFILES=\tsub.prj t.c\n')
    with WpeSession(str(tmp_path), SEED_TEXT, filename='t.c', wait=2.0, **COLS_ROWS) as w:
        _open_d_prj(w)
        w.key(KEY_RETURN, delay=2.0)        # the first member: sub.prj
        text = _screen_text(w)
        assert w.alive(), text
        assert 'AddressSanitizer' not in bytes(w.raw).decode('utf-8', 'replace')
    assert 'CMP' in text and 'EXENAME' in text, text      # the .prj as text
