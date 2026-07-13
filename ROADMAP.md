# xwpe roadmap

This document describes the direction for xwpe. Items are not commitments --
they are areas we intend to explore, ordered by impact and feasibility.
Feedback from users and testers shapes priorities.

The short version: the 1993 core logic is solid. Fred Kruse wrote a correct,
well-structured editor; Dennis Payne kept it alive. What needed modernising
was the rendering and I/O layers around that core -- not the editor itself.
Most of that modernisation has now shipped.

## Shipped in 1.6.x

The rescue is done and then some. Highlights of what landed since the
v1.5.31 takeover:

### Rendering (X11)
- **Xft, then Cairo+Pango.** The 1993 path drew text with raw Xlib
  (XDrawImageString) and a hand-rolled border tracker. v1.6.2 migrated all
  X11 text to Xft (anti-aliased TrueType, fontconfig fallback). v1.6.3 went
  further: a render-backend abstraction (`WpeRenderBackend`, in Kruse's own
  function-pointer style) with a **Cairo+Pango backend that is now the active
  X11 renderer**; Xft is preserved as the `--without-cairo` fallback.
- **Color emoji** in X11 via libXft/Cairo BGRA (Noto Color Emoji), plus emoji
  in terminal mode through ncursesw.
- **Pixmap double-buffering / single-flush-per-frame.** Zero flicker on resize,
  scroll, and window move. One present per frame -- the same model the Wayland
  backend uses.
- **Modern scrollbars.** Proportional thin track and thumb, triangular arrows,
  fluid real-time drag and mouse-wheel scroll, both axes.
