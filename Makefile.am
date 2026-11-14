SUBDIRS = docs

AUTOMAKE_OPTIONS = foreign

# --- Reproducible release tarballs ---
# "make dist" otherwise stamps the current date/time into the Texinfo
# @UPDATED line (via build-aux/mdate-sh) and into the per-file mtimes that
# tar records, so the tarball's sha256 changes on every run and downstream
# packagers (Debian, Fedora, openSUSE, Homebrew, the BSD ports) see a moving
# checksum. Pin every embedded timestamp to the last commit. A value already
# set in the environment still takes precedence.
SOURCE_DATE_EPOCH ?= $(shell git -C '$(top_srcdir)' log -1 --format=%ct 2>/dev/null)
# Export it ONLY when it is a real value.  Building from a release/archive tarball
# there is no .git, so the git command yields an EMPTY string; exporting
# SOURCE_DATE_EPOCH='' makes clang/gcc reject the build ("must be a non-negative
# decimal integer").  Packager builds (rpm, dpkg) set a valid value in the
# environment, which the ?= above keeps.  (A GNU-make $(if)/$(eval) one-liner,
# not an ifeq/endif block -- automake reserves `endif` for its own conditionals.)
$(if $(strip $(SOURCE_DATE_EPOCH)),$(eval export SOURCE_DATE_EPOCH))

# automake honours SOURCE_DATE_EPOCH for mdate-sh and zeroes the gzip header,
# but it does not pin the per-file mtimes in the dist tar; normalise them here
# so the archive is byte-for-byte stable across repeated "make dist" runs.
dist-hook:
	if test -n "$(SOURCE_DATE_EPOCH)"; then \
	  find "$(distdir)" -exec touch --no-dereference --date="@$(SOURCE_DATE_EPOCH)" {} + ; \
	fi

# --- Compiler flags ---
# gnu17 is required: the code uses K&R-compatible GNU extensions
AM_CFLAGS = $(XWPE_STDFLAG) \
  $(NCURSESW_CFLAGS) \
  $(X_CFLAGS) \
  $(XFT_CFLAGS) \
  $(CAIRO_CFLAGS) \
  $(PANGO_CFLAGS) \
  $(VTERM_CFLAGS) \
  $(JSONC_CFLAGS) \
  -DLIBRARY_DIR=\"$(pkglibdir)\" \
  -DXWPE_INFODIR=\"$(infodir)\" \
  -Wno-incompatible-pointer-types \
  -Wno-implicit-function-declaration \
  -Wno-int-conversion \
  -Wno-old-style-declaration

# Force-include config.h without modifying every .c file
AM_CPPFLAGS = -include config.h

# --- The single binary ---
bin_PROGRAMS = we

# The key reference is generated from the key table (we_bindings.c): this
# program prints docs/chapters/keys-generated.texi and the dump of a style's
# bindings.  make check compares its output with the files kept in the tree.
noinst_PROGRAMS = tools/keymap-texi
tools_keymap_texi_SOURCES = tools/keymap-texi.c we_bindings.c we_menus.c
tools_keymap_texi_CFLAGS = $(AM_CFLAGS)
# The menu rows without the editor's functions (we_menus.c, WPE_MENU_TABLE_ONLY).
tools_keymap_texi_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_MENU_TABLE_ONLY

we_SOURCES = \
  we_main.c \
  we_block.c \
  we_clip.c \
  we_unix.c \
  we_child.c \
  we_e_aus.c \
  we_edit.c \
  we_buffer.c \
  we_lineend.c \
  we_write.c \
  we_fl_fkt.c \
  we_fl_unix.c \
  we_hfkt.c \
  we_rowfind.c \
  we_menue.c \
  we_menus.c \
  we_menurows.c \
  we_mousedrain.c \
  we_termrelease.c \
  we_keymap.c \
  we_bindings.c \
  we_bars.c \
  we_mouse.c \
  we_opt.c \
  we_startcmd.c \
  we_startplan.c \
  we_path.c \
  we_number.c \
  we_progname.c \
  we_listsel.c \
  we_helpbuf.c \
  we_errpat.c \
  we_wind.c \
  we_prog.c \
  we_progn.c \
  we_debug.c \
  we_debprompt.c \
  we_debpath.c \
  we_debsess.c \
  we_debreq.c \
  we_gdbmi.c \
  WeString.c \
  WeSyntax.c \
  WeExpArr.c \
  WeLinux.c \
  we_gpm.c \
  we_xterm.c \
  WeXterm.c \
  we_term.c \
  we_render.c \
  we_render_cairo.c \
  we_fdloop.c \
  we_paint.c \
  we_layout.c \
  we_layout_desk.c \
  we_testdump.c \
  we_trace.c \
  we_vterm.c \
  we_dap_proto.c \
  we_dap.c \
  we_bsp.c \
  we_lsp.c \
  we_lsp_pos.c

we_LDADD = \
  $(NCURSESW_LIBS) \
  $(X_LIBS) \
  $(XFT_LIBS) \
  $(CAIRO_LIBS) \
  $(PANGO_LIBS) \
  $(VTERM_LIBS) \
  $(JSONC_LIBS) \
  $(GPM_LIBS) \
  $(UTIL_LIBS) \
  $(EXECINFO_LIBS) \
  $(PTHREAD_LIBS) \
  -lz

# --- Native Wayland backend (optional, --with-wayland) ---
# The xdg-shell protocol glue is generated at build time from the system
# wayland-protocols XML by wayland-scanner: the client header must exist before
# any .c that includes it is compiled (BUILT_SOURCES), and the generated .c is a
# nodist source (regenerated locally, never shipped in the dist tarball).
# we_wayland.c / we_render_wayland.c are listed in we_SOURCES under the
# conditional, so automake still distributes them when the backend is off.
if HAVE_WAYLAND
we_SOURCES += we_wayland.c we_render_wayland.c
nodist_we_SOURCES = xdg-shell-protocol.c primary-selection-unstable-v1-protocol.c \
  xdg-decoration-unstable-v1-protocol.c
BUILT_SOURCES = xdg-shell-client-protocol.h \
  primary-selection-unstable-v1-client-protocol.h \
  xdg-decoration-unstable-v1-client-protocol.h
CLEANFILES = xdg-shell-client-protocol.h xdg-shell-protocol.c \
  primary-selection-unstable-v1-client-protocol.h \
  primary-selection-unstable-v1-protocol.c \
  xdg-decoration-unstable-v1-client-protocol.h \
  xdg-decoration-unstable-v1-protocol.c
AM_CFLAGS += $(WAYLAND_CFLAGS)
we_LDADD += $(WAYLAND_LIBS)

xdg-shell-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
	  $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@
xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code \
	  $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

# Primary selection (middle-click paste): xwpe owns PRIMARY as well as CLIPBOARD,
# matching the X11 backend.  Same wayland-scanner generation as xdg-shell.
primary-selection-unstable-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
	  $(WAYLAND_PROTOCOLS_DIR)/unstable/primary-selection/primary-selection-unstable-v1.xml $@
