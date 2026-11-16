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
#  3. tests/objsyms.sh reads the undefined and the defined symbols of every
#     system's nm (GNU "addr U name", "U name", OpenBSD's bare "name", llvm's
#     dashes, Mach-O's leading "_") alike.
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
#  8. No product source names an operating system, includes a header that
#     exists on only some systems, or tests a platform fact (WPE_HAS_*,
#     always 0 or 1) with #ifdef: we_platform.h owns those.
#  9. Every fact configure writes with WPE_FACT has a line in its summary.
# 10. The two regex facts (the GNU escapes, reading UTF-8) are measured by
#     running the engine, three-valued for the escapes, and the search owner
#     and its test read them.
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
. "$SRCDIR/tests/objsyms.sh"
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
# The defined side, from the lines each system's nm -g printed in the BSD CI
# runs (FreeBSD and NetBSD: 16 hex digits and a type letter; OpenBSD: 8 hex
# digits, weak retguard cookies among the definitions and an address-less
# undefined column), and the formats of GNU, llvm and Mach-O.  A definition
# is a line with a hex address and a type letter that is not U; a weak
# undefined symbol (w, v) and an llvm undefined line (dashes) are not.
printf '                 U fclose\n00000000000002c0 T wpe_paint_modal_getch\n0000000000000100 t wpe_paint_desk_set\n' > "$WORK/nmd-freebsd"
printf '         U fclose\n00000000 W __retguard_1122\n00000000 T wpe_paint_modal_getch\n' > "$WORK/nmd-openbsd"
printf '000000000000042d T wpe_paint_modal_getch\n                 U fopen\n' > "$WORK/nmd-netbsd"
printf '                 U poll\n                 w __gmon_start__\n                 v __weak_ref\n0000000000000000 T main\n0000000000000010 B counter@@VER_1\n0000000000000020 W wpe_paint_modal_getch\n' > "$WORK/nmd-gnu"
printf '0000000000000000 T main\n---------------- U poll\n---------------- w __gmon_start__\n0000000000000008 D wpe_paint_modal_getch\n' > "$WORK/nmd-llvm"
printf '\nwe_paint.o:\n0000000000000000 T _main\n                 U _poll\n0000000000000040 T _wpe_paint_modal_getch\n' > "$WORK/nmd-macho"
for fmt in freebsd openbsd netbsd gnu llvm macho; do
	nm_defined_names < "$WORK/nmd-$fmt" > "$WORK/nmd-$fmt.out"
	case $fmt in
	macho) want="main _main wpe_paint_modal_getch" ;;
	openbsd) want="__retguard_1122 wpe_paint_modal_getch" ;;
	freebsd) want="wpe_paint_modal_getch wpe_paint_desk_set" ;;
	gnu) want="main counter wpe_paint_modal_getch" ;;
	llvm) want="main wpe_paint_modal_getch" ;;
	*) want="wpe_paint_modal_getch" ;;
	esac
	for w in $want; do
		if ! grep -qx "$w" "$WORK/nmd-$fmt.out"; then
			fail "nm_defined_names gives no $w from the $fmt format of nm -g:"
			cat "$WORK/nmd-$fmt.out"
		fi
	done
	for u in fclose fopen poll _poll __gmon_start__ __weak_ref; do
		if grep -qx "$u" "$WORK/nmd-$fmt.out"; then
			fail "nm_defined_names reads the undefined $u as defined, in the $fmt format of nm -g"
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

