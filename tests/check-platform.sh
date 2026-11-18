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
# 11. The safety probe for the word-boundary escapes runs each shape in a
#     child and tells a crash, a hang, a wrong answer, a match outside the
#     range and macOS's refusal of a start inside a character apart from ok.
# 12. That probe is run, not read: against a stub engine with each fault, and
#     against the host's real engine, whose two words must be config.h's facts.
# 13. A Find at a start inside a row is judged by the character before it on
#     every engine: regexec has one caller, engine_exec two, configure
#     measures whether the engine keeps the text before a start, and the
#     nocontext and macsim builds are in the test lists.
# 14. The compiler is run as configure gave it, flags and all, only through
#     tests/compiler.sh: no test quotes the variable, and the owner is proved
#     on a CC with a flag, on CC=cc, and by two mutants that must fail.
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
. "$(dirname "$0")/workdir.sh"
work_dir platform 2
if [ ! -f "$CONFIG" ]; then
	echo "check-platform: no $CONFIG" >&2
	exit 2
fi
# The one owner of the compiler call (tests/compiler.sh); row 14 holds it.
HERE=$(dirname "$0")
. "$HERE/compiler.sh"
test_cc_require
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
	test_cc -fsyntax-only -include "$1" -I"$SRCDIR" -I"$BUILDDIR" $INCFLAGS "$SRC" > "$2" 2>&1
}

if ! compile "$CONFIG" "$WORK/real.out"; then
	fail "tests/test_write_owner.c does not compile against the real config.h:"
	head -5 "$WORK/real.out"
fi
ROWS=2
printf '#include <limits.h>\n#ifndef __GLIBC__\n#error not glibc\n#endif\n' > "$WORK/glibc.c"
if compile "$WORK/mutant/config.h" "$WORK/mutant.out"; then
	if test_cc -fsyntax-only "$WORK/glibc.c" > /dev/null 2>&1; then
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
   test_cc -Werror -Werror=implicit-function-declaration -fsyntax-only "$WORK/empty.c" > /dev/null 2>&1; then
	fail "configure chose no declaration flag, but $CC accepts -Werror=implicit-function-declaration (the probe is broken)"
elif [ -z "$DECLFLAG" ]; then
	echo "SKIP: row 4: this compiler has no flag that makes an implicit declaration an error"
else
	# gcc 7 (NetBSD 9) only warns by default: -Wno-error= puts that default
	# back, and the configured flag has to win over it.
	if test_cc -fsyntax-only -Wno-error=implicit-function-declaration "$WORK/impl.c" > /dev/null 2>&1 &&
	   ! test_cc -fsyntax-only -Wno-error=implicit-function-declaration $DECLFLAG "$WORK/impl.c" > /dev/null 2>&1; then
		ROWS=$((ROWS + 1))
	elif ! test_cc -fsyntax-only -Wno-error=implicit-function-declaration "$WORK/impl.c" > /dev/null 2>&1; then
		echo "SKIP: row 4: this compiler refuses an implicit declaration even when told not to; the flag cannot be shown to matter here"
	else
		fail "the configured flag $DECLFLAG does not make an undeclared call an error"
	fi
fi

# Row 5: the -Wno- flags configure chose are all known to the compiler, so a
# compile prints no "unknown warning option" (clang prints one per flag per
# file otherwise).
WARNFLAGS=$(sed -n 's/^XWPE_WARNFLAGS = //p' "$BUILDDIR/Makefile")
test_cc -fsyntax-only $WARNFLAGS "$WORK/empty.c" > "$WORK/warn.out" 2>&1
if grep -qi 'unknown warning option\|unrecognized command.line option' "$WORK/warn.out"; then
	fail "$CC does not know the warning flags configure chose ($WARNFLAGS):"
	cat "$WORK/warn.out"
elif test_cc --version 2>/dev/null | grep -qi clang; then
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
if test_cc -o "$WORK/libm-bare" "$WORK/libm.c" > /dev/null 2>&1; then
	echo "SKIP: row 7: floor and ceil link without -lm on this system, so only OpenBSD shows the need"
