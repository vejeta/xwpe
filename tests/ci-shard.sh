#!/bin/sh
# ci-shard.sh -- run one job of the suite plan and write how it ended.
#
#   sh tests/ci-shard.sh K
#
# K is a shard index of plan/plan.json (tests/harness/suite_plan.py plan).  The
# shard runs the node ids of plan/args-K.txt and nothing else: the selection
# was applied once, by the build job's collection.  pytest is cut by
# tests/harness/job_cut.py after the plan's cut minutes: an interrupt to pytest
# alone, which makes it write the junit of every row it finished (a kill would
# leave none), then a kill of everything the run started when the interrupt
# did not end it within the grace.  The program that cuts is this tree's, the
# same on every image, so the cut never depends on the cut program a runner
# ships.  Its exit status is kept in shard-out/shard-K-status.json, which the
# gate reads, together with the tool's own report of the cut
# (shard-out/shard-K-cut.json): 124 for a cut, 137 for a cut that had to be
# killed.  The script ends with pytest's own status; the workflow step does not
# fail the job on it, the gate does.
#
# Environment (all with a default): PY (the suite's python), PLAN_DIR (plan),
# SHARD_OUT (shard-out), SHARD_CUT (the cut's duration, the plan's cut minutes
# by default; tests/test_ci_shard.py cuts a shard after seconds).
# Run from the tree's top, with the tree built.

set -u

SHARD=${1:?usage: ci-shard.sh SHARD}
PY=${PY:-/tmp/ptenv/bin/python}
PLAN_DIR=${PLAN_DIR:-plan}
OUT=${SHARD_OUT:-shard-out}
PLAN="$PY tests/harness/suite_plan.py"

mkdir -p "$OUT"
CUT=${SHARD_CUT:-$($PLAN cut-min)m}
WORKERS=$($PLAN workers)
ARGS="$PLAN_DIR/args-$SHARD.txt"
export XWPE_LIB="$PWD"
export WPE_BIN="$PWD/wpe"
export XWPE_UI_TRACE="$PWD/$OUT/shard-$SHARD-ui-trace.log"

START=$(date -u +%Y-%m-%dT%H:%M:%SZ)
rc=0
"$PY" tests/harness/job_cut.py --after "$CUT" --grace 120 \
  --report "$OUT/shard-$SHARD-cut.json" -- \
  "$PY" -m pytest -q -p no:cacheprovider \
  "@$ARGS" -n "$WORKERS" --reruns 2 --reruns-delay 3 \
  --rerun-except RowOverBudget -rf --durations=40 \
  --junitxml="$OUT/suite-junit-$SHARD.xml" || rc=$?
END=$(date -u +%Y-%m-%dT%H:%M:%SZ)

$PLAN status "$OUT/shard-$SHARD-status.json" --shard "$SHARD" --exit "$rc" \
  --started "$START" --ended "$END" --ids "$(wc -l < "$ARGS")" \
  --cut-report "$OUT/shard-$SHARD-cut.json"
exit "$rc"
