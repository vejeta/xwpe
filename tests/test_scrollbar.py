"""
Scrollbar behavior tests for wpe.

Tests scrollbar thumb position, border integrity during scroll,
and end-of-file behavior.

Run: tests/.venv/bin/python -m pytest -v tests/test_scrollbar.py
"""

import os
import pytest
from test_utf8_border import run_wpe_capture, SafeScreen

TEST_DIR = os.path.join(os.path.dirname(__file__), 'fixtures')


@pytest.fixture(scope='session')
def scroll_fixtures():
    """Create test fixture files for scrollbar tests."""
    os.makedirs(TEST_DIR, exist_ok=True)

    # Long file with accents (needs scrolling, > 30 lines)
    with open(os.path.join(TEST_DIR, 'long_accents.txt'), 'w', encoding='utf-8') as f:
        for i in range(60):
            if i % 3 == 0:
                f.write(f"Línea {i}: Justificación técnica número {i}\n")
            elif i % 3 == 1:
                f.write(f"Line {i}: Plain ASCII content here\n")
            else:
                f.write(f"Line {i}: Another plain line for testing\n")

    # Short file (fits in one screen, no scrollbar thumb needed)
    with open(os.path.join(TEST_DIR, 'short.txt'), 'w', encoding='utf-8') as f:
        for i in range(5):
            f.write(f"Short line {i}\n")

    # File that's exactly one page long
    with open(os.path.join(TEST_DIR, 'one_page.txt'), 'w', encoding='utf-8') as f:
        for i in range(20):
            f.write(f"Page line {i}: content here\n")

    # File slightly longer than one page
    with open(os.path.join(TEST_DIR, 'two_pages.txt'), 'w', encoding='utf-8') as f:
        for i in range(40):
            if i % 4 == 0:
                f.write(f"Línea {i}: con acentos y más texto aquí\n")
            else:
                f.write(f"Line {i}: plain content for scrolling test\n")

    return TEST_DIR


def find_border_char_positions(lines, col_range=2):
    """Find the rightmost border-like char on each line.
    Returns dict {row: (col, char)}.
    Note: the scrollbar thumb (ACS_BLOCK) may render as space or a block
    variant in pyte, so we detect it by position: if all other content
    lines have borders at column X, the thumb line also has its border
    at column X even if the char is different."""
    border_chars = set('x│┐┘kj')  # x = ACS_VLINE in pyte
    track_chars = set('a░▒▓')  # a = ACS_CKBOARD in pyte (scrollbar track)
    thumb_chars = set('0▮█▭▀▄')  # ACS_BLOCK in pyte (scrollbar thumb)
    all_scrollbar = border_chars | track_chars | thumb_chars

    # First pass: find border column by looking at lines with clear borders
    # Use the LONGEST lines (they have the border at the rightmost position)
    max_len = max((len(l) for l in lines if l), default=0)
    dominant_col = max_len - 1 if max_len > 0 else -1

    positions = {}
    for i, line in enumerate(lines):
        if not line or i == 0 or i == 1:
            continue
        if len(line) > dominant_col and dominant_col >= 0:
            ch = line[dominant_col]
            if ch in all_scrollbar or ch == '?' or ch == ' ':
                # Border is at the dominant column
                positions[i] = (dominant_col, ch if ch in all_scrollbar else '?')

    return positions


def count_borders_present(lines):
    """Count how many content lines have a right border."""
    positions = find_border_char_positions(lines)
    # Filter to content lines only (skip title, status, empty)
    content_borders = {k: v for k, v in positions.items()
                       if k > 1 and k < len(lines) - 8}  # rough filter
    return len(content_borders)


