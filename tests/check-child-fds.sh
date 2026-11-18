#!/bin/sh
# check-child-fds.sh -- one owner decides which descriptors a program the
# editor starts holds.
#
# A program started by the editor inherits every descriptor the editor has
# without close-on-exec: what whatever started the editor left open, and
# what the editor's libraries opened.  The owner of child starts (we_child.c)
# closes them in one step between its dup2 of 0, 1 and 2 and the exec.  That
# only holds while there is one exec, the step cannot be skipped on the way
# to it, and the calls that close a span are in one function.
#
#   check-child-fds.sh [--srcdir DIR] [--allow FILE]
#
# Rules, on the top level sources and tests/*.c, comments and the text of
# string literals blanked, a call read by name in the function it stands in (a function
# starts at a line in column 0 that names an identifier followed by "(" and
# does not end in ";"):
#  F1. One exec site: a call of execl, execlp, execle, execlpe, execv, execvp,
#      execve or execvpe appears in we_child.c once, in wpe_child_exec, and
#      nowhere else but the lines tests/child-fds.allow counts.
#  F2. The step cannot be skipped: wpe_child_exec calls wpe_child_close_others
#      once, after its wpe_child_io call and before its exec; no other
#      function calls it.  With F1 there is no way to an exec without it.
#  F3. One closer: close_range and closefrom are called in we_child.c, in
#      wpe_child_close_span, and nowhere else.  (tests/check-one-owner.sh
#      holds the same for the objects, so a macro cannot hide a second one.)
#
# Canaries: offenders planted in files of their own, an exec in a test, a
# copy of wpe_child_exec without the step, the step after the exec and before
# the io, a close_range outside its function; each must be named, and calls
# that are fine (a comment, a string, a longer name, the well-formed owner)
# must stay silent.  A scanner that misses a canary fails the script as blind.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
BAD=1
ALLOW=

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --allow) shift; ALLOW=$1 ;;
    *) echo "check-child-fds: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done

[ -n "$ALLOW" ] || ALLOW=$SRCDIR/tests/child-fds.allow
if [ ! -f "$ALLOW" ]; then
  echo "check-child-fds: no allow file at $ALLOW" >&2
  exit $BAD
fi

. "$(dirname "$0")/workdir.sh"
work_dir child-fds $BAD

