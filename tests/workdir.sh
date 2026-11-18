# workdir.sh -- the one way a shell test makes its work folder.
#
# Sourced, never run:   . "$(dirname "$0")/workdir.sh"
#
# A check script that makes a folder and removes it at exit with a trap that
# does not exit leaves the script running after TERM, with its folder gone:
# every later step then fails, or a loop ends and the script passes having
# tested nothing.  This file is the one place that knows how to do it right;
# tests/check-sh-idiom.sh (rule 1) names a script that makes its own folder or
# its own trap, and its unit row signals a child that uses work_dir.
#
#   work_dir NAME [STATUS]
#       WORK is a new folder NAME.XXXXXX under ${WORK_BASE:-${TMPDIR:-/tmp}}.
#       It is removed when the script ends, however it ends.  TERM and HUP end
#       the script with 143 and 129 (and INT with 130 when the shell gets
#       one).  A check run by make check is started by a runner with "&", and
#       a shell ignores INT in such a job on entry and cannot take it back, so
#       INT never reaches it there; TERM is what tests/with-deadline.sh sends.
#       STATUS (default 2) is the exit status when the folder cannot be made.
#
# Call it once, from the main shell, before any use of WORK: inside ( ) or
# $( ) the folder is removed when that subshell ends and WORK is empty in the
# script.  A second folder is made under $WORK, never by a second call (a
# second call is refused: its EXIT trap would replace the first and leave that
# folder behind).  A script that needs its own action at exit does not set
# its own EXIT trap, which would replace this one.
#
# A trapped signal runs when the foreground command ends, so a script blocked
# in a long child ends when that child does.
#
# The name() form only: a "function name" has its own EXIT trap in some
# shells (OpenBSD ksh).  The template keeps six X's, the least OpenBSD
# accepts, and does not use -t, which means different things on different
# systems.

WORK_DIR_MADE=

work_dir() {
	if [ -n "$WORK_DIR_MADE" ]; then
		echo "${0##*/}: work_dir called twice: make subfolders under \$WORK" >&2
		exit "${2:-2}"
	fi
	WORK_DIR_MADE=1
	work_dir_base=${WORK_BASE:-${TMPDIR:-/tmp}}
	WORK=$(mktemp -d "$work_dir_base/$1.XXXXXX") || exit "${2:-2}"
	case $WORK in
	"$work_dir_base/$1."?*) ;;
	*)
		echo "${0##*/}: work_dir: \"$WORK\" is not under $work_dir_base/$1." >&2
		rmdir "$WORK" 2> /dev/null
		exit "${2:-2}"
		;;
	esac
	trap 'rm -rf "$WORK"' EXIT
	trap 'exit 129' HUP
	trap 'exit 130' INT
	trap 'exit 143' TERM
}