elif test_cc -o "$WORK/libm-named" "$WORK/libm.c" $LIBM > "$WORK/libm.out" 2>&1; then
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
			while (length(line) > 0) {	# linear: one source line, cut down piece by piece
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
if ! grep -q 'WPE_HAS_REGEX_WORD_ESCAPES' "$WORK/test_rowfind.nocomment"; then
	fail "tests/test_rowfind.c does not read WPE_HAS_REGEX_WORD_ESCAPES outside a comment: its word-boundary rows would run where the engine has no such escape"
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
for word in crash range hang wrong illseq; do
	case $BNDPROBE in
	*"return \"$word\""*) ;;
	*) fail "configure's probe of the word-boundary escapes never answers \"$word\" (no return \"$word\"): a crash of the engine would read as safe" ;;
	esac
done
for shape in '\\\\B(a|b)' '\\\\b(a|b)' '(a|b)\\\\B' '\\\\<(a|b)' '\\\\B(b)b' '"editor-call"' '\\\\>.' '\\\\<b' '[[:>:]].' '[[:<:]]b' '"gt-dot-1"' '"gt-dot-2"' '"lt-b-1"'; do
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
# with $xwpe_regex_flag and $wpe_fact_REGEX_MULTIBYTE), built against a complete
# fake engine (it calls no library function: it compiles every pattern, finds
# nothing, and plants one fault by the row it is given), and its verdict is
# asserted, the same on every host.  The host's own engine is not assumed
# clean: one more case builds the probe against the real regcomp and regexec
# and asserts that its two words are the two facts config.h carries, so the
# program extracted here is shown to be the one configure ran, on the host it
# ran on.
#   no fault                          group=safe   boundary=safe
#   every shape refused               group=safe   boundary=safe
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
bnd_probe_source() { # 1|0: the multibyte fact; [flag]: regcomp flag, default 0; the C source as configure would write it
	xwpe_regex_flag=${2:-0}
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
/* A fake engine: it never calls the library, so a case means the same on a
   host whose own engine fails the probe.  STUB_FAULT names the fault:
   refused (every pattern is refused), crash (a row that starts with U+00C3,
   the control of the probe), hang and range (the editor-call row, which
   starts with a tab), wrong (an answer that depends on the second byte being
   a multibyte one), illseq (a start inside a character is refused with
   REG_ILLSEQ, as macOS's regexec does) and illseq-all (every call is refused
   so).  Anything else is an engine that finds nothing. */
#ifndef REG_ILLSEQ
#define REG_ILLSEQ 17
#endif
static int stub_regcomp(regex_t *re, const char *pat, int fl)
{
	const char *f = getenv("STUB_FAULT");
	(void) re; (void) pat; (void) fl;
	if (f != NULL && strcmp(f, "refused") == 0) return REG_BADPAT;
	return 0;
}
static int stub_regexec(const regex_t *re, const char *s, size_t n, regmatch_t *m, int fl)
{
	const char *f = getenv("STUB_FAULT");
	(void) re; (void) n;
	if (f == NULL) f = "none";
	if (strcmp(f, "crash") == 0 && s[0] == (char) 0xC3) raise(SIGSEGV);
	if (strcmp(f, "hang") == 0 && s[0] == 0x09) for (;;) ;
	if (strcmp(f, "range") == 0 && s[0] == 0x09) { m[0].rm_so = 0; m[0].rm_eo = 500; return 0; }
	if (strcmp(f, "wrong") == 0 && (unsigned char) s[1] >= 0x80) { m[0].rm_so = 3; m[0].rm_eo = 4; return 0; }
	if (strcmp(f, "illseq-all") == 0) return REG_ILLSEQ;
	if (strcmp(f, "illseq") == 0 && (fl & REG_STARTEND) && m[0].rm_so > 0 &&
	    ((unsigned char) s[m[0].rm_so] & 0xC0) == 0x80) return REG_ILLSEQ;
	return REG_NOMATCH;
}
#define regcomp stub_regcomp
#define regexec stub_regexec
EOF
bnd_expect() { # name multibyte-fact fault want
	bnd_probe_source "$2" > "$WORK/bndprobe.c"
	if ! test_cc -include "$WORK/bndstub.h" -o "$WORK/bndprobe" "$WORK/bndprobe.c" > "$WORK/bndprobe.log" 2>&1; then
		fail "row 12: the probe taken out of configure.ac does not build against the stub ($1): $(head -n 3 "$WORK/bndprobe.log" | tr '\n' ' ')"
		return
	fi
	said=$(STUB_FAULT=$3 "$WORK/bndprobe" 2>/dev/null)
	# "unknown" is also an answer of the engine (a refused call), so only the
	# probe's own no-locale line is a SKIP.
	case $said in
	"group=unknown boundary=unknown no-utf8-locale") echo "SKIP: row 12 ($1): no UTF-8 locale here to run the probe in"; BND_SKIPPED=1; return ;;
	esac
	got=$(printf '%s\n' "$said" | cut -d' ' -f1,2)
	if [ "$got" != "$4" ]; then
		fail "row 12: the probe says \"$got\" for $1, wanted \"$4\" (a probe that does not count it, or compares where it must not)"
		return
	fi
	echo "ok: row 12 ($1): $got"
}
case $BNDPROG in
*'compare_swapped = $wpe_fact_REGEX_MULTIBYTE;'*) ;;
*) fail "row 12: the probe does not start compare_swapped from the measured multibyte fact" ;;
esac
bnd_fact() { # name: the value of WPE_HAS_<name> in config.h, 0 when absent
	sed -n "s/^#define WPE_HAS_$1 \([01]\)\$/\1/p" "$CONFIG" | head -n 1
}
bnd_fact_of_word() { # safe|unsafe|unknown: the fact configure makes of the word
	case $1 in
	safe) echo 1 ;;
	*) echo 0 ;;
	esac
}
# The host's real engine, with no stub: the probe's two words must be the
# facts config.h holds.  A host with no UTF-8 locale cannot run the probe.
bnd_real_engine() {
	# configure compiles the probe with the flag it measured for the word
	# escapes (REG_GNU on NetBSD, REG_ENHANCED on macOS), so this run does too.
	real_flag=$(sed -n 's/^#define WPE_REGEX_WORD_ESCAPES \(.*\)$/\1/p' "$CONFIG" | head -n 1)
	bnd_probe_source "$FACT_MULTIBYTE_SEED" "${real_flag:-0}" > "$WORK/bndreal.c"
	if ! test_cc -o "$WORK/bndreal" "$WORK/bndreal.c" > "$WORK/bndreal.log" 2>&1; then
		fail "row 12: the probe taken out of configure.ac does not build against the host's engine: $(head -n 3 "$WORK/bndreal.log" | tr '\n' ' ')"
		return
	fi
	said=$("$WORK/bndreal" 2>/dev/null)
	case $said in
	"group=unknown boundary=unknown no-utf8-locale") echo "SKIP: row 12 (the host's real engine): no UTF-8 locale here to run the probe in"; BND_SKIPPED=1; return ;;
	esac
	got=$(printf '%s\n' "$said" | cut -d' ' -f1,2)
	group_word=${got%% *}
	group_word=${group_word#group=}
	boundary_word=${got#* }
	boundary_word=${boundary_word#boundary=}
	want_group=$(bnd_fact REGEX_SAFE_BOUNDARY_GROUP)
	want_boundary=$(bnd_fact REGEX_SAFE_BOUNDARY)
	if [ "$(bnd_fact_of_word "$group_word")" != "${want_group:-0}" ] ||
	   [ "$(bnd_fact_of_word "$boundary_word")" != "${want_boundary:-0}" ]; then
		fail "row 12: the host's real engine gives \"$got\" but config.h holds WPE_HAS_REGEX_SAFE_BOUNDARY_GROUP=${want_group:-0} WPE_HAS_REGEX_SAFE_BOUNDARY=${want_boundary:-0}: the probe run here is not the one configure ran"
		return
	fi
	echo "ok: row 12 (the host's real engine, flag ${real_flag:-0}): $got; config.h holds WPE_HAS_REGEX_SAFE_BOUNDARY_GROUP=${want_group:-0} WPE_HAS_REGEX_SAFE_BOUNDARY=${want_boundary:-0}"
}
BND_SKIPPED=
FACT_MULTIBYTE_SEED=$(bnd_fact REGEX_MULTIBYTE)
FACT_MULTIBYTE_SEED=${FACT_MULTIBYTE_SEED:-0}
bnd_expect "an engine with no fault" 1 none "group=safe boundary=safe"
bnd_expect "an engine that refuses every shape" 1 refused "group=safe boundary=safe"
bnd_expect "a crash on a control" 1 crash "group=unsafe boundary=unsafe"
bnd_expect "a wrong in-range answer" 1 wrong "group=unsafe boundary=unsafe"
bnd_expect "a match outside the range" 1 range "group=unsafe boundary=safe"
bnd_expect "a hang" 1 hang "group=unsafe boundary=safe"
bnd_expect "an engine that reads bytes" 0 wrong "group=safe boundary=safe"
bnd_expect "a refusal of a start inside a character" 1 illseq "group=safe boundary=safe"
bnd_expect "a refusal of every call (REG_ILLSEQ)" 1 illseq-all "group=unknown boundary=unknown"
bnd_real_engine
# A row with a SKIP did not prove its claim here.
if [ -z "$BND_SKIPPED" ]; then
	ROWS=$((ROWS + 1))
fi

# Row 13: a Find at a start inside a row is judged by the character before
# it on every engine, and one place decides how.  An engine that takes the
# start of a call for the beginning of the text (macOS) is handed that
# character by the owner (the context expression), an engine that keeps the
# text is asked as it is, and configure measures which is which by running
# it.  The structure that keeps this true:
#  - regexec is called only inside engine_regexec, and engine_exec only by
#    engine_match_direct and engine_match_with_context, so no third path
#    reads a row without deciding what stands before the start;
#  - we_rowfind.h decides from WPE_HAS_REGEX_START_CONTEXT in an #if, and
#    neither the owner nor its test reads that name (they read the decision);
#  - configure runs the probe with REG_STARTEND and the word-escapes flag;
#  - test_rowfind_nocontext is in TESTS, no test keeps a look-behind
#    exception, and no text still says a start is "judged without" its
#    character;
#  - the macOS job expects the boundary facts configure must measure there.
calls_outside() { # file name allowed-functions: call lines of NAME( in a function not in the list
	awk -v name="$2" -v allowed=" $3 " '
		/^[^ \t{}#]/ && /\(/ && !/;/ { cur = $0; sub(/\(.*/, "", cur); n = split(cur, w, /[ *]+/); cur = w[n] }
		{
			line = $0
			if (line ~ /^static /) next
			pat = "(^|[^A-Za-z0-9_])" name "\\("
			if (line ~ pat && index(allowed, " " cur " ") == 0) print FILENAME ": " cur ": " $0
		}' "$1"
}
strip_comments "$SRCDIR/we_rowfind.c" | blank_strings /dev/stdin > "$WORK/rowfind.calls"
OUTSIDE=$(calls_outside "$WORK/rowfind.calls" regexec engine_regexec)
if [ -n "$OUTSIDE" ]; then
	fail "row 13: regexec is called outside engine_regexec in we_rowfind.c, a path that does not decide what stands before the start:"
	echo "$OUTSIDE"
fi
if ! grep -qE '(^|[^A-Za-z0-9_])regexec\(' "$WORK/rowfind.calls"; then
	fail "row 13: we_rowfind.c has no regexec call the rule could see (the check reads nothing)"
fi
OUTSIDE=$(calls_outside "$WORK/rowfind.calls" engine_exec "engine_match_direct engine_match_with_context")
if [ -n "$OUTSIDE" ]; then
	fail "row 13: engine_exec is called outside engine_match_direct and engine_match_with_context:"
	echo "$OUTSIDE"
fi
if ! grep -qE '^[[:space:]]*#[[:space:]]*(if|elif).*WPE_HAS_REGEX_START_CONTEXT' "$WORK/we_rowfind_h.nocomment"; then
	fail "row 13: we_rowfind.h does not test WPE_HAS_REGEX_START_CONTEXT in an #if: the owner would take every engine for one that keeps the text before a start"
fi
if grep -q 'WPE_HAS_REGEX_START_CONTEXT' "$WORK/rowfind.nocomment" "$WORK/test_rowfind.nocomment"; then
	fail "row 13: we_rowfind.c or tests/test_rowfind.c reads WPE_HAS_REGEX_START_CONTEXT: read the decision WPE_ROWFIND_START_CONTEXT"
fi
SCPROBE=$(sed -n '/whether regexec started inside a row sees the text before it/,/^WPE_FACT(\[REGEX_START_CONTEXT\]/p' "$SRCDIR/configure.ac")
case $SCPROBE in
*'regcomp(&re, pat, REG_EXTENDED | $xwpe_regex_flag)'*) ;;
*) fail "row 13: configure's start-context probe does not compile with the word-escapes flag (regcomp(&re, pat, REG_EXTENDED | \$xwpe_regex_flag))" ;;
esac
case $SCPROBE in
*'regexec(&re, row, 1, m, REG_STARTEND | REG_NOTBOL)'*) ;;
*) fail "row 13: configure's start-context probe does not call regexec with REG_STARTEND | REG_NOTBOL from a start inside the row" ;;
esac
case $SCPROBE in
*AC_RUN_IFELSE*) ;;
*) fail "row 13: configure's start-context probe does not run the engine (AC_RUN_IFELSE)" ;;
esac
for build in test_rowfind_nocontext test_rowfind_macsim; do
	LISTED=$(grep -E "^[^#]*[[:space:]]$build([[:space:]]|\$)" "$SRCDIR/Makefile.am" | grep -vE '_(SOURCES|CFLAGS|CPPFLAGS)[[:space:]]*=' | wc -l | tr -d ' ')
	if [ "$LISTED" -lt 2 ]; then
		fail "row 13: Makefile.am's check_PROGRAMS and TESTS do not both list $build (nocontext proves the owner hands an engine the character before a start against a reference with context; macsim runs the macOS path of the test, where the reference has none)"
	fi
