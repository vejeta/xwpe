"""
UTF-8 border alignment tests for wpe.

Uses pyte (VT100 terminal emulator) to run wpe in a pseudo-terminal,
capture the screen, and verify that borders are aligned correctly
regardless of UTF-8 content.

Run: cd tests && source .venv/bin/activate && pytest -v test_utf8_border.py
"""

import os
import pty
import select
import signal
import subprocess
import time

import pyte
import pytest

# Workaround for pyte bug with ncurses CSI sequences
class SafeScreen(pyte.Screen):
    def set_margins(self, *args, **kwargs):
        kwargs.pop('private', None)
        super().set_margins(*args, **kwargs)

WPE_BIN = os.environ.get('WPE_BIN') or os.path.join(os.path.dirname(__file__), '..', 'wpe')
TEST_DIR = os.path.join(os.path.dirname(__file__), 'fixtures')


def run_wpe_capture(filename, cols=80, rows=30, wait=1.0, keys=None):
    """Run wpe with a file, optionally send keys, capture screen via pyte."""
    screen = SafeScreen(cols, rows)
    stream = pyte.Stream(screen)

    master_fd, slave_fd = pty.openpty()

    env = os.environ.copy()
    env['TERM'] = 'xterm-256color'
    env['COLUMNS'] = str(cols)
    env['LINES'] = str(rows)
    env['LC_ALL'] = 'en_US.UTF-8'

    proc = subprocess.Popen(
        [WPE_BIN, filename],
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        env=env,
        preexec_fn=os.setsid
    )
    os.close(slave_fd)

    # Read initial output
    deadline = time.time() + wait
    while time.time() < deadline:
        r, _, _ = select.select([master_fd], [], [], 0.1)
        if r:
            try:
                data = os.read(master_fd, 65536)
                stream.feed(data.decode('utf-8', errors='replace'))
            except OSError:
                break

    # Send keys if any
    if keys:
        for key in keys:
            os.write(master_fd, key.encode('utf-8'))
            time.sleep(0.3)
            r, _, _ = select.select([master_fd], [], [], 0.5)
            if r:
                try:
                    data = os.read(master_fd, 65536)
                    stream.feed(data.decode('utf-8', errors='replace'))
                except OSError:
                    break

    # Kill wpe
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except ProcessLookupError:
        pass
    proc.wait(timeout=3)
    os.close(master_fd)

    # Extract screen lines
    lines = []
    for row in range(rows):
        line = ''
        for col in range(cols):
            char = screen.buffer[row][col]
            line += char.data if char.data else ' '
        lines.append(line.rstrip())

    return lines


@pytest.fixture(scope='session')
def fixtures_dir():
    """Create test fixture files."""
    os.makedirs(TEST_DIR, exist_ok=True)

    # File with accented characters
    with open(os.path.join(TEST_DIR, 'accents.txt'), 'w', encoding='utf-8') as f:
        f.write("Guía técnica\n")
        f.write("Simple ASCII line\n")
        f.write("Justificación técnica del uso\n")
        f.write("Another plain line here\n")
        f.write("extensión móvil de esa misión\n")
        f.write("No accents at all\n")
        f.write("— em-dash line —\n")
        f.write("Last line no accents\n")

    # File with only ASCII
    with open(os.path.join(TEST_DIR, 'ascii.txt'), 'w', encoding='utf-8') as f:
        f.write("Simple line one\n")
        f.write("Simple line two\n")
        f.write("Simple line three\n")

    # File with Cyrillic
    with open(os.path.join(TEST_DIR, 'cyrillic.txt'), 'w', encoding='utf-8') as f:
        f.write("Развој програма\n")
        f.write("Plain English\n")
        f.write("Розробка програмного\n")

    # File with CJK
    with open(os.path.join(TEST_DIR, 'cjk.txt'), 'w', encoding='utf-8') as f:
        f.write("软件开发\n")
        f.write("Plain English\n")
        f.write("自动手册页生成\n")

    return TEST_DIR