# Row 8: no product source names an operating system, includes a header that
# exists on only some systems, or tests a platform fact with #ifdef.  They
# live in we_platform.h, the one place that may.
PRODUCT=$(cd "$SRCDIR" && ls ./*.c ./*.h tools/*.c tools/*.h 2>/dev/null | grep -v '^\./we_platform\.h$' | grep -v '^\./config\.h$')
OSHITS=$(cd "$SRCDIR" && grep -nE '__(linux|linux__|gnu_linux|APPLE|MACH|FreeBSD|NetBSD|OpenBSD|DragonFly|sun|osf|CYGWIN|GLIBC)__|_AIX' $PRODUCT 2>/dev/null)
if [ -n "$OSHITS" ]; then
	fail "a product source names an operating system (we_platform.h owns that):"
	echo "$OSHITS"
fi
HDRHITS=$(cd "$SRCDIR" && grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*<(linux/|sys/prctl\.h|sys/procctl\.h|sys/event\.h|sys/syscall\.h|pty\.h|util\.h|libutil\.h|execinfo\.h|sys/timerfd\.h|sys/epoll\.h|sys/inotify\.h|sys/sysmacros\.h)' $PRODUCT 2>/dev/null)
if [ -n "$HDRHITS" ]; then
	fail "a product source includes a header that exists on only some systems (we_platform.h owns that):"
	echo "$HDRHITS"
fi
IFDEFHITS=$(cd "$SRCDIR" && grep -nE '#[[:space:]]*(ifdef|ifndef)[[:space:]]+WPE_(HAS|REGEX)_|defined[[:space:]]*\(?[[:space:]]*WPE_(HAS|REGEX)_' $PRODUCT tests/*.c tests/*.h 2>/dev/null)
if [ -n "$IFDEFHITS" ]; then
	fail "a platform fact is tested with #ifdef; it is always defined, use #if:"
	echo "$IFDEFHITS"
fi
ROWS=$((ROWS + 1))

# Row 9: every fact configure writes into config.h (WPE_FACT) has a line in
# its summary, so a "no" always says what it turns off.
for fact in $(sed -n 's/.*WPE_FACT(\[\([A-Z_]*\)\].*/\1/p' "$SRCDIR/configure.ac"); do
	say=$(echo "$fact" | tr 'A-Z' 'a-z')
	case $fact in
	PARENT_DEATH_SIGNAL) say=pdeath ;;
	LINUX_CONSOLE) say=console ;;
	MEMFD_CREATE) say=memfd ;;
	REGEX_WORD_ESCAPES) say=regex ;;
	esac
	if ! grep -q "xwpe_say_$say" "$SRCDIR/configure.ac" ||
	   ! sed -n '/^AC_MSG_RESULT(\[$/,$p' "$SRCDIR/configure.ac" | grep -q "xwpe_say_$say"; then
		fail "configure's summary has no line for the fact WPE_HAS_$fact"
	fi
done
ROWS=$((ROWS + 1))

# Row 10: the two facts about the regular expression engine are measured by
# running it, never inferred from a declaration or a locale that exists, and
# the search owner and its test read them.  configure.ac must run \b \B \w \s
# and tell "understood", "refused" and "literal" apart (FreeBSD refuses them,
# OpenBSD takes them as letters), and must run "." on two bytes of UTF-8 to
# learn whether the engine reads characters (OpenBSD has a UTF-8 locale and
# a single-byte engine).  config.h carries each as 0 or 1.
GNUPROBE=$(sed -n '/AC_CACHE_CHECK(\[what regcomp does with the GNU escapes/,/^WPE_FACT(\[REGEX_GNU_ESCAPES\]/p' "$SRCDIR/configure.ac")
MBPROBE=$(sed -n '/AC_CACHE_CHECK(\[whether regcomp reads UTF-8 characters/,/^WPE_FACT(\[REGEX_MULTIBYTE\]/p' "$SRCDIR/configure.ac")
for word in understood refused literal; do
	case $GNUPROBE in
	*"\"$word\""*) ;;
	*) fail "configure's probe of the GNU escapes has no outcome \"$word\": it cannot tell the three answers apart" ;;
	esac
done
for esc in '\\\\bab' '\\\\Bab' '\\\\w' '\\\\s'; do
	case $GNUPROBE in
	*"$esc"*) ;;
	*) fail "configure's probe of the GNU escapes does not run the pattern spelt $esc in configure.ac" ;;
	esac
done
case $MBPROBE in
*regexec*rm_eo*) ;;
*) fail "configure's probe of WPE_HAS_REGEX_MULTIBYTE does not run the engine and read rm_eo" ;;
esac
for fact in REGEX_GNU_ESCAPES REGEX_MULTIBYTE REGEX_SAFE_BOUNDARY_GROUP REGEX_SAFE_BOUNDARY; do
	if ! grep -qE "^#define WPE_HAS_$fact [01]\$" "$CONFIG"; then
		fail "config.h does not define WPE_HAS_$fact as 0 or 1"
	fi
