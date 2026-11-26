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
#
# XWPE_WARNFLAGS holds the -Wno- flags this compiler knows (configure.ac
# probes each one); XWPE_DECLFLAG makes a call to an undeclared function an
# error on every compiler, old ones included (gcc 7 only warns by default).
# The check programs take TEST_CFLAGS: the same standard, declaration and
# format checks, without the editor's library flags.
TEST_CFLAGS = $(XWPE_STDFLAG) $(XWPE_DECLFLAG) $(XWPE_FORMATFLAGS)

AM_CFLAGS = $(XWPE_STDFLAG) $(XWPE_DECLFLAG) $(XWPE_FORMATFLAGS) $(XWPE_RESULTFLAG) \
  $(NCURSESW_CFLAGS) \
  $(X_CFLAGS) \
  $(XFT_CFLAGS) \
  $(CAIRO_CFLAGS) \
  $(PANGO_CFLAGS) \
  $(VTERM_CFLAGS) \
  $(JSONC_CFLAGS) \
  -DLIBRARY_DIR=\"$(pkglibdir)\" \
  -DXWPE_INFODIR=\"$(infodir)\" \
  $(XWPE_WARNFLAGS)

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
  we_closed.c \
  we_winkind.c \
  we_lineend.c \
  we_write.c \
  we_fileop.c \
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
  we_places.c \
  we_seen.c \
  we_read.c \
  we_number.c \
  we_progname.c \
  we_listsel.c \
  we_helpbuf.c \
  we_fmt.c \
  we_errpat.c \
  we_optval.c \
  we_wind.c \
  we_prog.c \
  we_progn.c \
  we_shtok.c \
  we_brkmatch.c \
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
  we_term.c \
  we_render.c \
  we_fdloop.c \
  we_paint.c \
  we_layout.c \
  we_layout_desk.c \
  we_grid.c \
  we_grid_desk.c \
  we_testdump.c \
  we_trace.c \
  we_features.c \
  we_dap_proto.c \
  we_dap.c \
  we_bsp.c \
  we_lsp.c \
  we_lsp_pos.c

# --- What each optional feature adds, named once ---
# A source is listed under the conditional that builds it, so a build without
# the feature compiles no hollow object, and the same list is told to the test
# scripts (WPE_FEATURE_SOURCES, below) so that "this object is not here" is
# answered by name: not built here, or a blind scan.  The word before each
# list is the configure option's (see `we --version`).
X11_SOURCES     = we_xterm.c WeXterm.c we_xbackbuf.c
CAIRO_SOURCES   = we_render_cairo.c
GPM_SOURCES     = we_gpm.c
VTERM_SOURCES   = we_vterm.c
WAYLAND_SOURCES = we_wayland.c we_render_wayland.c we_wlframe.c
AI_SOURCES      = we_ai.c we_ai_http.c we_ai_ui.c we_ai_diff.c we_ai_ws.c we_ai_ckrec.c we_ai_path.c we_ai_session.c we_contain.c
AI_HOST_SOURCES = we_ai_host.c
AI_TESTS        = test_ai_http test_ai_diff test_ai_path test_ai_scope test_ai_session test_ai_contain test_ai_read_waits test_ai_scan_swap test_ai_ckrec test_ai_tool_step
AI_TLS_CHECK    = test_ai_tls_live

if HAVE_X11
we_SOURCES += $(X11_SOURCES)
endif
if HAVE_CAIRO
we_SOURCES += $(CAIRO_SOURCES)
endif
if HAVE_LIBGPM
we_SOURCES += $(GPM_SOURCES)
endif
if HAVE_VTERM
we_SOURCES += $(VTERM_SOURCES)
endif

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
  $(LIBM) \
  -lz

# --- Native Wayland backend (optional, --with-wayland) ---
# The xdg-shell protocol glue is generated at build time from the system
# wayland-protocols XML by wayland-scanner: the client header must exist before
# any .c that includes it is compiled (BUILT_SOURCES), and the generated .c is a
# nodist source (regenerated locally, never shipped in the dist tarball).
# we_wayland.c / we_render_wayland.c are listed in we_SOURCES under the
# conditional, so automake still distributes them when the backend is off.
if HAVE_WAYLAND
we_SOURCES += $(WAYLAND_SOURCES)
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
we_wayland.$(OBJEXT) we_wlframe.$(OBJEXT) we_render_wayland.$(OBJEXT): \
  xdg-shell-client-protocol.h \
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
we_SOURCES += $(AI_SOURCES)
if WPE_AI_TLS
AM_CFLAGS += $(AITLS_CFLAGS)
we_LDADD += $(AITLS_LIBS)
endif
if WPE_AI_AGENT_HOST
we_SOURCES += $(AI_HOST_SOURCES)
endif
endif

# --- Header files (for dist) ---
noinst_HEADERS = \
  we_features.h \
  Xwpe.h \
  WeString.h \
  WeXterm.h \
  we_xbackbuf.h \
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
  we_platform.h \
  we_child.h \
  we_contain.h \
  we_startcmd.h \
  we_startplan.h \
  we_path.h \
  we_places.h \
  we_ai_session.h \
  we_seen.h \
  we_read.h \
  we_number.h \
  we_progname.h \
  we_listsel.h \
  we_helpbuf.h \
  we_fmt.h \
  we_numtext.h \
  we_errpat.h \
  we_optval.h \
  we_fieldtake.h \
  we_manpat.h \
  we_wayland.h \
  we_wlframe.h \
  we_fdloop.h \
  we_paint.h \
  we_layout.h \
  we_grid.h \
  we_testdump.h \
  we_buffer.h \
  we_closed.h \
  we_lineend.h \
  we_write.h \
  we_fileop.h \
  we_rowfind.h \
  we_rowread.h \
  we_utf8.h \
  we_trace.h \
  we_vterm.h \
  we_dap_proto.h \
  we_dap.h \
  we_bsp.h \
  we_lsp.h \
  we_lsp_pos.h \
  we_ai.h \
  we_ai_ckrec.h \
  we_ai_step.h \
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
check_PROGRAMS = test_brkmatch test_shtok test_lsp_positions test_checkheader test_fl_sel_len test_syntax_def test_fdloop test_clipboard test_lsp_buffer test_lsp_doctor test_lsp_semantic test_lsp_truecolor test_dap test_dap_dlv test_dap_gdb test_dap_lldb test_dap_scala test_bsp_close test_lsp_requests test_lsp_scala test_lsp_clangd test_lsp_python test_lsp_gopls test_lsp_rust test_debprompt test_debpath test_debsess test_debreq test_gdbmi test_menurows test_mousedrain test_termrelease test_child test_childsig test_childfds test_childfds_loop test_childsafe test_keymap test_bindings test_menus test_paint test_splice test_lineend test_write_owner test_fileop edit_driver test_startcmd test_startplan test_path test_path_say test_progname test_fieldtake test_layout test_rowfind test_rowfind_copy test_rowfind_nocontext test_rowfind_macsim test_rowfind_reject test_rowfind_illseq test_rowfind_badengine test_rowfind_badengine_copy test_rowfind_boundary test_rowfind_boundary_copy test_rowfind_nolocale test_rowfind_illseq_copy test_listsel test_helpbuf test_number_parse test_errpat test_optval test_fmt test_numtext test_winkind test_winkind_open

# The owner of a formatted text written into storage (we_fmt.c): a number is
# shown in full or refused, a command or a path is never written cut, a
# sentence that does not fit is cut at a character and ends in "...", a
# request built in blocks never carries a mark.  Every storage has guard
# bytes after it, so a byte written past the size fails in a plain build.
# The heap limit is lowered so its refusal can be reached.
test_fmt_SOURCES = tests/test_fmt.c we_fmt.c
test_fmt_CFLAGS = $(TEST_CFLAGS) -DWPE_FMT_HEAP_LIMIT=1000

# The line:column of a window's frame and the counts of a replace
# (we_numtext.h): shown in full at any size, or refused, never written past
# the caller's storage.
test_numtext_SOURCES = tests/test_numtext.c we_fmt.c
test_numtext_CFLAGS = $(TEST_CFLAGS)

# The owner of the selected entry of a list window (we_listsel.c): a
# selection names an entry or nothing, the blank row that ends a project
# list is never an entry, Delete cannot remove it, and the directory tree
# names no subdirectory for an ancestor row.
test_listsel_SOURCES = tests/test_listsel.c we_listsel.c
test_listsel_CFLAGS = $(TEST_CFLAGS)

# The owner of "which kind of window is this, and what does it hold"
# (we_winkind.c): each kind answers only for its own payload, a window the
# editor opened itself is known by the intent that opened it and not by its
# title, a command asked of a window without text is refused naming the
# command, and a close leaves nothing behind.
test_winkind_SOURCES = tests/test_winkind.c we_winkind.c tests/stub_write_window.c
test_winkind_CFLAGS = $(TEST_CFLAGS)

# The one finder of windows (we_winkind.c, e_window_find): for every kind of
# window and every desk of the table, a request for the editor's own window is
# never answered by a file of the user's, and a request for a file never by one
# of the editor's own windows, the clipboard or a list; a label is kept whole.
test_winkind_open_SOURCES = tests/test_winkind_open.c we_winkind.c tests/stub_write_window.c
test_winkind_open_CFLAGS = $(TEST_CFLAGS)

# The owner of a help or Info token taken into storage (we_helpbuf.c): a
# token is written whole or cut at a character, never past its buffer, and
# a mark that does not fit is refused.
test_helpbuf_SOURCES = tests/test_helpbuf.c we_helpbuf.c
test_helpbuf_CFLAGS = $(TEST_CFLAGS)

