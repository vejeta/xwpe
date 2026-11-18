# compiler.sh -- the one way a shell test runs the C compiler.
#
# Sourced, never run:   . "$(dirname "$0")/compiler.sh"
#
# configure hands the build a compiler that may carry flags (macOS:
# CC="gcc -std=gnu23", because autoconf appends the flag that selects the C
# level).  The words must be split, as the Makefile's own recipes do.  A
# test that quotes the variable looks for a program with the whole string as
# its name and fails "command not found"; a test that checks for the program
# first and skips when it is missing passes having tested nothing.
#
#   test_cc ARGS...    run the compiler as configure gave it
#   test_cc_require    compile one line once; exit 2 (the script could not
#                      run) when the compiler does not run
#
# The function is not named cc: with CC=cc (FreeBSD, OpenBSD) a function of
# that name would call itself without end.
#
# No other test script names the compiler variable in quotes, copies it, or
# checks that the program exists (tests/check-platform.sh row 14 reads every
# tests/*.sh for it).  run-tests.sh and check-child-exec.sh use their
# compiler variable unquoted, so they split it already.
#
# Written to fold into the shared check-script library that
# tests/check-sh-idiom.sh announces.

CC=${CC:-cc}

# test_cc ARGS... - the compiler, flags and all, given ARGS.
test_cc() {
	$CC "$@"
}

# test_cc_require - stop the script, status 2, when the compiler does not run.
# A broken compiler is a broken harness, never a reason to skip a check.
test_cc_require() {
	if ! printf 'int main(void) { return 0; }\n' | test_cc -x c -c -o /dev/null - > /dev/null 2>&1; then
		echo "${0##*/}: the compiler \"$CC\" does not run" >&2
		exit 2
	fi
}
