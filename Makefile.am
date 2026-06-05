SUBDIRS = docs

AUTOMAKE_OPTIONS = foreign

# --- Compiler flags ---
# gnu17 is required: the code uses K&R-compatible GNU extensions
AM_CFLAGS = -std=gnu17 \
  $(NCURSESW_CFLAGS) \
  $(X_CFLAGS) \
  $(XFT_CFLAGS) \
  $(CAIRO_CFLAGS) \
  $(PANGO_CFLAGS) \
  -DLIBRARY_DIR=\"$(pkglibdir)\" \
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
  we_fdloop.c

we_LDADD = \
  $(NCURSESW_LIBS) \
  $(X_LIBS) \
  $(XFT_LIBS) \
  $(CAIRO_LIBS) \
  $(PANGO_LIBS) \
  $(GPM_LIBS) \
  $(UTIL_LIBS) \
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
check_PROGRAMS = test_checkheader test_syntax_def

test_checkheader_SOURCES = tests/test_checkheader.c
test_checkheader_CFLAGS = -std=gnu17
test_checkheader_LDFLAGS =

test_syntax_def_SOURCES = tests/test_syntax_def.c
test_syntax_def_CFLAGS = -std=gnu17
test_syntax_def_LDFLAGS =

TESTS = test_checkheader test_syntax_def

# --- Extra files for distribution ---
EXTRA_DIST = \
  help.xwpe_eng \
  help.key_eng \
  syntax_def \
  xwpe.1_eng \
  xwpe.desktop \
  io.codeberg.mendezr.xwpe.metainfo.xml \
  xwpe.spec \
  icons/xwpe.svg \
  icons/xwpe-small.svg \
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
  tests/test_utf8_border.py \
  tests/test_scrollbar.py \
  tests/test_compile.py \
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
  tests/x11/conftest.py \
  tests/x11/test_editor_options.py \
  tests/samples/hello.c \
  tests/samples/big.c \
  tests/samples/two_errors.c \
  tests/samples/hello.cpp \
  tests/samples/hello.f90 \
  tests/samples/hello.p \
  tests/samples/hello.pl \
  tests/samples/hello.py \
  tests/samples/hello.cob \
  tests/samples/hello.tex \
  tests/samples/Hello.java \
  tests/fixtures/ascii.txt \
  tests/fixtures/accents.txt \
  tests/fixtures/long_accents.txt \
  tests/fixtures/cjk.txt \
  tests/fixtures/cyrillic.txt \
  tests/fixtures/emoji.txt \
  tests/fixtures/emoji_scroll.txt \
  tests/fixtures/short.txt \
  tests/fixtures/one_page.txt \
  tests/fixtures/two_pages.txt \
  tools/compiletest.c