# The token walkers of the definition finder (we_shtok.c): golden text of
# where each of the five stops over blanks, continuations, comments, quoted
# text, nesting and an #else side, so merging them cannot move a definition.
# The unit brings its own line reader; the editor's reads a window as well.
test_shtok_SOURCES = tests/test_shtok.c we_shtok.c
test_shtok_CFLAGS = $(TEST_CFLAGS)

# The bracket matcher (we_brkmatch.c): golden text of where Matching BracKet
# lands over each pair, quoted text, comments, a // tail and an #else side, so
# splitting the matcher into steps cannot move the cursor to another bracket.
test_brkmatch_SOURCES = tests/test_brkmatch.c we_brkmatch.c
test_brkmatch_CFLAGS = $(TEST_CFLAGS)

# The readers of a row (we_rowread.h): what a walk gets for a row the text
# does not have, the byte before a row and the bytes past its NUL, in a
# hostile frame that makes a read outside the text change the answer in a
# plain build too.  The unit needs edit.h and the header, no object.
check_PROGRAMS += test_rowread
test_rowread_SOURCES = tests/test_rowread.c we_rowread.h
test_rowread_CFLAGS = $(TEST_CFLAGS)

# The owner of a compiler's message pattern (we_errpat.c): a pattern is read
# once into rows and warning tests, a text cut inside a token is refused with
# a reason, a surprising reading is noted, and no byte outside the text is
# read, whatever the text.
test_errpat_SOURCES = tests/test_errpat.c we_errpat.c we_fmt.c
test_errpat_CFLAGS = $(TEST_CFLAGS)

# The owner of an option value's stored form (we_optval.c): a value reads
# back byte for byte, whatever it holds, and no byte outside the value or
# the buffer is touched.
test_optval_SOURCES = tests/test_optval.c we_optval.c
test_optval_CFLAGS = $(TEST_CFLAGS)

# Names the signals it ignores; the pyte rows run it as a program the editor
# started.  Built by `make check`, not run by it.
check_PROGRAMS += sigstate
sigstate_SOURCES = tests/sigstate.c
sigstate_CFLAGS = $(TEST_CFLAGS)

# The owner of how long a path the editor holds may be (we_path.c): copy,
# join and cat that refuse instead of cutting, at the boundary of a buffer.
test_path_SOURCES = tests/test_path.c we_path.c we_fmt.c
test_path_CFLAGS = $(TEST_CFLAGS)

# The sentence a refused path gets (we_path.h), checked against a system whose
# PATH_MAX is 1024 and NAME_MAX 255: the number is the system's, not the
# editor's room.  Its own program, because test_path.c reads the real PATH_MAX.
test_path_say_SOURCES = tests/test_path_say.c we_path.c we_fmt.c
test_path_say_CFLAGS = $(TEST_CFLAGS)

# The owner of the name of what a build makes from a source (we_progname.c):
# the extension and stem of a name, the program, the object and the debug
# binary under Exedir through the start plan's operand, a result that does
# not fit refused whole, and the project's two names.
test_progname_SOURCES = tests/test_progname.c we_progname.c we_path.c we_places.c we_read.c we_seen.c we_startplan.c we_startcmd.c we_child.c we_fdloop.c we_debreq.c we_fmt.c
test_progname_CFLAGS = $(TEST_CFLAGS)
test_progname_LDADD = $(UTIL_LIBS)

# The owner of a number the user or a file gives the editor (we_number.c):
# the parser reads a text whole and refuses overflow and trailing text, the
# range of each kind, and the field's accept (saturating Go to Line).  Links
# the editor's string helpers only; the status row is a stub of the test.
test_number_parse_SOURCES = tests/test_number_parse.c we_number.c WeString.c we_fmt.c
test_number_parse_CFLAGS = $(AM_CFLAGS)

# The owner of the copy out of a dialog field (we_fieldtake.h): a text that
# does not fit its destination is refused, never written past it.
test_fieldtake_SOURCES = tests/test_fieldtake.c
test_fieldtake_CFLAGS = $(TEST_CFLAGS)

# The pattern Function Index looks for in every MANPATH folder
# (we_manpat.h): built through the path owner, whole or refused.
check_PROGRAMS += test_manpat
test_manpat_SOURCES = tests/test_manpat.c we_path.c we_fmt.c
test_manpat_CFLAGS = $(TEST_CFLAGS)

# The owner of the start plan: we_startplan.c with the store (we_startcmd.c)
# and the children's owner it starts through.  The operand rule on every
# role, a folder value refused as an option, links followed out of the
# folder, a removal outside dropped and a write outside asked, the home
# folder never a root, a plan equal in effect to the user's own, one
# record per plan, the cover check of a step, and mutants of the rules.
test_startplan_SOURCES = tests/test_startplan.c we_places.c we_path.c we_read.c we_seen.c we_startplan.c we_startcmd.c we_child.c we_fdloop.c we_debreq.c we_fmt.c
test_startplan_CFLAGS = $(TEST_CFLAGS)
# The rows that make an allocation fail take over malloc and realloc, which
# needs a linker with --wrap.  Without one (Apple's ld) the test is built
# without them and reports those rows as SKIP.
if HAVE_LD_WRAP
test_startplan_CFLAGS += -DWPE_TEST_LD_WRAP=1
test_startplan_LDFLAGS = -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=realpath -Wl,--wrap=lstat
endif
test_startplan_LDADD = $(UTIL_LIBS)

# The owner of the question about a folder's commands: we_startcmd.c alone.
# The store's round trip (escapes, a changed command, one record per folder
# and key, the cap), every way a store fails closed (missing, another
# version, a bad line, a link), folders through a link, the user's own and
# the built-in texts, and the printable form of a command.
test_startcmd_SOURCES = tests/test_startcmd.c we_startcmd.c we_places.c we_path.c we_read.c we_seen.c we_fmt.c
test_startcmd_CFLAGS = $(TEST_CFLAGS)

# The text's owner: we_buffer.c alone (the editor's side of its seam is
# stubbed by the test).  Marks and breakpoints move with their lines, Undo
# and Redo give back exactly the text in one key per action, a refused
# allocation changes nothing, a view keeps no undo.
test_splice_SOURCES = tests/test_splice.c we_buffer.c we_winkind.c tests/stub_write_window.c
test_splice_CFLAGS = $(TEST_CFLAGS)

# The owner of a file's line endings: we_lineend.c on the buffer's owner.
# A file's bytes become rows and rows become bytes, in both styles, and
# what the buffer made goes back into it as it was.
test_lineend_SOURCES = tests/test_lineend.c we_buffer.c we_lineend.c we_winkind.c tests/stub_write_window.c
test_lineend_CFLAGS = $(TEST_CFLAGS)

# The owner of writing the user's text (we_write.c), the editor's side
# played by stubs: the one sentence a failed write says, the backup copy
# (private, counted as made only when whole), the restore after a write
# that fails half way (by rename, through a link, with a hard link, one that
# fails), the size limit as an outcome and SIGXFSZ put back.
test_write_owner_SOURCES = tests/test_write_owner.c tests/seen_rows.h tests/testdir.h we_write.c we_places.c we_path.c we_seen.c we_fmt.c we_winkind.c
test_write_owner_CFLAGS = $(TEST_CFLAGS)
test_write_owner_LDFLAGS = $(TEST_EXPORT_DYNAMIC)

# The owner of copying, moving, linking and removing the user's files
# (we_fileop.c) over the write owner's bytes, the editor's side played by
# stubs: the atom (a copy is new, private, whole or absent, and one rename
# puts it in place), the name that is not replaced, the sentence per step,
# the wastebasket.
test_fileop_SOURCES = tests/test_fileop.c we_fileop.c we_write.c we_path.c we_places.c we_read.c we_seen.c we_fmt.c we_winkind.c
test_fileop_CFLAGS = $(TEST_CFLAGS)
test_fileop_LDFLAGS = $(TEST_EXPORT_DYNAMIC)

# The one place a C test makes its folder and states what it assumes about
# the user running it (tests/testdir.h): the folder is in a group of the
# user's, a mode is read back, a premise that does not hold is a SKIP by name
# or, in an unprivileged run, a counted FAIL.  The refusals of a BSD are made
# on any system by the fake chown and fchmod (tests/fakechmod.h).
# tests/check-mode-premise.sh fails a test that sets a special-bit mode
# without it.
check_PROGRAMS += test_testdir
test_testdir_SOURCES = tests/test_testdir.c tests/testdir.h tests/fakechmod.h
test_testdir_CFLAGS = $(TEST_CFLAGS)
test_testdir_LDFLAGS = $(TEST_EXPORT_DYNAMIC)

# The one resolver of "the C library's function behind a test's hook"
# (tests/interpose.h): a hook is found under the name the compiler bound it
# to, so a system that renames a function (NetBSD's fstat is __fstat50) cannot
# send the hook back to itself.  The program hooks fstat, calls it once and
# proves the library's answer came back, and that a hook the library's
# function leads back into ends the program with its name.  Every hooking
# program links with TEST_EXPORT_DYNAMIC, which configure measures.
# tests/check-interpose.sh fails a test that looks a function up by its C
# spelling anywhere else.
check_PROGRAMS += test_interpose
test_interpose_SOURCES = tests/test_interpose.c tests/interpose.h tests/linebuf.h
test_interpose_CFLAGS = $(TEST_CFLAGS)
test_interpose_LDFLAGS = $(TEST_EXPORT_DYNAMIC)

# The owner of "is this the object I saw" (we_seen.c): the judge's table
# built by hand, the held folder, and the rows that need a file system that
# gives an inode number back.  Those rows run on a disk file system (the
# build tree, or XWPE_TEST_REUSE_DIR) and print which one; on one that never
# gives a number back they say SKIP and why.
check_PROGRAMS += test_seen
test_seen_SOURCES = tests/test_seen.c tests/seen_rows.h tests/testdir.h we_seen.c we_fmt.c
test_seen_CFLAGS = $(TEST_CFLAGS)
test_seen_LDFLAGS = $(TEST_EXPORT_DYNAMIC)

