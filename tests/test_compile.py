"""
F9 compile cycle and menu close tests for wpe.

Tests the compile/error-navigation workflow and verifies that
menus and popups close without display artefacts.

Run: tests/.venv/bin/python -m pytest -v tests/test_compile.py

Requires: gcc installed (for compile tests).
"""

import os
import pty
import select
import signal
import subprocess
import time
import shutil
import tempfile

import pyte
import pytest

from test_utf8_border import SafeScreen

WPE_BIN = os.environ.get('WPE_BIN') or os.path.join(os.path.dirname(__file__), '..', 'wpe')

# Key escape sequences (xterm)
KEY_F9 = '\033[20~'        # Make (compile + link)
KEY_ALT_T = '\033t'        # Next Error
KEY_ALT_V = '\033v'        # Previous Error
KEY_CTRL_Q = '\x11'        # Quit
KEY_ESC = '\033\r'         # Escape (xwpe convention: ESC + Return)
KEY_ALT_F_N = '\033fn'     # File -> New
KEY_F10 = '\033[21~'       # Menu bar
KEY_RIGHT = '\033[C'       # Right arrow
KEY_LEFT = '\033[B'        # Left arrow (actually Down, but we need Right)
KEY_DOWN = '\033[B'        # Down arrow
KEY_RETURN = '\r'          # Enter
KEY_ESC_RAW = '\033'       # Raw ESC


# See tests/wpe_driver.py: a loaded CI runner is slow, so XWPE_TEST_WAIT_SCALE
# stretches every wait/timeout (default 1.0; the Debian autopkgtest sets 3).
# All waits here funnel through the inner drain(), so scaling its deadline
# covers the startup wait, the ('wait', N) steps and the per-key delay.
WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1") or 1)


def run_wpe_in_dir(workdir, filename, cols=80, rows=30, wait=1.5, keys=None,
                   key_delay=0.3, read_timeout=0.5):
    """Run wpe in a specific working directory, send keys, capture screen."""
    screen = SafeScreen(cols, rows)
    stream = pyte.Stream(screen)

    master_fd, slave_fd = pty.openpty()

    env = os.environ.copy()
    env['TERM'] = 'xterm-256color'
    env['COLUMNS'] = str(cols)
    env['LINES'] = str(rows)
    env['LC_ALL'] = 'en_US.UTF-8'
    # Prevent xwpe from reading user's config
    env['HOME'] = workdir

    proc = subprocess.Popen(
        [WPE_BIN, filename],
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        cwd=workdir,
        env=env,
        preexec_fn=os.setsid
    )
    os.close(slave_fd)

    def drain(timeout):
        deadline = time.time() + timeout * WAIT_SCALE
        while time.time() < deadline:
            r, _, _ = select.select([master_fd], [], [], 0.1)
            if r:
                try:
                    data = os.read(master_fd, 65536)
                    stream.feed(data.decode('utf-8', errors='replace'))
                except OSError:
                    break

    # Wait for initial display
    drain(wait)

    # Send keys
    if keys:
        for key in keys:
            if isinstance(key, tuple) and key[0] == 'wait':
                drain(key[1])
                continue
            os.write(master_fd, key.encode('utf-8'))
            drain(key_delay)

    # Final drain
    drain(read_timeout)

    # Kill wpe
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
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


@pytest.fixture(scope='module')
def compile_dir():
    """Create a temporary directory with C source files for compile tests."""
    tmpdir = tempfile.mkdtemp(prefix='xwpe_test_')

    # Valid C program
    with open(os.path.join(tmpdir, 'hello.c'), 'w') as f:
        f.write('#include <stdio.h>\n')
        f.write('\n')
        f.write('int main() {\n')
        f.write('    printf("Hello, world!\\n");\n')
        f.write('    return 0;\n')
        f.write('}\n')

    # C program with exactly one error (for single-error navigation test)
    with open(os.path.join(tmpdir, 'one_error.c'), 'w') as f:
        f.write('#include <stdio.h>\n')
        f.write('\n')
        f.write('int main() {\n')
        f.write('    printf("only error")\n')   # missing semicolon (line 4)
        f.write('    return 0;\n')
        f.write('}\n')

    # C program with two errors
    with open(os.path.join(tmpdir, 'errors.c'), 'w') as f:
        f.write('#include <stdio.h>\n')
        f.write('\n')
        f.write('int main() {\n')
        f.write('    printf("line 4")\n')          # missing semicolon (line 4)
        f.write('    int x = 10;\n')
        f.write('    printf("line 6");\n')
        f.write('    int y = ;\n')                  # syntax error (line 7)
        f.write('    return 0;\n')
        f.write('}\n')

    # Create .xwpe dir so wpe doesn't complain
    os.makedirs(os.path.join(tmpdir, '.xwpe'), exist_ok=True)

    yield tmpdir

    # Cleanup
    shutil.rmtree(tmpdir, ignore_errors=True)