done
# The readers are looked for in the source with its comments taken out, so
# a comment that names a fact does not stand for reading it.  The owner and
# the test decide on the multibyte fact in an #if line; the test reads the
# escapes fact in plain code (an if), so it only has to be there.
strip_comments() { # file: C comments replaced by nothing (strings are not parsed)
	awk '
		{
			line = $0; out = ""
			while (length(line) > 0) {
				if (incom) {
					i = index(line, "*/")
					if (i == 0) { line = ""; break }
					line = substr(line, i + 2); incom = 0
				} else {
					a = index(line, "/*"); b = index(line, "//")
					if (b > 0 && (a == 0 || b < a)) { out = out substr(line, 1, b - 1); line = ""; break }
					if (a == 0) { out = out line; line = ""; break }
					out = out substr(line, 1, a - 1); line = substr(line, a + 2); incom = 1
				}
			}
			print out
		}' "$1"
}
blank_strings() { # file: the text of each "string literal" taken out, so a message that names a fact is no read of it
	awk '{ gsub(/"([^"\\]|\\.)*"/, "\"\""); print }' "$1"
}
# This is a structural guard: it sees that the fact is named in code, not
# that the code depends on it (an `#if 1 || WPE_HAS_REGEX_MULTIBYTE` would
# still pass).  The BSD runs are what show the rows take the right branch.
strip_comments "$SRCDIR/we_rowfind.c" | blank_strings /dev/stdin > "$WORK/rowfind.nocomment"
strip_comments "$SRCDIR/tests/test_rowfind.c" | blank_strings /dev/stdin > "$WORK/test_rowfind.nocomment"
if ! grep -qE '^[[:space:]]*#[[:space:]]*(if|elif).*WPE_HAS_REGEX_MULTIBYTE' "$WORK/rowfind.nocomment"; then
	fail "we_rowfind.c does not test WPE_HAS_REGEX_MULTIBYTE in an #if: it would take a locale existing for an engine that reads characters"
fi
if ! grep -qE '^[[:space:]]*#[[:space:]]*(if|elif).*WPE_HAS_REGEX_MULTIBYTE' "$WORK/test_rowfind.nocomment"; then
	fail "tests/test_rowfind.c does not test WPE_HAS_REGEX_MULTIBYTE in an #if: its rows would run where the engine cannot pass them"
fi
if ! grep -q 'WPE_HAS_REGEX_GNU_ESCAPES' "$WORK/test_rowfind.nocomment"; then
	fail "tests/test_rowfind.c does not read WPE_HAS_REGEX_GNU_ESCAPES outside a comment: a row would run where the engine cannot pass it"
fi
ROWS=$((ROWS + 1))

# Row 11: whether the engine survives a word-boundary escape beside an
# alternation is measured by running the class in a child of the probe, and
# the search owner reads the two facts.  NetBSD 10's regexec faults on
# `\B(a|b)[ab]` over a row with a multibyte character, so the probe must run
# those shapes (and their twins with \b, \< and the group before the escape),
# the editor's own 9-byte call, and controls outside the class, and must tell
# "crash", "range" and "hang" apart from "ok": a probe that cannot print a
# crash would call every system safe.  config.h carries both facts as 0 or 1
# (row 10's loop).  The decision is made once, in we_rowfind.h, from the two
# facts in an #if; the owner and the test read the decision, never the facts.
BNDPROBE=$(sed -n '/what regexec does with a word-boundary escape/,/^WPE_FACT(\[REGEX_SAFE_BOUNDARY_GROUP\]/p' "$SRCDIR/configure.ac")
if [ -z "$BNDPROBE" ]; then
	fail "configure.ac has no probe of what regexec does with a word-boundary escape"
fi
for word in crash range hang wrong; do
	case $BNDPROBE in
	*"return \"$word\""*) ;;
	*) fail "configure's probe of the word-boundary escapes never answers \"$word\" (no return \"$word\"): a crash of the engine would read as safe" ;;
	esac
done
for shape in '\\\\B(a|b)' '\\\\b(a|b)' '(a|b)\\\\B' '\\\\<(a|b)' '\\\\B(b)b' '"editor-call"' '\\\\>.' '\\\\<b' '[[:>:]].' '[[:<:]]b' '"gt-dot-1"' '"lt-b-1"'; do
	case $BNDPROBE in
	*"$shape"*) ;;
	*) fail "configure's probe of the word-boundary escapes does not run the shape spelt $shape in configure.ac" ;;
	esac
