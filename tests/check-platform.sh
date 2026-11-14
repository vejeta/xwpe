#!/bin/sh
# check-platform.sh -- the feature macros have one owner, configure.
#
# _GNU_SOURCE, _DEFAULT_SOURCE, _XOPEN_SOURCE and _POSIX_C_SOURCE decide
# which declarations a system header shows.  configure writes the right ones
# into config.h (AC_USE_SYSTEM_EXTENSIONS), and config.h is force-included
# before every source file.  A file that sets one of its own overrides the
# system's choice on one platform and not on another: on macOS a strict
# _XOPEN_SOURCE hid RTLD_NEXT and the test did not compile.
#
#   check-platform.sh [--srcdir DIR] [--builddir DIR]
#
# Rows:
#  1. No product source, tool or test source defines or undefines a feature
#     macro (config.h is the owner's output; tests/console/vtprobe.c is a
#     keep: it is built outside automake, on purpose, and is not scanned).
#  2. The mutation: tests/test_write_owner.c against a config.h without the
#     feature macros does not compile (on glibc nftw is declared only at the
#     extended level), and against the real config.h it does.  So config.h is
#     what makes the file compile.  On glibc this proves the mechanism.  A C
#     library whose default is already the full level (macOS, the BSDs,
#     musl) compiles the mutant too: there the row says SKIP, and only a
#     macOS run proves the Darwin claim.
#
# Exit: 0 clean, 1 a row failed, 2 the script could not run.

SRCDIR=${srcdir:-.}
BUILDDIR=.
while [ $# -gt 0 ]; do
	case $1 in
	--srcdir) SRCDIR=$2; shift ;;
	--builddir) BUILDDIR=$2; shift ;;
	*) echo "check-platform: unknown option $1" >&2; exit 2 ;;
	esac
	shift
done

FAIL=0
fail() { echo "FAIL: $*"; FAIL=1; }

# Row 1: text.
HITS=$(cd "$SRCDIR" && grep -nE '^[[:space:]]*#[[:space:]]*(define|undef)[[:space:]]+_(GNU|DEFAULT|XOPEN|POSIX_C)_SOURCE' \
	./*.c ./*.h tools/*.c tools/*.h tests/*.c tests/*.h 2>/dev/null | grep -vE '^\./config\.h:')
if [ -n "$HITS" ]; then
	fail "a source sets a feature macro of its own (configure owns them):"
	echo "$HITS"
fi

# Row 2: mutation.
CONFIG="$BUILDDIR/config.h"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/platform.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
if [ ! -f "$CONFIG" ]; then
	echo "check-platform: no $CONFIG" >&2
	exit 2
fi
CC=${CC:-cc}
SRC="$SRCDIR/tests/test_write_owner.c"

mkdir "$WORK/mutant"
grep -vE '^[[:space:]]*#[[:space:]]*define[[:space:]]+_[A-Z0-9_]+_SOURCE|^[[:space:]]*#[[:space:]]*define[[:space:]]+__EXTENSIONS__|^[[:space:]]*#[[:space:]]*define[[:space:]]+_ALL_SOURCE|^[[:space:]]*#[[:space:]]*define[[:space:]]+_TANDEM_SOURCE|^[[:space:]]*#[[:space:]]*define[[:space:]]+_POSIX_PTHREAD_SEMANTICS' \
	"$CONFIG" > "$WORK/mutant/config.h"

# The library's include folders only: its -D flags may carry feature macros
# of their own (Debian's ncursesw.pc sets _XOPEN_SOURCE=600), which would
# hide what config.h does.
INCFLAGS=
for f in $NCURSESW_CFLAGS; do
	case $f in -I*) INCFLAGS="$INCFLAGS $f" ;; esac
done

compile() { # config.h, output file
	"$CC" -fsyntax-only -include "$1" -I"$SRCDIR" -I"$BUILDDIR" $INCFLAGS "$SRC" > "$2" 2>&1
}

if ! compile "$CONFIG" "$WORK/real.out"; then
	fail "tests/test_write_owner.c does not compile against the real config.h:"
	head -5 "$WORK/real.out"
fi
ROWS=2
printf '#include <limits.h>\n#ifndef __GLIBC__\n#error not glibc\n#endif\n' > "$WORK/glibc.c"
if compile "$WORK/mutant/config.h" "$WORK/mutant.out"; then
	if "$CC" -fsyntax-only "$WORK/glibc.c" > /dev/null 2>&1; then
		fail "tests/test_write_owner.c compiles without the feature macros in config.h: the row is blind"
	else
		echo "SKIP: row 2: this C library declares nftw at its default level, so the mutant compiles and proves nothing here"
		ROWS=1
	fi
elif ! grep -q 'nftw' "$WORK/mutant.out"; then
	fail "the mutant failed, but not at nftw:"
	head -5 "$WORK/mutant.out"
fi

if [ $FAIL -eq 0 ]; then
	echo "PASS: feature macros have one owner ($ROWS rows)"
fi
exit $FAIL
