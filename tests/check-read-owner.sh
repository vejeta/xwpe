#!/bin/sh
# check-read-owner.sh -- a file or a folder is read by one owner.
#
# A name a user typed, a folder held, a model wrote or a server sent can lead
# to a pipe nobody writes to, a terminal, a device that never ends or a file of
# any size.  A plain fopen of it waits for ever, takes the keys the user is
# typing, or fills the memory.  That is avoided only while one piece of code
# (we_read.c) opens such a name: without waiting and without taking a
# terminal, judging the object on the descriptor before a byte is read,
# bounding what comes out, and handing back its identity.  A second piece that
# opens a file for reading, a folder for listing, or builds a read request by
# hand, is how a read waited, one site after another.
#
#   check-read-owner.sh [--allow FILE] [--srcdir DIR] [--nm NM] [--list]
#                       [--selftest] [OBJECT...]
#
# This is rule 22 (22 and 23 are named in the header of check-write-owner.sh).  Scope: the product sources
# and headers at the top level, never tests/.  Comments are blanked, a call
# split over lines is joined, a macro line is a site of its own, words are
# bounded (reopen( and my_fopen( are not fopen).  Outside we_read.c and
# we_read.h:
#  a. fopen or freopen whose mode is not a literal that starts with w or a.  A
#     mode that is a variable counts.
#  b. open, open64, openat or openat64 whose flags name none of O_WRONLY,
#     O_RDWR, O_CREAT, O_TRUNC and do not name O_DIRECTORY.  Not a finding: a
#     path that is the literal "/dev/null" or "/dev/tty" (the editor's own
#     devices); any other /dev/ literal is one.
#  c. opendir, scandir, nftw, ftw, fts_open or glob (their 64 spellings too),
#     anywhere; fdopendir outside we_fileop.c (the names owner holds the one
#     descriptor it opened itself).
#  d. fdopen with a mode that does not start with w or a.
#  e. a library that opens a file by name: json_object_from_file,
#     json_object_from_fd, gzopen, XrmGetFileDatabase.
#  f. getline or getdelim: a line with no bound of its own.
#  g. a WpeReadAsk built by hand ("WpeReadAsk x = {", a compound literal, or a
#     member assigned) instead of a WPE_READ_ASK_* form; a number as the bound
#     of WPE_READ_ASK_JUDGED; WPE_READ_UNBOUNDED, WPE_READ_ASK_SCAN,
#     WPE_READ_ASK_SCAN_NOFOLLOW and WPE_READ_ASK_LOAD, which hold no bound, in
#     a function the allow file does not list with its reason.
#  n. stat, lstat, fstatat, access or faccessat (their 64 spellings too) by
#     name in a File Manager listing function: e_find_files, e_find_dir,
#     e_find_open_folder, e_find_entry_is_*, e_find_look_row(s), e_file_info*,
#     fm_folder_has_entries, WpeGraphicalFileList, WpeFileListRows.  An entry
#     is looked at through the folder that was read, never by a joined path.
#  h. WPE_READ_KIND_PIPE: the user's decision is that only the load of a pipe
#     may name it, and that load is not built yet.
#  p. WPE_READ_ASK_PROJECT and WPE_READ_ASK_PROJECT_SEEN, anywhere but in
#     e_prj_read_open and its two steps (e_prj_read_held, e_prj_read_asked),
#     e_project_open_error and e_select_project: the open project is read by
#     one opener, which asks for the file the editor holds.  No allow line.
#  q. a call of a wpe_read_ face whose first argument is e_prog.project or a
#     call of e_prj_open_path(, in the same functions' absence: a reader that
#     takes the project by name, whatever form it asks with.  A local variable
#     holding the path escapes it; the rows of test_project_hold.py are the net
#     for that.  No allow line.
#  s. in e_edit_open and e_edit_take_file_state: wpe_write_identity_of_name,
#     wpe_write_identity_take_fd, wpe_seen_look_name, fstat or lstat.  The
#     window opened on a file takes its mode and identity from the outcome of
#     the read, so they describe the object its text came from.  No allow line.
# In we_read.c itself:
#  i. a string that holds "Cannot", "not found", "it is" or "larger": the
#     words are we_path.c's.
#  j. a memory stream made anywhere but in read_stream_of_hold, or made in a
#     shape a reader can write to or that leaves the bytes with the caller:
#     fmemopen, fopencookie whose mode is not "r", funopen with a write
#     callback, tmpfile.  The stream owns its bytes, so fclose frees them, no
#     caller holds a second pointer to release, and a reader is given no byte
#     the file does not hold.  On a system with neither callback family the
#     bytes live in a temporary file, which is why tmpfile is named here too
#     and belongs to the one function that makes the stream.  Outside
#     we_read.c: fmemopen, fopencookie, funopen and tmpfile.
#  l. ELOOP, EMLINK, EFTYPE, ENXIO, EOPNOTSUPP or ENOTSUP: another file at the
#     name is decided by looking at the name again, never from an errno that
#     each system spells its own way.
#  m. open, fopen, freopen, open64, openat64 or creat; openat outside
#     read_target_open_fd: the one open of the owner.
# Everywhere:
#  k. symbols (nm -u of the editor's objects, normalised: the 64 spellings, a
#     NetBSD __opendir30, a Mach-O $INODE64): opendir, scandir, nftw, ftw,
#     fts_open, glob, json_object_from_file and gzopen are undefined only in
#     objects the allow file lists.  A macro cannot hide a name from the
#     linker.  A row for an object this build does not make, or ending in
#     "/cfg" in its pass column (something only some configurations import),
#     is not stale when it finds nothing.
#  z. completeness: every open, open64, openat and openat64 token of the
#     blanked text (not a member: x.open, x->open) must be a call whose
#     arguments were read, so it was classified by a rule above or by rule 2
#     of check-write-owner.sh (a write), by O_DIRECTORY (a folder held) or by
#     the two /dev/ literals.  A token that is not a call (= open;, (open)(,
#     a wrapper macro name) or a call with too few arguments is a finding.
# The predicate that says an open is a write (the four flags, a fopen mode
# starting with w or a) is the one rule 2 of check-write-owner.sh uses; the two
# must stay one predicate.
#
# Offenders still to go are lines of tests/read-owner.allow:
#   file|r22x:function|count|pass|reason
# x is the clause letter.  The count may only fall, a line that matches
# nothing is stale and fails, and a line with a pass (P2, P3a, P3b, P3c) is
# deleted by that pass; "keep" is permanent and its reason says why the name
# is the editor's own and not one a user, a folder, a model or a server chose.
# --list prints what was found as allow lines.  --selftest feeds fixtures to
# the scan, one per offence and per shape that is no offence, and fails unless
# the scan names every one and is quiet on the rest (a scanner that sees
# nothing is not a check).
#
# Limits, said plainly: a source scan does not see an open reached through a
# function pointer or a macro of a system header; clause k covers the folder and
# library names only, because the write side imports open and fopen too.  A
# rule on the finished binary and a recorder of the calls a run makes are what
# would close the rest.
#
# Environment (set by `make check`): NM, ONE_OWNER_OBJECTS, srcdir.
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
HERE=$(dirname "$0")
# The one blanker of comments and literals (tests/blank-c.awk).
BLANK_C=$(cat "$HERE/blank-c.awk")
ALLOW=
LIST=0
SELFTEST=0
CANARY=1
NMTOOL=${NM:-nm}
OBJECTS=
BAD=1
. "$HERE/objsyms.sh"
. "$HERE/compiler.sh"

