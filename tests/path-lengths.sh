#!/bin/sh
# path-lengths.sh -- run the C tests with a TMPDIR of each given length.
#
# A test that passes at a short TMPDIR and fails at a long one depends on the
# length of its folder's path (a buffer sized for a short path, a Unix socket
# bound by its full name, a name cut in another folder).  The static rules of
# tests/check-test-paths.sh name the sites they can see; this run is what shows
# the rest.  `make check-path-lengths` calls it with 120 and 300, outside
# `make check` because it runs the whole C suite once per length.
#
#   path-lengths.sh [--make COMMAND] LENGTH...
#
# For each LENGTH the long base is made on disk under the build tree (the
# folder this script runs in), padded to exactly LENGTH bytes with components
# of at most 100 bytes, and `COMMAND check` runs with TMPDIR set to it.  A
# build tree whose own path is already longer than LENGTH runs at its own
# length, and the line says so.  The base is removed when the script ends.
# Exit: 0 when every run passed, 1 when one failed.

MAKE_COMMAND=${MAKE:-make}
LENGTHS=

while [ $# -gt 0 ]; do
  case $1 in
    --make) shift; MAKE_COMMAND=$1 ;;
    *) LENGTHS="$LENGTHS $1" ;;
  esac
  shift
done
if [ -z "$LENGTHS" ]; then
  echo "path-lengths: no length given" >&2
  exit 1
fi

. "$(dirname "$0")/workdir.sh"
WORK_BASE=$(pwd)
work_dir path-lengths 1

COMPONENT_MAX=100

# pad_to PATH LENGTH: PATH with components added until it is LENGTH bytes.
pad_to() {
  p=$1 want=$2
  while [ ${#p} -lt "$want" ]; do
    room=$((want - ${#p} - 1))
    if [ "$room" -le 0 ]; then
      break
    fi
    chunk=$room
    if [ "$chunk" -gt "$COMPONENT_MAX" ]; then
      chunk=$COMPONENT_MAX
      # a gap of one byte cannot be filled by a slash and a name
      [ $((room - chunk)) -eq 1 ] && chunk=$((chunk - 1))
    fi
    name=$(printf '%*s' "$chunk" '' | tr ' ' 'd')
    p=$p/$name
  done
  printf '%s\n' "$p"
}

status=0
for n in $LENGTHS; do
  base=$(pad_to "$WORK" "$n")
  mkdir -p "$base" || { echo "path-lengths: cannot make a base of $n bytes"; exit 1; }
  got=$(printf '%s' "$base" | wc -c | tr -d ' ')
  if [ "$got" != "$n" ]; then
    echo "path-lengths: the build tree is already $got bytes deep, so this run is at $got, not $n"
  fi
  echo "path-lengths: C tests with TMPDIR of $got bytes"
  if TMPDIR=$base $MAKE_COMMAND check; then
    echo "path-lengths: TMPDIR of $got bytes: ok"
  else
    echo "path-lengths: TMPDIR of $got bytes: FAIL"
    status=1
  fi
done
exit $status
