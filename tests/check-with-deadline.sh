#!/bin/sh
# check-with-deadline.sh -- tests/with-deadline.sh ends a command that hangs.
#
# `sleep 5` under `with-deadline.sh 1` must exit 124 within 2 s, print the
# TIMEOUT line, and leave the command gone.  A command that ends by itself
# keeps its own exit status.  Run under /bin/sh, which is dash on Debian: the
# script must not need bash.
#
# @scenario SC-child-wait-08
#
# Exit: 0 pass, 1 a row failed.

SRCDIR=${srcdir:-.}
SCRIPT=$SRCDIR/tests/with-deadline.sh
[ -f "$SCRIPT" ] || SCRIPT=$SRCDIR/with-deadline.sh
BAD=0
DIR=$(mktemp -d "${TMPDIR:-/tmp}/check-with-deadline.XXXXXX") || exit 1

fail() { echo "FAIL: $1"; BAD=1; }

# The command writes its PID, then becomes the sleep: the PID is the command's.
PIDFILE=$DIR/pid
START=$(date +%s)
OUT=$(/bin/sh "$SCRIPT" 1 /bin/sh -c "echo \$\$ > '$PIDFILE'; exec sleep 5")
RC=$?
END=$(date +%s)

[ "$RC" -eq 124 ] || fail "exit status $RC, wanted 124"
[ $((END - START)) -le 2 ] || fail "took $((END - START)) s, wanted at most 2"
case $OUT in
*"TIMEOUT: "*" after 1 s"*) ;;
*) fail "no TIMEOUT line in: $OUT" ;;
esac
if [ -s "$PIDFILE" ]; then
	PID=$(cat "$PIDFILE")
	if kill -0 "$PID" 2>/dev/null; then
		fail "the command (pid $PID) is still running"
		kill -KILL "$PID" 2>/dev/null
	fi
else
	fail "the command never started"
fi

/bin/sh "$SCRIPT" 5 /bin/sh -c 'exit 3'
RC=$?
[ "$RC" -eq 3 ] || fail "a command that ended by itself: status $RC, wanted 3"

rm -f "$PIDFILE"
rmdir "$DIR"
if [ "$BAD" -eq 0 ]; then
	echo "PASS: with-deadline.sh ends a hung command (124, TIMEOUT line, gone) and keeps a normal status"
fi
exit "$BAD"
