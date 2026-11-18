#!/bin/sh
# check-sh-idiom.sh -- the awk inside the check scripts is linear in the size
# of what it reads.
#
# The check scripts read whole C files in awk.  A scan that walks a file one
# character at a time through `substr(all, i, 1)`, or asks `length(all)` at
# every step, costs the size of the file for each step on an awk whose
# substr and length count characters, not bytes (gawk in a UTF-8 locale, the
# one-true-awk of FreeBSD 14 and OpenBSD 7.7): the check that took 7 s under
# gawk here ran past the 300 s deadline on two BSDs.  The idiom that stays
# linear on every awk: blank comments and literals one line at a time with
# the one blanker (tests/blank-c.awk, `blank_c`), and where a whole-file
# string cannot be avoided, `split(all, ch, "")` once, `n = length(all)` once,
# and index the array.
#
#   check-sh-idiom.sh [--srcdir DIR] [--selftest]
#
# Rule 2, over every tests/check-*.sh:
#  a. substr(V, e, 1) where V is a string the script builds up across lines
#     (a line of the form `V = V ...`): a character walk over a whole file.
#  b. X = substr(V, e) on the same V: a copy of the rest of the file for
#     every step of a scan.
#  c. length( inside the condition of a for or while: the length is taken
#     again at every step.  Take it once, before the loop.
# A line that carries `# linear:` and a reason (the string is one line, or
# one statement, and short) is not named.
#
# Rule 1, over every tests/*.sh and tests/*/*.sh except tests/workdir.sh
# (heredoc bodies are read as text): a script makes its work folder with
# work_dir from tests/workdir.sh and sets no trap of its own.  Named:
#  t. a line with the word trap (then trap, do trap, eval "trap, command
#     trap, a trap after ; or &&, a later EXIT trap that would replace the
#     owner's);
#  m. a mktemp that does not make its folder under "$WORK/";
#  s. work_dir after ( or $( or a backquote: the folder would be removed
#     when that subshell ends.
# A line that carries `# workdir:` and a reason (a test child that must
# ignore TERM) is not named.
#
# The unit row of rule 1 runs a child that sources the owner, under each of
# dash, bash --posix and busybox sh found, sends it TERM and then HUP, and
# requires exit 143 (129), the folder gone, no work done after the signal,
# and the folder still there after a $( ) and a pipeline.  Two mutant
# owners are run at the start of every run and must fail it: the one-line
# trap (ends the script never) and an EXIT-only trap (leaves the folder on
# dash and busybox; with no such shell that canary says SKIP).
#
# Canaries (--selftest, and at the start of every run): an offender per
# clause, on one line, and the same line with the marker and with the
# linear idiom; a scanner that does not name every offender, or names a
# clean line, fails the script as blind.  The offenders of rule 1 are built
# from pieces so that this file does not name itself.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
SELFTEST=0
while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "check-sh-idiom: unknown argument $1" >&2; exit 1 ;;
  esac
  shift
done

. "$(dirname "$0")/workdir.sh"
work_dir sh-idiom 1

