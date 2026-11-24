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
#     the count of window titles compared against a name is held at a
#     ceiling.  A title comparison is how a file the user named after one of
#     the editor's own panes is taken for that pane; the comparisons go down
#     as the readers move to the pane the owner holds, and a new one fails
#     here.
#  7. the payload is reached through the accessors: the count of accessor
#     calls over the count of holds is printed as the evidence, and the uses
#     of the two legacy fields the accessors still mirror are held at a
#     ceiling.  They go down as the readers move over; a new one fails here.
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
CEIL_B_USES=947
CEIL_S_USES=268
CEIL_CASTS=25
CEIL_DTMD=32
CEIL_DATNAM=73

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
# A window's title handed to a comparison: how a file the user named after
# one of the editor's own panes gets taken for that pane.  Counted, not
# refused: the readers move to the pane the owner holds one file at a time.
DATNAM_CMP_PAT='(strcmp|strncmp|strcasecmp|strstr|memcmp|fnmatch|WpeStrcstr) *\([^;]*datnam'

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

# How many window titles the readers compare against a name.  The owner is
# not a reader, and the one file-name helper the rule allows is counted with
# the rest until the pass that moves the readers off titles makes it strict.
count_titles() {
  readers=$(product_readers "$1")
  # shellcheck disable=SC2086
  grep -h -E -o -e "$DATNAM_CMP_PAT" $readers 2>/dev/null | grep -c .
}

check_no_pasted_name() {
  dir=$1
  pasted=0
  invented=0
  titles=0

  pasted=$(grep -h '##' "$dir"/we_winkind.c "$dir"/we_winkind.h 2>/dev/null |
           grep -c .)
  invented=$(grep -E -l -- "$PASTED_PAT" $(product_files "$dir") 2>/dev/null |
             grep -c .)
  titles=$(count_titles "$dir")

  printf 'rule 6: token pastes in the owner: %s, files naming a member the block does not have: %s\n' \
    "$pasted" "$invented"
  printf 'rule 6: window titles compared against a name: %s (ceiling %s)\n' \
    "$titles" "$CEIL_DATNAM"
  if [ "$pasted" -ne 0 ]; then
    say_problem "rule 6: the owner pastes $pasted token(s): a pasted name reaches a member without ever spelling it"
  fi
  if [ "$invented" -ne 0 ]; then
    say_problem "rule 6: $invented file(s) name a member of the owner's block that the block does not have"
  fi
  if [ "$titles" -gt "$CEIL_DATNAM" ]; then
    say_problem "rule 6: $titles window title(s) are compared against a name, over the ceiling of $CEIL_DATNAM: a title comparison takes a file the user named after a pane for that pane, which is what the tag the owner holds is for"
  fi
}

# ---- rule 7: the payload is reached through the accessors --------------

ACCESSOR_PAT='e_window_(is_text|text|screen|files|list|list_kind|pane|holds|top|top_except_pane|text_under|pane_find)\('
HOLD_PAT='e_window_(hold_text|hold_files|hold_list|hold_dropdown|replace_text|set_pane)\('

count_rule7() {
  dir=$1
  readers=$(product_readers "$dir")
  # shellcheck disable=SC2086
  accessors=$(grep -h -E -o -e "$ACCESSOR_PAT" $readers 2>/dev/null | grep -c .)
  # shellcheck disable=SC2086
  holds=$(grep -h -E -o -e "$HOLD_PAT" $readers 2>/dev/null | grep -c .)
  # shellcheck disable=SC2086
  b_uses=$(grep -h -E -o -e '->b\b' $readers 2>/dev/null | grep -c .)
  # shellcheck disable=SC2086
  s_uses=$(grep -h -E -o -e '->s\b' $readers 2>/dev/null | grep -c .)
  # shellcheck disable=SC2086
  casts=$(grep -h -E -o -e '\((BUFFER|FLWND|FLBFFR) \*\)' $readers 2>/dev/null |
          grep -c .)
  # shellcheck disable=SC2086
  dtmd=$(grep -h -E -o -e 'DTMD_ISTEXT\(' $readers 2>/dev/null | grep -c .)
  printf '%s %s %s %s %s %s\n' \
    "$accessors" "$holds" "$b_uses" "$s_uses" "$casts" "$dtmd"
}

