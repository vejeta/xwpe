#!/bin/sh
# check-brk-bounds.sh -- a row of the text is read through the readers.
#
# A buffer has exactly mxlines rows and a row holds len+2 bytes from s[0] and
# nothing before it.  A walk (the bracket matcher, the painters, the syntax
# scanner, the beautifier) stays inside that only while the readers of
# we_rowread.h are the only code that indexes a row, indexes its bytes or
# knows how many rows there are: a position outside the text reads as "no
# character".  A walk that indexes a row itself is how Run > Beautify once read
# the byte before a row and the byte after its NUL, how the syntax colours
# once read a column past a row, and how the bracket matcher once read the
# row above the first one.
#
#   check-brk-bounds.sh [--srcdir DIR] [--allow FILE] [--list] [--selftest]
#                       [FILE]
#
# With FILE the strict rules are applied to that one file (the bracket
# matcher, we_brkmatch.c, by default) and nothing else is read.  Without it:
#
#  strict (we_brkmatch.c, and we_rowread.h outside its readers): after
#  comments, string literals and character literals are blanked,
#   1. no line subscripts an array ("["), names "bf" or "mxlines", or takes a
#      ".s" or "->s" field;
#   2. a preprocessor line (and the lines a macro continues over) is scanned
#      everywhere, readers included, so a macro cannot carry a raw read out of
#      a reader;
#   3. no "##" token pasting, and in we_brkmatch.c no #include other than the
#      five the file has (ctype.h, string.h, messages.h, edit.h,
#      we_rowread.h): a header or a pasted name could bring a raw reader in
#      under another name.
#  everywhere: a reader that gives a row's address (wpe_row_text,
#  wpe_row_bytes, wpe_row_from, wpe_row_cstr) is called only as a direct
#  argument of a named consumer that stops at the NUL (ispif, ispelse,
#  ispendif, iscase, isstatus, strncmp, strstr, atoi) or of a named walker
#  that takes a row and its length (e_scfbol, e_mk_col).  Neither its result
#  nor what such a consumer gives back may be followed by "[", "+" or "-", so
#  a row's address never outlives the statement that asked for it.  A copy
#  (memcpy, memmove) takes every byte it is told to, so a row shorter than the
#  count is read past its NUL: the count is the row's own (wpe_row_len,
#  wpe_row_nrc) or a quantity the caller computed at run time, and a literal,
#  an arithmetic of literals, a sizeof or a name the file defines as a macro
#  is a count of the caller's own.
#  Two readers keep a row past the statement that asked for it: wpe_row_view,
#  for a walk that reads the rows later, and wpe_row_cstr or wpe_row_from
#  stored into what the row owner takes whole.  wpe_row_view, and an entry of
#  an array of rows or of pieces, are allowed only in the one file and
#  function KEEPERS names for them: an array outlives the statement, anybody
#  can index it, and the same function name in another file keeps nothing.
#  One piece of a line (a part's ".s"), which goes straight into the owner's
#  splice, is wpe_row_from's alone (wpe_row_cstr gives a whole row, not a
#  piece of one) and is allowed where it is written; a "[", "+" or "-" on that
#  field later in the same function is the read the store was allowed for.
#  every other top level .c and .h except we_buffer.c (the row owner): no line
#  outside the functions named in the allow file names "bf" (the array of
#  rows: its bytes, its fields, its address), reaches a row through a name
#  declared STRING * (a parameter or not), casts to one, or declares such a
#  name in a body.  A line that writes a row in place and reads one on the
#  right-hand side of that write is a read as well, and the write does not
#  cover it: that read is an entry of the row array, the array's address as an
#  operand, or a name declared STRING *.
#
# A function starts at the "{" that follows the last IDENT( of the code at
# file level and ends where the braces balance, so a body written on one line
# belongs to its own function and an indented closing brace cannot pass an
# exemption on to the next one (K&R headers and split headers included).  A
# brace inside a parameter list (a struct a function takes by value) is no
# body: the finder counts parentheses and only a "{" outside them opens or
# closes a function, so such a parameter cannot leave the body of its own
# function attributed to "(file scope)".
#
# tests/row-reads.allow lists the lines kept on purpose:
#
#   KIND|file|function|count|why
#
# A a line through a STRING * that holds the lines built for a call of the row
# owner (an array named "lines", or a member of a struct that keeps them): the
# owner's input; W a line that writes a row in place (the row owner's concern,
# not moved here); D the declaration of the row array.  A line that reads a
# row (R) or reaches one through a STRING * that is not the owner's input (S)
# is not allowed at all: it is moved onto the readers.  "count" is the number
# of lines the scan finds in that function, and a line that does not match is
# stale: the count falls when a function moves, never rises.
#
# Limits of a scan of the text: a row reached through a pointer the scan never
# sees named (a char * a function was handed), a typedef'd row pointer and a
# count that a macro of another file gives a copy are out of its reach.  The
# units under ASan, with their hostile frames, are the check for those.
#
# --list prints the allow lines the tree needs; --selftest plants each kind of
# offence in a copy and fails unless the scan names it.
SRCDIR=${srcdir:-.}
SELFTEST=0
LIST=0
FILE=
ALLOW=
READERS="wpe_row_has wpe_row_len wpe_row_nrc wpe_row_byte wpe_row_text wpe_row_bytes wpe_row_from wpe_row_cstr wpe_row_find wpe_row_view wpe_rows_exist wpe_row_stored wpe_row_ends_line wpe_row_glyph_at"
INCLUDES="ctype.h string.h messages.h edit.h we_rowread.h"
CONSUMERS="ispif ispelse ispendif iscase isstatus strncmp strncpy strstr strcspn atoi WpeStrnccmp strlen strcpy strcat memcpy e_make_line_num2 e_str_nrc e_p_is_context_row e_p_is_note_row e_p_unmatched_row_verdict"
WALKERS="e_scfbol e_mk_col dump_bytes e_field_paste_fit memcpy e_buffer_rows_push eol_sink_put wpe_help_token wpe_help_token_dup e_utf8_visual_len e_su_lblk e_su_rblk e_utf8_char_before e_utf8_seq_len e_utf8_visual_step e_lsp_units_of e_buffer_wrap_cut e_ins_nchar e_buffer_record_text e_str_len wpe_rowfind_row"
# The consumers that hand a pointer into the row on, so indexing or offsetting
# what they give back keeps the row's address past the statement.
POINTERS="strstr strchr strrchr"
# The consumers that take every byte they are told to, so the count they are
# given must be the row's own and not a number of the caller's.
COPIERS="memcpy memmove"
# reader=file|function: the one place that may keep a row past the statement.
# The file is part of the key: a function of that name in another file is
# another function, and keeps nothing.
KEEPERS="wpe_row_view=we_prog.c|e_p_run_begin wpe_row_cstr=we_debug.c|e_lsp_buffer_text wpe_row_from=we_edit.c|e_ins_long_run"
OWNER=we_buffer.c

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --allow) shift; ALLOW=$1 ;;
    --list) LIST=1 ;;
    --selftest) SELFTEST=1 ;;
    -*) echo "check-brk-bounds: unknown argument $1" >&2; exit 1 ;;
    *) FILE=$1 ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$SRCDIR/tests/row-reads.allow