done
if grep -qE 'COPY_DIFFERENCE_EXPECTED|OWNER_SEES_BEFORE|copy_differences' "$WORK/test_rowfind.nocomment"; then
	fail "row 13: tests/test_rowfind.c keeps a look-behind exception for the copy build (COPY_DIFFERENCE_EXPECTED, OWNER_SEES_BEFORE, copy_differences)"
fi
JUDGED=$(grep -n 'judged without' "$SRCDIR/we_rowfind.c" "$SRCDIR/we_rowfind.h" "$SRCDIR/Makefile.am" "$SRCDIR/tests/test_rowfind.c")
if [ -n "$JUDGED" ]; then
	fail "row 13: a text still says a start is \"judged without\" the character before it, which the owner now supplies:"
	echo "$JUDGED"
fi
if ! grep -q 'WPE_EXPECT_FACTS' "$SRCDIR/.github/workflows/macos.yml" 2>/dev/null ||
   ! grep -A3 'WPE_EXPECT_FACTS' "$SRCDIR/.github/workflows/macos.yml" | grep -q 'REGEX_SAFE_BOUNDARY=1'; then
	if [ -f "$SRCDIR/.github/workflows/macos.yml" ]; then
		fail "row 13: the macOS job does not expect REGEX_SAFE_BOUNDARY=1 (WPE_EXPECT_FACTS): a failed probe would turn the word escapes off without a red job"
	fi
