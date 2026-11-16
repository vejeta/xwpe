#!/bin/sh
# check-with-deadline.sh -- tests/with-deadline.sh ends a command that hangs.
#
# Four rows, run under /bin/sh (dash on Debian, ksh on OpenBSD): the script
# must not need bash.
#
#   1. A command that dies on TERM, under a 1 s deadline: exit 124, the
#      TIMEOUT line, the command gone, and the script returns at about the
#      deadline.  It must NOT wait the pause that separates TERM from KILL:
#      that pause belongs to a command that ignores TERM (row 2).
#   2. A command that ignores TERM: exit 124, gone (KILL ended it), and the
#      script returns at the deadline plus the pause.
#   3. A command that ends by itself keeps its own exit status.
#   4. The script holds no pipe of the caller open after it returns: a
#      reader of its output sees the end of the stream at once.
#
# The bounds follow from the arithmetic of the script, and are not tuned:
#
#   DEADLINE = 1 s, GRACE = the pause between TERM and KILL.
#   Row 1 returns at DEADLINE + SLACK, SLACK being what a loaded machine
#   adds to start and end two processes.  SLACK = 1 s times the scale a slow
#   machine sets for the test suite (XWPE_TEST_WAIT_SCALE, a float; the macOS
#   runner took 4 s at 1).
#   A script that waits its pause before it returns would take
#   DEADLINE + GRACE.  The row tells the two apart only while
#   GRACE > SLACK + 2 * GRAIN, so GRACE is set from SLACK and the clock's
#   grain (see below): the smallest whole number of seconds above that sum
#   (with-deadline.sh reads it from WITH_DEADLINE_GRACE).
#   Row 2 returns at DEADLINE + GRACE + SLACK, and not before DEADLINE +
#   GRACE: KILL does not come early.
#
# The clock counts milliseconds where date(1) can (GNU date: %N).  Where it
# cannot (BSD date prints a literal N), it counts whole seconds, and every
# bound then allows one second more, since a start and an end read in whole
# seconds can differ from the true span by up to one second.
#
# @scenario SC-child-wait-08
#
# Exit: 0 pass, 1 a row failed.

SRCDIR=${srcdir:-.}
SCRIPT=$SRCDIR/tests/with-deadline.sh
[ -f "$SCRIPT" ] || SCRIPT=$SRCDIR/with-deadline.sh
BAD=0
DIR=$(mktemp -d "${TMPDIR:-/tmp}/check-with-deadline.XXXXXX") || exit 1
trap 'rm -rf "$DIR"' EXIT

fail() { echo "FAIL: $1"; BAD=1; }

# The clock's grain, in milliseconds: 0 where date prints nanoseconds, 1000
# where it counts whole seconds only.
case $(date +%s%N 2>/dev/null) in
*[!0-9]* | "" | ??????????) GRAIN_MS=1000 ;;
*) GRAIN_MS=0 ;;
esac

# The bounds, in milliseconds, from the scale and the grain.
# SLACK_MS = 1000 * scale.  GRACE is the whole seconds that keep a script
# waiting its pause (DEADLINE + GRACE - GRAIN, the shortest it can read)
# above the slowest honest return (DEADLINE + SLACK + GRAIN): GRACE >
# SLACK + 2 * GRAIN: the smallest whole number of seconds that does.
set -- $(awk -v s="${XWPE_TEST_WAIT_SCALE:-1}" -v grain="$GRAIN_MS" 'BEGIN {
	if (s + 0 <= 0 || s + 0 > 1000) s = 1
	slack = int(1000 * s + 0.999)
	g = int((slack + 2 * grain) / 1000) + 1
	printf "%d %d", slack, g
}')
SLACK_MS=$1
GRACE_S=$2
DEADLINE_MS=1000
GRACE_MS=$((GRACE_S * 1000))

# now_ms - the clock in milliseconds, or in whole seconds times 1000 where
# the system has no finer clock.
now_ms() {
	if [ "$GRAIN_MS" -eq 0 ]; then
		ns=$(date +%s%N)
		echo $((ns / 1000000))
	else
		echo $(($(date +%s) * 1000))
	fi
}