@pytest.fixture(scope='module')
def has_gcc():
    """Check if gcc is available."""
    return shutil.which('gcc') is not None


class TestF9CompileSuccess:
    """Test F9 compile cycle with a valid C program."""

    def test_compile_produces_output(self, compile_dir, has_gcc):
        """F9 on a valid program should compile without errors."""
        if not has_gcc:
            pytest.skip("gcc not installed")
        lines = run_wpe_in_dir(
            compile_dir, 'hello.c',
            cols=80, rows=30, wait=1.5,
            keys=[
                KEY_F9,             # Compile + Link
                ('wait', 3.0),      # Wait for compilation
                ' ',                # Dismiss "press any key" popup
                ('wait', 0.5),
            ]
        )
        content = '\n'.join(lines)
        # Should NOT show error messages like "error:" in the editor area
        # The compile should succeed silently or show "Compiling" briefly
        # After success, we should be back in the editor with hello.c content
        assert 'hello.c' in content or 'Hello' in content or 'printf' in content, \
            f"Editor content not visible after compile: {content[:300]}"

    def test_object_file_created(self, compile_dir, has_gcc):
        """F9 should create .o and executable files."""
        if not has_gcc:
            pytest.skip("gcc not installed")
        # Clean any previous build artifacts
        for ext in ['.o', '.e']:
            path = os.path.join(compile_dir, 'hello' + ext)
            if os.path.exists(path):
                os.unlink(path)

        lines = run_wpe_in_dir(
            compile_dir, 'hello.c',
            cols=80, rows=30, wait=1.5,
            keys=[
                KEY_F9,
                ('wait', 3.0),
                ' ',                # Dismiss "press any key" popup
                ('wait', 0.5),
            ]
        )
        # Check that object file was created
        obj = os.path.join(compile_dir, 'hello.o')
        exe = os.path.join(compile_dir, 'hello.e')
        assert os.path.exists(obj) or os.path.exists(exe), \
            f"Build artifacts not created in {compile_dir}: {os.listdir(compile_dir)}"


    def test_cursor_returns_to_source(self, compile_dir, has_gcc):
        """After successful F9, cursor should be in source file, not Messages."""
        if not has_gcc:
            pytest.skip("gcc not installed")
        lines = run_wpe_in_dir(
            compile_dir, 'hello.c',
            cols=80, rows=30, wait=1.5,
            keys=[
                KEY_F9,
                ('wait', 3.0),
                ' ',                # Dismiss popup
                ('wait', 0.5),
            ]
        )
        # The active window title should show "hello.c", not "Messages"
        # Title bar is typically on line 2 (row index 2) of the active window
        content = '\n'.join(lines)
        # The title bar of the active (topmost) window should contain hello.c
        # Messages window title would show "Messages"
        # Check that hello.c appears in the title area and source content is visible
        has_source = 'hello.c' in content and ('printf' in content or '#include' in content)
        # If Messages is active, its title would be prominent
        messages_active = False
        for line in lines[1:5]:
            if 'Messages' in line and 'hello.c' not in line:
                messages_active = True
        assert has_source and not messages_active, \
            f"Cursor stayed on Messages instead of returning to source: {content[:400]}"


    def test_messages_pane_has_no_readonly_padlock(self, compile_dir, has_gcc):
        """The Messages output pane is non-editable but is NOT a file on disk, so
        it must not wear the read-only padlock -- that glyph means 'locked file'
        and belongs only to real read-only files (0444 / library sources)."""
        if not has_gcc:
            pytest.skip("gcc not installed")
        LOCK = "\U0001f512"        # the read-only padlock glyph
        lines = run_wpe_in_dir(
            compile_dir, 'hello.c',
            cols=80, rows=30, wait=1.5,
            keys=[KEY_F9, ('wait', 3.0), ' ', ('wait', 0.5)],
        )
        msg_lines = [r for r in lines if 'Messages' in r]
        assert msg_lines, \
            "the Messages window never appeared after F9:\n%s" % '\n'.join(lines)
        assert not any(LOCK in r for r in msg_lines), \
            "the Messages pane wrongly shows the read-only padlock:\n%s" \
            % '\n'.join(msg_lines)