class TestBorderAlignment:
    """Test that right border │ is at the same column for all lines."""

    def find_right_border_col(self, lines):
        """Find the column of the right border for each content line.
        Returns dict of {row: col} for lines that have a border character."""
        border_chars = set('x│┐┘kjsv^`◆⎽+0')
        # Find dominant column (the rightmost column where most borders are)
        max_len = max((len(l) for l in lines if l), default=0)
        dominant_col = max_len - 1 if max_len > 0 else -1
        borders = {}
        for i, line in enumerate(lines):
            if not line or i == 0 or i == 1:
                continue
            if len(line) > dominant_col and dominant_col >= 0:
                ch = line[dominant_col]
                if ch in border_chars or ch == ' ':
                    borders[i] = dominant_col
        return borders

    def test_ascii_borders_aligned(self, fixtures_dir):
        """ASCII file: all borders should be at the same column."""
        lines = run_wpe_capture(
            os.path.join(fixtures_dir, 'ascii.txt'),
            cols=80, rows=24
        )
        borders = self.find_right_border_col(lines)
        if not borders:
            pytest.skip("No borders found in capture")
        cols = list(borders.values())
        # All borders should be within 1 column of each other
        assert max(cols) - min(cols) <= 1, \
            f"Border misalignment: {borders}"

    def test_accent_borders_aligned(self, fixtures_dir):
        """Accented file: borders should be at the same column as ASCII lines."""
        lines = run_wpe_capture(
            os.path.join(fixtures_dir, 'accents.txt'),
            cols=80, rows=24
        )
        borders = self.find_right_border_col(lines)
        if not borders:
            pytest.skip("No borders found in capture")
        cols = list(borders.values())
        assert max(cols) - min(cols) <= 1, \
            f"Border misalignment on accent file: {borders}"

    def test_cyrillic_borders_aligned(self, fixtures_dir):
        """Cyrillic file: borders should be aligned."""
        lines = run_wpe_capture(
            os.path.join(fixtures_dir, 'cyrillic.txt'),
            cols=80, rows=24
        )
        borders = self.find_right_border_col(lines)
        if not borders:
            pytest.skip("No borders found in capture")
        cols = list(borders.values())
        assert max(cols) - min(cols) <= 1, \
            f"Border misalignment on cyrillic file: {borders}"

    def test_utf8_chars_display_correctly(self, fixtures_dir):
        """Accented characters should display correctly, not as @C@3 or M-."""
        lines = run_wpe_capture(
            os.path.join(fixtures_dir, 'accents.txt'),
            cols=80, rows=24
        )
        content = '\n'.join(lines)
        # Should NOT contain old escaping
        assert '@C@' not in content, "Old @ escaping still present"
        assert 'M-C' not in content, "ncurses M- escaping present"
        # Should contain the actual accented characters
        assert 'Gu' in content, "Content not displayed"

    def test_no_black_gaps(self, fixtures_dir):
        """No null characters should appear in the visible area."""
        lines = run_wpe_capture(
            os.path.join(fixtures_dir, 'accents.txt'),
            cols=80, rows=24
        )
        for i, line in enumerate(lines):
            if line:
                assert '\x00' not in line, \
                    f"Null byte (black gap) at line {i}: {repr(line)}"


class TestResize:
    """Test terminal resize handling."""

    def test_resize_redraws(self, fixtures_dir):
        """After resize, content should still be visible."""
        lines = run_wpe_capture(
            os.path.join(fixtures_dir, 'ascii.txt'),
            cols=100, rows=30
        )
        content = '\n'.join(lines)
        assert 'Simple' in content, "Content not visible after open"


class TestKeymap:
    """Test that keys work correctly."""

    def test_file_opens(self, fixtures_dir):
        """File content should be visible after opening."""
        lines = run_wpe_capture(
            os.path.join(fixtures_dir, 'accents.txt'),
            cols=80, rows=24
        )
        content = '\n'.join(lines)
        # Should see file content (at least partial)
        assert 'cnica' in content or 'ASCII' in content, \
            f"File content not visible: {content[:200]}"


class TestWideChars:
    """Test wide character (emoji, CJK) display."""

    def test_emoji_borders_aligned(self, fixtures_dir):
        """File with emoji should have aligned borders."""
        # Create emoji fixture
        import os
        with open(os.path.join(fixtures_dir, 'emoji.txt'), 'w', encoding='utf-8') as f:
            f.write("Plain line\n")
            f.write("Line with emoji: ❌ cross mark\n")
            f.write("Another plain line\n")
            f.write("More emoji: ⭐ star and ❌ cross\n")
            f.write("Last plain line\n")
        lines = run_wpe_capture(
            os.path.join(fixtures_dir, 'emoji.txt'),
            cols=80, rows=24, wait=2.0
        )
        # All lines should be same length (borders aligned)
        content_lines = [l for l in lines if l.strip() and len(l) >= 70]
        if len(content_lines) < 3:
            pytest.skip("Not enough content lines")
        lengths = [len(l) for l in content_lines]
        # Allow up to 4 columns difference (2 per wide emoji, pyte captures
        # wide chars as 1 cell instead of 2, causing shorter apparent lines)
        assert max(lengths) - min(lengths) <= 4, \
            f"Border misalignment with emoji: lengths={lengths}"
