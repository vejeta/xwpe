#!/bin/sh
# check-ai-tool-step.sh -- how a tool step of the assistant ended has one
# owner, we_ai_step.h.
#
# A tool step either answers now (the model reads the text next turn) or
# starts a page or a command on the loop and answers later.  Telling the two
# apart by the identity of a returned pointer (a static marker against
# malloc'd text) is one compiler inference from freeing the static: GCC
# decided the function returned fresh heap, folded the compare away, and the
# editor ended in free().  The outcome is an enum and the text travels through
# an out-parameter, so no pointer can stand for an outcome.
#
#   check-ai-tool-step.sh [--allow FILE] [--srcdir DIR] [--list] [--selftest]
#
# Rules (comments are blanked, literals emptied, then each line is read):
#  R1 No sentinel compare.  In the top-level sources, no == or != has on
#     either side a file-scope array, static or not, taken whole (name,
#     &name, &name[0]), the address of a file-scope static, or a macro of
#     one.  A cast on either side is stripped first.
#  R2 No static through a freeable pointer.  A function whose return type is
#     char *, unsigned char * or void * (not const) returns no file-scope
#     array, no address of a static, no member of a static object, no string
#     literal, no pointer arithmetic on one of those and no macro of any.
#     A sentinel passed through a local (char *r = mark; return r;) is not
#     seen here; R1 and R6 guard that case.  Known limits: a compare made in
#     a helper function (same_text(a, mark)) and one made on a struct field
#     (x.p == mark) are not seen.
#  R3 One writer of the outcome.  No AI_STEP_ constant appears in a return or
#     an assignment outside we_ai_step.h; a function typed enum ai_tool_step
#     returns a call or a variable.
#  R4 One writer of the text.  A function typed enum ai_tool_step does not
#     assign *text itself; it calls ai_step_result, ai_step_waits or
#     ai_step_no_tool, or another step function.
#  R5 The outcome is not dropped.  A call of a step function is the operand
#     of return, is assigned to a variable, or is an argument; never a
#     statement of its own.
#  R6 The old marker stays gone: the words of the removed sentinel appear in
#     no source or test.
#  R7 ai_step_no_tool is named only where the allow file lists it: the
#     no-tool-line branch of the chat's tool step and its turn budget.  The
#     word is counted wherever it stands (a call, a macro, a function pointer).
#  R8 A function that reads an outcome and is not itself a step function
#     calls ai_page_forget on a path where the step did not wait, so the page
#     owner names a task only while one waits; a call inside the block that
#     == AI_STEP_WAITS guards does not count.  An outcome is never compared
#     with a number.
#
# The allow file lists, one per line,
#   rule|file|function|expression|count|reason
# the offenders that stay: borrowed texts (R2), a position test (R1), the two
# places that may say "no tool" (R7).  A count only falls; a line that
# matches nothing is stale and fails.  Key a line by file, function and
# expression, never by a line number.
#
# Canaries: each rule's scan over a planted offender must name it, and over a
# planted clean line must not; a scan that fails either is wrong and fails the
# script.  --selftest runs only them.  --list prints what the scan found as
# allow lines.
#
# Exit: 0 clean, 1 offender, stale line or a wrong scanner.

SRCDIR=${srcdir:-.}
HERE=$(cd "$(dirname "$0")" && pwd)
ALLOW=
LIST=0
SELFTEST=0
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --allow) shift; ALLOW=$1 ;;
    --srcdir) shift; SRCDIR=$1 ;;
    --list) LIST=1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "check-ai-tool-step: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/ai-tool-step.allow

. "$HERE/workdir.sh"
work_dir ai-tool-step $BAD
BLANK_C=$(cat "$HERE/blank-c.awk")

