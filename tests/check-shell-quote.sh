#!/bin/sh
# check-shell-quote.sh -- a word the assistant hands to /bin/sh is quoted by
# one owner, wpe_ai_shell_quote (we_ai_ws.c).
#
# A format like "gcc -fsyntax-only '%s'" closes its quote at the first single
# quote in the file's name, and what follows runs as a command.  Every shell
# command the assistant builds takes its words from wpe_ai_shell_quote (or
# wpe_ai_shell_quote_whole) and puts the result in a bare %s.
#
#   check-shell-quote.sh [--srcdir DIR]
#
# Rule: no string literal in we_ai*.c holds '%s' (a format specifier between
# hand-written single quotes, with or without a width or precision, '%.*s'
# included).  Comment lines and wpe_ai_trace lines are not read: a trace is
# written to the trace file, never to a shell.
#
# Not seen: a quote and its %s in two separate literals ("'" "%s" "'"), and
# \"%s\" (double quotes do not stop $ or a backquote either).  Other messages
# use \"%s\", so that form cannot be refused by a pattern alone.
#
# Canaries: the same scan over planted offenders must name each, and must
# not name a planted trace or comment; a scan that fails either is wrong and
# fails the script.
#
# Exit: 0 clean, 1 offender or a wrong scanner.

SRCDIR=${srcdir:-.}
while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    *) echo "check-shell-quote: unknown argument $1" >&2; exit 1 ;;
  esac
  shift
done

# Prints "file:line: text" for each offending line of the files given.  -H
# keeps the file name even for one file, so the filters below always see
# "file:line:".
scan() {
  grep -HnE "\"[^\"]*'%[-0-9.*]*s'" "$@" 2>/dev/null |
    grep -vE '^[^:]*:[0-9]+:[[:space:]]*(/\*|\*|//)' |
    grep -vE '^[^:]*:[0-9]+:[[:space:]]*wpe_ai_trace[[:space:]]*\('
}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/check-shell-quote.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT
Q="'"
for canary in "snprintf(o, n, \"gcc -c ${Q}%s${Q}\", f);" \
              "snprintf(o, n, \"rm -- ${Q}%-10s${Q}\", f);" \
              "snprintf(o, n, \"rm -- ${Q}%.*s${Q}\", l, f);"; do
  printf '%s\n' "$canary" > "$TMP/canary.c"
  if [ -z "$(scan "$TMP/canary.c")" ]; then
    echo "check-shell-quote: the scan does not see a planted offender (blind): $canary" >&2
    exit 1
  fi
done
for quiet in " wpe_ai_trace(\"open ${Q}%s${Q}\", f);" \
             " /* snprintf(o, n, \"gcc ${Q}%s${Q}\", f); */"; do
  printf '%s\n' "$quiet" > "$TMP/quiet.c"
  if [ -n "$(scan "$TMP/quiet.c")" ]; then
    echo "check-shell-quote: the scan names a line no shell reads: $quiet" >&2
    exit 1
  fi
done

FOUND=$(cd "$SRCDIR" && scan we_ai*.c)
if [ -n "$FOUND" ]; then
  echo "check-shell-quote: hand-written '%s' in a format; quote the word with wpe_ai_shell_quote:" >&2
  echo "$FOUND" >&2
  exit 1
fi
echo "check-shell-quote: ok"
exit 0