primary-selection-unstable-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
	  $(WAYLAND_PROTOCOLS_DIR)/unstable/primary-selection/primary-selection-unstable-v1.xml $@

# Server-side decorations (title bar + resize borders): xwpe asks the compositor
# to draw the window frame via xdg-decoration.  Same wayland-scanner generation.
xdg-decoration-unstable-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
	  $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@
xdg-decoration-unstable-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
	  $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

# BUILT_SOURCES only fires for the default `all` target; this explicit
# dependency guarantees the generated client headers exist before we_wayland.c
# is compiled even for `make we` or a parallel `make -j`.
we_wayland.$(OBJEXT): xdg-shell-client-protocol.h \
  primary-selection-unstable-v1-client-protocol.h \
  xdg-decoration-unstable-v1-client-protocol.h
endif

# --- AI assistant (optional, --enable-ai) ---
# Grafted onto the fd-loop + LSP spine (Ollama / OpenAI-compatible / Claude).
# Listed under the conditional so automake still ships the sources in the dist
# tarball when AI is off, exactly like the Wayland backend above.  json-c is
# already linked; the plain-HTTP backends (Ollama, localhost OpenAI-compatible)
# need no extra library.  TLS libs are pulled in only under --enable-ai-tls.
if WPE_AI
we_SOURCES += we_ai.c we_ai_http.c we_ai_ui.c we_ai_diff.c we_ai_agent.c we_ai_ws.c we_ai_path.c
if WPE_AI_TLS
AM_CFLAGS += $(AITLS_CFLAGS)
we_LDADD += $(AITLS_LIBS)
endif
if WPE_AI_AGENT_HOST
we_SOURCES += we_ai_host.c
endif
endif

# --- Header files (for dist) ---
noinst_HEADERS = \
  Xwpe.h \
  WeString.h \
  WeXterm.h \
  WeProg.h \
  WeExpArr.h \
  we_debprompt.h \
  we_debpath.h \
  we_debsess.h \
  we_debreq.h \
  we_gdbmi.h \
  we_menurows.h \
  we_menus.h \
  we_mousedrain.h \
  we_termrelease.h \
  we_keymap.h \
  we_bindings.h \
  we_bars.h \
  we_render.h \
  we_render_glyphs.h \
  we_palette.h \
  we_childsig.h \
  we_child.h \
  we_startcmd.h \
  we_startplan.h \
  we_path.h \
  we_number.h \
  we_progname.h \
  we_listsel.h \
  we_helpbuf.h \
  we_errpat.h \
  we_fieldtake.h \
  we_wayland.h \
  we_fdloop.h \
  we_paint.h \
  we_layout.h \
  we_testdump.h \
  we_buffer.h \
  we_lineend.h \
  we_write.h \
  we_rowfind.h \
  we_utf8.h \
  we_trace.h \
  we_vterm.h \
  we_dap_proto.h \
  we_dap.h \
  we_bsp.h \
  we_lsp.h \
  we_lsp_pos.h \
  we_ai.h \
  we_ai_path.h \
  we_ai_http.h \
  we_ai_http_sync.h \
  we_ai_host.h \
  we_clip.h \
  attrb.h \
  edit.h \
  keys.h \
  makro.h \
  messages.h \
  model.h \
  options.h \
  progr.h \
  unixkeys.h \
  unixmakr.h

# --- Build-tree symlinks so ./wpe (and ./xwpe, ./xwe) work WITHOUT installing.
#     The mode is chosen by argv[0] basename: run as ./wpe and the programming
#     layer (Alt-Q LSP, F9 compile, debugger) is on; ./we is the plain editor.
#     Mirrors the install-exec-hook below, but in the build dir. ---
all-local: we$(EXEEXT)
	rm -f wpe$(EXEEXT) && $(LN_S) we$(EXEEXT) wpe$(EXEEXT)
if HAVE_X11
	rm -f xwpe$(EXEEXT) xwe$(EXEEXT) && \
	  $(LN_S) we$(EXEEXT) xwpe$(EXEEXT) && \
	  $(LN_S) we$(EXEEXT) xwe$(EXEEXT)
endif

clean-local:
	rm -f wpe$(EXEEXT) xwpe$(EXEEXT) xwe$(EXEEXT)

# --- Symlinks: wpe, xwpe, xwe -> we ---
install-exec-hook:
	cd $(DESTDIR)$(bindir) && \
	  rm -f wpe && \
	  $(LN_S) we wpe
if HAVE_X11
	cd $(DESTDIR)$(bindir) && \
	  rm -f xwpe xwe && \
	  $(LN_S) we xwpe && \
	  $(LN_S) we xwe
endif

uninstall-hook:
	rm -f $(DESTDIR)$(bindir)/wpe
if HAVE_X11
	rm -f $(DESTDIR)$(bindir)/xwpe \
	      $(DESTDIR)$(bindir)/xwe
endif

# --- Library/data files ---
xwpedatadir = $(pkglibdir)
xwpedata_DATA = syntax_def

# Help files need renaming on install
hicoloricondir = $(datadir)/icons/hicolor

install-data-local:
	$(MKDIR_P) $(DESTDIR)$(xwpedatadir)
	$(INSTALL_DATA) $(srcdir)/help.xwpe_eng $(DESTDIR)$(xwpedatadir)/help.xwpe
	$(INSTALL_DATA) $(srcdir)/help.key_eng $(DESTDIR)$(xwpedatadir)/help.key
	$(MKDIR_P) $(DESTDIR)$(hicoloricondir)/scalable/apps
	$(INSTALL_DATA) $(srcdir)/icons/xwpe.svg \
		$(DESTDIR)$(hicoloricondir)/scalable/apps/xwpe.svg
	@if command -v rsvg-convert >/dev/null 2>&1; then \
		for s in 256 128 64 48; do \
			$(MKDIR_P) $(DESTDIR)$(hicoloricondir)/$${s}x$${s}/apps; \
			rsvg-convert -w $$s -h $$s $(srcdir)/icons/xwpe.svg \
				-o $(DESTDIR)$(hicoloricondir)/$${s}x$${s}/apps/xwpe.png; \
		done; \
		for s in 32 24 16; do \
			$(MKDIR_P) $(DESTDIR)$(hicoloricondir)/$${s}x$${s}/apps; \
			rsvg-convert -w $$s -h $$s $(srcdir)/icons/xwpe-small.svg \
				-o $(DESTDIR)$(hicoloricondir)/$${s}x$${s}/apps/xwpe.png; \
		done; \
	else \
		echo "rsvg-convert not found; only scalable SVG installed"; \
	fi

