# xwpe roadmap

This document describes the direction for xwpe after the v1.6.2 rescue
release. Items are not commitments -- they are areas we intend to
explore, ordered by impact and feasibility. Feedback from users and
testers will shape priorities.

## v1.6.x maintenance

Bug fixes and small improvements that do not require architectural
changes. These can ship as point releases (v1.6.3, v1.6.4) at any time.

- Texinfo manual updates for Python, LaTeX, pdb, output buffer
- Alt-R N single-error revisit
- Perl syntax highlighting and F9 (perl -c)
- COBOL syntax highlighting and F9 (cobc)
- Debian packaging updates (Suggests, Build-Depends, Closes)
- pdb watch variables (Ctrl-G W)
- pdb step into (F7)

## v1.7 -- X11 modernisation

The 1993 X11 rendering uses raw Xlib calls (XDrawImageString,
XDrawLine, XFillRectangle) and a manual border tracking system
(extbyte/altextbyte) that has no awareness of window stacking. This
causes visual artefacts (border echoes, resize flicker) that required
extensive workarounds in v1.6.2. Dennis Payne identified this in his
TODO: "Use higher-level APIs instead of xlib and the lowest curses
interface."

### X11 rendering

Replace the raw Xlib rendering pipeline with a higher-level approach.
Candidates, in order of preference:

1. **Pixmap double-buffering** (Xlib only, no new deps): draw to an
   off-screen Pixmap, XCopyArea to the window. Eliminates flicker and
   simplifies border cleanup. Lowest risk, stays within Xlib.

2. **XCB + Cairo**: XCB is the modern replacement for Xlib. Cairo
   provides anti-aliased 2D rendering, UTF-8 text (via Pango), and
   automatic double-buffering. Adds dependencies but solves UTF-8,
   fonts, and flicker in one move.

3. **SDL2**: portable across X11 and Wayland. Provides a framebuffer
   abstraction. Would require implementing text rendering (SDL_ttf).
   Higher effort but opens the door to Wayland support.

### X11 emoji and modern fonts

v1.6.2 added UTF-8 text support via XFontSet + Xutf8DrawImageString.
Accented characters, Cyrillic, and most BMP characters render correctly.
However, emoji requires Xft with font fallback (fontconfig), which
cannot be mixed with the existing XDrawImageString/GC rendering pipeline
without a full rendering rewrite.

The v1.7 plan: **migrate all X11 text rendering to Xft**. This means:
- Replace XDrawImageString with XftDrawStringUtf8 everywhere
- Replace XFontStruct with XftFont for metrics
- Use XftDrawRect for background fills instead of XDrawImageString's
  implicit background
- Use fontconfig font fallback for emoji (libXft 2.3.5+ has BGRA
  support for color emoji via Noto Color Emoji)
- This is the approach used by st (suckless terminal), confirmed
  working with color emoji since libXft 2.3.5 (2022)

This is a ~200-line change concentrated in e_x_refresh and
fk_show_cursor, but it requires careful testing because ALL text
rendering changes at once. Partial Xft (hybrid with GC) was attempted
in v1.6.2 and failed due to state conflicts between the two systems.

### X11 clipboard

xwpe has partial X11 selection support (PRIMARY only, via
e_x_cp_X_to_buffer / e_x_copy_X_buffer / e_x_paste_X_buffer).
Needs:
- CLIPBOARD selection support (the "Ctrl-C/Ctrl-V" clipboard)
- Integration with xwpe's internal copy/paste keys (Ctrl-Ins/Shift-Ins
  or Ctrl-K B/K)
- UTF-8 content in selections

### Popup/window management

The PIC-based save/restore system (e_open_view / e_close_view) works
but does not save X11 border state (extbyte). The v1.6.2 workaround
(clearing extbyte interiors in e_x_refresh) is pragmatic but fragile.

Options for v1.7:
- **ncurses panels** (terminal mode): the panel library manages
  overlapping windows automatically. A migration plan exists (see
  the approved plan in session notes). This eliminates the manual
  save/restore and its associated bugs.
- **X11 mode**: if Cairo is adopted, use Cairo surfaces for popup
  content. If not, Pixmap per popup.

## v1.7 -- Debugger improvements

### DAP (Debug Adapter Protocol) client

DAP is the standard protocol for editor-debugger communication (used by
VS Code, Emacs, Neovim). A DAP client in xwpe would provide debugging
for any language with a DAP server (Rust, Go, JavaScript, C#, etc.)
without writing a backend for each.

Implementation approach:
- JSON-RPC over stdio (same pipe infrastructure xwpe already uses)
- Reuse the existing Messages buffer for output
- Map DAP events to xwpe's existing UI (breakpoints, stepping, watch)
- Start with debugpy (Python) as the first DAP server to test with,
  since we already have pdb as a comparison baseline

### Additional debugger backends

If DAP is not yet ready, individual backends can be added:
- **Perl debugger** (perl -d): 7th backend, similar to pdb
- **COBOL**: cobc compiles to native code, debug with gdb

## v1.7 -- Code quality

### Bonnema study

Guus Bonnema wrote ~391 commits of refactoring on the amagnasco
experimental branch (2018). Worth studying for:
- German-to-English identifier renames (schirm -> screen, etc.)
- Function extraction (we_screen.c from we_term.c)
- Consistent formatting

Not all changes are adoptable (Doxygen, dbg.h, src/ restructure add
weight without clear benefit). Cherry-pick the best parts.

### Internal improvements

- Debugger backend table (struct array) instead of if/else chains
- Dialog layout calculations instead of hardcoded coordinates
- syntax_def format documentation and possible simplification
- Safe buffer sizing (replace char tmp[80] patterns)

## v2.0 -- Longer term

- **Wayland support**: via SDL2 or a dedicated Wayland backend
- **DAP server mode**: expose xwpe's debugger to external editors
- **Git integration**: Payne's TODO mentioned CVS/RCS; modernised to
  git status, diff, blame in the editor
- **LSP client**: language server protocol for code completion,
  hover info, go-to-definition (major architectural change)
- **Config file modernisation**: replace binary xwperc with text format

## Non-goals

Some things we explicitly do not plan to do:
- **Rewrite in another language**: xwpe's C codebase is solid and
  maintainable. The 1993 core logic is correct; only the rendering
  and I/O layers need modernising.
- **GUI toolkit migration** (GTK, Qt): xwpe's value is being a
  lightweight terminal/X11 IDE. A full GUI toolkit would make it
  just another IDE.
- **Plugin system**: the codebase is small enough that features can
  be added directly. A plugin API would add complexity without clear
  users.