# The owner of "a file is read by a name the editor did not choose"
# (we_read.c): every kind of object under a name, with the link policy, the
# four faces and the three bounds, against a reference function that says
# what each cell must give, plus the identity, the memory stream and the
# folder face.  The open is hooked, so a row can say a device was never
# opened.
check_PROGRAMS += test_read_owner
test_read_owner_SOURCES = tests/test_read_owner.c tests/testdir.h we_read.c we_seen.c we_path.c we_fmt.c
test_read_owner_CFLAGS = $(TEST_CFLAGS)
test_read_owner_LDFLAGS = $(TEST_EXPORT_DYNAMIC)

# The same owner behind a line protocol, for the edit model's walks
# (tests/edit_model/cdriver.py).  Built by `make check`, not run by it.
edit_driver_SOURCES = tests/edit_driver.c we_buffer.c we_winkind.c tests/stub_write_window.c
edit_driver_CFLAGS = $(TEST_CFLAGS)

# The screen's owner: we_paint.c with the fd loop only (the desk is given
# through hooks the test sets).  A paint asked for under a box waits, the
# same paint is kept once, the queue is bounded and says what it refused,
# paints run in the order asked, one that opens a box leaves the rest for
# later, a box inside a box keeps the screen, and nothing runs on leave.
test_paint_SOURCES = tests/test_paint.c we_paint.c we_fdloop.c
test_paint_CFLAGS = $(TEST_CFLAGS)

# The box layout owner: we_layout.c with the screen's owner and the fd loop
# only (the desk is given through hooks the test sets).  Where each rule puts
# a box at the displays' sizes, the stack the walk lays out outermost first,
# and the stamp that keeps a background saved at another size from being put
# back.
test_layout_SOURCES = tests/test_layout.c we_layout.c we_paint.c we_fdloop.c
test_layout_CFLAGS = $(TEST_CFLAGS)

# The owner of a search over a row: we_rowfind.c alone.  A differential
# against the engine run by hand on every start of a row, the invariants of
# every hit, the progress of Search Again and Change All on rows wrapped at
# a chosen width, and the speed bounds.  Built twice: the second build forces
# the copy path that a C library without REG_STARTEND needs, where the engine
# cannot see the byte before a call's start and the owner hands it that
# character itself, so no row may differ from the reference.
test_rowfind_SOURCES = tests/test_rowfind.c we_rowfind.c
test_rowfind_CFLAGS = $(TEST_CFLAGS)
test_rowfind_copy_SOURCES = tests/test_rowfind.c we_rowfind.c
test_rowfind_copy_CFLAGS = $(TEST_CFLAGS)
test_rowfind_copy_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_ROWFIND_NO_STARTEND
# The nocontext build keeps REG_STARTEND and puts a mock engine under the owner
# that takes the start of a call for the beginning of the string, as macOS's
# regexec does, with the owner's decision forced to "no start context".  The
# reference asks the C library's own engine through the mock's hook.  Again
# no row may differ, and a group the engine reports wrongly is no hit.
test_rowfind_nocontext_SOURCES = tests/test_rowfind.c we_rowfind.c tests/mock_nocontext.c tests/mock_nocontext.h
test_rowfind_nocontext_CFLAGS = $(TEST_CFLAGS)
test_rowfind_nocontext_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_ROWFIND_MOCK_NOCONTEXT -DWPE_ROWFIND_NO_START_CONTEXT -include $(srcdir)/tests/mock_nocontext.h
# The macsim build is the nocontext build with the reference on the mock engine too, as
# on macOS, where the reference is the same engine that loses the text before
# a start: a row with a word-boundary escape that the reference then cannot
# answer is counted as unverifiable and printed.  The real oracle stays the
# third build (its reference is the C library's own engine); this one runs the
# macOS path of the test on Linux.
test_rowfind_macsim_SOURCES = tests/test_rowfind.c we_rowfind.c tests/mock_nocontext.c tests/mock_nocontext.h
test_rowfind_macsim_CFLAGS = $(TEST_CFLAGS)
test_rowfind_macsim_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_ROWFIND_MOCK_MACSIM -DWPE_ROWFIND_NO_START_CONTEXT -include $(srcdir)/tests/mock_nocontext.h
# The third build has no UTF-8 ctype to make (OpenBSD, a minimal system): the
# engine then reads bytes under a process locale that is not UTF-8, and the
# owner rejects a match that covers part of a character instead of widening it.
test_rowfind_reject_SOURCES = tests/test_rowfind.c we_rowfind.c
test_rowfind_reject_CFLAGS = $(TEST_CFLAGS)
test_rowfind_reject_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_ROWFIND_NO_UTF8_CTYPE
# The fourth build puts a mock engine under the owner and the reference that
# refuses invalid UTF-8 as FreeBSD's and macOS's do (REG_ILLSEQ), so the
# owner's two fallbacks to bytes run on Linux too, where glibc never refuses.
test_rowfind_illseq_SOURCES = tests/test_rowfind.c we_rowfind.c tests/mock_illseq.c tests/mock_illseq.h
test_rowfind_illseq_CFLAGS = $(TEST_CFLAGS)
# It also forces "no start context": the character the owner hands the engine
# before a start (a stand-in or a letter) must be valid UTF-8 too.
test_rowfind_illseq_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_ROWFIND_MOCK_ILLSEQ -DWPE_ROWFIND_NO_START_CONTEXT -include $(srcdir)/tests/mock_illseq.h
# The fifth build puts a mock engine under the owner that crashes on a
# word-boundary escape beside an alternation over a multibyte row (NetBSD
# 10's regexec does) and answers matches outside the row, with the owner's
# refusal forced on: the refusal must keep the crash out of reach, and no
# answer outside the row may become a hit or a search that never ends.
test_rowfind_badengine_SOURCES = tests/test_rowfind.c we_rowfind.c tests/mock_badengine.c tests/mock_badengine.h
test_rowfind_badengine_CFLAGS = $(TEST_CFLAGS)
test_rowfind_badengine_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_ROWFIND_MOCK_BADENGINE -DWPE_ROWFIND_UNSAFE_BOUNDARY_GROUP -include $(srcdir)/tests/mock_badengine.h
# The sixth build is the fifth on the copy path (no REG_STARTEND), where the
# check of the engine's answer is a second piece of code with its own rows.
test_rowfind_badengine_copy_SOURCES = tests/test_rowfind.c we_rowfind.c tests/mock_badengine.c tests/mock_badengine.h
test_rowfind_badengine_copy_CFLAGS = $(TEST_CFLAGS)
test_rowfind_badengine_copy_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_ROWFIND_MOCK_BADENGINE -DWPE_ROWFIND_NO_STARTEND -DWPE_ROWFIND_UNSAFE_BOUNDARY_GROUP -include $(srcdir)/tests/mock_badengine.h
# The seventh build forces the refusal of every word-boundary escape, as a
# system whose probe saw a control fail (FreeBSD 14's walk() crashes on \>. and
# misjudges \<b, with no alternation) gets it, on the real engine.  It is a
# build of its own, not part of the fifth, so that the fifth keeps running
# an escape with no bar through the engine and the group-only path.
test_rowfind_boundary_SOURCES = tests/test_rowfind.c we_rowfind.c
test_rowfind_boundary_CFLAGS = $(TEST_CFLAGS)
test_rowfind_boundary_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_ROWFIND_UNSAFE_BOUNDARY
# The eighth build is the seventh on the copy path.  With every word-boundary
# escape refused nothing reaches the engine that reads the byte before a
# call, so the copy path and the REG_STARTEND path are the same and no
# context expression is ever compiled: the case of a system (FreeBSD, NetBSD)
# whose engine is measured unsafe, run here on one whose engine is not.
test_rowfind_boundary_copy_SOURCES = tests/test_rowfind.c we_rowfind.c
test_rowfind_boundary_copy_CFLAGS = $(TEST_CFLAGS)
test_rowfind_boundary_copy_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_ROWFIND_NO_STARTEND -DWPE_ROWFIND_UNSAFE_BOUNDARY
# The ninth build is the fourth on the configuration of a system with no
# uselocale (NetBSD): the owner has no ctype of its own to run the engine
# under, so a row or a Find text that holds a byte which is not UTF-8 must
# still reach the engine as characters, which only the owner can make of it.
test_rowfind_nolocale_SOURCES = tests/test_rowfind.c we_rowfind.c tests/mock_illseq.c tests/mock_illseq.h
test_rowfind_nolocale_CFLAGS = $(TEST_CFLAGS)
# It forces "no start context" as well, as the fourth does.
test_rowfind_nolocale_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_ROWFIND_MOCK_ILLSEQ -DWPE_ROWFIND_NO_LOCALES -DWPE_ROWFIND_NO_START_CONTEXT -include $(srcdir)/tests/mock_illseq.h
# The tenth build is the fourth on the copy path (no REG_STARTEND): the copy
# the engine reads is a second piece of code that could hand it a byte which
# is not UTF-8, and only a build with the strict mock under it proves it does not.
test_rowfind_illseq_copy_SOURCES = tests/test_rowfind.c we_rowfind.c tests/mock_illseq.c tests/mock_illseq.h
test_rowfind_illseq_copy_CFLAGS = $(TEST_CFLAGS)
test_rowfind_illseq_copy_CPPFLAGS = $(AM_CPPFLAGS) -DWPE_ROWFIND_MOCK_ILLSEQ -DWPE_ROWFIND_NO_STARTEND -include $(srcdir)/tests/mock_illseq.h

