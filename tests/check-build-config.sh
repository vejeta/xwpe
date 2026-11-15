#!/bin/sh
# check-build-config.sh -- what this build contains is said once, and every
# face of it agrees.
#
# configure decides each fact (--enable-ai, --with-x, libvterm, ...).  It
# reaches the rest of the tree in three faces: config.h (WPE_BUILT_* defined
# to 0 or 1), the binary (`we --version` prints "built with: word=yes ..."),
# and the feature lists of Makefile.am (which sources and which tests belong
# to a word).  The test scripts read only the binary's line (tests/build-
# config.sh).  This script fails when the faces disagree, so that no reader
# is told something configure did not decide.
#
#   check-build-config.sh [--srcdir DIR]
#
# Rules:
#  R5a every WPE_BUILT_* of config.h is a word of the binary's line, with
#      the same value (config.h carries the word in the comment above it);
#  R5b every word of WPE_FEATURE_SOURCES and WPE_FEATURE_TESTS is a word of
#      that line;
#  R5c config.h leaves no WPE_BUILT_* undefined;
#  R5d every source of a feature list is an object of this build exactly
#      when its word is yes;
#  R5e every WPE_BUILT_* a source, a header or a test names is defined, by
#      config.h or by a #define of the tree: a misspelt name in
#      "#if WPE_BUILT_AIX" is a silent 0, and -Wundef does not reach the
#      test programs;
#  R5f test_bindings, run beside the binary, prints its one SKIP line for
#      the assistant bars exactly when the build has no assistant, so the
#      log of a partial build cannot read like a full one.
#
# And one expectation: WPE_EXPECT_BUILT ("enable-ai=yes with-wayland=yes"),
# when set, must equal the binary's answer for each word it names.  A CI job
# that configures a feature sets it, so a library missing from the image
# makes the job fail instead of turning a capability into "not built here".
#
# Environment (set by `make check`): WPE_BUILD_BINARY, WPE_FEATURE_SOURCES,
# WPE_FEATURE_TESTS, ONE_OWNER_OBJECTS, srcdir.  Needs test_bindings built
# beside WPE_BUILD_BINARY (`make check` builds it before the tests run).
# Exit: 0 clean, 1 a disagreement, 99 a binary that cannot say what it has.

SRCDIR=${srcdir:-.}
HERE=$(dirname "$0")
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    *) echo "check-build-config: unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done

. "$HERE/build-config.sh"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/build-config.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
: > "$WORK/problems"
problem() { echo "$*" >> "$WORK/problems"; }

CONFIG_H=$(dirname "$wpe_bc_binary")/config.h
[ -f "$CONFIG_H" ] || problem "no config.h beside $wpe_bc_binary ($CONFIG_H)"

# --- R5a, R5c: config.h against the binary
if [ -f "$CONFIG_H" ]; then
  awk '
    /1 when this build has [a-z0-9-]+, 0 when it does not/ {
      w = $0; sub(/.*has /, "", w); sub(/,.*/, "", w); word = w; next
    }
    /^#define WPE_BUILT_[A-Z0-9_]+ / {
      if (word == "") { print "nodoc " $2; next }
      print word, $3; word = ""; next
    }
    /^\/\* #undef WPE_BUILT_/ { print "undef " $3; next }' "$CONFIG_H" > "$WORK/config-facts"
  while read -r cword cvalue; do
    case $cword in
      nodoc) problem "config.h defines $cvalue with no word above it (the line of --version cannot be compared)"; continue ;;
      undef) problem "config.h leaves $cvalue undefined: a fact is always 0 or 1"; continue ;;
    esac
    if wpe_built "$cword"; then binary=1; else binary=0; fi
    [ "$binary" = "$cvalue" ] ||
      problem "config.h says $cword is $cvalue, the binary says $binary"
  done < "$WORK/config-facts"
  config_n=$(wc -l < "$WORK/config-facts" | tr -d ' ')
  line_n=$(printf '%s\n' "$_wpe_features" | wc -w | tr -d ' ')
  [ "$config_n" = "$line_n" ] ||
    problem "config.h has $config_n WPE_BUILT_* facts, the binary's line has $line_n words"
fi

