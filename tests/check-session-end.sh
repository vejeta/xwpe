#!/bin/sh
# check-session-end.sh -- how a session ends and how a request is registered
# have one owner each.
#
# A request is written into its client's table (a slot) only after the send
# succeeded.  A slot filled before the send has to be taken back when the
# send fails, and a send that fails while the slot is filled can end the
# table with the failed request still in it: the same request is then told
# twice, or its memory freed twice.  This script keeps the order in the
# source: send, then fill.
#
#   check-session-end.sh [--srcdir DIR] [--allow FILE] [--nm NM] [--selftest]
#
# Clauses in this script (the others land with the pass that moves their
# callers onto the owner; each is added to the table below, with its own
# canaries):
#
#  C5 order.  In each function of the C5 table (a client's send function),
#     (a) the client's fill function is called exactly once, and on a
#         statement after the last call of the send (a call split over
#         lines is read as one statement);
#     (b) nothing else writes through the slot: no assignment, increment or
#         indexed access through the slot pointer or the slot's members, and
#         no memcpy, memmove, memset, snprintf, sprintf, strcpy, strncpy,
#         strcat or strncat whose first argument is the slot;
#     (c) the slot pointer, or the address of a slot member, is passed to no
#         function but the fill and the send;
#     (d) the fill function is called from no function the table does not
#         name, and is never named without a call (a function pointer).
#     A function the table names that is not in the file, or a send or a fill
#     that is not there, fails: the scan is blind.
#
# Comments and string literals are blanked first (tests/blank-c.awk).
#
# --selftest runs only the canaries: a clean fixture that must pass, and a
# fixture per bypass that must be named by the clause, so that a scan gone
# blind fails.  Without it the script scans the sources and then runs the
# canaries.  --allow names the allow file (tests/session-end.allow: one
# line per excused function, Cn|file|function|reason|revisit; no clause of
# this pass reads one yet, so a line in it is refused until a clause does).
# --nm is accepted for the clauses that read object files, which land later.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
BAD=1
ALLOW=
NM=
SELFTEST_ONLY=
HERE=$(cd "$(dirname "$0")" && pwd)

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --allow) shift; ALLOW=$1 ;;
    --nm) shift; NM=$1 ;;
    --selftest) SELFTEST_ONLY=1 ;;
    *) echo "check-session-end: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/session-end.allow

# The C5 table: file|function|fill|send|slot variable|slot member prefixes|
# alias variable.  A function writes its slot through a pointer variable
# (dap), through the session's own members (bsp, where the fill writes
# out_id, out_method, out_fn, out_ctx, sent_ms and waiting_shown: one prefix
# for each, separated by blanks), or both; an unused field is empty.  The
# alias variable is the one whose copy would give another name to the slot.
C5_TABLE='we_dap.c|dap_call_send|dap_slot_fill|dap_send|p||p
we_dap.c|dap_send_owned|dap_slot_fill|dap_send|p||p
we_bsp.c|bsp_req_request|bsp_out_fill|bsp_send||b->out_ b->sent_ms b->waiting_shown|b'
C5_FILES='we_dap.c we_bsp.c'

. "$(dirname "$0")/workdir.sh"
work_dir session-end $BAD
: > "$WORK/problems"

problem() { echo "$*" >> "$WORK/problems"; }

# The one blanker of comments and literals (tests/blank-c.awk).
BLANK_C=$(cat "$HERE/blank-c.awk")

# The C5 scan of one file: c5_awk FILE TABLE prints one line per offence.
C5_AWK='
function note(ln, msg) { printf "C5: %s:%d: %s\n", base_name, ln, msg }