# timed_run PIDFILE COMMAND... - run with-deadline.sh with a 1 s deadline on
# COMMAND; sets OUT, RC and ELAPSED_MS.
timed_run() {
	pidfile=$1
	shift
	START=$(now_ms)
	OUT=$(WITH_DEADLINE_GRACE=$GRACE_S /bin/sh "$SCRIPT" 1 /bin/sh -c "$@")
	RC=$?
	END=$(now_ms)
	ELAPSED_MS=$((END - START))
}

# command_gone ROW PIDFILE - the command's PID, written by the command
# itself, no longer names a running process.
command_gone() {
	if [ -s "$2" ]; then
		PID=$(cat "$2")
		if kill -0 "$PID" 2>/dev/null; then
			fail "$1: the command (pid $PID) is still running"
			kill -KILL "$PID" 2>/dev/null
		fi
	else
		fail "$1: the command never started"
	fi
}

# check_timeout ROW - exit 124 and the TIMEOUT line, for the last timed_run.
check_timeout() {
	[ "$RC" -eq 124 ] || fail "$1: exit status $RC, wanted 124"
	case $OUT in
	*"TIMEOUT: "*" after 1 s"*) ;;
	*) fail "$1: no TIMEOUT line in: $OUT" ;;
	esac
}

# check_between ROW LOW HIGH - the last timed_run took LOW to HIGH ms, with
# the clock's grain allowed on both sides.
check_between() {
	low=$(($2 - GRAIN_MS))
	high=$(($3 + GRAIN_MS))
	if [ "$ELAPSED_MS" -lt "$low" ] || [ "$ELAPSED_MS" -gt "$high" ]; then
		fail "$1: took $ELAPSED_MS ms, wanted $2 to $3 ms (clock grain $GRAIN_MS ms)"
	fi
}

# Row 1.  The command writes its PID, then becomes the sleep: the PID is the
# command's, and the sleep dies on TERM.
PIDFILE=$DIR/pid1
timed_run "$PIDFILE" "echo \$\$ > '$PIDFILE'; exec sleep 30"
check_timeout "row 1 (dies on TERM)"
check_between "row 1 (dies on TERM)" "$DEADLINE_MS" "$((DEADLINE_MS + SLACK_MS))"
command_gone "row 1 (dies on TERM)" "$PIDFILE"

# Row 2.  The command ignores TERM (an ignored signal stays ignored through
# exec), so only KILL ends it, GRACE after TERM.
PIDFILE=$DIR/pid2
timed_run "$PIDFILE" "trap '' TERM; echo \$\$ > '$PIDFILE'; exec sleep 30"
check_timeout "row 2 (ignores TERM)"
check_between "row 2 (ignores TERM)" "$((DEADLINE_MS + GRACE_MS))" \
	"$((DEADLINE_MS + GRACE_MS + SLACK_MS))"
command_gone "row 2 (ignores TERM)" "$PIDFILE"

# Row 3.
/bin/sh "$SCRIPT" 5 /bin/sh -c 'exit 3'
RC=$?
[ "$RC" -eq 3 ] || fail "row 3: a command that ended by itself: status $RC, wanted 3"

# Row 4.  A command that ends by itself under a long deadline: its watcher
# must not outlive the script with the caller's pipe open.  The reader here
# is a command substitution: it returns when every writer has closed.
START=$(now_ms)
OUT=$(WITH_DEADLINE_GRACE=$GRACE_S /bin/sh "$SCRIPT" 30 /bin/sh -c 'exit 0')
END=$(now_ms)
ELAPSED_MS=$((END - START))
if [ "$ELAPSED_MS" -gt "$((SLACK_MS + GRAIN_MS))" ]; then
	fail "row 4: a command that ended at once held the caller's pipe for $ELAPSED_MS ms (wanted at most $SLACK_MS ms)"
fi

if [ "$BAD" -eq 0 ]; then
	echo "PASS: with-deadline.sh ends a hung command (124, TIMEOUT line, gone) at the deadline, waits the pause only for a command that ignores TERM, and keeps a normal status"
fi
exit "$BAD"
