"""File-Manager initial layout: centred, and grows with the terminal (capped).

Opening wpe/we with no file (or pressing F3) shows the File-Manager.  It used to
be a fixed ~56x21 box pinned near the top-left, wasting a big terminal.  It is
now centred and sized to ~3/4 of the screen, clamped to a sane min/max -- so on
a tall terminal the DirTree and Files list boxes get many more rows.

Geometry lives in e_fm_centered_geometry (we_fl_unix.c); every inner widget
already scales off the window box, so this one function drives it.

Run: tests/.venv/bin/python -m pytest -v tests/test_file_manager_layout.py
"""
import os
import pty
import select
import struct
import fcntl
import termios
import subprocess
import time

import pyte
import pytest

from test_utf8_border import SafeScreen

WE = os.path.join(os.path.dirname(__file__), "..", "we")
FM_TITLE = "File-Manager"


def _open_fm(cols, rows):
    """Launch `we` with no file at cols x rows; return the screen display lines."""
    screen = SafeScreen(cols, rows)
    stream = pyte.Stream(screen)
    mfd, sfd = pty.openpty()
    fcntl.ioctl(sfd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    env = dict(os.environ, TERM="xterm-256color", COLUMNS=str(cols),
               LINES=str(rows), LC_ALL="en_US.UTF-8", HOME="/tmp")
    p = subprocess.Popen([WE], stdin=sfd, stdout=sfd, stderr=sfd, env=env,
                         close_fds=True)
    os.close(sfd)
    try:
        time.sleep(2.0)
        while True:
            r, _, _ = select.select([mfd], [], [], 0.4)
            if not r:
                break
            try:
                data = os.read(mfd, 65536)
            except OSError:
                break
            if not data:
                break
            stream.feed(data.decode("utf-8", "replace"))
        return list(screen.display)
    finally:
        p.terminate()
        try:
            p.wait(timeout=3)
        except subprocess.TimeoutExpired:
            p.kill()
        os.close(mfd)


def _box_metrics(lines, cols):
    """(left, right, width, height) of the File-Manager box from its frame."""
    title = next((i for i, l in enumerate(lines) if FM_TITLE in l), None)
    assert title is not None, "File-Manager did not open:\n%s" % "\n".join(lines)
    row = lines[title]
    left = len(row) - len(row.lstrip())
    right = len(row.rstrip()) - 1
    width = right - left
    # The bottom border is a second full row of the horizontal frame char (the
    # char just right of the top-left corner); interior rows have few of it.
    hchar = row[left + 1]
    border_rows = [i for i, l in enumerate(lines)
                   if l.count(hchar) >= max(8, width // 2)]
    bottom = max(border_rows)
    return left, right, width, bottom - title


def test_file_manager_centered_and_taller_on_a_big_terminal():
    small = _open_fm(80, 24)
    big = _open_fm(120, 40)

    sl, sr, sw, sh = _box_metrics(small, 80)
    bl, br, bw, bh = _box_metrics(big, 120)

    # 1. Centred at 120 cols: left margin ~= right margin.
    left_margin, right_margin = bl, 120 - 1 - br
    assert abs(left_margin - right_margin) <= 3, \
        "File-Manager is not centred at 120 cols (left=%d right=%d):\n%s" \
        % (left_margin, right_margin, "\n".join(big))

    # 2. Taller and wider on the big terminal than on 80x24...
    assert bh > sh, \
        "box did not grow taller (80x24 h=%d, 120x40 h=%d)" % (sh, bh)
    assert bw > sw, \
        "box did not grow wider (80x24 w=%d, 120x40 w=%d)" % (sw, bw)

    # 3. ...but capped, not edge-to-edge (the 'sensible height/width' bound).
    assert bh <= 30, "box height not capped (h=%d)" % bh
    assert bw <= 78, "box width not capped (w=%d)" % bw

    # 4. Small terminal stays about its historical size (centred, ~20 tall).
    assert 18 <= sh <= 22, "80x24 box height changed unexpectedly (h=%d)" % sh
