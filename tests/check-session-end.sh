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
#  C6 loop.  In we_fdloop.c the way a failed queue leaves the loop has one
#     path each:
#     (a) wpe_fd_writer_flush is called only by wpe_fd_flush_one and
#         wpe_fd_write_flush, as the whole statement  r = wpe_fd_writer_flush(w);
#         and its result is read: wpe_fd_flush_one passes it to
#         wpe_fd_writer_fail_on_loop(w, r) on a later statement,
#         wpe_fd_write_flush to wpe_fd_writer_fail_at_call(w) and returns it
#         (return r;).  Neither holds a (void) cast;
#     (b) wpe_fd_writer_free is called only by wpe_fd_writer_finish,
#         wpe_fd_writer_forget, wpe_fd_write and wpe_fd_writer_lost_number;
#         free( is called only by wpe_fd_writer_free, and memset( only by
#         the functions that clear a slot today (wpe_fd_writer_free,
#         wpe_fd_writer_take, wpe_fd_write_failed_forget,
#         wpe_fd_write_owner_take_failed), so no helper drops a queue by
#         itself;
#     (c) wpe_fd_writer_finish is called once by wpe_fd_writer_flush, and by
#         wpe_fd_writer_fail_on_loop and wpe_fd_writer_fail_at_call;
#     (d) wpe_fd_writer_forget is called only by wpe_fd_close;
#     (e) close( is called only by wpe_fd_close; wpe_fd_close and
#         wpe_fd_close_when_written each call wpe_fd_write_failed_forget;
#         wpe_fd_write_failed_set names close_when_empty (it refuses a
#         descriptor the loop closes);
#     (f) an owner's tell is read (a call, a copy to a pointer, a macro at
#         file scope) only by wpe_fd_tell_failures, and assigned only by
#         wpe_fd_write_failed_set; wpe_fd_tell_failures is called only by
#         wpe_fd_flush_ready, on a statement after its last call of
#         wpe_fd_flush_one: nothing is told inside the walk;
#     (g) wpe_loop_owe is called by no function of this file (an exported
#         wrapper would let a host ask for the owed call), and
#         loop_owed_flag is raised only by wpe_loop_owe.
#     A function the clause names that is not in the file fails (the scan is
#     blind).
#
#  C8 owed hook.  wpe_loop_owe and wpe_loop_owed_set are named by no product
#     source but we_fdloop.c, we_fdloop.h and we_sessend.c (source half), and
#     no object but we_sessend.o imports them (object half, read with $(NM)
#     through tests/objsyms.sh).  The object half sees its own canaries or
#     reports a problem, never a pass: we_fdloop.o imports poll and defines
#     both names, and we_sessend.o, when it is among the objects, imports
#     wpe_loop_owe.  With no usable nm or no object list it prints SKIP and
#     exits 77 after the source halves have run, never 0 with a half unrun
#     (a compiler that does not run is a broken harness: exit 2).
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
# A #define whose body holds a ";", a brace or a call is refused in every
# scanned file (we_dap.c, we_bsp.c, we_fdloop.c): the scan reads statements
# and cannot expand a macro, so a macro with code would hide a forbidden call
# from every clause.  Continuation lines are followed; sizeof is not a call.
#
# --selftest runs only the canaries: a clean fixture that must pass, and a
# fixture per bypass that must be named by the clause, so that a scan gone
# blind fails.  Without it the script scans the sources and then runs the
# canaries.  --allow names the allow file (tests/session-end.allow: one
# line per excused function, Cn|file|function|reason|revisit; no clause of
# this pass reads one yet, so a line in it is refused until a clause does).
# --nm names the nm the clauses that read object files use (default: $NM,
# set by make check, else nm).  The objects to read are the arguments, or
# $ONE_OWNER_OBJECTS (the editor's objects, set by make check).
#
# Environment (set by `make check`): NM, ONE_OWNER_OBJECTS, CC, srcdir.
# Exit: 0 clean, 1 offenders or a blind scanner, 77 a half that needs nm, a
# compiler or an object list could not run.

SRCDIR=${srcdir:-.}
BAD=1
SKIP=77
ALLOW=
NMTOOL=${NM:-nm}
OBJECTS=
SELFTEST_ONLY=
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/objsyms.sh"
# The one owner of the compiler call (tests/compiler.sh).
. "$HERE/compiler.sh"

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --allow) shift; ALLOW=$1 ;;
    --nm) shift; NMTOOL=$1 ;;
    --selftest) SELFTEST_ONLY=1 ;;
    -*) echo "check-session-end: unknown argument $1" >&2; exit $BAD ;;
    *) OBJECTS="$OBJECTS $1" ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/session-end.allow
[ -n "$OBJECTS" ] || OBJECTS=$ONE_OWNER_OBJECTS

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

# The scanning functions the clauses share (awk text, put in front of each
# clause's program, after tests/blank-c.awk).  A program reads the file into
# code[1..total] with blank_c and calls index_functions(); then
# statements_of(fn) leaves the statements of fn in text[1..ns], each with
# the line it starts on in sline[].
AWK_COMMON='
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