class TestF9CompileErrors:
    """Test F9 compile cycle with a program that has errors."""

    def test_errors_shown_in_messages(self, compile_dir, has_gcc):
        """F9 on a file with errors should show error messages."""
        if not has_gcc:
            pytest.skip("gcc not installed")
        lines = run_wpe_in_dir(
            compile_dir, 'errors.c',
            cols=80, rows=30, wait=1.5,
            keys=[
                KEY_F9,
                ('wait', 3.0),
            ]
        )
        content = '\n'.join(lines)
        # Should show "error" somewhere (in Messages window or status)
        assert 'error' in content.lower(), \
            f"No error message visible after compiling errors.c: {content[:400]}"

    def test_alt_t_jumps_to_error(self, compile_dir, has_gcc):
        """Alt-T after compile errors should navigate to an error line."""
        if not has_gcc:
            pytest.skip("gcc not installed")
        lines = run_wpe_in_dir(
            compile_dir, 'errors.c',
            cols=80, rows=30, wait=1.5,
            keys=[
                KEY_F9,
                ('wait', 3.0),
                KEY_ALT_T,           # Next Error
                ('wait', 0.5),
            ]
        )
        content = '\n'.join(lines)
        # After Alt-T, we should see the errors.c content (cursor jumped there)
        assert 'errors.c' in content or 'printf' in content or 'main' in content, \
            f"Did not navigate to error file: {content[:400]}"


class TestSingleErrorNavigation:
    """Test Alt-T (Next Error) with exactly one compile error."""

    def test_alt_t_shows_single_error(self, compile_dir, has_gcc):
        """Alt-T with one error should navigate to it, not say 'no more'."""
        if not has_gcc:
            pytest.skip("gcc not installed")
        lines = run_wpe_in_dir(
            compile_dir, 'one_error.c',
            cols=80, rows=30, wait=1.5,
            keys=[
                KEY_F9,
                ('wait', 3.0),
                KEY_ALT_T,           # Next Error -- should show the one error
                ('wait', 0.5),
            ]
        )
        content = '\n'.join(lines)
        assert 'one_error.c' in content or 'printf' in content or 'main' in content, \
            f"Alt-T did not navigate to single error: {content[:400]}"

    def test_alt_t_twice_still_shows_error(self, compile_dir, has_gcc):
        """Alt-T twice with one error: second press re-shows same error."""
        if not has_gcc:
            pytest.skip("gcc not installed")
        lines = run_wpe_in_dir(
            compile_dir, 'one_error.c',
            cols=80, rows=30, wait=1.5,
            keys=[
                KEY_F9,
                ('wait', 3.0),
                KEY_ALT_T,           # First: navigate to error
                ('wait', 0.5),
                KEY_ALT_T,           # Second: should re-show, not lose position
                ('wait', 0.5),
            ]
        )
        content = '\n'.join(lines)
        assert 'one_error.c' in content or 'printf' in content or 'main' in content, \
            f"Alt-T twice lost error position: {content[:400]}"


