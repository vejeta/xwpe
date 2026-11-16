#!/bin/sh
# check-write-owner.sh -- the user's text is written by one owner.
#
# When a write of the user's text fails the user must be told once, with the
# path and the reason, and the text must stay where it is.  That holds only
# while one piece of code (we_write.c) opens the file, judges the write and
# builds the sentence, and every caller reads the outcome it returns.  A
# second piece that opens a file for writing, drops the result of a save, or
# spells the old "Can't open File %s" is how a failed save went unseen, one
# site after another.
#
#   check-write-owner.sh [--allow FILE] [--srcdir DIR] [--nm NM] [--list]
#                        [--selftest] [OBJECT...]
#
# Rules (the numbers are the design's):
#  1. Symbols (nm -u of the editor's objects): e_write and e_eol_write are
#     undefined only in we_write.o.
#  2. Every way to open a file for writing outside we_write.c: fopen, freopen
#     and fdopen with a mode that starts with w or a; open and openat whose
#     flags name O_WRONLY, O_RDWR, O_CREAT or O_TRUNC; creat.  Comments are
#     stripped and a call split over lines is joined.
#  4. A statement that is a save (e_save, e_write, wpe_write_file, _range,
#     _save, _text, _fd, _stream_open, _stream_close, e_opt_lines_store) alone or under (void): its outcome is dropped.
#  5. A save compared with numbers: on the call (== / != WPE_ESC, 0 or
#     E_SAVE_DECLINED) or through the variable that took its result, and any
#     E_SAVE_DECLINED.  A caller tests WPE_WRITE_WRITTEN, DECLINED or FAILED.
#  6. The old messages: ERR_FOPEN, ERR_OPEN_OPF, the File Manager's ERR_OREADFILE,
#     ERR_OWRITEFILE, ERR_ALLOC_CBUF, ERR_INCONSCOPY, ERR_RENFILE,
#     ERR_LINKFILE and ERR_NOWASTE, and the literal "Can't open File" outside
#     we_write.c.
#  1b. Symbols again: the File Manager's old copy, link, wastebasket and
#     folder functions are defined nowhere (a T in nm), and the write
#     owner's exports for a copy (wpe_write_create_private, _copy_fd,
#     _set_special_bits, _set_mode_fd, _set_mode_path, the SIGXFSZ guard) are
#     undefined only in we_fileop.o.
#  8. A name of a file made, moved, linked or removed outside we_fileop.c
#     and we_write.c: rename, renameat, link, linkat, symlink, symlinkat,
#     unlink, unlinkat, remove, rmdir, mkdir, mkdirat, chmod, fchmod,
#     fchmodat, chown, fchown, lchown, readlink, readlinkat, renameat2,
#     renamex_np.  Counted per function, strings and comments blanked.
#  9. The byte loop of a copy: fread, fwrite and a while loop whose
#     condition is feof, outside we_write.c, we_fileop.c and we_lineend.c.
# 10. The File Manager's option word assigned (flopt =, |=, &=): the options
#     are the user's, so an operation keeps its own copy by value.
#  1c. we_fileop.o does not import opendir: the only way it lists a folder is
#     fdopendir, on a descriptor it opened by name.
# 11. In we_fileop.c a path is resolved at the boundary and nowhere else: the
#     calls that take a path (open, opendir, fopen, stat, lstat, access,
#     realpath, mkdir, rmdir, rename, unlink, link, symlink, readlink,
#     chmod, remove, chdir, truncate), the token AT_FDCWD (an *at call on
#     the working folder is a path call), and the owner's own entry points
#     that take a path (wpe_fileop_copy, _move, _link, _remove and their
#     _tree forms, wpe_fileop_wastebasket_slot, fileop_parent_open), counted
#     per function with strings and comments blanked; a function's own
#     definition is not a call.  A walk addresses every entry by a folder's
#     descriptor and its name, so a link met on the way is never followed.
#     The functions that may resolve a path are lines of the allow file.
# 12. In we_write.c and we_fileop.c the permissions of a file are changed in
#     one place: chmod, fchmod and fchmodat appear only inside write_mode_set
#     (we_write.c), under every face of the mode owner (wpe_write_set_mode_fd,
#     _path, _special_bits).  A call anywhere else is a site that meets a
#     refused special bit on its own.  Strings and comments are blanked.
# 13. The result of e_close_window is read.  A close can be refused (a text
#     window the user keeps, a project list the file refuses) and says so with
#     WPE_ESC; a statement that is e_close_window(...) alone, under (void) or
#     after a case label drops that, so a loop goes on as if the window were
#     gone.  The functions where the window cannot refuse are lines of the
#     allow file with their reason.
# 14. The open project is switched in one place.  A write of e_prog.project
#     (assignment, strcpy and the like, REALLOC, FREE) is in a function the
#     allow file names (e_select_project, e_project_name, e_prj_anchor_path,
#     e_ini_prog, WpeReadProgramming, e_open_project_file).
# 15. In we_write.c the window's recorded mode (`filemode`, what the window
#     read when it loaded the file) is read inside one function,
#     write_mode_before_data, only as `filemode & WRITE_NINE_BITS` or in a
#     sign test (`filemode >= 0`, `< 0`), and that function names no special
#     bit: not WRITE_TWELVE_BITS, WRITE_SPECIAL_BITS, WRITE_NINE_BITS
#     complemented, S_ISUID, S_ISGID, S_ISVTX, wpe_write_set_special_bits,
#     write_keep_special_bits, nor an octal constant of four digits or more
#     with a digit above 0 in front of the permission digits (01000 ... 07777).
#     The token `filemode` also counts on preprocessor lines, where a macro
#     could carry it anywhere.  So the special bits of a write come from the
#     file it opened, never from a mode recorded at load.
# 17. The special bits a File Manager copy is given are decided in one place.
#     Outside we_write.c, wpe_write_set_special_bits( is called only in
#     fileop_job_fill_temp, its second argument is the variable `keep`, `keep`
#     is assigned once, from fileop_bits_for_copy( (which compares the owner
#     and group of the copy with the original's), and never changed after.
#     Passing the original's mode, or that mode masked, gives a program of
#     another user to whoever copies it.
# Rule 3 (the companion names) and rule 7 (the crash handler's order) come
# with the pass that moves them.
#
# Offenders still to go are lines of tests/write-owner.allow:
#   file|rN:function|count|pass|reason
# The count may only fall, a line that matches nothing is stale and fails,
# and a line with a pass is deleted by that pass.  --list prints what was
# found as allow lines.  --selftest feeds a fixture with one of each
# offence to the scan and fails unless the scan names every one (a scanner
# that sees nothing is not a check).
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
NMTOOL=${NM:-nm}
OBJECTS=
BAD=1
. "$HERE/objsyms.sh"

