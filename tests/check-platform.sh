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
#  3. tests/nm-undefined.sh reads the undefined symbols of every system's nm
#     (GNU "addr U name", "U name", OpenBSD's bare "name") alike.
#  4. An undeclared function is an error on every compiler: Makefile.am does
#     not hide it, and the configured flag wins over a compiler (gcc 7) that
#     only warns by default.
#  5. The -Wno- flags configure chose are known to the compiler in use.  The
#     clang half of this row needs clang; without it the row says SKIP.
#  6. we_unix.c prints a file size as a long long (off_t is not a long on
#     32-bit systems; only macOS and OpenBSD saw it, through -Wformat).
#  7. A program that calls floor and ceil links with configure's LIBM where
#     it does not link without it (OpenBSD: libcairo's own dependency on
#     libm is not followed by the linker).
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

# Row 3: nm -u is read the same on every system's nm output.
. "$SRCDIR/tests/nm-undefined.sh"
printf '                 U poll\n                 U e_write@GLIBC_2.2.5\n0000000000000000 T main\n' > "$WORK/nm-gnu"
printf 'U poll\nU e_write@GLIBC_2.2.5\n' > "$WORK/nm-ucol"
printf 'poll\ne_write@GLIBC_2.2.5\n' > "$WORK/nm-bare"
printf '\nwe_fdloop.o:\n                 U poll\n                 U e_write@GLIBC_2.2.5\n' > "$WORK/nm-hdr"
printf 'poll\ne_write@GLIBC_2.2.5\n' > "$WORK/nm-want"
for fmt in gnu ucol bare hdr; do
	nm_undefined_names < "$WORK/nm-$fmt" > "$WORK/nm-$fmt.out"
	if ! cmp -s "$WORK/nm-$fmt.out" "$WORK/nm-want"; then
		fail "nm_undefined_names misreads the $fmt format of nm -u:"
		cat "$WORK/nm-$fmt.out"
	fi
done
# Mach-O, bare (cctools) and with a U column (llvm-nm): the source's name
# is among the names the scripts compare.
printf '_poll\n_e_write\n' > "$WORK/nm-macho"
printf '                 U _poll\n                 U _e_write\n' > "$WORK/nm-machou"
for fmt in macho machou; do
	nm_undefined_source_names < "$WORK/nm-$fmt" > "$WORK/nm-$fmt.out"
	for want in poll e_write; do
		if ! grep -qx "$want" "$WORK/nm-$fmt.out"; then
			fail "nm_undefined_source_names gives no $want from the $fmt format of nm -u:"
			cat "$WORK/nm-$fmt.out"
		fi
	done
done
ROWS=$((ROWS + 1))

# Row 4: an undeclared function is an error on every compiler, and the
# source says so in one place.
if grep -n -e '-Wno-implicit-function-declaration' "$SRCDIR/Makefile.am" > "$WORK/noimpl.out"; then
	fail "Makefile.am hides implicit declarations (-Wno-implicit-function-declaration):"
	cat "$WORK/noimpl.out"
fi
for v in AM_CFLAGS TEST_CFLAGS; do
	if ! awk -v v="$v" '
		$1 == v && $2 == "=" { on = 1 }
		on && /\$\(XWPE_DECLFLAG\)/ { found = 1 }
		on && !/\\$/ { on = 0 }
		END { exit !found }' "$SRCDIR/Makefile.am"; then
		fail "Makefile.am: $v does not carry \$(XWPE_DECLFLAG)"
	fi
done
DECLFLAG=$(sed -n 's/^XWPE_DECLFLAG = //p' "$BUILDDIR/Makefile")
printf 'int main(void) { return undeclared_call(1); }\n' > "$WORK/impl.c"
printf 'int x;\n' > "$WORK/empty.c"
if [ -z "$DECLFLAG" ] &&
   "$CC" -Werror -Werror=implicit-function-declaration -fsyntax-only "$WORK/empty.c" > /dev/null 2>&1; then
	fail "configure chose no declaration flag, but $CC accepts -Werror=implicit-function-declaration (the probe is broken)"
elif [ -z "$DECLFLAG" ]; then
	echo "SKIP: row 4: this compiler has no flag that makes an implicit declaration an error"
else
	# gcc 7 (NetBSD 9) only warns by default: -Wno-error= puts that default
	# back, and the configured flag has to win over it.
	if "$CC" -fsyntax-only -Wno-error=implicit-function-declaration "$WORK/impl.c" > /dev/null 2>&1 &&
	   ! "$CC" -fsyntax-only -Wno-error=implicit-function-declaration $DECLFLAG "$WORK/impl.c" > /dev/null 2>&1; then
		ROWS=$((ROWS + 1))
	elif ! "$CC" -fsyntax-only -Wno-error=implicit-function-declaration "$WORK/impl.c" > /dev/null 2>&1; then
		echo "SKIP: row 4: this compiler refuses an implicit declaration even when told not to; the flag cannot be shown to matter here"
	else
		fail "the configured flag $DECLFLAG does not make an undeclared call an error"
	fi
fi

# Row 5: the -Wno- flags configure chose are all known to the compiler, so a
# compile prints no "unknown warning option" (clang prints one per flag per
# file otherwise).
WARNFLAGS=$(sed -n 's/^XWPE_WARNFLAGS = //p' "$BUILDDIR/Makefile")
"$CC" -fsyntax-only $WARNFLAGS "$WORK/empty.c" > "$WORK/warn.out" 2>&1
if grep -qi 'unknown warning option\|unrecognized command.line option' "$WORK/warn.out"; then
	fail "$CC does not know the warning flags configure chose ($WARNFLAGS):"
	cat "$WORK/warn.out"
elif "$CC" --version 2>/dev/null | grep -qi clang; then
	ROWS=$((ROWS + 1))
else
	# gcc accepts an unknown -Wno- flag in silence, so only clang can fail here.
	echo "SKIP: row 5: $CC is not clang (the macOS and BSD runs are the proof)"
fi

# Row 6: off_t is printed as a long long (it is not a long on 32-bit systems).
if grep -n '%6ld' "$SRCDIR/we_unix.c" > "$WORK/offt.out"; then
	fail "we_unix.c prints a size with %ld:"
	cat "$WORK/offt.out"
else
	ROWS=$((ROWS + 1))
fi

# Row 7: where floor and ceil live in libm and a link does not find them
# without it, configure names libm (LIBM) and the link with it succeeds.
LIBM=$(sed -n 's/^LIBM = //p' "$BUILDDIR/Makefile")
printf '#include <math.h>\nvolatile double v = 1.5;\nint main(void) { return (int) floor(v) + (int) ceil(v); }\n' > "$WORK/libm.c"
if "$CC" -o "$WORK/libm-bare" "$WORK/libm.c" > /dev/null 2>&1; then
	echo "SKIP: row 7: floor and ceil link without -lm on this system, so only OpenBSD shows the need"
elif "$CC" -o "$WORK/libm-named" "$WORK/libm.c" $LIBM > "$WORK/libm.out" 2>&1; then
	ROWS=$((ROWS + 1))
else
	fail "floor and ceil need libm and configure's LIBM ($LIBM) does not provide it:"
	head -5 "$WORK/libm.out"
fi

if [ $FAIL -eq 0 ]; then
	echo "PASS: platform decisions have one owner ($ROWS rows)"
fi
exit $FAIL
