#!/bin/sh
# check-checkpoint-record.sh -- what the assistant's checkpoint knows about each
# path has one owner (we_ai_ckrec.c).
#
# A name that is missing from a list has four causes: it was not there, the
# walk was refused, the walk was cut, or the object is there under another
# name.  The record keeps every cause; the checkpoint's code (we_ai_ws.c) asks
# it, and nothing else says a path is new, removed or moved.  This script keeps
# the code from going around the owner.
#
#   check-checkpoint-record.sh [--srcdir DIR] [--allow FILE] [--nm NM] [--selftest] [OBJECT...]
#
# Clauses (the source half reads we_ai*.c and we_ai*.h at the top of DIR):
#
#  a  Removing or renaming.  In we_ai_ws.c, we_ai_ckrec.c and the we_ai headers:
#     unlink, unlinkat, remove, rmdir, rename, renameat, renameat2, system and
#     popen are not named (a call, a function pointer, or a #define alias), and
#     no string holds a shell removal (rm, mv, find -delete, git clean/rm/mv/
#     reset/restore, xargs, shred, rsync) or names a git verb outside the set of
#     reads and the revert's own (ls-files, ls-tree, show, checkout, diff, stash,
#     add, commit, rev-parse, check-ignore, update-index).  Adjacent string
#     literals are one string.  A function of another we_ai*.c file that does
#     one of these, and every function that calls it (the closure), is named
#     when we_ai_ws.c or we_ai_ckrec.c calls it.  What a later pass still moves
#     is in tests/checkpoint-record.allow (file|clause:function|count|pass|reason;
#     a count that is exceeded, or a line that is no longer needed, fails).
#  b  Moving by the names owner.  Any wpe_fileop_ name in the same files (the
#     calls the revert actions will make are listed per function in the allow
#     file when they land).
#  c  No list of names of its own.  The token is_new; the names ck_before, ck_met,
#     ck_listed, ck_unwatched, ck_untracked, ck_ignored, ck_files; a file-scope
#     "char **" in we_ai_ws.c other than the four it holds; a struct ws_change
#     with other members than its four; the member kind written anywhere but
#     ws_add_change.
#  d  One place a row is made.  ws_add_change is called only by ws_rows_of_verdict,
#     ws_rows_of_edit, ws_changes_from_gitdiff and ws_row_of_unread_git_edit, and
#     is never named without a call or aliased by a #define.
#  e  One walk.  In we_ai_ws.c: readdir, opendir, fdopendir, scandir, nftw, ftw,
#     fts_open, glob, wpe_read_folder, wpe_read_entry_look and wpe_read_entry_kind
#     are named only by the functions the allow file lists (the closure of a
#     helper in another file applies as in a).
#  f  The words are the owner's.  The row words, the pane sentences and the last
#     lines of a revert are string literals only of we_ai_ckrec.c (checked in
#     we_ai_ws.c and the we_ai headers).
#  g  No result of the owner thrown away: no (void) cast of a wpe_ckrec_ call
#     in any we_ai*.c.
#  h  A state with no words or no test.  Every enumerator of WpeCkVerdictKind has
#     a case in wpe_ckrec_row_text, wpe_ckrec_sentence and wpe_ckrec_kind_is_kept;
#     every enumerator of WpeCkWhy has one in ck_why_words, ck_why_word and
#     wpe_ckrec_note_sentence; none of the six has a default:; every enumerator
#     is named in tests/test_ai_ckrec.c (WPE_CK_WHY_NONE excepted: it is "reached");
#     we_ai_ckrec.c compiles with -Wswitch-enum.
#  i  Identity is not decided here.  In we_ai_ckrec.c a comparison of two strings
#     (strcmp and its kin) is named only by the functions that order and look up
#     paths, and wpe_seen_size only by ck_maybe_copy.
#  o  Object half (read with $(NM) through tests/objsyms.sh): we_ai_ckrec.o imports
#     none of the removing or renaming calls; we_ai_ws.o imports them only where
#     the allow file lists a use.
#
# Comments and string literals are blanked first (tests/blank-c.awk); a call
# split over lines is read as one; a function with its body on one line, or in
# K&R form, is a function; a #define whose body holds code is refused.  A clause
# that names a function the file does not define fails: the scan is blind.
#
# --selftest runs only the canaries: a clean fixture that must be quiet, and one
# fixture per clause and per bypass above that must be named.  Without it the
# script scans the sources and then runs the canaries.  With no nm or no object
# list the object half is not run: SKIP, exit 77, never 0.
#
# Environment (set by `make check`): NM, ONE_OWNER_OBJECTS, CC, srcdir.
# Exit: 0 clean, 1 offenders or a blind scanner, 77 a half that needs nm.

SRCDIR=${srcdir:-.}
BAD=1
SKIP=77
ALLOW=
NMTOOL=${NM:-nm}
OBJECTS=
SELFTEST_ONLY=
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/objsyms.sh"
. "$HERE/compiler.sh"

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --allow) shift; ALLOW=$1 ;;
    --nm) shift; NMTOOL=$1 ;;
    --selftest) SELFTEST_ONLY=1 ;;
    -*) echo "check-checkpoint-record: unknown argument $1" >&2; exit $BAD ;;
    *) OBJECTS="$OBJECTS $1" ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/checkpoint-record.allow
[ -n "$OBJECTS" ] || OBJECTS=$ONE_OWNER_OBJECTS

. "$HERE/workdir.sh"
work_dir checkpoint-record $BAD
: > "$WORK/problems"

problem() { echo "$*" >> "$WORK/problems"; }

BLANK_C=$(cat "$HERE/blank-c.awk")

# The names each clause looks for, in the one place that holds them.
A_NAMES="unlink unlinkat remove rmdir rename renameat renameat2 system popen syscall"
X_NAMES="execl execlp execle execv execvp execvpe execve fexecve posix_spawn posix_spawnp"
KIND_CALLERS="ck_add_entry ck_walk_entry ck_folder_now"
WORDS_LAST="ALL changes reverted|no files changed|kept the change|not seen (the pane|removed by the run|moved by the run|cannot say what the run did"
E_NAMES="readdir opendir fdopendir scandir nftw ftw fts_open glob wpe_read_folder wpe_read_entry_look wpe_read_entry_kind"
C_NAMES="is_new ck_before ck_met ck_listed ck_unwatched ck_untracked ck_ignored ck_files"
GIT_VERBS="ls-files ls-tree show checkout diff stash add commit rev-parse check-ignore update-index"
ROW_CALLERS="ws_rows_of_verdict ws_rows_of_edit ws_changes_from_gitdiff ws_row_of_unread_git_edit ws_add_change"
CMP_CALLERS="ck_under_replaced_root ck_root_still_the_one ck_locate ck_known_object ck_walk_dir ck_is_skipped ck_add_tool_parents ck_verdict_cmp wpe_ckrec_verdict_of"
CHAR_PP_ALLOWED="ck_dirs ck_mode_paths ck_baks ws_sentences ws_session_grants"
WORDS="[AI] new file|[AI] removed|[AI] moved|[AI] second name|[AI] new, kept|[AI] replaced|[AI] not readable|not watched:|not seen after|is unseen|the snapshot watches|removed by the run|moved by the run|ALL changes reverted|no files changed|not seen (the pane|kept the change|is new, and it is as large|cannot say what the run did|the review saw|git could not list"
WS_STRUCT='typedef struct { char *path; int line; char *text; WpeCkVerdictKind kind; } ws_change;'