uninstall-local:
	rm -f $(DESTDIR)$(xwpedatadir)/help.xwpe \
	      $(DESTDIR)$(xwpedatadir)/help.key \
	      $(DESTDIR)$(hicoloricondir)/scalable/apps/xwpe.svg
	@for s in 256 128 64 48 32 24 16; do \
		rm -f $(DESTDIR)$(hicoloricondir)/$${s}x$${s}/apps/xwpe.png; \
	done

# --- Man pages ---
# xwpe.1 is the real page; we.1/wpe.1/xwe.1 are ".so man1/xwpe.1" redirects.
dist_man1_MANS = we.1 wpe.1 xwe.1 xwpe.1

# --- Desktop file + AppStream MetaInfo ---
desktopdir = $(datadir)/applications
desktop_DATA = xwpe.desktop
metainfodir = $(datadir)/metainfo
metainfo_DATA = io.codeberg.mendezr.xwpe.metainfo.xml

# --- Unit tests ---
check_PROGRAMS = test_lsp_positions test_checkheader test_fl_sel_len test_syntax_def test_fdloop test_clipboard test_lsp_buffer test_lsp_doctor test_lsp_semantic test_lsp_truecolor test_dap test_dap_nopidfd test_dap_dlv test_dap_gdb test_dap_lldb test_dap_scala test_bsp_close test_lsp_requests test_lsp_scala test_lsp_clangd test_lsp_python test_lsp_gopls test_lsp_rust test_debprompt test_debpath test_debsess test_debreq test_gdbmi test_menurows test_mousedrain test_termrelease test_child test_childsig test_keymap test_bindings test_menus test_paint test_splice test_lineend test_write_owner edit_driver test_startcmd test_startplan test_path test_progname test_fieldtake test_layout test_rowfind test_rowfind_copy test_rowfind_reject test_listsel test_helpbuf test_number_parse test_errpat

# The owner of the selected entry of a list window (we_listsel.c): a
# selection names an entry or nothing, the blank row that ends a project
# list is never an entry, Delete cannot remove it, and the directory tree
# names no subdirectory for an ancestor row.
test_listsel_SOURCES = tests/test_listsel.c we_listsel.c
test_listsel_CFLAGS = $(XWPE_STDFLAG)

# The owner of a help or Info token taken into storage (we_helpbuf.c): a
# token is written whole or cut at a character, never past its buffer, and
# a mark that does not fit is refused.
test_helpbuf_SOURCES = tests/test_helpbuf.c we_helpbuf.c
test_helpbuf_CFLAGS = $(XWPE_STDFLAG)

# The owner of a compiler's message pattern (we_errpat.c): a pattern is read
# once into rows and warning tests, a text cut inside a token is refused with
# a reason, a surprising reading is noted, and no byte outside the text is
# read, whatever the text.
test_errpat_SOURCES = tests/test_errpat.c we_errpat.c
test_errpat_CFLAGS = $(XWPE_STDFLAG)

# The owner of how long a path the editor holds may be (we_path.c): copy,
# join and cat that refuse instead of cutting, at the boundary of a buffer.
test_path_SOURCES = tests/test_path.c we_path.c
test_path_CFLAGS = $(XWPE_STDFLAG)

# The owner of the name of what a build makes from a source (we_progname.c):
# the extension and stem of a name, the program, the object and the debug
# binary under Exedir through the start plan's operand, a result that does
# not fit refused whole, and the project's two names.
test_progname_SOURCES = tests/test_progname.c we_progname.c we_path.c we_startplan.c we_startcmd.c we_child.c we_fdloop.c we_debreq.c
test_progname_CFLAGS = $(XWPE_STDFLAG)
test_progname_LDADD = $(UTIL_LIBS)

# The owner of a number the user or a file gives the editor (we_number.c):
# the parser reads a text whole and refuses overflow and trailing text, the
# range of each kind, and the field's accept (saturating Go to Line).  Links
# the editor's string helpers only; the status row is a stub of the test.
test_number_parse_SOURCES = tests/test_number_parse.c we_number.c WeString.c
test_number_parse_CFLAGS = $(AM_CFLAGS)

# The owner of the copy out of a dialog field (we_fieldtake.h): a text that
# does not fit its destination is refused, never written past it.
test_fieldtake_SOURCES = tests/test_fieldtake.c
test_fieldtake_CFLAGS = $(XWPE_STDFLAG)

# The owner of the start plan: we_startplan.c with the store (we_startcmd.c)
# and the children's owner it starts through.  The operand rule on every
# role, a folder value refused as an option, links followed out of the
# folder, a removal outside dropped and a write outside asked, the home
# folder never a root, a plan equal in effect to the user's own, one
# record per plan, the cover check of a step, and mutants of the rules.
test_startplan_SOURCES = tests/test_startplan.c we_startplan.c we_startcmd.c we_child.c we_fdloop.c we_debreq.c
test_startplan_CFLAGS = $(XWPE_STDFLAG)
# The rows that make an allocation fail take over malloc and realloc, which
# needs a linker with --wrap.  Without one (Apple's ld) the test is built
# without them and reports those rows as SKIP.
if HAVE_LD_WRAP
test_startplan_CFLAGS += -DWPE_TEST_LD_WRAP=1
test_startplan_LDFLAGS = -Wl,--wrap=malloc -Wl,--wrap=realloc
endif
test_startplan_LDADD = $(UTIL_LIBS)

# The owner of the question about a folder's commands: we_startcmd.c alone.
# The store's round trip (escapes, a changed command, one record per folder
# and key, the cap), every way a store fails closed (missing, another
# version, a bad line, a link), folders through a link, the user's own and
# the built-in texts, and the printable form of a command.
test_startcmd_SOURCES = tests/test_startcmd.c we_startcmd.c
test_startcmd_CFLAGS = $(XWPE_STDFLAG)

# The text's owner: we_buffer.c alone (the editor's side of its seam is
# stubbed by the test).  Marks and breakpoints move with their lines, Undo
# and Redo give back exactly the text in one key per action, a refused
# allocation changes nothing, a view keeps no undo.
test_splice_SOURCES = tests/test_splice.c we_buffer.c
test_splice_CFLAGS = $(XWPE_STDFLAG)

# The owner of a file's line endings: we_lineend.c on the buffer's owner.
# A file's bytes become rows and rows become bytes, in both styles, and
# what the buffer made goes back into it as it was.
test_lineend_SOURCES = tests/test_lineend.c we_buffer.c we_lineend.c
test_lineend_CFLAGS = $(XWPE_STDFLAG)

# The owner of writing the user's text (we_write.c), the editor's side
# played by stubs: the one sentence a failed write says, the backup copy
# (private, counted as made only when whole), the restore after a write
# that fails half way (by rename, through a link, with a hard link, one that
# fails), the size limit as an outcome and SIGXFSZ put back.
test_write_owner_SOURCES = tests/test_write_owner.c we_write.c
test_write_owner_CFLAGS = $(XWPE_STDFLAG)