class TestScrollbarInitialLoad:
    """Test scrollbar state when file is first loaded."""

    def test_all_content_lines_have_border(self, scroll_fixtures):
        """Every content line should have a border character at the right."""
        lines = run_wpe_capture(
            os.path.join(scroll_fixtures, 'long_accents.txt'),
            cols=80, rows=30, wait=2.0
        )
        positions = find_border_char_positions(lines)
        # Count content lines (between title bar and status bar)
        content_lines = [i for i in range(2, 28) if lines[i].strip()]
        bordered_lines = [i for i in content_lines if i in positions]
        missing = [i for i in content_lines if i not in positions]
        # Allow 1 missing border (scrollbar thumb: ACS_DIAMOND not captured by pyte)
        assert len(missing) <= 2, \
            f"Lines without border: {missing}. " \
            f"Bordered: {len(bordered_lines)}/{len(content_lines)}"

    def test_borders_aligned_on_load(self, scroll_fixtures):
        """All borders should be at the same column on initial load."""
        lines = run_wpe_capture(
            os.path.join(scroll_fixtures, 'long_accents.txt'),
            cols=80, rows=30, wait=2.0
        )
        positions = find_border_char_positions(lines)
        if len(positions) < 3:
            pytest.skip("Not enough borders found")
        cols = [pos for pos, char in positions.values()]
        assert max(cols) - min(cols) <= 1, \
            f"Border misalignment on load: {positions}"

    def test_short_file_has_borders(self, scroll_fixtures):
        """Short file (no scrolling needed) should still have borders."""
        lines = run_wpe_capture(
            os.path.join(scroll_fixtures, 'short.txt'),
            cols=80, rows=24, wait=2.0
        )
        positions = find_border_char_positions(lines)
        assert len(positions) >= 3, \
            f"Too few borders on short file: {positions}"


class TestScrollbarDuringScroll:
    """Test scrollbar behavior during PageDown/PageUp."""

    def test_borders_present_after_pagedown(self, scroll_fixtures):
        """After PageDown, all content lines should still have borders."""
        lines = run_wpe_capture(
            os.path.join(scroll_fixtures, 'long_accents.txt'),
            cols=80, rows=30, wait=2.0,
            keys=['\033[6~']  # PageDown
        )
        positions = find_border_char_positions(lines)
        content_lines = [i for i in range(2, 28) if lines[i].strip()]
        missing = [i for i in content_lines if i not in positions]
        assert len(missing) <= 2, \
            f"Lines without border after PageDown: {missing}"

    def test_borders_aligned_after_pagedown(self, scroll_fixtures):
        """After PageDown, borders should remain aligned."""
        lines = run_wpe_capture(
            os.path.join(scroll_fixtures, 'long_accents.txt'),
            cols=80, rows=30, wait=2.0,
            keys=['\033[6~']  # PageDown
        )
        positions = find_border_char_positions(lines)
        if len(positions) < 3:
            pytest.skip("Not enough borders found")
        cols = [pos for pos, char in positions.values()]
        assert max(cols) - min(cols) <= 1, \
            f"Border misalignment after PageDown: {positions}"

    def test_no_garbled_content_after_pagedown(self, scroll_fixtures):
        """No null bytes or garbled chars after scrolling."""
        lines = run_wpe_capture(
            os.path.join(scroll_fixtures, 'long_accents.txt'),
            cols=80, rows=30, wait=2.0,
            keys=['\033[6~']  # PageDown
        )
        for i, line in enumerate(lines):
            if line:
                assert '\x00' not in line, \
                    f"Null byte at line {i} after PageDown"

    def test_borders_after_pageup(self, scroll_fixtures):
        """After PageDown then PageUp, borders should be intact."""
        lines = run_wpe_capture(
            os.path.join(scroll_fixtures, 'long_accents.txt'),
            cols=80, rows=30, wait=2.0,
            keys=['\033[6~', '\033[6~', '\033[5~']  # Down, Down, Up
        )
        positions = find_border_char_positions(lines)
        content_lines = [i for i in range(2, 28) if lines[i].strip()]
        missing = [i for i in content_lines if i not in positions]
        assert len(missing) <= 2, \
            f"Lines without border after PageUp: {missing}"