while [ $# -gt 0 ]; do
  case $1 in
    --allow) shift; ALLOW=$1 ;;
    --srcdir) shift; SRCDIR=$1 ;;
    --nm) shift; NMTOOL=$1 ;;
    --list) LIST=1 ;;
    --selftest) SELFTEST=1 ;;
    -*) echo "check-write-owner: unknown argument $1" >&2; exit $BAD ;;
    *) OBJECTS="$OBJECTS $1" ;;
  esac
  shift
done
[ -n "$OBJECTS" ] || OBJECTS=$ONE_OWNER_OBJECTS
[ -n "$ALLOW" ] || ALLOW=$HERE/write-owner.allow

WORK=$(mktemp -d "${TMPDIR:-/tmp}/write-owner.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
: > "$WORK/problems"
problem() { echo "$*" >> "$WORK/problems"; }
: > "$WORK/notbuilt"       # one word per line, for what this build lacks

# What this build contains (tests/build-config.sh asks the binary once): an
# object of a feature the build lacks is not built, which is not the same as
# a scan that cannot see it.  Sourcing prints the build's line.
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

# require_owner_object NAME - the two owners this scan judges every other
# object against.  One that is not among the objects of this run means the
# list is wrong and the scan is blind; one of a feature this build lacks is
# named as not built here.
require_owner_object() {
  res=$(wpe_object_of "$1" "$OBJECTS")
  case $res in
    unknown)
      problem "object list has no $1 (the scan is blind)" ;;
    "not-built "*)
      echo "check-write-owner: skip $1: $(wpe_not_built_reason "${res#not-built }")" >&2
      echo "${res#not-built }" >> "$WORK/notbuilt" ;;
  esac
}