# The same owner behind a line protocol, for the edit model's walks
# (tests/edit_model/cdriver.py).  Built by `make check`, not run by it.
edit_driver_SOURCES = tests/edit_driver.c we_buffer.c
edit_driver_CFLAGS = $(XWPE_STDFLAG)

# The screen's owner: we_paint.c with the fd loop only (the desk is given
# through hooks the test sets).  A paint asked for under a box waits, the
# same paint is kept once, the queue is bounded and says what it refused,
# paints run in the order asked, one that opens a box leaves the rest for
# later, a box inside a box keeps the screen, and nothing runs on leave.
test_paint_SOURCES = tests/test_paint.c we_paint.c we_fdloop.c
test_paint_CFLAGS = $(XWPE_STDFLAG)

# The box layout owner: we_layout.c with the screen's owner and the fd loop
# only (the desk is given through hooks the test sets).  Where each rule puts
# a box at the displays' sizes, the stack the walk lays out outermost first,
# and the stamp that keeps a background saved at another size from being put
# back.
test_layout_SOURCES = tests/test_layout.c we_layout.c we_paint.c we_fdloop.c
test_layout_CFLAGS = $(XWPE_STDFLAG)

# The owner of a search over a row: we_rowfind.c alone.  A differential
# against the engine run by hand on every start of a row, the invariants of
# every hit, the progress of Search Again and Change All on rows wrapped at
# a chosen width, and the speed bounds.  Built twice: the second build forces
# the copy path that a C library without REG_STARTEND needs, where a word
# boundary at the start of a call is judged without the byte before it.
test_rowfind_SOURCES = tests/test_rowfind.c we_rowfind.c
test_rowfind_CFLAGS = $(XWPE_STDFLAG)
test_rowfind_copy_SOURCES = tests/test_rowfind.c we_rowfind.c
test_rowfind_copy_CFLAGS = $(XWPE_STDFLAG)
test_rowfind_copy_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_ROWFIND_NO_STARTEND
# The third build has no UTF-8 ctype to make (OpenBSD, a minimal system): the
# engine then reads bytes under a process locale that is not UTF-8, and the
# owner rejects a match that covers part of a character instead of widening it.
test_rowfind_reject_SOURCES = tests/test_rowfind.c we_rowfind.c
test_rowfind_reject_CFLAGS = $(XWPE_STDFLAG)
test_rowfind_reject_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_ROWFIND_NO_UTF8_CTYPE

# The key decoder's table: we_keymap.c alone, no editor and no display
# library.  Every bound pair has one row, the curses, keysym and escape
# sequence forms of a key give the same code, a chord the Debug menu shows is
# decoded, and an unbound key gives 0 and never a letter.
test_keymap_SOURCES = tests/test_keymap.c we_keymap.c we_bindings.c
test_keymap_CFLAGS = $(XWPE_STDFLAG)

# The key bindings (we_bindings.c) and the bottom bars built from them
# (we_bars.c), no editor and no display.  Every key code in both styles gives
# what the dispatchers gave before the keys became data, the Ctrl-K, Ctrl-O
# and Ctrl-G letters and the two single-key questions the same, every bar
# entry has the label, column, highlight and click code it had, and the rules
# of the table hold (no key bound twice, one bar key per action and style).
test_bindings_SOURCES = tests/test_bindings.c we_bindings.c we_bars.c
test_bindings_CFLAGS = $(AM_CFLAGS)

# The menu table (we_menus.c) and the key table, no editor: the rows, the
# three bars, the filter that decides a row from the key style, block mode,
# editor mode and display, and the rule that no menu Alt letter is a bound
# key.  Built with WPE_MENU_TABLE_ONLY the rows name no function and the
# builder is left out, so the test links with no editor symbol.
test_menus_SOURCES = tests/test_menus.c we_menus.c we_bindings.c
test_menus_CFLAGS = $(AM_CFLAGS)
test_menus_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_MENU_TABLE_ONLY

# The children's owner: we_child.c with the fd loop only (the status line is a
# hook, stubbed).  Real children: twenty ending at once each report their own
# status, one that ignores SIGTERM is killed after the grace, the end returns
# at once, an end before the watch cannot be lost, a signal after the end
# (or between the reap and the report) reaches nobody, a forgotten child
# leaves no callback, timer or slot, a full table or a full loop starts
# nothing, and the whole group is ended.  Its specs are built by hand, not
# by a start plan, so this one program is built with the refusal of
# unplanned starts off (WPE_CHILD_MECHANICS_TEST); tests/check-start-plan.sh
# fails if any other target or the editor names that switch.
test_child_SOURCES = tests/test_child.c we_child.c we_fdloop.c
test_child_CFLAGS = $(XWPE_STDFLAG) -DWPE_CHILD_MECHANICS_TEST
test_child_LDADD = $(UTIL_LIBS)

# A read that waits survives the editor's SIGCHLD: the handler is installed
# by the editor's own function, children end during a blocking read.  Also
# the short commands' cases live in test_child.
test_childsig_SOURCES = tests/test_childsig.c we_child.c we_fdloop.c
test_childsig_CFLAGS = $(XWPE_STDFLAG)
test_childsig_LDADD = $(UTIL_LIBS)

# The test includes we_fdloop.c itself (the id counter is private to it), so
# the file is not listed here again: it would be defined twice.
test_fdloop_SOURCES = tests/test_fdloop.c
test_fdloop_CFLAGS = $(XWPE_STDFLAG)

# Debugger prompt window unit test: we_debprompt.c only, no editor, always runs.
# This is the test F95 should have had -- a prompt split across two reads, and
# the 7-byte a68g prompt driven at every split length, which is what wrote two
# bytes past a 12-byte scratch in e_d_line_read.  Worth building under asan as
# well: the overflow is only visible there.
test_debprompt_SOURCES = tests/test_debprompt.c we_debprompt.c
test_debprompt_CFLAGS = $(XWPE_STDFLAG)

# Debugger start path unit test: we_debpath.c only, no editor, always runs.
# A source path too long to hand to a debugger is refused whole, never cut;
# no file system lets the editor be driven into that case, so it is checked
# here at the exact buffer edge.
test_debpath_SOURCES = tests/test_debpath.c we_debpath.c
test_debpath_CFLAGS = $(XWPE_STDFLAG)

# Debugger session state unit test: we_debsess.c only, no editor, no
# debugger, always runs.  What gdb, pdb, jdb and a68g print (transcripts in
# tests/transcripts/) is read into the session as the editor reads it: watch
# values and errors, the frame a stop marks, pdb's and a68g's stop lines,
# and jdb's set answer written into the middle of a breakpoint hit.
test_debsess_SOURCES = tests/test_debsess.c we_debsess.c we_gdbmi.c
test_debsess_CFLAGS = $(XWPE_STDFLAG)

