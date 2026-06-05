"""Project persistence regression test for wpe (terminal mode).

#137: adding a file to a project must write the .prj to disk immediately, not
only when the project window is closed -- otherwise an interrupted session
loses the additions. This drives Open project -> Add file and reads the .prj
back from disk WITHOUT closing the window.

Run: tests/.venv/bin/python -m pytest -v tests/test_project_persist.py
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
COLS, ROWS = 80, 30


@pytest.fixture
def project_dir(tmp_path):
    for fn in ('main.c', 'extra.c', 'util.c'):
        (tmp_path / fn).write_text('int x;\n')
    (tmp_path / 'p.prj').write_text(
        '#\n# xwpe - project-file: p.prj\nCMP=\tgcc\nEXENAME=\tp\nFILES=\tmain.c\n')
    return tmp_path


def _files_line(prj_path):
    for line in prj_path.read_text().splitlines():
        if line.startswith('FILES'):
            return line
    return ''


def test_add_file_persists_to_disk_without_closing(project_dir):
    screen = SafeScreen(COLS, ROWS)
    stream = pyte.Stream(screen)
    master_fd, slave_fd = pty.openpty()
    env = os.environ.copy()
    env.update(TERM='xterm-256color', COLUMNS=str(COLS), LINES=str(ROWS),
               LC_ALL='en_US.UTF-8', HOME=str(project_dir))
    proc = subprocess.Popen(
        [WPE_BIN, 'main.c'],
        stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
        cwd=str(project_dir), env=env, preexec_fn=os.setsid)
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

    prj = project_dir / 'p.prj'
    try:
        drain(1.5)
        # Open project: Alt-P P, Enter (Files box), Enter (open p.prj)
        for k in ('\033p', 'p', '\r', '\r'):
            os.write(master_fd, k.encode())
            drain(0.6)
        assert 'main.c' in _files_line(prj)
        assert 'extra.c' not in _files_line(prj)

        # Add: Alt-A -> Add FM; Enter -> Files box (extra.c first); Enter -> Add
        for k in ('\033a', '\r', '\r'):
            os.write(master_fd, k.encode())
            drain(0.6)

        # The project window is STILL open -- the .prj on disk must already
        # contain the added file.
        line = _files_line(prj)
        assert 'extra.c' in line, (
            'added file was not persisted to disk before closing the project '
            'window (#137 regression): FILES line is %r' % line)
        assert 'main.c' in line, 'existing file lost on Add: %r' % line
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), 9)
        except Exception:
            pass
        os.close(master_fd)
        proc.wait()