# scan FILE... - "file:line: clause: text" for each line of rule 2.
scan() {
  for f in "$@"; do
    awk -v file="$f" '
      function code_of(s) {
        sub(/^[ \t]+/, "", s)
        return s
      }
      function note_builder(line,   rest, name) {
        rest = line
        while (match(rest, /[A-Za-z_][A-Za-z0-9_]*[ \t]=[ \t][A-Za-z_][A-Za-z0-9_]*/)) {
          name = substr(rest, RSTART, RLENGTH)
          split(name, parts, /[ \t]=[ \t]/)
          if (parts[1] == parts[2]) built[parts[1]] = 1
          rest = substr(rest, RSTART + RLENGTH)
        }
      }
      FNR == NR { note_builder($0); next }
      {
        text = code_of($0)
        if (text ~ /^#/ || $0 ~ /#[ \t]*linear:/) next
        for (v in built) {
          if ($0 ~ ("substr\\([ \t]*" v "[ \t]*,[^,]*,[ \t]*1[ \t]*\\)"))
            print file ":" FNR ": a. character walk over " v ": " text
          else if ($0 ~ ("=[ \t]*substr\\([ \t]*" v "[ \t]*,[^,)]*\\)"))
            print file ":" FNR ": b. copy of the rest of " v ": " text
        }
        if ($0 ~ /(while|for)[ \t]*\(.*length\(/)
          print file ":" FNR ": c. length( in a loop condition: " text
      }' "$f" "$f"
  done
}

# canary NAME CODE EXPECT - CODE is a line of awk inside a script that builds
# V by appending; EXPECT is the clause letter the scan must name, or "none".
canary() {
  printf '%s\n' '    { all = all $0 " " }' "$2" > "$WORK/plant.sh"
  got=$(scan "$WORK/plant.sh")
  if [ "$3" = none ]; then
    [ -z "$got" ] || { echo "canary $1: a clean line was named: $got"; return 1; }
    return 0
  fi
  case $got in
    *": $3. "*) return 0 ;;
  esac
  echo "canary $1: the offender was not named (wanted clause $3, got: $got)"
  return 1
}

# scan_workdir FILE... - "file:line: clause: text" for each line of rule 1.
# Comment lines and lines with the marker `# workdir: <reason>` are not read.
scan_workdir() {
  for f in "$@"; do
    awk -v file="$f" '
      {
        text = $0
        sub(/^[ \t]+/, "", text)
        if (text ~ /^#/) next
        if ($0 ~ /#[ \t]*workdir:[ \t]*[^ \t]/) next
        if ($0 ~ /(^|[^A-Za-z0-9_.-])trap([ \t]|$)/)
          print file ":" FNR ": t. a signal handler of its own, use work_dir: " text
        if ($0 ~ /mk[t]emp/ && $0 !~ /"\$WORK\//)
          print file ":" FNR ": m. a work folder made outside work_dir: " text
        if ($0 ~ /[(`][ \t]*work_dir([ \t]|$)/)
          print file ":" FNR ": s. work_dir inside a subshell: " text
      }' "$f"
  done
}

# canary1 NAME CODE EXPECT - CODE is one line of shell; EXPECT is the clause
# letter scan_workdir must name, or "none".
canary1() {
  printf '%s\n' "$2" > "$WORK/plant1.sh"
  got=$(scan_workdir "$WORK/plant1.sh")
  if [ "$3" = none ]; then
    [ -z "$got" ] || { echo "canary $1: a clean line was named: $got"; return 1; }
    return 0
  fi
  case $got in
    *": $3. "*) return 0 ;;
  esac
  echo "canary $1: the offender was not named (wanted clause $3, got: $got)"
  return 1
}

# The words of the offenders are cut in two so this file does not hold them.
TR=tr
MK=mk
WK=w
TAB=$(printf '\t')

# unit_write_child FILE - the child of the unit row.  It takes the owner's
# path as $1 and an output folder as $2, makes its folder, runs a $( ) and a
# pipeline (an inherited EXIT trap in a subshell would remove the folder
# there), says the folder is still there, then works for about four seconds
# in a wait a signal can break, and says it ran on if it gets that far.
unit_write_child() {
  {
    printf '%s\n' '. "$1"' 'work_dir unit' 'echo "$WORK" > "$2/folder"'
    printf '%s\n' 'x=$(echo a); echo b | cat > /dev/null'
    printf '%s\n' 'if [ -d "$WORK" ]; then : > "$2/alive"; fi'
    printf '%s\n' 'i=0' 'while [ $i -lt 4 ]; do' '  sleep 1 & wait $!' '  i=$((i + 1))' 'done'
    printf '%s\n' ': > "$2/ran-on"'
  } > "$1"
}

# unit_wait_for FILE - wait up to ten seconds for FILE to exist.
unit_wait_for() {
  w=0
  while [ ! -e "$1" ] && [ $w -lt 50 ]; do
    sleep 0.2 2> /dev/null || sleep 1
    w=$((w + 1))
  done
  [ -e "$1" ]
}

# unit_run SHELL OWNER SIGNAL - run the child under SHELL (words) with OWNER
# sourced, send SIGNAL (TERM or HUP) to its exact PID once it has made and
# used its folder, and check: exit 143 or 129, the folder gone, no work after
# the signal, the folder there after the $( ) and the pipeline.  Status 0
# when all hold; UNIT_WHY says what failed.
unit_run() {
  u_out=$(mktemp -d "$WORK/unit.XXXXXX") || { UNIT_WHY="no folder"; return 1; }
  mkdir "$u_out/tmp"
  unit_write_child "$u_out/child.sh"
  # $1 is split on purpose: "bash --posix" is two words.
  # shellcheck disable=SC2086
  TMPDIR=$u_out/tmp $1 "$u_out/child.sh" "$2" "$u_out" > /dev/null 2>&1 < /dev/null &
  u_pid=$!
  case $3 in
    TERM) u_want=143 ;;
    *) u_want=129 ;;
  esac
  if ! unit_wait_for "$u_out/folder"; then
    kill "$u_pid" 2> /dev/null
    wait "$u_pid" 2> /dev/null
    UNIT_WHY="the child did not make its folder"
    return 1
  fi
  unit_wait_for "$u_out/alive" || :
  kill -"$3" "$u_pid"
  wait "$u_pid" 2> /dev/null
  u_rc=$?
  u_folder=$(cat "$u_out/folder")
  UNIT_WHY=
  if [ ! -e "$u_out/alive" ]; then
    UNIT_WHY="the folder was gone after a \$( ) and a pipeline"
  elif [ $u_rc -ne $u_want ]; then
    UNIT_WHY="exit status $u_rc, wanted $u_want"
  elif [ -d "$u_folder" ]; then
    UNIT_WHY="the folder was left behind"
  elif [ -e "$u_out/ran-on" ]; then
    UNIT_WHY="the script ran on after the signal"
  fi
  [ -z "$UNIT_WHY" ]
}

# unit_shell_words NAME - the words that run shell NAME, or nothing when it
# is not installed.
unit_shell_words() {
  command -v "$1" > /dev/null 2>&1 || return 0
  case $1 in
    bash) echo "bash --posix" ;;
    busybox) echo "busybox sh" ;;
    *) echo "$1" ;;
  esac
}

# unit_second_call SHELL OWNER - a second work_dir is refused before it makes
# a folder: exit 7 (the STATUS given), nothing after it runs, nothing left.
unit_second_call() {
  u2=$(mktemp -d "$WORK/unit2.XXXXXX") || return 1
  mkdir "$u2/tmp"
  printf '%s\n' '. "$1"' 'work_dir one' 'work_dir two 7' ': > "$2/went-on"' > "$u2/child.sh"
  # shellcheck disable=SC2086
  TMPDIR=$u2/tmp $1 "$u2/child.sh" "$2" "$u2" > /dev/null 2>&1 < /dev/null
  u2_rc=$?
  [ $u2_rc -eq 7 ] && [ ! -e "$u2/went-on" ] && [ -z "$(ls "$u2/tmp")" ]
}

# unit_base SHELL OWNER - WORK_BASE decides where the folder is made.
unit_base() {
  u3=$(mktemp -d "$WORK/unit3.XXXXXX") || return 1
  mkdir "$u3/base"
  printf '%s\n' '. "$1"' "WORK_BASE=\$2/base" 'work_dir x' 'echo "$WORK" > "$2/where"' > "$u3/child.sh"
  # shellcheck disable=SC2086
  $1 "$u3/child.sh" "$2" "$u3" > /dev/null 2>&1 < /dev/null
  case $(cat "$u3/where" 2> /dev/null) in
    "$u3/base/x."?*) return 0 ;;
  esac
  return 1
}

# unit_mutant NAME BODY - write the mutant owner NAME with the given trap
# line(s) in work_dir; the folder is made as in the owner.
unit_mutant() {
  {
    printf '%s\n' 'work_dir() {'
    printf '%s\n' '  WORK=$('"$MK"'temp -d "${TMPDIR:-/tmp}/$1.XXXXXX") || exit 2'
    printf '%s\n' "$2"
    printf '%s\n' '}'
  } > "$WORK/mutant-$1.sh"
}

unit_blind=0
unit_shells=
for name in dash bash busybox; do
  unit_shells="$unit_shells$(unit_shell_words $name)|"
done
OWNER_LIB=$SRCDIR/tests/workdir.sh
if [ ! -f "$OWNER_LIB" ]; then
  echo "check-sh-idiom: $OWNER_LIB is missing"
  unit_blind=1
fi
# The owner, under every shell found, for TERM and HUP.
if [ $unit_blind -eq 0 ]; then
  oldifs=$IFS
  IFS='|'
  for words in $unit_shells; do
    IFS=$oldifs
    [ -n "$words" ] || continue
    for sig in TERM HUP; do
      if ! unit_run "$words" "$OWNER_LIB" $sig; then
        echo "unit: tests/workdir.sh under $words, $sig: $UNIT_WHY"
        unit_blind=1
      fi
    done
    unit_second_call "$words" "$OWNER_LIB" || { echo "unit: $words: a second work_dir is not refused before it makes a folder"; unit_blind=1; }
    unit_base "$words" "$OWNER_LIB" || { echo "unit: $words: WORK_BASE is not where the folder is made"; unit_blind=1; }
    IFS='|'
    first_shell=${first_shell:-$words}
  done
  IFS=$oldifs
fi
# The canaries: the one-line trap never ends the script; an EXIT-only trap
# leaves the folder on dash and busybox.  Each must fail the unit run.
unit_mutant oneline "  $TR"'ap '"'"'rm -rf "$WORK"'"'"' EXIT HUP INT TERM'
unit_mutant exitonly "  $TR"'ap '"'"'rm -rf "$WORK"'"'"' EXIT'
if [ -n "$first_shell" ] && [ $unit_blind -eq 0 ]; then
  if unit_run "$first_shell" "$WORK/mutant-oneline.sh" TERM; then
    echo "canary: the one-line handler passes the unit run under $first_shell: the row is blind"
    unit_blind=1
  fi
  exit_only_seen=0
  for words in "dash" "busybox sh"; do
    case $unit_shells in
      *"$words|"*) ;;
      *) continue ;;
    esac
    exit_only_seen=1
    if unit_run "$words" "$WORK/mutant-exitonly.sh" TERM; then
      echo "canary: an EXIT-only handler passes the unit run under $words: the row is blind"
      unit_blind=1
    fi
  done
  if [ $exit_only_seen -eq 0 ]; then
    echo "SKIP: canary for an EXIT-only trap: no dash or busybox sh here, bash --posix removes the folder anyway"
  fi
fi

blind=0
canary1 trap-exit   "${TR}ap 'rm -rf \"\$WORK\"' EXIT" t || blind=1
canary1 trap-indent "  ${TR}ap 'exit 130' INT" t || blind=1
canary1 trap-semi   "x=1; ${TR}ap 'exit 1' EXIT" t || blind=1
canary1 trap-then   "if x; then ${TR}ap 'exit 1' EXIT; fi" t || blind=1
canary1 trap-do     "for i in a; do ${TR}ap 'exit 1' EXIT; done" t || blind=1
canary1 trap-bang   "! ${TR}ap 'exit 1' EXIT" t || blind=1
canary1 trap-eval   "eval \"${TR}ap 'exit 1' EXIT\"" t || blind=1
canary1 trap-cmd    "command ${TR}ap 'exit 1' EXIT" t || blind=1
canary1 trap-bslash "\\${TR}ap 'exit 1' EXIT" t || blind=1
canary1 trap-quoted "echo \"a; ${TR}ap b\"" t || blind=1
canary1 trap-tab    "${TR}ap${TAB}'exit 1' EXIT" t || blind=1
canary1 folder-out  "W=\$(${MK}temp -d /tmp/x.XXXXXX)" m || blind=1
canary1 sub-dollar  "x=\$(${WK}ork_dir foo)" s || blind=1
canary1 sub-paren   "(${WK}ork_dir foo)" s || blind=1
canary1 comment-ok  "# ${TR}ap 'exit 1' EXIT" none || blind=1
canary1 marked-ok   "run \"${TR}ap '' TERM\"  # workdir: a test child that must ignore TERM" none || blind=1
canary1 call-ok     "${WK}ork_dir foo 1" none || blind=1
canary1 call-ok2    "  ${WK}ork_dir foo \$BAD" none || blind=1
canary1 folder-ok   "x=\$(${MK}temp -d \"\$WORK/a.XXXXXX\")" none || blind=1
canary1 name-ok     "n_${TR}ap_count=1" none || blind=1
canary walk       '      for (i = 1; i <= n; i++) { c = substr(all, i, 1)' a || blind=1
canary walk-sp    '      c = substr(all , j, 1)' a || blind=1
canary rest       '        rest = substr(all, pos)' b || blind=1
canary rest-expr  '        rest = substr(all, at + plen)' b || blind=1
canary loop-for   '      for (i = from; i <= length(s); i++) {' c || blind=1
canary loop-while '      while (pos <= length(body)) {' c || blind=1
canary marked     '      c = substr(all, i, 1)  # linear: one line' none || blind=1
canary marked-len '      while (i <= length(s)) {  # linear: one line' none || blind=1
canary index-ok   '      c = ch[i]' none || blind=1
canary once-ok    '      n = length(all); for (i = 1; i <= n; i++) { c = ch[i] }' none || blind=1
canary slice-ok   '      after = substr(all, pos, 160)' none || blind=1
canary line-ok    '      c = substr(line, i, 1)' none || blind=1
canary comment-ok '      # for (i = 1; i <= length(all); i++) c = substr(all, i, 1)' none || blind=1
if [ $blind -ne 0 ] || [ $unit_blind -ne 0 ]; then
  echo "check-sh-idiom: the scanner is blind"
  exit 1
fi
if [ $SELFTEST -eq 1 ]; then
  echo "check-sh-idiom: canaries ok"
  exit 0
fi

# Rule 1 reads every shell script of the test folders, this one too (its
# canaries are built from pieces) but not the owner.
wfiles=$(ls "$SRCDIR"/tests/*.sh "$SRCDIR"/tests/*/*.sh 2>/dev/null | grep -v '/workdir\.sh$')
if [ -z "$wfiles" ]; then
  echo "check-sh-idiom: no scripts under $SRCDIR/tests" >&2
  exit 1
fi
# shellcheck disable=SC2086
wfound=$(scan_workdir $wfiles)
if [ -n "$wfound" ]; then
  echo "check-sh-idiom: a work folder or a trap outside tests/workdir.sh (call work_dir NAME [STATUS] once, or mark the line '# workdir: <reason>'):"
  printf '%s\n' "$wfound"
  exit 1
fi

# Rule 2: this script holds the offenders of its canaries as text, so it is not read.
files=$(ls "$SRCDIR"/tests/check-*.sh 2>/dev/null | grep -v '/check-sh-idiom\.sh$')
if [ -z "$files" ]; then
  echo "check-sh-idiom: no check scripts under $SRCDIR/tests" >&2
  exit 1
fi
# shellcheck disable=SC2086
found=$(scan $files)
if [ -n "$found" ]; then
  echo "check-sh-idiom: awk that is not linear (blank per line with blank_c, split once, index the array):"
  printf '%s\n' "$found"
  exit 1
fi
echo "check-sh-idiom: clean"
exit 0