# gdb/MI parser unit test: we_gdbmi.c and we_debsess.c only, no editor, no
# debugger, always runs.  What gdb 18.1 printed in MI mode (transcripts in
# tests/transcripts/gdb-mi-*.txt) is parsed into records, cut into lines
# at every byte position, and taken into the session: the stop's file and
# line, a breakpoint answered or refused with gdb's words, a SIGINT stop
# told from the user's, the exit code, and the console words for Messages.
test_gdbmi_SOURCES = tests/test_gdbmi.c we_gdbmi.c we_debsess.c
test_gdbmi_CFLAGS = $(XWPE_STDFLAG)

# Debugger request engine unit test: we_debreq.c only, no editor, no
# debugger, always runs.  A fake clock and a transcript stand for the world:
# the deadline and the waiting line, requests going out one at a time with
# the queue's bound, cancel ending every request, and a stop line that comes
# in the middle of an answer going to the editor instead of the request.
test_debreq_SOURCES = tests/test_debreq.c we_debreq.c
test_debreq_CFLAGS = $(XWPE_STDFLAG)

# Dropdown highlight unit test: we_menurows.c only, no editor, always runs.
# Wrap at both ends, step over dividers, the open/Home/End rows, the edge
# menus no editor menu has today -- in microseconds, where the pyte suite
# needs an editor and a pseudo-terminal per case.
test_menurows_SOURCES = tests/test_menurows.c we_menurows.c
test_menurows_CFLAGS = $(XWPE_STDFLAG)

# Mouse queue drain unit test: we_mousedrain.c only, no curses, always runs.
# A curses whose getmouse() answers "ok, empty event" forever (OpenBSD's) must
# not leave the drain spinning; the test has its own alarm so a hang is a failure.
test_mousedrain_SOURCES = tests/test_mousedrain.c we_mousedrain.c
test_mousedrain_CFLAGS = $(XWPE_STDFLAG)

# The string a signal handler writes to give the terminal back: we_termrelease.c
# only, no curses, always runs.
test_termrelease_SOURCES = tests/test_termrelease.c we_termrelease.c
test_termrelease_CFLAGS = $(XWPE_STDFLAG)

# Clipboard helpers unit test: RFC 4648 base64 + OSC 52 framing (we_clip.c
# only, no editor/X11). Always runs (no skip).
test_clipboard_SOURCES = tests/test_clipboard.c we_clip.c
test_clipboard_CFLAGS = $(XWPE_STDFLAG)

test_checkheader_SOURCES = tests/test_checkheader.c
test_checkheader_CFLAGS = $(XWPE_STDFLAG)
test_checkheader_LDFLAGS =

test_fl_sel_len_SOURCES = tests/test_fl_sel_len.c
test_fl_sel_len_CFLAGS = $(XWPE_STDFLAG)
test_fl_sel_len_LDFLAGS =

test_syntax_def_SOURCES = tests/test_syntax_def.c
test_syntax_def_CFLAGS = $(XWPE_STDFLAG)
test_syntax_def_LDFLAGS =

# LSP buffer-serialization unit test: the pure e_lsp_join_lines invariant (one
# '\n' per editor line, WPE_WR not doubled).  Links we_lsp.c only; no server,
# always runs (no skip).
test_lsp_buffer_SOURCES = tests/test_lsp_buffer.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_lsp_buffer_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_buffer_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP Doctor dedup unit test: the pure e_lsp_doc_is_new gate (re-pushed Doctor
# rendered only when changed; reset per session shows it again).  we_lsp.c only.
test_lsp_doctor_SOURCES = tests/test_lsp_doctor.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_lsp_doctor_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_doctor_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# The owner of the language server's columns (we_lsp_pos.c) alone: the units
# of every glyph shape in both encodings, the two conversions and their
# clamps, the rows of a text, the offer and the answer, and the decoder.
test_lsp_positions_SOURCES = tests/test_lsp_positions.c we_lsp_pos.c
test_lsp_positions_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_positions_LDADD = $(JSONC_LIBS)

# LSP semantic-tokens decode unit test: the pure delta-decode (we_lsp_pos.c).
test_lsp_semantic_SOURCES = tests/test_lsp_semantic.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_lsp_semantic_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_semantic_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP requests answered on the loop: each answer reaches its own request,
# the server's own requests are answered meanwhile, Esc, the deadline, the
# server's end and the close.  The test plays the server itself.
test_lsp_requests_SOURCES = tests/test_lsp_requests.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_lsp_requests_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_requests_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# Semantic-token truecolor palette unit test: pure token->RGB-slot lookup.
test_lsp_truecolor_SOURCES = tests/test_lsp_truecolor.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_lsp_truecolor_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_truecolor_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# DAP transport and start-machine unit test: the engine + json-c, no xwpe,
# no adapter (the test plays one on a loopback socket).  we_dap_sync.c, the
# engine's calls that wait, is linked into these tests only: the editor
# follows the engine from its loop and never waits on an adapter.  An
# adapter starts through its start plan (we_startplan.c, with its store
# we_startcmd.c), in these tests too.
test_dap_SOURCES = tests/test_dap.c we_dap.c we_dap_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_dap_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_dap_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# The same test with the engine built as on a system without pidfd (the
# BSDs, macOS), so the wait for an adapter that closed every descriptor is
# tested on Linux too.
test_dap_nopidfd_SOURCES = tests/test_dap.c we_dap.c we_dap_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_dap_nopidfd_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS) -DDAP_NO_PIDFD
test_dap_nopidfd_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# DAP engine integration test: drives a real `dlv dap` (SKIPs without dlv+go).
test_dap_dlv_SOURCES = tests/test_dap_dlv.c we_dap.c we_dap_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_dap_dlv_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_dap_dlv_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# DAP stdio transport integration test: drives a real `gdb --interpreter=dap`
# on a rustc-built binary (the Rust slice).  SKIPs without gdb+rustc.
test_dap_gdb_SOURCES = tests/test_dap_gdb.c we_dap.c we_dap_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_dap_gdb_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_dap_gdb_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# Same stdio transport against `lldb-dap` (the LLVM adapter; native on macOS).
# SKIPs without lldb-dap+rustc.
test_dap_lldb_SOURCES = tests/test_dap_lldb.c we_dap.c we_dap_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_dap_lldb_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_dap_lldb_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# JVM/Scala path: BSP bootstrap (we_bsp.c) + DAP TCP-client transport against a
# real scala-cli/Bloop.  SKIPs without scala-cli; slow on a cold coursier cache.
test_dap_scala_SOURCES = tests/test_dap_scala.c we_bsp.c we_bsp_sync.c we_dap.c we_dap_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_dap_scala_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_dap_scala_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# How the build server's launcher is ended, against a fake launcher (the test
# program itself); no scala-cli needed.  The close returns at once and the
# children's owner (we_child.c) ends the launcher on the fd loop, with no
# exit descriptor, the same on every system.
test_bsp_close_SOURCES = tests/test_bsp_close.c we_bsp.c we_bsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_bsp_close_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_bsp_close_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP engine: drives a real Metals language server on a single Scala file
# (hover/definition/completion/diagnostics).  SKIPs without metals + scala-cli.
test_lsp_scala_SOURCES = tests/test_lsp_scala.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_lsp_scala_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_scala_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP engine, breadth: drives a real clangd on a single C file with the SAME
# engine (only language id + spawned command differ).  Fast (no JVM/build
# server).  SKIPs without clangd.
test_lsp_clangd_SOURCES = tests/test_lsp_clangd.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_lsp_clangd_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_clangd_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP engine, breadth: drives a real Python server (pyright or pylsp) on a single
# .py file -- whichever is installed -- with the SAME engine.  SKIPs without one.
test_lsp_python_SOURCES = tests/test_lsp_python.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_lsp_python_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_python_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP engine, breadth: drives a real gopls on a single .go file in a tiny module.
# SKIPs without gopls + go.
test_lsp_gopls_SOURCES = tests/test_lsp_gopls.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_lsp_gopls_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_gopls_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP engine, breadth: drives a real rust-analyzer on a single .rs file in a tiny
# crate.  Slowest (indexes std on cold start).  SKIPs without rust-analyzer + cargo.
test_lsp_rust_SOURCES = tests/test_lsp_rust.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c
test_lsp_rust_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_rust_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