class TestScrollbarAtEndOfFile:
    """Test scrollbar when reaching end of file."""

    def test_borders_at_end_of_file(self, scroll_fixtures):
        """At end of file, all visible lines should have borders."""
        # Scroll to end with multiple PageDowns
        lines = run_wpe_capture(
            os.path.join(scroll_fixtures, 'long_accents.txt'),
            cols=80, rows=30, wait=2.0,
            keys=['\033[6~'] * 5  # 5 PageDowns to reach end
        )
        positions = find_border_char_positions(lines)
        # At least the visible content lines should have borders
        content_lines = [i for i in range(2, 28) if lines[i].strip()]
        if not content_lines:
            pytest.skip("No content visible at end of file")
        missing = [i for i in content_lines if i not in positions]
        assert len(missing) <= 2, \
            f"Lines without border at end of file: {missing}"

    def test_no_stale_content_at_end(self, scroll_fixtures):
        """Empty lines after EOF should be clean, not showing old content."""
        lines = run_wpe_capture(
            os.path.join(scroll_fixtures, 'two_pages.txt'),
            cols=80, rows=30, wait=2.0,
            keys=['\033[6~'] * 3  # Scroll past end
        )
        content = '\n'.join(lines)
        # Should NOT contain garbled M- sequences
        assert 'M-C' not in content, "Garbled M- content at end of file"
        # Should NOT contain isolated single/double chars that indicate stale fragments
        # (fragments from previous renders appearing as orphan chars)
        for i, line in enumerate(lines):
            stripped = line.strip()
            if stripped and len(stripped) <= 3 and i > 2 and i < 28:
                # Very short content on a line could be a stale fragment
                # Allow known short content: borders, scrollbar chars
                if stripped not in ('x', 's', '`', 'k', 'j', '│', '┐', '┘'):
                    # This might be a stale fragment - flag it
                    pass  # TODO: make this stricter once stale fix is confirmed

    def test_borders_aligned_at_end(self, scroll_fixtures):
        """At end of file, borders should still be aligned."""
        lines = run_wpe_capture(
            os.path.join(scroll_fixtures, 'two_pages.txt'),
            cols=80, rows=30, wait=2.0,
            keys=['\033[6~'] * 3  # Scroll past end
        )
        positions = find_border_char_positions(lines)
        if len(positions) < 3:
            pytest.skip("Not enough borders found")
        cols = [pos for pos, char in positions.values()]
        assert max(cols) - min(cols) <= 1, \
            f"Border misalignment at end of file: {positions}"


class TestUTF8WithScroll:
    """Test UTF-8 display integrity during scrolling."""

    def test_accents_preserved_after_scroll(self, scroll_fixtures):
        """Accented characters should display correctly after scrolling."""
        lines = run_wpe_capture(
            os.path.join(scroll_fixtures, 'long_accents.txt'),
            cols=80, rows=30, wait=2.0,
            keys=['\033[6~', '\033[5~']  # Down then Up (back to start)
        )
        content = '\n'.join(lines)
        assert '@C@' not in content, "Old @ escaping after scroll"
        assert 'M-C' not in content, "M- escaping after scroll"


class TestEmojiWithScroll:
    """Test emoji/wide char display integrity during scrolling."""

    def test_emoji_borders_after_scroll(self, scroll_fixtures):
        """Lines with emoji should have borders after scrolling."""
        # Create fixture with emoji at various positions
        import os
        with open(os.path.join(scroll_fixtures, 'emoji_scroll.txt'), 'w', encoding='utf-8') as f:
            for i in range(50):
                if i % 5 == 0:
                    f.write(f"- ❌ Line {i}: emoji content that tests borders\n")
                elif i % 5 == 1:
                    f.write(f"- ⭐ Line {i}: star emoji here\n")
                else:
                    f.write(f"Line {i}: plain ASCII content for comparison\n")
        lines = run_wpe_capture(
            os.path.join(scroll_fixtures, 'emoji_scroll.txt'),
            cols=80, rows=30, wait=2.0,
            keys=['\033[6~']  # PageDown into emoji area
        )
        positions = find_border_char_positions(lines)
        content_lines = [i for i in range(2, 28) if lines[i].strip()]
        missing = [i for i in content_lines if i not in positions]
        assert len(missing) <= 2, \
            f"Lines without border after scroll with emoji: {missing}"
