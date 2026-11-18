#!/bin/sh
# build-config-row.sh -- one build configuration is built and asked what it
# contains.
#
# configure probes Xft, Cairo and Pango and builds what it finds, so a machine
# that has some of them builds a program the default build never makes.  Each
# row of the table below is one such program: it is configured with the row's
# switches in a copy of the tree, built, and `make check` runs
# tests/check-build-config.sh on it with the words the row must have.  A row
# that stops compiling, or stops saying what it should, fails here and not on
# a user's machine.
#
#   build-config-row.sh [--srcdir DIR] [--jobs N] [--list] ROW...
#
# Rows (name: switches; words the binary must say):
#   x11-core     --without-xft --without-cairo --without-wayland
#                with-x=yes xft=no cairo=no pango=no with-wayland=no
#   x11-core-wl  --without-xft --with-wayland
#                with-x=yes xft=no cairo=yes pango=yes with-wayland=yes
#
# The copy is the files git tracks in DIR, with the content they have now, so
# edits that are not committed yet are what is built.  Untracked files are not
# copied (a large archive or another session's files may lie there), so a new
# file must be added to git first.  It lives in one directory made by mktemp under /var/tmp and is removed by its own
# name when the script ends.  This script is not in TESTS: it runs a whole
# configure and build, which `make check` must not nest.  Use
# `make check-build-configs`.
#
# Exit: 0 every row passed, 1 a row failed, 2 a bad call or no git tree.

SRCDIR=${srcdir:-.}
JOBS=${JOBS:-4}
LIST=0
ROWS=
BAD=1
USAGE=2
ROOT_PREFIX=/var/tmp/xwpe-row

# row_switches ROW - the configure switches of a row, or nothing when the row
# does not exist.
row_switches() {
  case $1 in
    x11-core)    echo "--without-xft --without-cairo --without-wayland" ;;
    x11-core-wl) echo "--without-xft --with-wayland" ;;
  esac
}

# row_expect ROW - the words the binary of the row must say.
row_expect() {
  case $1 in
    x11-core)    echo "with-x=yes xft=no cairo=no pango=no with-wayland=no" ;;
    x11-core-wl) echo "with-x=yes xft=no cairo=yes pango=yes with-wayland=yes" ;;
  esac
}

ALL_ROWS="x11-core x11-core-wl"

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --jobs) shift; JOBS=$1 ;;
    --list) LIST=1 ;;
    -*) echo "build-config-row: unknown argument $1" >&2; exit $USAGE ;;
    *) ROWS="$ROWS $1" ;;
  esac
  shift
done

if [ $LIST -eq 1 ]; then
  for r in $ALL_ROWS; do echo "$r: $(row_switches "$r") ; $(row_expect "$r")"; done
  exit 0
fi
[ -n "$ROWS" ] || ROWS=$ALL_ROWS
for r in $ROWS; do
  if [ -z "$(row_switches "$r")" ]; then
    echo "build-config-row: no row named $r (rows: $ALL_ROWS)" >&2
    exit $USAGE
  fi
done

if ! (cd "$SRCDIR" && git rev-parse --is-inside-work-tree >/dev/null 2>&1); then
  echo "build-config-row: $SRCDIR is not a git tree, so the files to copy are not known" >&2
  exit $USAGE
fi

DIR=$(mktemp -d "$ROOT_PREFIX.XXXXXX") || exit $USAGE
case $DIR in
  "$ROOT_PREFIX".?*) ;;
  *) echo "build-config-row: refusing to use $DIR" >&2; exit $USAGE ;;
esac
trap 'rm -rf -- "$DIR"' EXIT
trap 'exit 130' HUP INT TERM

# copy_tree DEST - the tree's files, listed by git, into DEST.
copy_tree() {
  mkdir -p "$1" || return 1
  (cd "$SRCDIR" && git ls-files -z --cached |
     tar --null --ignore-failed-read -T - -c -f -) | tar -x -f - -C "$1"
}

# run_row ROW - build one row in its own copy; 0 when it passed.
run_row() {
  row=$1
  work=$DIR/$row
  echo "build-config-row: $row: $(row_switches "$row")"
  copy_tree "$work" || { echo "build-config-row: $row: could not copy the tree"; return 1; }
  (
    cd "$work" || exit 1
    autoreconf -fi >"$work.autoreconf.log" 2>&1 || { echo "autoreconf failed:"; tail -n 20 "$work.autoreconf.log"; exit 1; }
    # shellcheck disable=SC2046
    ./configure $(row_switches "$row") >"$work.configure.log" 2>&1 || { echo "configure failed:"; tail -n 20 "$work.configure.log"; exit 1; }
    make -j"$JOBS" >"$work.make.log" 2>&1 || { echo "make failed:"; grep -E ' error: |Error [0-9]|undefined reference|multiple definition' "$work.make.log" | head -n 20; exit 1; }
    WPE_EXPECT_BUILT=$(row_expect "$row") make check TESTS=tests/check-build-config.sh >"$work.check.log" 2>&1 ||
      { echo "check-build-config failed:"; tail -n 30 "$work.check.log"; exit 1; }
    grep -E '^(PASS|FAIL|# (PASS|FAIL))' "$work.check.log" | head -n 5
  ) || { echo "build-config-row: $row: FAIL"; return 1; }
  echo "build-config-row: $row: ok"
}

failed=0
for r in $ROWS; do
  run_row "$r" || failed=1
done
exit $failed