# The key decoder's table: we_keymap.c alone, no editor and no display
# library.  Every bound pair has one row, the curses, keysym and escape
# sequence forms of a key give the same code, a chord the Debug menu shows is
# decoded, and an unbound key gives 0 and never a letter.
test_keymap_SOURCES = tests/test_keymap.c we_keymap.c we_bindings.c
test_keymap_CFLAGS = $(TEST_CFLAGS)

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
test_child_SOURCES = tests/test_child.c we_child.c we_fdloop.c we_fmt.c
test_child_CFLAGS = $(TEST_CFLAGS) -DWPE_CHILD_MECHANICS_TEST
test_child_LDFLAGS = $(TEST_EXPORT_DYNAMIC)
test_child_LDADD = $(UTIL_LIBS)

# A read that waits survives the editor's SIGCHLD: the handler is installed
# by the editor's own function, children end during a blocking read.  Also
# the short commands' cases live in test_child.
test_childsig_SOURCES = tests/test_childsig.c we_child.c we_fdloop.c we_fmt.c
test_childsig_CFLAGS = $(TEST_CFLAGS)
test_childsig_LDADD = $(UTIL_LIBS)

# Which descriptors a program the editor starts holds: the owner's close step
# (we_child.c) leaves it 0, 1 and 2 and what its spec names, whatever the
# editor inherited from what started it.  Built twice, as test_rowfind is:
# the second build takes the descriptor-by-descriptor loop that macOS runs, on
# any system, so Linux proves the loop and its ceiling, and it prints what the
# close step cost.
test_childfds_SOURCES = tests/test_childfds.c we_child.c we_fdloop.c we_fmt.c
test_childfds_CFLAGS = $(TEST_CFLAGS) -DWPE_CHILD_MECHANICS_TEST
test_childfds_LDADD = $(UTIL_LIBS)
test_childfds_loop_SOURCES = tests/test_childfds.c we_child.c we_fdloop.c we_fmt.c
test_childfds_loop_CFLAGS = $(TEST_CFLAGS) -DWPE_CHILD_MECHANICS_TEST -DWPE_CHILD_CLOSE_LOOP_ONLY
test_childfds_loop_LDADD = $(UTIL_LIBS)

# What the child of the editor may do between fork and exec: the graphical
# editor runs a library thread, so the child formats and allocates nothing.
# The rows that stage a lock held by another thread put the linker's --wrap
# around the calls a child must not make (stdio, the formatting calls,
# strerror) and around chdir, which a row uses to raise a signal inside the
# child.  Without a linker that has --wrap (Apple's ld) the test is built
# without those rows and reports them as SKIP.
test_childsafe_SOURCES = tests/test_childsafe.c we_child.c we_fdloop.c we_fmt.c
test_childsafe_CFLAGS = $(TEST_CFLAGS) -DWPE_CHILD_MECHANICS_TEST
test_childsafe_LDADD = $(UTIL_LIBS) $(PTHREAD_LIBS)
if HAVE_LD_WRAP
test_childsafe_CFLAGS += -DWPE_TEST_LD_WRAP=1
test_childsafe_LDFLAGS = -Wl,--wrap=fprintf -Wl,--wrap=__fprintf_chk -Wl,--wrap=fputc \
  -Wl,--wrap=fputs -Wl,--wrap=fwrite -Wl,--wrap=fflush -Wl,--wrap=putc \
  -Wl,--wrap=snprintf -Wl,--wrap=__snprintf_chk -Wl,--wrap=vsnprintf \
  -Wl,--wrap=__vsnprintf_chk -Wl,--wrap=strerror -Wl,--wrap=strerror_r \
  -Wl,--wrap=__xpg_strerror_r -Wl,--wrap=chdir
endif

# The hang guard every C test that starts a child arms (tests/test_watchdog.h),
# tested: an alarm names what did not finish, XWPE_TEST_WAIT_SCALE (a float)
# stretches it, earlier output survives it, and the two waits it offers
# (test_wait_child, test_wait_gone) bound themselves.  The runner that ends a
# whole test after SECS is tests/with-deadline.sh, tested by
# tests/check-with-deadline.sh.
check_PROGRAMS += test_watchdog
test_watchdog_SOURCES = tests/test_watchdog.c tests/test_watchdog.h tests/child_state.h
test_watchdog_CFLAGS = $(TEST_CFLAGS)

# The one reader of "is this process alive" for the C tests
# (tests/child_state.h): the process table of each system (Linux /proc,
# FreeBSD KERN_PROC, NetBSD KERN_PROC2, OpenBSD KERN_PROC), never
# kill(pid, 0), which finds a zombie.  Its canary makes a child run, end
# without being waited for, and be waited for, and the reader must say
# running, zombie, gone on every system; tests/check-child-state.sh fails a
# test that asks kill or killpg with the signal 0 outside the owners.
check_PROGRAMS += test_child_state
test_child_state_SOURCES = tests/test_child_state.c tests/child_state.h tests/test_watchdog.h
test_child_state_CFLAGS = $(TEST_CFLAGS)

# The test includes we_fdloop.c itself (the id counter is private to it), so
# the file is not listed here again: it would be defined twice.
test_fdloop_SOURCES = tests/test_fdloop.c tests/test_loop_until.h
test_fdloop_CFLAGS = $(TEST_CFLAGS)

# Debugger prompt window unit test: we_debprompt.c only, no editor, always runs.
# This is the test F95 should have had -- a prompt split across two reads, and
# the 7-byte a68g prompt driven at every split length, which is what wrote two
# bytes past a 12-byte scratch in e_d_line_read.  Worth building under asan as
# well: the overflow is only visible there.
test_debprompt_SOURCES = tests/test_debprompt.c we_debprompt.c
test_debprompt_CFLAGS = $(TEST_CFLAGS)

# Debugger start path unit test: we_debpath.c only, no editor, always runs.
# A source path too long to hand to a debugger is refused whole, never cut;
# no file system lets the editor be driven into that case, so it is checked
# here at the exact buffer edge.
test_debpath_SOURCES = tests/test_debpath.c we_debpath.c we_fmt.c
test_debpath_CFLAGS = $(TEST_CFLAGS)

# Debugger session state unit test: we_debsess.c only, no editor, no
# debugger, always runs.  What gdb, pdb, jdb and a68g print (transcripts in
# tests/transcripts/) is read into the session as the editor reads it: watch
# values and errors, the frame a stop marks, pdb's and a68g's stop lines,
# and jdb's set answer written into the middle of a breakpoint hit.
test_debsess_SOURCES = tests/test_debsess.c we_debsess.c we_gdbmi.c we_fmt.c
test_debsess_CFLAGS = $(TEST_CFLAGS)

# gdb/MI parser unit test: we_gdbmi.c and we_debsess.c only, no editor, no
# debugger, always runs.  What gdb 18.1 printed in MI mode (transcripts in
# tests/transcripts/gdb-mi-*.txt) is parsed into records, cut into lines
# at every byte position, and taken into the session: the stop's file and
# line, a breakpoint answered or refused with gdb's words, a SIGINT stop
# told from the user's, the exit code, and the console words for Messages.
test_gdbmi_SOURCES = tests/test_gdbmi.c we_gdbmi.c we_debsess.c we_fmt.c
test_gdbmi_CFLAGS = $(TEST_CFLAGS)

# Debugger request engine unit test: we_debreq.c only, no editor, no
# debugger, always runs.  A fake clock and a transcript stand for the world:
# the deadline and the waiting line, requests going out one at a time with
# the queue's bound, cancel ending every request, and a stop line that comes
# in the middle of an answer going to the editor instead of the request.
test_debreq_SOURCES = tests/test_debreq.c we_debreq.c we_fmt.c
test_debreq_CFLAGS = $(TEST_CFLAGS)

# Dropdown highlight unit test: we_menurows.c only, no editor, always runs.
# Wrap at both ends, step over dividers, the open/Home/End rows, the edge
# menus no editor menu has today -- in microseconds, where the pyte suite
# needs an editor and a pseudo-terminal per case.
test_menurows_SOURCES = tests/test_menurows.c we_menurows.c
test_menurows_CFLAGS = $(TEST_CFLAGS)

# Mouse queue drain unit test: we_mousedrain.c only, no curses, always runs.
# A curses whose getmouse() answers "ok, empty event" forever (OpenBSD's) must
# not leave the drain spinning; the test has its own alarm so a hang is a failure.
test_mousedrain_SOURCES = tests/test_mousedrain.c we_mousedrain.c
test_mousedrain_CFLAGS = $(TEST_CFLAGS)

# The string a signal handler writes to give the terminal back: we_termrelease.c
# only, no curses, always runs.
test_termrelease_SOURCES = tests/test_termrelease.c we_termrelease.c
test_termrelease_CFLAGS = $(TEST_CFLAGS)

# Clipboard helpers unit test: RFC 4648 base64 + OSC 52 framing (we_clip.c
# only, no editor/X11). Always runs (no skip).
test_clipboard_SOURCES = tests/test_clipboard.c we_clip.c
test_clipboard_CFLAGS = $(TEST_CFLAGS)

test_checkheader_SOURCES = tests/test_checkheader.c
test_checkheader_CFLAGS = $(TEST_CFLAGS)
test_checkheader_LDFLAGS =

test_fl_sel_len_SOURCES = tests/test_fl_sel_len.c
test_fl_sel_len_CFLAGS = $(TEST_CFLAGS)
test_fl_sel_len_LDFLAGS =

test_syntax_def_SOURCES = tests/test_syntax_def.c
test_syntax_def_CFLAGS = $(TEST_CFLAGS)
test_syntax_def_LDFLAGS =