TESTS = test_lsp_positions test_checkheader test_fl_sel_len test_syntax_def test_fdloop test_clipboard test_lsp_buffer test_lsp_doctor test_lsp_semantic test_lsp_truecolor test_dap test_dap_nopidfd test_dap_dlv \
  test_dap_gdb test_dap_lldb test_dap_scala test_bsp_close test_lsp_requests \
  test_lsp_scala test_lsp_clangd test_lsp_python test_lsp_gopls test_lsp_rust test_debprompt test_debpath test_debsess test_debreq test_gdbmi test_menurows test_mousedrain test_termrelease \
  test_child test_childsig test_keymap test_bindings test_menus test_paint test_splice test_lineend test_write_owner test_startcmd test_startplan test_path test_progname test_fieldtake test_layout test_rowfind test_rowfind_copy test_rowfind_reject test_listsel test_helpbuf test_number_parse test_errpat

# One owner of input and of waiting: tests/check-one-owner.sh reads the
# undefined symbols of the editor's own objects (the list below) with $(NM)
# and fails when code outside the loop and the displays waits or reads on
# its own.  Offenders still to go are in tests/one-owner.allow.
TESTS += tests/check-one-owner.sh

# One owner of what the editor decides about a program it starts: the
# start plan (we_startplan.c).  Every line of the start sites that builds a
# command line, a working folder or a shell line, or names a folder's
# program keys, is counted per function in tests/start-plan.allow, with
# the pass that moves it into a plan; the counts may only fall.
TESTS += tests/check-start-plan.sh

# One owner of how the assistant quotes a word for /bin/sh:
# wpe_ai_shell_quote.  tests/check-shell-quote.sh fails when a string in
# we_ai*.c holds a hand-written '%s'.
TESTS += tests/check-shell-quote.sh

# One owner of a file's line endings (we_lineend.c): no second reader or
# writer of the bytes of a line break.  Offenders still to go are in
# tests/line-ends.allow.
TESTS += tests/check-line-ends.sh

# One owner of writing the user's text (we_write.c): the only code that
# opens the user's file for writing, judges the write and builds the
# sentence a failed one says.  tests/check-write-owner.sh fails when another
# file opens a file for writing, drops or compares with numbers the result
# of a save, or spells the old message.  Offenders still to go are in
# tests/write-owner.allow, each with the pass that moves it.
TESTS += tests/check-write-owner.sh

# One owner of how long a command to a debugger may be: the request engine
# (we_debreq.h).  tests/check-debug-command.sh fails when a site sizes a
# debugger command by a number of its own; the caps that are not debugger
# lines are in tests/debug-command.allow, each with its reason.
TESTS += tests/check-debug-command.sh

# One owner of how a debugger request ended and of the words that say it: the
# request engine (we_debreq.[ch]).  tests/check-chain-outcome.sh fails when a
# step that owns a verdict has no failed, a chain ends without saying why, a
# chain is finished around the owner, the debugger's input is written
# without going through the gated writer, a sentence of a request's end is
# written outside the engine's header, a debugger names no other-prompt table,
# or the .pdbrc keyword rule comes back (tests/chain-outcome.allow holds the
# sentences of other owners, each with its reason).
TESTS += tests/check-chain-outcome.sh

# One owner of how long a path the editor holds may be (we_path.h) and of a
# dialog field's way out (we_fieldtake.h): tests/check-path-buffers.sh fails
# on an unbounded copy of a field's text, a refusal nobody reads, a call of
# e_add_arguments without its buffer's size, and a second definition of the
# two numbers.
TESTS += tests/check-path-buffers.sh

# One owner of a help or Info token taken into fixed storage (we_helpbuf.h)
# and of a buffer row's bytes (we_buffer.c): tests/check-help-buffers.sh
# fails on an unbounded copy into a char array, a line grown by shifting its
# tail, and a row written with strcpy (tests/help-buffers.allow holds the
# sites of other readers found when the scan was first run, each with its
# reason).
TESTS += tests/check-help-buffers.sh

# One owner of a number the user or a file gives the editor (we_number.c):
# tests/check-number-fields.sh fails on a second parser of an option's
# digits, a write of the five editor numbers outside the owner, a field
# without a type, a guard of a site against a value the owner already keeps
# in range, and a refusal nobody reads; what the file reader still parses by
# itself is counted in tests/number-fields.allow, and the count may only fall.
TESTS += tests/check-number-fields.sh

# One owner of the selection block mode's replacement (e_selection_run), of
# the pending auto-indent mark (the buffer owner) and of the final line break
# Save writes: tests/check-selection-mode.sh fails when a site writes the
# mark, passes the replacement's fault point or reads the selection bit by
# itself, or when e_write stops asking e_write_keeps_file_ending.  The block
# Ctrl-K B leaves open (the window's block_open) has one owner too: only
# we_buffer.c sets or clears it.
TESTS += tests/check-selection-mode.sh

# One owner of the name of what a build makes from a source (we_progname.h):
# tests/check-program-name.sh counts, per function, the cuts at a dot, the
# output suffixes in a string and the joins of a name derived on the spot;
# the functions still to move are in tests/program-name.allow, and the
# counts may only fall.
TESTS += tests/check-program-name.sh

# The editor never takes a terminal as its own: tests/check-tty-open.sh
# fails on an open() of a tty or pty name without O_NOCTTY.
TESTS += tests/check-tty-open.sh

# One owner of the selected entry of a list window (we_listsel.h):
# tests/check-list-entry.sh counts, per function, the indexings of a list's
# names by the highlighted row outside the owner; the file manager's panels
# still doing it are in tests/list-entry.allow, and the counts may only fall.
TESTS += tests/check-list-entry.sh

