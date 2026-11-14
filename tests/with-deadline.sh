#!/bin/sh
# with-deadline.sh SECS COMMAND [ARG...]
#
# Run COMMAND; if it is still running after SECS seconds, send it TERM, then
# KILL two seconds later, print "TIMEOUT: <command> after SECS s" and exit
# 124.  Otherwise exit with the command's own status.  Portable POSIX sh (it
# runs under dash and bash, on Linux, macOS and the BSDs), no timeout(1)
# needed.
#
# Used as the automake test runner: LOG_COMPILER="tests/with-deadline.sh 600",
# so a test that hangs ends with a line saying which one, and the rest of
# `make check` still runs.
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

flag=$(mktemp -d "${TMPDIR:-/tmp}/with-deadline.XXXXXX") || exit 2

"$@" &
pid=$!

(
	# The watcher is told to stop with TERM when the command ends first.
	# The TERM can come at any point, also between starting a sleep and
	# noting its PID, so the trap only records it and kills the sleep it
	# knows; after each start the watcher checks the record and kills the
	# sleep itself.  No sleep outlives the watcher.
	sleeper=
	stop=
	trap 'stop=1; [ -n "$sleeper" ] && kill "$sleeper" 2>/dev/null' TERM
	pause() {
		sleep "$1" &
		sleeper=$!
		if [ -n "$stop" ]; then
			kill "$sleeper" 2>/dev/null
			exit 0
		fi
		wait "$sleeper"
		[ -n "$stop" ] && exit 0
		sleeper=
	}
	pause "$secs"
	: > "$flag/fired"
	kill -TERM "$pid" 2>/dev/null
	pause 2
	kill -KILL "$pid" 2>/dev/null
) &
watcher=$!

wait "$pid"
rc=$?

kill -TERM "$watcher" 2>/dev/null
wait "$watcher" 2>/dev/null

if [ -e "$flag/fired" ]; then
	rm -f "$flag/fired"
	rmdir "$flag"
	echo "TIMEOUT: $* after $secs s"
	exit 124
fi
rmdir "$flag"
exit "$rc"