# scan FILE... - one tab separated record per call that matters, "base
# function line call", and one "base - 0 file" per file read.
scan() {
  awk '
    function base(p) { sub(/^.*\//, "", p); return p }
    function record(piece,   name) {
      name = piece
      sub(/^[^A-Za-z_]/, "", name)
      sub(/[ \t]*\($/, "", name)
      printf "%s\t%s\t%d\t%s\n", base(file), fn, FNR, name
    }
    FNR == 1 { file = FILENAME; fn = "(file scope)"; incom = 0
               printf "%s\t-\t0\tfile\n", base(file) }
    {
      line = $0; out = ""; n = length(line); i = 1; instr = 0
      while (i <= n) {
        c = substr(line, i, 1); d = substr(line, i, 2)
        if (incom) { if (d == "*/") { incom = 0; i += 2 } else i++; continue }
        if (instr) {
          # A call is never inside a string: blank its text, keep the quotes.
          if (c == "\\") { out = out "  "; i += 2; continue }
          if (c == "\"") { out = out c; instr = 0 }
          else out = out " "
          i++; continue
        }
        if (d == "/*") { incom = 1; i += 2; continue }
        if (d == "//") break
        if (c == "\"") instr = 1
        out = out c; i++
      }
      if (out ~ /^[A-Za-z_]/ && out ~ /[(]/ && out !~ /;[ \t]*$/ &&
          out !~ /[{]/ && out !~ /^typedef/) {
        name = out; sub(/[ \t]*[(].*/, "", name); sub(/.*[^A-Za-z0-9_]/, "", name)
        if (name != "") fn = name
        next
      }
      s = out
      while (match(s, /(^|[^A-Za-z0-9_.>])(exec(l|lp|le|lpe|v|vp|ve|vpe)|wpe_child_io|wpe_child_close_others|close_range|closefrom)[ \t]*\(/)) {
        record(substr(s, RSTART, RLENGTH))
        s = substr(s, RSTART + RLENGTH)
      }
    }' "$@"
}

# judge ALLOWFILE RECORDS - what is wrong with the calls of RECORDS: one
# problem a line, nothing when all is as the rules say.  The allow file's
# lines are "file|call|count|keep|reason" and count the exec calls outside
# the owner that are meant.
judge() {
  printf '%s\n' "$2" | awk -F'\t' -v allow="$1" '
    BEGIN {
      while ((getline line < allow) > 0) {
        if (line ~ /^[ \t]*(#|$)/) continue
        n = split(line, f, "|")
        if (n < 5 || f[3] !~ /^[0-9]+$/ || f[4] == "" || f[5] == "") { print "bad line in the allow file: " line; continue }
        key = f[1] "|" f[2]
        if (key in al) { print "duplicate allow line for " key; continue }
        al[key] = f[3]
      }
    }
    NF < 4 { next }
    $4 == "file" { if ($1 == "we_child.c") owner = 1; next }
    $4 ~ /^exec/ {
      if ($1 == "we_child.c") {
        if ($2 == "wpe_child_exec") { nexec++; execat = $3 }
        else print "F1: " $4 " in " $1 " function " $2 " line " $3 ": the one exec is in wpe_child_exec"
      } else {
        key = $1 "|" $4
        seen[key]++
        if (!(key in al)) print "F1: " $4 " in " $1 " function " $2 " line " $3 ": a program is started outside the owner"
      }
      next
    }
    $4 == "wpe_child_io" {
      if ($1 == "we_child.c" && $2 == "wpe_child_exec") { nio++; ioat = $3 }
      next
    }
    $4 == "wpe_child_close_others" {
      if ($1 == "we_child.c" && $2 == "wpe_child_exec") { ncl++; clat = $3 }
      else print "F2: the close step is called in " $1 " function " $2 " line " $3 ", not in wpe_child_exec"
      next
    }
    $4 == "close_range" || $4 == "closefrom" {
      if (!($1 == "we_child.c" && $2 == "wpe_child_close_span"))
        print "F3: " $4 " in " $1 " function " $2 " line " $3 ": only wpe_child_close_span closes a span"
      next
    }
    END {
      for (k in seen) if (k in al) {
        if (seen[k] > al[k] + 0) print "more than allowed: " k " has " seen[k] ", the allow file says " al[k]
        else if (seen[k] < al[k] + 0) print "fewer than the allow file says: " k " has " seen[k] ", lower the line"
      }
      for (k in al) if (!(k in seen)) print "stale allow line, delete it: " k
      if (owner) {
        if (nexec != 1) print "F1: wpe_child_exec of we_child.c has " nexec + 0 " exec calls, not one"
        if (nio != 1) print "F2: wpe_child_exec has " nio + 0 " wpe_child_io calls, not one"
        if (ncl != 1) print "F2: wpe_child_exec has " ncl + 0 " calls of wpe_child_close_others, not one: a start can run a program that holds what the editor inherited"
        else if (nio == 1 && nexec == 1) {
          if (!(ioat < clat)) print "F2: wpe_child_close_others (line " clat ") is not after wpe_child_io (line " ioat "): the pipe ends it must keep would be closed first"
          if (!(clat < execat)) print "F2: wpe_child_close_others (line " clat ") is not before the exec (line " execat ")"
        }
      }
    }' | sort
}

fail=0
files=$(ls "$SRCDIR"/*.c "$SRCDIR"/*.h "$SRCDIR"/tests/*.c "$SRCDIR"/tests/*.h 2>/dev/null)
if [ -z "$files" ]; then
  echo "check-child-fds: no sources under $SRCDIR" >&2
  exit $BAD
fi
if [ ! -f "$SRCDIR/we_child.c" ]; then
  echo "check-child-fds: no we_child.c under $SRCDIR" >&2
  exit $BAD
fi
# shellcheck disable=SC2086
found=$(scan $files)
left=$(judge "$ALLOW" "$found")
if [ -n "$left" ]; then
  echo "check-child-fds: a program started, or a span closed, outside the owner's one path, or an allow line that is off:"
  echo "$left"
  fail=1
fi

# A well-formed owner, the control of every canary below.
cat > "$WORK/owner.c" <<'EOF'
static void wpe_child_close_span(int lo, int hi, int ceiling)
{
 if (close_range(lo, ~0U, 0) == 0)
  return;
 closefrom(lo);
}

static void wpe_child_exec(const WpeChildSpec *spec)
{
 wpe_child_io(spec);
 wpe_child_close_others(k);
 execvp(spec->argv[0], spec->argv);
}
EOF

# canary NAME FILE EXPECT - scan the planted FILE (named so that the
# judge reads it as we_child.c or as another file); EXPECT is 1 when
# the judge must name something, 0 when it must stay silent.
canary() {
  got=$(judge "$ALLOW_EMPTY" "$(scan "$2")")
  if [ "$3" = 1 ] && [ -z "$got" ]; then
    echo "canary $1: a planted offender was not found"
    return 1
  fi
  if [ "$3" = 0 ] && [ -n "$got" ]; then
    echo "canary $1: a call that is fine was named:"
    echo "$got"
    return 1
  fi
  return 0
}

ALLOW_EMPTY=$WORK/allow-empty
: > "$ALLOW_EMPTY"
mkdir "$WORK/owner" "$WORK/bad"
blind=0

# The control: the owner as it should be is silent, in a file named we_child.c.
cp "$WORK/owner.c" "$WORK/owner/we_child.c"
canary control "$WORK/owner/we_child.c" 0 || blind=1

# F1: an exec in a test, and a second exec in the owner.
printf '%s\n' 'int f(char **argv) { execv(argv[0], argv); return 1; }' > "$WORK/bad/test_x.c"
canary exec-in-a-test "$WORK/bad/test_x.c" 1 || blind=1
printf '%s\n' 'void f(char **a) {
 execlp(a[0], a[0], (char *)0);
}' > "$WORK/bad/test_y.c"
canary execlp-two-lines "$WORK/bad/test_y.c" 1 || blind=1
{ cat "$WORK/owner.c"; printf '%s\n' 'static void other(char **a)
{
 execve(a[0], a, 0);
}'; } > "$WORK/bad/we_child.c"
canary second-exec-in-the-owner "$WORK/bad/we_child.c" 1 || blind=1

# F2: the owner's exec without the step, the step after the exec, before the
# io, called twice, and called from another function.
printf '%s\n' 'static void wpe_child_exec(const WpeChildSpec *spec)
{
 wpe_child_io(spec);
 execvp(spec->argv[0], spec->argv);
}' > "$WORK/bad/we_child.c"
canary no-step "$WORK/bad/we_child.c" 1 || blind=1
printf '%s\n' 'static void wpe_child_exec(const WpeChildSpec *spec)
{
 wpe_child_io(spec);
 execvp(spec->argv[0], spec->argv);
 wpe_child_close_others(k);
}' > "$WORK/bad/we_child.c"
canary step-after-exec "$WORK/bad/we_child.c" 1 || blind=1
printf '%s\n' 'static void wpe_child_exec(const WpeChildSpec *spec)
{
 wpe_child_close_others(k);
 wpe_child_io(spec);
 execvp(spec->argv[0], spec->argv);
}' > "$WORK/bad/we_child.c"
canary step-before-io "$WORK/bad/we_child.c" 1 || blind=1
printf '%s\n' 'static void wpe_child_exec(const WpeChildSpec *spec)
{
 wpe_child_io(spec);
 wpe_child_close_others(k);
 wpe_child_close_others(k);
 execvp(spec->argv[0], spec->argv);
}' > "$WORK/bad/we_child.c"
canary step-twice "$WORK/bad/we_child.c" 1 || blind=1
{ cat "$WORK/owner.c"; printf '%s\n' 'static void other(void)
{
 wpe_child_close_others(k);
}'; } > "$WORK/bad/we_child.c"
canary step-elsewhere "$WORK/bad/we_child.c" 1 || blind=1

# F3: a closer outside its function, in another file and in the owner.
printf '%s\n' 'void f(void) { close_range(3, ~0U, 0); }' > "$WORK/bad/we_other.c"
canary close-range-elsewhere "$WORK/bad/we_other.c" 1 || blind=1
printf '%s\n' 'void f(void) { closefrom(3); }' > "$WORK/bad/we_other2.c"
canary closefrom-elsewhere "$WORK/bad/we_other2.c" 1 || blind=1
{ cat "$WORK/owner.c"; printf '%s\n' 'static void other(void)
{
 closefrom(3);
}'; } > "$WORK/bad/we_child.c"
canary closer-in-another-function "$WORK/bad/we_child.c" 1 || blind=1

# Calls that are fine: a comment, a string, a longer name, a member, a
# definition of a function whose name only starts like one.
printf '%s\n' 'int f(void) { /* execvp(a, b) and close_range(3, 4, 0) */ return 0; }' > "$WORK/bad/we_c1.c"
canary comment "$WORK/bad/we_c1.c" 0 || blind=1
printf '%s\n' 'int f(void) { return puts("execvp(a) closefrom(3)"); }' > "$WORK/bad/we_c2.c"
canary string "$WORK/bad/we_c2.c" 0 || blind=1
printf '%s\n' 'static void wpe_child_exec_failed(const char *w)
{
 wpe_child_exec_failed_say(w);
 my_execvp(w);
}' > "$WORK/bad/we_c3.c"
canary longer-name "$WORK/bad/we_c3.c" 0 || blind=1
printf '%s\n' 'int f(struct t *p) { return p->closefrom(3) + q.close_range(1, 2, 0); }' > "$WORK/bad/we_c4.c"
canary member "$WORK/bad/we_c4.c" 0 || blind=1
printf '%s\n' 'static void wpe_child_io_pipes(const WpeChildSpec *spec)
{
 wpe_child_io_other(spec);
}' > "$WORK/bad/we_c5.c"
canary io-longer-name "$WORK/bad/we_c5.c" 0 || blind=1

# The counted exception: a planted exec in a test is silent with its allow
# line, named without it, and named when the file allows fewer or more.
printf '%s\n' 'int f(void) { return execl("/bin/true", "true", (char *)0); }' > "$WORK/bad/test_z.c"
planted=$(scan "$WORK/bad/test_z.c")
printf '%s\n' 'test_z.c|execl|1|keep|planted' > "$WORK/allow1"
printf '%s\n' 'test_z.c|execl|0|keep|planted' > "$WORK/allow0"
printf '%s\n' 'test_z.c|execl|2|keep|planted' > "$WORK/allow2"
printf '%s\n' 'test_w.c|execl|1|keep|planted' > "$WORK/allowx"
[ -n "$planted" ] && [ -z "$(judge "$WORK/allow1" "$planted")" ] || { echo "canary allow: a listed call was named or not found"; blind=1; }
[ -n "$(judge "$WORK/allow0" "$planted")" ] || { echo "canary allow: a call above its count was not named"; blind=1; }
[ -n "$(judge "$WORK/allow2" "$planted")" ] || { echo "canary allow: a count above the calls was not named"; blind=1; }
[ -n "$(judge "$WORK/allowx" "$planted")" ] || { echo "canary allow: a call in another file was not named"; blind=1; }
[ -n "$(judge "$WORK/allow1" "")" ] || { echo "canary allow: a stale line was not named"; blind=1; }
if [ $blind -ne 0 ]; then
  echo "check-child-fds: the scanner is blind"
  exit $BAD
fi

[ $fail -eq 0 ] && echo "check-child-fds: clean"
exit $fail