fi
echo "ok: row 13"
ROWS=$((ROWS + 1))

# Row 14: the compiler is run as configure gave it, flags and all, and only
# through tests/compiler.sh.  macOS configures CC="gcc -std=gnu23"; a call
# that quotes the variable looks for a program of that whole name and fails
# "command not found", and a check that tests for the program first and
# skips passes having tested nothing.  Three parts:
#  a. text: no tests/*.sh quotes the compiler variable (CC, cc, MAINT_CC),
#     asks command -v about it, or assigns another variable from CC outside
#     compiler.sh; no tests/*.py reads CC without shlex.split.  Comment lines
#     are not read.  run-tests.sh and check-child-exec.sh use their compiler
#     variable unquoted, so they split it already and are not named.
#  b. unit: compiler.sh works with a CC that carries a flag, and with CC=cc
#     (through a shim, so it holds on a host where cc is a program): a
#     function named cc would call itself without end there.
#  c. text: compiler.sh defines no function named like a word CC can take
#     (cc, gcc, clang, c99, or the first word of the real CC, such as
#     gcc-14 or c89), seen as "w()", "w ()", "w ( )" and "function w", and
#     test_cc() is not flagged.  The text rules of part a are run on samples
#     of each form (quoted defaults, local, mid-line, Python single quotes,
#     getenv) and on harmless ones (CCACHE_DIR).  Never run: such a
#     function recurses without end, and a shell with no depth limit (macOS
#     /bin/sh) grows until the job is killed.  A copy of compiler.sh that
#     quotes the variable must fail the unit check (it ends at once).
#     Every run of the unit script is held by tests/with-deadline.sh.
# The patterns are built from pieces so this row does not match itself.
Q='"'
S="'"
D='$'
# A quoted use: "$CC", "${CC}", "${CC:-cc}", "${CC-cc}".  An empty default
# ("${MAINT_CC:-}") only asks whether the variable is set, and is not one.
P_QUOTED="${Q}"'\$(\{(CC|cc|MAINT_CC)(:?[-=+?][^}]+)?\}|(CC|cc|MAINT_CC))'"${Q}"
P_COMMAND_V='command -v "?\$\{?(CC|cc)'
# Another variable set from CC: at the start of a line (after local, export,
# readonly or declare) or after ; & | ( on the same line.  CCACHE_DIR is not CC.
P_FROM_CC='[A-Za-z_][A-Za-z0-9_]*=\$\{?(CC|MAINT_CC)([^A-Za-z0-9_]|$)'
P_ASSIGN="(^|[;&|({]|(^|[[:space:]])(then|do|else|if|while|until|!))[[:space:]]*((local|export|readonly|declare)[[:space:]]+)?${P_FROM_CC}"
P_COMMENT='^[^:]*:[0-9]+:[[:space:]]*#'
P_PY='(environ(\.get\(|\[)|getenv\()[[:space:]]*['"${Q}${S}"'](CC|MAINT_CC)['"${Q}${S}"']'
COMPILER_OFFENDERS=
for f in "$SRCDIR"/tests/*.sh; do
	PAT="$P_QUOTED|$P_COMMAND_V"
	if [ "${f##*/}" != compiler.sh ]; then
		PAT="$PAT|$P_ASSIGN"
	fi
	COMPILER_OFFENDERS="$COMPILER_OFFENDERS$(grep -nHE "$PAT" "$f" | grep -vE "$P_COMMENT")"
