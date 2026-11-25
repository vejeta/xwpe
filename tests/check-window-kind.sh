#!/bin/sh
# check-window-kind.sh -- a window's payload is asked through its owner.
#
# One field of a window used to carry four different things: the text of a
# file, the File Manager's directory lists, a list window's rows and a
# dropdown's.  Two crashes came out of that, and both were reachable from one
# key: a language-server command read a list's corner numbers as a row
# pointer, and a build that ended while the text loop waited for a key was
# read through the payload the loop took before the wait.
#
# we_winkind.c now owns the answer.  Its block holds the three payloads as
# separate typed pointers and is named in no other file, so the compiler
# refuses an alias, a macro, a pasted name, a helper in another file and a
# void * round trip.  What this script keeps true is that nothing crept back
# in around it.
#
#   check-window-kind.sh [--srcdir DIR] [--list] [--selftest]
#
# It counts; it does not grep for presence.  A count is evidence a reader can
# check, and a bare `grep -q` says nothing at all when it fails.  What it
# prints and holds:
#
#  1. the compiler still refuses an incompatible pointer: the warning is not
#     silenced in configure.ac, and XWPE_WARNFLAGS in the generated Makefile
#     does not carry it either.  Both are checked because the word is never
#     written literally in the Makefile: configure builds it from a loop word,
#     and a compiler that does not know the warning adds nothing, so the
#     Makefile alone can look clean while the source silences it.
#  2. the owner's private block is named in we_winkind.c and in the one
#     declaration in edit.h, and in no other file.  A `void *` is the one
#     launder the compiler cannot see once the payload is opaque, so this
#     rule also counts a `(void *)` cast of an accessor's answer and a
#     `void *` variable set from one: either is a member read from outside by
#     another route.
#  3. no macro's expansion is the block, so no alias can hand a payload out
#     under another name.
#  4. the block is defined in no file but its owner.  A second definition is
#     what would let the file that carries it read a member directly, and it
#     is counted by the definition's own spelling rather than by the field's
#     name: such a reader casts the legacy field to the type it just defined,
#     so it never writes `held_` and a scan for the field does not see it.
#     The compiler is what refuses the spelling no pattern names, so the
#     selftest also builds one file that reaches a member and holds that the
#     build fails.
#  5. every write to the window stack is counted by the owner.  A key loop
#     that waited compares the count, and a raise changes what is on top
#     without changing how many windows there are, so a write the owner was
#     not told about is a window that can change under a waiting loop.  A
#     write into the stack's own tables counts as such a write: it puts a
#     window on top without touching the number of windows.  The one write
#     that is not a change is the desktop's start value.
#  6. no pasted name reaches a member, the owner itself pastes no tokens, and
#     no window title is compared against a name or copied into a pointer
#     anywhere but in the owner.  A title comparison is how a file the user
#     named after one of the editor's own panes is taken for that pane; the
#     readers ask the pane the owner holds, or a file-name question of the
#     owner, and a new comparison fails here.  The rule reads statements, not
#     lines: comments and the bodies of strings are dropped and the text is
#     joined up to each `;`, `{` and `}`, so a comparison split over two
#     lines, or one in the header of a `for`, is seen.  "Copied" means that
#     the whole right operand of an assignment or an initialiser is the title,
#     through parentheses, casts, both arms of `?:`, `+ n` and `&...[0]`;
#     `p = e_mkfilename(f->dirct, f->datnam)` hands the title to a call and is
#     not a copy.  The owner's e_window_title_text counts as the title too, so
#     the accessor is for handing straight to a call, never for keeping.  This closes the comparison split over lines and the local
#     pointer, and nothing more: a pointer returned into the title
#     (`strrchr(f->datnam, '.')`), a copy into a buffer
#     (`strcpy(buf, f->datnam)`) and the title passed to a helper that
#     compares are not seen here.  They close at the string itself, in a later
#     change that takes the title out of the window's own declaration.
#  7. the payload is reached through the accessors: the count of accessor
#     calls over the count of holds is printed as the evidence.  The window
#     declares no member named b or s (the two fields that carried the text
#     and its view state), so the compiler refuses a reader that goes around
#     the owner, and this rule fails if either comes back.  A payload cast of
#     the three kinds (a BUFFER, a list window or a File Manager's lists) is
#     refused outright: with the accessors typed there is nothing to cast.
#     What stays held at a ceiling is the number of DTMD_ISTEXT questions
#     asked of the mode character.
#  8. one maker.  e_window_open is the one function that finds or makes a
#     window, and the kind it is asked for, not the name, decides what it
#     is: e_edit_open is called from e_window_open only, e_window_set_pane is
#     called from the one body that builds a window and from nowhere else,
#     e_edit_view and e_window_view_slot are named nowhere, and e_edit,
#     e_edit_outcome and e_open_named_file never take a string literal or a
#     name in capitals as the file (the editor has no reason to open a user's
#     file by a name it wrote itself, so no list of pane titles is kept).
#
# --list prints the ceiling lines the tree needs.  --selftest copies the tree
# to a scratch folder, breaks one rule at a time, and asserts that each broken
# copy fails on its own rule and that the unbroken copy passes.

set -u

# The one way a shell test makes its work folder, so a signal ends the script
# with its folder and not after it (tests/check-sh-idiom.sh rule 1).
. "$(dirname "$0")/workdir.sh"
# The one way a shell test runs the compiler, flags and all, so the build
# canary below compiles with the compiler configure gave the build.
. "$(dirname "$0")/compiler.sh"

SRCDIR=.
LIST=0
SELFTEST=0

while [ $# -gt 0 ]; do
  case "$1" in
    --srcdir)   SRCDIR=${2:?--srcdir needs a folder}; shift 2 ;;
    --list)     LIST=1; shift ;;
    --selftest) SELFTEST=1; shift ;;
    *) echo "usage: $0 [--srcdir DIR] [--list] [--selftest]" >&2; exit 2 ;;
  esac
done

# The ceilings of rules 6 and 7, measured on the tree that introduced the
# owner and only ever allowed to go down.  --list prints what the tree needs
# now.
CEIL_CASTS=0
CEIL_DTMD=32