BLANK_C=$(cat "$(dirname "$0")/blank-c.awk")
. "$(dirname "$0")/workdir.sh"
work_dir brk-bounds 1

# The scanner, one awk program for the three modes (see scan).
AWK_SCAN='
    function isident(ch) { return ch ~ /[A-Za-z0-9_]/ }
    function flag(kind, token) { print kind "\t" FILENAME "\t" FNR "\t" linefn "\t" token }
    # The text after the parenthesis that closes the one open before pos.
    function after_close(s, pos,    depth, k, n, ch) {
      depth = 0; n = length(s)
      for (k = pos; k <= n; k++) {
        ch = substr(s, k, 1)
        if (ch == "(") depth++
        else if (ch == ")") { depth--; if (depth == 0) return substr(s, k + 1) }
      }
      return ""
    }
    # The name of the call this reader is a direct argument of, "" when the
    # text before it does not end in "name(" with no parenthesis between.
    function outer_call(before,    m) {
      if (!match(before, /(^|[^A-Za-z0-9_])[A-Za-z_][A-Za-z0-9_]*[ \t]*\([^()]*$/))
        return ""
      m = substr(before, RSTART, RLENGTH)
      sub(/\([^()]*$/, "", m)
      sub(/^.*[^A-Za-z0-9_]/, "", m)
      return m
    }
    # The text after the ")" that closes the call this reader is an argument
    # of: what its caller does with what that call gave back.
    function after_outer(tail,    depth, k, n, ch) {
      depth = 0; n = length(tail)
      for (k = 1; k <= n; k++) {
        ch = substr(tail, k, 1)
        if (ch == "(") depth++
        else if (ch == ")") { if (depth == 0) return substr(tail, k + 1); depth-- }
      }
      return ""
    }
    # The arguments that call gives after the ones of this reader.
    function outer_rest(tail,    depth, k, n, ch) {
      depth = 0; n = length(tail)
      for (k = 1; k <= n; k++) {
        ch = substr(tail, k, 1)
        if (ch == "(") depth++
        else if (ch == ")") { if (depth == 0) return substr(tail, 1, k - 1); depth-- }
      }
      return tail
    }
    # Whether a consumer that hands a pointer into the row on has its result
    # indexed or offset, which keeps the address of the row past the statement.
    function escapes_through(name, tail) {
      if (!(name in pointer_consumer)) return 0
      return after_outer(tail) ~ /^[ \t]*[-+\[]/
    }
    # Whether text holds a name this file defines as a macro: what such a name
    # stands for was fixed where it was defined, so it is no count the row gave.
    function macro_in(text,    mkey, mpart) {
      for (mkey in macro) {
        split(mkey, mpart, SUBSEP)
        if (mpart[1] != FILENAME) continue
        if (match(text, "(^|[^A-Za-z0-9_])" mpart[2] "([^A-Za-z0-9_]|$)")) return 1
      }
      return 0
    }
    # Whether a copy is told how many bytes to take by a number of its own
    # instead of by the row: a copy takes every byte it is told to, so a row
    # shorter than that number is read past its NUL.  Only wpe_row_len and
    # wpe_row_nrc give the count of the row itself.  A literal, an arithmetic of
    # literals, a sizeof or a macro of the file is a constant the caller made
    # up; what is left is a quantity it computed at run time.
    function copied_past_the_row(name, tail,    rest) {
      if (!(name in copier)) return 0
      rest = outer_rest(tail)
      sub(/^[^,]*,/, "", rest)
      gsub(/[ \t]/, "", rest)
      if (rest ~ /wpe_row_(len|nrc)[ \t]*\(/) return 0
      if (rest ~ /[0-9]/ || rest ~ /sizeof/) return 1
      return macro_in(rest)
    }
    # Whether this line is the one place KEEPERS names for a reader: a file and
    # a function, since a function of that name in another file is another
    # function and keeps nothing.
    function in_keeper(name) {
      return keeper[name] == filefn "|" linefn
    }
    # The piece of a line ("p.s", "pp->s") a store fills, "" when what is
    # stored into is no such field.
    function piece_field(before,    f) {
      if (!match(before, /[A-Za-z_][A-Za-z0-9_]*[ \t]*(->|\.)[ \t]*s[ \t]*=[ \t]*$/))
        return ""
      f = substr(before, RSTART, RLENGTH)
      sub(/[ \t]*=[ \t]*$/, "", f)
      gsub(/[ \t]/, "", f)
      return f
    }
    # Whether the result of the reader is stored where the row owner takes it
    # whole: one piece of a line anywhere, which only wpe_row_from gives
    # (wpe_row_cstr gives a whole row, not a piece of one); an entry of an
    # array of pieces or of rows only in the place named for that reader (an
    # array outlives the statement, so only the function that builds it may
    # fill it).  A piece stored here is remembered, since what the function
    # reads through it later is a read of the row.
    function hands_to_owner(name, before,    f) {
      if (name == "wpe_row_from" && (f = piece_field(before)) != "") {
        piece[FILENAME SUBSEP linefn SUBSEP f] = 1
        return 1
      }
      if (before ~ /\[[^]]*\]([.][A-Za-z_][A-Za-z0-9_]*)?[ \t]*=[ \t]*$/)
        return in_keeper(name)
      return 0
    }
    # The piece of a line a reader filled that text indexes or offsets, ""
    # when text leaves every piece whole.  The piece holds the bytes of the row
    # itself, so a read through it is the read the store was allowed for.
    function piece_read(text,    pkey, ppart, f, acc) {
      for (pkey in piece) {
        split(pkey, ppart, SUBSEP)
        if (ppart[1] != FILENAME || ppart[2] != linefn) continue
        f = ppart[3]
        acc = (index(f, "->") > 0) ? "->" : "\\."
        sub(/(->|\.)s$/, "", f)
        if (match(text, "(^|[^A-Za-z0-9_])" f "[ \t]*" acc "s[ \t]*[-+\\[]"))
          return ppart[3]
      }
      return ""
    }
    # Whether the call of NAME at pos in code stands where the rule allows.
    function call_ok(name, pos, list,    before, rest, tail, outer) {
      before = substr(code, 1, pos - 1)
      if (before ~ /^[ \t]*$/) before = prevcode
      rest = substr(code, pos + length(name))
      tail = after_close(rest, 1)
      outer = outer_call(before)
      if (tail ~ /^[ \t]*[-+\[]/) return 0
      if (escapes_through(outer, tail)) return 0
      if (copied_past_the_row(outer, tail)) return 0
      if (name == "wpe_row_view") return in_keeper(name)
      if ((name == "wpe_row_cstr" || name == "wpe_row_from") &&
          hands_to_owner(name, before))
        return 1
      if (name == "wpe_row_cstr")
        return 0
      if (name == "wpe_row_text")
        return before ~ ("(^|[^A-Za-z0-9_])(" list ")[ \t]*\\([^()]*$")
      return before ~ ("(^|[^A-Za-z0-9_])(" list ")[ \t]*\\(")
    }
    function check_calls(name, list,    base, rest, p, at, ch) {
      # The definition or the declaration of the reader is no call of it.
      if (startdepth == 0 && code ~ ("^[A-Za-z_][A-Za-z0-9_ \t*]*[ *]" name "[ \t]*\\("))
        return
      base = 0; rest = code
      while ((p = index(rest, name)) > 0) {
        at = base + p
        ch = (at > 1) ? substr(code, at - 1, 1) : " "
        if (!isident(ch) && !isident(substr(code, at + length(name), 1)) &&
            !call_ok(name, at, list))
          flag("R", "row-call")
        base = at + length(name) - 1
        rest = substr(code, base + 1)
      }
    }
    # Whether the line writes a field of a row.  The field must belong to a
    # bf[...] entry, so an assignment to the ".s" of another struct that names
    # a row on its right stays the read it is.
    function write_kind(c) {
      if (c ~ /bf[ \t]*\[[^][]*\][ \t]*\.[ \t]*(len|nrc)[ \t]*(\+\+|--|[-+*\/|&]?=([^=]|$))/) return 1
      if (c ~ /(\+\+|--)[ \t]*[A-Za-z_(][A-Za-z0-9_.>()+ \t-]*bf[ \t]*\[[^][]*\][ \t]*\.[ \t]*(len|nrc)/) return 1
      if (c ~ /bf[ \t]*\[[^][]*\][ \t]*\.[ \t]*s[ \t]*(\[[^][]*\])?[ \t]*[-+*\/|&]?=([^=]|$)/) return 1
      if (c ~ /\*[ \t]*\([^)]*bf[ \t]*\[[^][]*\][ \t]*\.[ \t]*s[ \t]*\+[^)]*\)[ \t]*[-+|&]?=([^=]|$)/) return 1
      if (c ~ /(REALLOC|MALLOC|FREE|realloc|malloc|free)[ \t]*\([^)]*(\.s|bf)/) return 1
      if (c ~ /(memcpy|memmove|memset|strcpy|strncpy)[ \t]*\([ \t]*[A-Za-z_][A-Za-z0-9_>.-]*bf[ \t]*\[[^][]*\][ \t]*\.[ \t]*s[ \t]*,/) return 1
      return 0
    }
    # The right-hand side of the last assignment on the line (not ==, !=, <=,
    # >=), "" when the line assigns nothing.
    function assigned(c,    k, n, at, ch, prev) {
      n = length(c); at = 0; prev = ""
      for (k = 1; k <= n; k++) {
        ch = substr(c, k, 1)
        if (ch == "=" && prev != "=" && prev != "!" && prev != "<" && prev != ">" &&
            substr(c, k + 1, 1) != "=")
          at = k
        prev = ch
      }
      return at ? substr(c, at + 1) : ""
    }
    # The name declared STRING * in this file that text reaches a row through,
    # "" when it reaches none.  A row reached through such a name is read
    # without a reader, whatever the name is called.
    function string_name_in(text,    skey, spart) {
      for (skey in strname) {
        split(skey, spart, SUBSEP)
        if (spart[1] != FILENAME) continue
        if (match(text, "(^|[^A-Za-z0-9_.>])" spart[2] "[ \t]*(->|\\[)")) return spart[2]
      }
      return ""
    }
    # What a line that writes a row reads on the right of that write, "" when
    # it reads no row there: an entry of the row array, the address of that
    # array as an operand, or a name declared STRING *.  Handing the own bytes
    # of the row to the allocator that gives them back is the write, not a read
    # of it.
    function reads_after_write(c,    rhs) {
      rhs = assigned(c)
      if (rhs ~ /^[ \t]*(\([ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*\)[ \t]*)*(REALLOC|MALLOC|FREE|realloc|malloc|free)[ \t]*\(/)
        return ""
      if (rhs ~ /(^|[^A-Za-z0-9_])bf([^A-Za-z0-9_]|$)/) return "bf"
      return (!prep && string_name_in(rhs) != "") ? "string-name" : ""
    }
    BEGIN {
      n = split(readers, r, " ")
      for (k = 1; k <= n; k++) isreader[r[k]] = 1
      n = split(includes, r, " ")
      for (k = 1; k <= n; k++) inc[r[k]] = 1
      n = split(pointers, r, " ")
      for (k = 1; k <= n; k++) pointer_consumer[r[k]] = 1
      n = split(copiers, r, " ")
      for (k = 1; k <= n; k++) copier[r[k]] = 1
      n = split(keepers, r, " ")
      for (k = 1; k <= n; k++) { split(r[k], kept, "="); keeper[kept[1]] = kept[2] }
      cons = consumers; gsub(/ /, "|", cons)
      walk = walkers; gsub(/ /, "|", walk)
    }
    FNR == 1 {
      seen[FILENAME]++
      pass = seen[FILENAME]
      blank_incom = 0; inmacro = 0; depth = 0; parens = 0; ppsp = 0
      fn = "(file scope)"; lastname = ""; prevcode = ""; hasassign = 0
      filefn = FILENAME; sub(/^.*\//, "", filefn)
    }
    {
      code = blank_c($0, 2)
      # The two sides of an #if hold the same braces and the same parentheses:
      # the second side starts from the depth the first started from, or a text
      # written twice would count its braces twice and every function below it
      # would be lost.
      if (!inmacro && code ~ /^[ \t]*#[ \t]*if/) {
        ppsp++; ppdepth[ppsp] = depth; ppfn[ppsp] = fn; ppparens[ppsp] = parens
      }
      else if (!inmacro && code ~ /^[ \t]*#[ \t]*(else|elif)/ && ppsp > 0) {
        depth = ppdepth[ppsp]; fn = ppfn[ppsp]; parens = ppparens[ppsp]
      }
      else if (!inmacro && code ~ /^[ \t]*#[ \t]*endif/ && ppsp > 0) ppsp--
      prep = (inmacro || code ~ /^[ \t]*#/)
      inmacro = (prep && $0 ~ /\\[ \t]*$/)
      # A name this file defines as a macro stands for what its definition says,
      # so what it gives a copy is no count the row gave (pass 1 collects).
      if (pass == 1 && prep && code ~ /^[ \t]*#[ \t]*define[ \t]+[A-Za-z_]/) {
        mac = code
        sub(/^[ \t]*#[ \t]*define[ \t]+/, "", mac)
        sub(/[^A-Za-z0-9_].*$/, "", mac)
        macro[FILENAME SUBSEP mac] = 1
      }
      startdepth = depth; startfn = fn; openedfn = ""
      if (!prep) {
        n = length(code); ident = ""; lastident = ""
        for (p = 1; p <= n; p++) {
          c = substr(code, p, 1)
          if (isident(c)) { ident = ident c; continue }
          if (ident != "") { lastident = ident; ident = "" }
          if (c == "(") {
            if (depth == 0 && parens == 0 && lastident != "") lastname = lastident
            parens++
          } else if (c == ")") {
            if (parens > 0) parens--
          }
          if (c == "=" && depth == 0 && parens == 0) hasassign = 1
          if (c == ";" && depth == 0) hasassign = 0
          # A brace inside a parameter list (a struct a function takes by value)
          # is no body of its own: only a brace outside the parentheses opens or
          # closes a function.
          if (parens == 0) {
            if (c == "{") {
              if (depth == 0) {
                fn = (hasassign || lastname == "") ? "(file scope)" : lastname
                if (openedfn == "") openedfn = fn
              }
              depth++
            } else if (c == "}") {
              depth--
              if (depth <= 0) { depth = 0; fn = "(file scope)"; lastname = ""; hasassign = 0 }
            }
          }
          if (c != " " && c != "\t") lastident = ""
        }
      }
      linefn = startfn
      if (startdepth == 0) linefn = (openedfn != "") ? openedfn : "(file scope)"
      declares = 0
      if (!prep && match(code, /(^|[^A-Za-z0-9_])STRING[ \t]*\*+[ \t]*[A-Za-z_][A-Za-z0-9_]*/)) {
        decl = substr(code, RSTART, RLENGTH)
        sub(/^.*\*[ \t]*/, "", decl)
        if (pass == 1) strname[FILENAME SUBSEP decl] = 1
        declares = 1
      }
      if (pass == 2) {
        exempt = (mode == "readers" && (linefn in isreader) && !prep)
        if (mode != "generic" && code ~ /##/) flag("R", "paste")
        if (prep && $0 ~ /^[ \t]*#[ \t]*include/ && mode == "strict") {
          target = $0
          gsub(/^[ \t]*#[ \t]*include[ \t]*[<"]/, "", target)
          gsub(/[>"].*$/, "", target)
          if (!(target in inc)) flag("R", "include")
        }
        if (!exempt) {
          if (mode != "generic") {
            if (code ~ /\[/) flag("R", "subscript")
            if (code ~ /(^|[^A-Za-z0-9_])bf([^A-Za-z0-9_]|$)/) flag("R", "bf")
            if (code ~ /(^|[^A-Za-z0-9_])mxlines([^A-Za-z0-9_]|$)/) flag("R", "mxlines")
            if (code ~ /(\.|->)[ \t]*s([^A-Za-z0-9_]|$)/) flag("R", "s-field")
          } else {
            kind = ""
            if (code ~ /(^|[^A-Za-z0-9_])bf([^A-Za-z0-9_]|$)/) {
              if (code ~ /STRING[ \t]*\*+[ \t]*bf/) kind = "D"
              else if (write_kind(code)) {
                kind = "W"
                # A write of a row that reads one on the right of that write is
                # both, whatever reaches the row it reads.
                rhsread = reads_after_write(code)
                if (rhsread == "string-name") flag("S", "string-name")
                else if (rhsread != "") flag("R", "bf")
              }
              else kind = "R"
              flag(kind, "bf")
            } else {
              struck = (!prep) ? string_name_in(code) : ""
              if (struck != "")
                flag((struck == "lines") ? "A" : "S", "string-name")
              else if (declares && startdepth > 0)
                flag((decl == "lines" || linefn == "(file scope)") ? "A" : "S", "string-decl")
            }
            # A cast to a row is a row: it reaches the bytes without a reader.
            if (!prep && code ~ /STRING[ \t]*\*+[ \t]*\)/) flag("S", "string-cast")
          }
          if (code ~ /wpe_row_text/) check_calls("wpe_row_text", cons)
          if (code ~ /wpe_row_bytes/) check_calls("wpe_row_bytes", walk)
          if (code ~ /wpe_row_from/) check_calls("wpe_row_from", walk)
          if (code ~ /wpe_row_cstr/) check_calls("wpe_row_cstr", walk)
          if (code ~ /wpe_row_view/) check_calls("wpe_row_view", walk)
          # A piece of a line a reader filled holds the bytes of the row itself:
          # a "[", "+" or "-" on it later in the function reads the row without
          # a reader.
          if (piece_read(code) != "") flag("R", "row-piece")
        }
      }
      if (code !~ /^[ \t]*$/) prevcode = code
    }'

# scan MODE FILE...: one "KIND<TAB>FILE<TAB>LINE<TAB>FUNCTION<TAB>TOKEN" per
# offence.  MODE is strict (every line), readers (strict outside the readers)
# or generic (the lines the allow file counts).  Each file is read twice: the
# first pass collects the names declared STRING * and the names defined as a
# macro, the second flags.
scan() {
  mode=$1
  shift
  for f in "$@"; do
    awk -v mode="$mode" -v readers="$READERS" -v includes="$INCLUDES" \
      -v consumers="$CONSUMERS" -v walkers="$WALKERS" -v pointers="$POINTERS" \
      -v copiers="$COPIERS" -v keepers="$KEEPERS" "$BLANK_C$AWK_SCAN" "$f" "$f"
  done
}

# generic_files: the top level sources the allow file counts.
generic_files() {
  for f in "$SRCDIR"/*.c "$SRCDIR"/*.h; do
    case ${f##*/} in
      we_buffer.c|we_brkmatch.c|we_rowread.h|config.h) ;;
      *) echo "$f" ;;
    esac
  done
}

# count_lines: "KIND|file|function|count" for each group of offences on stdin,
# the file named without its folder.
count_lines() {
  awk -F'\t' '
    { name = $2; sub(/^.*\//, "", name)
      key = $1 "|" name "|" $4
      if (!((key SUBSEP $3) in lineseen)) { lineseen[key SUBSEP $3] = 1; n[key]++ } }
    END { for (key in n) print key "|" n[key] }' | sort
}

# compare_allow COUNTS KEYS: name each group the allow file does not list, or
# lists with another count, and write each such group's "KIND|file|function" to
# KEYS so the failure can list that group's lines.
compare_allow() {
  awk -F'|' -v allow="$ALLOW" -v keys="$2" '
    # One group the allow file and the scan disagree about.
    function offend(key) { print key > keys }
    BEGIN {
      printf "" > keys
      while ((getline line < allow) > 0) {
        if (line ~ /^[ \t]*(#|$)/) continue
        split(line, a, "|")
        want[a[1] "|" a[2] "|" a[3]] = a[4]
        if (a[1] !~ /^[AWD]$/) { print "kind not allowed in " allow ": " line; badkind = 1 }
      }
    }
    { key = $1 "|" $2 "|" $3; have[key] = $4 }
    END {
      bad = badkind
      for (key in have)
        if (!(key in want)) { offend(key); print "not in " allow ": " key " (" have[key] " lines)"; bad = 1 }
        else if (want[key] != have[key]) { offend(key); print "stale count in " allow ": " key " lists " want[key] ", the scan finds " have[key]; bad = 1 }
      for (key in want)
        if (!(key in have)) { print "stale line in " allow ": " key " matches nothing"; bad = 1 }
      exit bad
    }' "$1"
}

# plant_strict FILE OUT: FILE with one offence of each strict kind appended.
plant_strict() {
  {
    cat "$1"
    printf '\nstatic int planted(BUFFER *b, int i, int j)\n{\n'
    printf ' int c = b->bf[i].s[j];      /* bf[0] in a comment is fine */\n'
    printf ' if (i < b->mxlines) return c;\n return "[mxlines]"[0];\n}\n'
    printf '\nstatic int wpe_row_byte2(BUFFER *b, int i, int j)\n{\n'
    printf '#define RAWC(b,i,j) ((b)->bf[(i)].s[(j)])\n return 0;\n}\n'
    printf '\nstatic int\nplanted_split(BUFFER *b, int i)\n{\n'
    printf ' return b->bf[i].len + b->mxlines;\n}\n'
    printf '\nstatic int planted_alias(BUFFER *b, int i, int j)\n{\n'
    printf ' const char *t = wpe_row_text(b, i, 0);\n return *(t + j - 1);\n}\n'
    printf '\nstatic int planted_arith(BUFFER *b, int i, int j)\n{\n'
    printf ' return wpe_row_text(b, i, 0)[j - 1];\n}\n'
    printf '\nstatic int planted_bytes(BUFFER *b, int i)\n{\n'
    printf ' const unsigned char *p = wpe_row_bytes(b, i, NULL) + 3;\n return *p;\n}\n'
    printf '\nstatic int planted_one(BUFFER *b){ return b->bf[0].len; }\n'
    printf 'static int planted_inherit(BUFFER *b, int i)\n{\n'
    printf ' return b->bf[i].len;\n}\n'
    printf '\nstatic int planted_misc(BUFFER *b, int i)\n{\n'
    printf '#include <stdio.h>\n#define GLUE(a) x##a\n return 0;\n}\n'
    printf '\nstatic int planted_ok(BUFFER *b, int i)\n{\n'
    printf ' return ispelse(wpe_row_text(b, i, 0)) + strncmp(wpe_row_text(b, i, 1), "ld", 2);\n}\n'
  } > "$2"
}

# plant_generic OUT: a file of the shapes the generic rule names, and of
# shapes it does not.  Each named function is one canary; the ones called
# after the code that keeps a row are the code's own shape, planted under the
# one name the rule allows it under.  The shapes a keeper is allowed are
# planted by plant_keepers, in files of the names KEEPERS gives them.
plant_generic() {
  {
    printf 'static int planted_byte(BUFFER *b, int i, int j)\n{\n return b->bf[i].s[j];\n}\n'
    printf 'static int planted_star(BUFFER *b, int y, int i)\n{\n return *(b->bf[y].s + i);\n}\n'
    printf 'static int planted_pair(BUFFER *b, int y)\n{\n return (b->bf + y)->s[0];\n}\n'
    printf 'static int planted_len(BUFFER *b, int y)\n{\n return b->bf[y].len;\n}\n'
    printf 'static void planted_addr(BUFFER *b, int y)\n{\n STRING *r = &b->bf[y];\n}\n'
    printf 'static void planted_array(BUFFER *b)\n{\n STRING *rows = b->bf;\n}\n'
    printf '#define RAW(b,y,x) ((b)->bf[(y)].s[(x)])\n'
    printf '#define PLANTED_ROW_BYTES 500\n'
    printf 'static int planted_alias(BUFFER *b, int y)\n{\n return wpe_row_text(b, y, 0)[-1];\n}\n'
    printf 'static int planted_local(BUFFER *b, int y)\n{\n const char *t = wpe_row_text(b, y, 0);\n return t[1];\n}\n'
    printf 'static int planted_bytes(BUFFER *b, int y, int n)\n{\n return *(wpe_row_bytes(b, y, &n) + 3);\n}\n'
    printf 'static const char *planted_cstr(BUFFER *b, int y)\n{\n const char *p = wpe_row_cstr(b, y);\n return p;\n}\n'
    printf 'static int planted_from(BUFFER *b, int y)\n{\n const unsigned char *p = wpe_row_from(b, y, 1);\n return p[0];\n}\n'
    printf 'static int planted_param(STRING *row, int i)\n{\n return row->s[i];\n}\n'
    printf 'static int planted_oneline(BUFFER *b){ return b->bf[0].len; }\n'
    printf 'static void planted_decl(void)\n{\n STRING *loose = NULL;\n}\n'
    # A row kept in an array that outlives the statement: named unless the
    # function is the one that builds the array for the row owner.
    printf 'static int planted_view_escape(BUFFER *b, int y)\n{\n const char *s; int n;\n'
    printf ' wpe_row_view(b, y, &s, &n);\n return s[-1];\n}\n'
    printf 'static int planted_cstr_escape(BUFFER *b, int y, char **lns)\n{\n'
    printf ' lns[0] = wpe_row_cstr(b, y);\n lns[0][0] = (char) y;\n return lns[0][-1];\n}\n'
    printf 'static int planted_from_escape(BUFFER *b, int y, WpeBufferPart *parts)\n{\n'
    printf ' parts[0].s = wpe_row_from(b, y, 0);\n return parts[0].s[-1];\n}\n'
    # A keeper is a file and a function: its own name in another file is
    # another function, and keeps nothing.
    printf 'static int e_p_run_begin(BUFFER *b, int y)\n{\n const char *s; int n;\n'
    printf ' wpe_row_view(b, y, &s, &n);\n return s[-1];\n}\n'
    # A piece of a line holds the bytes of the row itself, so what the function
    # reads through it later is a read; and only wpe_row_from gives a piece
    # (wpe_row_cstr gives a whole row, which no field of a part takes).
    printf 'static int planted_piece_index(BUFFER *b, int y)\n{\n WpeBufferPart p;\n'
    printf ' p.s = wpe_row_from(b, y, 0);\n return p.s[-1];\n}\n'
    printf 'static int planted_piece_offset(BUFFER *b, int y)\n{\n WpeBufferPart p;\n'
    printf ' p.s = wpe_row_from(b, y, 0);\n return *(p.s + 1);\n}\n'
    printf 'static int planted_piece_arrow(BUFFER *b, int y, WpeBufferPart *pp)\n{\n'
    printf ' pp->s = wpe_row_from(b, y, 0);\n return pp->s[-1];\n}\n'
    printf 'static int planted_cstr_piece(BUFFER *b, int y)\n{\n struct { const char *s; } keep;\n'
    printf ' keep.s = wpe_row_cstr(b, y);\n return keep.s[-1];\n}\n'
    # What a consumer of a reader gives back, and the count a copy is given: a
    # constant of the caller's own (a literal, an arithmetic of literals, a
    # sizeof, a macro of the file) is named, since no row gave it.
    printf 'static int planted_consumer_index(BUFFER *b, int y)\n{\n'
    printf ' return strstr(wpe_row_text(b, y, 0), "x")[-1];\n}\n'
    printf 'static void planted_consumer_count(BUFFER *b, int y, char *dst)\n{\n'
    printf ' memcpy(dst, wpe_row_text(b, y, 0), 500);\n}\n'
    printf 'static void planted_copy_sizeof(BUFFER *b, int y, char *dst)\n{\n'
    printf ' memcpy(dst, wpe_row_text(b, y, 0), sizeof dst);\n}\n'
    printf 'static void planted_copy_macro(BUFFER *b, int y, char *dst)\n{\n'
    printf ' memcpy(dst, wpe_row_text(b, y, 0), PLANTED_ROW_BYTES);\n}\n'
    printf 'static void planted_copy_arith(BUFFER *b, int y, char *dst)\n{\n'
    printf ' memcpy(dst, wpe_row_text(b, y, 0), 2 * 8);\n}\n'
    # A row reached through a cast: no name declared STRING * to see.
    printf 'static int planted_cast_row(BUFFER *b, int y)\n{\n'
    printf ' return (*(STRING **)b)[y].s[-1];\n}\n'
    # A write of another struct's field is the read of a row it names, and a
    # write of a row that reads one on its right is both, whatever reaches the
    # row it reads: the array of rows itself as an operand, or a name declared
    # STRING *.
    printf 'static WpeBufferPart planted_write_off_row(BUFFER *b, int y, int from)\n{\n'
    printf ' WpeBufferPart p;\n p.s = b->bf[y].s + from;\n p.n = 0;\n return p;\n}\n'
    printf 'static void planted_write_reads_row(BUFFER *b, int y, int x, Undo *rec)\n{\n'
    printf ' b->bf[y].s[x] = rec->u.c + b->bf[y].s[x - 1];\n}\n'
    printf 'static void planted_write_reads_addr(BUFFER *b, int y, int x)\n{\n'
    printf ' b->bf[y].s[x] = *(b->bf + y - 1)->s;\n}\n'
    printf 'static void planted_write_reads_string(BUFFER *b, int y, int x, STRING *row)\n{\n'
    printf ' b->bf[y].s[x] = row->s[x - 1];\n}\n'
    printf 'static void planted_write_from_reader(BUFFER *b, int y)\n{\n'
    printf ' b->bf[y].len = e_str_len(wpe_row_bytes(b, y, NULL));\n}\n'
    # A struct in a parameter list is no body of its own: the lines of the
    # function belong to the function, not to "(file scope)".
    printf 'static int planted_struct_param(struct { int n; } mark, BUFFER *b)\n{\n'
    printf ' return b->bf[mark.n].len;\n}\n'
    # The shapes the rules above must let through.
    printf 'static int planted_next(BUFFER *b)\n{\n return 0;\n}\n'
    printf 'static WpeBufferPart planted_part_from_reader(BUFFER *b, int y, int from)\n{\n'
    printf ' WpeBufferPart p;\n p.s = wpe_row_from(b, y, from);\n p.n = 0;\n return p;\n}\n'
    printf 'static int planted_strlen_result(BUFFER *b, int y)\n{\n'
    printf ' int total = strlen(wpe_row_text(b, y, 0)) + 1;\n return total;\n}\n'
    printf 'static void planted_copy_row_len(BUFFER *b, int y, char *dst)\n{\n'
    printf ' memcpy(dst, wpe_row_text(b, y, 0), (size_t) wpe_row_len(b, y));\n}\n'
    printf 'static void planted_copy_run_count(BUFFER *b, int y, char *dst)\n{\n'
    printf ' int n = 0;\n memcpy(dst, wpe_row_bytes(b, y, &n), (size_t) n);\n}\n'
    printf 'static int planted_ok(BUFFER *b, int y, int x, STRING *out)\n{\n'
    printf ' int bfx = 0;  /* bf[0] in a comment */\n'
    printf ' char *m = "b->bf[0].s";\n'
    printf ' bfx = wpe_row_byte(b, y, x) + wpe_row_len(b, y);\n'
    printf ' return strncmp(wpe_row_text(b, y, 0), "ld", 2) + (out == NULL) + bfx + (m != NULL);\n}\n'
  } > "$1"
}

# plant_keepers DIR: the shape each keeper is allowed, in a file of the name
# KEEPERS gives it, and one function of that file that is not the keeper.  A
# keeper is a file and a function: the same function name in another file
# keeps nothing, and another function of the right file keeps nothing either.
plant_keepers() {
  mkdir -p "$1"
  {
    printf 'static void e_p_run_begin(BUFFER *b, struct e_p_run *run, int i)\n{\n'
    printf ' wpe_row_view(b, i, &run->rows[i].s, &run->rows[i].len);\n}\n'
    printf 'static int planted_view_wrong_fn(BUFFER *b, int y)\n{\n const char *s; int n;\n'
    printf ' wpe_row_view(b, y, &s, &n);\n return s[-1];\n}\n'
  } > "$1/we_prog.c"
  {
    printf 'static const char *e_lsp_buffer_text(BUFFER *b, const char **lns, int y)\n{\n'
    printf ' lns[y] = wpe_row_cstr(b, y);\n return lns[0];\n}\n'
  } > "$1/we_debug.c"
  {
    printf 'static int e_ins_long_run(BUFFER *b, int ya, int xa, WpeBufferPart *parts)\n{\n'
    printf ' parts[0].s = wpe_row_from(b, ya, 0);\n parts[2].s = wpe_row_from(b, ya, xa);\n'
    printf ' return parts[0].n;\n}\n'
  } > "$1/we_edit.c"
}

# expect_named FOUND WANT...: fail unless each "function: token (kind)" is in
# FOUND.
expect_named() {
  found=$1
  shift
  missing=0
  for want in "$@"; do
    if ! grep -q "	$want\$" "$found"; then
      echo "check-brk-bounds: SELFTEST FAILED, not named: $want"
      missing=1
    fi
  done
  return $missing
}

# expect_legal FOUND NAME...: fail when a function the rules must let through
# is named at all.
expect_legal() {
  found=$1
  shift
  named=0
  for legal in "$@"; do
    if grep -q "^$legal[^A-Za-z0-9_]" "$found"; then
      echo "check-brk-bounds: SELFTEST FAILED, a legal function was named: $legal"
      grep "^$legal[^A-Za-z0-9_]" "$found"
      named=1
    fi
  done
  return $named
}

selftest() {
  plant_strict "$SRCDIR/we_brkmatch.c" "$WORK/planted.c"
  scan strict "$WORK/planted.c" |
    awk -F'\t' '{ print $4 "\t" $4 ": " $5 " (" $1 ")" }' > "$WORK/strict.txt"
  plant_generic "$WORK/generic.c"
  scan generic "$WORK/generic.c" |
    awk -F'\t' '{ print $4 "\t" $4 ": " $5 " (" $1 ")" }' > "$WORK/generic.txt"
  plant_keepers "$WORK/keep"
  scan generic "$WORK/keep/we_prog.c" "$WORK/keep/we_debug.c" "$WORK/keep/we_edit.c" |
    awk -F'\t' '{ print $4 "\t" $4 ": " $5 " (" $1 ")" }' > "$WORK/keepers.txt"
  cp "$SRCDIR/we_rowread.h" "$WORK/rowread.h"
  {
    printf '\nstatic inline int planted_reader(const BUFFER *b, int y)\n{\n return b->bf[y].len;\n}\n'
    printf '#define RAW_READ(b,y) ((b)->bf[(y)].len)\n'
    printf 'static inline int wpe_row_len2(const BUFFER *b){ return b->mxlines; }\n'
  } >> "$WORK/rowread.h"
  scan readers "$WORK/rowread.h" |
    awk -F'\t' '{ print $4 "\t" $4 ": " $5 " (" $1 ")" }' > "$WORK/readers.txt"
  bad=0
  expect_named "$WORK/strict.txt" "planted: subscript (R)" "planted: bf (R)" \
    "planted: mxlines (R)" "planted: s-field (R)" "wpe_row_byte2: bf (R)" \
    "wpe_row_byte2: s-field (R)" "planted_split: bf (R)" "planted_split: mxlines (R)" \
    "planted_arith: row-call (R)" "planted_bytes: row-call (R)" \
    "planted_alias: row-call (R)" "planted_one: bf (R)" "planted_inherit: bf (R)" \
    "planted_misc: include (R)" "planted_misc: paste (R)" || bad=1
  expect_named "$WORK/generic.txt" "planted_byte: bf (R)" "planted_star: bf (R)" \
    "planted_pair: bf (R)" "planted_len: bf (R)" "planted_addr: bf (R)" \
    "planted_array: bf (R)" "(file scope): bf (R)" "planted_alias: row-call (R)" \
    "planted_local: row-call (R)" "planted_bytes: row-call (R)" \
    "planted_param: string-name (S)" "planted_oneline: bf (R)" \
    "planted_cstr: row-call (R)" "planted_from: row-call (R)" \
    "planted_decl: string-decl (S)" \
    "planted_view_escape: row-call (R)" "planted_cstr_escape: row-call (R)" \
    "planted_from_escape: row-call (R)" "planted_consumer_index: row-call (R)" \
    "planted_consumer_count: row-call (R)" "planted_cast_row: string-cast (S)" \
    "planted_write_off_row: bf (R)" \
    "planted_write_reads_row: bf (W)" "planted_write_reads_row: bf (R)" \
    "planted_write_from_reader: bf (W)" \
    "e_p_run_begin: row-call (R)" \
    "planted_piece_index: row-piece (R)" "planted_piece_offset: row-piece (R)" \
    "planted_piece_arrow: row-piece (R)" "planted_cstr_piece: row-call (R)" \
    "planted_copy_sizeof: row-call (R)" "planted_copy_macro: row-call (R)" \
    "planted_copy_arith: row-call (R)" \
    "planted_write_reads_addr: bf (W)" "planted_write_reads_addr: bf (R)" \
    "planted_write_reads_string: bf (W)" \
    "planted_write_reads_string: string-name (S)" \
    "planted_struct_param: bf (R)" || bad=1
  expect_named "$WORK/keepers.txt" "planted_view_wrong_fn: row-call (R)" || bad=1
  expect_named "$WORK/readers.txt" "planted_reader: bf (R)" "(file scope): bf (R)" \
    "wpe_row_len2: mxlines (R)" || bad=1
  # A write of a row that reads nothing but a reader's answer is no read, and
  # a field of another struct is no write of a row.
  for absent in "planted_write_from_reader: bf (R)" "planted_write_off_row: bf (W)"; do
    if grep -q "	$absent\$" "$WORK/generic.txt"; then
      echo "check-brk-bounds: SELFTEST FAILED, named when it must not be: $absent"
      bad=1
    fi
  done
  expect_legal "$WORK/generic.txt" planted_ok planted_next planted_part_from_reader \
    planted_strlen_result planted_copy_row_len planted_copy_run_count || bad=1
  expect_legal "$WORK/keepers.txt" e_p_run_begin e_lsp_buffer_text e_ins_long_run || bad=1
  expect_legal "$WORK/strict.txt" planted_ok || bad=1
  expect_legal "$WORK/readers.txt" wpe_row_len || bad=1
  if [ $bad -ne 0 ]; then
    exit 1
  fi
  echo "check-brk-bounds: selftest ok"
}

[ $SELFTEST -eq 0 ] || selftest

if [ -n "$FILE" ]; then
  OFFENDERS=$(scan strict "$FILE" | awk -F'\t' '{ print $2 ":" $3 ": " $4 ": " $5 }')
  if [ -n "$OFFENDERS" ]; then
    echo "check-brk-bounds: code reads the text without the readers of we_rowread.h:"
    echo "$OFFENDERS"
    exit 1
  fi
  echo "check-brk-bounds: ok, $FILE reads the rows through the readers"
  exit 0
fi

# Strict: the bracket matcher and the readers' own header.
OFFENDERS=$( {
  scan strict "$SRCDIR/we_brkmatch.c"
  scan readers "$SRCDIR/we_rowread.h"
} | awk -F'\t' '{ print $2 ":" $3 ": " $4 ": " $5 }')
if [ -n "$OFFENDERS" ]; then
  echo "check-brk-bounds: code other than the readers of we_rowread.h reads the text:"
  echo "$OFFENDERS"
  exit 1
fi

# Generic: every other top level source, against the allow file.
GENERIC=$(generic_files)
# shellcheck disable=SC2086
scan generic $GENERIC > "$WORK/found.tsv"
count_lines < "$WORK/found.tsv" > "$WORK/counts.txt"
if [ $LIST -eq 1 ]; then
  awk -F'|' '{ print $1 "|" $2 "|" $3 "|" $4 "|" }' "$WORK/counts.txt"
  exit 0
fi
if [ ! -f "$ALLOW" ]; then
  echo "check-brk-bounds: no allow file $ALLOW" >&2
  exit 1
fi
if ! compare_allow "$WORK/counts.txt" "$WORK/keys.txt" > "$WORK/diff.txt"; then
  echo "check-brk-bounds: the rows are read outside the readers somewhere the allow file does not say:"
  cat "$WORK/diff.txt"
  # The lines of the groups that are named above, not of every listed group.
  awk -F'\t' -v keys="$WORK/keys.txt" '
    BEGIN { while ((getline line < keys) > 0) named[line] = 1 }
    { name = $2; sub(/^.*\//, "", name)
      if (($1 "|" name "|" $4) in named) print "  " $2 ":" $3 ": " $4 ": " $5 " (" $1 ")" }' \
    "$WORK/found.tsv" | head -n 40
  exit 1
fi
echo "check-brk-bounds: ok, only the readers index the rows; $(wc -l < "$WORK/counts.txt" | tr -d ' ') groups listed in the allow file"