done
if [ -n "$COMPILER_OFFENDERS" ]; then
	fail "row 14: a test script handles the compiler variable itself instead of test_cc:"
	echo "$COMPILER_OFFENDERS"
fi
# The text rules are run on samples, with the same pattern variables: each
# form must be seen, and the harmless ones must not be.  The samples are made
# from pieces so this file does not contain them as text.
# text_sees PATTERN TEXT - status 0 when PATTERN matches TEXT.
text_sees() {
	printf '%s\n' "$2" | grep -qE "$1"
}
TEXT_BLIND=0
for sample in \
	"X=${D}{CC:-cc}" \
	"${Q}${D}CC${Q} -c x.c" \
	"${Q}${D}{CC}${Q} -c x.c" \
	"${Q}${D}{CC:-cc}${Q} -c x.c" \
	"${Q}${D}{CC-cc}${Q} -c x.c" \
	"local cc=${D}CC" \
	"export cc=${D}CC" \
	"if true; then c=${D}CC; fi" \
	"a=1; b=${D}{CC}" \
	"true && c=${D}CC" \
	"if c=${D}CC; then :; fi" \
	"while c=${D}CC; do :; done" \
	"until c=${D}CC; do :; done" \
	"! c=${D}CC" \
	"command -v ${D}CC"; do
	if ! text_sees "$P_QUOTED|$P_COMMAND_V|$P_ASSIGN" "$sample"; then
		fail "row 14: the text rule does not see \"$sample\": the row is blind"
		TEXT_BLIND=1
	fi
