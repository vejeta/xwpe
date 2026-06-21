SUBDIRS = docs

AUTOMAKE_OPTIONS = foreign

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

we_SOURCES = \
  we_main.c \
  we_block.c \
  we_clip.c \
  we_unix.c \
  we_e_aus.c \
  we_edit.c \
  we_fl_fkt.c \
  we_fl_unix.c \
  we_hfkt.c \
  we_menue.c \
  we_mouse.c \
  we_opt.c \
  we_wind.c \
  we_prog.c \
  we_progn.c \
  we_debug.c \
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
  we_trace.c \
  we_vterm.c \
  we_dap_proto.c \
  we_dap.c \
  we_bsp.c \
  we_lsp.c

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
  -lz

# --- Header files (for dist) ---
noinst_HEADERS = \
  Xwpe.h \
  WeString.h \
  WeXterm.h \
  WeProg.h \
  WeExpArr.h \
  we_render.h \
  we_fdloop.h \
  we_trace.h \
  we_vterm.h \
  we_dap_proto.h \
  we_dap.h \
  we_bsp.h \
  we_lsp.h \
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
	$(MKDIR_P) $(DESTDIR)$(man1dir)
	$(INSTALL_DATA) $(srcdir)/xwpe.1_eng $(DESTDIR)$(man1dir)/xwpe.1
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
	      $(DESTDIR)$(man1dir)/xwpe.1 \
	      $(DESTDIR)$(hicoloricondir)/scalable/apps/xwpe.svg
	@for s in 256 128 64 48 32 24 16; do \
		rm -f $(DESTDIR)$(hicoloricondir)/$${s}x$${s}/apps/xwpe.png; \
	done

# --- Man pages ---
dist_man1_MANS = we.1 wpe.1 xwe.1

# --- Desktop file + AppStream MetaInfo ---
desktopdir = $(datadir)/applications
desktop_DATA = xwpe.desktop
metainfodir = $(datadir)/metainfo
metainfo_DATA = io.codeberg.mendezr.xwpe.metainfo.xml

# --- Unit tests ---
check_PROGRAMS = test_checkheader test_syntax_def test_fdloop test_clipboard test_lsp_buffer test_lsp_doctor test_lsp_semantic test_dap test_dap_dlv test_dap_gdb test_dap_lldb test_dap_scala test_lsp_scala test_lsp_clangd test_lsp_python test_lsp_gopls test_lsp_rust

test_fdloop_SOURCES = tests/test_fdloop.c we_fdloop.c
test_fdloop_CFLAGS = $(XWPE_STDFLAG)

# Clipboard helpers unit test: RFC 4648 base64 + OSC 52 framing (we_clip.c
# only, no editor/X11). Always runs (no skip).
test_clipboard_SOURCES = tests/test_clipboard.c we_clip.c
test_clipboard_CFLAGS = $(XWPE_STDFLAG)

test_checkheader_SOURCES = tests/test_checkheader.c
test_checkheader_CFLAGS = $(XWPE_STDFLAG)
test_checkheader_LDFLAGS =

test_syntax_def_SOURCES = tests/test_syntax_def.c
test_syntax_def_CFLAGS = $(XWPE_STDFLAG)
test_syntax_def_LDFLAGS =

# LSP buffer-serialization unit test: the pure e_lsp_join_lines invariant (one
# '\n' per editor line, WPE_WR not doubled).  Links we_lsp.c only; no server,
# always runs (no skip).
test_lsp_buffer_SOURCES = tests/test_lsp_buffer.c we_lsp.c we_dap_proto.c
test_lsp_buffer_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_buffer_LDADD = $(JSONC_LIBS)

# LSP Doctor dedup unit test: the pure e_lsp_doc_is_new gate (re-pushed Doctor
# rendered only when changed; reset per session shows it again).  we_lsp.c only.
test_lsp_doctor_SOURCES = tests/test_lsp_doctor.c we_lsp.c we_dap_proto.c
test_lsp_doctor_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_doctor_LDADD = $(JSONC_LIBS)

# LSP semantic-tokens decode unit test: the pure delta-decode (we_lsp.c).
test_lsp_semantic_SOURCES = tests/test_lsp_semantic.c we_lsp.c we_dap_proto.c
test_lsp_semantic_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_semantic_LDADD = $(JSONC_LIBS)

# DAP transport unit test: links the pure transport core + json-c, no xwpe.
test_dap_SOURCES = tests/test_dap.c we_dap_proto.c
test_dap_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_dap_LDADD = $(JSONC_LIBS)