while [ $# -gt 0 ]; do
  case $1 in
    --allow) shift; ALLOW=$1 ;;
    --srcdir) shift; SRCDIR=$1 ;;
    --nm) shift; NMTOOL=$1 ;;
    --list) LIST=1 ;;
    --selftest) SELFTEST=1 ;;
    --no-canary) CANARY=0 ;;
    -*) echo "check-read-owner: unknown argument $1" >&2; exit $BAD ;;
    *) OBJECTS="$OBJECTS $1" ;;
  esac
  shift
done
[ $SELFTEST -eq 1 ] || [ -n "$OBJECTS" ] || OBJECTS=$ONE_OWNER_OBJECTS
[ -n "$ALLOW" ] || ALLOW=$HERE/read-owner.allow

. "$HERE/workdir.sh"
work_dir read-owner $BAD
: > "$WORK/problems"
problem() { echo "$*" >> "$WORK/problems"; }
: > "$WORK/notbuilt"       # one word per line, for what this build lacks

if [ -n "$OBJECTS" ]; then
  [ $LIST -eq 1 ] && WPE_BUILD_QUIET=1
  . "$HERE/build-config.sh"
fi

# listed_objects - the path of each object of this run, as nm can read it.
# An object that is listed and not on disk is a problem of its own: the scan
# that reads it would otherwise pass without having looked.
listed_objects() {
  for o in $OBJECTS; do
    if [ -f "$o" ]; then
      echo "$o"
    elif [ -f "$SRCDIR/$o" ]; then
      echo "$SRCDIR/$o"
    else
      problem "listed object $o is not on disk: build the editor first (the scan is blind)"
    fi
  done
}