done
for sample in \
	"X=${D}CCACHE_DIR" \
	"X=${D}{CCACHE_DIR:-x}" \
	"${Q}${D}CCACHE${Q}" \
	"[ -z ${Q}${D}{MAINT_CC:-}${Q} ]" \
	"test_cc -c x.c" \
	"${Q}${D}XCC${Q}"; do
	if text_sees "$P_QUOTED|$P_COMMAND_V|$P_ASSIGN" "$sample"; then
		fail "row 14: the text rule flags \"$sample\", which is not the compiler variable"
		TEXT_BLIND=1
	fi
done
for sample in \
	"os.environ.get(${S}CC${S})" \
	"os.environ.get(${Q}CC${Q})" \
	"os.environ.get(${Q}CC${Q}, ${Q}cc${Q})" \
	"os.environ[${S}CC${S}]" \
	"os.environ[${Q}CC${Q}]" \
	"os.getenv(${Q}CC${Q})" \
	"os.environ.get(${Q}MAINT_CC${Q})" \
	"os.getenv(${S}CC${S}, ${Q}cc${Q})"; do
	if ! text_sees "$P_PY" "$sample"; then
		fail "row 14: the Python rule does not see \"$sample\": the row is blind"
		TEXT_BLIND=1
	fi
done
for sample in "os.environ.get(${Q}CCACHE_DIR${Q})" "os.getenv(${Q}CXX${Q})"; do
	if text_sees "$P_PY" "$sample"; then
		fail "row 14: the Python rule flags \"$sample\", which is not CC"
		TEXT_BLIND=1
	fi
