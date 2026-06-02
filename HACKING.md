# xwpe internals

Notes for contributors and porters. For user documentation see
`README.md` and `info xwpe`.

## SCREENCELL buffer model (1.6.0)

Version 1.6.0 replaced the 1993 byte-based screen buffer with
`SCREENCELL` (int ch + int attr per cell). UTF-8 is decoded via
`mbrtowc()` and stored as one value per visible column.

| Component | Before (1.5.x) | After (1.6.0) | Why |
|-----------|----------------|---------------|-----|
| Screen buffer | `char *schirm` (2 bytes/cell: char + attr) | `SCREENCELL *schirm` (int ch + int attr) | Holds decoded `wchar_t`; 1 cell = 1 visual column |
| Buffer macros | `*(schirm + 2*MAXSCOL*y + 2*x)` | `schirm[y*MAXSCOL+x].ch` | No byte offset math, supports values > 255 |
| Display loop | 1 byte per iteration | `mbrtowc()` decodes multi-byte, `i += nb` (bytes), `j++` (column) | One character per column regardless of byte count |
| Wide chars | Not handled | `wcwidth()` after decode; width=2 fills second cell | Emoji and CJK take 2 columns |
| Terminal refresh | `addch(c)` | `add_wch(&cc)` for wide, `addch(sp_chr[c])` for ACS | ncursesw native wide character output |
| Change detection | byte compare | struct field compare | Same logic, different type |
| Buffer alloc | `MALLOC(2 * MAXSCOL * MAXSLNS)` | `MALLOC(sizeof(SCREENCELL) * MAXSCOL * MAXSLNS)` | 8 bytes/cell instead of 2 |
| Save/restore | `e_gt_byte`/`e_pt_byte` | `memcpy` of SCREENCELL | Full cell preserved |
| ncurses | `-lncurses` | `-lncursesw` | Wide character API only in ncursesw |
| Resize | Not handled | `KEY_RESIZE` -> realloc + repaint | Modern terminals resize dynamically |
| Keyboard | VT100/Sun mapping | PC keyboard (PageUp, Home, End) | Original was for 1990s hardware terminals |

## Double-buffer rendering

xwpe maintains `schirm` (desired state) and `altschirm` (on-screen
state). `e_t_refresh()` (terminal) and `e_x_refresh()` (X11) compare
cell by cell and only repaint differences -- the same approach as
virtual DOM diffing in modern UI frameworks.

## Popup save/restore (PIC system)

`e_open_view()` saves the schirm region under a popup into a `PIC`
struct. `e_close_view()` restores it. The PIC does NOT save X11
border flags (`extbyte`). To compensate, `e_x_refresh()` clears
extbyte for the interior of all windows before rendering, preventing
hidden windows' borders from bleeding through.

## X11 border system

Since v1.6.3 with Xft, window borders are rendered as Unicode
box-drawing characters (U+2500 ─, U+2502 │, U+250C ┌, etc.) via
`XftDrawStringUtf8`, matching the terminal mode's ncurses ACS
rendering. The border characters are stored in SCREENCELL as ACS
indices 1-6 (set via RE1-RE6/RD1-RD6 in `we_xterm.c`).

The old NEWSTYLE `XDrawLine` system (`extbyte` array with per-cell
border flags) is disabled when Xft is active. Without Xft, the
legacy XDrawLine path is still used as fallback.

### X11 window state indicators (F R A S)

In NEWSTYLE X11 mode, four single-letter indicators appear on the
left border of each editor window (`we_wind.c:414-422`):

- **F** (y+2): File marker
- **R** (y+4): Read marker
- **A** (y+6): (historical, meaning undocumented)
- **S** (y+8): Save state (only shown when `f->ins != 8`)

These are clickable in X11 mode. They are an original Kruse/Payne
design element, not present in terminal mode (which has no room
for them in a single-column border).

### extbyte (legacy, non-Xft)

The `extbyte` flat array stores per-cell border flags (top, bottom,
left, right line segments). Known issue: no awareness of window
stacking -- hidden windows' border flags persist and must be cleared
explicitly.

## Source file overview

| File | Purpose |
|------|---------|
| `edit.h` | Main header, all type definitions |
| `we_term.c` | Terminal rendering (ncurses), mouse, debug output |
| `we_xterm.c` | X11 rendering, event loop, border segments |
| `WeXterm.c` | X11 initialisation, font, color, GC setup |
| `we_wind.c` | Window management, popup save/restore, frames |
| `we_edit.c` | Editor core, undo/redo, cursor movement |
| `we_debug.c` | Debugger backends (gdb, jdb, pdb), pipe management |
| `we_prog.c` | Compiler integration, F9, Run, error parsing |
| `we_fl_unix.c` | File Manager |
| `we_mouse.c` | Mouse event handling |
| `we_menue.c` | Menu system |
| `we_opt.c` | Options dialogs |
| `WeSyntax.c` | Syntax highlighting engine |

## Debugger backend architecture

Debugger type is `e_deb_type`: 0=gdb, 1=sdb, 2=dbx, 3=xdb, 4=jdb,
5=pdb. Each backend uses the same pipe infrastructure but differs in
prompt detection, command syntax, and output parsing. In X11 mode,
the debugger runs in an xterm via named pipes (`npipe[0-4]`). In
terminal mode, it uses regular pipes (`rfildes/wfildes/efildes`).

Program output is captured via pty (terminal mode) or named pipes
(X11 mode) and appended to the Messages buffer by `e_t_deb_out()`.