class TestPerlCompile:
    """Test F9 compile with Perl source files."""

    @pytest.fixture(autouse=True)
    def perl_files(self, compile_dir):
        with open(os.path.join(compile_dir, 'good.pl'), 'w') as f:
            f.write('#!/usr/bin/perl\n')
            f.write('use strict;\n')
            f.write('use warnings;\n')
            f.write('print "Hello\\n";\n')
        with open(os.path.join(compile_dir, 'bad.pl'), 'w') as f:
            f.write('#!/usr/bin/perl\n')
            f.write('use strict;\n')
            f.write('my $x = "missing"\n')  # missing semicolon
            f.write('print "Hello\\n";\n')

    def test_perl_syntax_ok(self, compile_dir):
        """F9 on valid Perl should show no errors."""
        if not shutil.which('perl'):
            pytest.skip("perl not installed")
        lines = run_wpe_in_dir(
            compile_dir, 'good.pl',
            cols=80, rows=30, wait=1.5,
            keys=[
                KEY_F9,
                ('wait', 3.0),
                ' ',
                ('wait', 0.5),
            ]
        )
        content = '\n'.join(lines)
        assert 'good.pl' in content or 'Hello' in content or 'print' in content, \
            f"Editor not visible after Perl compile: {content[:400]}"

    def test_perl_syntax_error(self, compile_dir):
        """F9 on Perl with syntax error should show error in Messages."""
        if not shutil.which('perl'):
            pytest.skip("perl not installed")
        lines = run_wpe_in_dir(
            compile_dir, 'bad.pl',
            cols=80, rows=30, wait=1.5,
            keys=[
                KEY_F9,
                ('wait', 3.0),
                ' ',                 # dismiss popup
                ('wait', 0.5),
            ]
        )
        content = '\n'.join(lines)
        assert 'error' in content.lower() or 'syntax' in content.lower() or \
               'bad.pl' in content, \
            f"No error shown for bad Perl: {content[:400]}"


class TestMenuClose:
    """Test that menus open and close without display artefacts."""

    def test_menu_open_close_no_artefacts(self, compile_dir):
        """Open menu with F10, close with ESC, no display corruption."""
        lines_before = run_wpe_in_dir(
            compile_dir, 'hello.c',
            cols=80, rows=30, wait=1.5,
            keys=[]
        )

        lines_after = run_wpe_in_dir(
            compile_dir, 'hello.c',
            cols=80, rows=30, wait=1.5,
            keys=[
                KEY_F10,             # Open menu
                ('wait', 0.5),
                KEY_ESC_RAW,         # Close menu
                KEY_ESC_RAW,
                ('wait', 0.5),
            ]
        )
        # The editor content should be visible and intact after menu close
        content_after = '\n'.join(lines_after)
        assert 'hello.c' in content_after or 'printf' in content_after, \
            f"Editor content not restored after menu close: {content_after[:300]}"

    def test_run_menu_open_close(self, compile_dir):
        """Open Run menu (Alt-R), close with ESC, no artefacts."""
        lines = run_wpe_in_dir(
            compile_dir, 'hello.c',
            cols=80, rows=30, wait=1.5,
            keys=[
                '\033r',             # Alt-R (Run menu)
                ('wait', 0.5),
                KEY_ESC_RAW,         # Close
                KEY_ESC_RAW,
                ('wait', 0.5),
            ]
        )
        content = '\n'.join(lines)
        # Editor should be intact
        assert 'printf' in content or 'Hello' in content or '#include' in content, \
            f"Editor content corrupted after Run menu: {content[:300]}"

    def test_debug_menu_open_close(self, compile_dir):
        """Open Debug menu (Alt-D), close with ESC, no artefacts."""
        lines = run_wpe_in_dir(
            compile_dir, 'hello.c',
            cols=80, rows=30, wait=1.5,
            keys=[
                '\033d',             # Alt-D (Debug menu)
                ('wait', 0.5),
                KEY_ESC_RAW,         # Close
                KEY_ESC_RAW,
                ('wait', 0.5),
            ]
        )
        content = '\n'.join(lines)
        assert 'printf' in content or 'Hello' in content or '#include' in content, \
            f"Editor content corrupted after Debug menu: {content[:300]}"

    def test_options_menu_open_close(self, compile_dir):
        """Open Options menu (Alt-O), close with ESC, no artefacts."""
        lines = run_wpe_in_dir(
            compile_dir, 'hello.c',
            cols=80, rows=30, wait=1.5,
            keys=[
                '\033o',             # Alt-O (Options menu)
                ('wait', 0.5),
                KEY_ESC_RAW,         # Close
                KEY_ESC_RAW,
                ('wait', 0.5),
            ]
        )
        content = '\n'.join(lines)
        assert 'printf' in content or 'Hello' in content or '#include' in content, \
            f"Editor content corrupted after Options menu: {content[:300]}"