done
if [ $TEXT_BLIND -eq 0 ]; then
	echo "ok: row 14 (the text rules see every form and miss the harmless ones)"
fi
PY_OFFENDERS=$(grep -nHE "$P_PY" "$SRCDIR"/tests/*.py | grep -v 'shlex\.split')
if [ -n "$PY_OFFENDERS" ]; then
	fail "row 14: a test reads CC without shlex.split, so a CC with flags is not found:"
	echo "$PY_OFFENDERS"
fi

# The unit script: FILE sourced with CC given, both calls must succeed.  It
# runs as its own program under tests/with-deadline.sh, so a compiler that
# waits for input or a loop that does not end is a named FAIL after
# UNIT_LIMIT seconds, not a job killed after ten minutes.
UNIT_LIMIT=20
cat > "$WORK/unit.sh" <<'UNIT'
#!/bin/sh
ulimit -c 0 2> /dev/null
CC=$2
. "$1"
test_cc_require && test_cc --version && test_cc -c -o /dev/null "$3"
UNIT
# owner_unit FILE CC - status 0 when the unit script passes, 124 on timeout.
owner_unit() {
	"$HERE/with-deadline.sh" "$UNIT_LIMIT" sh "$WORK/unit.sh" "$1" "$2" "$WORK/empty.c" < /dev/null > /dev/null 2>&1
}
# owner_unit_as_cc FILE - the same with CC=cc, where cc is a shim for the
# configured compiler first in PATH.
owner_unit_as_cc() {
	(
		PATH="$WORK/shim:$PATH"
		owner_unit "$1" cc
	)
}
# unit_fail TEXT STATUS - the FAIL line, naming a timeout as one.
unit_fail() {
	if [ "$2" -eq 124 ]; then
		fail "row 14: $1: no end after $UNIT_LIMIT s"
	else
		fail "row 14: $1"
	fi
}
# compiler_words CCSTRING - the words CC can take, as an ERE alternation: the
# fixed list and the basename of the first word of CCSTRING (gcc-14, clang-18,
# c89, a full path), each character that is not a letter, digit or _ in a
# bracket so it is read literally.
compiler_words() {
	if [ $# -gt 0 ]; then
		set -- $1
	else
		set -- $CC
	fi
	first=${1##*/}
	literal=$(printf '%s' "$first" | sed 's/[^A-Za-z0-9_]/[&]/g')
	if [ -n "$literal" ]; then
		echo "cc|gcc|clang|c99|$literal"
	else
		echo "cc|gcc|clang|c99"
	fi
}
# owner_defines_word FILE [CCSTRING] - status 0 when FILE defines a function
# named like a word CC can take (the fixed list and CCSTRING's first word):
# "cc()", "cc ()", "cc ( )" or "function cc".  Such a function would be called
# by "$CC" when CC is that word, forever.
owner_defines_word() {
	if [ $# -gt 1 ]; then
		words=$(compiler_words "$2")
	else
		words=$(compiler_words)
	fi
	grep -qE "^[[:space:]]*(function[[:space:]]+($words)([[:space:]]|\\(|\\{|\$)|($words)[[:space:]]*\\([[:space:]]*\\))" "$1"
}
mkdir "$WORK/shim"
set -- $CC
REAL_CC_PATH=$(command -v "$1")
shift
printf '#!/bin/sh\nexec %s %s "$@" < /dev/null\n' "$REAL_CC_PATH" "$*" > "$WORK/shim/cc"
chmod +x "$WORK/shim/cc"
printf 'int x;\n' > "$WORK/empty.c"
OWNER="$HERE/compiler.sh"
if [ ! -f "$OWNER" ]; then
	fail "row 14: $OWNER is missing"
else
	owner_unit "$OWNER" "$CC -O0"
	rc=$?
	if [ $rc -ne 0 ]; then
		unit_fail "tests/compiler.sh does not run a CC that carries a flag (CC=\"$CC -O0\")" $rc
	else
		echo "ok: row 14 (a CC with a flag)"
	fi
	owner_unit_as_cc "$OWNER"
	rc=$?
	if [ $rc -ne 0 ]; then
		unit_fail "tests/compiler.sh does not run with CC=cc" $rc
	else
		echo "ok: row 14 (CC=cc)"
	fi
	if owner_defines_word "$OWNER"; then
		fail "row 14: tests/compiler.sh defines a function named like a compiler word (cc, gcc, clang, c99, or the first word of CC: $CC): it would call itself when CC is that word"
	else
		echo "ok: row 14 (no function named like a compiler)"
	fi
	# The static check must see each spelling, for the fixed words and for the
	# first word of a CC outside the list, and must not see test_cc().  The
	# mutants are only read, never run: such a function does not end.
	MUTANT_BLIND=0
	for pair in 'cc|cc() {' 'cc|cc () {' 'cc|cc ( ) {' 'cc|function cc {' \
		'gcc-14 -std=gnu23|gcc-14() {' 'clang-18|clang-18 ( ) {' \
		'/usr/bin/c89|function c89 {' 'c89|c89 () {' 'x86_64-linux-gnu-gcc|x86_64-linux-gnu-gcc() {' \
		'g++|function g++ {'; do
		mutant_cc=${pair%%|*}
		spelling=${pair#*|}
		printf '%s\n\t$CC "$@"\n}\n' "$spelling" > "$WORK/mutant-function-cc.sh"
		if ! owner_defines_word "$WORK/mutant-function-cc.sh" "$mutant_cc"; then
			fail "row 14: the check for a function named like CC (CC=$mutant_cc) does not see \"$spelling\": the row is blind"
			MUTANT_BLIND=1
		fi
	done
	for spelling in 'test_cc() {' 'test_cc ( ) {' 'mycc() {' 'function test_cc {'; do
		printf '%s\n\t$CC "$@"\n}\n' "$spelling" > "$WORK/mutant-function-cc.sh"
		if owner_defines_word "$WORK/mutant-function-cc.sh" "gcc-14 -O2"; then
			fail "row 14: the check for a function named like CC flags \"$spelling\", which is not a compiler word"
			MUTANT_BLIND=1
		fi
	done
	if [ $MUTANT_BLIND -eq 0 ]; then
		echo "ok: row 14 (the function-name check sees cc(), cc (), cc ( ), function cc and the first word of CC, and not test_cc())"
	fi
	MUT_CALL='\$CC "\$@"'
	MUT_QUOTED="${Q}\$CC${Q} ${Q}\$@${Q}"
	sed "s/$MUT_CALL/$MUT_QUOTED/" "$OWNER" > "$WORK/mutant-quoted.sh"
	if cmp -s "$OWNER" "$WORK/mutant-quoted.sh"; then
		fail "row 14: the quoting mutant is the owner unchanged: the row is blind"
	else
		owner_unit "$WORK/mutant-quoted.sh" "$CC -O0"
		rc=$?
		if [ $rc -eq 0 ]; then
			fail "row 14: a compiler.sh that quotes the variable passes the unit check: the row is blind"
		elif [ $rc -eq 124 ]; then
			fail "row 14: the quoting mutant did not end after $UNIT_LIMIT s"
		else
			echo "ok: row 14 (the quoting mutant fails the unit check)"
		fi
	fi
fi
ROWS=$((ROWS + 1))

# ROWS counts the rows that proved their claim; a SKIP line leaves its row
# out.  The total is the "# Row N:" comments of this file, so it cannot drift.
ROWS_TOTAL=$(grep -c '^# Row [0-9][0-9]*:' "$0")
if [ $FAIL -eq 0 ]; then
	echo "PASS: platform decisions have one owner ($ROWS_TOTAL rows, $((ROWS_TOTAL - ROWS)) skipped here)"
fi
exit $FAIL
