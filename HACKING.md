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

## Cairo+Pango rendering backend (1.6.3)

Version 1.6.3 adds a Cairo+Pango rendering path alongside Xft.
The architecture follows Kruse's function pointer pattern: the
`WpeRenderBackend` struct (`we_render.h`) holds function pointers
for draw_rect, draw_text, draw_acs, flush, resize.  The X11 init
sets them to Cairo implementations; terminal mode ignores them.

### Text rendering: dual engine

ASCII (32-127) is rendered via **cairo_ft** -- FreeType directly
through Cairo's `cairo_show_glyphs()`.  One glyph struct on the
stack per call, no allocation, no heap pressure.

Non-ASCII (UTF-8 multibyte, emoji) falls back to **Pango** via
`pango_cairo_show_layout()`.  This is rare during normal editing
and scrolling.

Pango's `PangoLayout` corrupts the heap under heavy reuse (thousands
of `set_text` + `show_layout` per second causes `pango_layout_line_unref`
refcount underflow).  The cairo_ft path avoids this entirely for ASCII.

### Font fitting

Both Pango and cairo_ft need their font size adjusted to match
Xft's cell grid exactly.  Xft gives cells of `font_width x font_height`
pixels (typically 8x17).  Pango and FreeType at the same point size
produce different pixel dimensions.  The solution: iterative reduction
from `font_height` downward until the rendered glyph "M" fits within
the cell.

    Pango: pango_font_description_set_absolute_size(font, px * PANGO_SCALE)
    cairo_ft: cairo_matrix_init_scale(&mat, sz, sz)

### Scrollbar drag and the schirm model

The scrollbar drag loop (`e_scroll_drag_v` in `we_mouse.c`) uses
`XGrabPointer` + `MotionNotify` events with coalescing.  Each step:

1. `e_scroll_invalidate_content(f)` -- memset only the editor
   content cells in altschirm (not menu, borders, status bar)
2. `e_scroll_render_lines(f)` -- calls `e_pr_line` for visible
   lines (not `e_pr_c_line` which needs breakpoint array init)
3. `e_refresh()` -- diff renders only changed cells + chrome

Critical: `f->s->c.y` must be synced to `f->b->b.y` before calling
`e_pr_line`.  The macro `NUM_LINES_OFF_SCREEN_TOP` expands to
`f->s->c.y`, and `e_pr_line` uses it to calculate screen positions.
Without this sync, line addresses go out of range and crash.

Do NOT call `e_abs_refr()` during drag -- it memsets ALL 2400+
altschirm cells, forcing a full-screen redraw that is too slow for
60fps drag.  Do NOT call `e_pr_c_line` -- it accesses `s->brp[]`
(breakpoint array) which needs initialization from `e_abs_refr`.

### Resize flicker prevention

Four techniques combined:

- `XSetWindowBackgroundPixmap(display, window, None)` -- X server
  does not paint exposed areas
- `StaticGravity` -- X server does not move old pixels on resize
- `XCheckTypedWindowEvent` loop to compress ConfigureNotify events
- `_NET_WM_SYNC_REQUEST` protocol -- WM waits for app to finish
  painting before showing the resized window

### Window move: compositor-style recomposition

Kruse's window manager is a software compositor.  The correct screen
is the result of layering: desktop -> windows bottom-to-top -> borders
-> content.  The original `e_schirm` rebuilds this entire stack via
`e_abs_refr` (memset altschirm) each frame -- correct but slow.

The original `e_ed_kst` -> `e_change_pic` -> `e_close_view` cycle
caused TWO flushes per drag step: one showing the desktop with the
window removed, one showing the window at the new position.  The first
flush was the visible black flash.

`e_move_window_recompose` (we_wind.c) replaces this with a single-pass
compositor:

