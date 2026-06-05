# xwpe Test Suite

Automated terminal UI tests using [pyte](https://github.com/selectel/pyte)
(VT100 terminal emulator) and [pytest](https://pytest.org/).

## Running everything (recommended)

`tests/run-tests.sh` is the single entry point -- the local equivalent of
the Debian autopkgtest (the autopkgtest calls this same script):

```bash
./configure && make            # build xwpe first
tests/run-tests.sh             # C unit tests (make check) + pyte suite (./wpe)
tests/run-tests.sh --asan      # build we-asan and run the pyte suite under it
tests/run-tests.sh --x11       # headless X11 GUI suite (./xwpe under Xvfb)
tests/run-tests.sh --help
```

It uses a system pyte/pytest if installed (e.g. `python3-pyte`,
`python3-pytest`), otherwise it bootstraps a venv under `tests/.venv`.

## X11 GUI tests (xwpe)

`pyte` emulates a VT100 text terminal, so it covers `wpe` (ncurses) but
**cannot** exercise `xwpe`'s real Xft rendering or its X11 keyboard/mouse
path.  The `tests/x11/` suite fills that gap by driving a real `xwpe` under a
headless X server and asserting on screenshots.

```bash
tests/run-tests.sh --x11                 # via the single entry point
# or directly:
XWPE_BIN=$PWD/xwpe tests/.venv/bin/python -m pytest -q tests/x11/
```

Requirements (Debian package names) -- the suite **skips cleanly** if any are
absent, so the default `run-tests.sh` is unaffected on a headless buildd:

| Tool | Debian package | Why |
|------|----------------|-----|
| `Xvfb` | `xvfb` | headless X server |
| `matchbox-window-manager` | `matchbox-window-manager` | **required** WM -- see below |
| `xdotool` | `xdotool` | synthetic key/mouse via XTEST |
| `xwd` | `x11-utils` | capture the root window |
| `convert` | `imagemagick` | xwd -> PNG |
| Pillow | `python3-pil` | load PNG, assert on pixels |

A window manager is **not optional**: without one, `xwpe`'s X11 size handling
oscillates into an unrecoverable resize feedback loop under bare Xvfb (`xwpe`
never calls `XResizeWindow` itself -- a WM owns the geometry).  `matchbox`
maximises the single window and the loop never starts.

The X11 tests honour **`$XWPE_BIN`** (default `../xwpe`), mirroring `$WPE_BIN`.

### Running pytest directly

```bash
python3 -m venv tests/.venv
tests/.venv/bin/pip install pyte==0.8.1 pytest
tests/.venv/bin/python -m pytest -v tests/
```

The tests run the binary named by **`$WPE_BIN`** (default `../wpe`).  Point
it elsewhere to run the same scenarios against another build -- the
AddressSanitizer build, a valgrind wrapper, or an installed `/usr/bin/wpe`:

```bash
WPE_BIN=$PWD/we-asan tests/.venv/bin/python -m pytest -q tests/
```

### Memory-safety runs

A heap-buffer-overflow or use-after-free is silent on a normal build (the
process survives with corrupted memory), so the functional pyte assertions
cannot see it.  Run the suite under a sanitizer to catch that class -- this
is how the X11-repaint use-after-free and the Ctrl-K V block-move overflow
were found.  `tests/run-tests.sh --asan` does it for you; see HACKING.md
("Debugging crashes and memory bugs") for valgrind and the `we-asan` recipe.

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
- `test_block_asan.py` -- block Copy/Move under AddressSanitizer: asserts
  the `we-asan` build produces no ASan report (a heap-buffer-overflow is
  silent on the normal build, so it needs a sanitizer).  Skips if `we-asan`
  is not built.  Guards the Ctrl-K V line-buffer overflow fix

X11 GUI tests (xwpe under Xvfb, `tests/x11/`):
- `conftest.py` -- Xvfb + matchbox + xwpe session fixtures, xdotool input
  helpers, and a Pillow screenshot helper
- `test_editor_options.py` -- the Options > Editor dialog opens, and toggling
  a checkbox (Alt-K) renders a visible `[X]` mark that clears on a second
  toggle.  Guards the X11-only regression where NEWSTYLE drew check/radio
  marks as a near-invisible 3D bevel, so Space/mouse/Alt-K appeared to do
  nothing in xwpe (the value flipped but nothing was drawn)
- `test_block_marking.py` -- Begin Mark (^K B) honours the "WordStar block"
  mode: modern (default) clears the previous block on re-mark, WordStar keeps
  the end marker so a block stays highlighted.  Guards the regression where
  the keyboard ^K B bypassed e_blck_begin, so the mode flag had no effect and
  both modes behaved identically

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
