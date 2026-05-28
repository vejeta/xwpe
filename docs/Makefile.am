info_TEXINFOS = xwpe.texi

xwpe_TEXINFOS = \
  version.texi \
  chapters/intro.texi \
  chapters/installation.texi \
  chapters/interface.texi \
  chapters/editor.texi \
  chapters/compiling.texi \
  chapters/debugging.texi \
  chapters/projects.texi \
  chapters/configuration.texi \
  chapters/reference.texi \
  chapters/tutorials.texi \
  chapters/history.texi \
  chapters/bugs-and-limits.texi

html-local: xwpe.texi $(xwpe_TEXINFOS)
	$(MAKEINFO) --html -o xwpe.html $(srcdir)/xwpe.texi

pdf-local: xwpe.texi $(xwpe_TEXINFOS)
	if test "$(TEXI2PDF)" != ":"; then \
	  $(TEXI2PDF) -o xwpe.pdf $(srcdir)/xwpe.texi; \
	fi

clean-local:
	rm -rf xwpe.html xwpe.pdf xwpe.md xwpe.xml

EXTRA_DIST = \
  texinfo.tex \
  sources/README.md
