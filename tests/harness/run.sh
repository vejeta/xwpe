#!/bin/sh
# run.sh -- run the tests mapped to one capability's spec scenarios and write a
# per-scenario verdict file.
#
# Usage:
#   tests/harness/run.sh <capability> [layer]
#     layer: ncurses (default) | ncurses-fkey | asan | x11 | wayland | c-unit
#
# The ncurses layer leaves out the tests that press a terminal function key and
# the ncurses-fkey layer is exactly those; tests/harness/FKEY.txt lists them and
# says why.  A hosted runner's pseudo-terminal does not deliver those keys, so
# the plain layer can gate a job while the function-key one stays visible.
#
# The capability's manifest (tests/harness/<capability>/manifest.json) names
# the test for every scenario; harness.py turns the manifest into the file list
# for the layer and the junit/automake results back into scenario verdicts.
# Output: $HARNESS_OUT/<capability>-<layer>-<timestamp>.{xml,json} (default
# tests/harness/out/).  Exit status is non-zero when any scenario is FAIL or
# MISSING, so the script can gate CI.
#
# Honours WPE_BIN / XWPE_BIN like the rest of the suite; the asan layer builds
# we-asan through tests/run-tests.sh --asan when it is missing, and runs it
# through the name wpe-asan: the editor turns its programming layer (the Run,
# Debug and AI menus, Alt-G) on only when the name it was started under begins
# with "wpe", so a sanitizer binary started as we-asan is a plain text editor
# and every programming-mode scenario fails for a reason that has nothing to
# do with memory.  Sanitizer builds run about twice as slow, so the waits are
# stretched with XWPE_TEST_WAIT_SCALE (2 unless the caller sets it).
set -u

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
cd "$top"

cap=${1:-}
layer=${2:-ncurses}
[ -n "$cap" ] || { echo "usage: tests/harness/run.sh <capability> [layer]" >&2; exit 2; }
[ -f "$here/$cap/manifest.json" ] || { echo "run.sh: no manifest for '$cap'" >&2; exit 2; }

# Same interpreter choice as tests/run-tests.sh: a system pyte/pytest if
# present, else the venv that run-tests.sh bootstraps under tests/.venv.
pick_python() {
  if python3 -c "import pyte, pytest" 2>/dev/null; then
    echo python3
  elif [ -x "$top/tests/.venv/bin/python" ]; then
    echo "$top/tests/.venv/bin/python"
  else
    echo "run.sh: neither system pyte/pytest nor tests/.venv found; run tests/run-tests.sh once" >&2
    exit 2
  fi
}
PY=${PY:-$(pick_python)}
out=${HARNESS_OUT:-$here/out}
mkdir -p "$out"
stamp=$(date +%Y%m%dT%H%M%S)
base="$out/$cap-$layer-$stamp"

files=$("$PY" "$here/harness.py" files "$cap" "$layer")

run_pytest() {
  # $1 = binary env assignment, remaining = files
  bin_env=$1; shift
  [ -n "$*" ] || { echo "run.sh: no tests mapped for $cap/$layer" >&2; return 0; }
  env "$bin_env" "$PY" -m pytest -q "$@" -rxX --junitxml="$base.xml"
}

case "$layer" in
  ncurses)
    # Skip the function-key tests that share a file with gating ones.
    deselect=""
    for t in $("$PY" "$here/harness.py" deselect "$cap"); do
      deselect="$deselect --deselect $t"
    done
    run_pytest "WPE_BIN=${WPE_BIN:-$top/wpe}" $files $deselect
    ;;
  ncurses-fkey)
    run_pytest "WPE_BIN=${WPE_BIN:-$top/wpe}" $files
    ;;
  asan)
    # Build the sanitizer binary when it is missing or older than a source
    # file: a stale we-asan tests the tree as it was, not as it is, and its
    # verdicts then contradict the ncurses layer's for no real reason.
    stale=""
    if [ -x "$top/we-asan" ]; then
      stale=$(find "$top" -maxdepth 1 \( -name '*.c' -o -name '*.h' \) \
                -newer "$top/we-asan" | head -n 1)
    fi
    if [ ! -x "$top/we-asan" ] || [ -n "$stale" ]; then
      tests/run-tests.sh --asan-build >/dev/null
    fi
    # The name decides the mode (see the header): expose the sanitizer binary
    # as wpe-asan and start it under that name.
    ln -sfn we-asan "$top/wpe-asan"
    export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:log_path=$out/asan-$cap"
    export XWPE_TEST_WAIT_SCALE="${XWPE_TEST_WAIT_SCALE:-2}"
    run_pytest "WPE_BIN=$top/wpe-asan" $files
    ;;
  x11|wayland)
    run_pytest "XWPE_BIN=${XWPE_BIN:-$top/xwpe}" $files
    ;;
  console)
    # A real Linux VT, via the micro-VM that tests/console/conftest.py boots.
    # No editor binary takes part: the layer runs the shipped console predicate
    # inside the VM and asserts on what it prints to the serial port.
    run_pytest "WPE_BIN=${WPE_BIN:-$top/wpe}" $files
    ;;
  c-unit)
    make check TESTS="$(echo "$files" | tr '\n' ' ')"
    ;;
  *)
    echo "run.sh: unknown layer '$layer'" >&2; exit 2
    ;;
esac
rc=$?

if [ "$layer" = c-unit ]; then
  "$PY" "$here/harness.py" report "$cap" "$layer" "$top" > "$base.json"
else
  [ -f "$base.xml" ] || { echo "run.sh: pytest wrote no junit file" >&2; exit "${rc:-1}"; }
  "$PY" "$here/harness.py" report "$cap" "$layer" "$base.xml" > "$base.json"
fi
report_rc=$?

echo "scenario verdicts: $base.json"
cat "$base.json"
[ "$rc" -eq 0 ] && [ "$report_rc" -eq 0 ]