1. `e_restore_pic_to_schirm` -- restore saved background to schirm
2. `e_refresh_area` -- invalidate old rectangle in altschirm
3. `e_open_view` -- save new background from schirm
4. `e_ed_rahmen` -- draw borders at new position
5. `e_render_window_content` -- type-dispatched content (editor, file
   manager, data dialog, file dropdown)
6. `e_refresh_area` -- invalidate new rectangle
7. `e_cursor_pos_only` -- position cursor
8. `e_refresh` -- **single flush** (content + chrome + cursor)

No intermediate refreshes.  The Pixmap is fully composed before one
XCopyArea copies it to the window.  Same pattern for Wayland:
`wl_surface.commit` replaces XCopyArea.

### Single-flush architecture

`fk_show_cursor` renders cursor cells to the Pixmap but does NOT flush
them individually.  The single `WpeRender.flush_all()` in `e_x_refresh`
after `fk_show_cursor` copies the complete frame.

`WpeRender.blit` (cr_blit in we_render_cairo.c) provides Pixmap-to-Pixmap
copy within the backbuffer via `XCopyArea(pixmap, pixmap)`.  Not yet
used for window move but available as an optimization for large
resolutions (4K, 12000+ cells).

### Chrome post-pass

After the cell loop, `wpe_render_chrome()` draws modern scrollbar
chrome (thin track, proportional thumb, triangular arrows) over the
scrollbar column.  Only applies to text editor windows (DTMD_ISTEXT).
Named functions: `cr_chrome_arrow_up`, `cr_chrome_arrow_down`,
`cr_chrome_track`, `cr_chrome_thumb`, `cr_chrome_vscrollbar`.

### New files

| File | Purpose |
|------|---------|
| `we_render.h` | WpeRenderBackend struct, function pointer API |
| `we_render.c` | Global WpeRender instance |
| `we_render_cairo.c` | Cairo+Pango+FreeType backend, chrome post-pass |

### Build dependencies

Cairo requires: `libcairo2-dev`, `libpango1.0-dev` (pangocairo).
Detection via `PKG_CHECK_MODULES` in `configure.ac`.  When Cairo
is not available, the Xft path is used as fallback.  When X11 is
not available, ncurses terminal mode is used (unchanged).

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

## CHECKHEADER: automatic header dependency tracking

Enabled by `#define CHECKHEADER` in `model.h`.  When the user presses
F9, `e_check_header()` (we_prog.c) recursively follows `#include "..."`
directives and checks if any header's mtime is newer than the object
file.  If so, recompilation is triggered.

The parser handles:
- `/* ... */` block comments (multi-line)
- `//` line comments
- `#if 0` ... `#endif` blocks (skipped, including nested conditionals)
- `#ifdef`/`#ifndef` blocks (followed conservatively -- no symbol table)

Named functions:
- `e_chk_skip_whitespace_and_comments` -- skip whitespace + both comment styles
- `e_chk_track_conditional` -- #if/#ifdef/#ifndef/#else/#endif depth tracking
- `e_chk_extract_include` -- extract path from `#include "..."`
- `e_chk_next_directive` -- advance to next preprocessor directive
- `e_chk_save_if_open` -- auto-save if the header is open in the editor

Only `#include "..."` (quoted) includes are followed.  System includes
(`#include <...>`) are ignored -- they are system headers whose
timestamps are irrelevant for project recompilation.

Unit test: `tests/test_checkheader.c` (10 cases), runs via `make check`.

## Debugger backend architecture

Debugger type is `e_deb_type`: 0=gdb, 1=sdb, 2=dbx, 3=xdb, 4=jdb,
5=pdb. Each backend uses the same pipe infrastructure but differs in
prompt detection, command syntax, and output parsing. In X11 mode,
the debugger runs via named pipes (`npipe[0-4]`). In terminal mode,
it uses regular pipes (`rfildes/wfildes/efildes`).

### Program output via pty (all modes)