# A write that cannot change what is on top says so on its own line: the
# desktop's start value and the clipboard's slot 0, which is never a window
# of the stack.  The reason stands in the source, where a reader of that line
# meets it, instead of in a list here.
STACK_NOT_TOP='not the stack top'
# Writes this many lines apart or closer are one change of what is on top --
# a push writes the count, the id table and the window table -- and the note
# that tells the owner may stand anywhere in that change, or this far after
# it.  A write with no note that near is a window that can change under a
# waiting key loop uncounted.
STACK_NOTE_GAP=20

# A write to the stack top: it goes up, it goes down, or it is set -- and so
# is a write into either of the two tables the stack is.  The patterns are
# tight on purpose: "i <= cn->mxedt" only reads the count, and "cn->f[i] !=
# src" only reads a slot.
WRITE_PAT='mxedt[) ]*([+][+]|[-][-])|mxedt *= *[^=]|(->|[.])f\[[^]]*\] *= *[^=]|(->|[.])edt\[[^]]*\] *= *[^=]'
# The owner's private field, as a whole word: "e_status_drop_held_line" and
# "struct wpe_window_held" both contain the letters and are neither of them
# the field.
HELD_PAT='(^|[^A-Za-z0-9_])held_([^A-Za-z0-9_]|$)'
# A second definition of the block, spelled as the definition itself.  A file
# that carries one reaches a member through a cast of the legacy field, so it
# never writes the field's name and HELD_PAT does not see it.
BLOCK_DEF_PAT='struct wpe_window_held[[:space:]]*\{'
# A member of the block reached by a name the block does not have.
PASTED_PAT='->(text_|screen_|list_|files_)([^A-Za-z0-9_]|$)'
# The one launder an opaque payload still allows: an accessor's answer cast
# to `void *`, or a `void *` variable set from one.  Either puts a payload
# where any reader can take it back as any other kind's.
VOID_CAST_PAT='[(]void *\*[)] *[A-Za-z_]*e_window_'
VOID_VAR_PAT='void *\*[A-Za-z_][A-Za-z0-9_]* *= *[A-Za-z_]*e_window_'
# The window's title: the member read through a pointer or a value (a local
# variable that happens to be called datnam is not the title), and the text the
# owner hands out for it, which is the same string by another name.
TITLE_MEMBER_PAT='(->|[.])datnam|e_window_title_text[ ]*[(]'

# The one blanker of C comments and literals (tests/blank-c.awk), put in front
# of the awk programs below.
BLANK_C=$(cat "$(dirname "$0")/blank-c.awk")

