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

Rendering / input:
- `test_utf8_border.py` -- UTF-8 display, border alignment, emoji/wide chars
  (also provides the shared SafeScreen helper)
- `test_scrollbar.py` -- scrollbar during scroll, end-of-file, PageDown/PageUp
- `test_dialog_resize.py` -- dialog redraw across terminal resize
- `test_compile.py` -- F9 compile cycle, error navigation, menu close, popup cleanup
- `test_esc_key.py` -- a lone Esc registers on the first press; Alt-<key> still
  opens menus (#141)
- `test_mouse_tracking.py` -- xterm motion tracking re-armed (in order) after an
  F9 compile, so window drag/resize keeps working
- `test_maxcol_paste.py` -- small Max Columns + cut/paste/typing does not crash
  (Payne-era #87, fixed by the SCREENCELL migration)

Borland-style project management:
- `test_project_windows.py` -- picker/project window titles, white-on-white
  guard (fg!=bg), compact status bar, "No project open" guard, New vs Open
  Project, Delete of the last member, long-LIBNAME overflow guard
- `test_project_persist.py` -- a file added to a project is written to the .prj
  immediately (not only on window close)

Window backing-store (PIC view) double-free guards:
- `test_window_views.py` -- the three bulk view-release paths
  (open project + F9, Window > Tile, Window > Cascade, and them repeated)
  stay alive; a double-free aborts with SIGABRT.  Guards the e_free_view()
  helper introduced with the 1.6.3 leak fix

Window menu (Alt-W) operations:
- `test_window_menu.py` -- Zoom maximises the active window, Next cycles
  the active window, Close closes the active window, List All opens the
  "Windows" chooser

Block menu (Alt-B) operations:
- `test_block.py` -- Mark Whole + Delete empties the buffer, Move to
  Right/Left indents/unindents the block, indent+unindent round-trips.
  Asserts on the saved file (ground truth), not the live screen

C-source tests built by `make check`:
- `test_checkheader.c`, `test_syntax_def.c`

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