# LSP buffer-serialization unit test: the pure e_lsp_join_lines invariant (one
# '\n' per editor line, WPE_WR not doubled).  Links we_lsp.c only; no server,
# always runs (no skip).
test_lsp_buffer_SOURCES = tests/test_lsp_buffer.c we_places.c we_path.c we_read.c we_seen.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_lsp_buffer_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_lsp_buffer_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP Doctor dedup unit test: the pure e_lsp_doc_is_new gate (re-pushed Doctor
# rendered only when changed; reset per session shows it again).  we_lsp.c only.
test_lsp_doctor_SOURCES = tests/test_lsp_doctor.c we_places.c we_path.c we_read.c we_seen.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_lsp_doctor_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_lsp_doctor_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# The owner of the language server's columns (we_lsp_pos.c) alone: the units
# of every glyph shape in both encodings, the two conversions and their
# clamps, the rows of a text, the offer and the answer, and the decoder.
test_lsp_positions_SOURCES = tests/test_lsp_positions.c we_lsp_pos.c
test_lsp_positions_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_lsp_positions_LDADD = $(JSONC_LIBS)

# LSP semantic-tokens decode unit test: the pure delta-decode (we_lsp_pos.c).
test_lsp_semantic_SOURCES = tests/test_lsp_semantic.c we_places.c we_path.c we_read.c we_seen.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_lsp_semantic_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_lsp_semantic_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP requests answered on the loop: each answer reaches its own request,
# the server's own requests are answered meanwhile, Esc, the deadline, the
# server's end and the close.  The test plays the server itself.
test_lsp_requests_SOURCES = tests/test_lsp_requests.c we_places.c we_path.c we_read.c we_seen.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_lsp_requests_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_lsp_requests_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# Semantic-token truecolor palette unit test: pure token->RGB-slot lookup.
test_lsp_truecolor_SOURCES = tests/test_lsp_truecolor.c we_places.c we_path.c we_read.c we_seen.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_lsp_truecolor_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_lsp_truecolor_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# DAP transport and start-machine unit test: the engine + json-c, no xwpe,
# no adapter (the test plays one on a loopback socket).  we_dap_sync.c, the
# engine's calls that wait, is linked into these tests only: the editor
# follows the engine from its loop and never waits on an adapter.  An
# adapter starts through its start plan (we_startplan.c, with its store
# we_startcmd.c), in these tests too.
test_dap_SOURCES = tests/test_dap.c we_places.c we_path.c we_read.c we_seen.c we_dap.c we_dap_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_dap_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_dap_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# DAP engine integration test: drives a real `dlv dap` (SKIPs without dlv+go).
test_dap_dlv_SOURCES = tests/test_dap_dlv.c we_places.c we_path.c we_read.c we_seen.c we_dap.c we_dap_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_dap_dlv_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_dap_dlv_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# DAP stdio transport integration test: drives a real `gdb --interpreter=dap`
# on a rustc-built binary (the Rust slice).  SKIPs without gdb+rustc.
test_dap_gdb_SOURCES = tests/test_dap_gdb.c we_places.c we_path.c we_read.c we_seen.c we_dap.c we_dap_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_dap_gdb_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_dap_gdb_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# Same stdio transport against `lldb-dap` (the LLVM adapter; native on macOS).
# SKIPs without lldb-dap+rustc.
test_dap_lldb_SOURCES = tests/test_dap_lldb.c we_places.c we_path.c we_read.c we_seen.c we_dap.c we_dap_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_dap_lldb_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_dap_lldb_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# JVM/Scala path: BSP bootstrap (we_bsp.c) + DAP TCP-client transport against a
# real scala-cli/Bloop.  SKIPs without scala-cli; slow on a cold coursier cache.
test_dap_scala_SOURCES = tests/test_dap_scala.c we_places.c we_path.c we_read.c we_seen.c we_bsp.c we_bsp_sync.c we_dap.c we_dap_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_dap_scala_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_dap_scala_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# How the build server's launcher is ended, against a fake launcher (the test
# program itself); no scala-cli needed.  The close returns at once and the
# children's owner (we_child.c) ends the launcher on the fd loop, with no
# exit descriptor, the same on every system.
test_bsp_close_SOURCES = tests/test_bsp_close.c we_places.c we_path.c we_read.c we_seen.c we_bsp.c we_bsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_bsp_close_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_bsp_close_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP engine: drives a real Metals language server on a single Scala file
# (hover/definition/completion/diagnostics).  SKIPs without metals + scala-cli.
test_lsp_scala_SOURCES = tests/test_lsp_scala.c we_places.c we_path.c we_read.c we_seen.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_lsp_scala_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_lsp_scala_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP engine, breadth: drives a real clangd on a single C file with the SAME
# engine (only language id + spawned command differ).  Fast (no JVM/build
# server).  SKIPs without clangd.
test_lsp_clangd_SOURCES = tests/test_lsp_clangd.c we_places.c we_path.c we_read.c we_seen.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_lsp_clangd_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_lsp_clangd_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP engine, breadth: drives a real Python server (pyright or pylsp) on a single
# .py file -- whichever is installed -- with the SAME engine.  SKIPs without one.
test_lsp_python_SOURCES = tests/test_lsp_python.c we_places.c we_path.c we_read.c we_seen.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_lsp_python_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_lsp_python_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP engine, breadth: drives a real gopls on a single .go file in a tiny module.
# SKIPs without gopls + go.
test_lsp_gopls_SOURCES = tests/test_lsp_gopls.c we_places.c we_path.c we_read.c we_seen.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_lsp_gopls_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_lsp_gopls_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP engine, breadth: drives a real rust-analyzer on a single .rs file in a tiny
# crate.  Slowest (indexes std on cold start).  SKIPs without rust-analyzer + cargo.
test_lsp_rust_SOURCES = tests/test_lsp_rust.c we_places.c we_path.c we_read.c we_seen.c we_lsp.c we_lsp_pos.c we_lsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_lsp_rust_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_lsp_rust_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# What this build contains: tests/check-build-config.sh is first, so a run's
# log opens with the line `we --version` prints, and fails when configure, the
# binary and the feature lists above do not say the same thing.
#
# tests/check-window-kind.sh follows it: a window's payload is asked through
# we_winkind.c, the compiler still refuses a payload of the wrong kind, and
# every write to the window stack is counted by the owner.
TESTS = tests/check-build-config.sh tests/check-window-kind.sh test_lsp_positions test_checkheader test_fl_sel_len test_syntax_def test_fdloop test_clipboard test_lsp_buffer test_lsp_doctor test_lsp_semantic test_lsp_truecolor test_dap test_dap_dlv \
  test_dap_gdb test_dap_lldb test_dap_scala test_bsp_close test_lsp_requests \
  test_lsp_scala test_lsp_clangd test_lsp_python test_lsp_gopls test_lsp_rust test_debprompt test_debpath test_debsess test_debreq test_gdbmi test_menurows test_mousedrain test_termrelease \
  test_brkmatch test_shtok test_child test_childsig test_childfds test_childfds_loop test_childsafe test_keymap test_bindings test_menus test_paint test_splice test_lineend test_write_owner test_fileop test_startcmd test_startplan test_path test_path_say test_progname test_fieldtake test_layout test_rowfind test_rowfind_copy test_rowfind_nocontext test_rowfind_macsim test_rowfind_reject test_rowfind_illseq test_rowfind_badengine test_rowfind_badengine_copy test_rowfind_boundary test_rowfind_boundary_copy test_rowfind_nolocale test_rowfind_illseq_copy test_listsel test_helpbuf test_number_parse test_errpat test_optval test_fmt test_numtext test_winkind test_winkind_open

# One owner of input and of waiting: tests/check-one-owner.sh reads the
# undefined symbols of the editor's own objects (the list below) with $(NM)
# and fails when code outside the loop and the displays waits or reads on
# its own.  Offenders still to go are in tests/one-owner.allow.
TESTS += tests/check-one-owner.sh

# The editor's own C stays readable as the sweep changes it:
# tests/check-maintainability.sh measures the preprocessed sources with this
# build's own flags.  No f() declaration, no extern of a header's symbol, no
# getenv("XWPE_...") outside its owners and the terminal's mouse symbols in
# we_term.c are counted per file in tests/maintainability.allow (a count may
# only fall); the per-file and tree ceilings are reported, not enforced yet.
TESTS += tests/check-maintainability.sh

# The test hang guard and the per-test deadline runner (see test_watchdog).
TESTS += test_watchdog tests/check-with-deadline.sh
TESTS += test_child_state tests/check-child-state.sh
TESTS += tests/check-seen-rows.sh

# The BSD CI's list of known failures reports what it must (NEW, STALE, NO ROW
# LOG, NO SUMMARY, CHECK FAILED), so Linux runs its own self-test.
TESTS += tests/check-bsd-known-failures.sh

# The folder and the premises of a test that sets a special bit or a mode
# that keeps a user out come from one helper (tests/testdir.h, tested by
# test_testdir); tests/check-mode-premise.sh fails a test that does it
# without.
TESTS += test_testdir tests/check-mode-premise.sh
TESTS += test_interpose tests/check-interpose.sh

# The owner of "is this the object I saw" (we_seen.c, tested by test_seen):
# a record kept from an earlier moment is judged SAME, OTHER or UNSURE, never
# by an inode number alone.
TESTS += test_seen

# The owner of reading a file by a name the editor did not choose
# (we_read.c, tested by test_read_owner): no open waits, the object is the
# kind asked for, the bytes are bounded, and a refusal says which.
TESTS += test_read_owner

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

# One record of every version the CI pipelines run (tests/ci-versions.txt):
# tests/check-ci-versions.sh fails when a workflow, ci-run.sh or run-tests.sh
# disagrees with it, on a -latest runner label, an action not pinned by SHA
# or two versions of one action.  It exits 77 where .github is absent (a
# source tarball).  Offline; the record's age is checked by codeql.yml.
TESTS += tests/check-ci-versions.sh

# One owner of how the assistant's children are held (we_contain.c): every
# program the assistant starts goes through wpe_contain_start with the profile
# of what it is, and the start gate refuses one that names none.
# tests/check-contain-sites.sh fails when a we_ai*.c starts a child itself,
# spells WPE_CHILD_ORIGIN_AI, passes no profile, keeps an old capture face
# that took none, or an exec appears outside we_child.c.
TESTS += tests/check-contain-sites.sh

# One owner of how a tool step of the assistant ended (we_ai_step.h): the
# outcome is an enum, the text travels through an out-parameter, and no code
# tells a waiting step from a finished one by the identity of a pointer.
# tests/check-ai-tool-step.sh fails when a file compares a pointer with a
# static object, returns a static or a literal through a freeable pointer,
# returns an outcome of its own, drops a step's outcome, or brings back the
# old marker.  Offenders still to go are in tests/ai-tool-step.allow.
TESTS += tests/check-ai-tool-step.sh

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

# One owner of reading a file or a folder by a name the editor did not
# choose (we_read.c): the only code that opens such a name.
# tests/check-read-owner.sh fails when another file opens a name for reading
# with fopen, open, opendir or a library reader, builds a read request by
# hand, or spells the words of a refusal in the owner.  Offenders still to go
# are in tests/read-owner.allow, each with the pass that moves it.
TESTS += tests/check-read-owner.sh

# One owner of what the assistant's checkpoint knows about each path
# (we_ai_ckrec.c, tested by test_ai_ckrec): the only code that says a path was
# created, removed, moved, replaced or stayed, or that nobody can say.
# tests/check-checkpoint-record.sh fails when the checkpoint's code removes or
# renames a file outside the one function that takes a created file away, keeps
# a list of names of its own, decides "new" from a name, walks a folder around
# the owner, spells the words of a verdict, or drops what the owner returns
# (tests/checkpoint-record.allow holds what a later pass still moves).
if WPE_AI
TESTS += tests/check-checkpoint-record.sh
endif

# One owner of what a walk reads of a row: the readers of we_rowread.h are the
# only code that indexes a row, its bytes or the number of rows, in the
# bracket matcher (we_brkmatch.c) and in every file that no longer reads a
# row itself.  tests/check-brk-bounds.sh fails when any other function does,
# or when a macro or a pointer walk carries a raw read out of a reader;
# tests/row-reads.allow names, with their counts, the functions that write a
# row in place and the lines built for a call of the row owner.
TESTS += test_rowread tests/check-brk-bounds.sh

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

# One owner of how a session ends and of when a request is registered.  A
# request is written into its client's table only after the send succeeded:
# tests/check-session-end.sh fails when the debug adapter's or the build
# server's send function fills its slot first, writes the slot another way,
# or hands the slot to another function (tests/session-end.allow holds the
# functions each clause excuses, none for this one).  It also fails when a
# failed queue leaves the fd loop by any way but its one path, when an owner
# is told inside the walk of the writers, or when the owed hook is named
# outside its owner and its one user (read with $(NM) from the objects).
TESTS += tests/check-session-end.sh

# One owner of how long a path the editor holds may be (we_path.h) and of a
# dialog field's way out (we_fieldtake.h): tests/check-path-buffers.sh fails
# on an unbounded copy of a field's text, a refusal nobody reads, a call of
# e_add_arguments without its buffer's size, a second definition of the two
# numbers, a path number of a file's own, the system's PATH_MAX named outside
# the owner, and a strcpy, strcat, strncpy or sprintf nobody has judged
# (counted per function in tests/path-buffers.allow, which may only shrink).
TESTS += tests/check-path-buffers.sh test_manpat

# One owner of a help or Info token taken into fixed storage (we_helpbuf.h)
# and of a buffer row's bytes (we_buffer.c): tests/check-help-buffers.sh
# fails on an unbounded copy into a char array, a line grown by shifting its
# tail, and a row written with strcpy (tests/help-buffers.allow holds the
# sites of other readers found when the scan was first run, each with its
# reason).
TESTS += tests/check-help-buffers.sh

# One owner of a text built with a format (we_fmt.h): tests/check-fmt-writes.sh
# fails on sprintf, on a bounded write whose value is used or whose
# destination is at an offset, on a refusal of a whole text that is not read,
# and on a second spelling of the cut mark.  The bounded writes still to move
# are counted per function in tests/fmt-writes.allow, which may only shrink.
TESTS += tests/check-fmt-writes.sh

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

# One owner of the size the Wayland surface may commit: the frame owner
# (we_wlframe.c).  tests/check-wayland-frame.sh fails when another file
# attaches, damages, commits, acknowledges a configure, sizes the buffer or
# owns the surface's event queue, when a configure listener does more than
# record, when libwayland's dispatch, roundtrip or flush is called outside the
# connection functions of we_wayland.c, and when the lost-display end has a
# third caller or a second X I/O handler.  It scans sources, so it runs on a build without the backend too.
# test_wlframe runs the owner's state machine with the compositor replaced by a
# recorder (no display); it defines the two globals the owner reads (WpeWl,
# WpeRender).
TESTS += tests/check-wayland-frame.sh

# One owner of the X11 back buffer (we_xbackbuf.c): tests/check-x11-backbuf.sh
# fails when another file creates, frees or copies the pixmap, when its handle
# is a member of anything, or when Expose is handled outside
# e_x_answer_expose.  It scans sources, so it runs on every build.
TESTS += tests/check-x11-backbuf.sh
# One owner of the X11 connection after the server is gone (WeXterm.c, beside
# its I/O error handler): tests/check-x11-lost-owner.sh fails when a call that
# can put a request on the wire, or wait for a reply, is added outside that
# owner's guard, in a function, in a macro or behind a wrapper.  It scans the
# sources and the objects' undefined symbols, so a build without X skips it.
# The functions that need no guard of their own, and why, are counted in
# tests/x11-lost-owner.allow.
TESTS += tests/check-x11-lost-owner.sh
# test_xbackbuf runs the owner's sequencing with Xlib replaced by a recorder
# (no display): every release, the new buffer, the carry, every back, the old
# buffer freed; nothing copied while there is no buffer, and nothing copied
# once the connection is gone, including by a remake the loss interrupts.
if HAVE_X11
check_PROGRAMS += test_xbackbuf
TESTS += test_xbackbuf
test_xbackbuf_SOURCES = tests/test_xbackbuf.c we_xbackbuf.c we_grid.c
test_xbackbuf_CFLAGS = $(TEST_CFLAGS) $(X_CFLAGS) $(XFT_CFLAGS) $(NCURSESW_CFLAGS)
endif

# test_xcells runs the refresh's cell walk with the renderer, the back buffer
# and Xlib replaced by a recorder (no display): a loss found inside the request
# of one cell ends the row there, on both routes of a refresh, and no request
# follows it.  A check once a row would leave a row of requests on a server
# that is gone, which is the crash tests/x11/test_display_lost.py ends on.
if HAVE_X11
check_PROGRAMS += test_xcells
TESTS += test_xcells
test_xcells_SOURCES = tests/test_xcells.c we_xterm.c we_grid.c we_path.c we_fmt.c
test_xcells_CFLAGS = $(AM_CFLAGS)
test_xcells_LDADD = $(X_LIBS) $(XFT_LIBS) $(LIBM)
endif

# Build configurations the default build does not make (an X11 build without
# Xft and Cairo, with and without the Wayland backend): each row is configured,
# built and asked what it contains in a copy of the tree.  Not part of
# `make check`: it nests a whole configure and build, so it is its own target
# and a CI job runs it.
check-build-configs:
	$(SHELL) $(srcdir)/tests/build-config-row.sh --srcdir $(srcdir)
.PHONY: check-build-configs

# The C tests at a long TMPDIR: `make check` once with TMPDIR of 120 bytes and
# once with 300, on a long base made on disk in the build tree.  A test that
# depends on how long its folder's path is passes at a short one and fails
# here.  Not part of `make check`: it runs the whole C suite twice, so it is
# its own target and a CI job runs it.  tests/check-test-paths.sh names the
# sites it can see statically (a socket bound by full name, a folder buffer
# of a number, a bare join).
check-path-lengths:
	+MAKE='$(MAKE)' $(SHELL) $(srcdir)/tests/path-lengths.sh --make '$(MAKE)' 120 300
.PHONY: check-path-lengths

# One owner of how long a test folder's path may be (tests/testdir.h makes the
# folder, joins a name onto it and plants a socket in it): the check fails a
# test that binds a socket by its full path, declares a folder buffer of a
# number, or joins onto the folder with a bare snprintf.
TESTS += tests/check-test-paths.sh
if HAVE_WAYLAND
check_PROGRAMS += test_wlframe
TESTS += test_wlframe
test_wlframe_SOURCES = tests/test_wlframe.c we_wlframe.c we_path.c we_fmt.c \
	we_grid.c
test_wlframe_CFLAGS = $(TEST_CFLAGS) $(WAYLAND_CFLAGS)
test_wlframe_LDADD = $(WAYLAND_LIBS)
endif

# One exec site, one close step, one closer: tests/check-child-fds.sh fails
# on an exec outside the owner, on a child start that can skip the step that
# closes what the editor inherited, and on a close_range or closefrom
# outside it.
TESTS += tests/check-child-fds.sh

# What the child may do between the fork and the exec: only what takes no
# lock and allocates nothing (the graphical editor has a font thread whose
# lock a fork can freeze).  tests/check-child-exec.sh compiles we_child.c with
# the build's flags (MAINT_CC, MAINT_FLAGS), follows every symbol reachable
# from wpe_child_exec and fails on any that is not in
# tests/child-exec-safe.allow, with its reason.
TESTS += tests/check-child-exec.sh

# How a program waits for a child: the sync layers and the tests that start
# children wait on the editor's loop (we_fdloop.c), reap through the owner
# (we_child.c) and are bounded by the one hang guard (tests/test_watchdog.h),
# with a deadline on every wait and no second death notice per platform.
# tests/check-child-wait.sh fails on a wait, a reap or an alarm of its own, on
# a pidfd or kqueue, on a test of the owner that does not arm the guard and on
# a sync-layer wait with no deadline; the exceptions that are meant are in
# tests/child-wait.allow, with their reasons.
TESTS += tests/check-child-wait.sh

# The awk inside the check scripts is linear in the size of the file it
# reads, on every awk: the comments and literals are blanked one line at a
# time by the one blanker, tests/blank-c.awk, and a whole-file walk indexes an
# array of characters.  tests/check-sh-idiom.sh names a character walk over a
# string built across lines, a copy of the rest of it, and a length( in a loop
# condition, in any tests/check-*.sh.
TESTS += tests/check-sh-idiom.sh

# One owner of the selected entry of a list window (we_listsel.h):
# tests/check-list-entry.sh counts, per function, the indexings of a list's
# names by the highlighted row outside the owner; the file manager's panels
# still doing it are in tests/list-entry.allow, and the counts may only fall.
TESTS += tests/check-list-entry.sh

# One owner of the screen grid's size and of the memory behind it
# (we_grid.c).  test_grid links the owner alone and drives it with a
# recording allocator: the floor and the ceiling, the counts, zeroed planes,
# every refusal leaving the old grid whole, the first take, the same size, the
# size just refused, and a saved rectangle that reaches past every side.
# tests/check-grid-size.sh fails when code outside the owner multiplies the
# two sizes, allocates or frees a plane, or names the owner's object.
check_PROGRAMS += test_grid
TESTS += test_grid tests/check-grid-size.sh
test_grid_SOURCES = tests/test_grid.c we_grid.c
test_grid_CFLAGS = $(TEST_CFLAGS)

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

# One owner of what the process table says: tests/procinfo.py.  No test file
# builds a ps(1) argument vector of its own; tests/check-test-processes.sh
# fails on one that is not in tests/test-processes.allow, with its pass.
TESTS += tests/check-test-processes.sh

# One owner of a core file a test child leaves: a child that dies by a core
# signal on purpose gives up its core file first (test_child_dies_on_purpose
# in tests/test_watchdog.h); tests/check-test-core-exits.sh fails on a C test
# that does not and is not in tests/test-core-exits.allow, with its reason.
TESTS += tests/check-test-core-exits.sh

# One owner of who forks a test process: the main thread (an editor's death
# signal follows the thread that forked it).  tests/check-test-threads.sh
# fails on a test file that starts a session from a thread pool or a thread
# and is not in tests/test-threads.allow.
TESTS += tests/check-test-threads.sh

# One owner of what a row can run: the tool farm (tests/engine_guard.py), the
# PATH every process of a row gets, made by tests/gui_owner.py.  A program
# that is not in its baseline is not found, so a row never reaches an AI
# engine of the machine.  tests/check-test-real-tools.sh fails on a door
# around it (a program or the editor started outside the owner, the live-only
# opt-out in a file that is not live, an engine named by an absolute path)
# that is not in tests/test-real-tools.allow.
TESTS += tests/check-test-real-tools.sh

# One owner of how long a wait lasts: tests/gui_owner.py reads
# XWPE_TEST_WAIT_SCALE and tests/wpe_driver.py turns a row's seconds into a
# deadline (wait_screen, wait_until, wait_deadline).  tests/check-test-waits.sh
# fails on a test file that reads the variable, runs a wait loop of its own, or
# hands the owner a time it has already multiplied by the scale.
TESTS += tests/check-test-waits.sh

# The host tools the rows need and where the Linux job gets each:
# tests/ci-tools.txt is the one record.  tests/check-ci-tools.sh fails when a
# tool a manifest row declares has no line there, when an installed tool's
# package is not in the workflow's apt-get list, or when the job's
# XWPE_TEST_TOOLS_EXPECTED differs from the tools it installs.  Offline; a
# source tarball without .github is skipped.
TESTS += tests/check-ci-tools.sh

# tests/check-test-paths.sh (listed above) also holds the pytest rules: a
# window title is read by column, a path of N bytes is made by deep_folder, an
# asserted phrase is one of the pane reader's needles (rules P1 to P3, run
# from tests/check_test_paths.py, with their canaries).

# The ncurses pyte suite at a 300 byte temporary folder, the proof that the
# rows above hold.  Not part of `make check`: it is one run of the whole suite
# (an hour), so a scheduled CI job runs it.  It names the rows that are red at
# a long folder for a product reason (tests/path-lengths-pyte.sh).
check-path-lengths-pyte:
	$(SHELL) $(srcdir)/tests/path-lengths-pyte.sh --srcdir $(srcdir) --builddir .
.PHONY: check-path-lengths-pyte

# The feature macros have one owner, configure (AC_USE_SYSTEM_EXTENSIONS
# writes them into config.h): tests/check-platform.sh fails on a source that
# sets one of its own, and on a test that compiles without config.h's.  The
# same script holds the flag rows: an implicit declaration is an error, the
# -Wno- flags are known to the compiler, nm -u is read in every system's format.
TESTS += tests/check-platform.sh
AM_TESTS_ENVIRONMENT = NM='$(NM)'; export NM; \
  CC='$(CC)'; export CC; NCURSESW_CFLAGS='$(NCURSESW_CFLAGS)'; export NCURSESW_CFLAGS; \
  KEYMAP_TEXI='$(top_builddir)/tools/keymap-texi$(EXEEXT)'; export KEYMAP_TEXI; \
  ONE_OWNER_OBJECTS='$(we_OBJECTS)'; export ONE_OWNER_OBJECTS; \
  CHILD_WAIT_OBJECTS='test_dap-we_dap_sync.$(OBJEXT) test_lsp_buffer-we_lsp_sync.$(OBJEXT) test_bsp_close-we_bsp_sync.$(OBJEXT)'; export CHILD_WAIT_OBJECTS; \
  MAINT_CC='$(CC)'; export MAINT_CC; \
  MAINT_FLAGS='$(DEFS) $(DEFAULT_INCLUDES) -I$(srcdir) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS)'; export MAINT_FLAGS; \
  MAINT_SOURCES='$(we_SOURCES)'; export MAINT_SOURCES; \
  WPE_BUILD_BINARY='$(top_builddir)/we$(EXEEXT)'; export WPE_BUILD_BINARY; \
  WPE_FEATURE_SOURCES='with-x:$(X11_SOURCES);cairo:$(CAIRO_SOURCES);with-gpm:$(GPM_SOURCES);libvterm:$(VTERM_SOURCES);with-wayland:$(WAYLAND_SOURCES);enable-ai:$(AI_SOURCES);enable-ai-agent-host:$(AI_HOST_SOURCES)'; export WPE_FEATURE_SOURCES; \
  WPE_FEATURE_TESTS='enable-ai:$(AI_TESTS);enable-ai-tls:$(AI_TLS_CHECK)'; export WPE_FEATURE_TESTS;

# --- AI assistant unit tests (only when --enable-ai) ---
# WPE_AI comes from config.h (the build is configured --enable-ai here).  The
# framer test links AITLS_LIBS so it still links when the build also enabled TLS
# (the TLS code is then compiled in, though these tests exercise only the plain
# framer/diff); AITLS_LIBS is empty when --enable-ai-tls was not given.  The
# HTTP test also drives a GET on the editor's loop (we_fdloop.c) against a
# local socket; we_ai_http_sync.c, the waiting calls a test needs, is linked
# into the tests only, never into the editor.
if WPE_AI
check_PROGRAMS += $(AI_TESTS)
TESTS += $(AI_TESTS)
test_ai_path_SOURCES = tests/test_ai_path.c we_ai_path.c we_read.c we_seen.c we_places.c we_path.c we_fmt.c
test_ai_path_CFLAGS = $(TEST_CFLAGS)
test_ai_scope_SOURCES = tests/test_ai_scope.c we_ai_path.c we_read.c we_seen.c we_places.c we_path.c we_fmt.c
test_ai_scope_CFLAGS = $(TEST_CFLAGS)
# A pipe where a cloned folder keeps what the assistant's layer reads (the git
# config, "commondir", a build server's connection file) does not stop the
# editor: the reads go through the read owner (we_read.c).
test_ai_read_waits_SOURCES = tests/test_ai_read_waits.c we_ai_path.c we_places.c we_path.c we_read.c we_seen.c we_bsp.c we_bsp_sync.c we_child.c we_debreq.c we_dap_proto.c we_fdloop.c we_startplan.c we_startcmd.c we_fmt.c
test_ai_read_waits_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_ai_read_waits_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)
# A folder swapped for a link while the scope scan runs is not entered: the
# linker's --wrap puts a swap around the scan's look at an entry.  Without a
# linker that has --wrap (Apple's ld) the row is reported as SKIP.
test_ai_scan_swap_SOURCES = tests/test_ai_scan_swap.c we_ai_path.c we_places.c we_path.c we_read.c we_seen.c we_fmt.c
test_ai_scan_swap_CFLAGS = $(TEST_CFLAGS)
if HAVE_LD_WRAP
test_ai_scan_swap_CFLAGS += -DWPE_TEST_LD_WRAP=1
test_ai_scan_swap_LDFLAGS = -Wl,--wrap=wpe_read_entry_kind
endif
# The checkpoint's record (we_ai_ckrec.c): every state of a path at the two
# moments against the verdict the judge gives, a renamed file or folder, a bound
# on either side, a folder nobody may read, and the words of every verdict and
# note, each on a real folder the test makes.
test_ai_ckrec_SOURCES = tests/test_ai_ckrec.c tests/testdir.h tests/seen_rows.h we_ai_ckrec.c we_read.c we_seen.c we_places.c we_path.c we_fmt.c
test_ai_ckrec_CFLAGS = $(TEST_CFLAGS)
# How the assistant's children are held (we_contain.c): the environment each
# profile gives its child, the PATH and the program resolved in the parent,
# the limits and the report, and, through the owner of children (we_child.c,
# with the refusal of an assistant start that names no profile on), what a
# started child really gets.
test_ai_contain_SOURCES = tests/test_ai_contain.c we_contain.c we_path.c we_child.c we_fdloop.c we_fmt.c
test_ai_contain_CFLAGS = $(TEST_CFLAGS)
test_ai_contain_LDADD = $(UTIL_LIBS)
# What a saved conversation may hold (we_ai_session.c): the resume id's shape
# and the judge of a conversation file, with json-c and nothing of the editor.
test_ai_session_SOURCES = tests/test_ai_session.c we_ai_session.c
test_ai_session_CFLAGS = $(TEST_CFLAGS) $(JSONC_CFLAGS)
test_ai_session_LDADD = $(JSONC_LIBS)
test_ai_http_SOURCES = tests/test_ai_http.c we_ai_http.c we_ai_http_sync.c we_fdloop.c we_fmt.c
test_ai_http_CFLAGS = $(TEST_CFLAGS) $(AITLS_CFLAGS)
test_ai_http_LDADD = $(AITLS_LIBS)
test_ai_diff_SOURCES = tests/test_ai_diff.c we_ai_diff.c
test_ai_diff_CFLAGS = $(TEST_CFLAGS)
# How a tool step ended (we_ai_step.h): asks for -O2, because the shape it
# guards is the optimised one (the builder's own CFLAGS come after and win);
# it links nothing of the editor.
test_ai_tool_step_SOURCES = tests/test_ai_tool_step.c we_ai_step.h
test_ai_tool_step_CFLAGS = $(TEST_CFLAGS) -O2
# Live TLS smoke: built with the TLS backend but kept out of TESTS (it needs the
# network); run by hand, e.g. ./test_ai_tls_live https://api.github.com /zen
if WPE_AI_TLS
check_PROGRAMS += $(AI_TLS_CHECK)
test_ai_tls_live_SOURCES = tests/test_ai_tls_live.c we_ai_http.c we_ai_http_sync.c we_fdloop.c we_fmt.c
test_ai_tls_live_CFLAGS = $(TEST_CFLAGS) $(AITLS_CFLAGS)
test_ai_tls_live_LDADD = $(AITLS_LIBS)
endif
endif

