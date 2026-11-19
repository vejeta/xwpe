#!/bin/sh
# check-contain-sites.sh -- every program the assistant starts names how it is
# held, through the one owner, we_contain.c.
#
# A command the assistant runs with the editor's whole environment and no limit
# is how a provider key reaches a model's command.  That cannot happen while
# every start goes through wpe_contain_start with the profile of what it is,
# and the start gate (we_child.c) refuses an assistant start that names none.
# A second way to start an assistant child, or a site that passes no profile,
# is how one command would run unheld, one site after another.
#
#   check-contain-sites.sh [--srcdir DIR] [--selftest]
#
# Rules (comments and string literals are blanked first):
#  1. No we_ai*.c calls wpe_child_start, wpe_child_capture, wpe_child_probe or
#     wpe_child_run_to_file (or their _plan forms), nor the start plan's
#     wpe_startplan_start, _start_words, _start_carried, _capture, _probe,
#     _run_to_file or _fixed: the assistant's children start in we_contain.c.
#  2. WPE_CHILD_ORIGIN_AI is spelled only in we_contain.c (which sets it with
#     the containment), in we_child.c (the gate and its definition) and in
#     we_child.h.
#  3. Every wpe_contain_start( call in we_ai*.c passes a profile: a
#     &wpe_contain_<kind> or the name `profile`, never NULL or nothing.
#  4. ai_capture_spawn( is called only by ai_capture_start and ai_run_capture,
#     and each passes the `profile` it was given; ai_capture_start( and
#     ai_run_capture( are called only by the capture faces, each with a
#     &wpe_contain_<kind>; host_spawn( is called only by wpe_host_start, with
#     host_profile( or a &wpe_contain_<kind>.
#  5. The old faces are gone: wpe_ai_run_capture(, wpe_ai_run_capture_status(,
#     wpe_ai_run_capture_bounded( and wpe_ai_capture_start( are spelled
#     nowhere.
#  6. execve, execvp and the other exec calls appear only in we_child.c.
#  7. The start gate reads the profile: wpe_child_unplanned in we_child.c
#     spells WPE_CHILD_ORIGIN_AI and the containment's refusal.
#
# Canaries: each rule's scan over a planted offender must name it, and over a
# planted clean line must not; a scan that fails either is wrong and fails the
# script.  --selftest runs only them.
#
# Exit: 0 clean, 1 offender or a wrong scanner.

SRCDIR=${srcdir:-.}
MODE=all
while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --selftest) MODE=selftest ;;
    *) echo "check-contain-sites: unknown argument $1" >&2; exit 1 ;;
  esac
  shift
done

BLANK_C=$(cat "$(dirname "$0")/blank-c.awk")

# blank FILE... : the files with comments and the contents of literals blanked,
# each line as "file:line:text".
blank() {
  for f in "$@"; do
    awk "$BLANK_C"' BEGIN { blank_incom = 0 }
      { printf "%s:%d:%s\n", FILENAME, NR, blank_c($0, 0) }' "$f"
  done
}

# Rule 1.
scan_starts() {
  grep -E '(^|[^A-Za-z0-9_])(wpe_child_start|wpe_child_capture|wpe_child_probe|wpe_child_run_to_file|wpe_child_capture_plan|wpe_child_probe_plan|wpe_child_run_to_file_plan|wpe_startplan_start|wpe_startplan_start_words|wpe_startplan_start_carried|wpe_startplan_capture|wpe_startplan_probe|wpe_startplan_run_to_file|wpe_startplan_fixed)[[:space:]]*\('
}
# Rule 2 (the files that may spell it are not given to this scan).
scan_origin() {
  grep -E 'WPE_CHILD_ORIGIN_AI'
}
# Rule 3: a call whose first argument is not a profile.
scan_unprofiled() {
  grep -E 'wpe_contain_start[[:space:]]*\(' |
    grep -vE 'wpe_contain_start[[:space:]]*\([[:space:]]*(&wpe_contain_[a-z_]+|profile)[[:space:]]*,' |
    grep -vE '^[^:]*:[0-9]+:[[:space:]]*(static[[:space:]]+)?(int|char|void)[[:space:]]+wpe_contain_start'
}
# Rule 5.
scan_old_faces() {
  grep -E '(^|[^A-Za-z0-9_])(wpe_ai_run_capture|wpe_ai_run_capture_status|wpe_ai_run_capture_bounded|wpe_ai_capture_start)[[:space:]]*\('
}
# Rule 6.
scan_exec() {
  grep -E '(^|[^A-Za-z0-9_])(execve|execvp|execv|execl|execlp|execle|execvpe|fexecve)[[:space:]]*\('
}

