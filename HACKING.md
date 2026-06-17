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

#### Chrome must follow the compositor: index by Z-LEVEL, not window number

The chrome is a SEPARATE pass from the cell compositor, so it must use
the SAME window ordering or the two disagree and scrollbars paint over
the wrong windows.  Two parallel arrays, both indexed by **z-level**
(`f[1]` = bottom of the stack ... `f[mxedt]` = top = active):

| Array | Meaning |
|-------|---------|
| `cn->f[z]`   | the `FENSTER *` at z-level `z` |
| `cn->edt[z]` | that window's NUMBER |

The cell compositor `e_repaint_desk_nopic` walks `f[1..mxedt]` and treats
`f[mxedt]` as active.  `wpe_render_chrome` MUST do the same: iterate
`WpeEditor->f[w]`, active = `(w == mxedt)`.

Bug we hit (#165): the chrome instead indexed `f[edt[w]]` (by window
NUMBER) and tested `edt[w] == edt[curedt]` for active.  That matches the
z-level walk ONLY while `edt` is the identity permutation -- i.e. while
no window has ever been raised.  After a mouse click raises a covered
window, `e_switch_window` permutes `edt`, the two passes disagree, and
the chrome reads the wrong window's geometry -> scrollbars bleed.
**Rule: any pass that draws per-window must index `f[]` by z-level and
take `f[mxedt]` as active, exactly like `e_repaint_desk_nopic`.**

#### Clipping a covered window's scrollbar: SET algebra, not even-odd

The fluid scrollbars are drawn directly to the Cairo surface, after the
cells, z-order-blind.  So each window's bars must be clipped to the part
of it still visible: its own rectangle MINUS the union of every window
stacked above it.  Helpers (we_render_cairo.c):

- `cr_window_pixel_rect(f, *r)` -- window cells -> device-pixel rect.
- `e_chrome_visible_region(ed, w)` -- `f[w]`'s rect minus the higher
  windows, as a `cairo_region_t`.
- `cr_clip_to_region(reg)` -- clip the context to that region.

