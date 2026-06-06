# xwpe Test Suite

Automated terminal UI tests using [pyte](https://github.com/selectel/pyte)
(VT100 terminal emulator) and [pytest](https://pytest.org/).

## Running everything (recommended)

`tests/run-tests.sh` is the single entry point -- the local equivalent of
the Debian autopkgtest (the autopkgtest calls this same script):

```bash
./configure && make            # build xwpe first
tests/run-tests.sh             # EVERYTHING: C unit + pyte (./wpe) + X11 (./xwpe)
tests/run-tests.sh --asan      # build we-asan and run the pyte suite under it
tests/run-tests.sh --x11       # ONLY the headless X11 GUI suite (./xwpe)
tests/run-tests.sh --help
```

The default run executes all three layers.  Its X11 step self-skips when the
headless X stack (see below) or Pillow is missing, so the default stays green
on a text-only buildd; `--x11` runs just that layer.

It uses a system pyte/pytest if installed (e.g. `python3-pyte`,
`python3-pytest`), otherwise it bootstraps a venv under `tests/.venv`.

## Dependencies (Debian/Ubuntu package names)

Building xwpe and the C unit tests (`make check`) needs the normal build
toolchain -- see the top-level README and `debian/control` Build-Depends.
The two functional layers add the following; a test for a tool that is
absent **skips** rather than fails, so you only need what you want to cover.

**pyte layer** (`./wpe`, ncurses):
- `python3-pyte`, `python3-pytest` -- else `run-tests.sh` bootstraps a venv
- compilers/debuggers the F9 / Ctrl-F10 tests drive: `gcc`, `g++`,
  `gfortran`, `fp-compiler` (Pascal), `gdb`

**X11 GUI layer** (`./xwpe`, Xft -- see the table below):
- `xvfb`, `matchbox-window-manager`, `xdotool`, `x11-utils`, `imagemagick`,
  `python3-pil`

Install every test extra in one go:

```bash
sudo apt-get install -y \
  python3-pyte python3-pytest gcc g++ gfortran fp-compiler gdb \
  xvfb matchbox-window-manager xdotool x11-utils imagemagick python3-pil
```

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
| `xdotool` | `xdotool` | synthetic key/mouse (function keys by keycode -- see below) |
| `xwd` | `x11-utils` | capture the root window |
| `convert` | `imagemagick` | xwd -> PNG |
| Pillow | `python3-pil` | load PNG, assert on pixels |

A window manager is **not optional**: without one, `xwpe`'s X11 size handling
oscillates into an unrecoverable resize feedback loop under bare Xvfb (`xwpe`
never calls `XResizeWindow` itself -- a WM owns the geometry).  `matchbox`
maximises the single window and the loop never starts.

matchbox is started with an **empty `-kbdconfig`** (`tests/x11/matchbox-kbdconfig`):
its default config grabs `<Alt>n` (next), `<Alt>p` (prev), `<Alt>c` (close),
`<Alt>d`, `<Alt>Tab` ... at the X root.  Those Alt-`<letter>` combos are
exactly the dialog/menu hotkeys `xwpe` uses, so the default config makes the WM
swallow them before they reach `xwpe` -- which looks like an "Alt-hotkey
broken" bug but is not.  The empty config binds nothing.

#### Function keys are injected by keycode (xdotool bug #491)

`xdotool key F3` does **not** reliably send a bare F3.  xdotool resolves a
keysym to a keycode *plus* the modifiers needed to reach the level that keysym
sits on, and on `xkeyboard-config` >= 2.46 that resolver is wrong for the
function-key rows (it picks a modifier-bearing mapping): the event arrives as
**Alt+F3** instead of F3.  See xdotool issue
[#491](https://github.com/jordansissel/xdotool/issues/491) and fix
[#493](https://github.com/jordansissel/xdotool/pull/493).

This bit xwpe specifically: `we_xterm.c` decodes Alt+F3 as `AF3` -- a *different*
key from `F3` -- so every bare/Ctrl function-key accelerator (Save F2, Open F3,
Find F4, Make F9, Run ^F9 ...) silently did nothing in the harness while
working fine for a human at a real keyboard.  Letter and Alt/Ctrl-`<letter>`
combos are unaffected (their level-1 mapping is explicit).

The fix lives in `conftest.py`: `session.key()` routes every token through
`_to_keycodes()`, which rewrites any `F<n>` token (bare or inside a combo like
`ctrl+F9`) to its raw X keycode -- read live from `xmodmap -pke` -- so xdotool
skips the broken resolver and the true function key reaches xwpe.  Tests just
write `xwpe.key("F2")` or `xwpe.key("ctrl+F9")` as usual.

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

Coverage is being organised **one module per menu section** (`test_menu_<x>.py`
for wpe, `tests/x11/test_menu_<x>.py` where X11 rendering/input differs), built
on the shared `tests/wpe_driver.py` (a pyte `WpeSession` that seeds a file,
drives menus, saves, and reads back the file as ground truth).

**Incoherence flagging.**  Each test encodes the behaviour we believe *should*
hold.  When coverage turns up a behaviour that is wrong but not yet fixed, the
test is marked `@incoherence("...")` (from `wpe_driver`), which records it as an
xfail.  `pytest -rxX` then prints every `INCOHERENCE:` as a manual-review queue;
once the behaviour is fixed the test flips to XPASS and the marker is removed.

**Shortcut double-coverage (#159).**  Each menu item is exercised BOTH through
the menu and through its advertised keyboard accelerator -- a separate decode
path (`e_prog_switch` / `e_tst_dfkt` for the terminal, `we_xterm.c` for X11)
where mapping bugs hide (an item can work from the menu while its accelerator is
dead).  `test_*_via_<key>` / `test_chord_*` functions cover Save F2, Open F3, Find F4,
Make F9, Run ^F9/Alt-U, Help F1, Cut/Copy/Paste/Undo/Redo, Show Buffer Alt-Y,
Go to Line Alt-G, Toggle Breakpoint ^G B, Zoom/Next/List/Close, and the WordStar
Block chords ^K X/^K Y/^K I/^K U.  Function-key accelerators work in the X11
suite only because of the keycode injection above (xdotool #491).

The System (#), Options and Project menus carry NO advertised accelerators in
`we_menue.c` (every item is reachable by the menu only -- there is no key hint
in any label and no global key binding for their handlers), so there is no
shortcut path to double-cover there; they are exercised through the menu alone.

Menu sections (`test_menu_<x>.py`, Alt-`<x>`) -- one per environment:
- `test_menu_edit.py` (wpe) + `x11/test_menu_edit.py` (xwpe) -- Edit (Alt-E):
  Cut, Copy/Paste, Cut+Paste round trip, Undo, Redo -- asserted on the saved
  file.  Run in BOTH front-ends because the X11 key/menu path decodes input
  separately, so behaviour can diverge (as the keyboard ^K B bug showed)
- `test_menu_search.py` (wpe) + `x11/test_menu_search.py` (xwpe) -- Search
  (Alt-S): Go to Line, Find (cursor move verified by typing a marker), and
  Replace + Change All (Alt-N / Alt-P / Alt-A choreography).  Both front-ends
  pass identically.  (While writing this the X11 run first looked like an
  Alt-hotkey divergence; it was matchbox grabbing `<Alt>n/p` -- see the
  matchbox-kbdconfig note below.  xwpe was correct.)
- `test_menu_file.py` (wpe) + `x11/test_menu_file.py` (xwpe) -- File (Alt-F):
  Save and Save aLl persist an edit; Save As drives the File-Manager dialog
  (type a Name, Alt-S) and writes a new file; New opens a fresh buffer
  (verified by typing into it and Saving As).  Both front-ends pass
- `test_menu_system.py` (wpe) + `x11/test_menu_system.py` (xwpe) -- System
  (Alt-#): About WE shows the program name + authors, System Info reports the
  current file, Clear Desktop closes the window.  wpe checks the rendered
  screen; xwpe checks a screenshot diff (a dialog appeared).  The '#' menu has
  no Alt-letter, so the X11 twin opens it with xwpe's ESC meta-prefix
  (`esc_menu`) -- the X11 equivalent of the terminal's Alt encoding
- `test_menu_help.py` (wpe) + `x11/test_menu_help.py` (xwpe) -- Help (Alt-H):
  Editor opens the manual viewer, Topic Search opens a prompt

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
- `test_window_menu.py` (wpe) + `x11/test_menu_window.py` (xwpe) -- Zoom
  maximises the active window, Next cycles the active window, Close closes the
  active window, List All opens the "Windows" chooser; the X11 twin also covers
  Tile and Cascade via screenshot diffs (a second window is opened first)

Block menu (Alt-B) operations:
- `test_block.py` -- Mark Whole + Delete empties the buffer, Move to
  Right/Left indents/unindents the block, indent+unindent round-trips.
  Asserts on the saved file (ground truth), not the live screen.  Each is
  driven BOTH through the Alt-B menu and through the WordStar `^K` chord
  (`^K X` Mark Whole, `^K Y` Delete, `^K I`/`^K U` indent/unindent) -- the #159
  double-coverage, since the chord is a separate decode from the menu
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
  both modes behaved identically.  Also the #159 chord twins (`^K X` + `^K Y`
  Delete, `^K X` + `^K I` indent) asserted on the saved file in the X11 path

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