done
case $BNDPROBE in
*fork*waitpid*) ;;
*) fail "configure's probe of the word-boundary escapes does not run each shape in a child (fork, waitpid): a crash would end configure" ;;
esac
strip_comments "$SRCDIR/we_rowfind.h" | blank_strings /dev/stdin > "$WORK/we_rowfind_h.nocomment"
if ! grep -qE '^[[:space:]]*#[[:space:]]*(if|elif).*WPE_HAS_REGEX_SAFE_BOUNDARY_GROUP' "$WORK/we_rowfind_h.nocomment"; then
	fail "we_rowfind.h does not test WPE_HAS_REGEX_SAFE_BOUNDARY_GROUP in an #if: the owner would let an escape beside | reach an engine measured to fault"
fi
if ! grep -qE '^[[:space:]]*#[[:space:]]*(if|elif).*WPE_HAS_REGEX_SAFE_BOUNDARY([^_A-Z]|$)' "$WORK/we_rowfind_h.nocomment"; then
	fail "we_rowfind.h does not test WPE_HAS_REGEX_SAFE_BOUNDARY in an #if: the owner would let any word-boundary escape reach an engine measured to fault"
fi
if ! grep -q 'WPE_ROWFIND_BOUNDARY_GROUP_SAFE' "$WORK/test_rowfind.nocomment"; then
	fail "tests/test_rowfind.c does not read WPE_ROWFIND_BOUNDARY_GROUP_SAFE outside a comment: its rows would run where the owner refuses, or refuse where it runs"
fi
ROWS=$((ROWS + 1))