# Rules 6 and 8 read statements, not lines.  This turns a C file into one
# statement per line, "file TAB line TAB function TAB header TAB text": the
# comments are dropped and a string's or a character's body is dropped (so a
# ';' or an '=' inside one is not text of the statement), the preprocessor's
# lines with their continuations are statements of their own, and the rest is
# joined up to each ';', '{' and '}'.  "function" is the function the
# statement is in ("" outside one) and "header" is 1 for the statement that
# opens a function's body.  POSIX awk only.
STATEMENTS_AWK='
function fname(t,    n) {
  if (!match(t, /[A-Za-z_][A-Za-z0-9_]*[ ]*\(/)) return ""
  n = substr(t, RSTART, RLENGTH)
  sub(/[ ]*\($/, "", n)
  return n
}
function emit(l, f, h, t) {
  gsub(/[ \t]+/, " ", t)
  sub(/^ /, "", t)
  if (t ~ /^[{}]?$/) return
  print FILENAME "\t" l "\t" f "\t" h "\t" t
}
FNR == 1 { blank_incom = 0; depth = 0; fn = ""; stmt = ""; pp = ""; inpp = 0 }
{
  raw = $0
  line = blank_c(raw, 0)
  if (inpp) {
    pp = pp " " line
    if (raw !~ /\\$/) { emit(ppl, "", 0, pp); inpp = 0; pp = "" }
    next
  }
  if (line ~ /^[ \t]*#/) {
    ppl = FNR; pp = line
    if (raw ~ /\\$/) { inpp = 1; next }
    emit(ppl, "", 0, pp); pp = ""
    next
  }
  while (line != "") {
    if (!match(line, /[;{}]/)) {
      if (line ~ /[^ \t]/) { if (stmt == "") startl = FNR; stmt = stmt " " line }
      break
    }
    d = substr(line, RSTART, 1)
    piece = substr(line, 1, RSTART)
    line = substr(line, RSTART + 1)
    if (stmt == "") startl = FNR
    stmt = stmt " " piece
    hdr = 0
    if (d == "{") {
      if (depth == 0 && stmt ~ /\(/) { fn = fname(stmt); hdr = 1 }
      depth++
    }
    emit(startl, fn, hdr, stmt)
    if (d == "}") { depth--; if (depth <= 0) { depth = 0; fn = "" } }
    stmt = ""
  }
}'

# Rule 6 over the statements of the readers: "CMP file:line" for a title
# handed to a comparison, "COPY file:line" for a title copied into a pointer.
TITLE_AWK='
function balanced(s, from,    n, ch, k, depth, c) {
  n = split(s, ch, "")
  depth = 0
  for (k = from; k <= n; k++) {
    c = ch[k]
    if (c == "(") depth++
    else if (c == ")") { if (depth == 0) break; depth-- }
  }
  return substr(s, from, k - from)
}
function strip_calls(r,    n, ch, out, k, depth, c, prev) {
  n = split(r, ch, "")
  out = ""
  prev = ""
  for (k = 1; k <= n; k++) {
    c = ch[k]
    if (c == "(" && prev ~ /[A-Za-z0-9_]/) {
      depth = 1
      while (depth > 0 && k < n) {
        k++
        c = ch[k]
        if (c == "(") depth++
        else if (c == ")") depth--
      }
      out = out "()"
      prev = ")"
      continue
    }
    out = out c
    prev = c
  }
  return out
}
function operand(ch, n, from, t,    k, depth, c) {
  depth = 0
  for (k = from; k <= n; k++) {
    c = ch[k]
    if (c == "(") depth++
    else if (c == ")") { if (depth == 0) break; depth-- }
    else if (c == "," && depth == 0) break
    else if (c == ";") break
    else if (c == "=" && depth == 0) {
      if (ch[k + 1] == "=" || ch[k - 1] ~ /[!<>]/) { k++; continue }
      return ""
    }
  }
  return strip_calls(substr(t, from, k - from))
}
function copies(t,    n, ch, i, prev, found) {
  found = 0
  n = split(t, ch, "")
  for (i = 1; i <= n; i++) {
    if (ch[i] != "=") continue
    prev = i > 1 ? ch[i - 1] : ""
    if (ch[i + 1] == "=") { i++; continue }
    if (prev ~ /[!<>=]/) continue
    if (operand(ch, n, i + 1, t) ~ MEMBER) found++
  }
  return found
}
function compares(t,    n, rest, s, l, args) {
  n = 0
  rest = t
  while (match(rest, /(strcmp|strncmp|strcasecmp|strstr|memcmp|fnmatch|WpeStrcstr)[ ]*\(/)) {
    s = RSTART; l = RLENGTH
    if (s > 1 && substr(rest, s - 1, 1) ~ /[A-Za-z0-9_]/) { rest = substr(rest, s + l); continue }
    args = balanced(rest, s + l)
    if (args ~ MEMBER) n++
    rest = substr(rest, s + l)
  }
  return n
}
BEGIN { FS = "\t"; MEMBER = ENVIRON["MEMBER_PAT"] }
{
  if (index($5, "datnam") == 0 && index($5, "e_window_title_text") == 0) next
  if (compares($5) > 0) print "CMP " $1 ":" $2
  if (copies($5) > 0) print "COPY " $1 ":" $2
}'

# Rule 8 over the statements of every product file but the owner's: "OPEN" for
# a second caller of the body, "TAG" for a tag written anywhere but in it,
# "GONE" for a removed name that is back, "NAME" for a file opened by a name
# the editor wrote itself.
MAKER_AWK='
function balanced(s, from,    n, ch, k, depth, c) {
  n = split(s, ch, "")
  depth = 0
  for (k = from; k <= n; k++) {
    c = ch[k]
    if (c == "(") depth++
    else if (c == ")") { if (depth == 0) break; depth-- }
  }
  return substr(s, from, k - from)
}
function second_arg(a,    len, ch, k, depth, c, nth, start) {
  len = split(a, ch, "")
  depth = 0; nth = 1; start = 1
  for (k = 1; k <= len; k++) {
    c = ch[k]
    if (c == "(" || c == "[") depth++
    else if (c == ")" || c == "]") depth--
    else if (c == "," && depth == 0) {
      if (nth == 2) return substr(a, start, k - start)
      nth++; start = k + 1
    }
  }
  return nth == 2 ? substr(a, start) : ""
}
function named_literal(t,    rest, s, l, a) {
  rest = t
  while (match(rest, /(^|[^A-Za-z0-9_])(e_edit_outcome|e_edit|e_open_named_file)[ ]*\(/)) {
    s = RSTART; l = RLENGTH
    a = second_arg(balanced(rest, s + l))
    gsub(/^[ ]+|[ ]+$/, "", a)
    if (a ~ /^"/ || a ~ /^[A-Z][A-Z0-9_]*$/) return 1
    rest = substr(rest, s + l)
  }
  return 0
}
BEGIN { FS = "\t" }
$4 == 1 { next }
{
  t = $5
  where = $1 ":" $2
  if (t ~ /(^|[^A-Za-z0-9_])e_edit_open[ ]*\(/ && $3 != "e_window_open") print "OPEN " where
  if (t ~ /(^|[^A-Za-z0-9_])e_window_set_pane[ ]*\(/ &&
      ($1 !~ /we_edit\.c$/ || $3 != "e_edit_open")) print "TAG " where
  if (t ~ /(^|[^A-Za-z0-9_])(e_edit_view|e_window_view_slot)([^A-Za-z0-9_]|$)/) print "GONE " where
  if (named_literal(t)) print "NAME " where
}'

problems=0

# A problem goes to stderr and the counts a rule prints go to stdout.  Both
# start with "rule N:", so a selftest canary that read one stream could not
# tell a rule that found the break from the same rule's evidence line saying
# it found none.
say_problem() {
  printf '%s\n' "$1" >&2
  problems=$((problems + 1))
}

# The product's own sources: the top level of the tree, which is where the
# editor lives.  The test drivers, the tools and any build copy of the tree
# (.asan-build, .delegation) are not the product.
product_files() {
  for f in "$1"/*.c "$1"/*.h; do
    [ -f "$f" ] || continue
    printf '%s\n' "$f"
  done
}

# The product's .c files as one line, without the owner itself: rule 7 counts
# the readers, and the owner's own definitions are not a reader.
product_readers() {
  for f in "$1"/*.c; do
    [ -f "$f" ] || continue
    case "$f" in
      */we_winkind.c) continue ;;
    esac
    printf '%s ' "$f"
  done
}

# The product files outside the owner that define the block, one name per
# line.  A definition anywhere but we_winkind.c puts a member in reach of the
# file that carries it, in a header as much as in a .c.
block_defined_outside() {
  for f in $(product_files "$1"); do
    case "$f" in
      */we_winkind.c) continue ;;
    esac
    if grep -E -q "$BLOCK_DEF_PAT" "$f"; then
      basename "$f"
    fi
  done
}

# ---- rule 1: the compiler still refuses an incompatible pointer ---------

check_compiler_flag() {
  dir=$1
  in_configure=0
  in_makefile=0

  if [ -f "$dir/configure.ac" ]; then
    # The loop word configure builds the flag from, not a comment about it.
    in_configure=$(grep -E '^for ace_w in.*incompatible-pointer-types' \
                   "$dir/configure.ac" | grep -c .)
  fi
  if [ -f "$dir/Makefile" ]; then
    in_makefile=$(grep '^XWPE_WARNFLAGS' "$dir/Makefile" |
                  grep -c 'incompatible-pointer-types')
  fi
  printf 'rule 1: incompatible-pointer-types silenced in configure.ac: %s, in the generated Makefile: %s\n' \
    "$in_configure" "$in_makefile"
  if [ "$in_configure" -ne 0 ]; then
    say_problem "rule 1: configure.ac silences -Wincompatible-pointer-types ($in_configure loop word(s)): the compiler must refuse a payload of the wrong kind, not let the build quiet it"
  fi
  if [ "$in_makefile" -ne 0 ]; then
    say_problem "rule 1: XWPE_WARNFLAGS in the generated Makefile carries -Wno-incompatible-pointer-types"
  fi
}

# ---- rules 2 to 4: the owner's block stays private ---------------------

check_block_private() {
  dir=$1
  named_elsewhere=0
  edit_h_names=0
  macro_lines=0
  defined_outside=""
  defined_count=0
  files=""

  for f in $(product_files "$dir"); do
    case "$f" in
      */we_winkind.c) continue ;;
      */edit.h)
        # The one declaration of the field is allowed; a second mention, a
        # macro over it or a definition is not.
        edit_h_names=$(grep -E -c "$HELD_PAT" "$f")
        if [ "$edit_h_names" -gt 1 ]; then
          say_problem "rule 2: edit.h names the owner's block $edit_h_names times; the one declaration of the field is all it carries"
        fi
        continue ;;
    esac
    if grep -E -q "$HELD_PAT" "$f"; then
      files="$files $(basename "$f")"
      named_elsewhere=$((named_elsewhere + 1))
    fi
  done

  macro_lines=$(grep -h '^#define' $(product_files "$dir") 2>/dev/null |
                grep -E -c "$HELD_PAT|wpe_window_held")
  defined_outside=$(block_defined_outside "$dir")
  defined_count=$(printf '%s\n' "$defined_outside" | grep -c .)
  # shellcheck disable=SC2086
  void_casts=$(grep -h -E -o -e "$VOID_CAST_PAT" $(product_readers "$dir") \
               2>/dev/null | grep -c .)
  # shellcheck disable=SC2086
  void_vars=$(grep -h -E -o -e "$VOID_VAR_PAT" $(product_readers "$dir") \
              2>/dev/null | grep -c .)

  printf 'rule 2: files outside we_winkind.c and edit.h naming the block: %s%s\n' \
    "$named_elsewhere" "${files:+ ($files )}"
  printf 'rule 2: accessor answers cast to void *: %s, void * variables set from one: %s\n' \
    "$void_casts" "$void_vars"
  printf 'rule 3: macro lines whose expansion is the block: %s\n' "$macro_lines"
  printf 'rule 4: files outside we_winkind.c that define the block: %s%s\n' \
    "$defined_count" "${defined_outside:+ ($defined_outside)}"

  if [ "$named_elsewhere" -ne 0 ]; then
    say_problem "rule 2: $named_elsewhere file(s) outside we_winkind.c and edit.h name the owner's block:$files -- a member read from outside is how one kind's payload gets read as another's"
  fi
  if [ "$void_casts" -ne 0 ] || [ "$void_vars" -ne 0 ]; then
    say_problem "rule 2: $void_casts accessor answer(s) cast to void * and $void_vars void * variable(s) set from one: a void * is the one launder an opaque payload allows, and what comes back out of it is any kind the reader likes"
  fi
  if [ "$macro_lines" -ne 0 ]; then
    say_problem "rule 3: $macro_lines macro line(s) expand to the owner's block: an alias hands a payload out under another name"
  fi
  if [ "$defined_count" -ne 0 ]; then
    say_problem "rule 4: $defined_count file(s) outside we_winkind.c define struct wpe_window_held:$defined_outside -- a second definition lets that file read a member directly, through a cast of the legacy field that never names it"
  fi
}

# ---- rule 5: every write to the window stack is counted ----------------

stack_awk() {
  # One awk over one file: how many writes to the stack, how many notes to
  # the owner, and how many changes it was not told about.
  #
  # Writes this close together are one change of what is on top -- a push
  # writes the count, the id table and the window table, and one note tells
  # the owner of all three -- so they are read as a group and the note may
  # stand anywhere in the group or up to the same gap after it.  A write
  # that cannot change what is on top says so on its own line and is not a
  # write for this rule's purposes.
  #
  # The patterns reach awk through the environment and not through an
  # assignment argument: awk unescapes the value of an assignment, which
  # would take the backslashes out of the bracket escapes.
  WRITE_PAT="$WRITE_PAT" NOT_TOP_PAT="$STACK_NOT_TOP" awk -v gap="$1" '
    BEGIN {
      writes = 0; notes = 0; untold = 0
      open = 0; told = 0; last = -1
      writepat = ENVIRON["WRITE_PAT"]; nottop = ENVIRON["NOT_TOP_PAT"]
    }
    index($0, nottop) { next }
    /e_window_stack_changed *\(/ { notes++; if (open) told = 1; next }
    $0 ~ writepat {
      writes++
      if (!open || NR - last > gap) {
        if (open && !told) untold++
        open = 1; told = 0
      }
      last = NR
      next
    }
    open && NR - last > gap { if (!told) untold++; open = 0 }
    END {
      if (open && !told) untold++
      printf "%d %d %d", writes, notes, untold
    }'
}

check_stack_counted() {
  dir=$1
  writes=0
  notes=0
  untold=0
  where=""

  for f in "$dir"/*.c; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    read -r w n u <<EOF
$(stack_awk "$STACK_NOTE_GAP" < "$f")
EOF
    [ "$w" -eq 0 ] && [ "$n" -eq 0 ] && continue
    writes=$((writes + w))
    notes=$((notes + n))
    if [ "$u" -gt 0 ]; then
      untold=$((untold + u))
      where="$where $base($u)"
    fi
  done

  printf 'rule 5: writes to the window stack: %s, told to the owner: %s, untold changes: %s%s\n' \
    "$writes" "$notes" "$untold" "${where:+ in$where}"
  if [ "$untold" -ne 0 ]; then
    say_problem "rule 5: $untold change(s) of the window stack in$where are not told to e_window_stack_changed: a key loop that waits compares the count, so a window can change under it uncounted"
  fi
}

# ---- rule 6: no pasted name reaches a member ---------------------------

# The statements of some files, one per line (see STATEMENTS_AWK), in the file
# $1; the files are the rest of the arguments.
statements_into() {
  out=$1
  shift
  awk "$BLANK_C$STATEMENTS_AWK" "$@" > "$out" 2>/dev/null
}

# The titles the readers compare or copy outside the owner, one location per
# line ("CMP file:line" or "COPY file:line").  The owner is not a reader: its
# file-name questions (e_window_title_is, e_window_title_ends_with,
# e_window_is_file, e_window_is_path) are the only place a title is compared.
title_findings() {
  readers=$(product_readers "$1")
  # shellcheck disable=SC2086
  statements_into "$WORK/readers.stm" $readers
  MEMBER_PAT="$TITLE_MEMBER_PAT" awk "$TITLE_AWK" "$WORK/readers.stm"
}

# The locations of one kind of finding, as a short list for a message.
finding_places() {
  printf '%s\n' "$1" | grep "^$2 " | head -5 | sed "s/^$2 //" | tr '\n' ' '
}

check_no_pasted_name() {
  dir=$1
  pasted=0
  invented=0
  titles=0
  copies=0

  pasted=$(grep -h '##' "$dir"/we_winkind.c "$dir"/we_winkind.h 2>/dev/null |
           grep -c .)
  invented=$(grep -E -l -- "$PASTED_PAT" $(product_files "$dir") 2>/dev/null |
             grep -c .)
  found=$(title_findings "$dir")
  titles=$(printf '%s\n' "$found" | grep -c '^CMP ')
  copies=$(printf '%s\n' "$found" | grep -c '^COPY ')

  printf 'rule 6: token pastes in the owner: %s, files naming a member the block does not have: %s\n' \
    "$pasted" "$invented"
  printf 'rule 6: window titles compared outside the owner: %s, copied into a pointer: %s (none allowed)\n' \
    "$titles" "$copies"
  if [ "$pasted" -ne 0 ]; then
    say_problem "rule 6: the owner pastes $pasted token(s): a pasted name reaches a member without ever spelling it"
  fi
  if [ "$invented" -ne 0 ]; then
    say_problem "rule 6: $invented file(s) name a member of the owner's block that the block does not have"
  fi
  if [ "$titles" -ne 0 ]; then
    say_problem "rule 6: $titles window title(s) are compared outside the owner ($(finding_places "$found" CMP)): a title comparison takes a file the user named after a pane for that pane, which is what the tag the owner holds is for"
  fi
  if [ "$copies" -ne 0 ]; then
    say_problem "rule 6: $copies window title(s) are copied into a pointer outside the owner ($(finding_places "$found" COPY)): a copy is compared where no rule sees it; ask the owner the question, or hand the title to the call that needs it"
  fi
}

# ---- rule 8: one maker ------------------------------------------------

# The places rule 8 refuses, one per line ("OPEN", "TAG", "GONE" or "NAME",
# then file:line).  The owner's own files are not scanned for the tag: it
# defines the function that writes it.
maker_findings() {
  files=""
  for f in $(product_files "$1"); do
    case "$f" in
      */we_winkind.c|*/we_winkind.h) continue ;;
    esac
    files="$files $f"
  done
  # shellcheck disable=SC2086
  statements_into "$WORK/product.stm" $files
  awk "$MAKER_AWK" "$WORK/product.stm"
}

check_one_maker() {
  dir=$1
  found=$(maker_findings "$dir")
  second=$(printf '%s\n' "$found" | grep -c '^OPEN ')
  tags=$(printf '%s\n' "$found" | grep -c '^TAG ')
  gone=$(printf '%s\n' "$found" | grep -c '^GONE ')
  named=$(printf '%s\n' "$found" | grep -c '^NAME ')

  printf 'rule 8: e_edit_open called outside e_window_open: %s, a window tag written outside the one body: %s\n' \
    "$second" "$tags"
  printf 'rule 8: removed openers named again: %s, a file opened by a name the editor wrote itself: %s (none allowed)\n' \
    "$gone" "$named"
  if [ "$second" -ne 0 ]; then
    say_problem "rule 8: e_edit_open is called outside e_window_open ($(finding_places "$found" OPEN)): one function finds or makes a window, from the kind it was asked for"
  fi
  if [ "$tags" -ne 0 ]; then
    say_problem "rule 8: a window's tag is written outside the body that builds it ($(finding_places "$found" TAG)): the kind is written once, from the ask, and never from a name"
  fi
  if [ "$gone" -ne 0 ]; then
    say_problem "rule 8: e_edit_view or e_window_view_slot is named again ($(finding_places "$found" GONE)): a view is opened by its kind through e_window_view_open"
  fi
  if [ "$named" -ne 0 ]; then
    say_problem "rule 8: $named file open(s) by a literal or a capitalised name ($(finding_places "$found" NAME)): a user's file is opened by the name the user gave, and a window of the editor's own by its kind"
  fi
}

# ---- rule 7: the payload is reached through the accessors --------------

ACCESSOR_PAT='e_window_(is_text|text|screen|files|list|list_kind|pane|holds|top|top_except_pane|text_under|pane_find)\('
HOLD_PAT='e_window_(hold_text|hold_files|hold_list|hold_dropdown|replace_text|set_pane)\('

# A payload cast, in either of the two spellings a text rule can see: a cast
# of an accessor's answer to any pointer type (the answer is already typed, so
# a cast of it is a pun), and a cast to any of the payload types by name, with
# the struct tags and the view-state type, and blanks anywhere inside the
# parentheses.  What no text rule can catch: a cast through a typed variable
# (a pointer of the wrong kind set from an accessor and cast on a later line)
# and a typedef alias of a payload type declared in another file.  Those are
# what the compiler's incompatible-pointer-types error, kept on by rule 1, is
# for.
# The insert mode written from a kind: a list's number stored where the typing
# mode of text lives.  Text rules see the spellings a constructor would use (a
# kind name, the parameter that carries it, a digit above the three typing
# modes); a kind computed into a variable first is kept out by the owner,
# which sets a window that is not text back to insert mode 0 when it holds
# the payload, and by test_winkind.c, which poisons the mode before each hold.
INS_KIND_PAT='(->|[.])ins[[:space:]]*=[[:space:]]*([^=;]*(WPE_LISTKIND|kind|[^A-Za-z0-9_]sw)|[2-79]([^0-9]|$))'
CAST_ACCESSOR_PAT='[(][[:space:]]*[A-Za-z_][A-Za-z0-9_ ]*[*]+[[:space:]]*[)][[:space:]]*e_window_(text|screen|files|list)[(]'
CAST_TYPE_PAT='[(][[:space:]]*(struct[[:space:]]+)?(BUFFER|BFF|FLWND|fl_wnd|FLBFFR|FLBFF|SCHIRM|SCHRM)[[:space:]]*[*]+[[:space:]]*[)]'

# How many members named by the one letter the window used to carry (b for
# the text, s for its view state) the window's own declaration has.  Only the
# declaration of the window is read, so the same letter in another structure
# (the text's cursor, a row's bytes) is not counted.
window_members() {
  awk -v letter="$2" '
    /^typedef struct FNST *\{/ { inside = 1; next }
    inside && /^\} *FENSTER;/ { inside = 0 }
    inside && $0 ~ "[ \t*,]" letter "[ \t]*[;,[]" { n++ }
    END { print n + 0 }' "$1/edit.h" 2>/dev/null
}

count_rule7() {
  dir=$1
  readers=$(product_readers "$dir")
  # shellcheck disable=SC2086
  accessors=$(grep -h -E -o -e "$ACCESSOR_PAT" $readers 2>/dev/null | grep -c .)
  # shellcheck disable=SC2086
  holds=$(grep -h -E -o -e "$HOLD_PAT" $readers 2>/dev/null | grep -c .)
  b_uses=$(window_members "$dir" b)
  s_uses=$(window_members "$dir" s)
  # shellcheck disable=SC2086
  casts=$(grep -h -E -o -e "$CAST_TYPE_PAT" -e "$CAST_ACCESSOR_PAT" $readers \
          2>/dev/null | grep -c .)
  # shellcheck disable=SC2086
  dtmd=$(grep -h -E -o -e 'DTMD_ISTEXT\(' $readers 2>/dev/null | grep -c .)
  # shellcheck disable=SC2086
  ins_kinds=$(grep -h -E -o -e "$INS_KIND_PAT" $readers 2>/dev/null | grep -c .)
  printf '%s %s %s %s %s %s %s\n' \
    "$accessors" "$holds" "$b_uses" "$s_uses" "$casts" "$dtmd" "$ins_kinds"
}

read_rule7() {
  set -- $(count_rule7 "$1")
  R7_ACCESSORS=$1
  R7_HOLDS=$2
  R7_B=$3
  R7_S=$4
  R7_CASTS=$5
  R7_DTMD=$6
  R7_INS_KINDS=$7
}

check_accessors() {
  dir=$1
  read_rule7 "$dir"

  printf 'rule 7: the payload is reached through %s accessors over %s holds\n' \
    "$R7_ACCESSORS" "$R7_HOLDS"
  printf 'rule 7: members b and s declared by the window: %s and %s (none allowed), payload casts %s (ceiling %s), DTMD_ISTEXT %s (ceiling %s)\n' \
    "$R7_B" "$R7_S" "$R7_CASTS" "$CEIL_CASTS" "$R7_DTMD" "$CEIL_DTMD"

  printf 'rule 7: insert mode written from a kind: %s (none allowed)\n' \
    "$R7_INS_KINDS"
  if [ "$R7_INS_KINDS" -gt 0 ]; then
    say_problem "rule 7: $R7_INS_KINDS write(s) of a list's kind into a window's insert mode: the kind is held beside the rows (e_window_hold_list) and the insert mode is the typing mode of text"
  fi
  if [ "$R7_ACCESSORS" -eq 0 ]; then
    say_problem "rule 7: no product file asks the owner anything: the accessors are the only way to reach a payload"
  fi
  if [ "$R7_B" -gt 0 ]; then
    say_problem "rule 7: the window declares a member b again: the text is asked of the owner, and a member beside it is a second way to reach it"
  fi
  if [ "$R7_S" -gt 0 ]; then
    say_problem "rule 7: the window declares a member s again: the view state is asked of the owner, and a member beside it is a second way to reach it"
  fi
  if [ "$R7_CASTS" -gt "$CEIL_CASTS" ]; then
    say_problem "rule 7: $R7_CASTS payload cast(s), over the ceiling of $CEIL_CASTS: a cast of a window's payload is the pun the owner exists to end"
  fi
  if [ "$R7_DTMD" -gt "$CEIL_DTMD" ]; then
    say_problem "rule 7: DTMD_ISTEXT is used $R7_DTMD times, over the ceiling of $CEIL_DTMD: the kind is the owner's answer, not the mode character"
  fi
}

list_ceilings() {
  read_rule7 "$SRCDIR"
  printf 'CEIL_CASTS=%s\n' "$R7_CASTS"
  printf 'CEIL_DTMD=%s\n' "$R7_DTMD"
}

run_checks() {
  check_compiler_flag "$1"
  check_block_private "$1"
  check_stack_counted "$1"
  check_no_pasted_name "$1"
  check_accessors "$1"
  check_one_maker "$1"
}

# ---- the selftest -------------------------------------------------------

selftest() {
  work=$WORK        # made by work_dir in the main shell, removed when it ends
  bad=0

  # A copy of the tree's own sources, so a planted break is the only
  # difference between a passing run and a failing one.
  mkdir -p "$work/tree"
  for f in "$SRCDIR"/*.c "$SRCDIR"/*.h "$SRCDIR/configure.ac"; do
    [ -f "$f" ] && cp "$f" "$work/tree/"
  done
  if [ -f "$SRCDIR/Makefile" ]; then
    grep '^XWPE_WARNFLAGS' "$SRCDIR/Makefile" > "$work/tree/Makefile"
  else
    printf 'XWPE_WARNFLAGS = -Wno-int-conversion\n' > "$work/tree/Makefile"
  fi

  run_one() {
    problems=0
    run_checks "$1" > /dev/null 2>&1
    [ "$problems" -eq 0 ]
  }

  # The tree as it stands must pass: a selftest over a tree that already
  # fails proves nothing about the rules.
  if ! run_one "$work/tree"; then
    echo "check-window-kind: selftest: the tree under test already fails" >&2
    ( problems=0; run_checks "$work/tree" )
    bad=1
  fi

  # Each planted break must fail, and must fail on its own rule.  A rule's
  # evidence line and its problem line both begin with "rule N:", so only the
  # problems, which go to stderr, are read here: a canary that matched the
  # evidence line would pass over a rule that saw nothing at all.
  plant_and_expect() {
    what=$1
    rule=$2
    shift 2
    rm -rf "$work/broken"
    cp -r "$work/tree" "$work/broken"
    "$@" "$work/broken"
    problems=0
    run_checks "$work/broken" > /dev/null 2>"$work/found"
    if ! grep -q "^$rule:" "$work/found"; then
      echo "check-window-kind: selftest: $what did not trip $rule" >&2
      echo "check-window-kind: selftest: what it did report:" >&2
      sed 's/^/  /' "$work/found" >&2
      bad=1
    fi
  }

  plant_and_expect "the warning silenced again" "rule 1" \
    sh -c 'printf "\nfor ace_w in incompatible-pointer-types int-conversion; do :; done\n" >> "$0/configure.ac"'

  plant_and_expect "the block named from outside" "rule 2" \
    sh -c 'printf "\nvoid planted_reader(FENSTER *f){ (void)f->held_; }\n" >> "$0/we_wind.c"'

  plant_and_expect "an accessor's answer laundered through a void *" "rule 2" \
    sh -c 'printf "\nvoid *planted_launder(FENSTER *f){ return (void *)e_window_list(f); }\nvoid planted_var(FENSTER *f){ void *rows = e_window_list(f); (void)rows; }\n" >> "$0/we_wind.c"'

  plant_and_expect "an alias for the block" "rule 3" \
    sh -c 'printf "\n#define WIN_HELD(f) ((f)->held_)\n" >> "$0/we_wind.c"'

  plant_and_expect "the block defined in a header" "rule 4" \
    sh -c 'printf "\nstruct wpe_window_held { int planted; };\n" >> "$0/we_winkind.h"'

  # The spelling the field's own name does not cover: a .c defines the block
  # and reaches a member through a cast of the legacy field, so no `held_`
  # appears in it for rule 2 to see and no header for the old rule 4 to scan.
  plant_and_expect "the block defined in a .c file and a member read through it" "rule 4" \
    sh -c 'printf "\nstruct wpe_window_held { FLWND *planted; };\nvoid planted_cast_reader(FENSTER *f){ (void)((struct wpe_window_held *)e_window_list(f))->planted; }\n" >> "$0/we_wind.c"'

  plant_and_expect "a stack write the owner is not told about" "rule 5" \
    sh -c 'printf "\nvoid planted_push(ECNT *cn){ (cn->mxedt)++; }\n" >> "$0/we_wind.c"'

  plant_and_expect "a stack raise the owner is not told about" "rule 5" \
    sh -c 'printf "\nvoid planted_raise(ECNT *cn){ cn->mxedt = 1; }\n" >> "$0/we_wind.c"'

  plant_and_expect "a window put on the stack the owner is not told about" "rule 5" \
    sh -c 'printf "\nvoid planted_put(ECNT *cn, FENSTER *g, int i){ cn->f[i] = g; }\n" >> "$0/we_wind.c"'

  plant_and_expect "a pasted member name" "rule 6" \
    sh -c 'printf "\n#define PLANTED(f, m) ((f)->held_->m)\nvoid p(FENSTER *f){ (void)f->text_; }\n" >> "$0/we_winkind.c"'

  plant_and_expect "a window title compared against a pane's name" "rule 6" \
    sh -c 'printf "\n#define PLANTED_TITLE \"Messages\"\nint planted_is_pane(FENSTER *f){ return strcmp(f->datnam, PLANTED_TITLE); }\n" >> "$0/we_wind.c"'

  plant_and_expect "a title compared in a call split over two lines" "rule 6" \
    sh -c 'printf "\nint planted_split(FENSTER *f){\n return strcmp(f->dirct,\n   f->datnam);\n}\n" >> "$0/we_wind.c"'

  plant_and_expect "the owner's title text handed to a comparison" "rule 6" \
    sh -c 'printf "\nint planted_text_cmp(FENSTER *f){ return strcmp(e_window_title_text(f), \"Messages\"); }\n" >> "$0/we_wind.c"'

  plant_and_expect "the owner's title text kept in a local" "rule 6" \
    sh -c 'printf "\nint planted_text_copy(FENSTER *f){\n const char *nm = e_window_title_text(f);\n return nm[0];\n}\n" >> "$0/we_wind.c"'

  plant_and_expect "a title compared in the header of a for" "rule 6" \
    sh -c 'printf "\nvoid planted_for(FENSTER *f, char *x){\n int i;\n for (i = 0; i < 3 && !strcmp(x,\n   f->datnam); i++)\n  ;\n}\n" >> "$0/we_wind.c"'

  plant_and_expect "a title copied into a local and compared" "rule 6" \
    sh -c 'printf "\nint planted_local(FENSTER *f){\n char *nm = f->datnam;\n return strcmp(nm, \"Messages\");\n}\n" >> "$0/we_wind.c"'

  plant_and_expect "a title copied through an assignment split over two lines" "rule 6" \
    sh -c 'printf "\nvoid planted_assign(FENSTER *f, const char **p){\n *p =\n   f->datnam;\n}\n" >> "$0/we_wind.c"'

  plant_and_expect "a title copied through the arm of a ternary" "rule 6" \
    sh -c 'printf "\nconst char *planted_ternary(FENSTER *f, int i){\n const char *p;\n p = i < 0 ? f->datnam : \"x\";\n return p;\n}\n" >> "$0/we_wind.c"'

  plant_and_expect "a title copied through a cast, an offset and an index" "rule 6" \
    sh -c 'printf "\nvoid planted_offset(FENSTER *f){\n char *a = (char *)(f->datnam + 1);\n char *b = &f->datnam[0];\n (void)a; (void)b;\n}\n" >> "$0/we_wind.c"'

  plant_and_expect "a title copied in a macro continued over two lines" "rule 6" \
    sh -c 'printf "\n#define PLANTED_COPY(f, p) \\\\\n  ((p) = (f)->datnam)\n" >> "$0/we_wind.c"'

  plant_and_expect "a literal name given to e_edit" "rule 8" \
    sh -c 'printf "\nvoid planted_literal(ECNT *cn){ e_edit(cn, \"Messages\"); }\n" >> "$0/we_wind.c"'

  plant_and_expect "a name in capitals given to e_edit" "rule 8" \
    sh -c 'printf "\n#define PLANTED_NAME \"Messages\"\nvoid planted_capitals(ECNT *cn){ e_edit(cn, PLANTED_NAME); }\n" >> "$0/we_wind.c"'

  plant_and_expect "a literal name given to e_edit_outcome" "rule 8" \
    sh -c 'printf "\nvoid planted_outcome(ECNT *cn, WpeReadOutcome *why){ e_edit_outcome(cn, \"Help\", why); }\n" >> "$0/we_wind.c"'

  plant_and_expect "a literal name given to e_open_named_file" "rule 8" \
    sh -c 'printf "\nvoid planted_named(ECNT *cn){ e_open_named_file(cn, \"Messages\"); }\n" >> "$0/we_wind.c"'

  plant_and_expect "a call split over two lines with a literal name" "rule 8" \
    sh -c 'printf "\nvoid planted_split_open(ECNT *cn){\n e_edit(cn,\n   \"Watches\");\n}\n" >> "$0/we_wind.c"'

  plant_and_expect "a second caller of the body that builds a window" "rule 8" \
    sh -c 'printf "\nvoid planted_open(ECNT *cn){ e_edit_open(cn, 0, 0, 0, 0); }\n" >> "$0/we_wind.c"'

  plant_and_expect "a window tag written outside the body that builds it" "rule 8" \
    sh -c 'printf "\nvoid planted_tag(FENSTER *f){ e_window_set_pane(f, WPE_PANE_MESSAGES); }\n" >> "$0/we_prog.c"'

  plant_and_expect "a removed opener named again" "rule 8" \
    sh -c 'printf "\nint e_edit_view(ECNT *cn, char *name);\n" >> "$0/we_wind.c"'

  plant_and_expect "the window's text member declared again" "rule 7" \
    sh -c 'awk "{ print } /^typedef struct FNST *\\{/ { print \" struct BFF *b;\" }" "$0/edit.h" > "$0/edit.h.planted" && mv "$0/edit.h.planted" "$0/edit.h"'

  plant_and_expect "the window's view-state member declared again" "rule 7" \
    sh -c 'awk "{ print } /^typedef struct FNST *\\{/ { print \" struct SCHRM *s;\" }" "$0/edit.h" > "$0/edit.h.planted" && mv "$0/edit.h.planted" "$0/edit.h"'

  plant_and_expect "a payload cast" "rule 7" \
    sh -c 'printf "\nvoid planted_cast(FENSTER *f){ (void)(FLWND *)e_window_list(f); }\n" >> "$0/we_wind.c"'

  plant_and_expect "a cast to a struct tag" "rule 7" \
    sh -c 'printf "\nvoid planted_tag(void *p){ (void)(struct BFF *)p; (void)(struct fl_wnd *)p; }\n" >> "$0/we_wind.c"'

  plant_and_expect "a cast to the view-state type" "rule 7" \
    sh -c 'printf "\nvoid planted_view(void *p){ (void)(SCHIRM *)p; (void)(struct SCHRM *)p; }\n" >> "$0/we_wind.c"'

  plant_and_expect "a cast spelled with blanks inside" "rule 7" \
    sh -c 'printf "\nvoid planted_blanks(void *p){ (void)( FLBFF  * )p; (void)(BUFFER*)p; }\n" >> "$0/we_wind.c"'

  plant_and_expect "an accessor's answer cast to any pointer type" "rule 7" \
    sh -c 'printf "\nvoid planted_alias(FENSTER *f){ (void)(struct planted_alias *)e_window_text(f); (void)(SCHIRM *)e_window_screen(f); }\n" >> "$0/we_wind.c"'

  plant_and_expect "a list's kind written into the insert mode" "rule 7" \
    sh -c 'printf "\nvoid planted_ins(FENSTER *f, int sw){ f->ins = sw; }\nvoid planted_ins2(FENSTER *f){ f->ins = WPE_LISTKIND_WINDOWS; }\n" >> "$0/we_wind.c"'

  plant_and_expect "new questions asked of the mode character" "rule 7" \
    sh -c 'i=0; while [ $i -lt 60 ]; do printf "\nvoid planted_mode_%s(FENSTER *f){ (void)DTMD_ISTEXT(f->dtmd); }\n" "$i" >> "$0/we_wind.c"; i=$((i+1)); done'

  # The compiler's own canary, and the one member read no count can see: a
  # file outside we_winkind.c that names a member of the block must not
  # build, because there the block is an incomplete type.  Its control is the
  # same file naming the field alone, and it must build: without that, a
  # canary refused for another reason (a header the copy lacks, a compiler
  # that is not there) would look like the member read was caught.
  build_canary() {
    mkdir -p "$work/canary"
    printf '#include "edit.h"\nvoid wpe_canary_field(FENSTER *f){ (void)f->held_; }\n' \
      > "$work/canary/field.c"
    printf '#include "edit.h"\nvoid wpe_canary_member(FENSTER *f){ (void)f->held_->text; }\n' \
      > "$work/canary/member.c"
    if ! test_cc -fsyntax-only -I"$work/tree" "$work/canary/field.c" \
         > "$work/canary/field.log" 2>&1; then
      echo "check-window-kind: selftest: the build canary's own control does not compile, so the canary proves nothing" >&2
      sed 's/^/  /' "$work/canary/field.log" >&2
      bad=1
      return 1
    fi
    if test_cc -fsyntax-only -I"$work/tree" "$work/canary/member.c" \
         > "$work/canary/member.log" 2>&1; then
      echo "check-window-kind: selftest: a file outside we_winkind.c named a member of the block and the compiler allowed it" >&2
      bad=1
      return 1
    fi
    return 0
  }
  test_cc_require
  build_canary

  if [ $bad -eq 0 ]; then
    echo "check-window-kind: selftest ok"
  else
    echo "check-window-kind: selftest FAILED"
  fi
  return $bad
}

# ---- main ---------------------------------------------------------------

# Made here, in the main shell, and not inside a function: work_dir's EXIT trap
# belongs to the shell that ends the script, and a folder made in a function
# called from a subshell would go with the subshell.  Rules 6 and 8 keep the
# statements they read in it.
work_dir window-kind 2

if [ $SELFTEST -eq 1 ]; then
  selftest
  exit $?
fi

if [ ! -d "$SRCDIR" ]; then
  echo "check-window-kind: $SRCDIR is not a folder" >&2
  exit 2
fi

if [ $LIST -eq 1 ]; then
  list_ceilings
  exit 0
fi

run_checks "$SRCDIR"

if [ "$problems" -eq 0 ]; then
  echo "check-window-kind: every rule holds"
  exit 0
fi
echo "check-window-kind: $problems problem(s)"
exit 1