read_rule7() {
  set -- $(count_rule7 "$1")
  R7_ACCESSORS=$1
  R7_HOLDS=$2
  R7_B=$3
  R7_S=$4
  R7_CASTS=$5
  R7_DTMD=$6
}

check_accessors() {
  dir=$1
  read_rule7 "$dir"

  printf 'rule 7: the payload is reached through %s accessors over %s holds\n' \
    "$R7_ACCESSORS" "$R7_HOLDS"
  printf 'rule 7: legacy field uses ->b %s (ceiling %s), ->s %s (ceiling %s), payload casts %s (ceiling %s), DTMD_ISTEXT %s (ceiling %s)\n' \
    "$R7_B" "$CEIL_B_USES" "$R7_S" "$CEIL_S_USES" \
    "$R7_CASTS" "$CEIL_CASTS" "$R7_DTMD" "$CEIL_DTMD"

  if [ "$R7_ACCESSORS" -eq 0 ]; then
    say_problem "rule 7: no product file asks the owner anything: the accessors are the only way to reach a payload"
  fi
  if [ "$R7_B" -gt "$CEIL_B_USES" ]; then
    say_problem "rule 7: ->b is used $R7_B times, over its ceiling of $CEIL_B_USES: a new reader of the legacy field belongs on an accessor"
  fi
  if [ "$R7_S" -gt "$CEIL_S_USES" ]; then
    say_problem "rule 7: ->s is used $R7_S times, over its ceiling of $CEIL_S_USES"
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
  printf 'CEIL_B_USES=%s\n' "$R7_B"
  printf 'CEIL_S_USES=%s\n' "$R7_S"
  printf 'CEIL_CASTS=%s\n' "$R7_CASTS"
  printf 'CEIL_DTMD=%s\n' "$R7_DTMD"
  printf 'CEIL_DATNAM=%s\n' "$(count_titles "$SRCDIR")"
}

run_checks() {
  check_compiler_flag "$1"
  check_block_private "$1"
  check_stack_counted "$1"
  check_no_pasted_name "$1"
  check_accessors "$1"
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
    sh -c 'printf "\nstruct wpe_window_held { FLWND *planted; };\nvoid planted_cast_reader(FENSTER *f){ (void)((struct wpe_window_held *)f->b)->planted; }\n" >> "$0/we_wind.c"'

  plant_and_expect "a stack write the owner is not told about" "rule 5" \
    sh -c 'printf "\nvoid planted_push(ECNT *cn){ (cn->mxedt)++; }\n" >> "$0/we_wind.c"'

  plant_and_expect "a stack raise the owner is not told about" "rule 5" \
    sh -c 'printf "\nvoid planted_raise(ECNT *cn){ cn->mxedt = 1; }\n" >> "$0/we_wind.c"'

  plant_and_expect "a window put on the stack the owner is not told about" "rule 5" \
    sh -c 'printf "\nvoid planted_put(ECNT *cn, FENSTER *g, int i){ cn->f[i] = g; }\n" >> "$0/we_wind.c"'

  plant_and_expect "a pasted member name" "rule 6" \
    sh -c 'printf "\n#define PLANTED(f, m) ((f)->held_->m)\nvoid p(FENSTER *f){ (void)f->text_; }\n" >> "$0/we_winkind.c"'

  plant_and_expect "a window title compared against a pane's name" "rule 6" \
    sh -c 'printf "\nint planted_is_pane(FENSTER *f){ return strcmp(f->datnam, STRIP_WINDOW_MESSAGES); }\n" >> "$0/we_wind.c"'

  plant_and_expect "new readers of the legacy fields" "rule 7" \
    sh -c 'i=0; while [ $i -lt 60 ]; do printf "\nvoid planted_cast_%s(FENSTER *f){ (void)(FLWND *)f->b; (void)f->s; (void)DTMD_ISTEXT(f->dtmd); }\n" "$i" >> "$0/we_wind.c"; i=$((i+1)); done'

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

if [ $SELFTEST -eq 1 ]; then
  # Made here, in the main shell, and not inside selftest: work_dir's EXIT trap
  # belongs to the shell that ends the script, and a folder made in a function
  # called from a subshell would go with the subshell.
  work_dir window-kind 2
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