# ---- the scanner ------------------------------------------------------------
#
# One awk program per file.  It prints HIT lines (clause|file|function|what),
# DIR lines (the functions that name a removing, renaming or walking call, for
# the closure) and EDGE lines (who calls whom).  The program reads the file
# into code[] (literals blanked) and lit[] (literals kept), cuts it into
# functions at the column-0 headers, and walks the identifiers of each
# function's text joined into one string, so a call split over lines or a
# whole body on one line reads the same.
SCAN_AWK='
function header_name(line,   head, parts, k) {
  head = line
  sub(/\(.*/, "", head)
  sub(/[^A-Za-z0-9_]+$/, "", head)
  k = split(head, parts, /[^A-Za-z0-9_]+/)
  return parts[k]
}
function is_header(line) {
  return (line ~ /^[A-Za-z_]/ && line ~ /\(/ && line !~ /;[ \t]*$/ &&
          line !~ /^(typedef|struct|union|enum|extern)[ \t]/)
}
function hit(clause, fn, what) {
  printf "HIT|%s|%s|%s|%s\n", clause, base_name, fn, what
}
function listed(name, list,   n, parts, i) {
  n = split(list, parts, " ")
  for (i = 1; i <= n; i++) if (parts[i] == name) return 1
  return 0
}
# The sets, split once: A_SET[name] for every name of the list.
function fill_set(set, list,   n, parts, i) {
  n = split(list, parts, " ")
  for (i = 1; i <= n; i++) set[parts[i]] = 1
}
# Does the function FN have a body?  In a header a column-0 line may be a
# prototype that runs over several lines: a ";" before the first "{" says so.
function has_body(fn,   t, pb, ps) {
  if (!is_hdr) return 1
  t = ftext[fn]
  pb = index(t, "{"); ps = index(t, ";")
  return pb > 0 && (ps == 0 || pb < ps)
}
# One identifier TOK of function FN, a "call" when "(" follows, else "bare".
# A prototype gives no edge and no direct use (the closure reads functions
# that have a body).
function name_seen(fn, tok, kind,   body) {
  body = has_body(fn)
  if ((tok in A_SET) || (tok in X_SET)) prims[fn]++
  if (kind == "call" && body && !((fn SUBSEP tok) in edge_done)) {
    edge_done[fn SUBSEP tok] = 1
    printf "EDGE|%s|%s|%s\n", base_name, fn, tok
  }
  if (tok in A_SET) {
    if (body) printf "DIR|a|%s|%s|%s\n", base_name, fn, tok
    if (scope_ab) hit("a", fn, kind " of " tok)
  }
  if (substr(tok, 1, 11) == "wpe_fileop_") {
    if (body) printf "DIR|b|%s|%s|%s\n", base_name, fn, tok
    if (scope_ab) hit("b", fn, kind " of " tok)
  }
  if (tok in E_SET) {
    if ((tok in EL_SET) && body) printf "DIR|e|%s|%s|%s\n", base_name, fn, tok
    if (is_ws) hit("e", fn, kind " of " tok)
  }
  if (tok in C_SET) hit("c", fn, "the name " tok)
  if (tok == "ws_add_change" && !(fn in RC_SET))
    hit("d", fn, kind " of ws_add_change outside the places a row is made")
  if (is_ck && (tok in CMP_SET) && !(fn in CC_SET))
    hit("i", fn, kind " of " tok " outside the functions that order and look up paths")
  if (is_ck && tok == "wpe_seen_size" && fn != "ck_maybe_copy")
    hit("i", fn, "wpe_seen_size outside ck_maybe_copy")
  if (is_ck && tok == "wpe_seen_nlink")
    hit("i", fn, "wpe_seen_nlink: identity is not decided from a link count")
  if (is_ck && tok == "wpe_seen_kind" && !(fn in KC_SET))
    hit("i", fn, "wpe_seen_kind outside the functions that make an entry")
  if (is_ck_or_ws && (tok in X_SET)) hit("a", fn, kind " of " tok)
  if (is_ai && body && (tok in X_SET)) printf "DIR|a|%s|%s|%s\n", base_name, fn, tok
}
function walk_names(fn, text,   rest, tok, after) {
  rest = text
  while (match(rest, /[A-Za-z_][A-Za-z0-9_]*/)) {
    tok = substr(rest, RSTART, RLENGTH)
    after = substr(rest, RSTART + RLENGTH)
    rest = after
    name_seen(fn, tok, (after ~ /^[ \t]*\(/) ? "call" : "bare")
  }
}
# The strings of one function: shell words and git verbs (a), the words the
# owner spells (f).
function check_literals(fn, text,   t, rest, verb, parts, n, i) {
  t = text
  gsub(/"[ \t]*"/, "", t)                     # adjacent literals are one
  if (scope_ab) {
    if (t ~ /(^|[^A-Za-z0-9_])rm[ \t]/ || t ~ /(^|[^A-Za-z0-9_])mv[ \t]/ ||
        t ~ /find[^"]*-delete/ || t ~ /(^|[^A-Za-z0-9_])xargs[ \t]/ ||
        t ~ /(^|[^A-Za-z0-9_])(shred|rsync)[ \t]/ || t ~ /"(rm|mv|rmdir|unlink|shred)"/ ||
        t ~ /git[ \t]+(--literal-pathspecs[ \t]+)?(clean|rm|mv|reset|restore|rebase|prune|gc)([^A-Za-z-]|$)/)
      hit("a", fn, "a shell command that removes or moves")
  }
  if (is_ws) {
    if (t ~ /("|&&|;|\|)[ \t]*(find|ls|du|tree)[ \t]/)
      hit("e", fn, "a shell walk through the command runner")
    rest = t
    while (match(rest, /("|&&|;|\|)[ \t]*git[ \t]+(--literal-pathspecs[ \t]+)?[a-z%][a-z-]*/)) {
      verb = substr(rest, RSTART, RLENGTH)
      rest = substr(rest, RSTART + RLENGTH)
      n = split(verb, parts, /[ \t]+/)
      verb = parts[n]
      if (verb !~ /^%/ && !listed(verb, git_verbs)) hit("a", fn, "the git verb " verb " is not one of the allowed set")
    }
    rest = t
    while (match(rest, /ws_git_list[ \t]*\([ \t]*"[a-z-]+/)) {
      verb = substr(rest, RSTART, RLENGTH)
      rest = substr(rest, RSTART + RLENGTH)
      sub(/^.*"/, "", verb)
      if (!listed(verb, git_verbs)) hit("a", fn, "ws_git_list asks git for " verb)
    }
  }
  if (is_ai && !is_ws && !is_hdr && !is_ck) {
    n = split(words_last, parts, "|")
    for (i = 1; i <= n; i++)
      if (index(t, parts[i]) > 0) hit("f", fn, "the words " parts[i] " are the owner" "\047" "s")
  }
  if (is_ws || is_hdr) {
    n = split(words, parts, "|")
    for (i = 1; i <= n; i++)
      if (index(t, parts[i]) > 0) hit("f", fn, "the words " parts[i] " are the owner" "\047" "s")
  }
}
# A #define: a macro with code, or one that names a call this clause forbids.
function check_define(body, lineno,   probe, name, rest, tok, after) {
  name = body
  sub(/^[ \t]*#[ \t]*define[ \t]+/, "", name)
  sub(/[^A-Za-z0-9_].*/, "", name)
  probe = body
  sub(/^[ \t]*#[ \t]*define[ \t]+[A-Za-z0-9_]+/, "", probe)
  sub(/^\([^)]*\)/, "", probe)
  gsub(/sizeof[ \t]*\(/, "(", probe)
  gsub(/__attribute__[ \t]*\(\([^)]*\)\)/, "", probe)
  if (!scope_ab) return
  if (probe ~ /[;{}]/ || probe ~ /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)
    hit("a", "#define", "#define " name " holds code: the scan cannot expand a macro")
  rest = probe
  while (match(rest, /[A-Za-z_][A-Za-z0-9_]*/)) {
    tok = substr(rest, RSTART, RLENGTH)
    rest = substr(rest, RSTART + RLENGTH)
    if (tok in A_SET) hit("a", "#define", "#define " name " names " tok)
    if (substr(tok, 1, 11) == "wpe_fileop_") hit("b", "#define", "#define " name " names " tok)
    if (is_ws && (tok in E_SET)) hit("e", "#define", "#define " name " names " tok)
    if (tok == "ws_add_change") hit("d", "#define", "#define " name " names ws_add_change")
  }
}
BEGIN {
  blank_incom = 0; total = 0
  fill_set(A_SET, a_names); fill_set(E_SET, e_names); fill_set(C_SET, c_names)
  fill_set(RC_SET, row_callers); fill_set(CC_SET, cmp_callers)
  fill_set(CMP_SET, "strcmp strncmp memcmp strcasecmp strncasecmp bcmp")
  fill_set(X_SET, x_names); fill_set(KC_SET, kind_callers)
  is_ck_or_ws = (is_ws + 0) || (is_ck + 0) || (is_hdr + 0)
  fill_set(EL_SET,"readdir opendir fdopendir scandir nftw ftw fts_open glob wpe_read_folder")
}
{
  total++
  was = blank_incom
  lit[total] = blank_c($0, 1)
  now = blank_incom
  blank_incom = was
  code[total] = blank_c($0, 2)
  blank_incom = now
}
END {
  fname = "-"
  for (i = 1; i <= total; i++) {
    line = code[i]
    if (line ~ /^[ \t]*#[ \t]*define[ \t]/) {
      body = line
      lbody = lit[i]
      while (body ~ /\\[ \t]*$/ && i < total) {
        sub(/\\[ \t]*$/, "", body)
        i++
        body = body " " code[i]
        lbody = lbody " " lit[i]
      }
      check_define(body, i)
      check_literals("#define", lbody)
      continue
    }
    if (line ~ /^[ \t]*#/) continue
    if (is_header(line)) { fname = header_name(line); defined[fname] = 1 }
    ftext[fname] = ftext[fname] " " line
    flit[fname] = flit[fname] " " lit[i]
    if (is_ws && line ~ /^(static[ \t]+)?char[ \t]*\*[ \t]*\*[ \t]*[A-Za-z_]/ && line !~ /\(/) {
      nm = line
      sub(/^(static[ \t]+)?char[ \t]*\*[ \t]*\*[ \t]*/, "", nm)
      sub(/[^A-Za-z0-9_].*/, "", nm)
      if (!listed(nm, char_pp)) hit("c", "-", "a list of names of its own: char **" nm)
    }
    if (is_ws && line ~ /^(static[ \t]+)?char[ \t]*\*[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*\[/ && line !~ /\(/) {
      nm = line
      sub(/^(static[ \t]+)?char[ \t]*\*[ \t]*/, "", nm)
      sub(/[^A-Za-z0-9_].*/, "", nm)
      if (!listed(nm, char_pp)) hit("c", "-", "a list of names of its own: an array of char pointers " nm)
    }
    if (is_ws && line ~ /typedef[ \t]+struct/ && line ~ /ws_change[ \t]*;/) {
      norm = line
      gsub(/[ \t]+/, " ", norm)
      sub(/^ /, "", norm)
      sub(/ $/, "", norm)
      if (norm != ws_struct) hit("c", "-", "struct ws_change has members other than its four")
    }
  }
  for (fn in ftext) {
    walk_names(fn, ftext[fn])
    check_literals(fn, flit[fn])
    if (is_ws && fn != "ws_add_change" && ftext[fn] ~ /(\.|->)[ \t]*kind[ \t]*=[^=]/)
      hit("c", fn, "the member kind is written outside ws_add_change")
    if (is_ck && !(fn in CC_SET) &&
        (ftext[fn] ~ /\*[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*(==|!=)[ \t]*\*[ \t]*[A-Za-z_]/ ||
         ftext[fn] ~ /[A-Za-z_][A-Za-z0-9_]*[ \t]*\[[^]]*\][ \t]*(==|!=)[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*\[/))
      hit("i", fn, "compares characters of two paths outside the functions that order and look up paths")
    if (is_any_c && ftext[fn] ~ /\([ \t]*void[ \t]*\)[ \t]*wpe_ckrec_/)
      hit("g", fn, "a result of the owner is thrown away with a (void) cast")
  }
  if (is_ws) {
    split(need_ws, need, " ")
    for (k in need) if (!(need[k] in defined)) printf "BLIND|%s|%s is not defined: the clause is blind\n", base_name, need[k]
  }
  for (fn in defined)
    if (has_body(fn)) {
      printf "DEF|%s|%s\n", base_name, fn
      printf "PRIMS|%s|%s|%d\n", base_name, fn, prims[fn] + 0
    }
}
'

# scan_file FILE - the scanner on one file, with the sets of names the clauses use.
scan_file() {
  path=$1
  file=$(basename "$path")
  case $file in
    we_ai_ws.c) is_ws=1; is_ck=0; ab=1 ;;
    we_ai_ckrec.c) is_ws=0; is_ck=1; ab=1 ;;
    *.h) is_ws=0; is_ck=0; ab=1 ;;
    *) is_ws=0; is_ck=0; ab=0 ;;
  esac
  is_hdr=0; is_ai=0
  case $file in *.h) is_hdr=1 ;; esac
  case $file in we_ai*) is_ai=1 ;; esac
  awk -v base_name="$file" -v is_ws=$is_ws -v is_ck=$is_ck -v scope_ab=$ab -v is_hdr=$is_hdr \
      -v is_ai=$is_ai -v x_names="$X_NAMES" -v kind_callers="$KIND_CALLERS" -v words_last="$WORDS_LAST" \
      -v is_any_c=$is_ai \
      -v a_names="$A_NAMES" -v e_names="$E_NAMES" -v c_names="$C_NAMES" \
      -v git_verbs="$GIT_VERBS" -v row_callers="$ROW_CALLERS" -v cmp_callers="$CMP_CALLERS" \
      -v char_pp="$CHAR_PP_ALLOWED" -v words="$WORDS" -v ws_struct="$WS_STRUCT" \
      -v need_ws="ws_add_change ws_rows_of_verdict ws_rows_of_edit ws_changes_from_gitdiff ws_row_of_unread_git_edit" \
      "$BLANK_C$SCAN_AWK" "$path"
}

# The closure of the removing, renaming and walking calls over the functions of
# the we_ai files: a function of another file that does one of them, and every
# function that calls such a function, is named where we_ai_ws.c or
# we_ai_ckrec.c calls it.  Reads the DIR and EDGE lines of every file.
CLOSURE_AWK='
BEGIN {
  FS = "|"
  while ((getline line < leaf_file) > 0) {
    if (line ~ /^#/ || line ~ /^[ \t]*$/) continue
    n = split(line, f, "|")
    if (n != 5) continue
    split(f[2], cf, ":")
    if (cf[1] == "leaf") { leaf[cf[2]] = 1; leaf_in[cf[2]] = f[1]; leaf_n[cf[2]] = f[3] + 0; leaf_row[cf[2]] = line }
  }
}
$1 == "PRIMS" { prim_n[$2 SUBSEP $3] = $4 + 0 }
$1 == "DIR" { reach[$2 SUBSEP $4] = 1; dirfile[$2 SUBSEP $4] = $3; sets[$2] = 1 }
$1 == "EDGE" { nedge++; efile[nedge] = $2; ecaller[nedge] = $3; ecallee[nedge] = $4; defined_in[$3] = $2 }
$1 == "DEF" { defined_in[$3] = $2 }
END {
  # A leaf row names the file that defines the function, and its count is how
  # many removing or exec calls the function names directly: exact, and it may
  # only fall.  A leaf must not hide a removal added to its own body.
  for (lf in leaf) {
    if (!(lf in defined_in)) { printf "PROBLEM|leaf %s: no scanned file defines it: %s\n", lf, leaf_row[lf]; continue }
    if (defined_in[lf] != leaf_in[lf]) {
      printf "PROBLEM|leaf %s: the row names %s but it is defined in %s: %s\n", lf, leaf_in[lf], defined_in[lf], leaf_row[lf]
      continue
    }
    got = prim_n[defined_in[lf] SUBSEP lf] + 0
    if (got > leaf_n[lf])
      printf "PROBLEM|leaf %s names %d removing or exec call(s) in its own body, its row allows %d\n", lf, got, leaf_n[lf]
    else if (got < leaf_n[lf])
      printf "PROBLEM|stale allow line (the leaf names %d such call(s) now): %s\n", got, leaf_row[lf]
  }
  do {
    changed = 0
    for (e = 1; e <= nedge; e++)
      for (s in sets)
        if (((s SUBSEP ecallee[e]) in reach) && (ecallee[e] in defined_in) && !((s SUBSEP ecaller[e]) in reach) &&
            !(ecallee[e] in leaf)) {
          reach[s SUBSEP ecaller[e]] = 1; changed = 1
        }
  } while (changed)
  for (e = 1; e <= nedge; e++) {
    if (efile[e] != "we_ai_ws.c" && efile[e] != "we_ai_ckrec.c") continue
    callee = ecallee[e]
    if (!(callee in defined_in) || defined_in[callee] == efile[e]) continue
    if (defined_in[callee] == "we_ai_ckrec.c" || defined_in[callee] == "we_read.c") continue   # the owners may walk
    for (s in sets)
      if ((s SUBSEP callee) in reach && (callee in leaf)) {
        if (!(callee in leaf_used)) { leaf_used[callee] = 1; printf "LEAF|%s\n", callee }
      }
      else if ((s SUBSEP callee) in reach && !((efile[e] "|" ecaller[e] "|" s "|" callee) in done)) {
        done[efile[e] "|" ecaller[e] "|" s "|" callee] = 1
        printf "HIT|%s|%s|%s|closure: calls %s (%s), which removes, renames or walks\n", s, efile[e], ecaller[e], callee, defined_in[callee]
      }
  }
}
'

# reduce_hits ALLOW - the HIT lines on stdin against the allow file: a hit with
# no line, or more hits than a line allows, is an offender; a line that allows
# more than there are hits (or a function nobody names) is stale.  Prints the
# offences.
reduce_hits() {
  allow=$1
  awk -F'|' -v allow_file="$allow" '
    BEGIN {
      while ((getline line < allow_file) > 0) {
        if (line ~ /^#/ || line ~ /^[ \t]*$/) continue
        n = split(line, f, "|")
        if (n != 5) { printf "check-checkpoint-record: allow line is not file|clause:function|count|pass|reason: %s\n", line; continue }
        split(f[2], cf, ":")
        if (cf[1] == "leaf") continue
        key = cf[1] "|" f[1] "|" cf[2]
        allowed[key] = f[3] + 0
        allow_line[key] = line
      }
    }
    $1 == "HIT" {
      key = $2 "|" $3 "|" $4
      count[key]++
      what[key, count[key]] = $5
    }
    END {
      for (key in count) {
        a = (key in allowed) ? allowed[key] : 0
        if (count[key] > a) {
          split(key, kp, "|")
          for (i = a + 1; i <= count[key]; i++)
            printf "%s: %s: %s: %s\n", toupper(kp[1]), kp[2], kp[3], what[key, i]
        }
        else if (count[key] < a) printf "check-checkpoint-record: stale allow line (the count fell to %d): %s\n", count[key], allow_line[key]
      }
      for (key in allowed)
        if (!(key in count)) printf "check-checkpoint-record: stale allow line (nothing is named any more): %s\n", allow_line[key]
    }'
}

# stale_leaves ALLOW USED - a leaf the allow file declares that the checkpoint's
# code no longer calls (or that no longer reaches a removing call) is stale.
stale_leaves() {
  awk -F'|' -v used_file="$2" '
    BEGIN { while ((getline u < used_file) > 0) used[u] = 1 }
    /^#/ || /^[ \t]*$/ { next }
    { split($2, cf, ":")
      if (cf[1] == "leaf" && !(cf[2] in used))
        printf "check-checkpoint-record: stale leaf (no call of it from the checkpoint reaches a removing call): %s\n", $0 }' "$1"
}

# src_scan DIR TESTC ALLOW - the source half on a folder; prints the offences.
src_scan() {
  dir=$1; testc=$2; allow=$3
  : > "$WORK/scan.all"
  for path in "$dir"/we_ai*.c "$dir"/we_ai*.h; do
    [ -f "$path" ] || continue
    scan_file "$path" >> "$WORK/scan.all"
  done
  # every other product source takes part in the closure only: its functions
  # are the helpers a call from the checkpoint may go through
  for path in "$dir"/*.c; do
    [ -f "$path" ] || continue
    case $(basename "$path") in we_ai*) continue ;; esac
    scan_file "$path" | grep -E '^(EDGE|DIR|DEF|PRIMS)\|' >> "$WORK/scan.all"
  done
  for need in we_ai_ws.c we_ai_ckrec.c we_ai_ckrec.h; do
    [ -f "$dir/$need" ] || echo "check-checkpoint-record: $dir/$need is not there: the scan is blind"
  done
  grep '^BLIND|' "$WORK/scan.all" | sed 's/^BLIND|//; s/|/: /'
  grep '^HIT|' "$WORK/scan.all" > "$WORK/scan.hits"
  awk -v leaf_file="$allow" "$CLOSURE_AWK" "$WORK/scan.all" > "$WORK/scan.closure"
  grep '^HIT|' "$WORK/scan.closure" >> "$WORK/scan.hits"
  grep '^LEAF|' "$WORK/scan.closure" | cut -d'|' -f2 > "$WORK/scan.leaves"
  grep '^PROBLEM|' "$WORK/scan.closure" | cut -d'|' -f2- | awk '{ print "check-checkpoint-record: " $0 }'
  reduce_hits "$allow" < "$WORK/scan.hits" | sort
  stale_leaves "$allow" "$WORK/scan.leaves"
  enum_scan "$dir" "$testc"
}

# ---- clause h: every state has words and a test -----------------------------

# enum_names HEADER ENUMTYPE - the enumerators of one enum of the owner's header.
enum_names() {
  awk "$BLANK_C"'
    { line = blank_c($0, 2) }
    line ~ /typedef[ \t]+enum[ \t]*\{/ { inside = 1; list = ""; sub(/^.*\{/, "", line) }
    inside {
      rest = line
      while (match(rest, /WPE_CK_[A-Z_]+/)) { list = list " " substr(rest, RSTART, RLENGTH); rest = substr(rest, RSTART + RLENGTH) }
      if (line ~ /\}/) {
        name = line; sub(/^.*\}[ \t]*/, "", name); sub(/[ \t]*;.*$/, "", name)
        if (name == want) all = all list
        inside = 0
      }
    }
    END { print all }' want="$2" "$1"
}

# fn_body FILE NAME - the text of one function, from its header to the closing
# brace at column 0, comments and literals blanked.
fn_body() {
  awk "$BLANK_C"'
    { line = blank_c($0, 2) }
    line ~ /^[A-Za-z_]/ && line ~ /\(/ && line !~ /;[ \t]*$/ {
      head = line; sub(/\(.*/, "", head); sub(/[^A-Za-z0-9_]+$/, "", head); k = split(head, p, /[^A-Za-z0-9_]+/)
      infn = (p[k] == want)
    }
    infn { print line }
    infn && line ~ /^\}/ { infn = 0 }' want="$2" "$1"
}

# switch_check CFILE ENUM_LIST FUNCTION... - every enumerator has a case in each
# function, and no function has a default.
switch_check() {
  cfile=$1; enums=$2; shift 2
  for fn in "$@"; do
    fn_body "$cfile" "$fn" > "$WORK/body.txt"
    if [ ! -s "$WORK/body.txt" ]; then
      echo "H: $(basename "$cfile"): $fn is not defined: the clause is blind"
      continue
    fi
    for en in $enums; do
      tr '\n' ' ' < "$WORK/body.txt" | grep -q "case[[:space:]][[:space:]]*$en[[:space:]]*:" ||
        echo "H: $(basename "$cfile"): $fn has no case for $en"
    done
    tr '\n' ' ' < "$WORK/body.txt" | grep -q "default[[:space:]]*:" &&
      echo "H: $(basename "$cfile"): $fn has a default: an enumerator added later would pass unseen"
  done
}

enum_scan() {
  dir=$1; testc=$2
  [ -f "$dir/we_ai_ckrec.h" ] && [ -f "$dir/we_ai_ckrec.c" ] || return 0
  kinds=$(enum_names "$dir/we_ai_ckrec.h" WpeCkVerdictKind)
  whys=$(enum_names "$dir/we_ai_ckrec.h" WpeCkWhy)
  [ -n "$kinds" ] && [ -n "$whys" ] || { echo "H: we_ai_ckrec.h: WpeCkVerdictKind or WpeCkWhy not found: the clause is blind"; return 0; }
  switch_check "$dir/we_ai_ckrec.c" "$kinds" wpe_ckrec_row_text wpe_ckrec_sentence wpe_ckrec_kind_is_kept wpe_ckrec_kind_is_gone
  switch_check "$dir/we_ai_ckrec.c" "$whys" ck_why_words ck_why_word wpe_ckrec_note_sentence
  if [ ! -f "$testc" ]; then
    echo "H: $testc is not there: no state has a test"
    return 0
  fi
  awk "$BLANK_C"'{ print blank_c($0, 2) }' "$testc" > "$WORK/test.code"
  for en in $kinds $whys; do
    [ "$en" = WPE_CK_WHY_NONE ] && continue
    grep -q -w "$en" "$WORK/test.code" || echo "H: $(basename "$testc"): $en is not in a row: a state with no test"
  done
}

# ---- the object half ---------------------------------------------------------

object_imports() { "$NMTOOL" -u "$1" 2>/dev/null | nm_undefined_source_names; }

# a_object_scan OBJECT... - who imports a removing or renaming call, read from
# the objects.  The owner's own canaries are seen or reported, never passed over.
a_object_scan() {
  seen_ck=0
  for obj in "$@"; do
    base=$(basename "$obj")
    base=${base#we-}
    if [ ! -f "$obj" ]; then echo "O: object $obj not found (build before the check)"; continue; fi
    object_imports "$obj" > "$WORK/obj.imports"
    case $base in
      we_ai_ckrec.o)
        seen_ck=1
        grep -qx readdir "$WORK/obj.imports" ||
          echo "O: canary: readdir not seen in we_ai_ckrec.o (LTO objects, wrong nm, or an empty object list)"
        for sym in $A_NAMES; do
          grep -qx "$sym" "$WORK/obj.imports" && echo "O: we_ai_ckrec.o imports $sym: the record removes and renames nothing"
        done ;;
      we_ai_ws.o)
        grep -qx mkstemp "$WORK/obj.imports" ||
          echo "O: canary: mkstemp not seen in we_ai_ws.o (LTO objects, wrong nm, or an empty object list)"
        for sym in $A_NAMES; do
          grep -qx "$sym" "$WORK/obj.imports" || continue
          [ "$sym" = unlink ] && grep -q "^we_ai_ws.c|a:[A-Za-z0-9_]*|[1-9]" "$ALLOW" 2> /dev/null && continue
          echo "O: we_ai_ws.o imports $sym and the allow file lists no use of it"
        done ;;
    esac
  done
  [ $seen_ck -eq 1 ] || echo "O: canary: we_ai_ckrec.o is not among the objects (an empty object list)"
}

SKIPPED=
real_objects() {
  if [ -z "$OBJECTS" ]; then
    SKIPPED="$SKIPPED the object half of the real tree not run (no object list: ONE_OWNER_OBJECTS or arguments);"
  elif [ "$NMTOOL" = ":" ] || ! command -v "$NMTOOL" > /dev/null 2>&1; then
    SKIPPED="$SKIPPED the object half of the real tree not run (no nm: $NMTOOL);"
  else
    a_object_scan $OBJECTS
  fi
}

# ---- the fixtures of the canaries ---------------------------------------------
#
# A clean tree holds the functions the clauses read, in their right shape, with
# marker lines (a comment that names the place).  A canary replaces a marker by
# an offender and the scan must name it, in a message that holds a word.

emit_ws_clean() {
  cat <<'EOF'
#include "we_ai_ckrec.h"
/* quiet in a comment: "[AI] new file", ALL changes reverted, unlink( and rm -f, readdir( */
typedef struct { char *path; int line; char *text; WpeCkVerdictKind kind; } ws_change;
static char **ck_dirs;    static int ck_dirs_n;
static char **ck_mode_paths;
/*PLANT:fs*/
static void ws_add_change(ws_change **c, int *n, int *cap, const char *path, int line, const char *text, WpeCkVerdictKind kind)
{
 (*c)[*n].kind = kind;
}
static void ws_rows_of_verdict(ws_change **c, int *n, int *cap, const WpeCkVerdict *v)
{
 ws_add_change(c, n, cap,
               "p", 1, "t", v->kind);
}
static void ws_rows_of_edit(ws_change **c, int *n, int *cap)
{
 ws_add_change(c, n, cap, "p", 1, "t", WPE_CK_STAYED);
}
static void ws_changes_from_gitdiff(ws_change **c, int *n, int *cap)
{
 ws_add_change(c, n, cap, "p", 1, "t", WPE_CK_STAYED);
}
static void ws_row_of_unread_git_edit(ws_change **c, int *n, int *cap)
{
 ws_add_change(c, n, cap, "p", 1, "t", WPE_CK_STAYED);
}
int wpe_ai_scope_files(void)
{
 DIR *d = wpe_read_folder("x", 0, 0);
 struct dirent *de = readdir(d);
 return de != 0;
}
static int ws_take_away_snapshot(const char *p)
{
 return unlink(p);
}
static int ws_revert_command(char *cmd, int sz)
{
 return snprintf(cmd, sz, "cd %s && rm -f -- %s 2>&1", "a", "b");
}
static void ws_git_ok(void)
{
 run("cd %s && git --literal-pathspecs show %s", "x");
 ws_git_list("ls-files -z -- .");
 run("git %s", "x");
 wpe_write_leaf_ok("x");
 wpe_write_wrap("x");
}
static void ws_other(void)
{
 /*PLANT:body*/
}
/*PLANT:file*/
EOF
}

emit_other_clean() {
  cat <<'EOF'
static int other_clean(void) { return 0; }
/*PLANT:other*/
EOF
}

emit_product_clean() {
  cat <<'EOF'
static int product_clean(void) { return 0; }
void wpe_write_leaf_ok(const char *p)
{
 unlink(p);
 /*PLANT:leafbody*/
}
void wpe_write_wrap(const char *p) { wpe_write_leaf_ok(p); }
/*PLANT:nonai*/
EOF
}

emit_ck_clean() {
  cat <<'EOF'
static int ck_locate(const char *a, const char *b)
{
 return strcmp(a, b);
}
static int ck_maybe_copy(const WpeSeen *s)
{
 return (int)wpe_seen_size(s);
}
static int ck_other(void)
{
 /*PLANT:ck_body*/
 return 0;
}
const char *wpe_ckrec_row_text(const WpeCkVerdict *v)
{
 switch (v->kind) {
 case WPE_CK_STAYED: return 0;
 case WPE_CK_CREATED: return "a";
 case WPE_CK_MOVED: return "b";    /*PLANT:case_moved*/
 }
 /*PLANT:switch_tail*/
 return 0;
}
int wpe_ckrec_sentence(char *out, int n, const WpeCkVerdict *v)
{
 switch (v->kind) {
 case WPE_CK_STAYED: case WPE_CK_CREATED: return 0;
 case WPE_CK_MOVED: return 1;
 }
 return 0;
}
int wpe_ckrec_kind_is_kept(WpeCkVerdictKind kind)
{
 switch (kind) {
 case WPE_CK_MOVED: return 1;
 case WPE_CK_STAYED: case WPE_CK_CREATED: return 0;
 }
 return 0;
}
int wpe_ckrec_kind_is_gone(WpeCkVerdictKind kind)
{
 switch (kind) {
 case WPE_CK_MOVED: return 1;
 case WPE_CK_STAYED: case WPE_CK_CREATED: return 0;
 }
 return 0;
}
static const char *ck_why_words(WpeCkWhy why)
{
 switch (why) {
 case WPE_CK_WHY_NONE: return "n";
 case WPE_CK_WHY_BOUND: return "b";
 }
 return "n";
}
static const char *ck_why_word(WpeCkWhy why)
{
 switch (why) {
 case WPE_CK_WHY_NONE: return "n";
 case WPE_CK_WHY_BOUND: return "b";
 }
 return "n";
}
int wpe_ckrec_note_sentence(char *out, int n, const WpeCkNote *note)
{
 switch (note->why) {
 case WPE_CK_WHY_NONE: break;
 case WPE_CK_WHY_BOUND: return 1;
 }
 return 0;
}
EOF
}

emit_hdr_clean() {
  cat <<'EOF'
typedef enum {
 WPE_CK_STAYED,            /* a comment */
 WPE_CK_CREATED,
 WPE_CK_MOVED
} WpeCkVerdictKind;
typedef enum { WPE_CK_WHY_NONE, WPE_CK_WHY_BOUND } WpeCkWhy;
/*PLANT:hdr*/
EOF
}

emit_hdr2_clean() {
  cat <<'EOF'
static inline int words_clean(void) { return 0; }
/*PLANT:hdr2*/
EOF
}

emit_test_clean() {
  cat <<'EOF'
int kinds[] = { WPE_CK_STAYED, WPE_CK_CREATED, WPE_CK_WHY_BOUND };
int moved = WPE_CK_MOVED;    /*PLANT:tst_moved*/
EOF
}

emit_allow_clean() {
  cat <<'EOF'
# the fixture's allow file
we_ai_ws.c|e:wpe_ai_scope_files|2|keep|reads what the assistant may read
we_ai_ws.c|a:ws_take_away_snapshot|1|B|a created file is taken away
we_ai_ws.c|a:ws_revert_command|1|B|the git mode take-away
we_write.c|leaf:wpe_write_leaf_ok|1|keep|an owner entry point
# /*PLANT:allow*/
EOF
}

# plant_into DIR MARKER TEXT - DIR gets the clean fixtures, with the line that
# holds MARKER replaced by TEXT (\n is a line break; an empty TEXT deletes it).
plant_into() {
  dir=$1; marker=$2; text=$3
  rm -rf "$dir"; mkdir -p "$dir"
  for path in "$WORK/fx"/*; do
    file=$(basename "$path")
    awk -v m="/*PLANT:$marker*/" -v t="$text" '
      index($0, m) { if (t != "") print t; next }
      { print }' "$path" > "$dir/$file"
  done
}

# plant_more DIR MARKER TEXT - the same, in a folder already planted.
plant_more() {
  dir=$1; marker=$2; text=$3
  for path in "$dir"/*; do
    awk -v m="/*PLANT:$marker*/" -v t="$text" '
      index($0, m) { if (t != "") print t; next }
      { print }' "$path" > "$path.new" && mv "$path.new" "$path"
  done
}

canaries_run=0

# canary NAME MARKER TEXT WORD - the planted offender must be named, in a
# message that holds WORD.  canary2 plants a second marker first.
canary() {
  name=$1; marker=$2; text=$3; word=$4
  canaries_run=$((canaries_run + 1))
  plant_into "$WORK/fx-case" "$marker" "$text"
  case_scan "$name" "$word"
}
canary2() {
  name=$1; m1=$2; t1=$3; m2=$4; t2=$5; word=$6
  canaries_run=$((canaries_run + 1))
  plant_into "$WORK/fx-case" "$m1" "$t1"
  plant_more "$WORK/fx-case" "$m2" "$t2"
  case_scan "$name" "$word"
}
case_scan() {
  src_scan "$WORK/fx-case" "$WORK/fx-case/test_ai_ckrec.c" "$WORK/fx-case/fx.allow" > "$WORK/fx-case.out"
  grep -q -F "$2" "$WORK/fx-case.out" || problem "canary $1: not named with \"$2\" (the clause is blind)"
}

selftest() {
  mkdir -p "$WORK/fx"
  emit_ws_clean > "$WORK/fx/we_ai_ws.c"
  emit_other_clean > "$WORK/fx/we_ai_other.c"
  emit_product_clean > "$WORK/fx/we_write.c"
  emit_ck_clean > "$WORK/fx/we_ai_ckrec.c"
  emit_hdr_clean > "$WORK/fx/we_ai_ckrec.h"
  emit_hdr2_clean > "$WORK/fx/we_ai_words.h"
  emit_test_clean > "$WORK/fx/test_ai_ckrec.c"
  emit_allow_clean > "$WORK/fx/fx.allow"

  # The clean fixtures are quiet: the owner's own walk, an unlink with its
  # allow line, the words in a comment, a git verb of the allowed set.
  src_scan "$WORK/fx" "$WORK/fx/test_ai_ckrec.c" "$WORK/fx/fx.allow" > "$WORK/fx-clean.out"
  if [ -s "$WORK/fx-clean.out" ]; then
    problem "selftest: the clean fixtures are named (the clause is noisy):"
    cat "$WORK/fx-clean.out" >> "$WORK/problems"
  fi

  # a: the removing and renaming calls, and every way round them.
  canary a-call body ' unlink(p);' "call of unlink"
  canary a-remove body ' remove(p);' "call of remove"
  canary a-rename body ' rename(p, q);' "call of rename"
  canary a-rmdir body ' rmdir(p);' "call of rmdir"
  canary a-system body ' system(p);' "call of system"
  canary a-popen body ' popen(p, "r");' "call of popen"
  canary a-one-line fs 'static void one(char *p) { unlink(p); }' "unlink"
  canary a-knr fs 'static void kr_drop(p)\nchar *p;\n{ unlink(p); }' "unlink"
  canary a-split body ' unlink\n  (p);' "unlink"
  canary a-pointer fs 'static int (*kill_it)(const char *) = unlink;' "bare of unlink"
  canary a-define-alias fs '#define DROP unlink' "#define DROP names unlink"
  canary a-define-code fs '#define KILL(p) unlink(p)' "#define KILL holds code"
  canary a-over-allow body ' unlink(p);' "ws_other"
  canary a-adjacent-literals body ' run("r" "m -f -- %s", p);' "shell command that removes"
  canary a-adjacent-tab body ' run("cd x && r" "m\t-f %s", p);' "shell command that removes"
  canary a-find-delete body ' run("find . -delete");' "shell command that removes"
  canary a-git-clean body ' run("cd %s && git clean -f");' "shell command that removes"
  canary a-git-rm body ' run("git rm --cached x");' "shell command that removes"
  canary a-mv body ' run("cd d && mv a b");' "shell command that removes"
  canary a-git-verb body ' run("cd %s && git frobnicate x");' "git verb frobnicate"
  canary a-git-list-verb body ' ws_git_list("clean -f");' "asks git for clean"
  canary a-hidden-in-header hdr2 'static inline void hdr_drop(char *p) { unlink(p); }' "unlink"
  canary2 a-helper-elsewhere other 'static void ai_drop(const char *p) { unlink(p); }' body ' ai_drop("x");' "closure: calls ai_drop"
  canary2 a-helper-two-deep other 'static void ai_drop(const char *p) { unlink(p); }\nstatic void ai_wrap(const char *p) { ai_drop(p); }' body ' ai_wrap("x");' "closure: calls ai_wrap"
  canary2 a-helper-rename other 'static void ai_move(const char *p) { rename(p, p); }' body ' ai_move("x");' "closure: calls ai_move"

  # a, round two: a helper in a product file that is not a we_ai file, a raw
  # system call, the exec family, a lone program name.
  canary2 a-helper-product-file nonai 'static void wpe_write_copy_remove(const char *p) { unlink(p); }' body ' wpe_write_copy_remove("x");' "closure: calls wpe_write_copy_remove"
  canary2 a-exec-helper-in-ai-file other 'static void wpe_ai_plant_exec(void) { execlp("rm", "rm", "-f", "x", 0); }' body ' wpe_ai_plant_exec();' "closure: calls wpe_ai_plant_exec"
  canary2 a-depth-two-new-file nonai 'static void drop_it(const char *p) { unlink(p); }\nvoid wpe_new_public(const char *p) { drop_it(p); }' body ' wpe_new_public("x");' "closure: calls wpe_new_public"
  canary2 a-depth-three-new-file nonai 'static void drop_it(const char *p) { unlink(p); }\nstatic void mid_it(const char *p) { drop_it(p); }\nvoid wpe_new_top(const char *p) { mid_it(p); }' body ' wpe_new_top("x");' "closure: calls wpe_new_top"
  canary a-leaf-hides-a-removal leafbody ' if (p[0] == 0x2f) unlink(p);' "names 2 removing or exec call"
  canary a-leaf-hides-an-exec leafbody ' execlp("rm", "rm", "-f", p, 0);' "names 2 removing or exec call"
  canary2 a-leaf-file-mismatch nonai 'void wpe_plant_two_deep(const char *p) { unlink(p); }' allow 'we_fileop.c|leaf:wpe_plant_two_deep|1|keep|x' "defined in we_write.c"
  canary a-leaf-no-such-function allow 'we_write.c|leaf:wpe_plant_nowhere|0|keep|x' "no scanned file defines it"
  canary a-stale-leaf allow 'we_write.c|leaf:wpe_write_never_called|1|keep|x' "stale leaf"
  canary a-syscall body ' syscall(SYS_unlink, p);' "call of syscall"
  canary a-execlp body ' execlp("rm", "rm", "-f", p, 0);' "call of execlp"
  canary a-execvp body ' execvp(prog, argv);' "call of execvp"
  canary a-posix-spawn body ' posix_spawn(&pid, "/bin/rm", 0, 0, argv, 0);' "call of posix_spawn"
  canary a-lone-rm body ' char *argv[] = { "rm", p, 0 };' "shell command that removes"

  # b: the names owner.
  canary b-call body ' wpe_fileop_remove(p, &x, &y);' "call of wpe_fileop_remove"
  canary b-tree body ' wpe_fileop_remove_tree(p);' "call of wpe_fileop_remove_tree"
  canary b-pointer fs 'static int (*mover)(const char *) = wpe_fileop_move;' "bare of wpe_fileop_move"
  canary b-define fs '#define MOVE wpe_fileop_move' "#define MOVE names wpe_fileop_move"

  # c: no list of names of its own.
  canary c-is-new body ' int is_new = 1;' "is_new"
  canary c-renamed-list fs 'static char **ck_seen_names;' "ck_seen_names"
  canary c-old-list fs 'static char **ck_before;' "ck_before"
  canary c-ck-files body ' ck_files = 0;' "ck_files"
  canary c-struct-member fs 'typedef struct { char *path; int line; char *text; int fresh; WpeCkVerdictKind kind; } ws_change;' "struct ws_change"
  canary c-kind-write body ' c->kind = 1;' "kind is written outside ws_add_change"
  canary c-kind-write-dot body ' (*c)[0].kind = WPE_CK_CREATED;' "kind is written outside ws_add_change"

  canary c-char-array fs 'static char *seen_names[64];' "array of char pointers seen_names"
  canary c-char-array-plain fs 'char *seen_names[WS_MAX];' "array of char pointers seen_names"

  # d: where a row is made.
  canary d-call body ' ws_add_change(c, n, cap, "p", 1, "t", WPE_CK_CREATED);' "outside the places a row is made"
  canary d-pointer body ' void (*add)(void) = ws_add_change;' "bare of ws_add_change"
  canary d-define fs '#define ADD ws_add_change' "#define ADD names ws_add_change"

  # e: one walk.
  canary e-readdir body ' readdir(d);' "call of readdir"
  canary e-opendir body ' opendir(p);' "call of opendir"
  canary e-fdopendir body ' fdopendir(1);' "call of fdopendir"
  canary e-scandir body ' scandir(p, 0, 0, 0);' "call of scandir"
  canary e-nftw body ' nftw(p, 0, 1, 0);' "call of nftw"
  canary e-glob body ' glob(p, 0, 0, 0);' "call of glob"
  canary e-read-folder body ' wpe_read_folder(p, 0, 0);' "call of wpe_read_folder"
  canary e-entry-look body ' wpe_read_entry_look(d, "x", 0);' "call of wpe_read_entry_look"
  canary e-pointer fs 'static void *walker = readdir;' "bare of readdir"
  canary e-define fs '#define WALK opendir' "#define WALK names opendir"
  canary2 e-helper-elsewhere other 'static void ai_walk(const char *p) { opendir(p); }' body ' ai_walk("x");' "closure: calls ai_walk"

  canary e-shell-find body ' run("cd %s && find . -type f", q);' "shell walk"
  canary e-shell-ls body ' run("ls -R");' "shell walk"

  # f: the words are the owner's.
  canary f-helper-in-another-file other 'static void say_last(void) { puts("ALL changes reverted to the checkpoint"); }' "words ALL changes reverted"
  canary f-helper-sentence other 'static void say_it(void) { puts("moved by the run: x"); }' "words moved by the run"
  canary f-new-file body ' say("[AI] new file");' "words [AI] new file"
  canary f-split body ' say("[AI] new " "file");' "words [AI] new file"
  canary f-removed body ' say("[AI] removed");' "words [AI] removed"
  canary f-last-line body ' say("ALL changes reverted to the checkpoint");' "words ALL changes reverted"
  canary f-empty body ' say("=== AI changeset: no files changed ===");' "words no files changed"
  canary f-not-watched body ' say("not watched: x");' "words not watched:"
  canary f-bound body ' say("the snapshot watches %d files");' "words the snapshot watches"
  canary f-in-header hdr2 '#define WS_ALL "ALL changes reverted"' "words ALL changes reverted"
  canary f-in-header-table hdr2 'static const char *words_row[] = { "[AI] moved here" };' "words [AI] moved"
  canary f-define-in-ws fs '#define WS_LAST "kept the change"' "words kept the change"

  # g: no result of the owner thrown away.
  canary g-void-cast body ' (void)wpe_ckrec_judge_all(r, &v);' "thrown away"
  canary g-void-cast-spaced body ' ( void ) wpe_ckrec_may_remove(r, p, &v);' "thrown away"

  # h: every state has words and a test.
  canary h-missing-case case_moved '' "wpe_ckrec_row_text has no case for WPE_CK_MOVED"
  canary h-default switch_tail ' default: return 0;' "has a default"
  canary h-missing-test tst_moved '' "WPE_CK_MOVED is not in a row"
  canary h-new-enumerator hdr 'typedef enum { WPE_CK_WHY_EXTRA } WpeCkWhy;' "WPE_CK_WHY_EXTRA"
  canary h-new-kind-no-case hdr 'typedef enum { WPE_CK_NEWKIND } WpeCkVerdictKind;' "WPE_CK_NEWKIND"

  # i: identity is not decided here.
  canary i-strcmp ck_body ' strcmp(a, b);' "call of strcmp"
  canary i-strncmp ck_body ' strncmp(a, b, 3);' "call of strncmp"
  canary i-memcmp ck_body ' memcmp(a, b, 3);' "call of memcmp"
  canary i-pointer ck_body ' int (*cmp)(const char *, const char *) = strcmp;' "bare of strcmp"
  canary i-nlink ck_body ' return (int)wpe_seen_nlink(s);' "wpe_seen_nlink"
  canary i-kind ck_body ' return wpe_seen_kind(s) == 1;' "wpe_seen_kind outside"
  canary i-kind-and-size ck_body ' return wpe_seen_kind(s) == wpe_seen_kind(t);' "wpe_seen_kind outside"
  canary i-char-compare ck_body ' return *a == *b;' "compares characters"
  canary i-index-compare ck_body ' return a[i] != b[i];' "compares characters"
  canary i-size ck_body ' return (int)wpe_seen_size(s);' "wpe_seen_size outside ck_maybe_copy"

  selftest_compile
  selftest_objects
}

# The -Wswitch-enum half: a switch that misses an enumerator does not compile.
selftest_compile() {
  test_cc_require
  mkdir -p "$WORK/fxc"
  cat > "$WORK/fxc/miss.c" <<'EOF'
typedef enum { A_ONE, A_TWO, A_THREE } A;
int f(A a)
{
 switch (a) {
 case A_ONE: return 1;
 case A_TWO: return 2;
 }
 return 0;
}
EOF
  canaries_run=$((canaries_run + 1))
  if test_cc -Wswitch-enum -Werror=switch-enum -fsyntax-only -c "$WORK/fxc/miss.c" > /dev/null 2>&1; then
    problem "canary h-switch-enum: a switch that misses an enumerator compiles (the flag is not in force)"
  fi
}

emit_obj_ck_clean() { cat <<'EOF'
#include <dirent.h>
int ck_clean(DIR *d) { return readdir(d) != 0; }
EOF
}
emit_obj_ck_unlink() { cat <<'EOF'
#include <dirent.h>
#include <unistd.h>
int ck_dirty(DIR *d, const char *p) { return readdir(d) != 0 || unlink(p); }
EOF
}
emit_obj_ws_clean() { cat <<'EOF'
#include <stdlib.h>
int ws_clean(char *t) { return mkstemp(t); }
EOF
}
emit_obj_ws_rename() { cat <<'EOF'
#include <stdio.h>
#include <stdlib.h>
int ws_dirty(char *t) { return mkstemp(t) || rename(t, t); }
EOF
}

make_fixture_object() {
  name=$1; emitter=$2
  $emitter > "$WORK/fxo/${name%.o}.c"
  test_cc -c -o "$WORK/fxo/$name" "$WORK/fxo/${name%.o}.c" 2> "$WORK/fxo/cc.err"
}

selftest_objects() {
  if [ "$NMTOOL" = ":" ] || ! command -v "$NMTOOL" > /dev/null 2>&1; then
    SKIPPED="$SKIPPED the selftest's object canaries not run (no nm: $NMTOOL);"
    return
  fi
  mkdir -p "$WORK/fxo"
  make_fixture_object we_ai_ckrec.o emit_obj_ck_clean &&
    make_fixture_object we_ai_ws.o emit_obj_ws_clean || {
    problem "selftest: the fixture objects were not compiled: $(cat "$WORK/fxo/cc.err")"
    return
  }
  $NMTOOL -u "$WORK/fxo/we_ai_ckrec.o" > /dev/null 2>&1 || { problem "selftest: $NMTOOL cannot read an object"; return; }
  canaries_run=$((canaries_run + 1))
  ALLOW_SAVE=$ALLOW; ALLOW=$WORK/fx/fx.allow
  a_object_scan "$WORK/fxo/we_ai_ckrec.o" "$WORK/fxo/we_ai_ws.o" > "$WORK/fxo/clean.out"
  if [ -s "$WORK/fxo/clean.out" ]; then
    problem "selftest: the clean objects are named (the clause is noisy):"
    cat "$WORK/fxo/clean.out" >> "$WORK/problems"
  fi
  # The record's object that imports unlink.
  canaries_run=$((canaries_run + 1))
  mkdir -p "$WORK/fxo/d1"
  emit_obj_ck_unlink > "$WORK/fxo/d1/we_ai_ckrec.c"
  test_cc -c -o "$WORK/fxo/d1/we_ai_ckrec.o" "$WORK/fxo/d1/we_ai_ckrec.c" 2> /dev/null
  a_object_scan "$WORK/fxo/d1/we_ai_ckrec.o" "$WORK/fxo/we_ai_ws.o" > "$WORK/fxo/case.out"
  grep -q -F "we_ai_ckrec.o imports unlink" "$WORK/fxo/case.out" ||
    problem "canary o-ckrec-imports-unlink: not named (the object half is blind)"
  # The checkpoint's object that imports rename, which no allow line lists.
  canaries_run=$((canaries_run + 1))
  mkdir -p "$WORK/fxo/d2"
  emit_obj_ws_rename > "$WORK/fxo/d2/we_ai_ws.c"
  test_cc -c -o "$WORK/fxo/d2/we_ai_ws.o" "$WORK/fxo/d2/we_ai_ws.c" 2> /dev/null
  a_object_scan "$WORK/fxo/we_ai_ckrec.o" "$WORK/fxo/d2/we_ai_ws.o" > "$WORK/fxo/case.out"
  grep -q -F "we_ai_ws.o imports rename" "$WORK/fxo/case.out" ||
    problem "canary o-ws-imports-rename: not named (the object half is blind)"
  # An object list without the record's object.
  canaries_run=$((canaries_run + 1))
  a_object_scan "$WORK/fxo/we_ai_ws.o" > "$WORK/fxo/case.out"
  grep -q -F "we_ai_ckrec.o is not among the objects" "$WORK/fxo/case.out" ||
    problem "canary o-no-record-object: not named (the object half is blind)"
  # An object nm cannot read.
  canaries_run=$((canaries_run + 1))
  mkdir -p "$WORK/fxo/d3"
  echo "this is not an object" > "$WORK/fxo/d3/we_ai_ckrec.o"
  a_object_scan "$WORK/fxo/d3/we_ai_ckrec.o" > "$WORK/fxo/case.out"
  grep -q -F "readdir not seen in we_ai_ckrec.o" "$WORK/fxo/case.out" ||
    problem "canary o-unreadable-object: not named (the object half is blind)"
  # An object that is not there.
  canaries_run=$((canaries_run + 1))
  a_object_scan "$WORK/fxo/missing.o" > "$WORK/fxo/case.out"
  grep -q -F "missing.o not found" "$WORK/fxo/case.out" ||
    problem "canary o-missing-object: not named (the object half is blind)"
  ALLOW=$ALLOW_SAVE
}

# The allow file must be there and name only clauses the script reads.
check_allow() {
  [ -f "$ALLOW" ] || { problem "no allow file at $ALLOW"; return; }
  awk -F'|' '
    /^#/ || /^[ \t]*$/ { next }
    { split($2, cf, ":")
      if (NF != 5 || cf[1] !~ /^(a|b|e|leaf)$/) { print "check-checkpoint-record: allow line " NR " is not file|clause:function|count|pass|reason of clause a, b or e: " $0; bad = 1 } }
    END { exit bad }' "$ALLOW" >> "$WORK/problems" || :
}

check_allow

# The canaries come first: a scan that sees nothing must fail the build.
selftest

if [ -z "$SELFTEST_ONLY" ]; then
  { src_scan "$SRCDIR" "$SRCDIR/tests/test_ai_ckrec.c" "$ALLOW"; real_objects; } > "$WORK/real.out"
  cat "$WORK/real.out" >> "$WORK/problems"
  test_cc_require
  canaries_run=$((canaries_run + 1))
  # The file is compiled as the build compiles it, through the config.h that
  # configure wrote for this system (Makefile.am: AM_CPPFLAGS), never with
  # feature macros named here: _XOPEN_SOURCE=600 hides AT_FDCWD on FreeBSD
  # and OpenBSD, so the check failed there on a declaration the build has.
  if ! test_cc -std=gnu17 -I. -include ./config.h -Wswitch-enum -Werror=switch-enum \
        -fsyntax-only -I"$SRCDIR" "$SRCDIR/we_ai_ckrec.c" > "$WORK/swenum.out" 2>&1; then
    problem "H: we_ai_ckrec.c does not compile with -Wswitch-enum:"
    cat "$WORK/swenum.out" >> "$WORK/problems"
  fi
fi

if [ -s "$WORK/problems" ]; then
  echo "check-checkpoint-record: FAIL"
  cat "$WORK/problems"
  exit $BAD
fi
if [ -n "$SKIPPED" ]; then
  echo "check-checkpoint-record: SKIP,$SKIPPED the source halves and their canaries passed"
  exit $SKIP
fi
echo "check-checkpoint-record: ok (a, b, c, d, e, f, g, h, i, o; $canaries_run canaries)"
exit 0
