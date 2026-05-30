# xwpe Test Suite

Automated terminal UI tests using [pyte](https://github.com/selectel/pyte)
(VT100 terminal emulator) and [pytest](https://pytest.org/).

## Setup

```bash
python3 -m venv tests/.venv
tests/.venv/bin/pip install pyte==0.8.1 pytest
```

## Running

```bash
# Build xwpe first
./configure && make

# Create wpe symlink (tests use terminal mode)
ln -sf we wpe

# Run all tests
tests/.venv/bin/python -m pytest -v tests/
```

## Test files

- `test_utf8_border.py` -- UTF-8 display, border alignment, emoji/wide chars
- `test_scrollbar.py` -- scrollbar during scroll, end-of-file, PageDown/PageUp
- `test_compile.py` -- F9 compile cycle, error navigation, menu close, popup cleanup

## What the tests verify

- Right border aligned on all lines regardless of UTF-8 content
- Accented characters (Latin, Cyrillic) display correctly
- No @C@3 escaping or M-C garbling
- No null byte gaps in visible area
- Emoji and CJK wide characters don't break borders
- Borders present after PageDown, PageUp, and at end of file
- No stale content after scrolling
- Terminal resize produces visible content
- File opens and content is displayed
- F9 compiles a valid C program (creates .o and .e files)
- F9 on invalid code shows errors in Messages window
- Alt-T navigates to the error location
- Compile popup dismissed cleanly (no artefacts)
- Menus (Run, Debug, Options) open and close without display corruption
- Multiple menu open/close cycles cause no progressive degradation
- Compile then menu open/close leaves editor intact
