#!/bin/sh
# ci-shard.sh -- run one job of the suite plan and write how it ended.
#
#   sh tests/ci-shard.sh K
#
# K is a shard index of plan/plan.json (tests/harness/suite_plan.py plan).  The
# shard runs the node ids of plan/args-K.txt and nothing else: the selection
# was applied once, by the build job's collection, and the pytest options of
# the shard's layer are in that same file, so this script knows nothing about
# layers.  pytest is cut by tests/harness/job_cut.py after the shard's own cut
# minutes: an interrupt to pytest alone, which makes it write the junit of
# every row it finished (a kill would leave none), then a kill of everything
# the run started when the interrupt did not end it within the grace.  The
# program that cuts is this tree's, the same on every image, so the cut never
# depends on the cut program a runner ships.  Its exit status is kept in
# shard-out/shard-K-status.json, which the gate reads, together with the tool's
# own report of the cut (shard-out/shard-K-cut.json): 124 for a cut, 137 for a
# cut that had to be killed.  The script ends with pytest's own status; the
# workflow step does not fail the job on it, the gate does.
#
# A shard of a display layer keeps the log tree of its rows under --basetemp:
# every process's log, the session's Xvfb or weston log, and the crash file of
# an editor that died.  This script copies the regular files of that tree to
# shard-logs/logs-K/, a sibling of what the gate reads, so that the artifact of
# a shard's junit, status and trace holds no log tree and the gate, which
# downloads every shard's, downloads no screenshot.  A row plants link loops in
# its own folder on purpose (a folder operation must not follow one) and an
# upload that stats every entry it walks ends with ELOOP, holding nothing at
# all: find does not follow a link and -type f never names one, so the loops
# stay behind while every log, screenshot and crash file goes.  The names find
# gives are handed to the copy with a `./` in front of them and a `--` before
# them, so a log named `-x.log` is a file and not an option that loses the
# whole copy.
#
# Environment (all with a default): PY (the suite's python), PLAN_DIR (plan),
# SHARD_OUT (shard-out, what the gate reads), SHARD_LOGS_OUT (shard-logs, the
# copy of a display shard's log tree), SHARD_CUT (the cut's duration, the
# shard's own cut minutes by default; tests/test_ci_shard.py cuts a shard after
# seconds), SHARD_LOGS_ROOT (where a display shard's log tree and its runtime
# folder live: the runner's own temporary folder when it has one, a new one
# when it has not, and never the checkout, which a launch that names it as its
# working folder refuses).  A folder this script makes itself is removed again
# once the copy is done: nothing reads it after that, and a machine that runs a
# shard a day would otherwise keep the log tree of every one of them.
# XDG_RUNTIME_DIR is not an input: the shard always makes its own, 0700, under
# that folder, and never reuses an inherited one.  Run from the tree's top,
# with the tree built.

set -u

SHARD=${1:?usage: ci-shard.sh SHARD}
PY=${PY:-/tmp/ptenv/bin/python}
PLAN_DIR=${PLAN_DIR:-plan}
OUT=${SHARD_OUT:-shard-out}
LOGS_OUT=${SHARD_LOGS_OUT:-shard-logs}
PLAN="$PY tests/harness/suite_plan.py"
# A folder outside the checkout, whatever the machine: RUNNER_TEMP is set on a
# runner and nowhere else, and a basetemp of /suite-logs-K is not writable.
# SHARD_TEMP names the one this script made itself, and only that one is
# removed at the end: the runner removes its own, and a folder the caller
# named is the caller's.  This is no check script, so it does not make that
# folder with work_dir: a shard ends with pytest's own status, and the traps
# work_dir sets would end it with theirs.
SHARD_TEMP=""
SHARD_LOGS_ROOT=${SHARD_LOGS_ROOT:-${RUNNER_TEMP:-}}
if [ -z "$SHARD_LOGS_ROOT" ] && ! SHARD_TEMP=$(mktemp -d); then  # workdir: a shard is no check script
  echo "ci-shard.sh: cannot make a temporary folder of its own" >&2
  exit 1
fi
SHARD_LOGS_ROOT=${SHARD_LOGS_ROOT:-$SHARD_TEMP}
PLAN_JSON="$PLAN_DIR/plan.json"
ARGS="$PLAN_DIR/args-$SHARD.txt"

# A shard that cannot make the folders it writes to has nowhere to put the
# junit and the status the gate reads, so it says so and stops instead of
# running a suite whose result never reaches the gate.
if ! mkdir -p "$OUT" "$SHARD_LOGS_ROOT"; then
  echo "ci-shard.sh: cannot make the folders it writes to: $OUT $SHARD_LOGS_ROOT" >&2
  exit 1
fi
OUT=$(cd "$OUT" && pwd)
CUT_MIN=$($PLAN cut-min "$SHARD" --plan "$PLAN_JSON")
CUT=${SHARD_CUT:-${CUT_MIN}m}
BASETEMP=""
LOGS=""
if [ "$($PLAN logs "$SHARD" --plan "$PLAN_JSON")" = yes ]; then
  BASETEMP="$SHARD_LOGS_ROOT/suite-logs-$SHARD"
  LOGS="$LOGS_OUT/logs-$SHARD"
  if ! mkdir -p "$LOGS"; then
    echo "ci-shard.sh: cannot make the folder it copies its logs to: $LOGS" >&2
    exit 1
  fi
  LOGS=$(cd "$LOGS" && pwd)
fi

export XWPE_LIB="$PWD"
export WPE_BIN="$PWD/wpe"
export XWPE_BIN="$PWD/xwpe"
export XWPE_UI_TRACE="$OUT/shard-$SHARD-ui-trace.log"
# A compositor puts its socket in XDG_RUNTIME_DIR, so a display shard needs one:
# always this shard's own, 0700, under the folder above.  An inherited one
# belongs to whoever set it, the desktop or another job, and a shard that took
# it would put its socket where another run looks for its own and change the
# mode of a folder that is none of its business.  A shard that cannot make one
# says so and stops.
RUNTIME_DIR="$SHARD_LOGS_ROOT/runtime-$SHARD"
if ! mkdir -p "$RUNTIME_DIR" || ! chmod 700 "$RUNTIME_DIR"; then
  echo "ci-shard.sh: cannot make a private XDG_RUNTIME_DIR at $RUNTIME_DIR" >&2
  exit 1
fi
export XDG_RUNTIME_DIR="$RUNTIME_DIR"

START=$(date -u +%Y-%m-%dT%H:%M:%SZ)
rc=0
"$PY" tests/harness/job_cut.py --after "$CUT" --grace 120 \
  --report "$OUT/shard-$SHARD-cut.json" -- \
  "$PY" -m pytest -q -p no:cacheprovider \
  "@$ARGS" ${BASETEMP:+"--basetemp=$BASETEMP"} \
  --junitxml="$OUT/suite-junit-$SHARD.xml" || rc=$?
END=$(date -u +%Y-%m-%dT%H:%M:%SZ)

if [ -n "$LOGS" ] && [ -d "$BASETEMP" ]; then
  ( cd "$BASETEMP" && find . -type f -printf './%P\0' \
      | xargs -0 -r cp --parents -t "$LOGS" -- )
fi
if [ -n "$SHARD_TEMP" ]; then
  rm -rf "$SHARD_TEMP"
fi

$PLAN status "$OUT/shard-$SHARD-status.json" --shard "$SHARD" --exit "$rc" \
  --started "$START" --ended "$END" --ids "$(grep -c '::' "$ARGS")" \
  --cut-report "$OUT/shard-$SHARD-cut.json" --plan "$PLAN_JSON"
exit "$rc"