# --- Extra files for distribution ---
EXTRA_DIST = \
  contrib/xwpe-env \
  contrib/setup.sh \
  contrib/bsd-test/README.md \
  contrib/bsd-test/ci-run.sh \
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
  tests/check-maintainability.sh \
  tests/maintainability.py \
  tests/maintainability.allow \
  tests/frames_compare.py \
  tests/with-deadline.sh \
  tests/bsd-known-failures.sh \
  tests/check-bsd-known-failures.sh \
  tests/check-mode-premise.sh \
  tests/mode_premise.py \
  tests/seen_reuse.py \
  tests/seen_premise.py \
  tests/check-with-deadline.sh \
  tests/test_watchdog.h \
  tests/test_loop_until.h \
  tests/child_state.h \
  tests/test_child_state.c \
  tests/check-child-state.sh \
  tests/check_child_state.py \
  tests/check-seen-rows.sh \
  tests/interpose.h \
  tests/linebuf.h \
  tests/test_interpose.c \
  tests/check-interpose.sh \
  tests/check-start-plan.sh \
  tests/start-plan.allow \
  tests/check-shell-quote.sh \
  tests/check-ci-versions.sh \
  tests/ci-versions.txt \
  tests/check-contain-sites.sh \
  tests/check-ai-tool-step.sh \
  tests/ai-tool-step.allow \
  tests/test_ai_tool_step.c \
  tests/test_ai_tool_step.py \
  tests/check-line-ends.sh \
  tests/line-ends.allow \
  tests/test_lineend.c \
  tests/check-platform.sh \
  tests/objsyms.sh \
  tests/compiler.sh \
  tests/workdir.sh \
  tests/check-test-paths.sh \
  tests/path-lengths.sh \
  tests/nm-undefined.sh \
  tests/build-config.sh \
  tests/check-build-config.sh \
  tests/check-window-kind.sh \
  we_ai_agent.c \
  tests/check-write-owner.sh \
  tests/check-read-owner.sh \
  tests/read-owner.allow \
  tests/check-checkpoint-record.sh \
  tests/checkpoint-record.allow \
  tests/test_ai_ckrec.c \
  tests/test_ai_changeset_record.py \
  tests/test_read_owner.c \
  tests/test_read_waits.py \
  tests/test_read_waits_plain.py \
  tests/test_ai_read_waits.c \
  tests/test_ai_scan_swap.c \
  tests/test_ai_read_owner.py \
  tests/test_ai_read_tools.py \
  tests/check-brk-bounds.sh \
  tests/row-reads.allow \
  tests/test_rowread.c \
  tests/test_row_readers.py \
  tests/test_help_paint.py \
  tests/help_pages.py \
  tests/test_help_check.py \
  tests/test_error_search_text.py \
  tests/x11/test_help_paint.py \
  tests/wayland/test_help_paint.py \
  tests/write-owner.allow \
  tests/test_write_owner.c \
  tests/test_ai_session.c \
  tests/test_fileop.c \
  tests/check-debug-command.sh \
  tests/debug-command.allow \
  tests/check-chain-outcome.sh \
  tests/chain-outcome.allow \
  tests/check-session-end.sh \
  tests/session-end.allow \
  tests/check-path-buffers.sh \
  tests/path-buffers.allow \
  tests/check-help-buffers.sh \
  tests/help-buffers.allow \
  tests/check-fmt-writes.sh \
  tests/fmt-writes.allow \
  tests/check-number-fields.sh \
  tests/number-fields.allow \
  tests/test_number_parse.c \
  tests/test_number_fields.py \
  tests/check-selection-mode.sh \
  tests/check-list-entry.sh \
  tests/list-entry.allow \
  tests/test_listsel.c \
  tests/test_winkind.c \
  tests/test_winkind_open.c \
  tests/stub_write_window.c \
  tests/test_helpbuf.c \
  tests/test_fmt.c \
  tests/test_fmt_sites.py \
  tests/test_ai_formatted_requests.py \
  tests/test_numtext.c \
  tests/test_shtok.c \
  tests/test_brkmatch.c \
  tests/test_errpat.c \
  tests/test_optval.c \
  tests/test_option_codec.py \
  tests/x11/test_option_codec.py \
  tests/wayland/test_option_codec.py \
  tests/test_child_fds.py \
  tests/x11/test_child_fds.py \
  tests/wayland/test_child_fds.py \
  tests/test_help_buffers.py \
  tests/x11/test_help_buffers.py \
  tests/wayland/test_help_buffers.py \
  tests/test_list_entry.py \
  tests/check-program-name.sh \
  tests/check-tty-open.sh \
  tests/tty-open.allow \
  tests/check-wayland-frame.sh \
  tests/wayland-frame.allow \
  tests/check-x11-backbuf.sh \
  tests/check-x11-lost-owner.sh \
  tests/x11-lost-owner.allow \
  tests/test_xbackbuf.c \
  tests/test_xcells.c \
  tests/test_grid.c \
  tests/check-grid-size.sh \
  tests/grid-size.allow \
  tests/test_grid_ncurses.py \
  tests/x11/test_grid.py \
  tests/x11-backbuf.allow \
  tests/build-config-row.sh \
  tests/test_wlframe.c \
  tests/wayland/test_grid.py \
  tests/wayland/test_surface_bound.py \
  tests/wayland/test_display_lost.py \
  tests/x11/test_display_lost.py \
  tests/check-child-fds.sh \
  tests/child-fds.allow \
  tests/check-child-exec.sh \
  tests/child-exec-safe.allow \
  tests/check-child-wait.sh \
  tests/check-sh-idiom.sh \
  tests/blank-c.awk \
  tests/child-wait.allow \
  tests/program-name.allow \
  tests/check-box-layout.sh \
  tests/box-layout.allow \
  tests/check-keymap.sh \
  tests/check-keymap.allow \
  tests/check-menus.sh \
  tests/check-menus.allow \
  tests/check-test-processes.sh \
  tests/check_test_processes.py \
  tests/test-processes.allow \
  tests/check-test-core-exits.sh \
  tests/check_test_core_exits.py \
  tests/test-core-exits.allow \
  tests/check-test-threads.sh \
  tests/check_test_threads.py \
  tests/test-threads.allow \
  tests/check-test-real-tools.sh \
  tests/check_test_real_tools.py \
  tests/check-test-waits.sh \
  tests/check_test_waits.py \
  tests/check-ci-tools.sh \
  tests/check_ci_tools.py \
  tests/ci-tools.txt \
  tests/ci-install.sh \
  tests/ci-venv.sh \
  tests/ci-shard.sh \
  tests/ci-uncut-steps.txt \
  tests/stubs/no-grace/timeout \
  tests/inner_job_cut.py \
  tests/test_job_cut.py \
  tests/harness/job_cut.py \
  tests/program_gate.py \
  tests/test-waits.allow \
  tests/harness/suite_plan.py \
  tests/harness/suite-timings.json \
  tests/test-real-tools.allow \
  tests/engine_guard.py \
  tests/live_gate.py \
  tests/check_test_paths.py \
  tests/test-pane-needles.allow \
  tests/path-lengths-pyte.sh \
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
  tests/test_project_hold.py \
  tests/test_window_views.py \
  tests/test_window_menu.py \
  tests/test_block.py \
  tests/test_block_asan.py \
  tests/test_clipboard_osc52.py \
  tests/wpe_driver.py \
  tests/host_tools.py \
  tests/procinfo.py \
  tests/build_config.py \
  tests/test_config_aware_menu.py \
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
  tests/wayland/test_number_fields.py \
  tests/x11/conftest.py \
  tests/x11/matchbox-kbdconfig \
  tests/x11/test_editor_options.py \
  tests/x11/test_number_fields.py \
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
