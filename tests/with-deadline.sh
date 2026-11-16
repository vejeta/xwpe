#!/bin/sh
# with-deadline.sh SECS COMMAND [ARG...]
#
# Run COMMAND; if it is still running after SECS seconds, send it TERM, then
# KILL after a pause (two seconds; WITH_DEADLINE_GRACE sets it), print
# "TIMEOUT: <command> after SECS s" and exit 124.  Otherwise exit with the
# command's own status.  Portable POSIX sh (it runs under dash and bash, on
# Linux, macOS and the BSDs), no timeout(1) needed.
#
# Used as the automake test runner: LOG_COMPILER="tests/with-deadline.sh 600",
# so a test that hangs ends with a line saying which one, and the rest of
# `make check` still runs.
#
# The script returns when the command ends: at the deadline for a command
# that dies on TERM, and after the pause for one that ignores it.  It never
# waits the pause on top of a command that is already gone.  The watcher that
# keeps the clock is stopped by the parent killing the one sleep it is in,
# not by a signal the watcher must catch: a shell that does not break a wait
# for a trapped signal would otherwise hold the script for the whole pause.  The watcher has no output of its own and
# is cut off from the caller's descriptors, so a pause that outlives the
# script cannot keep the caller's pipe open (the test's `$( )` reads until
# every writer has closed).
#
# The signals go to the command's PID only, not to its process group: under
# dash a non-interactive shell cannot start a job as a group leader, and the
# debug adapters the tests start run in their own session anyway.  A child
# the command itself started and did not wait for can therefore outlive it;
# those are left to the runner (the CI job ends them) and the test's own
# watchdog (tests/test_watchdog.h) is what ends them in a normal run.

if [ $# -lt 2 ]; then
	echo "usage: with-deadline.sh SECS COMMAND [ARG...]" >&2
	exit 2
fi
secs=$1
shift

grace=${WITH_DEADLINE_GRACE:-2}
case $grace in
*[!0-9]* | "") grace=2 ;;
esac

flag=$(mktemp -d "${TMPDIR:-/tmp}/with-deadline.XXXXXX") || exit 2

"$@" &
pid=$!

(
	# pause SECS - sleep, and tell the parent which sleep this is so that it
	# can end it.  Returns 1 when the parent said the command is done: the
	# parent writes $flag/done first and reads $flag/sleeper after, this
	# side writes $flag/sleeper first and reads $flag/done after, so at
	# least one of them sees the other, whichever runs first.
	pause() {
		sleep "$1" &
		sleeper=$!
		echo "$sleeper" > "$flag/sleeper"
		[ -e "$flag/done" ] && kill "$sleeper" 2>/dev/null
		wait "$sleeper" 2>/dev/null
		: > "$flag/sleeper"
		[ -e "$flag/done" ] && return 1
		return 0
	}
	pause "$secs" || exit 0
	: > "$flag/fired"
	kill -TERM "$pid" 2>/dev/null
	pause "$grace" || exit 0
	kill -KILL "$pid" 2>/dev/null
) >/dev/null 2>&1 </dev/null &
watcher=$!

wait "$pid"
rc=$?

: > "$flag/done"
sleeper=
# The watcher may remove the record between the test and the read.
{ read -r sleeper < "$flag/sleeper"; } 2>/dev/null
[ -n "$sleeper" ] && kill "$sleeper" 2>/dev/null
wait "$watcher" 2>/dev/null

if [ -e "$flag/fired" ]; then
	rm -f "$flag/fired" "$flag/done" "$flag/sleeper"
	rmdir "$flag"
	echo "TIMEOUT: $* after $secs s"
	exit 124
fi
rm -f "$flag/done" "$flag/sleeper"
rmdir "$flag"
exit "$rc"