# The names a call may not drop: the owner's three and every function typed
# enum ai_tool_step in the files given.
STEP_BASE='ai_step_result|ai_step_waits|ai_step_no_tool'
step_names() {
  names=$(awk '
    /^(static[ \t]+)?enum[ \t]+ai_tool_step[ \t]+[A-Za-z_0-9]+[ \t]*\(/ {
      s = $0; sub(/^(static[ \t]+)?enum[ \t]+ai_tool_step[ \t]+/, "", s); sub(/[ \t]*\(.*/, "", s); print s
    }' "$@" | sort -u | awk '{ printf "%s%s", sep, $0; sep = "|" } END { print "" }')
  if [ -z "$names" ]; then
    echo "$STEP_BASE"
  else
    echo "$STEP_BASE|$names"
  fi
}

# scan FILE... : the findings of every rule over the files, as
# "rule|file|function|expression", one per occurrence.  $NAMES is the step
# function names, as an awk alternation.
SCAN='
function trim(s) { sub(/^[[:space:]]+/, "", s); sub(/[[:space:]]+$/, "", s); return s }
function emit(rule, expr) { print rule "|" base "|" (fn == "" ? "-" : fn) "|" expr }

# the position of the ")" that closes the "(" at the start of e, or 0
function matching_close(e,   n, ch, i, d) {
  n = split(e, ch, ""); d = 0
  for (i = 1; i <= n; i++) {
    if (ch[i] == "(") d++
    else if (ch[i] == ")") { d--; if (d == 0) return i }
  }
  return 0
}
# the position of the first want outside any bracket of e, or 0
function top_pos(e, want,   n, ch, i, d) {
  n = split(e, ch, ""); d = 0
  for (i = 1; i <= n; i++) {
    if (ch[i] == "(" || ch[i] == "[") d++
    else if (ch[i] == ")" || ch[i] == "]") d--
    else if (d == 0 && ch[i] == want) return i
  }
  return 0
}

# Is this expression a file-scope static taken whole, the address of one, a
# string literal, or a macro of those?  Returns "" when it is not.
function atom_static(a, depth,   amp, idx0, core) {
  a = trim(a)
  if (depth > 6) return ""
  if (a ~ /^""([[:space:]]*"")*$/) return "literal"
  amp = 0; idx0 = 0
  if (a ~ /^&/) { amp = 1; sub(/^&[[:space:]]*/, "", a) }
  if (a ~ /\[[[:space:]]*0[[:space:]]*\]$/) { idx0 = 1; sub(/\[[[:space:]]*0[[:space:]]*\]$/, "", a) }
  if (a !~ /^[A-Za-z_][A-Za-z0-9_]*$/) return ""
  if (idx0 && !amp) return ""
  if (a in isarr) return "static"
  if (amp && (a in isscal)) return "static"
  if (!amp && !idx0 && (a in macbody)) return atom_static(macbody[a], depth + 1)
  return ""
}

# Judge what a function returns: a freeable text pointer returns no static.
function judge_text(e, depth,   q, c) {
  e = trim(e)
  if (depth > 8 || e == "") return
  while (e ~ /^\(/ && matching_close(e) == length(e)) e = trim(substr(e, 2, length(e) - 2))  # linear: one expression, short
  if (e ~ /^\([A-Za-z_][A-Za-z0-9_ ]*\*+[[:space:]]*\)/) {
    sub(/^\([^)]*\)[[:space:]]*/, "", e)
    judge_text(e, depth + 1); return
  }
  q = top_pos(e, "?")
  if (q > 0) {
    c = top_pos(substr(e, q + 1), ":")
    if (c == 0) { judge_text(substr(e, q + 1), depth + 1); return }
    judge_text(substr(e, q + 1, c - 1), depth + 1)
    judge_text(substr(e, q + 1 + c), depth + 1)
    return
  }
  # pointer arithmetic on a static is still the static: mark + 0, 1 + mark
  q = top_pos(e, "+")
  if (q > 1 && substr(e, q + 1, 1) != "+") {
    judge_text(substr(e, 1, q - 1), depth + 1)
    judge_text(substr(e, q + 1), depth + 1)
    return
  }
  # a member of a static object: the member may be an array of it
  if (e ~ /^[A-Za-z_][A-Za-z0-9_]*(\.|->)[A-Za-z_][A-Za-z0-9_]*$/) {
    c = e; sub(/(\.|->).*/, "", c)
    if ((c in isarr) || (c in isscal)) emit("R2", e)
    return
  }
  if (atom_static(e, 0) != "") emit("R2", e)
}

# the position of the word return in s, or 0
function return_pos(s,   p, b, a) {
  p = index(s, "return")
  if (!p) return 0
  b = (p == 1) ? "" : substr(s, p - 1, 1)
  a = substr(s, p + 6, 1)
  if ((b == "" || b !~ /[A-Za-z0-9_]/) && (a == "" || a !~ /[A-Za-z0-9_]/)) return p
  return 0
}

# One finished return statement: the expression after the word.
function one_return(expr) {
  if (expr ~ /AI_STEP_/ && base != "we_ai_step.h") emit("R3", "return " trim(expr))
  if (fn_is_step && trim(expr) !~ /^[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(/ && trim(expr) !~ /^[a-z_][a-z0-9_]*$/ && expr !~ /AI_STEP_/)
    emit("R3", "return " trim(expr))
  if (fn_is_text) judge_text(expr, 0)
}

# A macro: remember its body, for the macros of a static or a literal.
function note_macro(L,   s, nm, body) {
  s = L; sub(/^#[[:space:]]*define[[:space:]]+/, "", s)
  nm = s; sub(/[^A-Za-z0-9_].*/, "", nm)
  if (nm == "" || substr(s, length(nm) + 1, 1) == "(") return
  body = trim(substr(s, length(nm) + 1))
  if (body != "") macbody[nm] = body
}

# A file-scope static: a name to watch in the compares.
function note_static(L,   s) {
  if (L ~ /^[A-Za-z_][^(={;]*[^A-Za-z0-9_][A-Za-z_][A-Za-z0-9_]*\[/) {
    s = L; sub(/\[.*/, "", s); sub(/.*[^A-Za-z0-9_]/, "", s); isarr[s] = 1
  } else if (L ~ /^[A-Za-z_][^(={]*[^A-Za-z0-9_*][A-Za-z_][A-Za-z0-9_]*[[:space:]]*(=|;)/) {
    s = L; sub(/[[:space:]]*(=.*|;.*)$/, "", s); sub(/.*[^A-Za-z0-9_]/, "", s); isscal[s] = 1
  } else if (L ~ /^\}[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*;/) {
    s = L; sub(/^\}[[:space:]]*/, "", s); sub(/[[:space:]]*;.*/, "", s); isscal[s] = 1
  }
}

# R1 over one line: a compare with a static array taken whole or the address
# of a static.
function compares(L,   nm, re) {
  if (L !~ /==|!=/ || L ~ /^[[:space:]]*#/) return
  # a cast on either side is no disguise: (uintptr_t)mark, (char *)mark
  gsub(/\([A-Za-z_][A-Za-z0-9_ ]*\**[[:space:]]*\)[[:space:]]*/, "", L)
  for (nm in isarr) {
    if (index(L, nm) == 0) continue
    re = "(==|!=)[[:space:]]*(&[[:space:]]*" nm "([[:space:]]*\\[[[:space:]]*0[[:space:]]*\\])?|" nm ")([^A-Za-z0-9_[]|$)"
    if (L ~ re) emit("R1", nm)
    re = "(^|[^A-Za-z0-9_.>\\]&])" nm "[[:space:]]*(==|!=)"
    if (L ~ re) emit("R1", nm)
    re = "&[[:space:]]*" nm "([[:space:]]*\\[[[:space:]]*0[[:space:]]*\\])?[[:space:]]*(==|!=)"
    if (L ~ re) emit("R1", nm)
  }
  for (nm in isscal) {
    if (index(L, nm) == 0) continue
    re = "(==|!=)[[:space:]]*&" nm "([^A-Za-z0-9_]|$)"
    if (L ~ re) emit("R1", "&" nm)
    re = "(^|[^A-Za-z0-9_&])&" nm "[[:space:]]*(==|!=)"
    if (L ~ re) emit("R1", "&" nm)
  }
  for (nm in macbody) {
    if (index(L, nm) == 0 || atom_static(nm, 0) == "") continue
    re = "(==|!=)[[:space:]]*" nm "([^A-Za-z0-9_]|$)"
    if (L ~ re) emit("R1", nm)
    re = "(^|[^A-Za-z0-9_])" nm "[[:space:]]*(==|!=)"
    if (L ~ re) emit("R1", nm)
  }
}

# R5 over one line: a step function called as a statement of its own.
function dropped(L, in_fn,   rest, pre, re, p, call) {
  if (!in_fn) return
  rest = L
  re = "(^|[^A-Za-z0-9_])(" names ")[[:space:]]*\\("
  while (match(rest, re)) {
    p = RSTART + (substr(rest, RSTART, 1) ~ /[A-Za-z0-9_]/ ? 0 : 1)
    pre = trim(substr(rest, 1, p - 1))
    call = substr(rest, p, RLENGTH - (p - RSTART))
    sub(/[[:space:]]*[(]$/, "", call)
    if (pre == "" || pre ~ /[;{})]$/ || pre ~ /(^|[^A-Za-z0-9_])else$/)
      emit("R5", call)
    rest = substr(rest, RSTART + RLENGTH)
  }
}

function end_function() {
  if (fn != "" && !fn_is_step && fn_has_step && !fn_has_forget) emit("R8", "AI_STEP_")
  fn = ""; fn_is_step = 0; fn_is_text = 0; pend = 0
  delete evar
}

# R8 over one line of a function: does the page owner get forgotten on a path
# where the step did not wait?  A call inside the block (or the one statement)
# an == AI_STEP_WAITS guards does not count: that path leaves the task waiting.
function track_forget(L,   nopen, nclose, tmp, guarded, d0) {
  d0 = depth
  guarded = (wdepth > 0) || wnext
  if (L ~ /==[[:space:]]*AI_STEP_WAITS/ || L ~ /AI_STEP_WAITS[[:space:]]*==/) {
    if (L ~ /ai_page_forget[[:space:]]*\(/) guarded = 1
    if (L ~ /\{[[:space:]]*$/) { if (wdepth == 0) wdepth = d0 + 1 }
    else if (L !~ /;[[:space:]]*$/) wnext = 2
  }
  if (L ~ /AI_STEP_/ && base != "we_ai_step.h") fn_has_step = 1
  if (L ~ /ai_page_forget[[:space:]]*\(/ && !guarded) fn_has_forget = 1
  if (wnext == 1) {
    if (L ~ /^[[:space:]]*\{[[:space:]]*$/) wdepth = d0 + 1
    wnext = 0
  } else if (wnext == 2) wnext = 1
  tmp = L; nopen = gsub(/\{/, "{", tmp)
  tmp = L; nclose = gsub(/\}/, "}", tmp)
  depth = depth + nopen - nclose
  if (wdepth > 0 && depth < wdepth) wdepth = 0
}

# R8: an outcome is read by its name, never by a number.
function numeric_compare(L,   v, re) {
  for (v in evar) {
    re = "(^|[^A-Za-z0-9_])" v "[[:space:]]*(==|!=)[[:space:]]*[0-9]"
    if (L ~ re) emit("R8", "numeric compare")
    re = "[0-9][[:space:]]*(==|!=)[[:space:]]*" v "([^A-Za-z0-9_]|$)"
    if (L ~ re) emit("R8", "numeric compare")
  }
  re = "(" names ")[[:space:]]*\\([^;]*\\)[[:space:]]*(==|!=)[[:space:]]*[0-9]"
  if (L ~ re) emit("R8", "numeric compare")
  if (L ~ /enum[[:space:]]+ai_tool_step[[:space:]]+[A-Za-z_]/) {
    v = L; sub(/.*enum[[:space:]]+ai_tool_step[[:space:]]+/, "", v); sub(/[^A-Za-z0-9_].*/, "", v)
    if (v != "") evar[v] = 1
  }
}

FNR == 1 {
  base = FILENAME; sub(/.*\//, "", base)
  blank_incom = 0
  delete isarr; delete isscal; delete macbody
  fn = ""; cand = ""; candtype = ""; typeline = ""; pend = 0; stmt = ""
  fn_is_step = 0; fn_is_text = 0
}

{
  L = blank_c($0, 0)
  # R7: the word is counted wherever it stands (a call, a macro, a pointer)
  if (base != "we_ai_step.h" && L ~ /(^|[^A-Za-z0-9_])ai_step_no_tool([^A-Za-z0-9_]|$)/) emit("R7", "ai_step_no_tool")
  if (L ~ /^[[:space:]]*#[[:space:]]*define[[:space:]]/) { note_macro(L); next }
  if (L ~ /^[[:space:]]*#/) next
  if (L ~ /^[[:space:]]*$/) next

  if (fn == "") {
    note_static(L)
    compares(L)
    # a function header, at the first column, with a "(" and no ";" at its end
    if (L ~ /^[A-Za-z_]/ && L ~ /[(]/ && L !~ /;[[:space:]]*$/ && L !~ /^(typedef|struct|union)/) {
      hdr = L; sub(/[(].*/, "", hdr); hdr = trim(hdr)
      nm = hdr; sub(/.*[^A-Za-z0-9_]/, "", nm)
      candtype = substr(hdr, 1, length(hdr) - length(nm))
      if (trim(candtype) == "") candtype = typeline
      cand = nm
    } else if (cand != "" && L ~ /^[[:space:]]/ && L ~ /;[[:space:]]*$/) {
      cand = ""
    } else if (cand != "" && L ~ /^\{/) {
      fn = cand; cand = ""
      fn_is_step = (candtype ~ /enum[[:space:]]+ai_tool_step/)
      fn_is_text = (candtype !~ /const/ && candtype ~ /(char|void)[[:space:]]*\*[[:space:]]*$/ && candtype !~ /\*[[:space:]]*\*[[:space:]]*$/)
      fn_has_step = 0; fn_has_forget = 0; pend = 0
      depth = 1; wdepth = 0; wnext = 0
    }
    if (L ~ /^[A-Za-z_][A-Za-z0-9_ ]*\**[[:space:]]*$/) typeline = L; else typeline = ""
    next
  }

  # inside a function
  if (L ~ /^\}/) { end_function(); next }
  compares(L)
  if (L ~ /AI_STEP_/ && base != "we_ai_step.h") fn_has_step = 1
  track_forget(L)
  numeric_compare(L)
  if (L ~ /(^|[^=!<>])=[[:space:]]*AI_STEP_/ && base != "we_ai_step.h") emit("R3", "= AI_STEP_")
  if (fn_is_step && base != "we_ai_step.h" && L ~ /[*][[:space:]]*text[[:space:]]*=([^=]|$)/) emit("R4", "*text =")
  dropped(L, 1)

  if (pend) {
    stmt = stmt " " L  # linear: one statement, a few lines
    if (index(L, ";")) { sub(/;.*/, "", stmt); one_return(stmt); pend = 0 }
    next
  }
  p = return_pos(L)
  if (p) {
    stmt = substr(L, p + 6)
    if (index(stmt, ";")) { sub(/;.*/, "", stmt); one_return(stmt) } else pend = 1
  }
}
'

# scan_files NAMES FILE... : the findings over the files.
scan_files() {
  names=$1; shift
  awk -v names="$names" "$BLANK_C$SCAN" "$@"
}

# R6 over the files given: the words of the removed marker.
scan_old_marker() {
  grep -H -o -E '(AI_TOOL_WAITS|ai_tool_waits)' "$@" 2>/dev/null |
    awk -F: '{ f = $1; sub(/.*\//, "", f); print "R6|" f "|-|" $2 }'
}

# --- the canaries -------------------------------------------------------
# fixture NAME BODY-FILE : run the scan over one planted file.
planted_dir() { mkdir -p "$WORK/plant" && echo "$WORK/plant"; }

expect_found() {
  # expect_found RULE EXPR WHY : the scan over $WORK/plant/c.c must name it
  if ! scan_files "$STEP_BASE|ai_run" "$WORK/plant/c.c" | grep -q -F "$1|c.c|$3|$2"; then
    echo "check-ai-tool-step: rule $1 is blind to a planted offender ($4): $2" >&2
    return 1
  fi
}
expect_clean() {
  # expect_clean RULE WHY : the scan over $WORK/plant/c.c names nothing of RULE
  if scan_files "$STEP_BASE|ai_run" "$WORK/plant/c.c" | grep -q "^$1|"; then
    echo "check-ai-tool-step: rule $1 names a line that is fine ($2)" >&2
    scan_files "$STEP_BASE|ai_run" "$WORK/plant/c.c" | grep "^$1|" >&2
    return 1
  fi
}

selftest() {
  bad=0
  planted_dir > /dev/null
  # R1: a compare with a static array, with its macro, and with the address of a static
  cat > "$WORK/plant/c.c" <<'EOF'
static char planted_mark[] = "(marker)";
static int planted_obj;
#define PLANTED_MACRO planted_mark
int cmp(char *r, char *p)
{
 if (r == planted_mark)
  return 1;
 if (p != PLANTED_MACRO)
  return 2;
 if (&planted_obj != (int *)p)
  return 3;
 if (p == &planted_mark[0])
  return 4;
 return 0;
}
EOF
  expect_found R1 planted_mark cmp "a compare with a static array" || bad=1
  expect_found R1 PLANTED_MACRO cmp "a compare with a macro of one" || bad=1
  expect_found R1 '&planted_obj' cmp "a compare with the address of a static" || bad=1
  cat > "$WORK/plant/c.c" <<'EOF'
static char planted_mark[] = "(marker)";
int ok(char *r, int n)
{
 if (planted_mark[0] == r[0])
  return 1;
 if (r == NULL || n == 0)
  return 2;
 return 0;
}
EOF
  expect_clean R1 "a char of the array, a plain compare" || bad=1
  # R2: a static, a literal, a macro and the K&R form through a char *
  cat > "$WORK/plant/c.c" <<'EOF'
static char planted_mark[] = "(marker)";
#define PLANTED_MACRO planted_mark
#define PLANTED_WORDS "words"
static char *ret_static(void)
{
 return planted_mark;
}
static char *ret_literal(void)
{
 return "text";
}
static char *ret_macro(void)
{
 return PLANTED_MACRO;
}
static char *ret_words(void)
{
 return PLANTED_WORDS;
}
char *ret_knr(void)
{
 return(planted_mark);
}
char *ret_slot(void)
{
 return &planted_mark[0];
}
char *ret_ternary(int c)
{
 return c ? planted_mark : NULL;
}
EOF
  expect_found R2 planted_mark ret_static "a static through char *" || bad=1
  expect_found R2 '""' ret_literal "a literal through char *" || bad=1
  expect_found R2 PLANTED_MACRO ret_macro "a macro of a static" || bad=1
  expect_found R2 PLANTED_WORDS ret_words "a macro of a literal" || bad=1
  expect_found R2 planted_mark ret_knr "the K&R return(x)" || bad=1
  expect_found R2 '&planted_mark[0]' ret_slot "the address of the first element" || bad=1
  expect_found R2 planted_mark ret_ternary "a static in a ternary branch" || bad=1
  cat > "$WORK/plant/c.c" <<'EOF'
static char planted_mark[] = "(marker)";
static const char *ret_const(void)
{
 return "text";
}
static char *ret_heap(void)
{
 return strdup("text");
}
static char *ret_arg(char *p)
{
 return p;
}
static int ret_number(void)
{
 return planted_mark[0];
}
EOF
  expect_clean R2 "a const return, a copy, an argument, a number" || bad=1
  # R3: a constant returned or assigned, outside the header
  cat > "$WORK/plant/c.c" <<'EOF'
static enum ai_tool_step ai_run(char **text)
{
 return AI_STEP_WAITS;
}
static enum ai_tool_step ai_run_two(char **text)
{
 enum ai_tool_step k = AI_STEP_RESULT;
 return k;
}
EOF
  expect_found R3 "return AI_STEP_WAITS" ai_run "a constant returned" || bad=1
  expect_found R3 "= AI_STEP_" ai_run_two "a constant assigned" || bad=1
  cat > "$WORK/plant/c.c" <<'EOF'
static enum ai_tool_step ai_run(char **text)
{
 enum ai_tool_step k = ai_step_result(text, NULL);
 if (k == AI_STEP_WAITS)
  return k;
 return ai_step_waits(text);
}
EOF
  expect_clean R3 "a call and a variable that a call assigned" || bad=1
  # R4: a step function that writes the text itself
  cat > "$WORK/plant/c.c" <<'EOF'
static enum ai_tool_step ai_run(char **text)
{
 *text = strdup("x");
 return ai_step_waits(text);
}
EOF
  expect_found R4 "*text =" ai_run "the text written by a step" || bad=1
  # R5: a step function called and its outcome dropped
  cat > "$WORK/plant/c.c" <<'EOF'
static enum ai_tool_step ai_run(char **text);
static int use(char **text)
{
 ai_run(text);
 return 0;
}
EOF
  expect_found R5 ai_run use "a dropped outcome" || bad=1
  cat > "$WORK/plant/c.c" <<'EOF'
static enum ai_tool_step ai_run(char **text);
static enum ai_tool_step use(char **text)
{
 enum ai_tool_step k;
 k = ai_run(text);
 if (k == AI_STEP_WAITS)
  return k;
 return ai_run(text);
}
EOF
  expect_clean R5 "an assigned and a returned call" || bad=1
  # R6: the old marker
  printf '%s\n' '#define AI_TOOL_WAITS ai_tool_waits_mark' > "$WORK/plant/c.c"
  if ! scan_old_marker "$WORK/plant/c.c" | grep -q '^R6|c.c|'; then
    echo "check-ai-tool-step: rule R6 is blind to the old marker" >&2; bad=1
  fi
  printf '%s\n' 'int AI_PROCESS_WAITS = 1;' > "$WORK/plant/c.c"
  if [ -n "$(scan_old_marker "$WORK/plant/c.c")" ]; then
    echo "check-ai-tool-step: rule R6 names AI_PROCESS_WAITS" >&2; bad=1
  fi
  # R7: a second place that says "no tool"
  cat > "$WORK/plant/c.c" <<'EOF'
static enum ai_tool_step ai_run(char **text)
{
 return ai_step_no_tool(text);
}
EOF
  expect_found R7 ai_step_no_tool ai_run "a place that says no tool" || bad=1
  # R8: a consumer that reads the outcome and never forgets the page
  cat > "$WORK/plant/c.c" <<'EOF'
static int consume(char **text)
{
 enum ai_tool_step s = ai_run(text);
 if (s == AI_STEP_WAITS)
  return 1;
 return 0;
}
EOF
  expect_found R8 AI_STEP_ consume "a consumer without ai_page_forget" || bad=1
  cat > "$WORK/plant/c.c" <<'EOF'
static int consume(char **text)
{
 enum ai_tool_step s = ai_run(text);
 if (s == AI_STEP_WAITS)
  return 1;
 ai_page_forget();
 return 0;
}
EOF
  expect_clean R8 "a consumer that forgets the page" || bad=1
  # R1: an array that is not static, and casts on either side
  cat > "$WORK/plant/c.c" <<'EOF'
char plain_mark[] = "(marker)";
static char planted_mark[] = "(marker)";
int cmp(char *r)
{
 if (r == plain_mark)
  return 1;
 if ((uintptr_t)r == (uintptr_t)planted_mark)
  return 2;
 if ((char *)planted_mark != r)
  return 3;
 return 0;
}
EOF
  expect_found R1 plain_mark cmp "a compare with an array that is not static" || bad=1
  expect_found R1 planted_mark cmp "a compare through a cast" || bad=1
  # R2: a member of a static, pointer arithmetic, an array that is not static
  cat > "$WORK/plant/c.c" <<'EOF'
char plain_buf[10];
static char planted_mark[] = "(marker)";
static struct {
 char name[4];
} planted_obj;
char *ret_member(void)
{
 return planted_obj.name;
}
char *ret_plus(void)
{
 return planted_mark + 0;
}
char *ret_plain(void)
{
 return plain_buf;
}
EOF
  expect_found R2 planted_obj.name ret_member "a member of a static object" || bad=1
  expect_found R2 planted_mark ret_plus "pointer arithmetic on a static" || bad=1
  expect_found R2 plain_buf ret_plain "an array that is not static" || bad=1
  # R7: the word in a macro and in a function pointer, outside any call
  cat > "$WORK/plant/c.c" <<'EOF'
#define SAY_NO ai_step_no_tool
static enum ai_tool_step (*planted_fp)(char **) = ai_step_no_tool;
EOF
  scan_files "$STEP_BASE|ai_run" "$WORK/plant/c.c" > "$WORK/plant/found"
  [ "$(grep -c '^R7|c.c|-|ai_step_no_tool$' "$WORK/plant/found")" = 2 ] || {
    echo "check-ai-tool-step: rule R7 is blind to a macro or a function pointer" >&2; bad=1
  }
  # R8: a number for an outcome, and a forget on the waiting path only
  cat > "$WORK/plant/c.c" <<'EOF'
static int consume(char **text)
{
 enum ai_tool_step step = ai_run(text);
 if (step == 2)
  return 1;
 ai_page_forget();
 return 0;
}
EOF
  expect_found R8 "numeric compare" consume "an outcome compared with a number" || bad=1
  cat > "$WORK/plant/c.c" <<'EOF'
static int consume(char **text)
{
 enum ai_tool_step step = ai_run(text);
 if (step == AI_STEP_WAITS) {
  ai_page_forget();
  return 1;
 }
 return 0;
}
static int consume_bare(char **text)
{
 enum ai_tool_step step = ai_run(text);
 if (step == AI_STEP_WAITS)
  ai_page_forget();
 return 0;
}
EOF
  expect_found R8 AI_STEP_ consume "a forget on the waiting path only" || bad=1
  expect_found R8 AI_STEP_ consume_bare "a forget on the waiting statement only" || bad=1
  cat > "$WORK/plant/c.c" <<'EOF'
static int consume(char **text)
{
 enum ai_tool_step step = ai_run(text);
 if (step == AI_STEP_WAITS)
  return 1;
 ai_page_forget();
 return 0;
}
static int consume_two(char **text)
{
 enum ai_tool_step step = ai_run(text);
 if (step == AI_STEP_NO_TOOL) {
  ai_page_forget();
  return 0;
 }
 if (step == AI_STEP_WAITS) {
  return 1;
 }
 ai_page_forget();
 return 2;
}
EOF
  expect_clean R8 "a forget on the path that does not wait" || bad=1
  return $bad
}

if [ $SELFTEST -eq 1 ]; then
  selftest || exit $BAD
  echo "check-ai-tool-step: selftest ok"
  exit 0
fi

selftest || exit $BAD

cd "$SRCDIR" || exit $BAD
FILES=$(ls ./*.c ./*.h 2>/dev/null)
CFILES=$(ls ./*.c 2>/dev/null)
[ -n "$CFILES" ] || { echo "check-ai-tool-step: no sources under $SRCDIR"; exit 77; }
# shellcheck disable=SC2086
NAMES=$(step_names $CFILES)

# shellcheck disable=SC2086
scan_files "$NAMES" $FILES | sort > "$WORK/found"
# R6 over the sources and every test, except this script and its allow file.
OLDFILES=$(ls ./*.c ./*.h tests/*.c tests/*.h tests/*.py tests/*.sh 2>/dev/null |
  grep -v -e 'check-ai-tool-step.sh' -e 'ai-tool-step.allow')
# shellcheck disable=SC2086
scan_old_marker $OLDFILES >> "$WORK/found"
sort "$WORK/found" > "$WORK/found.sorted"

if [ $LIST -eq 1 ]; then
  uniq -c "$WORK/found.sorted" |
    awk '{ n = $1; $1 = ""; sub(/^ /, "", $0); print $0 "|" n "|reason" }'
  exit 0
fi

# The allow file: rule|file|function|expression|count|reason
: > "$WORK/allowed"
if [ -f "$ALLOW" ]; then
  awk -F'|' '
    /^[ \t]*(#|$)/ { next }
    {
      if (NF < 6 || $5 !~ /^[0-9]+$/ || $6 == "") { print "bad line " NR " in the allow file: " $0 > "/dev/stderr"; bad = 1; next }
      key = $1 "|" $2 "|" $3 "|" $4
      if (key in seen) { print "duplicate allow line for " key > "/dev/stderr"; bad = 1; next }
      seen[key] = 1
      print key "|" $5 "|" $6
    }
    END { exit bad }' "$ALLOW" > "$WORK/allowed" || {
      echo "check-ai-tool-step: FAIL (the allow file $ALLOW has malformed or duplicate lines)"
      exit $BAD
    }
else
  echo "check-ai-tool-step: FAIL (no allow file at $ALLOW)"
  exit $BAD
fi

uniq -c "$WORK/found.sorted" | awk '{ n = $1; $1 = ""; sub(/^ /, "", $0); print $0 "|" n }' > "$WORK/counts"

awk -F'|' '
  FILENAME == ARGV[1] { key = $1 "|" $2 "|" $3 "|" $4; al[key] = $5; why[key] = $6; next }
  {
    key = $1 "|" $2 "|" $3 "|" $4; got[key] = 1
    if (!(key in al)) {
      print "offender: " key " (rule " $1 ": see the head of tests/check-ai-tool-step.sh)"; bad = 1
    } else if ($5 + 0 > al[key] + 0) {
      print "more than allowed: " key " has " $5 ", the allow file says " al[key] " (" why[key] ")"; bad = 1
    } else if ($5 + 0 < al[key] + 0) {
      print "fewer than the allow file says: " key " has " $5 ", lower the line from " al[key] " (" why[key] ")"; bad = 1
    }
  }
  END {
    for (k in al) if (!(k in got)) { print "stale entry, delete it: " k; bad = 1 }
    exit bad
  }' "$WORK/allowed" "$WORK/counts" > "$WORK/report"
status=$?

if [ $status -ne 0 ]; then
  echo "check-ai-tool-step: FAIL"
  cat "$WORK/report"
  exit $BAD
fi
echo "check-ai-tool-step: ok ($(wc -l < "$WORK/found.sorted" | tr -d ' ') listed finding(s))"
exit 0