# 1 when LINE opens a function: a col-0 header that is no declaration.
function is_header(line) {
  return (line ~ /^[A-Za-z_]/ && line ~ /\(/ && line !~ /;[ \t]*$/ &&
          line !~ /^(typedef|struct|union|enum|extern)[ \t]/)
}

# How many times the character CH is in TEXT (literals are blanked already).
function count_char(text, ch,   n) {
  n = gsub(ch, "", text)
  return n
}

# Refuse a #define whose body holds code.  The scan reads statements, not
# what a macro expands to, so a macro with code would hide a forbidden call
# from every rule: its body (continuation lines followed, comments and strings
# blanked) may hold no ";", "{" or "}" and no call "name(" (sizeof is
# not one).  Needs note(ln, msg) from the program.
function scan_defines(   i, ln0, body, name, k, probe) {
  for (i = 1; i <= total; i++) {
    if (code[i] !~ /^[ \t]*#[ \t]*define[ \t]/) continue
    ln0 = i
    body = code[i]
    while (body ~ /\\[ \t]*$/ && i < total) {
      sub(/\\[ \t]*$/, "", body)
      i++
      body = body " " code[i]
    }
    sub(/\\[ \t]*$/, "", body)
    sub(/^[ \t]*#[ \t]*define[ \t]+/, "", body)
    name = body
    sub(/[^A-Za-z0-9_].*/, "", name)
    sub(/^[A-Za-z0-9_]+/, "", body)            # the name
    sub(/^\([^)]*\)/, "", body)                # the parameters of a function-like macro
    probe = body
    gsub(/sizeof[ \t]*\(/, "(", probe)
    if (probe ~ /[;{}]/ || probe ~ /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)
      note(ln0, "#define " name " holds code: the scan cannot expand a macro, so a macro with a statement or a call is refused")
  }
}

# The function each line is in: the last col-0 header seen.  defined[name]
# and header_line[name] hold the functions the file defines.
function index_functions(   i, fname, line) {
  fname = ""
  for (i = 1; i <= total; i++) {
    line = code[i]
    if (is_header(line)) {
      fname = header_name(line)
      defined[fname] = 1
      header_line[fname] = i
    }
    func_of[i] = fname
  }
}

# The statements of the function FNM: a line that holds ";", "{" or "}" ends
# one.  The body opens at the first "{" at or after the header line (on the
# header line itself for K&R style and one-line functions: what follows the
# "{" is the first statement), and ends where its braces balance, so a
# one-line function does not take the lines after it.  A call split over
# lines is one statement.
function statements_of(fnm,   i, stmt, start, line, depth, opened) {
  ns = 0; stmt = ""; start = 0; depth = 0; opened = 0
  for (i = header_line[fnm]; i <= total && func_of[i] == fnm; i++) {
    line = code[i]
    if (!opened) {
      if (index(line, "{") == 0) continue
      opened = 1
      line = substr(line, index(line, "{"))      # from the opening brace on
    }
    depth += count_char(line, "\\{") - count_char(line, "\\}")
    if (stmt == "") start = i
    stmt = stmt " " line
    if (line ~ /[;{}]/) {
      gsub(/[ \t]*->[ \t]*/, "->", stmt)      # "p -> x" is "p->x"
      ns++; text[ns] = stmt; sline[ns] = start; stmt = ""; start = 0
    }
    if (depth <= 0) break
  }
}
'

# The C5 scan of one file: c5_awk FILE TABLE prints one line per offence.
C5_AWK='
function note(ln, msg) { printf "C5: %s:%d: %s\n", base_name, ln, msg }

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
  index_functions()
  scan_defines()

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

    statements_of(fnm)

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
    awk -v base_name="$file" -v table="$table" "$BLANK_C$AWK_COMMON$C5_AWK" "$dir/$file"
  done
}

# The C6 scan of we_fdloop.c: one line per offence.
C6_AWK='
function note(ln, msg) { printf "C6: %s:%d: %s\n", base_name, ln, msg }

# The statement TEXT with its blanks squeezed to one and its ends trimmed.
function squeeze(s) {
  gsub(/[ \t]+/, " ", s)
  sub(/^ /, "", s)
  sub(/ $/, "", s)
  return s
}

# 1 when the function FN is one of the names in the blank-separated LIST.
function among(fn, list,   n, names, k) {
  n = split(list, names, " ")
  for (k = 1; k <= n; k++) if (names[k] == fn) return 1
  return 0
}

# A call of NAME in the statement S (scan_calls), refused outside the
# functions of LIST; the name used without a call is refused everywhere.
function only_called_from(s, ln, fn, name, list, why) {
  scan_calls(s, name)
  if (bare_n) note(ln, name " is named without a call in " fn)
  if (call_n && !among(fn, list)) note(ln, name " is called from " fn ": " why)
  return call_n
}

BEGIN { blank_incom = 0 }
{ code[FNR] = blank_c($0, 0); total = FNR }

END {
  index_functions()
  scan_defines()
  nneed = split("wpe_fd_flush_one wpe_fd_write_flush wpe_fd_flush_ready wpe_fd_tell_failures wpe_fd_close wpe_fd_close_when_written wpe_fd_write_failed_set wpe_fd_writer_finish wpe_fd_writer_flush wpe_fd_writer_fail_on_loop wpe_fd_writer_fail_at_call wpe_fd_writer_lost_number wpe_fd_write wpe_fd_writer_forget wpe_loop_owe", need, " ")
  blind = 0
  for (k = 1; k <= nneed; k++)
    if (!(need[k] in defined)) { note(0, need[k] " is not in the file (the scan is blind)"); blind = 1 }
  if (blind) exit

  for (fn in defined) {
    statements_of(fn)
    flush_at = 0; fail_loop_at = 0; fail_call_at = 0; return_r_at = 0
    one_at = 0; tell_at = 0; finish_here = 0; forget_here = 0; closed_set = 0
    for (k = 1; k <= ns; k++) {
      s = squeeze(text[k]); ln = sline[k]

      # (a) the result of the writer
      scan_calls(s, "wpe_fd_writer_flush")
      if (bare_n) note(ln, "wpe_fd_writer_flush is named without a call in " fn)
      if (call_n) {
        if (!among(fn, "wpe_fd_flush_one wpe_fd_write_flush"))
          note(ln, "wpe_fd_writer_flush is called from " fn ", not from wpe_fd_flush_one or wpe_fd_write_flush")
        else if (s !~ /^(int )?r = wpe_fd_writer_flush ?\( ?w ?\) ?;$/)
          note(ln, "the result of wpe_fd_writer_flush is not read as the whole statement r = wpe_fd_writer_flush(w);")
        else flush_at = k
      }
      if (among(fn, "wpe_fd_flush_one wpe_fd_write_flush") && s ~ /\( ?void ?\)/)
        note(ln, "a (void) cast in " fn ": a failed queue must not be dropped unread")
      if (s ~ /wpe_fd_writer_fail_on_loop ?\( ?w ?, ?r ?\)/ && flush_at && k > flush_at) fail_loop_at = k
      if (s ~ /wpe_fd_writer_fail_at_call ?\( ?w ?\)/ && flush_at && k > flush_at) fail_call_at = k
      if (s ~ /^return r ?;$/ && flush_at && k > flush_at) return_r_at = k

      # (b) the slot freed
      only_called_from(s, ln, fn, "wpe_fd_writer_free", "wpe_fd_writer_finish wpe_fd_writer_forget wpe_fd_write wpe_fd_writer_lost_number", "a failed queue is dropped by wpe_fd_writer_fail_on_loop, wpe_fd_writer_fail_at_call or wpe_fd_writer_lost_number")

      # (c) the queue finished
      finish_here += only_called_from(s, ln, fn, "wpe_fd_writer_finish", "wpe_fd_writer_flush wpe_fd_writer_fail_on_loop wpe_fd_writer_fail_at_call", "a queue is finished by the flush that emptied it or by a failure path")

      # (d) the queue forgotten
      only_called_from(s, ln, fn, "wpe_fd_writer_forget", "wpe_fd_close", "the owner'\''s close is the one place a queue is forgotten")

      # (e) the one close
      scan_calls(s, "close")
      if (call_n && fn != "wpe_fd_close")
        note(ln, "close( is called from " fn ": the loop closes only through wpe_fd_close")
      scan_calls(s, "wpe_fd_write_failed_forget")
      if (call_n) forget_here = 1
      if (s ~ /close_when_empty/) closed_set = 1

      # (b) again: no inlined drop of a writer in another helper
      scan_calls(s, "free")
      if (call_n && fn != "wpe_fd_writer_free")
        note(ln, "free( is called from " fn ": a queue'\''s memory is given back by wpe_fd_writer_free only")
      scan_calls(s, "memset")
      if (call_n && !among(fn, "wpe_fd_writer_free wpe_fd_writer_take wpe_fd_write_failed_forget wpe_fd_write_owner_take_failed"))
        note(ln, "memset( is called from " fn ": only the functions that clear a slot today may (wpe_fd_writer_free, wpe_fd_writer_take, wpe_fd_write_failed_forget, wpe_fd_write_owner_take_failed)")

      # (f) the order of the tell and the walk
      scan_calls(s, "wpe_fd_flush_one")
      if (call_n) one_at = k
      scan_calls(s, "wpe_fd_tell_failures")
      if (call_n) {
        if (fn != "wpe_fd_flush_ready")
          note(ln, "wpe_fd_tell_failures is called from " fn ", not from wpe_fd_flush_ready")
        if (!tell_at) tell_at = k
      }

      # (g) the owed hook: asked for by no function of this file, and the flag
      # raised by wpe_loop_owe alone
      scan_calls(s, "wpe_loop_owe")
      if (call_n)
        note(ln, "wpe_loop_owe is called from " fn ": only the owner of the sessions'\'' ends may ask for the owed call")
      if (s ~ /loop_owed_flag ?= ?1/ && fn != "wpe_loop_owe")
        note(ln, "loop_owed_flag is raised in " fn ": only wpe_loop_owe raises it")
    }
    if (fn == "wpe_fd_flush_one" && !fail_loop_at)
      note(header_line[fn], fn " never passes the result to wpe_fd_writer_fail_on_loop(w, r) after reading it")
    if (fn == "wpe_fd_write_flush" && !fail_call_at)
      note(header_line[fn], fn " never passes a failed queue to wpe_fd_writer_fail_at_call(w) after reading its result")
    if (fn == "wpe_fd_write_flush" && !return_r_at)
      note(header_line[fn], fn " does not return the result (return r;) after the flush")
    if (fn == "wpe_fd_writer_flush" && finish_here != 1)
      note(header_line[fn], fn " finishes the queue " finish_here " times: once, for the queue it emptied")
    if ((fn == "wpe_fd_close" || fn == "wpe_fd_close_when_written") && !forget_here)
      note(header_line[fn], fn " does not call wpe_fd_write_failed_forget: a number reused later would reach the old owner")
    if (fn == "wpe_fd_write_failed_set" && !closed_set)
      note(header_line[fn], fn " does not name close_when_empty: it must refuse a descriptor the loop closes")
    if (fn == "wpe_fd_flush_ready") {
      if (!one_at) note(header_line[fn], fn " does not call wpe_fd_flush_one (the scan is blind)")
      if (!tell_at) note(header_line[fn], fn " does not call wpe_fd_tell_failures: a failed queue is never told")
      if (one_at && tell_at && tell_at < one_at)
        note(header_line[fn], fn " tells before the walk is over (wpe_fd_tell_failures above the last wpe_fd_flush_one)")
    }
  }

  # (f) the tell, read anywhere in the file, file scope and macros included:
  # a member named tell is read by wpe_fd_tell_failures only, and assigned by
  # wpe_fd_write_failed_set.  A line is in a function between its "{" and "}"
  # whose braces are still open, from the "{" on the header line or at column
  # 0; a "#" line is never in one.
  depth = 0
  for (i = 1; i <= total; i++) {
    line = code[i]
    before = depth
    depth += count_char(line, "\\{") - count_char(line, "\\}")
    if (depth < 0) depth = 0
    in_fn = (before > 0 || line ~ /^\{/ || (is_header(line) && index(line, "{") > 0))
    where = (in_fn && line !~ /^[ \t]*#/) ? func_of[i] : "file scope"
    if (line !~ /(\.|->) ?tell([^A-Za-z0-9_]|$)/) continue
    if (where == "wpe_fd_tell_failures") continue
    if (where == "wpe_fd_write_failed_set" && line ~ /->tell ?= ?tell ?;/) continue
    note(i, "an owner'\''s tell is read in " where ": only wpe_fd_tell_failures tells, after the walk")
  }
}
'

# c6_scan DIR - the C6 offences of DIR/we_fdloop.c, one per line.
c6_scan() {
  dir=$1
  if [ ! -f "$dir/we_fdloop.c" ]; then
    echo "C6: we_fdloop.c not found under $dir (the scan is blind)"
    return
  fi
  awk -v base_name=we_fdloop.c "$BLANK_C$AWK_COMMON$C6_AWK" "$dir/we_fdloop.c"
}

# The C8 source half: the words of the owed hook in a product source that is
# not its owner or its one user.  c8_source_scan DIR prints one line per hit.
C8_AWK='
BEGIN { blank_incom = 0 }
{
  line = blank_c($0, 0)
  if (line ~ /(^|[^A-Za-z0-9_])wpe_loop_owe([^A-Za-z0-9_]|$)/)
    printf "C8: %s:%d: wpe_loop_owe is named outside we_fdloop.c, we_fdloop.h and we_sessend.c\n", base_name, FNR
  if (line ~ /(^|[^A-Za-z0-9_])wpe_loop_owed_set([^A-Za-z0-9_]|$)/)
    printf "C8: %s:%d: wpe_loop_owed_set is named outside we_fdloop.c, we_fdloop.h and we_sessend.c\n", base_name, FNR
}
'

c8_source_scan() {
  dir=$1
  for path in "$dir"/*.c "$dir"/*.h; do
    [ -f "$path" ] || continue
    file=$(basename "$path")
    case $file in
      we_fdloop.c|we_fdloop.h|we_sessend.c) continue ;;
    esac
    awk -v base_name="$file" "$BLANK_C$C8_AWK" "$path"
  done
}

# The C8 object half: who imports the owed hook, read through $NMTOOL.  Its
# own canaries are seen or reported, never passed over.  c8_object_scan
# OBJECT... prints one line per hit.
object_imports() { "$NMTOOL" -u "$1" 2>/dev/null | nm_undefined_source_names; }
object_defines() { "$NMTOOL" -g "$1" 2>/dev/null | nm_defined_names; }

c8_object_scan() {
  seen_loop=0
  for obj in "$@"; do
    base=$(basename "$obj")
    base=${base#we-}
    if [ ! -f "$obj" ]; then
      echo "C8: object $obj not found (build before the check)"
      continue
    fi
    object_imports "$obj" > "$WORK/obj.imports"
    object_defines "$obj" > "$WORK/obj.defines"
    case $base in
      we_fdloop.o)
        seen_loop=1
        grep -qx poll "$WORK/obj.imports" ||
          echo "C8: canary: poll not seen in we_fdloop.o (LTO objects, wrong nm, or an empty object list)"
        for sym in wpe_loop_owe wpe_loop_owed_set; do
          grep -qx "$sym" "$WORK/obj.defines" ||
            echo "C8: canary: $sym not defined in we_fdloop.o (LTO objects, wrong nm, or an empty object list)"
        done ;;
      we_sessend.o)
        grep -qx wpe_loop_owe "$WORK/obj.imports" ||
          echo "C8: canary: wpe_loop_owe not seen in we_sessend.o (LTO objects, wrong nm, or an empty object list)" ;;
      *)
        for sym in wpe_loop_owe wpe_loop_owed_set; do
          grep -qx "$sym" "$WORK/obj.imports" &&
            echo "C8: $base imports $sym: only we_sessend.o may use the owed hook"
        done ;;
    esac
  done
  [ $seen_loop -eq 1 ] ||
    echo "C8: canary: we_fdloop.o is not among the objects (an empty object list)"
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
#define DAP_NO_SEQ_X (-1)
#define DAP_SLOT_BYTES(n) ((n) * sizeof(dap_pending))
/*PLANT:dap_define*/
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
# holds MARKER replaced by TEXT (\n in TEXT is a line break; an empty TEXT
# deletes the line).
plant_into() {
  dir=$1; marker=$2; text=$3
  rm -rf "$dir"; mkdir -p "$dir"
  for path in "$WORK/fx"/*; do
    file=$(basename "$path")
    awk -v m="/*PLANT:$marker*/" -v t="$text" '
      index($0, m) { print t; next }
      { print }' "$path" > "$dir/$file"
  done
}

canaries_run=0

# The clause the canaries below are about, and the function that scans for it.
CLAUSE=C5
SCAN=c5_scan

# canary NAME MARKER TEXT WORD - the planted offender must be named by the
# clause in force, in a message that holds WORD.
canary() {
  name=$1; marker=$2; text=$3; word=$4
  canaries_run=$((canaries_run + 1))
  plant_into "$WORK/fx-case" "$marker" "$text"
  $SCAN "$WORK/fx-case" > "$WORK/fx-case.out"
  if ! grep -q -F "$word" "$WORK/fx-case.out"; then
    problem "canary $CLAUSE $name: not named with \"$word\" (the clause is blind)"
  fi
}

# canary_tree NAME DIR WORD - the same, for a tree built by hand.
canary_tree() {
  name=$1; dir=$2; word=$3
  canaries_run=$((canaries_run + 1))
  $SCAN "$dir" > "$WORK/fx-case.out"
  if ! grep -q -F "$word" "$WORK/fx-case.out"; then
    problem "canary $CLAUSE $name: not named with \"$word\" (the clause is blind)"
  fi
}

# ---- the fixtures of C6 and C8 -------------------------------------------

# A we_fdloop.c with only the functions C6 reads, in their right shape.  The
# marker comments are replaced by an offender (a marker on a statement's own
# line deletes the statement when replaced by nothing).
emit_loop_clean() {
  cat <<'EOF'
#define WPE_FD_WRITE_X (-2)
#define WPE_FD_SLOT_BYTES(n) \
  ((n) * sizeof(wpe_fd_writer))
/*PLANT:define*/
static void wpe_fd_writer_free(wpe_fd_writer *w)
{
 free(w->buf);
 memset(w, 0, sizeof *w);
}

static void wpe_fd_writer_finish(wpe_fd_writer *w)
{
 int fd = w->fd, close_it = w->close_when_empty;

 wpe_fd_writer_free(w);
 if (!close_it)
  return;
 /*PLANT:finish*/
 wpe_fd_close(fd);
}

static void wpe_fd_writer_fail_on_loop(wpe_fd_writer *w, int how)
{
 wpe_fd_write_owner_mark(w->fd, how);
 wpe_fd_writer_finish(w);
}

static void wpe_fd_writer_fail_at_call(wpe_fd_writer *w)
{
 wpe_fd_writer_finish(w);
}

static void wpe_fd_writer_lost_number(wpe_fd_writer *w)
{
 wpe_fd_write_owner_mark(w->fd, WPE_FD_WRITE_REFUSED);
 wpe_fd_writer_free(w);
}

static int wpe_fd_writer_flush(wpe_fd_writer *w)
{
 int r = wpe_fd_write_once(w->fd, w->buf, 1, 0);

 if (r != WPE_FD_WRITE_SENT)
 {
  /*PLANT:flush_failed*/
  return r;
 }
 if (wpe_fd_writer_waiting(w))
  return WPE_FD_WRITE_QUEUED;
 wpe_fd_writer_finish(w);
 return WPE_FD_WRITE_SENT;
}

int wpe_fd_write(int fd, const void *buf, size_t n, const char *what,
                 const char *ends)
{
 wpe_fd_writer *w = wpe_fd_writer_take(fd);
 int r = wpe_fd_writer_queue_rest(w, buf, n);

 if (r != WPE_FD_WRITE_QUEUED)
  wpe_fd_writer_free(w);
 return r;
}

int wpe_fd_write_flush(int fd)
{
 wpe_fd_writer *w = wpe_fd_writer_find(fd);
 int r;

 if (!w)
  return WPE_FD_WRITE_SENT;
 /*PLANT:wflush_top*/
 r = wpe_fd_writer_flush(w);
 /*PLANT:wflush_after*/
 if (r == WPE_FD_WRITE_CLOSED || r == WPE_FD_WRITE_REFUSED)
  wpe_fd_writer_fail_at_call(w); /*PLANT:wflush_fail*/
 return r;
}

static void wpe_fd_writer_forget(int fd)
{
 wpe_fd_writer *w = wpe_fd_writer_find(fd);

 if (!w)
  return;
 wpe_fd_writer_free(w);
}

int wpe_fd_close(int fd)
{
 if (fd < 0)
  return 0;
 wpe_fd_write_failed_forget(fd); /*PLANT:close_forget*/
 wpe_fd_writer_forget(fd);
 wpe_fd_del(fd);
 return close(fd);
}

int wpe_fd_close_when_written(int fd)
{
 wpe_fd_writer *w;

 if (fd < 0)
  return 0;
 wpe_fd_write_failed_forget(fd); /*PLANT:cww_forget*/
 w = wpe_fd_writer_find(fd);
 if (!w || !wpe_fd_writer_waiting(w))
 {
  wpe_fd_close(fd);
  return 0;
 }
 w->close_when_empty = 1;
 return 1;
}

int wpe_fd_write_failed_set(int fd, wpe_fd_write_failed_fn tell, void *data)
{
 wpe_fd_write_owner *o;
 const wpe_fd_writer *w;

 if (fd < 0 || !tell)
  return -1;
 w = wpe_fd_writer_find(fd);
 if (w && w->close_when_empty) /*PLANT:set_refusal*/
  return -1;
 o = wpe_fd_write_owner_find(fd);
 o->tell = tell;
 o->data = data;
 return 0;
}

static void wpe_fd_tell_failures(void)
{
 wpe_fd_write_owner o;

 while (wpe_fd_write_owner_take_failed(&o))
 {
  /*PLANT:tell_loop*/
  o.tell(o.fd, o.how, o.data);
 }
}
/*PLANT:filescope*/

void wpe_loop_owe(void)
{
 loop_owed_flag = 1;
}

static int wpe_loop_owed_run(void)
{
 loop_owed_flag = 0;
 return 1;
}
/*PLANT:helper*/

static void wpe_fd_flush_one(wpe_fd_writer *w, short revents)
{
 int r;

 if (revents & POLLNVAL)
 {
  wpe_fd_writer_lost_number(w);
  return;
 }
 /*PLANT:one_top*/
 r = wpe_fd_writer_flush(w);
 /*PLANT:one_after*/
 if (r == WPE_FD_WRITE_CLOSED || r == WPE_FD_WRITE_REFUSED)
  wpe_fd_writer_fail_on_loop(w, r); /*PLANT:one_fail*/
}

static void wpe_fd_flush_ready(const struct pollfd *pfd, const int *wmap, int n)
{
 int i;

 for (i = 0; i < n; i++)
 {
  wpe_fd_writer *w = &writer_table[wmap[i]];

  /*PLANT:walk*/
  wpe_fd_flush_one(w, pfd[i].revents);
 }
 /*PLANT:ready_after_walk*/
 wpe_fd_tell_failures();
}
/*PLANT:wrapper*/
EOF
}

# The product sources of C8: the owner of the owed hook and its one user may
# name it, a prose mention anywhere may not count, and the host may not use it.
emit_owed_clean() {
  cat <<'EOF'
/* we_sessend.c names the owed hook: it is its one user. */
static void sessend_run_owed(void)
{
}

void wpe_sessend_init(void)
{
 wpe_loop_owed_set(sessend_run_owed);
 wpe_loop_owe();
}
EOF
}

emit_host_clean() {
  cat <<'EOF'
/* A host never calls wpe_loop_owe: the owner of the sessions' ends does. */
static void e_host_noted(void)
{
 const char *text = "wpe_loop_owe is a word in a string";

 /*PLANT:host*/
 (void)text;
}
EOF
}

emit_loop_header() {
  cat <<'EOF'
typedef void (*wpe_loop_owed_fn)(void);
int  wpe_loop_owed_set(wpe_loop_owed_fn fn);
void wpe_loop_owe(void);
EOF
}

# What the object half reads: objects made from these sources, in the state
# the loop and its one user leave them.
emit_obj_loop_c() {
  cat <<'EOF'
#include <poll.h>
static int flag;
void wpe_loop_owe(void) { flag = 1; }
int wpe_loop_owed_set(void (*fn)(void)) { return fn != 0; }
int loop_poll(void) { return poll(0, 0, 0) + flag; }
EOF
}

emit_obj_sessend_c() {
  cat <<'EOF'
void wpe_loop_owe(void);
int wpe_loop_owed_set(void (*fn)(void));
void sessend_owe(void) { wpe_loop_owe(); }
EOF
}

emit_obj_other_c() {
  cat <<'EOF'
int wpe_loop_owed_set(void (*fn)(void));
int other_user(void) { return wpe_loop_owed_set(0); }
EOF
}

emit_obj_innocent_c() {
  cat <<'EOF'
int innocent(void) { return 1; }
EOF
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
  # A macro that writes the slot would hide the write from the scan.
  canary macro-writes-the-slot dap_define '#define DAP_PRESET_SLOT do { p->kind = DAP_REQ_CALL; } while (0)' "#define DAP_PRESET_SLOT holds code"
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

  selftest_c6
  selftest_c8_source
  selftest_c8_objects
}

# The C6 canaries: the clean loop passes; each bypass is named.
selftest_c6() {
  CLAUSE=C6
  SCAN=c6_scan
  emit_loop_clean > "$WORK/fx/we_fdloop.c"
  c6_scan "$WORK/fx" > "$WORK/fx-clean6.out"
  if [ -s "$WORK/fx-clean6.out" ]; then
    problem "selftest: the clean loop fixture is named by C6 (the clause is noisy):"
    cat "$WORK/fx-clean6.out" >> "$WORK/problems"
  fi

  # (a) the writer's result.
  canary base-flush-ready walk ' wpe_fd_writer_flush(w);' "called from wpe_fd_flush_ready"
  canary void-cast-after one_after ' (void)r;' "a (void) cast"
  canary void-cast-call one_top ' (void)wpe_fd_writer_flush(w);' "a (void) cast"
  canary flush-result-unassigned one_top ' wpe_fd_writer_flush(w);' "not read as the whole statement"
  canary flush-from-another-function tell_loop ' r = wpe_fd_writer_flush(w);' "called from wpe_fd_tell_failures"
  canary flush-named-bare one_after ' int (*f)(wpe_fd_writer *) = wpe_fd_writer_flush;' "named without a call"
  canary flush-result-never-failed one_fail '' "never passes the result to wpe_fd_writer_fail_on_loop"
  canary write-flush-void wflush_after ' (void)r;' "a (void) cast"
  canary write-flush-no-fail wflush_fail '' "never passes a failed queue to wpe_fd_writer_fail_at_call"
  canary write-flush-no-return wflush_fail '' "does not return the result"
  # (b) the slot freed.
  canary free-in-the-walk one_top ' wpe_fd_writer_free(w);' "wpe_fd_writer_free is called from wpe_fd_flush_one"
  canary free-named-bare one_after ' void (*f)(wpe_fd_writer *) = wpe_fd_writer_free;' "named without a call"
  # (c) the queue finished.
  canary finish-twice-in-flush flush_failed ' wpe_fd_writer_finish(w);' "finishes the queue 2 times"
  canary finish-in-the-walk one_after ' wpe_fd_writer_finish(w);' "wpe_fd_writer_finish is called from wpe_fd_flush_one"
  # (d) the queue forgotten.
  canary forget-in-the-walk one_after ' wpe_fd_writer_forget(w->fd);' "wpe_fd_writer_forget is called from wpe_fd_flush_one"
  # (e) the one close.
  canary plain-close-in-finish finish ' close(fd);' "close( is called from wpe_fd_writer_finish"
  canary plain-close-in-the-walk one_after ' close(w->fd);' "close( is called from wpe_fd_flush_one"
  canary close-without-forget close_forget '' "wpe_fd_close does not call wpe_fd_write_failed_forget"
  canary close-when-written-without-forget cww_forget '' "wpe_fd_close_when_written does not call wpe_fd_write_failed_forget"
  canary set-without-refusal set_refusal '' "does not name close_when_empty"
  # (f) the tell.
  canary tell-in-the-walk one_after ' o->tell(w->fd, 0, 0);' "tell is read in wpe_fd_flush_one"
  canary tell-call-in-the-walk one_after ' wpe_fd_tell_failures();' "wpe_fd_tell_failures is called from wpe_fd_flush_one"
  canary tell-above-the-walk walk ' wpe_fd_tell_failures();' "tells before the walk is over"
  canary tell-through-a-macro filescope '#define TELL(o, h) ((o)->tell((o)->fd, h, (o)->data))' "read in file scope"
  canary tell-copied-to-a-pointer one_after ' void (*f)(int, int, void *) = o->tell;' "read in wpe_fd_flush_one"
  # (b) again: a drop inlined in a helper other than wpe_fd_writer_free.
  canary free-in-a-helper helper 'static void wpe_fd_writer_drop_quietly(wpe_fd_writer *w)\n{\n free(w->buf);\n memset(w, 0, sizeof *w);\n}' "free( is called from wpe_fd_writer_drop_quietly"
  canary clear-in-a-helper helper 'static void wpe_fd_writer_drop_quietly(wpe_fd_writer *w)\n{\n free(w->buf);\n memset(w, 0, sizeof *w);\n}' "memset( is called from wpe_fd_writer_drop_quietly"
  canary clear-table-slot one_after ' memset(&writer_table[0], 0, sizeof writer_table[0]);' "memset( is called from wpe_fd_flush_one"
  canary clear-through-an-alias one_after ' wpe_fd_writer *slot = w; memset(slot, 0, sizeof *slot);' "memset( is called from wpe_fd_flush_one"
  # A macro with code hides a call from every rule: it is refused.
  canary macro-close define '#define WPE_SHUT(f) do { close(f); } while (0)' "#define WPE_SHUT holds code"
  canary macro-owe define '#define WPE_OWE_SOON() wpe_loop_owe()' "#define WPE_OWE_SOON holds code"
  canary macro-drop define '#define WPE_DROP(w) do { free((w)->buf); memset((w), 0, sizeof *(w)); } while (0)' "#define WPE_DROP holds code"
  canary macro-continued define '#define WPE_SHUT2(f) \\\n do { close(f); } while (0)' "#define WPE_SHUT2 holds code"
  canary macro-statement-only define '#define WPE_STEP x = 1;' "#define WPE_STEP holds code"
  # A body whose "{" is on the header line: K&R style and one-line functions.
  canary one-line-wrapper wrapper 'void wpe_loop_please(void) { wpe_loop_owe(); }' "wpe_loop_owe is called from wpe_loop_please"
  canary knr-close helper 'static void wpe_fd_sneaky_close(int fd) {\n close(fd);\n}' "close( is called from wpe_fd_sneaky_close"
  canary one-line-close helper 'static void wpe_fd_sneaky_close(int fd) { close(fd); }' "close( is called from wpe_fd_sneaky_close"
  # (g) the owed hook inside the loop's own file.
  canary exported-wrapper wrapper 'void wpe_loop_please(void)\n{\n wpe_loop_owe();\n}' "wpe_loop_owe is called from wpe_loop_please"
  canary flag-raised-elsewhere one_after ' loop_owed_flag = 1;' "loop_owed_flag is raised in wpe_fd_flush_one"

  # A scan that cannot find what it checks must fail, not pass.
  mkdir -p "$WORK/fx6-blind"
  awk '/^static void wpe_fd_tell_failures/ { skip = 1 } skip && /^}/ { skip = 0; next } !skip { print }' \
    "$WORK/fx/we_fdloop.c" > "$WORK/fx6-blind/we_fdloop.c"
  canary_tree function-missing "$WORK/fx6-blind" "wpe_fd_tell_failures is not in the file"
  mkdir -p "$WORK/fx6-nofile"
  canary_tree file-missing "$WORK/fx6-nofile" "we_fdloop.c not found"
}

# The C8 source canaries.
selftest_c8_source() {
  CLAUSE=C8
  SCAN=c8_source_scan
  emit_owed_clean > "$WORK/fx/we_sessend.c"
  emit_host_clean > "$WORK/fx/we_debug.c"
  emit_loop_header > "$WORK/fx/we_fdloop.h"
  c8_source_scan "$WORK/fx" > "$WORK/fx-clean8.out"
  if [ -s "$WORK/fx-clean8.out" ]; then
    problem "selftest: the clean C8 fixtures are named (the clause is noisy):"
    cat "$WORK/fx-clean8.out" >> "$WORK/problems"
  fi
  canary host-owes host ' wpe_loop_owe();' "wpe_loop_owe is named outside"
  canary host-names-the-hook host ' wpe_loop_owed_set(0);' "wpe_loop_owed_set is named outside"
  canary host-spaced host ' wpe_loop_owe ( );' "wpe_loop_owe is named outside"
  canary host-takes-address host ' void (*f)(void) = wpe_loop_owe;' "wpe_loop_owe is named outside"
}

# make_fixture_object NAME SOURCE-EMITTER - compile one fixture object, or say
# why it cannot be made.  Return: 0 made, 1 not.
make_fixture_object() {
  name=$1; emitter=$2
  $emitter > "$WORK/fxo/${name%.o}.c"
  test_cc -c -o "$WORK/fxo/$name" "$WORK/fxo/${name%.o}.c" 2> "$WORK/fxo/cc.err"
}

# The C8 object canaries: they need a compiler and nm; without either they
# are not run, which the exit status says (77), never a pass.
selftest_c8_objects() {
  CLAUSE=C8
  if [ "$NMTOOL" = ":" ] || ! command -v "$NMTOOL" > /dev/null 2>&1; then
    SKIPPED="$SKIPPED the selftest's C8 object canaries not run (no nm: $NMTOOL);"
    return
  fi
  test_cc_require
  mkdir -p "$WORK/fxo"
  make_fixture_object we_fdloop.o emit_obj_loop_c &&
    make_fixture_object we_sessend.o emit_obj_sessend_c &&
    make_fixture_object we_other.o emit_obj_other_c &&
    make_fixture_object we_innocent.o emit_obj_innocent_c || {
    problem "selftest: the C8 fixture objects were not compiled: $(cat "$WORK/fxo/cc.err")"
    return
  }
  canaries_run=$((canaries_run + 1))
  c8_object_scan "$WORK/fxo/we_fdloop.o" "$WORK/fxo/we_sessend.o" "$WORK/fxo/we_innocent.o" > "$WORK/fxo/clean.out"
  if [ -s "$WORK/fxo/clean.out" ]; then
    problem "selftest: the clean C8 objects are named (the clause is noisy):"
    cat "$WORK/fxo/clean.out" >> "$WORK/problems"
  fi
  # A second importer of the owed hook.
  canaries_run=$((canaries_run + 1))
  c8_object_scan "$WORK/fxo/we_fdloop.o" "$WORK/fxo/we_sessend.o" "$WORK/fxo/we_other.o" > "$WORK/fxo/case.out"
  grep -q -F "we_other.o imports wpe_loop_owed_set" "$WORK/fxo/case.out" ||
    problem "canary C8 second-importer: not named (the clause is blind)"
  # The owner's user that stopped importing the hook.
  canaries_run=$((canaries_run + 1))
  mkdir -p "$WORK/fxo/empty"
  cp "$WORK/fxo/we_innocent.o" "$WORK/fxo/empty/we_sessend.o"
  c8_object_scan "$WORK/fxo/we_fdloop.o" "$WORK/fxo/empty/we_sessend.o" > "$WORK/fxo/case.out"
  grep -q -F "wpe_loop_owe not seen in we_sessend.o" "$WORK/fxo/case.out" ||
    problem "canary C8 sessend-without-import: not named (the clause is blind)"
  # An object nm cannot read: named we_fdloop.o, it shows none of the canaries.
  canaries_run=$((canaries_run + 1))
  mkdir -p "$WORK/fxo/unreadable"
  echo "this is not an object" > "$WORK/fxo/unreadable/we_fdloop.o"
  c8_object_scan "$WORK/fxo/unreadable/we_fdloop.o" > "$WORK/fxo/case.out"
  grep -q -F "poll not seen in we_fdloop.o" "$WORK/fxo/case.out" ||
    problem "canary C8 unreadable-object: not named (the clause is blind)"
  # An object list without the loop's object.
  canaries_run=$((canaries_run + 1))
  c8_object_scan "$WORK/fxo/we_innocent.o" > "$WORK/fxo/case.out"
  grep -q -F "we_fdloop.o is not among the objects" "$WORK/fxo/case.out" ||
    problem "canary C8 no-loop-object: not named (the clause is blind)"
  # An object that is not there.
  canaries_run=$((canaries_run + 1))
  c8_object_scan "$WORK/fxo/we_fdloop.o" "$WORK/fxo/missing.o" > "$WORK/fxo/case.out"
  grep -q -F "missing.o not found" "$WORK/fxo/case.out" ||
    problem "canary C8 missing-object: not named (the clause is blind)"
}

# What this run could not do: said at the end with status 77, never passed.
SKIPPED=

# The object half of the real tree: the objects make built.
real_objects() {
  if [ -z "$OBJECTS" ]; then
    SKIPPED="$SKIPPED the C8 object half of the real tree not run (no object list: ONE_OWNER_OBJECTS or arguments);"
  elif [ "$NMTOOL" = ":" ] || ! command -v "$NMTOOL" > /dev/null 2>&1; then
    SKIPPED="$SKIPPED the C8 object half of the real tree not run (no nm: $NMTOOL);"
  else
    c8_object_scan $OBJECTS
  fi
}

check_allow

if [ -z "$SELFTEST_ONLY" ]; then
  { c5_scan "$SRCDIR"; c6_scan "$SRCDIR"; c8_source_scan "$SRCDIR"; real_objects; } > "$WORK/real.out"
  cat "$WORK/real.out" >> "$WORK/problems"
fi
selftest

if [ -s "$WORK/problems" ]; then
  echo "check-session-end: FAIL"
  cat "$WORK/problems"
  exit $BAD
fi
if [ -n "$SKIPPED" ]; then
  echo "check-session-end: SKIP,$SKIPPED the source halves and their canaries passed"
  exit $SKIP
fi
echo "check-session-end: ok (C5, C6, C8, $canaries_run canaries)"
exit 0