# DAP engine integration test: drives a real `dlv dap` (SKIPs without dlv+go).
test_dap_dlv_SOURCES = tests/test_dap_dlv.c we_dap.c we_dap_proto.c
test_dap_dlv_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_dap_dlv_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# DAP stdio transport integration test: drives a real `gdb --interpreter=dap`
# on a rustc-built binary (the Rust slice).  SKIPs without gdb+rustc.
test_dap_gdb_SOURCES = tests/test_dap_gdb.c we_dap.c we_dap_proto.c
test_dap_gdb_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_dap_gdb_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# Same stdio transport against `lldb-dap` (the LLVM adapter; native on macOS).
# SKIPs without lldb-dap+rustc.
test_dap_lldb_SOURCES = tests/test_dap_lldb.c we_dap.c we_dap_proto.c
test_dap_lldb_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_dap_lldb_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# JVM/Scala path: BSP bootstrap (we_bsp.c) + DAP TCP-client transport against a
# real scala-cli/Bloop.  SKIPs without scala-cli; slow on a cold coursier cache.
test_dap_scala_SOURCES = tests/test_dap_scala.c we_bsp.c we_dap.c we_dap_proto.c
test_dap_scala_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_dap_scala_LDADD = $(JSONC_LIBS) $(UTIL_LIBS)

# LSP engine: drives a real Metals language server on a single Scala file
# (hover/definition/completion/diagnostics).  SKIPs without metals + scala-cli.
test_lsp_scala_SOURCES = tests/test_lsp_scala.c we_lsp.c we_dap_proto.c
test_lsp_scala_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_scala_LDADD = $(JSONC_LIBS)

# LSP engine, breadth: drives a real clangd on a single C file with the SAME
# engine (only language id + spawned command differ).  Fast (no JVM/build
# server).  SKIPs without clangd.
test_lsp_clangd_SOURCES = tests/test_lsp_clangd.c we_lsp.c we_dap_proto.c
test_lsp_clangd_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_clangd_LDADD = $(JSONC_LIBS)

# LSP engine, breadth: drives a real Python server (pyright or pylsp) on a single
# .py file -- whichever is installed -- with the SAME engine.  SKIPs without one.
test_lsp_python_SOURCES = tests/test_lsp_python.c we_lsp.c we_dap_proto.c
test_lsp_python_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_python_LDADD = $(JSONC_LIBS)

# LSP engine, breadth: drives a real gopls on a single .go file in a tiny module.
# SKIPs without gopls + go.
test_lsp_gopls_SOURCES = tests/test_lsp_gopls.c we_lsp.c we_dap_proto.c
test_lsp_gopls_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_gopls_LDADD = $(JSONC_LIBS)

# LSP engine, breadth: drives a real rust-analyzer on a single .rs file in a tiny
# crate.  Slowest (indexes std on cold start).  SKIPs without rust-analyzer + cargo.
test_lsp_rust_SOURCES = tests/test_lsp_rust.c we_lsp.c we_dap_proto.c
test_lsp_rust_CFLAGS = $(XWPE_STDFLAG) $(JSONC_CFLAGS)
test_lsp_rust_LDADD = $(JSONC_LIBS)

TESTS = test_checkheader test_syntax_def test_fdloop test_clipboard test_lsp_buffer test_lsp_doctor test_lsp_semantic test_dap test_dap_dlv \
  test_dap_gdb test_dap_lldb test_dap_scala test_lsp_scala test_lsp_clangd test_lsp_python test_lsp_gopls test_lsp_rust

# --- Extra files for distribution ---
EXTRA_DIST = \
  contrib/xwpe-env \
  contrib/setup.sh \
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
  tests/test_checkheader.c \
  tests/test_syntax_def.c \
  tests/test_clipboard.c \
  tests/test_lsp_buffer.c \
  tests/test_lsp_doctor.c \
  tests/test_lsp_semantic.c \
  tests/test_dap.c \
  tests/test_dap_dlv.c \
  tests/test_dap_gdb.c \
  tests/test_dap_lldb.c \
  tests/test_utf8_border.py \
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
  tests/x11/conftest.py \
  tests/x11/matchbox-kbdconfig \
  tests/x11/test_editor_options.py \
  tests/x11/test_block_marking.py \
  tests/x11/test_clipboard.py \
  tests/x11/test_menu_edit.py \
  tests/x11/test_menu_search.py \
  tests/x11/test_menu_file.py \
  tests/x11/test_menu_system.py \
  tests/x11/test_menu_help.py \
  tests/x11/test_menu_window.py \
  tests/x11/test_menu_project.py \
  tests/x11/test_menu_run.py \
  tests/x11/test_menu_debug.py \
  tests/x11/test_menu_status_bar.py \
  tests/x11/test_popup_close_button.py \
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