# One layout path for every box: the layout owner (we_layout.c).
# tests/check-box-layout.sh fails when a box's place is computed outside
# the owner, a display lays the windows out on its own, or a background is
# dropped by hand; what the boxes still to move do is counted in
# tests/box-layout.allow, and the counts may only fall.
TESTS += tests/check-box-layout.sh

# One owner of what a key does: the key table (we_bindings.c).  No other
# file decides an action from a key code or names a key to the user:
# tests/check-keymap.sh scans every we_*.c for key comparisons and key
# names and fails on any that is not in tests/check-keymap.allow, with its
# reason; an allowed line that no longer matches fails too.  And the key
# reference in docs/chapters is the table's own output.
TESTS += tests/check-keymap.sh tests/check-keymap-docs.sh

# One owner of what a menu shows: the menu table (we_menus.c).  The items are
# built nowhere else, the dropdown engine (we_menue.c) is blind to the key
# style, block mode and editor mode, and the bar is laid out in one place:
# tests/check-menus.sh fails on a site that does otherwise.
TESTS += tests/check-menus.sh
AM_TESTS_ENVIRONMENT = NM='$(NM)'; export NM; \
  KEYMAP_TEXI='$(top_builddir)/tools/keymap-texi$(EXEEXT)'; export KEYMAP_TEXI; \
  ONE_OWNER_OBJECTS='$(we_OBJECTS)'; export ONE_OWNER_OBJECTS;

# --- AI assistant unit tests (only when --enable-ai) ---
# WPE_AI comes from config.h (the build is configured --enable-ai here).  The
# framer test links AITLS_LIBS so it still links when the build also enabled TLS
# (the TLS code is then compiled in, though these tests exercise only the plain
# framer/diff); AITLS_LIBS is empty when --enable-ai-tls was not given.  The
# HTTP test also drives a GET on the editor's loop (we_fdloop.c) against a
# local socket; we_ai_http_sync.c, the waiting calls a test needs, is linked
# into the tests only, never into the editor.
if WPE_AI
check_PROGRAMS += test_ai_http test_ai_diff test_ai_path test_ai_scope
TESTS += test_ai_http test_ai_diff test_ai_path test_ai_scope
test_ai_path_SOURCES = tests/test_ai_path.c we_ai_path.c
test_ai_path_CFLAGS = $(XWPE_STDFLAG)
test_ai_scope_SOURCES = tests/test_ai_scope.c we_ai_path.c
test_ai_scope_CFLAGS = $(XWPE_STDFLAG)
test_ai_http_SOURCES = tests/test_ai_http.c we_ai_http.c we_ai_http_sync.c we_fdloop.c
test_ai_http_CFLAGS = $(XWPE_STDFLAG) $(AITLS_CFLAGS)
test_ai_http_LDADD = $(AITLS_LIBS)
test_ai_diff_SOURCES = tests/test_ai_diff.c we_ai_diff.c
test_ai_diff_CFLAGS = $(XWPE_STDFLAG)
# Live TLS smoke: built with the TLS backend but kept out of TESTS (it needs the
# network); run by hand, e.g. ./test_ai_tls_live https://api.github.com /zen
if WPE_AI_TLS
check_PROGRAMS += test_ai_tls_live
test_ai_tls_live_SOURCES = tests/test_ai_tls_live.c we_ai_http.c we_ai_http_sync.c we_fdloop.c
test_ai_tls_live_CFLAGS = $(XWPE_STDFLAG) $(AITLS_CFLAGS)
test_ai_tls_live_LDADD = $(AITLS_LIBS)
endif
endif