class TestCompilePopup:
    """Test that the compile progress popup closes cleanly."""

    def test_compile_popup_dismissed_cleanly(self, compile_dir, has_gcc):
        """After F9 + keypress to dismiss, popup is gone and editor restored."""
        if not has_gcc:
            pytest.skip("gcc not installed")
        lines = run_wpe_in_dir(
            compile_dir, 'hello.c',
            cols=80, rows=30, wait=1.5,
            keys=[
                KEY_F9,
                ('wait', 3.0),
                ' ',                # Dismiss "press any key" popup
                ('wait', 0.5),
            ]
        )
        content = '\n'.join(lines)
        # After dismissing the popup, "Compiling" should NOT be on screen
        center_lines = lines[10:20]
        center_content = '\n'.join(center_lines)
        assert 'Compiling' not in center_content, \
            f"Compile popup not dismissed: {center_content}"
        # Editor content should be visible
        assert 'printf' in content or '#include' in content or 'hello' in content.lower(), \
            f"Editor not restored after compile popup: {content[:300]}"

    def test_no_null_bytes_after_compile(self, compile_dir, has_gcc):
        """No null bytes should appear after compilation."""
        if not has_gcc:
            pytest.skip("gcc not installed")
        lines = run_wpe_in_dir(
            compile_dir, 'hello.c',
            cols=80, rows=30, wait=1.5,
            keys=[
                KEY_F9,
                ('wait', 3.0),
                ' ',                # Dismiss popup
                ('wait', 0.5),
            ]
        )
        for i, line in enumerate(lines):
            if line:
                assert '\x00' not in line, \
                    f"Null byte at line {i} after compile: {repr(line)}"


class TestMultipleMenuCycles:
    """Test opening and closing menus multiple times."""

    def test_three_menu_cycles(self, compile_dir):
        """Open/close menu 3 times, no progressive degradation."""
        keys = []
        for _ in range(3):
            keys.extend([
                '\033r',             # Alt-R
                ('wait', 0.3),
                KEY_ESC_RAW,
                KEY_ESC_RAW,
                ('wait', 0.3),
            ])

        lines = run_wpe_in_dir(
            compile_dir, 'hello.c',
            cols=80, rows=30, wait=1.5,
            keys=keys
        )
        content = '\n'.join(lines)
        assert 'printf' in content or '#include' in content, \
            f"Editor degraded after 3 menu cycles: {content[:300]}"

        # No null bytes
        for i, line in enumerate(lines):
            if line:
                assert '\x00' not in line, \
                    f"Null byte at line {i} after menu cycles"

    def test_compile_then_menu_no_artefacts(self, compile_dir, has_gcc):
        """Compile, then open/close menu, no artefacts."""
        if not has_gcc:
            pytest.skip("gcc not installed")
        lines = run_wpe_in_dir(
            compile_dir, 'hello.c',
            cols=80, rows=30, wait=1.5,
            keys=[
                KEY_F9,
                ('wait', 3.0),
                ' ',                 # Dismiss compile popup
                ('wait', 0.5),
                '\033r',             # Alt-R
                ('wait', 0.5),
                KEY_ESC_RAW,
                KEY_ESC_RAW,
                ('wait', 0.5),
            ]
        )
        content = '\n'.join(lines)
        # Should see editor content, not garbage
        has_content = ('printf' in content or '#include' in content or
                       'hello' in content.lower())
        assert has_content, \
            f"Display corrupted after compile+menu: {content[:300]}"


def test_output_pane_marked_tool_not_padlock(compile_dir):
    """Compiling spawns the Messages output pane (ins==8, backing no file).  It
    is a tool pane, so it wears the tool glyph (U+2699 gear, 'this is a tool
    pane') and NOT the padlock (U+1F512, 'this is a locked file' -- reserved for
    real files on disk, see test_readonly_marker.py).  The source compiled here
    is writable, so no padlock must appear anywhere on screen.  The tool glyph is
    a BMP width-1 symbol so it renders consistently (the wrench emoji blanked on
    some terminals)."""
    lines = run_wpe_in_dir(
        compile_dir, 'one_error.c',
        cols=80, rows=30, wait=1.5,
        keys=[KEY_F9, ('wait', 3.0)]
    )
    content = '\n'.join(lines)
    assert 'Messages' in content, \
        f"compile did not open the Messages output pane:\n{content[:400]}"
    assert '\U0001F512' not in content, \
        f"read-only padlock leaked onto a tool/output pane:\n{content[:400]}"
    assert '⚙' in content, \
        f"tool/output pane is missing the tool marker:\n{content[:400]}"