# The name of the function whose header is LINE.
function header_name(line,   head, parts, k) {
  head = line
  sub(/\(.*/, "", head)
  sub(/[^A-Za-z0-9_]+$/, "", head)
  k = split(head, parts, /[^A-Za-z0-9_]+/)
  return parts[k]
}

# Scan TEXT for the identifier NAME: call_n counts the calls, bare_n the uses
# with no call, first_off and last_off are the offsets of the first and the
# last call.
function scan_calls(text, name,   at, rest, before, after, consumed, absolute, len) {
  call_n = 0; bare_n = 0; first_off = 0; last_off = 0
  consumed = 0; rest = text; len = length(name)
  while ((at = index(rest, name)) > 0) {
    absolute = consumed + at
    before = absolute > 1 ? substr(text, absolute - 1, 1) : ""
    after = substr(rest, at + len)
    if (before !~ /[A-Za-z0-9_]/ && after !~ /^[A-Za-z0-9_]/) {
      if (after ~ /^[ \t]*\(/) {
        call_n++
        if (!first_off) first_off = absolute
        last_off = absolute
      } else bare_n++
    }
    consumed += at + len - 1
    rest = substr(rest, at + len)
  }
}

# 1 when what follows the member name at POS of TEXT is an assignment, an
# increment or a decrement.
function written_after(text, pos,   rest) {
  rest = substr(text, pos)
  match(rest, /^[A-Za-z0-9_]*/)
  rest = substr(rest, RLENGTH + 1)
  sub(/^[ \t]*(\[[^]]*\][ \t]*)*/, "", rest)
  return (rest ~ /^(=([^=]|$)|\+=|-=|\*=|\/=|%=|\|=|&=|\^=|<<=|>>=|\+\+|--)/)
}

# 1 when TEXT, up to POS, ends with an increment or a decrement operator.
function stepped_before(text, pos,   head) {
  head = substr(text, 1, pos - 1)
  return (head ~ /(\+\+|--)[ \t]*&?[ \t]*$/)
}

# Offences of kind (b): a write through the slot named by SV (a pointer
# variable) or PX (member prefixes, separated by blanks) in the statement
# TEXT, which starts on line LN, and a copy of ALIAS (the variable that
# would give the slot another name).  "p->x", "p -> x" and "(*p).x" are one
# spelling.
function slot_writes(text, ln, sv, px, alias,   re, alt, tok, nt, pxs, np, j, rest, consumed, at, absolute, before, k) {
  nt = 0
  if (sv != "") {
    gsub(("\\([ \\t]*\\*[ \\t]*" sv "[ \\t]*\\)[ \\t]*\\.[ \\t]*"), "(*" sv ").", text)
    tok[++nt] = sv "->"
    tok[++nt] = "(*" sv ")."
  }
  np = split(px, pxs, " ")
  for (j = 1; j <= np; j++) tok[++nt] = pxs[j]
  for (k = 1; k <= nt; k++) {
    consumed = 0; rest = text
    while ((at = index(rest, tok[k])) > 0) {
      absolute = consumed + at
      before = absolute > 1 ? substr(text, absolute - 1, 1) : ""
      if (before !~ /[A-Za-z0-9_.>]/ &&
          (written_after(text, absolute + length(tok[k])) ||
           stepped_before(text, absolute)))
        note(ln, "assignment through the slot: " substr(text, absolute, 30))
      consumed += at + length(tok[k]) - 1
      rest = substr(rest, at + length(tok[k]))
    }
  }
  alt = ""
  if (sv != "") alt = sv "([^A-Za-z0-9_]|$)"
  for (j = 1; j <= np; j++) alt = (alt == "" ? "" : alt "|") pxs[j]
  re = "(memcpy|memmove|memset|snprintf|sprintf|strcpy|strncpy|strcat|strncat)[ \t]*\\([ \t]*&?[ \t]*(" alt ")"
  if (text ~ re)
    note(ln, "the slot is the first argument of a copy: " substr(text, 1, 40))
  if (sv != "") {
    if (text ~ ("(^|[;{}(,=])[ \t]*\\*[ \t]*" sv "[ \t]*=([^=]|$)"))
      note(ln, "assignment through the slot: *" sv)
    if (text ~ ("(^|[^A-Za-z0-9_.>])" sv "[ \t]*\\["))
      note(ln, "indexed access through the slot pointer " sv)
  }
  if (alias != "" &&
      (text ~ ("(^|[^=!<>+*/%&|^-])=[ \t]*&?[ \t]*" alias "[ \t]*[;,]") ||
       text ~ ("(^|[^=!<>+*/%&|^-])=[ \t]*&[ \t]*" alias "->")))
    note(ln, "the slot is given another name (a copy of " alias "): " substr(text, 1, 40))
}

# The identifier that names the function called around the argument that
# starts at POS (the offset of the "(" or "," before it) of TEXT; "" when
# none.
function callee_at(text, pos,   nch, ch, j, depth, name, c) {
  nch = split(text, ch, "")
  j = pos
  if (ch[j] == ",") {
    depth = 0
    for (j = pos - 1; j >= 1; j--) {
      if (ch[j] == ")") depth++
      else if (ch[j] == "(") {
        if (depth == 0) break
        depth--
      }
    }
  }
  j--
  while (j >= 1 && ch[j] ~ /[ \t]/) j--
  name = ""
  while (j >= 1 && ch[j] ~ /[A-Za-z0-9_]/) { name = ch[j] name; j-- }
  return name
}

# Offences of kind (c): the slot pointer SV, or the address of a member of the
# slot (&SV-> or &PX), passed to a function other than FILL and SEND.
function slot_passed(text, ln, sv, px, fill, send,   re, pxs, np, j, rest, consumed, absolute, callee) {
  re = ""
  if (sv != "") re = "[(,][ \t]*&?[ \t]*" sv "[ \t]*[,)]|[(,][ \t]*&[ \t]*" sv "->"
  np = split(px, pxs, " ")
  for (j = 1; j <= np; j++) re = (re == "" ? "" : re "|") "[(,][ \t]*&[ \t]*" pxs[j]
  consumed = 0; rest = text
  while (match(rest, re)) {
    absolute = consumed + RSTART
    callee = callee_at(text, absolute)
    if (callee != fill && callee != send && callee !~ /^(if|while|for|switch|return|sizeof)$/)
      note(ln, "the slot is passed to " (callee == "" ? "an expression" : callee) ", not only to " fill " and " send)
    consumed += RSTART + RLENGTH - 1
    rest = substr(rest, RSTART + RLENGTH)
  }
}

BEGIN { blank_incom = 0 }
{ code[FNR] = blank_c($0, 0); total = FNR }

END {
  # The function each line is in: the last col-0 header seen.
  fname = ""
  for (i = 1; i <= total; i++) {
    line = code[i]
    if (line ~ /^[A-Za-z_]/ && line ~ /\(/ && line !~ /;[ \t]*$/ &&
        line !~ /^(typedef|struct|union|enum|extern)[ \t]/) {
      fname = header_name(line)
      defined[fname] = 1
      header_line[fname] = i
    }
    func_of[i] = fname
  }

  nrows = split(table, rowtext, ";")
  for (r = 1; r <= nrows; r++) {
    split(rowtext[r], f, "|")
    if (f[1] != base_name) continue
    fnm = f[2]; fill = f[3]; send = f[4]; sv = f[5]; px = f[6]; alias = f[7]
    allowed[fill "|" fnm] = 1
    fills[fill] = 1
    if (!(fnm in defined)) {
      note(0, fnm " is not in the file (the scan is blind)")
      continue
    }
    if (!(fill in defined))
      note(header_line[fnm], fill " is not defined in the file (the scan is blind)")

    # the statements of the function: a line that holds ";", "{" or "}" ends one
    # (the body starts at the "{" that opens it, after a header of several lines)
    ns = 0; stmt = ""; start = 0
    for (i = header_line[fnm] + 1; i <= total && code[i] !~ /^\{/; i++) ;
    for (i = i + 1; i <= total && func_of[i] == fnm; i++) {
      if (stmt == "") start = i
      stmt = stmt " " code[i]
      if (code[i] ~ /[;{}]/) {
        gsub(/[ \t]*->[ \t]*/, "->", stmt)      # "p -> x" is "p->x"
        ns++; text[ns] = stmt; sline[ns] = start; stmt = ""; start = 0
      }
    }

    fill_n = 0; send_n = 0; fill_key = 0; send_key = 0
    for (k = 1; k <= ns; k++) {
      scan_calls(text[k], fill)
      fill_n += call_n
      if (call_n && !fill_key) fill_key = sline[k] * 100000 + first_off
      if (bare_n) note(sline[k], fill " is named without a call in " fnm)
      scan_calls(text[k], send)
      send_n += call_n
      if (call_n) send_key = sline[k] * 100000 + last_off
      slot_writes(text[k], sline[k], sv, px, alias)
      slot_passed(text[k], sline[k], sv, px, fill, send)
    }
    if (send_n == 0) note(header_line[fnm], fnm " has no call of " send " (the scan is blind)")
    if (fill_n == 0) note(header_line[fnm], fnm " has no call of " fill ": the request is never registered")
    if (fill_n > 1) note(header_line[fnm], fnm " calls " fill " more than once")
    if (fill_n >= 1 && send_n >= 1 && fill_key < send_key)
      note(int(fill_key / 100000), "the slot is filled before the send (" fill " above " send ") in " fnm)
  }

  # kind (d): who calls a fill function
  for (fill in fills) {
    for (i = 1; i <= total; i++) {
      line = code[i]
      if (index(line, fill) == 0) continue
      if (line ~ /^[A-Za-z_]/ && line ~ /;[ \t]*$/) continue
      if (func_of[i] == fill) continue
      scan_calls(line, fill)
      if (bare_n) note(i, fill " is named without a call in " func_of[i])
      if (call_n && !((fill "|" func_of[i]) in allowed))
        note(i, fill " is called from " func_of[i] ", which the table does not name")
    }
  }
}
'

# c5_scan DIR - the C5 offences of the sources under DIR, one per line.
c5_scan() {
  dir=$1
  for file in $C5_FILES; do
    if [ ! -f "$dir/$file" ]; then
      echo "C5: $file not found under $dir (the scan is blind)"
      continue
    fi
    table=$(printf '%s' "$C5_TABLE" | tr '\n' ';')
    awk -v base_name="$file" -v table="$table" "$BLANK_C$C5_AWK" "$dir/$file"
  done
}

# The allow file: no clause of this pass reads one, so a line is refused.
check_allow() {
  [ -f "$ALLOW" ] || { problem "no allow file at $ALLOW"; return; }
  awk -F'|' '
    /^#/ || /^[ \t]*$/ { next }
    { print "check-session-end: allow line " NR " is not read by any clause yet: " $0; bad = 1 }
    END { exit bad }' "$ALLOW" >> "$WORK/problems" || :
}

# ---- the fixtures of the canaries -----------------------------------------
#
# A clean copy of each file holds the functions of the C5 table in their
# right order, with marker lines (a comment that names the place).  A canary
# replaces one marker line by an offender and the scan must name it.

emit_dap_clean() {
  cat <<'EOF'
/**
 * dap_slot_fill - Register a request that went out.
 */
static void dap_slot_fill(dap_pending *p, const dap_pending *what)
{
 *p = *what;
}

static int dap_send_owned(e_dap_session *s, const char *command,
                          struct json_object *args, int kind)
{
 dap_pending *p = dap_pending_free_slot(s);
 dap_pending what;
 int seq;

 if (!p || s->eof)
 {
  if (args)
   json_object_put(args);
  return DAP_NO_SEQ;
 }
 /*PLANT:owned_top*/
 seq = dap_send(s, command, args);
 if (seq == DAP_NO_SEQ)
  return DAP_NO_SEQ;
 memset(&what, 0, sizeof what);
 what.kind = kind;
 what.seq = seq;
 dap_slot_fill(p, &what);
 return seq;
}

static int dap_call_send(e_dap_session *s, dap_pending *p, const char *command,
                         struct json_object *args, char *file, int *lines,
                         int n, e_d_answered_fn fn, void *ctx)
{
 dap_pending what;
 /*PLANT:call_top*/
 int seq = dap_send(s, command,
                    args);

 if (seq == DAP_NO_SEQ)
 {
  free(file);
  free(lines);
  return E_D_SEND_GONE;
 }
 memset(&what, 0, sizeof what);
 what.kind = DAP_REQ_CALL;
 what.seq = seq;
 what.file = file;
 snprintf(what.cmd, sizeof what.cmd, "%s", command);
 /*PLANT:call_before_fill*/
 dap_slot_fill(p,
               &what);
 /*PLANT:call_after_fill*/
 dap_calls_time(s);
 return E_D_SEND_OK;
}

static void dap_read_once(e_dap_session *s)
{
 /*PLANT:read_once*/
 dap_drain_queue(s);
}
EOF
}

# The two functions as they were before the reorder: the slot is written
# first, taken back when the send fails.
emit_dap_old() {
  cat <<'EOF'
static void dap_slot_fill(dap_pending *p, const dap_pending *what)
{
 *p = *what;
}

static int dap_send_owned(e_dap_session *s, const char *command,
                          struct json_object *args, int kind)
{
 dap_pending *p = dap_pending_free_slot(s);

 if (!p || s->eof)
 {
  return DAP_NO_SEQ;
 }
 p->kind = kind;
 p->seq = dap_send(s, command, args);
 if (p->seq != DAP_NO_SEQ)
  return p->seq;
 dap_pending_release(p);
 return DAP_NO_SEQ;
}

static int dap_call_send(e_dap_session *s, dap_pending *p, const char *command,
                         struct json_object *args, char *file, int *lines,
                         int n, e_d_answered_fn fn, void *ctx)
{
 p->kind = DAP_REQ_CALL;
 p->fn = fn;
 p->ctx = ctx;
 p->file = file;
 p->lines = lines;
 p->n = n;
 snprintf(p->cmd, sizeof p->cmd, "%s", command);
 p->sent_ms = dap_now(s);
 p->seq = dap_send(s, command, args);
 if (p->seq == DAP_NO_SEQ)
 {
  dap_pending_release(p);
  return E_D_SEND_GONE;
 }
 dap_calls_time(s);
 return E_D_SEND_OK;
}
EOF
}

emit_bsp_clean() {
  cat <<'EOF'
static void bsp_out_fill(e_bsp_session *b, int id, const char *method,
                         e_d_answered_fn answered, void *ctx)
{
 b->out_id = id;
 snprintf(b->out_method, sizeof b->out_method, "%s", method);
 b->out_fn = answered;
 b->out_ctx = ctx;
}

static int bsp_req_request(void *engine, const char *cmd,
                           e_d_answered_fn answered, void *ctx)
{
 e_bsp_session *b = engine;
 struct json_object *args = NULL;
 int id;

 bsp_stage_clear(b);
 /*PLANT:bsp_top*/
 id = ++b->id;
 if (bsp_send(b, id, cmd, args) != 0)
 {
  snprintf(b->err, sizeof b->err, "BSP send failed: %s", cmd);
  return E_D_SEND_GONE;
 }
 /*PLANT:bsp_before_fill*/
 bsp_out_fill(b, id, cmd, answered, ctx);
 /*PLANT:bsp_after_fill*/
 bsp_arm(b, E_D_TIMER_QUIET, E_D_REQ_QUIET_MS);
 return E_D_SEND_OK;
}

static void bsp_read_once(e_bsp_session *b)
{
 /*PLANT:bsp_read_once*/
 bsp_drain(b);
}
EOF
}

# plant_into DIR MARKER TEXT - DIR gets the clean fixtures, with the line that
# holds MARKER replaced by TEXT (\n in TEXT is a line break).
plant_into() {
  dir=$1; marker=$2; text=$3
  rm -rf "$dir"; mkdir -p "$dir"
  for file in $C5_FILES; do
    awk -v m="/*PLANT:$marker*/" -v t="$text" '
      index($0, m) { print t; next }
      { print }' "$WORK/fx/$file" > "$dir/$file"
  done
}

canaries_run=0

# canary NAME MARKER TEXT WORD - the planted offender must be named by C5,
# in a message that holds WORD.
canary() {
  name=$1; marker=$2; text=$3; word=$4
  canaries_run=$((canaries_run + 1))
  plant_into "$WORK/fx-case" "$marker" "$text"
  c5_scan "$WORK/fx-case" > "$WORK/fx-case.out"
  if ! grep -q -F "$word" "$WORK/fx-case.out"; then
    problem "canary C5 $name: not named with \"$word\" (the clause is blind)"
  fi
}

# canary_tree NAME DIR WORD - the same, for a tree built by hand.
canary_tree() {
  name=$1; dir=$2; word=$3
  canaries_run=$((canaries_run + 1))
  c5_scan "$dir" > "$WORK/fx-case.out"
  if ! grep -q -F "$word" "$WORK/fx-case.out"; then
    problem "canary C5 $name: not named with \"$word\" (the clause is blind)"
  fi
}

selftest() {
  mkdir -p "$WORK/fx"
  emit_dap_clean > "$WORK/fx/we_dap.c"
  emit_bsp_clean > "$WORK/fx/we_bsp.c"

  # The clean fixtures pass.
  c5_scan "$WORK/fx" > "$WORK/fx-clean.out"
  if [ -s "$WORK/fx-clean.out" ]; then
    problem "selftest: the clean fixtures are named (the clause is noisy):"
    cat "$WORK/fx-clean.out" >> "$WORK/problems"
  fi

  # The present dap_call_send and dap_send_owned: the slot written first.
  mkdir -p "$WORK/fx-old"
  emit_dap_old > "$WORK/fx-old/we_dap.c"
  cp "$WORK/fx/we_bsp.c" "$WORK/fx-old/we_bsp.c"
  canary_tree old-order "$WORK/fx-old" "assignment through the slot"
  canary_tree old-order-no-fill "$WORK/fx-old" "never registered"

  # The fill above the send.
  canary fill-above-send call_top ' dap_slot_fill(p, &what);' "filled before the send"
  canary fill-above-send-owned owned_top ' dap_slot_fill(p, &what);' "filled before the send"
  canary fill-above-send-bsp bsp_top ' bsp_out_fill(b, 7, cmd, answered, ctx);' "filled before the send"

  # A write through the slot by another way than the fill.
  canary memcpy-above-send call_top ' memcpy(p, &what, sizeof *p);' "first argument of a copy"
  canary memset-slot call_top ' memset(p, 0, sizeof *p);' "first argument of a copy"
  canary snprintf-member call_top ' snprintf(p->cmd, sizeof p->cmd, "%s", command);' "first argument of a copy"
  canary assign-member call_top ' p->kind = DAP_REQ_CALL;' "assignment through the slot"
  canary assign-member-owned owned_top ' p->kind = kind;' "assignment through the slot"
  canary assign-after-fill call_after_fill ' p->sent_ms = 0;' "assignment through the slot"
  canary increment-member call_top ' p->n++;' "assignment through the slot"
  canary pre-increment-member call_top ' ++p->n;' "assignment through the slot"
  canary deref-assign call_top ' *p = what;' "assignment through the slot"
  canary indexed-slot call_top ' p[0].kind = 1;' "indexed access"
  canary slot-passed call_top ' dap_pending_release(p);' "the slot is passed to dap_pending_release"
  canary slot-address-passed call_top ' dap_note(s, &p->seq);' "the slot is passed to dap_note"
  canary slot-passed-nested call_top ' dap_note(s, dap_pick(1, p));' "the slot is passed to dap_pick"

  # The fill: not called, called twice, named without a call, called elsewhere.
  canary two-fills call_after_fill ' dap_slot_fill(p, &what);' "more than once"
  canary fill-named-bare call_top ' void (*f)(dap_pending *, const dap_pending *) = dap_slot_fill;' "named without a call"
  canary fill-from-other-function read_once ' dap_slot_fill(s->pending, 0);' "called from dap_read_once"
  canary bsp-fill-from-other-function bsp_read_once ' bsp_out_fill(b, 1, "x", 0, 0);' "called from bsp_read_once"

  # The slot members of the session (the build server's slot).
  canary bsp-member-above-send bsp_top ' b->out_id = ++b->id;' "assignment through the slot"
  canary bsp-method-above-send bsp_top ' snprintf(b->out_method, sizeof b->out_method, "%s", cmd);' "first argument of a copy"
  canary bsp-member-address bsp_top ' bsp_take(&b->out_fn);' "the slot is passed to bsp_take"
  canary bsp-member-after-fill bsp_after_fill ' b->out_ctx = 0;' "assignment through the slot"
  canary bsp-sent-ms-above-send bsp_top ' b->sent_ms = bsp_now(b);' "assignment through the slot"
  canary bsp-waiting-shown-above-send bsp_top ' b->waiting_shown = 0;' "assignment through the slot"
  canary bsp-sent-ms-after-fill bsp_after_fill ' b->sent_ms++;' "assignment through the slot"

  # Other spellings of the same write, and a second name for the slot.
  canary spaced-arrow call_top ' p -> kind = DAP_REQ_CALL;' "assignment through the slot"
  canary spaced-arrow-owned owned_top ' p  ->  seq = 3;' "assignment through the slot"
  canary parenthesised-deref call_top ' (*p).kind = DAP_REQ_CALL;' "assignment through the slot"
  canary parenthesised-deref-spaced call_top ' ( * p ) . kind = DAP_REQ_CALL;' "assignment through the slot"
  canary alias-declared call_top ' dap_pending *q = p;' "another name"
  canary alias-assigned call_top ' dap_pending *q; q = p;' "another name"
  canary alias-member-address call_top ' int *q = &p->seq;' "another name"
  canary alias-bsp bsp_top ' e_bsp_session *q = b;' "another name"

  # A scan that cannot find what it checks must fail, not pass.
  mkdir -p "$WORK/fx-blind"
  awk '/^static int dap_send_owned/ { skip = 1 } skip && /^}/ { skip = 0; next } !skip { print }' \
    "$WORK/fx/we_dap.c" > "$WORK/fx-blind/we_dap.c"
  cp "$WORK/fx/we_bsp.c" "$WORK/fx-blind/we_bsp.c"
  canary_tree function-missing "$WORK/fx-blind" "dap_send_owned is not in the file"
  mkdir -p "$WORK/fx-nosend"
  awk '{ sub(/dap_send\(s, command, args\)/, "dap_post(s, command, args)"); print }' \
    "$WORK/fx/we_dap.c" > "$WORK/fx-nosend/we_dap.c"
  cp "$WORK/fx/we_bsp.c" "$WORK/fx-nosend/we_bsp.c"
  canary_tree send-missing "$WORK/fx-nosend" "has no call of dap_send"
  mkdir -p "$WORK/fx-nofile"
  cp "$WORK/fx/we_dap.c" "$WORK/fx-nofile/we_dap.c"
  canary_tree file-missing "$WORK/fx-nofile" "we_bsp.c not found"
}

check_allow

if [ -z "$SELFTEST_ONLY" ]; then
  c5_scan "$SRCDIR" > "$WORK/real.out"
  cat "$WORK/real.out" >> "$WORK/problems"
fi
selftest

if [ -s "$WORK/problems" ]; then
  echo "check-session-end: FAIL"
  cat "$WORK/problems"
  exit $BAD
fi
echo "check-session-end: ok (C5, $canaries_run canaries)"
exit 0