# --- R5b, R5d: the feature lists against the binary and the object list
[ -n "$WPE_FEATURE_SOURCES" ] || problem "WPE_FEATURE_SOURCES is not set (run under make check)"
printf '%s\n' "$WPE_FEATURE_SOURCES" | tr ';' '\n' > "$WORK/feature-sources"
while IFS=: read -r fword flist; do
  [ -n "$fword" ] || continue
  case " $_wpe_features " in
    *" $fword="*) ;;
    *) problem "WPE_FEATURE_SOURCES names $fword, which the binary's line does not have"; continue ;;
  esac
  for fsrc in $flist; do
    fobj=${fsrc%.c}.o
    if wpe_built "$fword"; then wanted=yes; else wanted=no; fi
    present=no
    for o in $ONE_OWNER_OBJECTS; do
      [ "$(basename "$o")" = "$fobj" ] && present=yes
    done
    [ "$present" = "$wanted" ] ||
      problem "$fsrc belongs to $fword=$wanted but $fobj is $( [ $present = yes ] && echo in || echo not in ) the objects of this build"
  done
done < "$WORK/feature-sources"
printf '%s\n' "$WPE_FEATURE_TESTS" | tr ';' '\n' > "$WORK/feature-tests"
while IFS=: read -r fword flist; do
  [ -n "$fword" ] || continue
  case " $_wpe_features " in
    *" $fword="*) ;;
    *) problem "WPE_FEATURE_TESTS names $fword, which the binary's line does not have" ;;
  esac
done < "$WORK/feature-tests"

# --- R5e: every WPE_BUILT_* name in the tree is one config.h defines
#     A name some source defines for itself (we_startcmd.h's
#     WPE_BUILT_STORE_NAME is a record-store constant, not a fact) is not a
#     misspelling: only a name defined nowhere is.
if [ -f "$CONFIG_H" ]; then
  (cd "$SRCDIR" && ls *.c *.h tests/*.c tests/*.h tools/*.c 2>/dev/null) > "$WORK/tree-files"
  {
    sed -n 's/^#[[:space:]]*define[[:space:]][[:space:]]*\(WPE_BUILT_[A-Z0-9_]*\).*/\1/p' "$CONFIG_H"
    while read -r f; do
      sed -n 's/^#[[:space:]]*define[[:space:]][[:space:]]*\(WPE_BUILT_[A-Z0-9_]*\).*/\1/p' "$SRCDIR/$f"
    done < "$WORK/tree-files"
  } | sort -u > "$WORK/defined-names"
  while read -r f; do
    grep -ho 'WPE_BUILT_[A-Z0-9_][A-Z0-9_]*' "$SRCDIR/$f" 2>/dev/null | sort -u |
      while read -r name; do
        grep -qx "$name" "$WORK/defined-names" ||
          echo "$f names $name, which nothing defines (a misspelt fact is a silent 0)" >> "$WORK/problems"
      done
  done < "$WORK/tree-files"
fi

# --- the expectation of the job that configured this build
if [ -n "$WPE_EXPECT_BUILT" ]; then
  for pair in $WPE_EXPECT_BUILT; do
    eword=${pair%%=*}
    evalue=${pair#*=}
    if wpe_built "$eword"; then actual=yes; else actual=no; fi
    [ "$actual" = "$evalue" ] ||
      problem "this job expects $eword=$evalue but the binary says $eword=$actual (a library missing from the image?)"
  done
fi

# --- R5f: test_bindings says what it could not check
#     A build without the assistant has no assistant bars; the program then
#     prints one SKIP line naming them, and a build with the assistant prints
#     none.  Without the line a log of a partial build reads like a full one.
check_bindings_skip_line() {
  bindings=$(dirname "$wpe_bc_binary")/test_bindings
  [ -x "$bindings" ] || { problem "no test_bindings beside $wpe_bc_binary: build it (make check builds it first)"; return; }
  "$bindings" > "$WORK/bindings.out" 2>&1
  if grep -q '^SKIP: eblst_ai, eblst_lspai bars: not built here: --enable-ai$' "$WORK/bindings.out"; then said=yes; else said=no; fi
  if wpe_built enable-ai; then wanted=no; else wanted=yes; fi
  [ "$said" = "$wanted" ] ||
    problem "test_bindings skip line for the assistant bars: said=$said, this build (enable-ai=$(wpe_built enable-ai && echo yes || echo no)) wants $wanted"
}
check_bindings_skip_line

if [ -s "$WORK/problems" ]; then
  echo "check-build-config: FAIL"
  cat "$WORK/problems"
  exit $BAD
fi

# What this build does not have, by name: the sources and tests that follow
# the words set to no.
for list in "$WORK/feature-sources" "$WORK/feature-tests"; do
  while IFS=: read -r fword flist; do
    [ -n "$fword" ] || continue
    wpe_built "$fword" && continue
    echo "not built here ($(wpe_option_of "$fword")): $flist"
  done < "$list"
done
echo "check-build-config: ok (the binary, config.h and the feature lists say the same $line_n things)"
exit 0