# scan FILE - one line "rule file function count" for each offence of the
# clauses above in FILE.  Comments are blanked, string literals kept; a
# function starts at a line in column 0 that names an identifier followed by
# "(" and does not end with ";" (a prototype); a statement is the text up to
# the next ; { or }.
scan() {
  awk -v file="$(basename "$1")" "$BLANK_C"'
    function close_paren(s, p,   depth, i, n, c) {
      depth = 0; n = length(s)
      for (i = p; i <= n; i++) {
        c = substr(s, i, 1)
        if (c == "\"") { i++; while (i <= n && substr(s, i, 1) != "\"") { if (substr(s, i, 1) == "\\") i++; i++ } continue }
        if (c == "(") depth++
        else if (c == ")") { depth--; if (depth == 0) return i + 1 }
      }
      return 0
    }
    function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
    # the nth top-level argument (1-based) of the call whose "(" is at p in s,
    # blanks trimmed; "" when the call has fewer
    function nth_arg(s, p, n,   depth, i, len, c, k, start) {
      depth = 0; k = 1; len = length(s); start = p + 1
      for (i = p; i <= len; i++) {
        c = substr(s, i, 1)
        if (c == "\"") { i++; while (i <= len && substr(s, i, 1) != "\"") { if (substr(s, i, 1) == "\\") i++; i++ } continue }
        if (c == "(" || c == "[" || c == "{") depth++
        else if (c == ")" || c == "]" || c == "}") {
          depth--
          if (depth == 0) { if (k == n) return trim(substr(s, start, i - start)); return "" }
        } else if (c == "," && depth == 1) {
          if (k == n) return trim(substr(s, start, i - start))
          k++; start = i + 1
        }
      }
      return ""
    }
    # how many top-level arguments the call whose "(" is at p has
    function arg_count(s, p,   n) {
      n = 0
      while (nth_arg(s, p, n + 1) != "") n++
      return n
    }
    function note(rule, n) { if (ppmode) ppcnt[rule] += n; else cnt[rule] += n }
    function flush_fn(   r) {
      if (fn == "") return
      for (r in cnt) if (cnt[r] > 0) print r, file, fn, cnt[r]
      delete cnt; delete asks
    }
    # every word-bounded match of the extended regex re in t counts for tag;
    # a member access (x.name, x->name) is not a name of this program
    function count_word(t, re, tag,   n) {
      n = 0
      while (match(t, "(^|[^A-Za-z0-9_.>])(" re ")([^A-Za-z0-9_]|$)")) { note(tag, 1); n++; t = substr(t, RSTART + RLENGTH - 1) }
      return n
    }
    # the same for a call: the name followed by "(" (a variable called ftw or
    # getline is not the function)
    function count_call(t, re, tag,   n) {
      n = 0
      while (match(t, "(^|[^A-Za-z0-9_.>])(" re ")[ \t]*[(]")) { note(tag, 1); n++; t = substr(t, RSTART + RLENGTH - 1) }
      return n
    }
    function owner_file() { return (file == "we_read.c" || file == "we_read.h") }
    # clauses a and d: a mode that is a literal starting with w or a is a write
    function note_modes(s,   t, p, e, args, mode) {
      t = s
      while (match(t, /(^|[^A-Za-z0-9_.>])(fopen|fopen64)[ \t]*[(]/)) {
        p = RSTART + RLENGTH - 1; e = close_paren(t, p)
        if (e > 0 && substr(t, p, e - p) !~ /"[wa]/) note("r22a", 1)
        t = substr(t, p + 1)
      }
      t = s
      while (match(t, /(^|[^A-Za-z0-9_.>])(freopen|freopen64)[ \t]*[(]/)) {
        p = RSTART + RLENGTH - 1; e = close_paren(t, p)
        if (e > 0 && substr(t, p, e - p) !~ /"[wa]/) note("r22a", 1)
        t = substr(t, p + 1)
      }
      t = s
      while (match(t, /(^|[^A-Za-z0-9_.>])fdopen[ \t]*[(]/)) {
        p = RSTART + RLENGTH - 1; mode = nth_arg(t, p, 2)
        if (mode !~ /^"[wa]/) note("r22d", 1)
        t = substr(t, p + 1)
      }
    }
    # clauses b and z: every open token must be a call whose arguments were read
    function note_opens(s, m,   t, p, e, args, name, need, path, tokens, parsed) {
      tokens = 0; parsed = 0
      t = m
      while (match(t, /(^|[^A-Za-z0-9_.>])(open|open64|openat|openat64)([^A-Za-z0-9_]|$)/)) { tokens++; t = substr(t, RSTART + RLENGTH - 1) }
      t = s
      while (match(t, /(^|[^A-Za-z0-9_.>])(open|open64|openat|openat64)[ \t]*[(]/)) {
        name = substr(t, RSTART, RLENGTH); sub(/^[^A-Za-z]/, "", name); sub(/[ \t]*[(]$/, "", name)
        p = RSTART + RLENGTH - 1; e = close_paren(t, p)
        need = (name ~ /^openat/) ? 3 : 2
        if (e > 0 && arg_count(t, p) >= need) {
          parsed++
          args = substr(t, p, e - p)
          path = nth_arg(t, p, need == 3 ? 2 : 1)
          if (args ~ /(O_WRONLY|O_RDWR|O_CREAT|O_TRUNC)/) { }
          else if (args ~ /O_DIRECTORY/) { }
          else if (path == "\"/dev/null\"" || path == "\"/dev/tty\"") { }
          else note("r22b", 1)
        }
        t = substr(t, p + 1)
      }
      if (tokens > parsed) note("r22z", tokens - parsed)
    }
    # clause g: how a read request is built
    function note_asks(s, m,   t, p, a, v, e) {
      t = m
      while (match(t, /WpeReadAsk[ \t*]+[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*[{]/)) { note("r22g", 1); t = substr(t, RSTART + RLENGTH) }
      t = m
      while (match(t, /[(][ \t]*WpeReadAsk[ \t]*[)][ \t]*[{]/)) { note("r22g", 1); t = substr(t, RSTART + RLENGTH) }
      t = m
      while (match(t, /WpeReadAsk[ \t*]+[A-Za-z_][A-Za-z0-9_]*/)) {
        v = substr(t, RSTART, RLENGTH); sub(/^WpeReadAsk[ \t*]+/, "", v); asks[v] = 1; t = substr(t, RSTART + RLENGTH)
      }
      for (v in asks)
        if (m ~ ("(^|[^A-Za-z0-9_])" v "[ \t]*(\\.|->)[ \t]*(what|how|kinds|bound|over|expect|dirfd)[ \t]*(=[^=]|\\|=|&=)")) note("r22g", 1)
      t = s
      while (match(t, /(^|[^A-Za-z0-9_.>])WPE_READ_ASK_JUDGED[ \t]*[(]/)) {
        p = RSTART + RLENGTH - 1; a = nth_arg(t, p, 2)
        if (a ~ /^[0-9]/) note("r22g", 1)
        t = substr(t, p + 1)
      }
      count_word(m, "WPE_READ_UNBOUNDED|WPE_READ_ASK_SCAN|WPE_READ_ASK_SCAN_NOFOLLOW|WPE_READ_ASK_LOAD", "r22g")
    }
    # clauses p and q: the open project is read by one opener.  p: the forms
    # WPE_READ_ASK_PROJECT and WPE_READ_ASK_PROJECT_SEEN are named only in the
    # functions of that opener and the two gates; q: no read owner face is
    # given e_prog.project or a call of e_prj_open_path( as its path.  A local
    # variable holding the path escapes q; the rows of test_project_hold.py are
    # the net for that.
    function note_project(s, m,   t, p, a) {
      if (fn ~ /^(e_prj_read_open|e_prj_read_held|e_prj_read_asked|e_project_open_error|e_select_project)$/) return
      count_word(m, "WPE_READ_ASK_PROJECT|WPE_READ_ASK_PROJECT_SEEN", "r22p")
      t = s
      while (match(t, /(^|[^A-Za-z0-9_.>])wpe_read_[a-z_0-9]+[ \t]*[(]/)) {
        p = RSTART + RLENGTH - 1; a = nth_arg(t, p, 1)
        if (a ~ /^e_prog[ \t]*\.[ \t]*project$/ || a ~ /^e_prj_open_path[ \t]*[(]/) note("r22q", 1)
        t = substr(t, p + 1)
      }
    }
    # the clauses of we_read.c itself
    function note_owner(s, m,   t, p, e, lit, a, mode, name) {
      t = s
      while (match(t, /"([^"\\]|\\.)*"/)) {
        lit = substr(t, RSTART, RLENGTH)
        if (lit ~ /Cannot|not found|it is|larger/) note("r22i", 1)
        t = substr(t, RSTART + RLENGTH)
      }
      count_word(m, "fmemopen", "r22j")
      t = s
      while (match(t, /(^|[^A-Za-z0-9_.>])(fopencookie|funopen|tmpfile)[ \t]*[(]/)) {
        name = substr(t, RSTART, RLENGTH); sub(/^[^A-Za-z]/, "", name); sub(/[ \t]*[(]$/, "", name)
        p = RSTART + RLENGTH - 1
        if (fn != "read_stream_of_hold") note("r22j", 1)
        else if (name == "fopencookie" && nth_arg(t, p, 2) != "\"r\"") note("r22j", 1)
        else if (name == "funopen" && nth_arg(t, p, 3) != "NULL") note("r22j", 1)
        t = substr(t, p + 1)
      }
      count_word(m, "ELOOP|EMLINK|EFTYPE|ENXIO|EOPNOTSUPP|ENOTSUP", "r22l")
      count_word(m, "open|fopen|freopen|open64|openat64|creat", "r22m")
      if (fn != "read_target_open_fd") count_word(m, "openat", "r22m")
    }
    function statement(s,   m) {
      m = s; gsub(/"([^"\\]|\\.)*"/, "\"\"", m)
      if (owner_file()) {
        if (file == "we_read.c") note_owner(s, m)
        return
      }
      note_modes(s)
      note_opens(s, m)
      note_asks(s, m)
      note_project(s, m)
      count_call(m, "opendir|scandir|nftw|ftw|fts_open|glob|opendir64|scandir64|nftw64|ftw64|fts_open64|glob64", "r22c")
      if (file != "we_fileop.c") count_call(m, "fdopendir", "r22c")
      count_call(m, "json_object_from_file|json_object_from_fd|gzopen|gzopen64|XrmGetFileDatabase", "r22e")
      count_call(m, "getline|getdelim", "r22f")
      if (fn ~ /^(e_find_(files|dir|open_folder|entry_is_[a-z]+|look_rows?)|e_file_info[a-z_]*|fm_folder_has_entries|WpeGraphicalFileList|WpeFileListRows)$/)
        count_call(m, "stat|lstat|fstatat|access|faccessat|eaccess|stat64|lstat64|fstatat64", "r22n")
      count_word(m, "WPE_READ_KIND_PIPE", "r22h")
      if (fn ~ /^(e_edit_open|e_edit_take_file_state)$/)
        count_call(m, "wpe_write_identity_of_name|wpe_write_identity_take_fd|wpe_seen_look_name|fstat|lstat|fstat64|lstat64", "r22s")
      if (m ~ /(^|[^A-Za-z0-9_.>])(fmemopen|fopencookie|funopen|tmpfile)([^A-Za-z0-9_]|$)/) note("r22j", 1)
    }
    BEGIN { blank_incom = 0; fn = "-"; stmt = "" }
    {
      out = blank_c($0, 1)
      if (out ~ /^[ \t]*#/) {
        # a macro line is a site of its own, under the name (preprocessor)
        if (out ~ /^[ \t]*#[ \t]*define/) { ppmode = 1; statement(out); ppmode = 0 }
        next
      }
      if (out ~ /^(static[ \t]+)?(const[ \t]+)?char[ \t]*[*][ \t]*[A-Za-z_0-9]+\[\][ \t]*=/) {
        name = out; sub(/\[\].*/, "", name); sub(/.*[^A-Za-z0-9_]/, "", name)
        statement(stmt); stmt = ""; flush_fn(); fn = name
      }
      if (out ~ /^[A-Za-z_]/ && out ~ /[(]/ && out !~ /;[ \t]*$/ && out !~ /^(typedef|struct[^(]*[{]|enum|static[ \t]+[A-Za-z_ *]*=)/) {
        name = out; sub(/[ \t]*[(].*/, "", name); sub(/.*[^A-Za-z0-9_]/, "", name)
        if (name != "") { statement(stmt); stmt = ""; flush_fn(); fn = name }
      }
      if (out ~ /^[ \t]*$/) next
      stmt = stmt " " out
      if (out ~ /[;{}][ \t]*$/) { statement(stmt); stmt = "" }
    }
    END {
      statement(stmt); flush_fn()
      for (r in ppcnt) if (ppcnt[r] > 0) print r, file, "(preprocessor)", ppcnt[r]
    }' "$1"
}

# scan_tree DIR - every product source and header at the top level of DIR.
scan_tree() {
  for f in "$1"/*.c "$1"/*.h; do
    [ -f "$f" ] || continue
    scan "$f"
  done
}

# scan_symbols OBJECT... - one line "r22k object symbol 1" for each folder or
# library opener an object imports, as the C source spells it.
scan_symbols() {
  for o in "$@"; do
    base=$(basename "$o")
    "$NMTOOL" -u "$o" 2>/dev/null | nm_undefined_names | awk -v obj="$base" "$OBJSYMS_AWK_LIB"'
      {
        n = objsyms_source_name($0)
        sub(/[@$].*/, "", n)
        if (n ~ /^(opendir|scandir|nftw|ftw|fts_open|glob|gzopen)64$/) sub(/64$/, "", n)
        if (n == "opendir" || n == "scandir" || n == "nftw" || n == "ftw" || n == "fts_open" ||
            n == "glob" || n == "json_object_from_file" || n == "gzopen") seen[n] = 1
      }
      END { for (n in seen) print "r22k", obj, n, 1 }'
  done
}

# --selftest: each clause must name its offence in a fixture, and must be
# quiet on what is no offence.
if [ $SELFTEST -eq 1 ]; then
  fx=$WORK/fx
  mkdir "$fx"
  cat > "$fx/we_fake.c" <<'EOF'
/* one of each offence */
int fake_a(char *p, char *mode)
{
 FILE *a = fopen(p, "rb");
 FILE *b = fopen(p, mode);
 FILE *c = freopen(p, "r", stdin);
 return(0);
}

int fake_b(char *p, int d)
{
 int a = open(p, O_RDONLY);
 int b = open(p, O_NOFOLLOW);
 int c = open(p,
              O_RDONLY |
              O_NONBLOCK);
 int e = openat(d, p, O_RDONLY | O_NONBLOCK);
 int g = open("/dev/sda", O_RDONLY);
 *fdv = open(p, O_RDONLY);
 return(a);
}

#define OPEN_RO(p) open(p, O_RDONLY)

int fake_c(char *p)
{
 DIR *a = opendir(p);
 int b = scandir(p, &l, 0, 0);
 nftw(p, cb, 8, 0);
 glob("*", 0, 0, &g);
 DIR *f = fdopendir(fd);
 return(0);
}

int fake_d(int fd)
{
 FILE *a = fdopen(fd, "r");
 FILE *b = fdopen(fd, mode);
 return(0);
}

int fake_e(char *p)
{
 json_object_from_file(p);
 gzopen(p, "rb");
 XrmGetFileDatabase(p);
 return(0);
}

int fake_f(FILE *fp)
{
 getline(&b, &n, fp);
 getdelim(&b, &n, 0, fp);
 return(0);
}

int fake_g(char *p)
{
 WpeReadAsk a = { "file", 0, 1, 4096 };
 WpeReadAsk b = WPE_READ_ASK_CONFIG("x");
 WpeReadAsk c = WPE_READ_ASK_SCAN("x");
 b.bound = 4096;
 x = (WpeReadAsk){ "x" };
 y = WPE_READ_ASK_JUDGED("x", 4096, 1, 0);
 z = WPE_READ_UNBOUNDED;
 w = WPE_READ_ASK_SCAN_NOFOLLOW("x");
 return(0);
}

int fake_h(void)
{
 return(WPE_READ_KIND_PIPE);
}

int fake_reader(ECNT *cn)
{
 WpeReadAsk a = WPE_READ_ASK_PROJECT("project file");
 WpeReadAsk b = WPE_READ_ASK_PROJECT_SEEN("project file", seen);
 FILE *c = wpe_read_stream_open(e_prog.project, &cfg, &o);
 char *d = wpe_read_text(e_prj_open_path(cn), &cfg, &n, &o);
 return(0);
}

int e_edit_take_file_state(FENSTER *f, int fd)
{
 wpe_write_identity_of_name(f->dirct, f->datnam, &f->file_id);
 wpe_write_identity_take_fd(&f->file_id, fd);
 fstat(fd, &st);
 return(0);
}

int fake_j(char *buf, int n)
{
 FILE *a = fmemopen(buf, n, "r");
 FILE *b = fopencookie(buf, "r", calls);
 FILE *c = funopen(buf, rd, 0, sk, cl);
 FILE *d = tmpfile();
 return(0);
}

int e_file_info_look(char *p)
{
 struct stat b;
 stat(p, &b);
 lstat(p, &b);
 return(access(p, F_OK));
}

int fake_z(void)
{
 int (*f)(const char *, int, ...) = open;
 int a = (open)("x", 0);
 int b = open("x");
 return(0);
}
EOF
  cat > "$fx/we_quiet.c" <<'EOF'
/* what is no offence */
int quiet_writes(char *p, int d)
{
 FILE *a = fopen(p, "w");
 FILE *b = fopen(p, "ab");
 int c = open(p, O_WRONLY | O_CREAT, 0600);
 int e = open(p, O_RDWR);
 int f = openat(d, p, O_WRONLY | O_TRUNC);
 return(a != 0);
}

int quiet_folders(char *p)
{
 int a = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
 int b = open("/dev/null", O_RDONLY);
 int c = open("/dev/tty", O_RDWR);
 int e = openat(AT_FDCWD, "/dev/null", O_RDONLY);
 return(a);
}

int quiet_stat_elsewhere(char *p)
{
 struct stat b;
 stat(p, &b);
 return(access(p, F_OK));
}

int e_find_files(char *p)
{
 return(wpe_read_entry_look(d, p, &s) + wpe_seen_stat_of(p) + my_stat(p));
}

static FILE *e_prj_read_open(FENSTER *f)
{
 WpeReadAsk ask = WPE_READ_ASK_PROJECT("project file");
 return(wpe_read_stream_open(e_prj_open_path(f->ed), &ask, &o));
}

int e_select_project(ECNT *cn, char *path)
{
 WpeReadAsk ask = WPE_READ_ASK_PROJECT("project file");
 return(wpe_read_name_ok(path, &ask, &why));
}

int quiet_names(struct x *x)
{
 /* fopen(p, "r") in a comment, and getline( too */
 char *s = "fopen(p, \"r\") opendir(x) getline(";
 x->open(p);
 x.open(p);
 reopen(p);
 my_fopen(p);
 my_fmemopen(p);
 x->funopen(p);
 my_tmpfile(p);
 x->tmpfile(p);
 s_getline(p);
 wpe_read_stream_open(p, &ask, &o);
 WpeReadAsk ask = WPE_READ_ASK_CONFIG("file");
 WpeReadAsk dir = WPE_READ_ASK_AT(WPE_READ_ASK_CONFIG("file"), 3);
 return(0);
}
EOF
  cat > "$fx/we_read.c" <<'EOF'
/* the owner: it may open, and has its own clauses */
static int read_target_open_fd(const char *p, int d)
{
 return(openat(d, p, O_RDONLY));
}

static FILE *read_stream_of_hold(void *hold)
{
 cookie_io_functions_t calls;
 FILE *a = fopencookie(hold, "r", calls);
 FILE *b = funopen(hold, rd, NULL, sk, cl);
 FILE *c = tmpfile();
 return(a ? a : (b ? b : c));
}

static int read_other(const char *p, int d, int *e)
{
 FILE *fp = fdopen(d, "r");
 char *w = "Cannot open";
 char *x = "it is a pipe";
 char *y = "File not found";
 char *z = "it is larger";
 FILE *m = fmemopen(NULL, 8, "w+");
 FILE *n = fopencookie(hold, "r+", calls);
 FILE *o = funopen(hold, rd, wr, sk, cl);
 FILE *q = tmpfile();
 if (*e == ELOOP || *e == EMLINK || *e == EFTYPE) return(1);
 if (*e == ENXIO || *e == EOPNOTSUPP || *e == ENOTSUP) return(2);
 open(p, 0);
 fopen(p, "r");
 return(openat(d, p, 0));
}
EOF
  cat > "$fx/we_read.h" <<'EOF'
/* the forms are defined here */
#define WPE_READ_UNBOUNDED ((size_t)-1)
#define WPE_READ_ASK_SCAN(w) ((WpeReadAsk){ (w), 0, 1, WPE_READ_UNBOUNDED })
#define WPE_READ_KIND_PIPE 0x2u
EOF
  : > "$WORK/fake.found"
  for f in "$fx"/*.c "$fx"/*.h; do scan "$f" >> "$WORK/fake.found"; done
  for want in \
    "r22a we_fake.c fake_a 3" \
    "r22b we_fake.c fake_b 6" \
    "r22b we_fake.c (preprocessor) 1" \
    "r22c we_fake.c fake_c 5" \
    "r22d we_fake.c fake_d 2" \
    "r22e we_fake.c fake_e 3" \
    "r22f we_fake.c fake_f 2" \
    "r22g we_fake.c fake_g 7" \
    "r22h we_fake.c fake_h 1" \
    "r22p we_fake.c fake_reader 2" \
    "r22q we_fake.c fake_reader 2" \
    "r22s we_fake.c e_edit_take_file_state 3" \
    "r22j we_fake.c fake_j 4" \
    "r22n we_fake.c e_file_info_look 3" \
    "r22z we_fake.c fake_z 3" \
    "r22i we_read.c read_other 4" \
    "r22j we_read.c read_other 4" \
    "r22l we_read.c read_other 6" \
    "r22m we_read.c read_other 3"; do
    if ! grep -qx "$want" "$WORK/fake.found"; then
      echo "check-read-owner: FAIL (selftest: the scan does not name: $want; it found:)"
      cat "$WORK/fake.found"
      exit $BAD
    fi
  done
  # what is no offence: nothing in the quiet file, nothing but the owner's own
  # clauses in we_read.c (read_target_open_fd, the memory stream of
  # read_stream_of_hold and the macros of we_read.h)
  if grep -E "^r22. we_quiet.c " "$WORK/fake.found" > /dev/null; then
    echo "check-read-owner: FAIL (selftest: a shape that is no offence was counted; it found:)"
    grep -E "^r22. we_quiet.c " "$WORK/fake.found"
    exit $BAD
  fi
  if grep -E "^r22. we_read.h |^r22. we_read.c read_target_open_fd |^r22. we_read.c read_stream_of_hold " "$WORK/fake.found" > /dev/null; then
    echo "check-read-owner: FAIL (selftest: the owner's own open, its own memory stream or the forms of its header were counted; it found:)"
    cat "$WORK/fake.found"
    exit $BAD
  fi
  if grep -E "^r22[a-h] we_read.c " "$WORK/fake.found" > /dev/null; then
    echo "check-read-owner: FAIL (selftest: a rule for the callers was counted in the owner; it found:)"
    cat "$WORK/fake.found"
    exit $BAD
  fi
  # clause k: an opener planted in an object is named, however the system
  # spells it; an object listed and not on disk is a problem of its own
  printf 'DIR *f(const char *p) { return opendir(p); }\nint g(void) { return gzopen != 0; }\n' > "$WORK/planted.c"
  test_cc_require
  if test_cc -c -o "$WORK/planted.o" "$WORK/planted.c" > /dev/null 2>&1 && command -v "$NMTOOL" > /dev/null 2>&1; then
    scan_symbols "$WORK/planted.o" > "$WORK/fake.syms"
    if ! grep -qx "r22k planted.o opendir 1" "$WORK/fake.syms"; then
      echo "check-read-owner: FAIL (selftest: clause k does not name opendir planted in an object; it found:)"
      cat "$WORK/fake.syms"
      exit $BAD
    fi
  fi
  printf '__opendir30\nopendir@GLIBC_2.2.5\nglob64\n_nftw64\n' | awk "$OBJSYMS_AWK_LIB"'
    { n = objsyms_source_name($0); sub(/^_/, "", n); sub(/[@$].*/, "", n); sub(/64$/, "", n); print n }' > "$WORK/fake.spell"
  if [ "$(tr '\n' ' ' < "$WORK/fake.spell")" != "opendir opendir glob nftw " ]; then
    echo "check-read-owner: FAIL (selftest: a system spelling of an opener is not folded; it gave:)"
    cat "$WORK/fake.spell"
    exit $BAD
  fi
  # the allow file: a stale row fails, a row above the finding fails, a keep
  # row with no reason fails, and a clean tree with its exact rows passes
  rm -f "$fx/we_fake.c" "$fx/we_read.c" "$fx/we_read.h"
  printf 'int only(const char *p) { FILE *f = fopen(p, "r"); return f != 0; }\n' > "$fx/we_only.c"
  printf 'we_only.c|r22a:only|1|P3a|a stub\n' > "$WORK/allow.ok"
  printf 'we_only.c|r22a:only|1|P3a|a stub\nwe_gone.c|r22a:gone|1|P3a|stale\n' > "$WORK/allow.stale"
  printf 'we_only.c|r22a:only|0|P3a|too few\n' > "$WORK/allow.low"
  printf 'we_only.c|r22a:only|1|keep|\n' > "$WORK/allow.noreason"
  : > "$WORK/allow.none"
  # (no objects: the fixture tree has none, and the symbol half is not under test here)
  env ONE_OWNER_OBJECTS= sh "$0" --no-canary --srcdir "$fx" --allow "$WORK/allow.ok" > "$WORK/run.ok" 2>&1 ||
    { echo "check-read-owner: FAIL (selftest: an exact allow row did not pass; it said:)"; cat "$WORK/run.ok"; exit $BAD; }
  # each bad allow file must fail, and for the reason it was made for
  printf 'we_only.c|r22a:only|2|P3a|too many\n' > "$WORK/allow.high"
  for pair in "stale:stale entry" "low:more than allowed" "high:fewer than" "noreason:bad line" "none:offender"; do
    bad=${pair%%:*}
    word=${pair#*:}
    if env ONE_OWNER_OBJECTS= sh "$0" --no-canary --srcdir "$fx" --allow "$WORK/allow.$bad" > "$WORK/run.$bad" 2>&1; then
      echo "check-read-owner: FAIL (selftest: an allow file with a $bad row passed)"
      exit $BAD
    fi
    if ! grep -q "$word" "$WORK/run.$bad"; then
      echo "check-read-owner: FAIL (selftest: an allow file with a $bad row failed, but not with \"$word\"; it said:)"
      cat "$WORK/run.$bad"
      exit $BAD
    fi
  done
  echo "check-read-owner: selftest ok"
  exit 0
fi

# the symbols (clause k)
: > "$WORK/symfound"
if command -v "$NMTOOL" >/dev/null 2>&1 && [ -n "$OBJECTS" ]; then
  res=$(wpe_object_of we_read.o "$OBJECTS")
  case $res in
    unknown) problem "object list has no we_read.o (the scan is blind)" ;;
    "not-built "*) echo "${res#not-built }" >> "$WORK/notbuilt" ;;
  esac
  # shellcheck disable=SC2046
  scan_symbols $(listed_objects) > "$WORK/symfound"
fi

# the source
scan_tree "$SRCDIR" > "$WORK/srcfound"
{
  awk '{ print $2 "|" $1 ":" $3 "|" $4 }' "$WORK/srcfound"
  awk '{ print $2 "|" $1 ":" $3 "|" $4 }' "$WORK/symfound"
} | sort > "$WORK/found"

if [ $CANARY -eq 1 ]; then
  [ -f "$SRCDIR/we_read.c" ] || problem "canary: we_read.c not found under $SRCDIR"
  sh "$0" --selftest > /dev/null 2>&1 || problem "canary: the scan is blind (run $0 --selftest)"
fi

if [ $LIST -eq 1 ]; then
  awk -F'|' '{ print $1 "|" $2 "|" $3 "|PASS|reason" }' "$WORK/found"
  exit 0
fi

: > "$WORK/allowed"
if [ -f "$ALLOW" ]; then
  awk -F'|' '
    /^[ \t]*(#|$)/ { next }
    {
      if (NF < 5 || $3 !~ /^[0-9]+$/ || $4 == "" || $5 == "" || $5 ~ /^[ \t]+$/) { print "bad line " NR " in the allow file: " $0 > "/dev/stderr"; bad = 1; next }
      key = $1 "|" $2
      if (key in seen) { print "duplicate allow line for " key > "/dev/stderr"; bad = 1; next }
      seen[key] = 1
      print $1 "|" $2 "|" $3 "|" $4
    }
    END { exit bad }' "$ALLOW" > "$WORK/allowed" ||
    problem "the allow file $ALLOW has malformed or duplicate lines"
else
  problem "no allow file at $ALLOW"
fi

# The objects of the allow file's symbol rows that this build does not make
# (an object of a feature configure left out): such a row finds nothing and is
# not stale.  With no objects read at all, no symbol row can be judged.
: > "$WORK/notbuilt_objs"
kscan=0
if [ -n "$OBJECTS" ] && command -v "$NMTOOL" >/dev/null 2>&1; then
  kscan=1
  for ob in $(awk -F'|' '$2 ~ /^r22k:/ { print $1 }' "$WORK/allowed" | sort -u); do
    res=$(wpe_object_of "$ob" "$OBJECTS")
    case $res in
      "not-built "*) echo "$ob" >> "$WORK/notbuilt_objs"; echo "${res#not-built }" >> "$WORK/notbuilt" ;;
    esac
  done
fi

awk -F'|' -v kscan=$kscan -v nbobjs=" $(tr '\n' ' ' < "$WORK/notbuilt_objs")" '
  FILENAME == ARGV[1] { al[$1 "|" $2] = $3; pass[$1 "|" $2] = $4; next }
  {
    key = $1 "|" $2; got[key] = $3
    rule = $2; sub(/:.*/, "", rule)
    if (!(key in al)) {
      if (rule == "r22a" || rule == "r22d")
        print "offender: " $1 " " $2 ": a file is opened for reading by name with fopen or fdopen.  A pipe or a terminal under the name waits or takes the keys, a device fills the memory: read it through wpe_read_stream_open or wpe_read_open (we_read.h), or list the function with its pass"
      else if (rule == "r22b")
        print "offender: " $1 " " $2 ": a file is opened for reading by name with open or openat, or a /dev/ literal other than /dev/null and /dev/tty is opened.  Read it through wpe_read_open, wpe_read_text or wpe_read_stream_open, or list the function with its pass"
      else if (rule == "r22c")
        print "offender: " $1 " " $2 ": a folder is listed or walked outside the owner (opendir, scandir, nftw, ftw, fts_open, glob, or fdopendir outside we_fileop.c): list it through wpe_read_folder, or list the function with its pass"
      else if (rule == "r22e")
        print "offender: " $1 " " $2 ": a library opens a file by name (json_object_from_file, json_object_from_fd, gzopen, XrmGetFileDatabase): read the bytes with wpe_read_text and hand them to the parser, or list the function with its pass"
      else if (rule == "r22f")
        print "offender: " $1 " " $2 ": getline or getdelim takes a line of any length.  Read it over the owner'"'"'s stream, whose bytes are bounded, or list the function with the reason its stream is"
      else if (rule == "r22g")
        print "offender: " $1 " " $2 ": a read request is built by hand or by number (a WpeReadAsk filled in, a member assigned, a bound written as a digit), or it holds no bound (WPE_READ_UNBOUNDED, WPE_READ_ASK_SCAN, WPE_READ_ASK_LOAD) in a function the allow file does not list with its reason.  Use a WPE_READ_ASK_* form"
      else if (rule == "r22n")
        print "offender: " $1 " " $2 ": a File Manager listing function looks at an entry by a path (stat, lstat, access or a spelling of them).  The look of an entry is asked of the folder that was read, through wpe_read_entry_look or wpe_read_entry_look_target, so a name never lands on the working folder'"'"'s file of the same name.  No allow line"
      else if (rule == "r22p")
        print "offender: " $1 " " $2 ": WPE_READ_ASK_PROJECT or WPE_READ_ASK_PROJECT_SEEN is named outside e_prj_read_open (and its two steps), e_project_open_error and e_select_project.  The open project is read by one opener, which asks the read owner for the file the editor holds: call e_prj_read_open.  No allow line"
      else if (rule == "r22q")
        print "offender: " $1 " " $2 ": a read owner face is given e_prog.project or e_prj_open_path( as its path.  The open project is read by e_prj_read_open, which asks for the file the editor holds and asks the user when the name leads to another.  No allow line"
      else if (rule == "r22s")
        print "offender: " $1 " " $2 ": the window opened on a file takes its mode and identity by a second look at the file (a name or a descriptor) instead of from the outcome of the read.  The window must describe the very object its text came from: use the mode and the seen of the WpeReadOutcome.  No allow line"
      else if (rule == "r22h")
        print "offender: " $1 " " $2 ": WPE_READ_KIND_PIPE is named outside the owner.  Only the load of a pipe may accept one, and it is not built yet.  No allow line"
      else if (rule == "r22i")
        print "offender: " $1 " " $2 ": a sentence word is spelt in we_read.c (Cannot, not found, it is, larger).  The words of a refusal are we_path.c'"'"'s: map the outcome onto them.  No allow line"
      else if (rule == "r22j")
        print "offender: " $1 " " $2 ": a memory stream made outside read_stream_of_hold of we_read.c, or one a reader can write to or that leaves the bytes with the caller (fmemopen anywhere, fopencookie with a mode other than \"r\", funopen with a write callback, tmpfile anywhere else).  The stream owns its bytes, so fclose frees them and a reader is given no byte the file does not hold.  No allow line"
      else if (rule == "r22l")
        print "offender: " $1 " " $2 ": an errno that one system names for another kind of file (ELOOP, EMLINK, EFTYPE, ENXIO, EOPNOTSUPP, ENOTSUP) in we_read.c.  Decide it by looking at the name again (read_open_failure).  No allow line"
      else if (rule == "r22m")
        print "offender: " $1 " " $2 ": open, fopen, freopen, open64, openat64 or creat in we_read.c, or openat outside read_target_open_fd.  The owner has one open.  No allow line"
      else if (rule == "r22k")
        print "offender: " $1 " " $2 ": the object imports a folder or library opener (opendir, scandir, nftw, ftw, fts_open, glob, json_object_from_file, gzopen) that only the objects of the allow file may.  List the object with its pass, or move the read onto we_read.c"
      else if (rule == "r22z")
        print "offender: " $1 " " $2 ": an open or openat token that is not a call with its arguments (a function pointer, a call in parentheses, a wrapper name, too few arguments) is not classified by any clause.  Call open or openat plainly, or list the function with its reason"
      else
        print "offender: " $1 " " $2 ": a file is read outside the owner (we_read.c): move it onto the owner, or list it with its pass"
      bad = 1
    } else if ($3 + 0 > al[key] + 0) {
      print "more than allowed: " $1 " " $2 " has " $3 ", the allow file says " al[key] " (" pass[key] ")"; bad = 1
    } else if ($3 + 0 < al[key] + 0) {
      print "fewer than the allow file says: " $1 " " $2 " has " $3 ", lower the line from " al[key] " (" pass[key] ")"; bad = 1
    }
  }
  END {
    for (k in al) {
      if (k in got) continue
      split(k, kp, "|")
      # a row for an object this build does not make, or one a configuration may lack
      if (kp[2] ~ /^r22k:/ && (!kscan || pass[k] ~ /\/cfg$/)) continue
      if (kp[2] ~ /^r22k:/ && index(nbobjs, " " kp[1] " ") > 0) continue
      print "stale entry, delete it: " k " (" pass[k] ")"; bad = 1
    }
    exit bad
  }' "$WORK/allowed" "$WORK/found" > "$WORK/report"
status=$?

if [ -s "$WORK/problems" ] || [ $status -ne 0 ]; then
  echo "check-read-owner: FAIL"
  cat "$WORK/problems" "$WORK/report"
  exit $BAD
fi
not_built_note=
[ -n "$OBJECTS" ] && not_built_note=$(wpe_not_built_note "$WORK/notbuilt")
echo "check-read-owner: ok ($(wc -l < "$WORK/found" | tr -d ' ') counted site(s), all in the allow file$not_built_note)"
exit 0