selftest() {
  . "$(dirname "$0")/workdir.sh"
  work_dir check-contain-sites 1
  T=$WORK
  bad=0
  # each: name|scan|planted offender
  while IFS='|' read -r name scan planted; do
    printf '%s\n' "$planted" > "$T/c.c"
    if [ -z "$(blank "$T/c.c" | $scan)" ]; then
      echo "check-contain-sites: the $name scan does not see a planted offender (blind): $planted" >&2
      bad=1
    fi
  done <<EOF
starts|scan_starts| rc = wpe_child_start(&spec, &c, ended, data);
starts|scan_starts| out = wpe_child_capture(argv, buf, n, 5, &st, "x");
starts|scan_starts| rc = wpe_startplan_start(p, argv, &io, &c, ended, data);
starts|scan_starts| rc = wpe_startplan_start_words(p, &io, &c, ended, data);
starts|scan_starts| rc = wpe_startplan_start_carried(p, &io, &c, ended, data);
starts|scan_starts| rc = wpe_startplan_capture(p, out, max, 5000, &st, "x");
starts|scan_starts| rc = wpe_startplan_probe(p, out, max, 5000, &st, "x");
starts|scan_starts| rc = wpe_startplan_run_to_file(p, path, 5000, &st, "x");
starts|scan_starts| p = wpe_startplan_fixed(kind, "x", argv);
origin|scan_origin| spec.origin = WPE_CHILD_ORIGIN_AI;
unprofiled|scan_unprofiled| rc = wpe_contain_start(NULL, &spec, &c, ended, data);
unprofiled|scan_unprofiled| rc = wpe_contain_start(p, &spec, &c, ended, data);
oldfaces|scan_old_faces| out = wpe_ai_run_capture(cmd);
oldfaces|scan_old_faces| out = wpe_ai_run_capture_status(cmd, &st);
oldfaces|scan_old_faces| c = wpe_ai_capture_start(cmd, 5, done, ud);
exec|scan_exec| execvp(argv[0], argv);
exec|scan_exec| execve(prog, argv, envp);
EOF
  while IFS='|' read -r name scan clean; do
    printf '%s\n' "$clean" > "$T/c.c"
    if [ -n "$(blank "$T/c.c" | $scan)" ]; then
      echo "check-contain-sites: the $name scan names a line that is fine: $clean" >&2
      bad=1
    fi
  done <<EOF
starts|scan_starts| /* wpe_child_start(&spec, &c, ended, data); */
starts|scan_starts| wpe_ai_trace("wpe_child_start(");
starts|scan_starts| /* wpe_startplan_start(p, argv, &io, &c, ended, data); */
starts|scan_starts| p = wpe_startplan_new(kind, "x");
starts|scan_starts| wpe_startplan_free(p);
starts|scan_starts| rc = wpe_contain_start(profile, &spec, &c, ended, data);
unprofiled|scan_unprofiled| rc = wpe_contain_start(&wpe_contain_tool, &spec, &c, ended, data);
unprofiled|scan_unprofiled| if (wpe_contain_start(profile, &spec, &h->child, host_on_ended, h) != 0)
unprofiled|scan_unprofiled| int wpe_contain_start(const WpeContainProfile *p, WpeChildSpec *spec, WpeChild *c,
oldfaces|scan_old_faces| out = wpe_ai_run_capture_git(cmd);
oldfaces|scan_old_faces| out = wpe_ai_run_capture_git_status(cmd, &st);
oldfaces|scan_old_faces| c = wpe_ai_capture_start_tool(cmd, 5, done, ud);
exec|scan_exec| /* execvp(argv[0], argv) */
exec|scan_exec| wpe_child_exec(spec, p, &t, &keep, &words, editor);
EOF
  return $bad
}

if [ "$MODE" = selftest ]; then
  selftest || exit 1
  echo "check-contain-sites: selftest ok"
  exit 0
fi

selftest || exit 1
cd "$SRCDIR" || exit 1
fail=0

AI_FILES=$(ls we_ai*.c 2>/dev/null)
ALL_FILES=$(ls *.c 2>/dev/null)

report() {
  # report RULE TEXT FOUND
  if [ -n "$3" ]; then
    echo "check-contain-sites: rule $1: $2" >&2
    echo "$3" >&2
    fail=1
  fi
}

# shellcheck disable=SC2086
report 1 "an assistant file starts a child itself; start it with wpe_contain_start and a profile:" \
  "$(blank $AI_FILES | scan_starts)"

OTHERS=$(for f in $ALL_FILES; do case $f in (we_contain.c|we_child.c) ;; (*) echo "$f" ;; esac; done)
# shellcheck disable=SC2086
report 2 "WPE_CHILD_ORIGIN_AI outside we_contain.c and we_child.c:" "$(blank $OTHERS | scan_origin)"

# shellcheck disable=SC2086
report 3 "wpe_contain_start without a profile:" "$(blank $AI_FILES | scan_unprofiled)"

# Rule 4: the callers of the private spawn and start functions.
callers() {
  # callers FUNCTION : "file:line:text" of every call of it.  A definition
  # starts in the first column; a call is inside a function, indented.
  blank $AI_FILES | grep -E "(^|[^A-Za-z0-9_])$1[[:space:]]*\(" |
    grep -vE '^[^:]*:[0-9]+:[A-Za-z]'
}
spawn_calls=$(callers ai_capture_spawn)
off=$(printf '%s\n' "$spawn_calls" | grep -vE 'ai_capture_spawn[[:space:]]*\([^;]*,[[:space:]]*profile[[:space:]]*\)' | grep -v '^$')
report 4 "ai_capture_spawn called without the profile it was given:" "$off"
n=$(printf '%s\n' "$spawn_calls" | grep -c .)
if [ "$n" != 2 ]; then
  report 4 "ai_capture_spawn must be called by ai_capture_start and ai_run_capture only (found $n calls):" "$spawn_calls"
fi
off=$(callers ai_capture_start | grep -vE 'ai_capture_start[[:space:]]*\([^;]*&wpe_contain_[a-z_]+\)' | grep -v '^$')
report 4 "ai_capture_start called without a &wpe_contain_<kind>:" "$off"
off=$(callers ai_run_capture | grep -vE 'ai_run_capture[[:space:]]*\([^;]*&wpe_contain_[a-z_]+\)' | grep -v '^$')
report 4 "ai_run_capture called without a &wpe_contain_<kind>:" "$off"
off=$(callers host_spawn | grep -vE 'host_spawn[[:space:]]*\([^;]*(host_profile[[:space:]]*\(|&wpe_contain_[a-z_]+)' | grep -v '^$')
report 4 "host_spawn called without a profile:" "$off"

# shellcheck disable=SC2086
report 5 "an old capture face that took no profile:" "$(blank $ALL_FILES | scan_old_faces)"

EXECS=$(for f in $ALL_FILES; do case $f in (we_child.c) ;; (*) echo "$f" ;; esac; done)
# shellcheck disable=SC2086
report 6 "an exec outside we_child.c:" "$(blank $EXECS | scan_exec)"

# Rule 7: the gate.
gate=$(awk '/^wpe_child_unplanned|^static int wpe_child_unplanned|^static int wpe_child_uncontained/ { on = 1 }
            on { print } /^}/ { on = 0 }' we_child.c)
case $gate in
  *WPE_CHILD_ORIGIN_AI*) ;;
  *) report 7 "wpe_child_unplanned does not judge an assistant start (WPE_CHILD_ORIGIN_AI):" "we_child.c" ;;
esac
case $gate in
  *"->contain"*) ;;
  *) report 7 "the start gate does not read the containment:" "we_child.c" ;;
esac

if [ "$fail" != 0 ]; then
  exit 1
fi
echo "check-contain-sites: ok"
exit 0