- **System monospace font** detection (matches the user's terminal font).

### Native Wayland backend (1.6.8, hardened in 1.6.9)
- **The graphical editor runs natively on Wayland** -- no X server, no
  XWayland -- auto-selected from `WAYLAND_DISPLAY`. It plugs into the render
  abstraction as a new backend (additive, not a rewrite): a `wl_display`
  connection, `wl_surface` + **xdg-shell** for window management, `wl_shm`
  buffers, and input via `wl_keyboard` / `wl_pointer` with **xkbcommon**. The
  same Cairo+Pango calls and single-flush-per-frame model map directly onto
  `wl_surface.commit`.
- **Clipboard interop** via `wl_data_device` (both CLIPBOARD and PRIMARY,
  interoperating with `wl-copy`/`wl-paste`), and a real-desktop robustness pass
  (resize/relayout coalescing, pointer re-entrancy) in 1.6.9.
- Covered by a headless `tests/wayland` suite (Xvfb + weston) that mirrors the
  X11 suite, so a regression in the Wayland input/render path is caught.

### UTF-8 everywhere
- Full UTF-8 in the terminal (ncursesw + a SCREENCELL buffer model) and in
  X11 (Xft): accents, Cyrillic, CJK, combining/wide characters.
- UTF-8 in dialog text fields, and X11 dead-key compose for accented input.

### Debugger and run
- **Event-driven debugging.** poll()-based multiplexing of the display, the
  gdb pipe, and a pty. When the program blocks on fgets/scanf you type in the
  Messages window and the UI never freezes (same pattern as st, cgdb, foot).
- Six debug backends reachable from one UI: **gdb, jdb (Java), pdb (Python)**,
  with the groundwork shared. Program output captured into Messages (Ctrl-G P),
  no xterm, no screen switching.
- Compilers/linters: gcc, g++, gfortran, fpc, javac, python3, pdflatex,
  **perl (-c), cobc (COBOL)** -- nine in all, with configurable error parsing.

### Dialogs and UI fidelity (Borland spirit)
- Checkbox/radio marks made visible in X11, Tab/Shift-Tab navigation, Alt
  hotkeys, legible dialog colors, UTF-8 fields.
- **Popups behave like Borland's**: no inert maximize box, a single working
  close box (click [X] = Esc); document windows keep their zoom box.
- WordStar vs modern block marking actually differ now.
- **Full mouse on the bare Linux console (GPM)** -- pointer, click, window
  drag and resize, with no X. wpe's GPM connection is pumped through the
  event loop, matching the terminal-emulator mouse (xterm 1002). On a real
  VT it behaves like Midnight Commander's mouse, plus drag/resize.

### Old bugs, finally closed
- **A 30-year memory leak.** `e_close_view()` freed the window backing buffer
  only for `sw < 2`, leaking a full-screen buffer on every repaint -- worst
  during debug stepping. The guard is byte-for-byte identical back to Kruse's
  1.4.2 (1997); Payne documented it as "location unknown" in 2000. valgrind
  driven through a headless harness finally cornered it: ~940 KB/2 sessions
  down to 3 bytes.
- A ~30-year heap-buffer-overflow in block Move (Ctrl-K V), caught with
  AddressSanitizer. The "Esc needs three presses" annoyance, gone after 20+
  years. Several X11 crashes (use-after-free in the desktop repaint) fixed.

### Quality net
- Automated tests: a pyte (VT100) suite for `wpe` and a headless X11 GUI suite
  for `xwpe` (Xvfb + matchbox + xdotool + Pillow), per menu section, run by
  `tests/run-tests.sh`. This is how the dual-mode regressions get caught --
  behaviour can diverge between the ncurses and X11 front-ends, so both are
  tested.
- Modern autotools build (configure.ac + Makefile.am), a 12-chapter Texinfo
  manual, and an archived history repo for the Kruse/Payne-era material.

## v1.6.4 -- console polish

Small, self-contained items, mostly about the bare Linux console (no X). These
need manual testing on a real VT (the pyte/Xvfb harnesses are emulators, so
they cannot reproduce console-only behaviour).

- **Console scrollbar/chrome glyphs.** The scrollbar now uses ACS_CKBOARD /
  ACS_BLOCK (shipped) so it renders on any console font. The maximize/close
  title-bar glyphs (geometric squares, multiplication-X) are not in console
  fonts and still need a CP437/ASCII fallback when `TERM=linux`.
- **Unicode scrollbars inside dialogs** (windows already do this).

## v1.7 -- modern protocols and platform reach

### DAP (Debug Adapter Protocol) client
The standard editor-debugger protocol (VS Code, Emacs, Neovim). A DAP client
gives xwpe debugging for any language with a DAP server (Rust, Go, JS, C#, ...)
without writing a backend per language. JSON-RPC over the stdio pipe
infrastructure xwpe already has; reuse the Messages buffer and existing
breakpoint/step/watch UI. First target: debugpy (Python), with pdb as the
baseline to compare against.

### X11 clipboard
xwpe has PRIMARY selection support. Still needed: CLIPBOARD selection (the
Ctrl-C/Ctrl-V clipboard), wired to xwpe's copy/paste keys, with UTF-8 content.

### Code quality (Bonnema study + internals)
Guus Bonnema put real work into ~391 refactoring commits on the amagnasco
`experimental` branch (2018) -- the guy earned a serious read, and it deserves
study before we cherry-pick: German-to-English identifier renames
(schirm -> screen), function extraction, consistent formatting. Not all of it
is adoptable; take the best parts. Alongside: a debugger-backend table instead
of if/else chains, dialog layout calculations instead of hardcoded
coordinates, and safer buffer sizing -- the boy-scout cleanup of the 1993
"pointer soup", done opportunistically with the test net in place.

## v2.0 -- longer term

- **DAP server mode**: expose xwpe's debugger to external editors.
- **Git integration**: Payne's TODO mentioned CVS/RCS; modernised to git
  status/diff/blame in the editor.
- **LSP client**: completion, hover, go-to-definition (a major architectural
  change, hence 2.0).
- **Config modernisation**: replace the binary xwperc with a text format.

## Non-goals

Things we explicitly do not plan to do:
- **Rewrite in another language.** The C codebase is solid and maintainable;
  the 1993 logic is correct. Only the rendering and I/O layers needed work.
- **GUI toolkit migration (GTK, Qt).** xwpe's value is being a lightweight
  terminal / X11 / Wayland IDE. A full toolkit would make it just another
  IDE.
- **Plugin system.** The codebase is small enough that features go in directly;
  a plugin API would add complexity without clear users.