# --- Extra files for distribution ---
EXTRA_DIST = \
  contrib/xwpe-env \
  contrib/setup.sh \
  contrib/bsd-test/README.md \
  contrib/bsd-test/sgr_mouse_probe.c \
  contrib/bsd-test/freebsd/Vagrantfile \
  contrib/bsd-test/freebsd/provision.sh \
  contrib/bsd-test/openbsd/Vagrantfile \
  contrib/bsd-test/openbsd/provision.sh \
  contrib/bsd-test/netbsd/Vagrantfile \
  contrib/bsd-test/netbsd/provision.sh \
  help.xwpe_eng \
  help.key_eng \
  syntax_def \
  xwpe.1_eng \
  xwpe.desktop \
  io.codeberg.mendezr.xwpe.metainfo.xml \
  xwpe.spec \
  icons/xwpe.svg \
  icons/xwpe-small.svg \
  icons/xwpe_icon_data.h \
  xwpe-1.5.31.lsm \
  xwpe-conf.patch \
  README.md \
  CHANGELOG \
  old/README.payne-2006 \
  old/we.help_gr \
  old/we_djgpp.c \
  old/xwpe.1_gr \
  tests/README.md \
  tests/run-tests.sh \
  tests/check-one-owner.sh \
  tests/one-owner.allow \
  tests/check-start-plan.sh \
  tests/start-plan.allow \
  tests/check-shell-quote.sh \
  tests/check-line-ends.sh \
  tests/line-ends.allow \
  tests/test_lineend.c \
  tests/check-write-owner.sh \
  tests/write-owner.allow \
  tests/test_write_owner.c \
  tests/check-debug-command.sh \
  tests/debug-command.allow \
  tests/check-chain-outcome.sh \
  tests/chain-outcome.allow \
  tests/check-path-buffers.sh \
  tests/check-help-buffers.sh \
  tests/help-buffers.allow \
  tests/check-number-fields.sh \
  tests/number-fields.allow \
  tests/test_number_parse.c \
  tests/test_number_fields.py \
  tests/check-selection-mode.sh \
  tests/check-list-entry.sh \
  tests/list-entry.allow \
  tests/test_listsel.c \
  tests/test_helpbuf.c \
  tests/test_errpat.c \
  tests/test_help_buffers.py \
  tests/x11/test_help_buffers.py \
  tests/wayland/test_help_buffers.py \
  tests/test_list_entry.py \
  tests/check-program-name.sh \
  tests/check-tty-open.sh \
  tests/program-name.allow \
  tests/check-box-layout.sh \
  tests/box-layout.allow \
  tests/check-keymap.sh \
  tests/check-keymap.allow \
  tests/check-menus.sh \
  tests/check-menus.allow \
  tests/test_menus.c \
  tests/test_menu_golden.py \
  tests/test_menu_refusals.py \
  tests/test_menu_record.py \
  tests/menus \
  docs/chapters/menus-generated.texi \
  tests/check-keymap-docs.sh \
  tests/test_keymap_docs.py \
  docs/chapters/keys-cua-1.6.txt \
  docs/chapters/keys-borland-1.6.txt \
  docs/chapters/keys-modern-changes.texi \
  docs/chapters/keys-delivery.txt \
  tests/test_keys_session.py \
  tests/test_keys_delivery.py \
  tests/stubs/a68g/a68g \
  tests/stubs/gdb \
  tests/stubs/full-pipe/lldb-dap \
  tests/stubs/jdb/jdb \
  tests/stubs/legacy/legacy-debugger \
  tests/stubs/old-gdb/gdb \
  tests/stubs/pdb/python3 \
  tests/transcripts/gdb-where-library.txt \
  tests/transcripts/gdb-where.txt \
  tests/transcripts/gdb-watch-value.txt \
  tests/transcripts/gdb-watch-error.txt \
  tests/transcripts/pdb-stop.txt \
  tests/transcripts/pdb-watch-value.txt \
  tests/transcripts/pdb-watch-error.txt \
  tests/transcripts/jdb-stop.txt \
  tests/transcripts/jdb-watch-value.txt \
  tests/transcripts/jdb-interleave-8213954.txt \
  tests/transcripts/a68g-stop.txt \
  tests/transcripts/a68g-watch-value.txt \
  tests/transcripts/a68g-watch-error.txt \
  tests/transcripts/legacy-stub-rows.txt \
  tests/transcripts/gdb-mi-break-error.txt \
  tests/transcripts/gdb-mi-break-insert.txt \
  tests/transcripts/gdb-mi-break-multiple.txt \
  tests/transcripts/gdb-mi-break-pending.txt \
  tests/transcripts/gdb-mi-exit.txt \
  tests/transcripts/gdb-mi-exit-octal.txt \
  tests/transcripts/gdb-mi-sigint.txt \
  tests/transcripts/gdb-mi-stack-eval.txt \
  tests/transcripts/gdb-mi-step-console.txt \
  tests/transcripts/gdb-mi-stop-breakpoint.txt \
  tests/test_checkheader.c \
  tests/test_fl_sel_len.c \
  tests/test_syntax_def.c \
  tests/test_clipboard.c \
  tests/test_lsp_buffer.c \
  tests/test_lsp_doctor.c \
  tests/test_lsp_semantic.c \
  tests/test_lsp_truecolor.c \
  tests/test_dap.c \
  tests/test_dap_dlv.c \
  tests/test_dap_gdb.c \
  tests/test_dap_lldb.c \
  tests/test_utf8_border.py \
  tests/test_mouse_sgr_raw.py \
  tests/test_truecolor_console.py \
  tests/test_scrollbar.py \
  tests/test_compile.py \
  tests/test_make_output.py \
  tests/test_build_cwd.py \
  tests/test_dialog_resize.py \
  tests/test_esc_key.py \
  tests/test_maxcol_paste.py \
  tests/test_mouse_tracking.py \
  tests/test_project_windows.py \
  tests/test_project_persist.py \
  tests/test_window_views.py \
  tests/test_window_menu.py \
  tests/test_block.py \
  tests/test_block_asan.py \
  tests/test_clipboard_osc52.py \
  tests/wpe_driver.py \
  tests/test_menu_edit.py \
  tests/test_menu_search.py \
  tests/test_menu_file.py \
  tests/test_menu_system.py \
  tests/test_menu_help.py \
  tests/test_menu_project.py \
  tests/test_menu_debug.py \
  tests/test_menu_status_bar.py \
  tests/test_popup_close_button.py \
  tests/test_scrollbar_glyphs.py \
  tests/test_keys_modified.py \
  tests/test_keymap_terminal.py \
  tests/test_private_modes.py \
  tests/test_terminal_owner.py \
  tests/test_wayland_keysym.py \
  tests/test_chrome_ascii.py \
  tests/test_algol68.py \
  tests/test_algol68_debug.py \
  tests/test_ga68.py \
  tests/test_ga68_debug.py \
  tests/test_go_dap.py \
  tests/test_rust_dap.py \
  tests/test_scala_dap.py \
  tests/test_dap_scala.c \
  tests/test_scala_lsp.py \
  tests/test_scala_lsp_complete.py \
  tests/test_scala_lsp_format.py \
  tests/test_scala_lsp_live.py \
  tests/test_scala_lsp_diag_marks.py \
  tests/test_scala_lsp_definition.py \
  tests/test_lsp_scala.c \
  tests/test_lsp_clangd.c \
  tests/test_clangd_lsp.py \
  tests/test_clangd_blanklines.py \
  tests/test_lsp_python.c \
  tests/test_python_lsp.py \
  tests/test_lsp_gopls.c \
  tests/test_gopls_lsp.py \
  tests/test_lsp_rust.c \
  tests/test_rust_lsp.py \
  tests/test_syntax_algol68.py \
  tests/test_debugger_watchdog.py \
  tests/test_macos_smoke.py \
  tests/wayland/conftest.py \
  tests/wayland/test_menu_edit.py \
  tests/wayland/test_menu_by_mode.py \
  tests/wayland/test_popup_close_button.py \
  tests/wayland/test_clipboard.py \
  tests/wayland/test_block_marking.py \
  tests/wayland/test_menu_search.py \
  tests/wayland/test_menu_file.py \
  tests/wayland/test_menu_window.py \
  tests/wayland/test_menu_system.py \
  tests/wayland/test_menu_help.py \
  tests/wayland/test_keymap.py \
  tests/wayland/test_menu_nav.py \
  tests/wayland/test_editor_options.py \
  tests/x11/conftest.py \
  tests/x11/matchbox-kbdconfig \
  tests/x11/test_editor_options.py \
  tests/x11/test_block_marking.py \
  tests/x11/test_clipboard.py \
  tests/x11/test_menu_edit.py \
  tests/x11/test_menu_by_mode.py \
  tests/x11/test_menu_search.py \
  tests/x11/test_menu_file.py \
  tests/x11/test_menu_system.py \
  tests/x11/test_menu_help.py \
  tests/x11/test_menu_window.py \
  tests/x11/test_menu_project.py \
  tests/x11/test_menu_run.py \
  tests/x11/test_keymap.py \
  tests/x11/test_menu_debug.py \
  tests/test_screen_owner.py \
  tests/x11/test_screen_owner.py \
  tests/wayland/test_screen_owner.py \
  tests/wayland/test_terminal_owner.py \
  tests/x11/test_menu_status_bar.py \
  tests/x11/test_popup_close_button.py \
  tests/x11/test_terminal_owner.py \
  tests/x11/test_terminal_owner_session.py \
  tests/x11/test_user_screen.py \
  tests/x11/test_syntax_algol68.py \
  tests/inputs/README \
  tests/inputs/hello.c \
  tests/inputs/paint.c \
  tests/inputs/big.c \
  tests/inputs/two_errors.c \
  tests/inputs/hello.cpp \
  tests/inputs/hello.f90 \
  tests/inputs/hello.p \
  tests/inputs/hello.pl \
  tests/inputs/hello.py \
  tests/inputs/hello.cob \
  tests/inputs/hello.tex \
  tests/inputs/hello.a68 \
  tests/inputs/hello_ga68.a68 \
  tests/inputs/Hello.java \
  tools/compiletest.c