# scan FILE - one line "rule file function count" for each offence of the
# rules 2, 4, 5 and 6 in FILE.  Comments are blanked, string literals kept;
# a function starts at a line in column 0 that names an identifier followed
# by "(" and does not end with ";" (a prototype); a statement is the text up
# to the next ; { or }.
scan() {
  awk -v file="$(basename "$1")" "$BLANK_C"'
    function ident_end(s, i,   n) { n = length(s); while (i <= n && substr(s, i, 1) ~ /[A-Za-z0-9_]/) i++; return i }
    # the index just past the ")" that closes the "(" at position p
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
    function note(rule, n) { cnt[rule] += n }
    function flush_fn(   r) {
      if (fn == "") return
      for (r in cnt) if (cnt[r] > 0) print r, file, fn, cnt[r]
      delete cnt; delete taken; bound17 = 0
    }
    # rule 17 for the code of one statement, strings blanked.  The set-ID bits
    # a copy is given are the value fileop_bits_for_copy returned, held in
    # `keep` and never changed: the call that puts them on is in
    # fileop_job_fill_temp and its second argument is that `keep`.  The
    # definition (we_write.c) is no call.
    function note17(m,   t, rest, arg) {
      t = m
      if (fn != "fileop_job_fill_temp") { }
      else if (match(t, /(^|[^A-Za-z0-9_.>])keep[ \t]*=[ \t]*fileop_bits_for_copy[ \t]*[(]/)) bound17 = 1
      else if (match(t, /(^|[^A-Za-z0-9_.>])keep[ \t]*(\|=|&=|\^=|<<=|>>=|=[^=])/)) note("r17", 1)
      t = m
      while (fn == "fileop_job_fill_temp" && match(t, /(^|[^A-Za-z0-9_.>])wpe_write_set_mode_fd[ \t]*[(]/)) {
        note("r17", 1); t = substr(t, RSTART + RLENGTH)
      }
      t = m
      while (match(t, /(^|[^A-Za-z0-9_.>])wpe_write_set_special_bits[ \t]*[(]/)) {
        rest = substr(t, RSTART + RLENGTH)
        arg = rest; sub(/^[^,]*,[ \t]*/, "", arg); sub(/[ \t]*[,)].*/, "", arg)
        if (fn != "fileop_job_fill_temp" || arg != "keep" || !bound17) note("r17", 1)
        t = rest
      }
    }
    function prev_char(s, p,   i, c) {
      for (i = p - 1; i >= 1; i--) { c = substr(s, i, 1); if (c != " " && c != "\t") return c }
      return ""
    }
    function prev_word_else(s, p,   head) {
      head = substr(s, 1, p - 1); sub(/[ \t]+$/, "", head)
      return head ~ /(^|[^A-Za-z0-9_])else$/
    }
    # rule 15 for the code of one statement, strings blanked.  Outside
    # write_mode_before_data every `filemode` counts.  Inside it a `filemode`
    # counts unless it is masked with WRITE_NINE_BITS or tested against 0, and
    # every token of a special bit counts.
    function note15(m,   t, rest) {
      t = m
      # the member declaration of the window is no read of it
      gsub(/(^|[^A-Za-z0-9_])int[ \t]+filemode([^A-Za-z0-9_]|$)/, " ", t)
      while (match(t, /(^|[^A-Za-z0-9_])filemode([^A-Za-z0-9_]|$)/)) {
        rest = substr(t, RSTART + RLENGTH - 1)
        if (fn != "write_mode_before_data") note("r15", 1)
        else if (rest !~ /^[ \t]*&[ \t]*WRITE_NINE_BITS([^A-Za-z0-9_]|$)/ && rest !~ /^[ \t]*(>=|<)[ \t]*0([^A-Za-z0-9_]|$)/) note("r15", 1)
        t = substr(t, RSTART + RLENGTH - 1)
      }
      if (fn != "write_mode_before_data") return
      t = m
      while (match(t, /(^|[^A-Za-z0-9_])(WRITE_TWELVE_BITS|WRITE_SPECIAL_BITS|S_ISUID|S_ISGID|S_ISVTX|wpe_write_set_special_bits|write_keep_special_bits)([^A-Za-z0-9_]|$)/)) { note("r15", 1); t = substr(t, RSTART + RLENGTH - 1) }
      t = m
      while (match(t, /~[ \t]*[(]?[ \t]*WRITE_NINE_BITS/)) { note("r15", 1); t = substr(t, RSTART + RLENGTH) }
      t = m
      while (match(t, /(^|[^A-Za-z0-9_])0+[1-7][0-7][0-7][0-7]([^A-Za-z0-9_]|$)/)) { note("r15", 1); t = substr(t, RSTART + RLENGTH - 1) }
      # the same bits spelled in hex or decimal, or built by a shift: the
      # owner of the nine bits needs no number but the mask
      t = m
      while (match(t, /(^|[^A-Za-z0-9_])0[xX][0-9A-Fa-f]+/)) { note("r15", 1); t = substr(t, RSTART + RLENGTH) }
      t = m
      while (match(t, /(^|[^A-Za-z0-9_.])[1-9][0-9][0-9]+([^A-Za-z0-9_]|$)/)) { note("r15", 1); t = substr(t, RSTART + RLENGTH - 1) }
      t = m
      while (match(t, /<</)) { note("r15", 1); t = substr(t, RSTART + RLENGTH) }
    }
    function statement(s,   t, p, q, e, rest, name, v, c, m, ident) {
      # rule 2: every way to open a file for writing
      t = s
      while (match(t, /(^|[^A-Za-z0-9_])(fopen|freopen|fdopen)[ \t]*[(]/)) {
        p = RSTART + RLENGTH - 1; e = close_paren(t, p)
        if (e > 0 && substr(t, p, e - p) ~ /"[wa]/) note("r2", 1)
        t = substr(t, p + 1)
      }
      t = s
      while (match(t, /(^|[^A-Za-z0-9_])(open|openat)[ \t]*[(]/)) {
        p = RSTART + RLENGTH - 1; e = close_paren(t, p)
        if (e > 0 && substr(t, p, e - p) ~ /(O_WRONLY|O_RDWR|O_CREAT|O_TRUNC)/) note("r2", 1)
        t = substr(t, p + 1)
      }
      t = s
      while (match(t, /(^|[^A-Za-z0-9_])creat[ \t]*[(]/)) {
        note("r2", 1)
        t = substr(t, RSTART + RLENGTH)
      }
      # rules 4 and 5: the saves
      t = s; gsub(/\([ \t]*void[ \t]*\)/, " ", t)
      while (match(t, /(^|[^A-Za-z0-9_])(e_save|e_write|wpe_write_file|wpe_write_range|wpe_write_save|wpe_write_text|wpe_write_fd|wpe_write_stream_open|wpe_write_stream_close|e_opt_lines_store|wpe_fileop_(copy|move|link|remove|mkdir|rmdir|chmod|rename_noreplace|wastebasket_slot|wastebasket))[ \t]*[(]/)) {
        name = substr(t, RSTART, RLENGTH); sub(/^[^A-Za-z]/, "", name); sub(/[ \t]*[(]$/, "", name)
        q = RSTART; p = RSTART + RLENGTH - 1; e = close_paren(t, p)
        np = q
        if (substr(t, q, 1) !~ /[A-Za-z_]/) np = q + 1
        if (e > 0) {
          rest = substr(t, e)
          c = prev_char(t, np)
          if (rest ~ /^[ \t]*;/ && (c == "" || c == ";" || c == "{" || c == "}" || c == ")" || prev_word_else(t, np))) note("r4", 1)
          # the int returns of the owner (0 or -1) may be compared with 0; the
          # saves that return a status or a key are read by name
          status_call = (name ~ /^(e_save|e_write|wpe_write_save)$/)
          if (status_call && rest ~ /^[ \t]*(==|!=)[ \t]*(WPE_ESC|E_SAVE_DECLINED|0)([^A-Za-z0-9_]|$)/) note("r5", 1)
          head = substr(t, 1, q); if (status_call && head ~ /[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*[(]?[ \t]*$/ && head !~ /(==|!=|<=|>=)[ \t]*[(]?[ \t]*$/) {
            v = head; sub(/[ \t]*=[ \t]*[(]?[ \t]*$/, "", v); sub(/.*[^A-Za-z0-9_]/, "", v)
            if (v != "") taken[v] = 1
          }
        }
        t = substr(t, p + 1)
      }
      for (v in taken)
        if (s ~ ("(^|[^A-Za-z0-9_])" v "[ \t]*(==|!=)[ \t]*(WPE_ESC|E_SAVE_DECLINED|0)([^A-Za-z0-9_]|$)")) note("r5", 1)
      m = s
      while (match(m, /(^|[^A-Za-z0-9_])E_SAVE_DECLINED([^A-Za-z0-9_]|$)/)) { note("r5", 1); m = substr(m, RSTART + RLENGTH) }
      # rule 6: the old message
      m = s
      while (match(m, /(^|[^A-Za-z0-9_])ERR_(FOPEN|OPEN_OPF|OREADFILE|OWRITEFILE|ALLOC_CBUF|INCONSCOPY|RENFILE|LINKFILE|NOWASTE)([^A-Za-z0-9_]|$)/)) { note("r6", 1); m = substr(m, RSTART + RLENGTH) }
      m = s
      while (match(m, /Can[\\\047]*t open File/)) { note("r6", 1); m = substr(m, RSTART + RLENGTH) }
      # rules 8, 9 and 10 look at code: the strings are blanked
      m = s; gsub(/"([^"\\]|\\.)*"/, "\"\"", m)
      # rule 8: a name of the user'"'"'s files made, moved or removed outside the owners
      t = m
      while (match(t, /(^|[^A-Za-z0-9_.>])(rename|renameat|renameat2|renamex_np|renameatx_np|link|linkat|symlink|symlinkat|unlink|unlinkat|remove|rmdir|mkdir|mkdirat|chmod|fchmod|fchmodat|chown|fchown|lchown|readlink|readlinkat)[ \t]*[(]/)) {
        note("r8", 1); t = substr(t, RSTART + RLENGTH)
      }
      # rule 11 (we_fileop.c only): a path is resolved at the boundary
      t = m
      while (match(t, /(^|[^A-Za-z0-9_.>])(open|opendir|fopen|stat|lstat|access|realpath|mkdir|rmdir|rename|unlink|link|symlink|readlink|chmod|remove|chdir|truncate)[ \t]*[(]/)) {
        note("r11", 1); t = substr(t, RSTART + RLENGTH)
      }
      t = m
      while (match(t, /(^|[^A-Za-z0-9_])AT_FDCWD([^A-Za-z0-9_]|$)/)) { note("r11", 1); t = substr(t, RSTART + RLENGTH) }
      t = m
      while (match(t, /(^|[^A-Za-z0-9_.>])(wpe_fileop_(copy|move|link|remove)(_tree)?|wpe_fileop_wastebasket_slot|fileop_parent_open)[ \t]*[(]/)) {
        ident = substr(t, RSTART, RLENGTH); sub(/^[^A-Za-z]/, "", ident); sub(/[ \t]*[(]$/, "", ident)
        if (ident != fn) note("r11", 1)
        t = substr(t, RSTART + RLENGTH)
      }
      # rule 12 (we_write.c and we_fileop.c): the permissions of a file are
      # changed in one place, write_mode_set
      t = m
      while (match(t, /(^|[^A-Za-z0-9_.>])(chmod|fchmod|fchmodat)[ \t]*[(]/)) {
        if (fn != "write_mode_set") note("r12", 1)
        t = substr(t, RSTART + RLENGTH)
      }
      # rule 17 (every file but we_write.c): the set-ID bits of a copy are
      # the ones fileop_bits_for_copy returned
      if (file != "we_write.c") note17(m)
      # rule 15 (we_write.c only): the recorded mode of a window is read in one
      # function, as its nine bits, and that function names no special bit
      note15(m)
      # rule 9: the byte loop of a copy
      t = m
      while (match(t, /(^|[^A-Za-z0-9_])(fread|fwrite)[ \t]*[(]/)) { note("r9", 1); t = substr(t, RSTART + RLENGTH) }
      t = m
      while (match(t, /(^|[^A-Za-z0-9_])while[ \t]*[(][^;{]*feof[ \t]*[(]/)) { note("r9", 1); t = substr(t, RSTART + RLENGTH) }
      # rule 10: the File Manager'"'"'s options changed by assignment
      t = m
      while (match(t, /flopt[ \t]*(\|=|&=|=[^=])/)) { note("r10", 1); t = substr(t, RSTART + RLENGTH) }
      # rule 13: a window close that can be refused (the project window, a
      # text window the user keeps) is read; a case label ends with a colon
      # and the statement after it starts there
      t = m; gsub(/\([ \t]*void[ \t]*\)/, " ", t)
      # a character constant is a number here (case 0x71 and a colon inside
      # one must not look like labels), then every label colon (case X:,
      # default:, again:) becomes a statement end; a ?: colon is not at a
      # statement start and stays
      gsub(/\047([^\047\\]|\\.)*\047/, "0", t)
      while (match(t, /(^|[;{}])[ \t]*(case[^:;{}?]*|default|[A-Za-z_][A-Za-z0-9_]*)[ \t]*:/))
        t = substr(t, 1, RSTART + RLENGTH - 2) ";" substr(t, RSTART + RLENGTH)
      while (match(t, /(^|[^A-Za-z0-9_])e_close_window[ \t]*[(]/)) {
        q = RSTART; p = RSTART + RLENGTH - 1; e = close_paren(t, p)
        np = q
        if (substr(t, q, 1) !~ /[A-Za-z_]/) np = q + 1
        if (e > 0) {
          rest = substr(t, e); c = prev_char(t, np)
          if (rest ~ /^[ \t]*;/ && (c == "" || c == ";" || c == "{" || c == "}" || c == ")" || prev_word_else(t, np))) note("r13", 1)
          # the third clause of a for, which drops its value
          else if ((c == "" || c == ";") && rest ~ /^[ \t]*[,)]/) note("r13", 1)
        }
        t = substr(t, p + 1)
      }
      # rule 14: the open project is switched in one place; any write of
      # e_prog.project, its release included, is a named function
      t = m
      while (match(t, /e_prog[ \t]*\.[ \t]*project[ \t]*(\[[^]]*\][ \t]*)?=[^=]/)) { note("r14", 1); t = substr(t, RSTART + RLENGTH) }
      t = m
      while (match(t, /(strcpy|strncpy|strcat|memcpy|snprintf|sprintf|wpe_path_copy|wpe_path_join|wpe_path_cat|REALLOC|realloc|FREE|free|WpeFree)[ \t]*[(][ \t]*e_prog[ \t]*\.[ \t]*project[ \t]*[,)]/)) { note("r14", 1); t = substr(t, RSTART + RLENGTH) }
    }
    BEGIN { blank_incom = 0; fn = "-"; stmt = "" }
    {
      out = blank_c($0, 1)
      if (out ~ /^#/) {
        # rule 15: a macro can carry the window'"'"'s mode anywhere, so the
        # token counts on a preprocessor line, as a site of its own
        pp = out; gsub(/"([^"\\]|\\.)*"/, "\"\"", pp)
        while (match(pp, /(^|[^A-Za-z0-9_])filemode([^A-Za-z0-9_]|$)/)) { pp15++; pp = substr(pp, RSTART + RLENGTH - 1) }
        next
      }
      # a table of strings at file scope is named by its array, not by the
      # function above it
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
    END { statement(stmt); flush_fn(); if (pp15 > 0) print "r15", file, "(preprocessor)", pp15 }' "$1"
}

# scan_header_mode FILE - rule 15 for a header: every `filemode` of a header
# is a site, whatever carries it (a macro, one split by a backslash, a static
# inline function), except the member declaration of the window (`int
# filemode`), which scan leaves out.
scan_header_mode() {
  scan "$1" | awk '$1 == "r15"'
}

# --selftest: each rule must name its offence in a fixture.
if [ $SELFTEST -eq 1 ]; then
  cat > "$WORK/we_fake.c" <<'EOF'
/* one of each offence, and what is no offence */
int fake_open(FENSTER *f)
{
 FILE *fp = fopen("/tmp/x", "w");
 int fd = open("/tmp/y",
               O_WRONLY | O_CREAT, 0600);
 FILE *in = fopen("/tmp/z", "r");
 int rd = open("/tmp/z", O_RDONLY);
 return(fd);
}

int fake_dropped(FENSTER *f)
{
 e_save(f);
 (void)e_save(f);
 if (f->save)
  e_save(f);
 wpe_write_range(f, "p",
                 0, 0, 1, 1, 0, &o);
 wpe_fileop_copy(a, b, &p, &o);
 wpe_write_stream_close(fp, &o);
 e_opt_lines_store(path, &lines, &o);
 return(e_save(f));
}

int fake_numbers(FENSTER *f)
{
 int ret;

 if (e_save(f) == WPE_ESC)
  return(1);
 ret = e_save(f);
 if (ret == WPE_ESC)
  return(2);
 if (ret == WPE_WRITE_FAILED)
  return(3);
 return(0);
}

int fake_message(FENSTER *f)
{
 e_error(e_msg[ERR_FOPEN], 0, f->fb);
 e_error(e_msg[ERR_INCONSCOPY], 0, f->fb);
 e_error(e_msg[ERR_OPEN_OPF], 0, f->fb);
 return(E_SAVE_DECLINED);
}

int fake_names(char *a, char *b)
{
 char *say = "remove( is in a string, rename( too";
 int fine = e_rename(a, b, 0) + f->unlink(a) + wpe_fileop_remove(a, 0, 0);
 rename(a, b);
 unlink(a);
 return(mkdir(b, 0700));
}

int fake_loop(FILE *fp, FILE *to)
{
 char buf[10];
 int n;

 while (!feof(fp))
 {
  n = fread(buf, 1, sizeof buf, fp);
 }
 return(n);
}

int fake_options(FENSTER *f)
{
 f->ed->flopt = 1;
 f->ed->flopt &= ~2;
 if (f->ed->flopt == 3)
  return(1);
 return(0);
}

int fake_walk(int dfd, char *s, char *x)
{
 DIR *d = opendir(s);
 char *say = "stat( and unlinkat(AT_FDCWD, and opendir( in a string";
 /* opendir(x) and stat(x, &st) in a comment */
 int fine = j->open(x) + t.stat(x) + fstatat(dfd, x, &st, 0);
 DIR *e = fdopendir(dfd);
 int fd = openat(dfd, x, O_RDONLY) + mkdirat(dfd, x, 0700);
 stat(x, &st);
 return(unlinkat(AT_FDCWD, s, 0));
}

int wpe_fileop_copy(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o)
{
 return(0);
}

int fake_entry(WalkLevel *ent)
{
 int rc = wpe_fileop_copy(ent->spath, ent->dpath, t->p, &e);
 return(rc + fileop_parent_open(a, &fd, n, 9));
}

int fake_mode(int fd)
{
 char *say = "fchmod( in a string";
 /* fchmod(fd, 0600) in a comment */
 return(fchmod(fd, 02755));
}

static int write_mode_set(const WriteModeTarget *t, mode_t mode)
{
 return(t->path != 0 ? chmod(t->path, mode) : fchmod(t->fd, mode));
}

int fake_close(FENSTER *f, int c)
{
 e_close_window(f);
 (void)e_close_window(f);
 switch (c)
 {
 case WPE_ACT_CLOSE_WINDOW: e_close_window(f);
 case 'q': e_close_window(f);
 case ':': e_close_window(f);
 default: e_close_window(f);
 }
 again: e_close_window(f);
 for (c = 0; c < 2; e_close_window(f), c++)
  c = c;
 c = c ? e_close_window(f) : 0;
 c ? c : e_close_window(f);
 if (e_close_window(f) == WPE_ESC)
  return(1);
 c = e_close_window(f);
 return(e_close_window(f));
}

int fake_project(char *p)
{
 e_prog.project = WpeStrdup(p);
 e_prog.project[0] = 0;
 strcpy(e_prog.project, p);
 e_prog.project = REALLOC(e_prog.project, 9);
 FREE(e_prog.project);
 wpe_path_copy(e_prog.project, 99, p);
 if (e_prog.project == 0)
  return(1);
 p = e_prog.project;
 return(access(e_prog.project, 0));
}

int fake_save(FENSTER *f, int fd)
{
 WpeModeKept k;
 return(wpe_write_set_mode_fd(fd, f->filemode & WRITE_TWELVE_BITS, &k));
}

#define WINDOW_MODE(f) ((f)->filemode)
EOF
  # rule 15: the owner of the window's mode, three ways to leave its rule
  cat > "$WORK/we_fake_r15a.c" <<'EOF'
static void write_mode_before_data(const WriteTarget *t, FENSTER *f)
{
 if (f->filemode >= 0)
  write_mode_set(t, f->filemode);
}
EOF
  cat > "$WORK/we_fake_r15b.c" <<'EOF'
static void write_mode_before_data(const WriteTarget *t, FENSTER *f)
{
 if (f->filemode >= 0)
  write_mode_set(t, (f->filemode & WRITE_NINE_BITS) | S_ISGID | 04000 | 01777);
}
EOF
  cat > "$WORK/we_fake_r15c.c" <<'EOF'
static void write_mode_before_data(const WriteTarget *t, FENSTER *f)
{
 WpeModeKept kept;

 if (f->filemode >= 0)
  (void)wpe_write_set_mode_fd(t->fd, (mode_t)f->filemode & WRITE_NINE_BITS, &kept);
 if (f->filemode < 0)
  return;
 write_mode_set(t, 0755);
}
EOF
  cat > "$WORK/we_fake_r15d.c" <<'EOF'
static void write_mode_before_data(const WriteTarget *t, FENSTER *f)
{
 write_mode_set(t, (f->filemode & WRITE_NINE_BITS) | 0xC00 | 1024 | (1 << 9));
}
EOF
  # rule 17: the original's mode given to a copy, that mode masked, a
  # `keep` changed after the owner returned it, and the right shape
  cat > "$WORK/we_fake_r17a.c" <<'EOF'
static int fileop_job_fill_temp(FileJob *j, int sfd, int fd, int durable)
{
 err = wpe_write_set_special_bits(fd, j->sst.st_mode, &j->kept);
 return(err);
}
EOF
  cat > "$WORK/we_fake_r17b.c" <<'EOF'
static int fileop_job_fill_temp(FileJob *j, int sfd, int fd, int durable)
{
 mode_t keep = j->sst.st_mode & 07777;

 err = wpe_write_set_special_bits(fd, keep, &j->kept);
 return(err);
}
EOF
  cat > "$WORK/we_fake_r17c.c" <<'EOF'
static int fileop_job_fill_temp(FileJob *j, int sfd, int fd, int durable)
{
 mode_t keep;

 keep = fileop_bits_for_copy(j, fd, &dropped);
 keep |= S_ISUID;
 err = wpe_write_set_special_bits(fd, keep, &j->kept);
 return(err);
}
static int fileop_stash(FileJob *j, int fd)
{
 return(wpe_write_set_special_bits(fd, j->sst.st_mode, &j->kept));
}
EOF
  cat > "$WORK/we_fake_r17d.c" <<'EOF'
static int fileop_job_fill_temp(FileJob *j, int sfd, int fd, int durable)
{
 mode_t keep;

 keep = fileop_bits_for_copy(j, fd, &dropped);
 err = wpe_write_set_mode_fd(fd, j->sst.st_mode, &j->kept);
 err = wpe_write_set_special_bits(fd, keep, &j->kept);
 return(err);
}
EOF
  cat > "$WORK/we_fake_r17ok.c" <<'EOF'
static int fileop_job_fill_temp(FileJob *j, int sfd, int fd, int durable)
{
 mode_t keep;

 keep = fileop_bits_for_copy(j, fd, &dropped);
 err = wpe_write_sync_before_bits(fd, keep, durable);
 err = wpe_write_set_special_bits(fd, keep, &j->kept);
 return(err);
}
EOF
  cat > "$WORK/we_fake.h" <<'EOF'
typedef struct {
 int filemode, flg;
} FakeWindow;
#define WPE_WINDOW_MODE(f) ((f)->filemode)
EOF
  scan "$WORK/we_fake.c" | sort > "$WORK/fake.found"
  scan "$WORK/we_fake_r15d.c" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r17a.c" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r17b.c" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r17c.c" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r17d.c" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r17ok.c" >> "$WORK/fake.found"
  scan_header_mode "$WORK/we_fake.h" >> "$WORK/fake.found"
  # the other two shapes a header can carry the mode in: a macro split by a
  # backslash, and a static inline function
  cat > "$WORK/we_fake2.h" <<'EOF'
#define WPE_WINDOW_MODE(f) \
 ((f)->filemode)
EOF
  cat > "$WORK/we_fake3.h" <<'EOF'
static inline int window_mode(FENSTER *f)
{
 return(f->filemode);
}
EOF
  scan_header_mode "$WORK/we_fake2.h" >> "$WORK/fake.found"
  scan_header_mode "$WORK/we_fake3.h" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r15a.c" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r15b.c" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r15c.c" >> "$WORK/fake.found"
  for want in "r2 we_fake.c fake_open 2" "r4 we_fake.c fake_dropped 7" \
              "r5 we_fake.c fake_numbers 2" "r5 we_fake.c fake_message 1" \
              "r6 we_fake.c fake_message 3" "r8 we_fake.c fake_names 3" \
              "r9 we_fake.c fake_loop 2" "r10 we_fake.c fake_options 2" \
              "r11 we_fake.c fake_walk 3" "r11 we_fake.c fake_entry 2" \
              "r12 we_fake.c fake_mode 1" "r13 we_fake.c fake_close 8" \
              "r14 we_fake.c fake_project 7" \
              "r17 we_fake_r17a.c fileop_job_fill_temp 1" \
              "r17 we_fake_r17b.c fileop_job_fill_temp 2" \
              "r17 we_fake_r17c.c fileop_job_fill_temp 1" \
              "r17 we_fake_r17c.c fileop_stash 1" \
              "r17 we_fake_r17d.c fileop_job_fill_temp 1" \
              "r15 we_fake.c fake_save 1" "r15 we_fake.c (preprocessor) 1" \
              "r15 we_fake_r15a.c write_mode_before_data 1" \
              "r15 we_fake_r15b.c write_mode_before_data 3" \
              "r15 we_fake_r15d.c write_mode_before_data 3" \
              "r15 we_fake.h (preprocessor) 1" \
              "r15 we_fake2.h - 1" "r15 we_fake3.h window_mode 1"; do
    grep -qx "$want" "$WORK/fake.found" || {
      echo "check-write-owner: FAIL (selftest: the scan does not name \"$want\"; it found:)"
      cat "$WORK/fake.found"
      exit $BAD
    }
  done
  # the right shape of the copy's mode is no offence
  if grep -q "^r17 we_fake_r17ok.c " "$WORK/fake.found"; then
    echo "check-write-owner: FAIL (selftest: the mode fileop_bits_for_copy returned was counted; it found:)"
    cat "$WORK/fake.found"
    exit $BAD
  fi
  # a function's own definition is not a call of it
  if grep -q "^r11 we_fake.c wpe_fileop_copy " "$WORK/fake.found"; then
    echo "check-write-owner: FAIL (selftest: a definition was counted as a path call; it found:)"
    cat "$WORK/fake.found"
    exit $BAD
  fi
  # write_mode_set is the one place a chmod may stand
  if grep -q "^r12 we_fake.c write_mode_set " "$WORK/fake.found"; then
    echo "check-write-owner: FAIL (selftest: the chmod inside write_mode_set was counted; it found:)"
    cat "$WORK/fake.found"
    exit $BAD
  fi
  # the member declaration in a header is no site: the fixture's struct has
  # one beside its macro, and only the macro is named
  if [ "$(grep -c "^r15 we_fake.h " "$WORK/fake.found")" != 1 ]; then
    echo "check-write-owner: FAIL (selftest: the member declaration of a header was counted; it found:)"
    cat "$WORK/fake.found"
    exit $BAD
  fi
  # the owner of the window's mode, reading it as its nine bits and a sign
  # test and naming no special bit, is no offender (the 0755 there is not one)
  if grep -q "^r15 we_fake_r15c.c " "$WORK/fake.found"; then
    echo "check-write-owner: FAIL (selftest: the masked read in write_mode_before_data was counted; it found:)"
    cat "$WORK/fake.found"
    exit $BAD
  fi
  echo "check-write-owner: selftest ok"
  exit 0
fi

# Rule 1: the symbols.
: > "$WORK/symfound"
if command -v "$NMTOOL" >/dev/null 2>&1 && [ -n "$OBJECTS" ]; then
  require_owner_object we_write.o
  require_owner_object we_fileop.o
  for o in $(listed_objects); do
    base=$(basename "$o")
    # what the file operations owner may call of the write owner, and what
    # no object defines any more (the File Manager's old copy loop and the
    # folder code that P5b replaces: their allow lines say so)
    [ "$base" = we_write.o ] || [ "$base" = we_fileop.o ] ||
    "$NMTOOL" -u "$o" 2>/dev/null | nm_undefined_source_names | awk -v obj="$base" '
      {
        n = $0; sub(/@.*/, "", n)
        if (n == "wpe_write_create_private" || n == "wpe_write_copy_fd" ||
            n == "wpe_write_set_special_bits" || n == "wpe_write_set_mode_fd" ||
            n == "wpe_write_set_mode_path" || n == "wpe_write_guard_enter" ||
            n == "wpe_write_guard_leave") print "r1", obj, n, 1
      }'
    [ "$base" = we_write.o ] ||
    "$NMTOOL" -u "$o" 2>/dev/null | nm_undefined_source_names | awk -v obj="$base" '
      {
        n = $0; sub(/@.*/, "", n)
        if (n == "e_write" || n == "e_eol_write") print "r1", obj, n, 1
      }'
    # rule 1c: the names owner lists a folder only through a descriptor it
    # opened by name.  Mach-O spells the symbol _opendir$INODE64 on x86_64.
    [ "$base" != we_fileop.o ] ||
    "$NMTOOL" -u "$o" 2>/dev/null | nm_undefined_source_names | awk -v obj="$base" '
      {
        n = $0; sub(/[@$].*/, "", n)
        if (n == "opendir") print "r1c", obj, n, 1
      }'
    "$NMTOOL" -g "$o" 2>/dev/null | nm_defined_names > "$WORK/defined"
    # the owners' own definitions must be seen, or the scan below is blind
    case $base in
      we_write.o) grep -qx wpe_write_copy_fd "$WORK/defined" ||
        problem "canary: wpe_write_copy_fd not seen defined in we_write.o (the deleted-symbol scan is blind)" ;;
      we_fileop.o) grep -qx wpe_fileop_copy "$WORK/defined" ||
        problem "canary: wpe_fileop_copy not seen defined in we_fileop.o (the deleted-symbol scan is blind)" ;;
    esac
    awk -v obj="$base" '
      {
        n = $0
        if (n == "WpeCopyFileCont" || n == "WpeRenameCopy" || n == "WpeRenameCopyDir" ||
            n == "WpeRemoveDir" || n == "WpeLinkFile" || n == "WpeRenameLink" ||
            n == "WpeGetWastefile" || n == "e_mess_win" || n == "e_rename_dir")
          print "r1", obj, n, 1
      }' "$WORK/defined"
  done > "$WORK/symfound"
fi

# Rules 2, 4, 5, 6: the source.
: > "$WORK/srcfound"
for f in "$SRCDIR"/we_*.c "$SRCDIR"/We*.c; do
  [ -f "$f" ] || continue
  # the owners: the write owner is no offender; the names owner (we_fileop.c)
  # makes and moves names and copies bytes, and the line endings owner
  # (we_lineend.c) walks rows, so each is exempt from the rule it is the owner of
  case ${f##*/} in
    we_write.c) scan "$f" | awk '$1 == "r12" || $1 == "r15"' >> "$WORK/srcfound"; continue ;;
    we_fileop.c) scan "$f" | awk '$1 != "r8" && $1 != "r9" && $1 != "r10" && $1 != "r15"' >> "$WORK/srcfound"; continue ;;
    we_lineend.c) scan "$f" | awk '$1 != "r9" && $1 != "r11" && $1 != "r12" && $1 != "r15"' >> "$WORK/srcfound"; continue ;;
  esac
  scan "$f" | awk '$1 != "r11" && $1 != "r12" && $1 != "r15"' >> "$WORK/srcfound"
done
# rule 15 in the headers: a macro cannot carry the window's recorded mode
for f in "$SRCDIR"/*.h; do
  [ -f "$f" ] || continue
  scan_header_mode "$f" >> "$WORK/srcfound"
done
{
  awk '{ print $2 "|" $1 ":" $3 "|" $4 }' "$WORK/srcfound"
  awk '{ print $2 "|" $1 ":" $3 "|" $4 }' "$WORK/symfound"
} | sort > "$WORK/found"

[ -f "$SRCDIR/we_write.c" ] || problem "canary: we_write.c not found under $SRCDIR"
sh "$0" --selftest > /dev/null 2>&1 || problem "canary: the scan is blind (run $0 --selftest)"

if [ $LIST -eq 1 ]; then
  awk -F'|' '{ print $1 "|" $2 "|" $3 "|PASS|reason" }' "$WORK/found"
  exit 0
fi

: > "$WORK/allowed"
if [ -f "$ALLOW" ]; then
  awk -F'|' '
    /^[ \t]*(#|$)/ { next }
    {
      if (NF < 5 || $3 !~ /^[0-9]+$/ || $4 == "" || $5 == "") { print "bad line " NR " in the allow file: " $0 > "/dev/stderr"; bad = 1; next }
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

awk -F'|' '
  FILENAME == ARGV[1] { al[$1 "|" $2] = $3; pass[$1 "|" $2] = $4; next }
  {
    key = $1 "|" $2; got[key] = $3
    rule = $2; sub(/:.*/, "", rule)
    if (!(key in al)) {
      if (rule == "r13")
        print "offender: " $1 " " $2 ": the result of e_close_window is dropped; it says WPE_ESC when the window stays (a text window the user keeps, a project list its file refuses): read it, or list the function with its reason"
      else if (rule == "r15")
        print "offender: " $1 " " $2 ": the window'"'"'s recorded mode is read outside write_mode_before_data, or there as more than its nine bits, or that function names a special bit: the special bits of a write come from the file it opened, never from a mode recorded at load"
      else if (rule == "r17")
        print "offender: " $1 " " $2 ": the set-ID bits of a copy are not the ones fileop_bits_for_copy returned: it compares the owner and group of the copy with the original'"'"'s, so give wpe_write_set_special_bits the variable keep it assigned, and never the original'"'"'s mode (also not through wpe_write_set_mode_fd)"
      else if (rule == "r14")
        print "offender: " $1 " " $2 ": e_prog.project is written outside the one place that switches the project: go through e_select_project, or list the function with its reason"
      else
        print "offender: " $1 " " $2 ": the user'"'"'s text is written outside the owner (we_write.c): move it onto the owner, or list it with its pass"
      bad = 1
    } else if ($3 + 0 > al[key] + 0) {
      print "more than allowed: " $1 " " $2 " has " $3 ", the allow file says " al[key] " (" pass[key] ")"; bad = 1
    } else if ($3 + 0 < al[key] + 0) {
      print "fewer than the allow file says: " $1 " " $2 " has " $3 ", lower the line from " al[key] " (" pass[key] ")"; bad = 1
    }
  }
  END {
    for (k in al) if (!(k in got)) { print "stale entry, delete it: " k " (" pass[k] ")"; bad = 1 }
    exit bad
  }' "$WORK/allowed" "$WORK/found" > "$WORK/report"
status=$?

if [ -s "$WORK/problems" ] || [ $status -ne 0 ]; then
  echo "check-write-owner: FAIL"
  cat "$WORK/problems" "$WORK/report"
  exit $BAD
fi
not_built_note=
[ -n "$OBJECTS" ] && not_built_note=$(wpe_not_built_note "$WORK/notbuilt")
echo "check-write-owner: ok ($(wc -l < "$WORK/found" | tr -d ' ') counted site(s), all in the allow file$not_built_note)"
exit 0