**Do NOT clip with an even-odd fill-rule path** (rect_self + rect_cover1
+ rect_cover2, `CAIRO_FILL_RULE_EVEN_ODD`).  Even-odd computes the XOR of
the rectangles, which equals "self minus the covers" only while the
covering windows do not overlap EACH OTHER.  In a cascade they do: a
point under TWO higher windows lies in 3 rectangles (self + 2 covers) ->
odd -> even-odd wrongly keeps it INSIDE the clip, so window 1's scrollbar
reappears exactly in the triple-overlap band (the original #165 report:
"the scrollbar of the first window reappears where it intersects the
third").  `cairo_region_subtract_rectangle` does true integer-rectangle
set difference and a region is a list of DISJOINT rectangles, so filling
them with the default winding rule is an exact clip.  **Rule: subtract
overlapping regions with `cairo_region_t`, never with an even-odd path.**

A second, orthogonal check (`e_chrome_col/row_content_visible`) skips the
bar entirely when a window is covered down to just its border, so it
shows the plain border line instead of a lone floating scrollbar.

Regression net: `tests/x11/test_window_zoom_redraw.py` --
`test_triple_overlap_no_scrollbar_bleed` builds the exact 3-window
double-cover via Size/Move and asserts no bleed; `test_zoom_*`,
`test_cascade_*`, `test_click_reorder_*` cover the z-order paths.

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

## ncurses mouse drag and window relayout (1.6.3)

### xterm mouse mode 1002

Terminal mouse drag requires mode 1002 (`\033[?1002h`), which reports
`REPORT_MOUSE_POSITION` events while a button is held.  Mode 1000
(the default ncurses click mode) only reports press and release, not
motion.  Mode 1002 is the same protocol used by Midnight Commander,
tmux, and vim for drag operations.

Enabled at terminal init (`e_t_initscr`), disabled at exit
(`e_endwin`).  SGR extended coordinates (`\033[?1006h`) are enabled
alongside mode 1002 for terminals wider than 223 columns, where the
X10 encoding cannot represent the coordinate.

### g_mouse_buttons: global button state

```c
int g_mouse_buttons = 0;  /* bitmask: bit 0 = button 1, etc. */
```

Declared in `we_term.c`, declared `extern` in `edit.h`.  Updated
in two places:

1. `e_t_getch` KEY_MOUSE handler: decodes `BUTTON1_PRESSED` /
   `BUTTON1_RELEASED` and sets/clears bit 0.
2. `fk_t_mouse`: reads `g_mouse_buttons` to determine if a drag
   is in progress and sets `e_mouse.k` accordingly.

Why a global instead of using `e_mouse.k` directly: `e_mouse.k` is
written by `fk_t_mouse` and read by `we_mouse.c` drag loops.  When
a popup opens during a drag (e.g. F9 error popup), the popup's own
event loop consumes PRESSED/RELEASED pairs, leaving `e_mouse.k` in
an inconsistent state when the popup closes.  `g_mouse_buttons`
tracks the physical button state independently of any popup's event
consumption.

### e_mouse_flush (we_mouse.c)

Called at the exit of popup/menu event loops (`e_mess_win`,
`e_opt_kst`) to resynchronise mouse state after a popup:

1. Drain all pending `KEY_MOUSE` events from the ncurses queue
   via `getmouse()`.
2. Re-enable mode 1002 (`\033[?1002h\033[?1006h`) -- ncurses
   internally disables mouse tracking when processing certain
   escape sequences, and popup event loops can leave it off.
3. Reset `g_mouse_buttons = 0`.

Without this, the first drag after a popup hangs because stale
RELEASED events are queued and mode 1002 is silently disabled.

### Window relayout on resize (we_wind.c)

`e_relayout_windows()` handles proportional window scaling when
the terminal is resized (SIGWINCH in terminal mode, ConfigureNotify
in X11 mode).

The algorithm uses **edge-attachment detection**: for each window
coordinate (top, bottom, left, right), it checks whether the
coordinate was touching the screen edge before the resize.  Edge-
attached coordinates follow the edge (stay at row 1, or track
MAXSLNS-2).  Interior coordinates scale proportionally:

```c
int e_scale_y(int y, int old_h, int new_h)
{
    return (y * new_h + old_h / 2) / old_h;  /* round-to-nearest */
}
```

Round-to-nearest prevents ratio drift: without it, repeated 1-row
shrink/grow cycles accumulate truncation error and windows slowly
collapse toward the top.

Minimum window dimensions: 3 lines height, 10 columns width.
Minimum terminal: MAXSLNS >= 6, MAXSCOL >= 30.

### SCHIRM_INBOUNDS bounds checking (unixmakr.h)

```c
#define SCHIRM_INBOUNDS(y, x) \
    ((y) >= 0 && (y) < MAXSLNS && (x) >= 0 && (x) < MAXSCOL)
```

All schirm access macros (`e_pr_char`, `e_gt_char`, `e_gt_col`,
`e_pt_col`, `e_gt_flags`, `e_pt_flags`) check bounds before
accessing `schirm[y * MAXSCOL + x]`.  Out-of-bounds writes are
silently dropped; out-of-bounds reads return space/0.

This prevents heap corruption when dialogs or windows extend beyond
the terminal during resize.  Before SCHIRM_INBOUNDS, a dialog wider
than the terminal would write past the schirm allocation and corrupt
the heap, producing crashes that appeared random.

### Messages window positioning (we_wind.c, we_edit.c, we_prog.c)

`e_position_messages_window()` enforces a 2/3 + 1/3 vertical split
when Messages is created or repositioned:

- Editor windows occupy the top 2/3 of the screen
- Messages occupies the bottom 1/3
- If only one editor window exists, it is resized to make room
- Messages fills the full width (columns 0 to MAXSCOL-1)

Called from: `e_edit` (Messages creation), `e_run_make` (F9),
and `e_relayout_windows` (resize with Messages open).

### Dialog resize during SIGWINCH (we_opt.c, we_term.c)

Dialogs (`e_opt_kst`) survive terminal resize via a restart pattern:
WPE_RESIZE closes the dialog PIC, re-centers coordinates with
`e_opt_center_dialog`, and jumps to `e_opt_kst_restart` which
creates a fresh PIC and redraws.

Three mechanisms prevent crashes and artifacts:

1. **SIGWINCH blocking in e_t_refresh**: `sigprocmask(SIG_BLOCK)`
   prevents a second SIGWINCH from resizing ncurses stdscr while
   `move()`/`addch()` are iterating.  The pending signal is
   delivered after `sigprocmask(SIG_SETMASK)` restores the mask.
   The iteration is also clamped to `min(MAXSLNS, LINES)` x
   `min(MAXSCOL, COLS)` as a second guard.

2. **PIC invalidation before repaint**: `e_free_all_pics()` frees
   all FENSTER PICs before `e_repaint_desk` creates fresh ones.
   Without this, `e_firstl` creates PICs with stale pre-resize
   coordinates that corrupt the PIC stack and hang F9.

3. **Dialog clipping**: when the terminal is smaller than the
   dialog's original dimensions, `e_opt_element_visible()` skips
   elements outside the frame during initial draw, and the
   interactive loop is bypassed entirely (only Esc and resize
   accepted).  `e_repaint_desk_nopic()` redraws all windows with
   frames on dialog close without creating PICs.

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
5=pdb, 6=a68g, 7=DAP. The first seven are text backends: each uses the
same pipe infrastructure but differs in prompt detection, command
syntax, and output parsing. In X11 mode, the debugger runs via named
pipes (`npipe[0-4]`); in terminal mode, regular pipes
(`rfildes/wfildes/efildes`).

`DEB_DAP` (7) is different in kind: a Debug Adapter Protocol client that
gives source-level debugging for any language with a DAP adapter (Go via
Delve, Rust via gdb, ...). It is its own subsystem -- engine + transports
+ a per-language descriptor -- documented separately in **HACKING-DAP.md**.
This section covers only the text backends.

The **LSP client** is a sibling subsystem on the same JSON-RPC plumbing:
`we_lsp.c` is the editor-free engine (spawn a language server, initialize,
manage the open document, issue requests) and a bridge in `we_debug.c`
binds the `Alt-Q` prefix to IDE features -- completion, hover, go-to-def,
references, rename, diagnostics -- for Scala, C/C++, Python, Go and Rust.
A new server is a row in the `e_lsp_servers[]` descriptor table, not new
code. Documented separately in **HACKING-LSP.md**.

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
`poll()` and per-fd callbacks.  Both modes integrate with the poll
loop:

- **X11**: `e_x_getch` registers the X11 connection fd (NULL
  callback, poll-only) so keyboard events wake up poll alongside
  gdb and pty data.
- **Terminal**: `e_t_getch_poll` registers STDIN_FILENO, uses
  `timeout(50)` for non-blocking `getch()` (50ms allows ncurses
  to assemble mouse escape sequences), then `wpe_fd_poll(-1)` to
  wait for any fd.  `e_t_utf8_assemble` reads continuation bytes
  to decode multi-byte UTF-8 into codepoints.

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

## The PIC view lifecycle (and how to not double-free it)

Every window keeps an off-screen backing view, `f->pic`, a `PIC` whose
`buf` is a full-screen `SCREENCELL` array (~19 KB).  `e_open_view`
allocates it; `e_close_view` restores the saved background and frees it.

The repaint path re-creates these views: `e_rep_win_tree` /
`e_x_repaint_desk` -> `e_firstl` -> `e_ed_kst` -> `e_change_pic`, and
`e_change_pic` closes the *existing* `f->pic` (with `e_close_view`) before
opening a fresh one.  That design has one hard invariant:

> **Never free a window's view without immediately NULLing the pointer.**
> If you `free(f->pic)` but leave `f->pic` dangling, the next repaint hands
> the freed pointer to `e_change_pic` -> `e_close_view`, which either
> double-frees it ("free(): invalid pointer", SIGABRT) or reads the
> freed-and-reused struct (garbage coordinates -> out-of-bounds index ->
> SIGSEGV).

Always release a view through the one helper that cannot forget the NULL:

```c
void e_free_view(PIC **pp);   /* frees pp->buf and *pp, then sets *pp = NULL */
```

Used by every bulk view-release: `e_switch_window`, `e_ed_cascade`,
`e_ed_tile` (we_wind.c) and `e_x_repaint_desk` (we_xterm.c).  Four separate
sites grew the same `free(...->pic); /* forgot NULL */` bug over the years;
routing them all through `e_free_view` makes the whole class impossible by
construction.  Do not hand-write `free(cn->f[i]->pic)` -- call the helper.

## Debugging crashes and memory bugs

xwpe is interactive, so crashes usually surface during a soak test rather
than in the pyte suite.  Tools, cheapest first:

### coredumpctl (already enabled on most systemd distros)

`/proc/sys/kernel/core_pattern` pipes cores to `systemd-coredump`.  After
*any* crash:

```sh
coredumpctl info <PID>      # quick: prints the stack trace summary
coredumpctl gdb  <PID>      # full gdb on the core; then: bt full, frame N, p *pic
```

Rule: **do not rebuild between the crash and the analysis.**  The core's
addresses only resolve against the exact binary that crashed (build-id
match); a rebuilt `we` gives a garbage backtrace.  To keep a core for later:

```sh
coredumpctl dump <PID> --output /tmp/xwpe.core
gdb -q -batch -ex 'bt full' -ex 'frame 0' -ex 'info locals' we /tmp/xwpe.core
```

A real example: a `./xwpe` SIGSEGV resolved in seconds to
`e_close_view (we_wind.c:468)` with `pic->a = {1762978608, 32531}` --
obviously a freed-and-reused `PIC`, i.e. a use-after-free in the repaint
path (fixed via `e_free_view`, see above).

### gdb wrapper (native speed, auto-backtrace)

```sh
gdb -q -batch -ex run -ex 'bt full' -ex 'info registers' \
    --args ./xwpe docs/examples/debug_test.c
```

Use during the soak test when you can reproduce on demand; it prints the
backtrace the instant it crashes, no core needed.

### AddressSanitizer build (best for use-after-free / heap bugs)

ASan reports *both* the alloc/free site and the bad-access site at the
moment of the bug, before it degrades into a confusing garbage-coordinate
crash.  ~2x slowdown -- fine interactively.

```sh
make clean
make CFLAGS="-fsanitize=address -g -O1 -fno-omit-frame-pointer" \
     LDFLAGS="-fsanitize=address"
cp we we-asan
make clean && make          # restore the normal binary
ln -sf we-asan wpe-asan     # terminal mode  (basename not starting with 'x')
ln -sf we-asan xwpe-asan    # X11 mode       (basename starting with 'x')
```

The mode is chosen from argv[0] (we_unix.c): a basename starting with `x`
selects X11.  Run with leak detection off (X11/pango/glib leak at exit,
noise here) and the report sent to a file (the TUI clobbers stderr):

```sh
ASAN_OPTIONS="detect_leaks=0:log_path=/tmp/xwpe-asan:abort_on_error=1" \
    ./xwpe-asan docs/examples/debug_test.c
# on a heap bug, read /tmp/xwpe-asan.<pid>  (abort_on_error also leaves a
# core for coredumpctl)
```

The PIC use-after-free above would have printed
"freed by e_x_repaint_desk / used by e_close_view" directly.

### valgrind (deepest, slowest ~20-50x)

```sh
valgrind --leak-check=full --track-origins=yes ./xwpe file 2>vg.log
```

Catches invalid reads/writes even when they do not segfault, and finds
leaks.  The 24-year debugger memory leak was found this way, driven through
gdb sessions by a headless pyte harness (a clean Alt-X exit so valgrind
prints its report).  See the pyte tests in `tests/` and the leak-hunt
write-up for the driving technique.

#### Reading the report: what is actionable, what is noise

A leak sweep (1.6.3) ran a full debug session under valgrind for every
backend and both front-ends, driven headless through a pty (wpe) or xdotool
(xwpe) with a clean Alt-X exit:

| run | definitely + indirectly lost |
|-----|------------------------------|
| wpe + gdb / pdb / jdb | 0 bytes, 0 errors (after the brp/c_sw fixes) |
| xwpe (X11) + gdb      | per-operation clean; init-only remainder (below) |

Triage rules learned:
- **Per-operation / per-frame leaks are the real bugs.** A block lost once per
  window close, per debug step, or per repaint is what to chase.  The two
  found this way -- `s->brp` and `c_sw`, leaked at every window close -- are
  fixed in e_close_window.  The fluid-scrollbar chrome allocates a
  `cairo_region_t` every frame (e_chrome_visible_region) and the sweep
  confirmed it is balanced -- 0 region leaks.
- **One-time init leaks at exit are benign and usually NOT worth a freeze.**
  In X11, `cr_init_pango_font` (the Pango layout/font) and `WpeXtermInit` (X
  colour/key tables) are allocated once and never freed -- the OS reclaims
  them instantly.  The render backend even has `cr_cleanup` written but
  nothing calls it (X11's WpeDisplayEnd is WpeNullFunction).  Wiring a real
  X11 teardown belongs on 1.6.4, not a one-liner in the shared e_exit.
- **Most X11 "definitely lost" is library, not xwpe.** fontconfig, pangoft2,
  pango and Xlib keep global caches they never free at exit (~1.6 MB here).
  Universal to any Pango/X program; unfixable in xwpe.  Filter the stacks for
  `we_*` / `e_*` frames before believing a number.

Drivers used for the sweep live outside the repo (xwpe-dev/), not in tests/:
valgrind is too slow (~30x) for the default `run-tests.sh` net.

## Data files and `XWPE_LIB` (1.6.5)

xwpe loads several data files at runtime: `syntax_def` (highlighting rules),
the in-app help (`help.xwpe`, `help.key`) and the option file (`xwperc`).  The
search is in `e_lib_dir()` (we_unix.c):

    XWPE_LIB (if set and non-empty)   ->   else LIBRARY_DIR (configure prefix)

`LIBRARY_DIR` is baked in by `configure` to the install prefix.  `XWPE_LIB`
overrides it so an *uninstalled* build finds its data files:
`XWPE_LIB="$(pwd)" ./wpe foo.c`.  All the data-file call sites route through
`e_lib_dir()` -- the `WpeSyntaxGetSystem` macro (WeProg.h) and the
`e_mkfilename(e_lib_dir(), ...)` opens in we_edit.c / we_fl_fkt.c / we_opt.c.

Caveat: `syntax_def` keeps its name in the build tree, but the help files are
`help.xwpe_eng` / `help.key_eng` there and only get their final names on
`make install`; so `XWPE_LIB`=build-tree gives full highlighting but Help needs
an install (or renamed copies).  Personal override still wins for syntax:
`~/.xwpe/syntax_def` is tried before the system path (`WpeSyntaxGetPersonal`),
which is the classic "stale personal copy shadows the new rules" trap.

`contrib/xwpe-env` is a shell-aware helper (the `brew shellenv` idiom): it emits
`export` / `set -gx` for bash/zsh/fish and sets `XWPE_LIB`, plus `JAVA_HOME`
(Homebrew, macOS `java_home`, or `/usr/lib/jvm`), clangd's keg-only `PATH`, and
the Coursier app dir.  Invoke via `sh` so a lost +x bit never matters:
`eval "$(sh contrib/xwpe-env)"` (bash/zsh) or
`sh contrib/xwpe-env --shell fish | source` (fish).

## Portability: macOS and the BSDs (1.6.5)

The tree builds the terminal `wpe` on macOS/BSD, not just Linux.  The gotchas
that bit, all now handled:

- **openpty()'s header differs.**  `<pty.h>` is glibc-only; macOS/BSD use
  `<util.h>`, some BSDs `<libutil.h>`.  `configure` probes all three
  (`AC_CHECK_HEADERS`) and we_dap.c / we_prog.c / we_debug.c pick via
  `HAVE_*_H`.  The link is found by `AC_SEARCH_LIBS([openpty],[util])` -- libc
  on macOS, `-lutil` on Linux/BSD.
- **`WEXITSTATUS` needs an lvalue.**  macOS expands it to `*(int*)&(x)`, so
  `WEXITSTATUS(pclose(pp))` does not compile -- store the status first
  (we_prog.c).
- **Terminal mouse without X11/GPM.**  The `MOUSE` macro (model.h) gated
  `struct mouse` on X11 or GPM only; a `--without-x --without-gpm` build left it
  undefined while we_term.c's ncurses KEY_MOUSE path uses it.  `MOUSE` now also
  keys off `NCURSES` (ncurses has its own xterm mouse, independent of X11/GPM).
- **Run from the build dir.**  `make` creates the `wpe`/`xwpe`/`xwe` symlinks
  next to `we` (`all-local` in Makefile.am), since the mode is chosen by
  `argv[0]` basename; no `make install` needed to get `./wpe`.

macOS terminal note (not a build issue): Terminal.app sends Option as accented
characters, not a Meta/Esc prefix, so the whole `Alt-Q` layer looks dead --
enable "Use Option as Meta key", or use iTerm2/kitty (which also ship modern
terminfo).  Documented in README.

The X11 `xwpe` build runs on **macOS under XQuartz** too (Homebrew libX11 +
Xft/Cairo), with three X11-specific gotchas, all now handled in `WeXterm.c`:

- **WM_NAME dropped.**  `XmbSetWMProperties` silently omits WM_NAME/WM_ICON_NAME
  when the libX11 locale module is incomplete (the Homebrew build) -- window
  managers, `wmctrl` and `xdotool --name` then cannot find the window.  We now
  also set the legacy STRING props (`XStoreName`/`XSetIconName`) and the EWMH
  `_NET_WM_NAME`/`_NET_WM_ICON_NAME` (UTF8_STRING) directly.
- **altMask parser was off by one.**  The `xwpe*altMask: modN` X-resource read
  the digit at index 4 (`"mod1"[4]` is past the string), so `mod1`..`mod5` were
  ignored and Alt stayed on the built-in default.  Fixed to scan past `"mod"` +
  whitespace.  Matters on macOS, where XQuartz delivers Alt on a different mod
  than the Linux default, so the resource is how you point Alt at the right key.
- **Pango metrics vs the seed grid.**  `cr_init_pango_font()` overwrites
  `font_width/height` with Pango's real metrics; on macOS they differ enough
  from the seed values that a dialog opened before the first `ConfigureNotify`
  (the start-up File Manager) rendered squashed.  We refit `MAXSCOL`/`MAXSLNS`
  to the real font right after `wpe_render_cairo_init()`.  On Linux the seed and
  Pango metrics nearly coincide, so the bug never surfaced there.

The headless X11 GUI suite (`tests/x11/`) is portable to XQuartz: it picks the
window manager (`matchbox` on CI, `twm` from XQuartz -- with a minimal NoTitle /
RandomPlacement / NoGrabServer rc), the XWD decoder (ImageMagick 6 `convert`, or
IM7 `magick`+`xwdtopnm` since IM7 dropped the XWD delegate), pins the window to a
fixed geometry so coordinate scans match under either WM, finds the window by
WM_CLASS (the Homebrew `xdotool --name` regex is broken), and probes
`otool`/`ldd` for the libvterm linkage.  66/66 pass on both XQuartz and matchbox.

## File Manager geometry (1.6.5)

The File Manager box used to be hardcoded (`f->a=(11,2)`, `f->e=+55,+20`) --
fixed and pinned near the top-left.  `e_fm_centered_geometry()` (we_fl_unix.c)
now sizes it to ~3/4 of the screen, clamps to a sane min (~56x21) / max (78x30),
and centres it.  Nothing else changed: every inner widget already derives from
the window box, because `NUM_COLS_ON_SCREEN` / `NUM_LINES_ON_SCREEN` (edit.h)
are defined window-relative (`f->e.x - f->a.x`, `f->e.y - f->a.y`), so the
dir-tree and file-list boxes, the field widths and the button hints all scale
off `f->a`/`f->e` for free.