# Row 12: the probe of row 11 is run, not read.  Its C program is taken out
# of configure.ac, put through the shell as configure does (a here-document
# with $xwpe_regex_flag and $wpe_fact_REGEX_MULTIBYTE), built against a stub
# that runs the real engine and plants one fault, and its verdict is asserted:
#   no fault                          group=safe   boundary=safe
#   a crash on a control's row        group=unsafe boundary=unsafe
#   a wrong in-range answer           group=unsafe boundary=unsafe
#   a match outside the range         group=unsafe boundary=safe
#   a hang                            group=unsafe boundary=safe
#   an engine that reads bytes        group=safe   boundary=safe  (the multibyte
#       fact is 0, as on OpenBSD: the swap of a multibyte character for
#       letters changes the word classes there by design, so it is not compared)
# So a probe that stops counting "wrong", never reaches the compare, or
# compares where the engine reads bytes, turns this row red.
BNDPROG=$(printf '%s\n' "$BNDPROBE" | awk '
	/AC_LANG_PROGRAM\(\[\[/ { sub(/.*AC_LANG_PROGRAM\(\[\[/, ""); on = 1 }
	on == 0 { next }
	{
		line = $0
		if (on == 1 && (i = index(line, "]], [["))) {
			print substr(line, 1, i - 1); print "int main(void)"; print "{"
			line = substr(line, i + 6); on = 2
		}
		if (on == 2 && (j = index(line, "]])"))) { print substr(line, 1, j - 1); print "}"; exit }
		print line
	}')
bnd_probe_source() { # 1|0: the multibyte fact; the C source as configure would write it
	xwpe_regex_flag=0
	wpe_fact_REGEX_MULTIBYTE=$1
	eval "cat <<_BND_EOF
$BNDPROG
_BND_EOF"
}
cat > "$WORK/bndstub.h" <<'EOF'
#include <locale.h>
#include <regex.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
static int stub_regcomp(regex_t *re, const char *pat, int fl)
{
	const char *f = getenv("STUB_FAULT");
	if (f != NULL && strcmp(f, "byte") == 0) setlocale(LC_CTYPE, "C");
	return regcomp(re, pat, fl);
}
static int stub_regexec(const regex_t *re, const char *s, size_t n, regmatch_t *m, int fl)
{
	const char *f = getenv("STUB_FAULT");
	int rc;
	if (f == NULL) f = "none";
	if (strcmp(f, "byte") == 0) setlocale(LC_CTYPE, "C");
	rc = regexec(re, s, n, m, fl);
	if (strcmp(f, "crash") == 0 && s[0] == (char) 0xC3) raise(SIGSEGV);
	if (strcmp(f, "hang") == 0 && s[0] == 0x09) for (;;) ;
	if (strcmp(f, "range") == 0 && s[0] == 0x09) { m[0].rm_so = 0; m[0].rm_eo = 500; return 0; }
	if (strcmp(f, "wrong") == 0 && (unsigned char) s[1] >= 0x80) { m[0].rm_so = 3; m[0].rm_eo = 4; return 0; }
	return rc;
}
#define regcomp stub_regcomp
#define regexec stub_regexec
EOF
bnd_expect() { # name multibyte-fact fault want
	bnd_probe_source "$2" > "$WORK/bndprobe.c"
	if ! "$CC" -include "$WORK/bndstub.h" -o "$WORK/bndprobe" "$WORK/bndprobe.c" > "$WORK/bndprobe.log" 2>&1; then
		fail "row 12: the probe taken out of configure.ac does not build against the stub ($1): $(head -n 3 "$WORK/bndprobe.log" | tr '\n' ' ')"
		return
	fi
	got=$(STUB_FAULT=$3 "$WORK/bndprobe" 2>/dev/null | cut -d' ' -f1,2)
	case $got in
	"group=unknown boundary=unknown") echo "SKIP: row 12 ($1): no UTF-8 locale here to run the probe in"; return ;;
	esac
	if [ "$got" != "$4" ]; then
		fail "row 12: the probe says \"$got\" for $1, wanted \"$4\" (a probe that does not count it, or compares where it must not)"
	fi
}
case $BNDPROG in
*'compare_swapped = $wpe_fact_REGEX_MULTIBYTE;'*) ;;
*) fail "row 12: the probe does not start compare_swapped from the measured multibyte fact" ;;
esac
# The stub runs the host's real engine and adds one fault, so the cases only
# mean something where the host engine itself passes the probe: configure's
# measure of this host is in config.h.  Where it fails (FreeBSD and NetBSD
# crash on \>. and the group shapes, OpenBSD's single-byte engine answers
# "wrong" against the swapped row) the row says SKIP and runs no case that
# needs a clean host.  The byte case needs a host clean of the two safety
# facts only: it makes the stub read bytes and seeds the multibyte fact 0.
bnd_fact() { # name: the value of WPE_HAS_<name> in config.h, 0 when absent
	sed -n "s/^#define WPE_HAS_$1 \([01]\)\$/\1/p" "$CONFIG" | head -n 1
}
FACT_BOUNDARY=$(bnd_fact REGEX_SAFE_BOUNDARY)
FACT_GROUP=$(bnd_fact REGEX_SAFE_BOUNDARY_GROUP)
FACT_MULTIBYTE=$(bnd_fact REGEX_MULTIBYTE)
FACT_VALUES="WPE_HAS_REGEX_SAFE_BOUNDARY=${FACT_BOUNDARY:-0} WPE_HAS_REGEX_SAFE_BOUNDARY_GROUP=${FACT_GROUP:-0} WPE_HAS_REGEX_MULTIBYTE=${FACT_MULTIBYTE:-0}"
if [ "$FACT_BOUNDARY" = 1 ] && [ "$FACT_GROUP" = 1 ]; then
	HOST_CLEAN_BYTES=1
else
	HOST_CLEAN_BYTES=0
fi
if [ "$HOST_CLEAN_BYTES" = 1 ] && [ "$FACT_MULTIBYTE" = 1 ]; then
	HOST_CLEAN_CHARS=1
else
	HOST_CLEAN_CHARS=0
fi
if [ "$HOST_CLEAN_CHARS" = 1 ]; then
	bnd_expect "an engine with no fault" 1 none "group=safe boundary=safe"
	bnd_expect "a crash on a control" 1 crash "group=unsafe boundary=unsafe"
	bnd_expect "a wrong in-range answer" 1 wrong "group=unsafe boundary=unsafe"
	bnd_expect "a match outside the range" 1 range "group=unsafe boundary=safe"
	bnd_expect "a hang" 1 hang "group=unsafe boundary=safe"
else
	echo "SKIP: row 12: the host engine fails the probe or reads bytes ($FACT_VALUES)"
fi
if [ "$HOST_CLEAN_BYTES" = 1 ]; then
	bnd_expect "an engine that reads bytes" 0 byte "group=safe boundary=safe"
else
	echo "SKIP: row 12: the host engine fails the probe, no byte case ($FACT_VALUES)"
fi
# A row whose two halves both skipped proved nothing here: it is not counted.
if [ "$HOST_CLEAN_CHARS" = 1 ] || [ "$HOST_CLEAN_BYTES" = 1 ]; then
	ROWS=$((ROWS + 1))
fi

if [ $FAIL -eq 0 ]; then
	echo "PASS: platform decisions have one owner ($ROWS rows)"
fi
exit $FAIL
