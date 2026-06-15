info_TEXINFOS = xwpe.texi

xwpe_TEXINFOS = \
  version.texi \
  chapters/intro.texi \
  chapters/installation.texi \
  chapters/interface.texi \
  chapters/editor.texi \
  chapters/compiling.texi \
  chapters/debugging.texi \
  chapters/lsp.texi \
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
  sources/README.md \
  examples/README.md \
  examples/compile_error_test.c \
  examples/compile_error_test.cpp \
  examples/compile_error_test.f90 \
  examples/compile_error_test.pas \
  examples/compile_error_test.java \
  examples/compile_error_test.py \
  examples/compile_error_test.tex \
  examples/compile_error_test.a68 \
  examples/debug_test.c \
  examples/debug_test.cpp \
  examples/debug_test.f90 \
  examples/debug_test.pas \
  examples/debug_test.java \
  examples/debug_test.py \
  examples/debug_test.a68 \
  examples/debug_test.go \
  examples/debug_test.rs \
  examples/debug_test.scala \
  examples/debug_test_long_output.py \
  examples/headers_test.c \
  examples/headers_test.h \
  examples/fake.h \
  examples/disabled.h \
  examples/debug_constructor.c \
  examples/interactive_test.c \
  examples/scala-lsp/README.md \
  examples/scala-lsp/project.scala \
  examples/scala-lsp/shapes.scala \
  examples/scala-lsp/main.scala \
  examples/c-lsp/README.md \
  examples/c-lsp/Makefile \
  examples/c-lsp/c-lsp.prj \
  examples/c-lsp/compile_flags.txt \
  examples/c-lsp/main.cpp \
  examples/c-lsp/shapes.cpp \
  examples/c-lsp/shapes.hpp \
  examples/c-lsp/actions.cpp \
  examples/go-lsp/README.md \
  examples/go-lsp/go.mod \
  examples/go-lsp/main.go \
  examples/go-lsp/shapes.go \
  examples/go-lsp/actions.go \
  examples/python-lsp/README.md \
  examples/python-lsp/main.py \
  examples/python-lsp/shapes.py \
  examples/python-lsp/actions.py \
  examples/rust-lsp/README.md \
  examples/rust-lsp/Cargo.toml \
  examples/rust-lsp/Cargo.lock \
  examples/rust-lsp/src/main.rs \
  examples/rust-lsp/src/shapes.rs \
  examples/rust-lsp/src/actions.rs