`openpty()` creates a master/slave pair. gdb redirects the inferior's
stdout to the slave via `r > /dev/pts/N`. The parent reads from the
master fd.  After each debug step, `e_d_pty_flush_to_messages()`
drains the pty and writes new output to the Messages buffer.  The
`call (void)fflush(0)` gdb command forces the inferior to flush its
stdio buffers before draining.

This replaces the old xterm-based approach (1993-2026) where an
xterm window displayed program output.  The pty approach is used by
Eclipse CDT, VS Code, and gdbgui.  Benefits: no focus stealing, no
external window, Wayland-compatible.

### Start symbol

`e_get_start_symbol()` returns the configured entry point (default
"main").  Used by `e_mk_brk_main()` across all backends.  Ctrl-G R
and Ctrl-G T both set a breakpoint at the start symbol before
running.  Configurable via "Start sYmbol:" in Compiler-Options.

### System frame auto-skip

When gdb steps into system/library code (libc, runtime startup),
the source file is not found locally.  Instead of showing an error,
`e_read_output` sends `tbreak main` + `continue` to skip past the
system frames and stop at the next user function.

### Clean program exit

`e_read_output` detects `[Inferior ... exited` in gdb's response
and calls `e_d_quit()` cleanly instead of letting subsequent
commands produce "no process to debug" errors.

### Event-driven I/O (1.6.3)

The 1993 debugger communication was synchronous: `e_d_line_read`
blocked waiting for gdb.  When the debugged program blocked on
`fgets`/`scanf`, the IDE froze.

`wpe_fd_poll` (we_fdloop.c) multiplexes all file descriptors with
`poll()` and per-fd callbacks.  In X11, `e_x_getch` registers
the X11 connection fd (NULL callback, poll-only) so keyboard events
wake up poll alongside gdb and pty data.

The incremental gdb parser (`e_d_accum_t`) processes one line per
callback instead of looping in `e_read_output`.  Three functions:
`e_d_accum_init` (register fds), `e_d_accum_line` (process one line),
`e_d_accum_complete` (parse response, flush pty, position cursor).

Program output goes through the pty.  `e_d_pty_read_to_messages`
drains the pty and appends char-by-char to the Messages buffer via
`e_d_messages_append_char` (handles \r, \n, \b, printable).
`e_d_messages_redraw` scrolls the viewport and refreshes.

After each step, `e_d_flush_inferior_stdout` sends
`call (void)fflush(0)` to gdb so that `printf` without `\n` is
visible immediately (same technique as Eclipse CDT and gdbgui).

User keyboard input during `fgets`: `e_debug_console_input`
(we_edit.c) writes to the pty master via `e_d_pty_write_utf8`.
The pty terminal discipline echoes characters back, which the pty
callback displays in Messages.

### Dead key compose and UTF-8 input (1.6.3)

XIM/ibus does not always compose dead keys (the dead key event is
consumed by `XFilterEvent` but `XmbLookupString` returns the base
character uncomposed).  xterm, GTK, and Qt all implement their own
compose fallback.

`e_compose_dead` (we_xterm.c) is a static compose table covering
acute, grave, diaeresis, tilde, circumflex, and cedilla for all
Latin-1 accented characters.  `e_compose_pending` holds the dead
key keysym between the dead key event and the base character event.
Applied in `e_x_getch` (editor), `e_x_kbhit` (dialogs), and
`e_debug_console_input` (debug console).

`e_codepoint_to_utf8` (we_edit.c) converts a Unicode codepoint to
1-4 UTF-8 bytes.  Used by the editor (e_ins_nchar with multi-byte
buffer), the debug console (e_d_pty_write_utf8), and dialog fields
(e_dialog_encode_char).

Dialog text fields (e_schreib_leiste in we_e_aus.c) were refactored
for UTF-8: `e_schr_nchar` decodes multi-byte sequences for rendering,
cursor movement uses `e_utf8_prev`/`e_utf8_next`, and backspace/delete
operate on whole codepoints.  Helper functions: `e_utf8_charlen`,
`e_utf8_decode_at`, `e_utf8_visual_width`.
