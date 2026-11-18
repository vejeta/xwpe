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
#     _set_special_bits, _set_mode_fd, _set_perm, the SIGXFSZ guard) are
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
#     _tree forms, wpe_fileop_wastebasket_slot, fileop_parent_open and
#     wpe_write_set_perm), counted
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
# 16. A mode is never changed by name outside the owner.  chmod, lchmod and
#     fchmodat (the faces that take a path, a link followed or not) appear
#     only inside write_mode_set (we_write.c), which reaches a file through
#     the descriptor it opened and compared with what lstat saw, or, for a
#     file that cannot be opened, through the name without following a link
#     and checks afterwards that the name still led to the same inode.  The
#     path faces of the owner are wpe_write_set_perm (the Attributes
#     dialog) and wpe_write_add_perm (Ctrl-F9).  A site that is still to
#     move is a line of the allow file marked with its pass (P2e), so it
#     stays visible.  Word-bounded like rule 12: wpe_fileop_chmod( and
#     fchmod( (by descriptor) are not sites.
# 19. No file but we_fileop.c calls wpe_fileop_chmod( (the face with no
#     identity, kept for the owner's own tests).  A caller that names a file
#     the user was shown calls wpe_fileop_chmod_shown with the stat it showed,
#     so a later caller cannot drop the identity by mistake.  No allow line.
# 17. The special bits a File Manager copy is given are decided in one place.
#     Outside we_write.c, wpe_write_set_special_bits( is called only in
#     fileop_job_fill_temp, its second argument is the variable `keep`, `keep`
#     is assigned once, from fileop_bits_for_copy( (which compares the owner
#     and group of the copy with the original's), and never changed after.
#     Passing the original's mode, or that mode masked, gives a program of
#     another user to whoever copies it.
# 18. The file a window read (FENSTER.file_id) is written only by the owner
#     of identities.  Outside we_write.c the token `file_id` appears only as
#     the operand of `&` (`&f->file_id`, handed to wpe_write_identity_take,
#     _forget or _of_name, which e_edit, e_recover and the allocation sites
#     do, or to a question that reads it).  An assignment, a member write
#     (`f->file_id.known = 1`) or any other read of it is a second place
#     that decides which file is the window's own.
# 20. Another file at the name is decided from the name, never from an
#     errno.  Each system has its own word for a link, a socket or a FIFO
#     met by an open or a chmod (ELOOP, EMLINK, EFTYPE, ENXIO, EOPNOTSUPP),
#     so a list of them is right on one system and wrong on the next.
#     Comments and strings are blanked and the names bounded by word, as in
#     rule 12.  In we_write.c none of ELOOP, ENXIO, EMLINK, EOPNOTSUPP and
#     ENOTSUP appears, except ELOOP in write_open_failure (the one value the
#     owner reports for a link that took a name that is never followed: set
#     there, never read), and EFTYPE appears only in write_mode_refusal (a
#     refused sticky bit).  In we_fileop.c EOPNOTSUPP, ENOTSUP and EMLINK appear only
#     in fileop_rename_noreplace_at (what a system without the rename or the
#     link says of it), and ELOOP only in fileop_job_dst_is_folder and
#     fileop_root_dst_follow (their opens follow links, so ELOOP is a cycle on
#     every system) and fileop_walk (the depth limit, set and not read).
#     ENXIO and EFTYPE appear nowhere in we_fileop.c.  No allow line.
# 21. The assistant writes a user's file in two places only.  In we_ai*.c
#     wpe_write_save_text(, wpe_write_save_text_ask( and wpe_write_restore_text( appear only inside
#     wpe_ai_write_judged_file (its writes) and ws_restore_before_image (the
#     revert), and each call passes the expectation of a look (the last but
#     one argument is a variable, never a literal NULL or 0), so the owner
#     holds the write to the file the user was shown.  No allow line.
# 24. "Is this the object I saw" is judged in one place, we_seen.c (22 and 23
#     belong to the read owner and the path walk).  Strings and comments are
#     blanked, names bounded by word, a macro counts as a site of its own.
#     Each clause has its own tag in the allow file (r24a to r24g):
#     a. st_ino and st_dev appear in no product source but we_seen.c.  The one
#        permanent row is we_startcmd.c built_rec_identify, which writes the
#        numbers to a store and asks "unchanged", never "same".
#     b. a member access .seen_ or ->seen_ (the members of a WpeSeen) appears
#        only in we_seen.c and we_seen.h.  A second reader is a second judge.
#     c. statx(, st_birthtim, st_birthtimespec and stx_btime appear only in
#        we_seen.c.
#     d. ck_rdev, ck_rino, and wpe_write_identity_is( or _differs( with three
#        arguments (a device and an inode passed by hand) appear nowhere, in
#        the product sources and in the tests.
#     f. the second argument of wpe_fileop_chmod_shown( and
#        wpe_write_set_perm_seen( (the file the user was shown) is NULL or a
#        WpeSeen called shown.  The build accepts incompatible pointers, so a
#        struct stat passed there compiles and is compared by number.
#     e. in tests/*.c and tests/*.h, st_ino compared with != or asserted equal
#        to 0 appears only in other_object_while_held (tests/seen_rows.h),
#        which takes the descriptor that holds the first object.  "Another
#        inode" asserted with nothing held is the defect itself.
#     g. a call of wpe_seen_into_stat outside we_seen.c.  The bridge hands a
#        record to the mode owner as a device and inode; the pass that moves
#        the mode owner onto records deletes it and its allow row.
#     Clauses (a) to (c) scan the product sources and headers, not the tests:
#     a test makes files and reads their numbers on purpose.  The sites still
#     to move are lines of the allow file, each naming its function and the
#     pass (N2 to N5) that deletes it.  tests/seen_rows.h and the rows of
#     tests/test_seen.c read members of the record on purpose and are held to
#     (d) and (e) only.
# 25. The editor's own places and the names it chooses itself are decided in
#     one place each.  A name the editor picks (the conversation, a before
#     image, a list for git) is written by the owner of its own folders
#     (wpe_own_dir, wpe_write_own_bytes in we_write.c), never through a link;
#     where the user's home, state and configuration are is decided in
#     we_places.c.  Strings are kept and comments blanked, names bounded by
#     word.  Each clause has its own tag in the allow file (r25a to r25h):
#     a. wpe_write_text(, wpe_write_stream_open( and _flags( appear outside
#        we_write.c only in the functions the allow file lists, each with how
#        the user named the path.
#     b. XDG_STATE_HOME, XDG_CONFIG_HOME, the piece .local/state and a ".config"
#        piece appear only in we_places.c and we_places.h (headers and macros
#        included).
#     c. no working-folder substitute, and no allow line: a literal starting
#        with a dot assigned to a name with home, dir, base, state or config in
#        it (also as the source of a strcpy, strncpy, wpe_path_copy, snprintf
#        or sprintf into such a name), the literal ./.local, and, in a
#        function that also calls getenv(, a snprintf of a literal starting
#        with a dot and a ?: whose last branch is ".".
#     d. wpe_state_dir( is called only in we_places.c, write_own_base
#        (we_write.c), wpe_crash_path_init (we_unix.c) and startcmd_state_dir
#        (we_startcmd.c, whose unit tests link no write owner).  No allow line.
#     e. a literal that starts with /tmp/ ends in XXXXXX (a template).
#     f. e_ai_resume_session and e_ai_last_session_id are assigned only in
#        wpe_ai_resume_id_take.  No allow line.
#     g. wpe_write_own_bytes( and wpe_write_own_remove( take a WPE_OWN_ kind as
#        their first argument, never a variable.  No allow line.
#     h. wpe_write_bytes( appears nowhere.  No allow line.
#     i. getenv("HOME"), secure_getenv("HOME") and getpwuid( appear only in
#        we_places.c (the user's home is asked of wpe_user_home).  A use that
#        is not a place of the editor's own is a line of the allow file with
#        the reason (the `~` of a path the user typed, the name of an owner of
#        a file).  Limits, said plainly: this is a review gate, not a proof.
#        It does not see a ?: whose "." is split across functions, a fallback
#        to "." through a name with no place word in it (home, dir, base,
#        state, config), nor getenv(MACRO) where the macro hides "HOME".
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

. "$(dirname "$0")/workdir.sh"
work_dir write-owner $BAD
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
  awk -v file="$(basename "$1")" -v tests="${2:-0}" "$BLANK_C"'
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
    # a hit on a preprocessor line is counted apart (ppmode), under the name
    # "(preprocessor)", so a macro is not charged to the function above it
    function note(rule, n) { if (ppmode) ppcnt[rule] += n; else cnt[rule] += n }
    function flush_fn(   r) {
      if (fn == "") return
      # rule 25c: a literal that starts with a dot is the format of a snprintf,
      # or the last branch of a ?:, in a function that also reads the
      # environment (a place that falls back on the working folder when the
      # variable is not set)
      if (fn_getenv && fn_dotfmt > 0) cnt["r25c"] += fn_dotfmt
      if (fn_getenv && fn_dotternary > 0) cnt["r25c"] += fn_dotternary
      fn_getenv = 0; fn_dotfmt = 0; fn_dotternary = 0
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
    # rule 18 for the code of one statement, strings blanked: every `file_id`
    # token that is not the operand of an address-of (`&` and not `&&`).
    function note18(m,   off, p, i) {
      off = 1
      while ((p = index(substr(m, off), "file_id")) > 0) {
        p += off - 1; off = p + 7
        if (p > 1 && substr(m, p - 1, 1) ~ /[A-Za-z0-9_]/) continue
        if (substr(m, p + 7, 1) ~ /[A-Za-z0-9_]/) continue
        i = p - 1
        while (i >= 1 && substr(m, i, 1) ~ /[A-Za-z0-9_.>-]/) i--
        while (i >= 1 && substr(m, i, 1) ~ /[ \t]/) i--
        if (i < 1 || substr(m, i, 1) != "&" || (i > 1 && substr(m, i - 1, 1) == "&")) note("r18", 1)
      }
    }
    # rule 21 for the code of one statement, strings blanked: the assistant
    # writes a user'"'"'s file in two places only, wpe_ai_write_judged_file (its
    # writes) and ws_restore_before_image (the revert).  Each call passes the
    # expectation of a look, a variable and never a literal NULL or 0 (the
    # last but one argument), so the owner holds the write to the file the
    # user was shown.
    function note21(m,   t, rest) {
      t = m
      while (match(t, /(^|[^A-Za-z0-9_.>])(wpe_write_save_text|wpe_write_save_text_ask|wpe_write_restore_text)[ \t]*[(]/)) {
        rest = substr(t, RSTART + RLENGTH); sub(/;.*/, "", rest)
        if (fn != "wpe_ai_write_judged_file" && fn != "ws_restore_before_image") note("r21", 1)
        else if (rest ~ /(^|,)[ \t]*(NULL|0)[ \t]*,[^,]*$/) note("r21", 1)
        t = substr(t, RSTART + RLENGTH)
      }
    }
    # rule 24 (the owner of "is this the object I saw", we_seen.c), for the
    # code of one statement, strings blanked.  Clauses, each its own tag:
    #   r24a  st_ino or st_dev, outside we_seen.c (an inode number remembered
    #         and compared later is what a file system gives back)
    #   r24b  a member access .seen_X or ->seen_X outside we_seen.c and
    #         we_seen.h (a second judge reads the record itself)
    #   r24c  statx(, st_birthtim, st_birthtimespec, stx_btime outside we_seen.c
    #   r24d  ck_rdev, ck_rino, and wpe_write_identity_is( or _differs( with
    #         three arguments (the number and the device passed by hand), anywhere
    #   r24e  (tests only) st_ino compared with != or asserted equal to 0
    #         outside other_object_while_held
    #   r24g  a call of wpe_seen_into_stat outside we_seen.c: the bridge that
    #         hands a record to the mode owner as a number, deleted with the
    #         pass that moves the mode owner onto records
    # The count of the arguments of a call: the commas at its own depth plus one.
    function arg_count(s, p,   depth, i, n, c, commas) {
      depth = 0; commas = 0; n = length(s)
      for (i = p; i <= n; i++) {
        c = substr(s, i, 1)
        if (c == "(" || c == "[" || c == "{") depth++
        else if (c == ")" || c == "]" || c == "}") { depth--; if (depth == 0) return commas + 1 }
        else if (c == "," && depth == 1) commas++
      }
      return 0
    }
    # every word-bounded match of the extended regex re in t counts for tag
    function count_word(t, re, tag,   hits) {
      while (match(t, "(^|[^A-Za-z0-9_])(" re ")([^A-Za-z0-9_]|$)")) { note(tag, 1); t = substr(t, RSTART + RLENGTH - 1) }
    }
    # the nth top-level argument (1-based) of the call whose "(" is at p in s,
    # blanks trimmed; "" when the call has fewer
    function nth_arg(s, p, n,   depth, i, len, c, k, start) {
      depth = 0; k = 1; len = length(s); start = p + 1
      for (i = p; i <= len; i++) {
        c = substr(s, i, 1)
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
    function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
    # clause f: the record of "the file the user was shown" (the Attributes
    # dialog, Ctrl-F9) is a WpeSeen called shown.  The second argument of
    # wpe_fileop_chmod_shown and wpe_write_set_perm_seen is NULL or shown; a
    # struct stat passed there compiles (the build accepts incompatible
    # pointers) and is compared by number, so it is a site.
    function note24_shown(m,   t, p, a) {
      t = m
      while (match(t, /(^|[^A-Za-z0-9_])(wpe_fileop_chmod_shown|wpe_write_set_perm_seen)[ \t]*[(]/)) {
        p = RSTART + RLENGTH - 1
        a = nth_arg(t, p, 2)
        if (a !~ /^(NULL|&?shown|const[ \t]+WpeSeen[ \t]*\*[ \t]*shown)$/) note("r24f", 1)
        t = substr(t, p + 1)
      }
    }
    function note24_inside(m,   t) {
      if (file == "we_seen.c" || file == "we_seen.h") return
      note24_shown(m)
      count_word(m, "st_ino|st_dev", "r24a")
      t = m
      while (match(t, /(\.|->)[ \t]*seen_[a-z_]+/)) { note("r24b", 1); t = substr(t, RSTART + RLENGTH) }
      t = m
      while (match(t, /(^|[^A-Za-z0-9_])statx[ \t]*[(]/)) { note("r24c", 1); t = substr(t, RSTART + RLENGTH) }
      count_word(m, "st_birthtim|st_birthtimespec|stx_btime", "r24c")
      count_word(m, "wpe_seen_into_stat", "r24g")
    }
    function note24_everywhere(m,   t, p) {
      count_word(m, "ck_rdev|ck_rino", "r24d")
      t = m
      while (match(t, /(^|[^A-Za-z0-9_])wpe_write_identity_(is|differs)[ \t]*[(]/)) {
        p = RSTART + RLENGTH - 1
        if (arg_count(t, p) == 3) note("r24d", 1)
        t = substr(t, p + 1)
      }
    }
    function note24_tests(m,   t) {
      note24_everywhere(m)
      if (fn == "other_object_while_held") return
      t = m
      while (match(t, /st_ino[ \t]*!=|!=[ \t]*[A-Za-z_][A-Za-z0-9_]*(\.|->)st_ino|st_ino[ \t]*==[^,;]*,[ \t]*0[ \t]*[,)]/)) { note("r24e", 1); t = substr(t, RSTART + RLENGTH) }
    }
    # the preprocessor lines of a product source: a macro may carry the tokens
    function note24_pp(pp) {
      note24_inside(pp)
      note24_everywhere(pp)
    }
    # rule 20 for one name of an errno: is it allowed in this function?
    function allowed20(id) {
      if (file == "we_write.c") return((fn == "write_mode_refusal" && id == "EFTYPE") || (fn == "write_open_failure" && id == "ELOOP"))
      if (id == "ELOOP") return(fn == "fileop_job_dst_is_folder" || fn == "fileop_root_dst_follow" || fn == "fileop_walk")
      if (id == "EOPNOTSUPP" || id == "ENOTSUP" || id == "EMLINK") return(fn == "fileop_rename_noreplace_at")
      return(0)
    }
    # rule 20 for the code of one statement, strings blanked: every errno
    # that one system names for "another kind of file here", unless the
    # function may.
    function note20(m,   t, id) {
      t = m
      while (match(t, /(^|[^A-Za-z0-9_])(ELOOP|ENXIO|EMLINK|EOPNOTSUPP|ENOTSUP|EFTYPE)([^A-Za-z0-9_]|$)/)) {
        id = substr(t, RSTART, RLENGTH); gsub(/[^A-Z]/, "", id)
        if (!allowed20(id)) note("r20", 1)
        t = substr(t, RSTART + RLENGTH - 1)
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
    # rule 25 (the editors own places and the names it chooses), for one
    # statement: S has its strings kept and M has them blanked.  Clauses, each
    # its own tag:
    #   r25a  wpe_write_text(, wpe_write_stream_open( and _flags( outside
    #         we_write.c: a name the user typed, with its reason in the allow file
    #   r25h  wpe_write_bytes( anywhere (the face is deleted)
    #   r25b  XDG_STATE_HOME, XDG_CONFIG_HOME, .local/state or a ".config"
    #         piece outside we_places.c and we_places.h
    #   r25c  a name of a place that is the working folder: a literal that
    #         starts with a dot assigned to home, dir, base, state or config,
    #         the literal ./.local, and a snprintf of a literal that starts
    #         with a dot in a function that also calls getenv(
    #   r25d  wpe_state_dir( outside we_places.c, write_own_base (we_write.c)
    #         and wpe_crash_path_init (we_unix.c)
    #   r25e  a literal that starts with /tmp/ and does not end in XXXXXX
    #   r25f  e_ai_resume_session or e_ai_last_session_id assigned outside
    #         wpe_ai_resume_id_take
    #   r25g  wpe_write_own_bytes( or wpe_write_own_remove( whose first
    #         argument does not start with WPE_OWN_ (a kind, never a variable)
    #   r25i  getenv("HOME") or getpwuid( outside we_places.c
    function note25(s, m,   t, p, a, lit) {
      if (file == "we_places.c" || file == "we_places.h") return
      t = m
      while (match(t, /(^|[^A-Za-z0-9_.>])wpe_write_bytes[ \t]*[(]/)) { note("r25h", 1); t = substr(t, RSTART + RLENGTH) }
      if (file != "we_write.c") {
        t = m
        while (match(t, /(^|[^A-Za-z0-9_.>])(wpe_write_text|wpe_write_stream_open|wpe_write_stream_open_flags)[ \t]*[(]/)) { note("r25a", 1); t = substr(t, RSTART + RLENGTH) }
        t = m
        while (match(t, /(^|[^A-Za-z0-9_.>])(wpe_write_own_bytes|wpe_write_own_remove)[ \t]*[(]/)) {
          p = RSTART + RLENGTH - 1
          a = nth_arg(t, p, 1)
          if (a !~ /^WPE_OWN_/) note("r25g", 1)
          t = substr(t, p + 1)
        }
      }
      t = s
      while (match(t, /XDG_(STATE|CONFIG)_HOME/)) { note("r25b", 1); t = substr(t, RSTART + RLENGTH) }
      t = s
      while (match(t, "[.]local/state")) { note("r25b", 1); t = substr(t, RSTART + RLENGTH) }
      t = s
      while (match(t, "[\"/][.]config")) { note("r25b", 1); t = substr(t, RSTART + RLENGTH) }
      t = s
      while (match(t, "[A-Za-z_0-9]*(home|dir|base|state|config)[A-Za-z_0-9]*[ \t]*=[ \t]*\"[.]")) { note("r25c", 1); t = substr(t, RSTART + RLENGTH) }
      t = s
      while (match(t, "=[^=;]*[?][^;]*:[ \t]*\"[.]\"")) { fn_dotternary++; t = substr(t, RSTART + RLENGTH) }
      t = s
      while (match(t, "(strcpy|strncpy|wpe_path_copy|snprintf|sprintf)[ \t]*[(][ \t]*[A-Za-z_0-9.>-]*(home|dir|base|state|config)[A-Za-z_0-9]*[ \t]*,([^,]*,)?[ \t]*\"[.]")) { note("r25c", 1); t = substr(t, RSTART + RLENGTH) }
      t = s
      while (match(t, "[.]/[.]local")) { note("r25c", 1); t = substr(t, RSTART + RLENGTH) }
      t = s
      while (match(t, "snprintf[ \t]*[(][^,]*,[^,]*,[ \t]*\"[.]")) { fn_dotfmt++; t = substr(t, RSTART + RLENGTH) }
      t = s
      while (match(t, /(^|[^A-Za-z0-9_.>])(secure_)?getenv[ \t]*[(][ \t]*"HOME"[ \t]*[)]/)) { note("r25i", 1); t = substr(t, RSTART + RLENGTH) }
      t = m
      while (match(t, /(^|[^A-Za-z0-9_.>])getpwuid(_r)?[ \t]*[(]/)) { note("r25i", 1); t = substr(t, RSTART + RLENGTH) }
      if (m ~ /(^|[^A-Za-z0-9_.>])getenv[ \t]*[(]/) fn_getenv = 1
      t = m
      while (match(t, /(^|[^A-Za-z0-9_.>])wpe_state_dir[ \t]*[(]/)) {
        if (!(file == "we_write.c" && fn == "write_own_base") && !(file == "we_unix.c" && fn == "wpe_crash_path_init") && !(file == "we_startcmd.c" && fn == "startcmd_state_dir")) note("r25d", 1)
        t = substr(t, RSTART + RLENGTH)
      }
      t = s
      while (match(t, "\"/tmp/[^\"]*\"")) {
        lit = substr(t, RSTART, RLENGTH)
        if (lit !~ /XXXXXX"$/) note("r25e", 1)
        t = substr(t, RSTART + RLENGTH)
      }
      t = m
      while (match(t, /(^|[^A-Za-z0-9_.>])(e_ai_resume_session|e_ai_last_session_id)[ \t]*=[^=]/)) {
        if (fn != "wpe_ai_resume_id_take") note("r25f", 1)
        t = substr(t, RSTART + RLENGTH - 1)
      }
    }
    # the same tokens on a preprocessor line, where a macro may carry them
    function note25_pp(raw) {
      if (file == "we_places.c" || file == "we_places.h") return
      while (match(raw, /XDG_(STATE|CONFIG)_HOME/)) { note("r25b", 1); raw = substr(raw, RSTART + RLENGTH) }
      while (match(raw, "[.]local/state")) { note("r25b", 1); raw = substr(raw, RSTART + RLENGTH) }
      while (match(raw, "[\"/][.]config")) { note("r25b", 1); raw = substr(raw, RSTART + RLENGTH) }
    }
    function statement(s,   t, p, q, e, rest, name, v, c, m, ident) {
      # a test source is held to rule 24 only (clauses d and e)
      if (tests == 1) { m = s; gsub(/"([^"\\]|\\.)*"/, "\"\"", m); note24_tests(m); return }
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
      # rule 25: the editors own places and the names it chooses
      note25(s, m)
      # rule 24 (clauses a to d, f and g; e scans the tests): the identity of an object is judged in we_seen.c
      note24_inside(m)
      note24_everywhere(m)
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
      while (match(t, /(^|[^A-Za-z0-9_.>])(wpe_fileop_(copy|move|link|remove)(_tree)?|wpe_fileop_wastebasket_slot|fileop_parent_open|wpe_write_set_perm(_seen)?)[ \t]*[(]/)) {
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
      # rule 16 (every file): no mode is changed by name outside the owner
      t = m
      while (match(t, /(^|[^A-Za-z0-9_.>])(chmod|lchmod|fchmodat)[ \t]*[(]/)) {
        if (fn != "write_mode_set") note("r16", 1)
        t = substr(t, RSTART + RLENGTH)
      }
      # rule 19 (every file but we_fileop.c): the Attributes dialog and any
      # other caller go through wpe_fileop_chmod_shown, which carries the
      # identity of the file the user was shown
      t = m
      while (file != "we_fileop.c" && match(t, /(^|[^A-Za-z0-9_.>])wpe_fileop_chmod[ \t]*[(]/)) {
        note("r19", 1); t = substr(t, RSTART + RLENGTH)
      }
      # rule 17 (every file but we_write.c): the set-ID bits of a copy are
      # the ones fileop_bits_for_copy returned
      if (file != "we_write.c") note17(m)
      # rule 18 (every file but we_write.c): the identity of a window is only
      # ever handed to the owner of identities by address
      if (file != "we_write.c") note18(m)
      # rule 20 (we_write.c and we_fileop.c): another file at the name is
      # decided from the name, never from the errno of one system
      if (file == "we_write.c" || file == "we_fileop.c") note20(m)
      # rule 21 (the assistant'"'"'s files, we_ai*.c): the owner'"'"'s text faces are
      # called from the two functions that write for the assistant
      if (file ~ /^we_ai/) note21(m)
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
        if (tests == 1) next
        while (match(pp, /(^|[^A-Za-z0-9_])filemode([^A-Za-z0-9_]|$)/)) { pp15++; pp = substr(pp, RSTART + RLENGTH - 1) }
        # rule 24: a macro may carry an inode, a stamp or a statx call too
        ppmode = 1; note24_inside(pp); note24_everywhere(pp); note25_pp(out); ppmode = 0
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
    END {
      statement(stmt); flush_fn()
      if (pp15 > 0) print "r15", file, "(preprocessor)", pp15
      for (r in ppcnt) if (ppcnt[r] > 0) print r, file, "(preprocessor)", ppcnt[r]
    }' "$1"
}

# scan_tests FILE - the rule 24 clauses a test source is held to (d and e),
# in the same "rule file function count" lines.
scan_tests() {
  scan "$1" 1
}

# scan_header_mode FILE - rule 15 for a header: every `filemode` of a header
# is a site, whatever carries it (a macro, one split by a backslash, a static
# inline function), except the member declaration of the window (`int
# filemode`), which scan leaves out.
scan_header_mode() {
  scan "$1" | awk '$1 == "r15" || $1 ~ /^r24/ || $1 == "r25b"'
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

int fake_by_name(int fd, char *a)
{
 char *say = "chmod( and lchmod( in a string";
 /* fchmodat(AT_FDCWD, a, 0600, 0) in a comment */
 int fine = wpe_fileop_chmod_shown(a, &st, 0600, 0) + t.chmod(a) + f->chmod(a) + fchmod(fd, 0600);
 chmod(a, 0644);
 lchmod(a, 0644);
 return(fchmodat(fd, a, 0644, 0));
}

int fake_no_identity(char *a)
{
 char *say = "wpe_fileop_chmod( in a string";
 return(wpe_fileop_chmod(a, 0600, 0) + wpe_fileop_chmod_shown(a, &st, 0600, 0));
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
  # rule 18: the window's identity written or read outside the owner, and
  # the right shape (by address, to the owner of identities)
  cat > "$WORK/we_fake_r18.c" <<'EOF'
static void fake_identity(FENSTER *f, struct stat *st)
{
 f->file_id.known = 1;
 f->file_id = other;
 if (ok && f->file_id.known)
  return;
 wpe_write_identity_take(&f->file_id, st);
 wpe_write_identity_forget(& f->file_id);
}
EOF
  # rule 21: the assistant's files call the owner's text faces from two
  # functions, each passing the expectation of a look
  cat > "$WORK/we_ai_fake.c" <<'EOF'
int wpe_ai_write_judged_file(FENSTER *f, const char *real, const char *text,
                             const WpeWriteExpect *x, WpeWriteOutcome *o)
{
 if (w)
  return(wpe_write_save_text_ask(real, text, &w->file_id, WPE_WRITE_NOFOLLOW, asked, x, o));
 return(wpe_write_save_text(real, text, NULL, WPE_WRITE_NOFOLLOW, x, o));
}
static int ws_restore_before_image(FENSTER *f, const char *path, const char *text, size_t len)
{
 int ok = wpe_write_restore_text(path, text, len, mode, w ? &w->file_id : NULL, &x, &o);
 ok += wpe_write_save_text_ask(path, text, NULL, 0, NULL, NULL, &o);
 return(wpe_write_restore_text(path, text, len, mode, NULL, NULL, &o) + ok);
}
static int ai_sneaky(FENSTER *f)
{
 int ok = wpe_write_save_text_ask(p, t, NULL, 0, NULL, &x, &o);
 return(wpe_write_save_text(p, t, NULL, 0, &x, &o) + ok);
}
EOF
  # rule 20: the rule looks at we_write.c and we_fileop.c by name, so the
  # fixtures are files of those names in a folder of their own
  mkdir "$WORK/r20" || exit $BAD
  cat > "$WORK/r20/we_write.c" <<'EOF'
static int write_mode_refusal(int err)
{
#ifdef EFTYPE
 if (err == EFTYPE)
  return(1);
#endif
 return(err == EPERM);
}
static int write_open_failure(const char *path, const WriteAim *aim, int err, WpeWriteOutcome *o)
{
 if (aim->open != WRITE_OPEN_NAMED_NOFOLLOW)
  return(err);
 o->replaced_by_link = 1;
 return(ELOOP);
}
static int write_target_open_check(int err)
{
 if (err == ELOOP)
  return(1);
 return(0);
}
static int write_mode_open_failed(WriteModeTarget *t, int err)
{
 /* ELOOP and ENXIO in a comment are no offence, nor are "EMLINK" in a string */
 const char *s = "ELOOP";
 int bound = MY_ELOOP + ELOOP_MAX;

 if (err == ELOOP || err == ENXIO)
  t->replaced = 1;
 return(err == EMLINK || err == EOPNOTSUPP || err == ENOTSUP || err == EFTYPE);
}
EOF
  cat > "$WORK/r20/we_fileop.c" <<'EOF'
static int fileop_rename_noreplace_at(int fromfd, const char *from, int tofd, const char *to)
{
 int err = errno;

 if (err != EOPNOTSUPP && err != ENOTSUP && err != EMLINK)
  return(-1);
 return(err == ELOOP);
}
static int fileop_job_dst_is_folder(FileJob *j, int pfd, const char *name)
{
 return(err == ELOOP ? ENOTDIR : err);
}
static int fileop_elsewhere(int err)
{
 if (err == ENXIO || err == EFTYPE)
  return(1);
 return(err == ELOOP || err == EMLINK);
}
EOF
  cat > "$WORK/we_fake.h" <<'EOF'
typedef struct {
 int filemode, flg;
} FakeWindow;
#define WPE_WINDOW_MODE(f) ((f)->filemode)
EOF
  # rule 24: one fixture for each of its clauses.  (a) an inode and a device
  # compared outside the owner (the comment, the string and a longer name are
  # no offence), (b) a member of a record read outside it, (c) statx and the
  # birth members called outside it, (d) the old folder numbers and the
  # three-argument form of the identity question, (e) in a test, "another
  # inode" asserted with nothing held.  A file named we_seen.c is the owner and
  # is exempt from (a) to (c).
  cat > "$WORK/we_fake_r24.c" <<'EOF'
int fake_inode(struct stat *sa, struct stat *sb)
{
 /* sa->st_ino == sb->st_ino in a comment */
 char *say = "st_ino in a string";
 int my_st_inode = 0;

 if (sa->st_ino == sb->st_ino && sa->st_dev == sb->st_dev)
  return(1);
 return(my_st_inode);
}
int fake_member(WpeSeen *rec, WpeSeenName *n)
{
 int seen_ino = 0;

 if (rec->seen_ino != 0 && n->seen.seen_ctime.tv_sec)
  return(seen_ino);
 return(0);
}
int fake_statx(int fd)
{
 struct statx sx;
 struct stat st;

 return(statx(fd, "", 0, 0, &sx) + (int)sx.stx_btime.tv_sec + (int)st.st_birthtim.tv_sec + (int)st.st_birthtimespec.tv_sec);
}
int fake_ck(FENSTER *c, struct stat *x)
{
 ck_rdev = x->dev;
 if (wpe_write_identity_is(&c->file_id, x->dev, x->ino))
  return(ck_rino);
 if (wpe_write_identity_is(&c->file_id, &seen))
  return(1);
 return(wpe_write_identity_differs(&w->file_id, st.st_dev, st.st_ino));
}
int fake_bridge(WpeSeen *rec, struct stat *st)
{
 wpe_seen_into_stat(rec, st);
 return(0);
}
int fake_shown(const char *filen, struct stat *buf, WpeNameSeen *seen)
{
 int rc = wpe_fileop_chmod_shown(filen, buf, &seen, 0644, &fo);

 rc += wpe_fileop_chmod_shown(filen, NULL, seen, 0644, &fo);
 rc += wpe_write_set_perm_seen(filen, &shown, seen, 0644, &kept);
 rc += wpe_write_set_perm_seen(filen, f(a, b), seen, 0644, &kept);
 return(rc);
}
EOF
  cat > "$WORK/we_fake_r24.h" <<'EOF'
#define SAME_FILE(a, b) ((a)->st_ino == (b)->st_ino)
EOF
  mkdir "$WORK/r24" || exit $BAD
  cat > "$WORK/r24/we_seen.c" <<'EOF'
int owner_judge(struct stat *sa, WpeSeen *rec)
{
 struct statx sx;

 return(sa->st_ino == rec->seen_ino && statx(0, "", 0, 0, &sx) == 0 && sx.stx_btime.tv_sec);
}
EOF
  mkdir "$WORK/r24t" || exit $BAD
  cat > "$WORK/r24t/test_fake.c" <<'EOF'
static int other_object_while_held(int held_fd, const struct stat *now)
{
 struct stat held;

 return(held.st_ino != now->st_ino);
}
int fake_row(struct stat a, struct stat b, struct stat *p)
{
 chk_int("a new inode", a.st_ino != b.st_ino, 1);
 chk_int("not the same", a.st_ino == b.st_ino, 0);
 chk_int("the same", a.st_ino == b.st_ino, 1);
 if (lstat(q, &st) != 0 || x.ino != st.st_ino)
  return(1);
 if (p->st_ino != 0)
  return(2);
 return(wpe_write_identity_is(&o.written, a.st_dev, a.st_ino));
}
EOF
  # rule 25: one fixture for each clause, and the shapes that are no offence.
  # (a) the three by-name faces and (h) the deleted one, (b) the places read
  # by hand, (c) a place that falls back on the working folder in three
  # spellings and in a second function, (d) the state folder asked outside
  # its resolver, (e) a fixed name in /tmp, (f) the resume ids assigned, (g) an
  # own face given a variable.  The files named we_places.c, we_write.c and
  # we_unix.c are the owners and are exempt where the rule says so.
  cat > "$WORK/we_fake_r25.c" <<'EOF'
int fake_by_name(const char *p, const char *t, WpeWriteOutcome *o)
{
 char *say = "wpe_write_text( is in a string";
 /* wpe_write_stream_open(p, o) in a comment */
 int fine = j->wpe_write_text(p, t, o);
 int a = wpe_write_text(p, t, o);
 FILE *b = wpe_write_stream_open(p, o);
 FILE *c = wpe_write_stream_open_flags(p, 1, o);
 return(wpe_write_bytes(p, t, 4, o) + fine);
}
int fake_places(char *buf, size_t n)
{
 const char *x = getenv("XDG_STATE_HOME");
 const char *y = getenv("XDG_CONFIG_HOME");
 const char *say = "my.config is no piece";
 const char *dir = ".config";
 /* XDG_STATE_HOME in a comment is no offence */
 snprintf(buf, n, "%s/.local/state/xwpe", x);
 return(y != NULL);
}
int fake_home(char *buf, size_t n)
{
 const char *home = ".";
 const char *fine = "/";
 const char *name = ".";
 const char *want = getenv("HOME");
 if (home == NULL || want == NULL)
  return(0);
 return(home != fine);
}
int fake_dot_base(char *buf, size_t n)
{
 char *base = "./x";
 char *path = "./.local/state/xwpe";
 return(base != path);
}
int fake_ternary(char *buf, size_t n)
{
 const char *home = getenv("XWPE_PLACE");
 const char *p = home ? home : ".";
 return(p != NULL);
}
int fake_plain_ternary(char *buf, size_t n)
{
 const char *dir = (buf != NULL) ? buf : ".";
 return(dir != NULL);
}
int fake_strcpy(char *buf, size_t n)
{
 char home[64];
 strcpy(home, ".");
 wpe_path_copy(state_dir, n, ".");
 wpe_path_copy(buf, n, ".");
 return(buf != NULL);
}
int fake_home_env(char *buf, size_t n)
{
 const char *h = getenv("HOME");
 struct passwd *pw = getpwuid(getuid());
 const char *other = getenv("HOMEWORK");
 const char *sec = secure_getenv("HOME");
 return(h != NULL && pw != NULL && other != NULL && sec != NULL);
}
int fake_consent(char *buf, size_t n)
{
 const char *home = getenv("HOME");
 if (home)
  snprintf(buf, n, "%s/plain", home);
 else
  snprintf(buf, n, ".xwpe_ai_consented");
 return(0);
}
int fake_no_getenv(char *buf, size_t n)
{
 snprintf(buf, n, ".hidden-name");
 return(0);
}
int fake_state(char *buf, size_t n)
{
 return(wpe_state_dir(buf, n));
}
int fake_tmp(void)
{
 FILE *a = fopen("/tmp/xwpe-fixed.txt", "a");
 char *t = "/tmp/xwpe_XXXXXX";
 char *d = "/tmp";
 return(mkdtemp(t) != NULL);
}
int fake_resume(const char *s)
{
 e_ai_resume_session = ws_strdup(s);
 e_ai_last_session_id = NULL;
 if (e_ai_resume_session == NULL)
  return(0);
 return(wpe_ai_resume_id_take(&e_ai_resume_session, s));
}
int wpe_ai_resume_id_take(char **slot, const char *s)
{
 e_ai_resume_session = *slot;
 return(0);
}
int fake_own(int kind, WpeWriteOutcome *o)
{
 int a = wpe_write_own_bytes(WPE_OWN_STATE, "ai", "x.session", "t", 1, o);
 int b = wpe_write_own_bytes(kind, "ai", "x.session", "t", 1, o);
 wpe_write_own_remove(d, "ai", "x.session");
 wpe_write_own_remove(WPE_OWN_RUN, NULL, "x");
 return(a + b);
}
#define FAKE_DIR ".config"
EOF
  mkdir "$WORK/r25" || exit $BAD
  cat > "$WORK/r25/we_places.c" <<'EOF'
int wpe_state_dir(char *buf, size_t n)
{
 const char *x = getenv("XDG_STATE_HOME");
 const char *h = getenv("HOME");
 struct passwd *pw = getpwuid(geteuid());
 const char *home = ".";
 snprintf(buf, n, "%s/.local/state/xwpe", x);
 return(wpe_state_dir(buf, n));
}
EOF
  mkdir "$WORK/r25w" || exit $BAD
  cat > "$WORK/r25w/we_write.c" <<'EOF'
static int write_own_base(WpeOwnKind kind, char *buf, size_t n)
{
 return(wpe_state_dir(buf, n));
}
static int write_other(char *buf, size_t n)
{
 return(wpe_state_dir(buf, n) + wpe_write_text(buf, "x", 0));
}
int wpe_write_own_bytes(WpeOwnKind kind, const char *sub, const char *leaf, const char *bytes,
                        size_t len, WpeWriteOutcome *o)
{
 return(wpe_write_stream_open(sub, o) != NULL);
}
EOF
  mkdir "$WORK/r25u" || exit $BAD
  cat > "$WORK/r25u/we_unix.c" <<'EOF'
static void wpe_crash_path_init(void)
{
 wpe_state_dir(wpe_crash_dir, sizeof wpe_crash_dir);
}
EOF
  mkdir "$WORK/r25s" || exit $BAD
  cat > "$WORK/r25s/we_startcmd.c" <<'EOF'
static int startcmd_state_dir(char *out, size_t sz)
{
 return wpe_state_dir(out, sz) == 0 ? 0 : -1;
}
static int startcmd_other(char *out, size_t sz)
{
 return wpe_state_dir(out, sz);
}
EOF
  cat > "$WORK/we_fake_r25.h" <<'EOF'
#define WPE_STATE_BELOW ".local/state"
#define WPE_ENV "XDG_CONFIG_HOME"
EOF
  scan "$WORK/we_fake.c" | sort > "$WORK/fake.found"
  scan "$WORK/we_fake_r25.c" >> "$WORK/fake.found"
  scan "$WORK/r25/we_places.c" >> "$WORK/fake.found"
  scan "$WORK/r25w/we_write.c" >> "$WORK/fake.found"
  scan "$WORK/r25u/we_unix.c" >> "$WORK/fake.found"
  scan "$WORK/r25s/we_startcmd.c" >> "$WORK/fake.found"
  scan_header_mode "$WORK/we_fake_r25.h" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r24.c" >> "$WORK/fake.found"
  scan_header_mode "$WORK/we_fake_r24.h" >> "$WORK/fake.found"
  scan "$WORK/r24/we_seen.c" >> "$WORK/fake.found"
  scan_tests "$WORK/r24t/test_fake.c" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r15d.c" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r17a.c" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r17b.c" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r17c.c" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r17d.c" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r17ok.c" >> "$WORK/fake.found"
  scan "$WORK/we_fake_r18.c" >> "$WORK/fake.found"
  scan "$WORK/r20/we_write.c" >> "$WORK/fake.found"
  scan "$WORK/r20/we_fileop.c" >> "$WORK/fake.found"
  scan "$WORK/we_ai_fake.c" >> "$WORK/fake.found"
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
              "r12 we_fake.c fake_mode 1" "r16 we_fake.c fake_by_name 3" "r19 we_fake.c fake_no_identity 1" \
              "r13 we_fake.c fake_close 8" \
              "r14 we_fake.c fake_project 7" \
              "r17 we_fake_r17a.c fileop_job_fill_temp 1" \
              "r17 we_fake_r17b.c fileop_job_fill_temp 2" \
              "r17 we_fake_r17c.c fileop_job_fill_temp 1" \
              "r17 we_fake_r17c.c fileop_stash 1" \
              "r17 we_fake_r17d.c fileop_job_fill_temp 1" \
              "r18 we_fake_r18.c fake_identity 3" \
              "r21 we_ai_fake.c ws_restore_before_image 2" \
              "r21 we_ai_fake.c ai_sneaky 2" \
              "r20 we_write.c write_mode_open_failed 6" \
              "r20 we_write.c write_target_open_check 1" \
              "r20 we_fileop.c fileop_rename_noreplace_at 1" \
              "r20 we_fileop.c fileop_elsewhere 4" \
              "r24a we_fake_r24.c fake_inode 4" "r24b we_fake_r24.c fake_member 2" \
              "r24c we_fake_r24.c fake_statx 4" "r24d we_fake_r24.c fake_ck 4" \
              "r24a we_fake_r24.c fake_ck 2" "r24a we_fake_r24.h (preprocessor) 2" \
              "r24f we_fake_r24.c fake_shown 2" "r24g we_fake_r24.c fake_bridge 1" \
              "r24e test_fake.c fake_row 4" "r24d test_fake.c fake_row 1" \
              "r15 we_fake.c fake_save 1" "r15 we_fake.c (preprocessor) 1" \
              "r15 we_fake_r15a.c write_mode_before_data 1" \
              "r15 we_fake_r15b.c write_mode_before_data 3" \
              "r15 we_fake_r15d.c write_mode_before_data 3" \
              "r15 we_fake.h (preprocessor) 1" \
              "r15 we_fake2.h - 1" "r15 we_fake3.h window_mode 1" \
              "r25a we_fake_r25.c fake_by_name 3" "r25h we_fake_r25.c fake_by_name 1" \
              "r25b we_fake_r25.c fake_places 4" "r25c we_fake_r25.c fake_places 1" \
              "r25c we_fake_r25.c fake_home 1" \
              "r25c we_fake_r25.c fake_dot_base 2" "r25b we_fake_r25.c fake_dot_base 1" \
              "r25c we_fake_r25.c fake_consent 1" \
              "r25c we_fake_r25.c fake_ternary 1" "r25c we_fake_r25.c fake_strcpy 2" \
              "r25i we_fake_r25.c fake_home 1" "r25i we_fake_r25.c fake_home_env 3" \
              "r25d we_fake_r25.c fake_state 1" "r25e we_fake_r25.c fake_tmp 1" \
              "r25f we_fake_r25.c fake_resume 2" "r25g we_fake_r25.c fake_own 2" \
              "r25b we_fake_r25.c (preprocessor) 1" \
              "r25d we_write.c write_other 1" "r25d we_startcmd.c startcmd_other 1" \
              "r25b we_fake_r25.h (preprocessor) 2"; do
    grep -qx "$want" "$WORK/fake.found" || {
      echo "check-write-owner: FAIL (selftest: the scan does not name \"$want\"; it found:)"
      cat "$WORK/fake.found"
      exit $BAD
    }
  done
  # rule 24: the owner's own file, and the one helper that asserts "another
  # inode" with a descriptor held, are no offence
  if grep -q "^r24[abcg] we_seen.c " "$WORK/fake.found"; then
    echo "check-write-owner: FAIL (selftest: a file named we_seen.c was counted by rule 24; it found:)"
    cat "$WORK/fake.found"
    exit $BAD
  fi
  if grep -q "^r24e test_fake.c other_object_while_held " "$WORK/fake.found"; then
    echo "check-write-owner: FAIL (selftest: other_object_while_held was counted by rule 24e; it found:)"
    cat "$WORK/fake.found"
    exit $BAD
  fi
  # the two functions rule 21 allows, with a look, are no offence
  if grep -q "^r21 we_ai_fake.c wpe_ai_write_judged_file " "$WORK/fake.found"; then
    echo "check-write-owner: FAIL (selftest: the assistant's write with a look was counted by rule 21; it found:)"
    cat "$WORK/fake.found"
    exit $BAD
  fi
  # rule 25: the owners (we_places.c, and in we_write.c and we_unix.c the one
  # function that asks for the state folder) and the shapes that are no offence
  if grep -q "^r25[a-i] we_places.c \|^r25d we_write.c write_own_base \|^r25a we_write.c \|^r25g we_write.c \|^r25d we_unix.c \|^r25d we_startcmd.c startcmd_state_dir \|^r25c we_fake_r25.c fake_no_getenv \|^r25c we_fake_r25.c fake_plain_ternary \|^r25i we_fake_r25.c fake_no_getenv \|^r25f we_fake_r25.c wpe_ai_resume_id_take " "$WORK/fake.found"; then
    echo "check-write-owner: FAIL (selftest: an owner or a shape rule 25 allows was counted; it found:)"
    cat "$WORK/fake.found"
    exit $BAD
  fi
  # the functions rule 20 allows, and the names it bounds by word, are no offence
  if grep -q "^r20 we_write.c write_mode_refusal \|^r20 we_write.c write_open_failure \|^r20 we_fileop.c fileop_job_dst_is_folder " "$WORK/fake.found"; then
    echo "check-write-owner: FAIL (selftest: an errno where rule 20 allows it was counted; it found:)"
    cat "$WORK/fake.found"
    exit $BAD
  fi
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
  if grep -q "^r16 we_fake.c write_mode_set " "$WORK/fake.found"; then
    echo "check-write-owner: FAIL (selftest: the chmod inside write_mode_set was counted by rule 16; it found:)"
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
            n == "wpe_write_set_perm" || n == "wpe_write_guard_enter" ||
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
    we_write.c) scan "$f" | awk '$1 == "r12" || $1 == "r15" || $1 == "r16" || $1 == "r20" || $1 ~ /^r24/ || $1 ~ /^r25/' >> "$WORK/srcfound"; continue ;;
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
# rule 24 in the tests: clauses (d) and (e) only.  Clauses (a) to (c) are not
# asked of a test, which makes files and reads their numbers on purpose; what a
# test may not do is assert "another inode" with nothing held (clause e).
for f in "$SRCDIR"/tests/*.c "$SRCDIR"/tests/*.h; do
  [ -f "$f" ] || continue
  scan_tests "$f" >> "$WORK/srcfound"
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
      else if (rule == "r16")
        print "offender: " $1 " " $2 ": a mode is changed by name outside write_mode_set (we_write.c): go through wpe_write_set_perm or wpe_write_add_perm, which open the file once and change that descriptor, or list the function with its pass"
      else if (rule == "r19")
        print "offender: " $1 " " $2 ": wpe_fileop_chmod has no identity of the file the user was shown: call wpe_fileop_chmod_shown with the stat the dialog took"
      else if (rule == "r17")
        print "offender: " $1 " " $2 ": the set-ID bits of a copy are not the ones fileop_bits_for_copy returned: it compares the owner and group of the copy with the original'"'"'s, so give wpe_write_set_special_bits the variable keep it assigned, and never the original'"'"'s mode (also not through wpe_write_set_mode_fd)"
      else if (rule == "r20")
        print "offender: " $1 " " $2 ": another file at the name is decided from an errno (ELOOP, ENXIO, EMLINK, EOPNOTSUPP, ENOTSUP or EFTYPE), which each system spells differently: look at the name again (lstat and compare with what was first seen: write_mode_name_changed, write_name_kind) and never ask the errno.  No allow line"
      else if (rule == "r21")
        print "offender: " $1 " " $2 ": the assistant writes a user'"'"'s file outside wpe_ai_write_judged_file and ws_restore_before_image, or passes no look (a literal NULL or 0 for the expectation): take the look with wpe_path_look_write and write through those two.  No allow line"
      else if (rule == "r24a")
        print "offender: " $1 " " $2 ": st_ino or st_dev is read outside we_seen.c.  An inode number remembered from an earlier moment is given back by the file system after its file is gone: keep a WpeSeen (wpe_seen_look_fd, _name, _at) and ask wpe_seen_judge, or for two looks of one moment wpe_seen_same_now.  A site still to move is a line of the allow file with its pass"
      else if (rule == "r24b")
        print "offender: " $1 " " $2 ": a member of a WpeSeen (.seen_ or ->seen_) is read outside we_seen.c.  A second reader of the record is a second judge: use wpe_seen_judge, wpe_seen_known or wpe_seen_kind"
      else if (rule == "r24c")
        print "offender: " $1 " " $2 ": statx, st_birthtim, st_birthtimespec or stx_btime outside we_seen.c.  The birth time is read in one place, with its system notes: take a WpeSeen"
      else if (rule == "r24d")
        print "offender: " $1 " " $2 ": ck_rdev or ck_rino (a folder number kept by hand: hold the folder with wpe_seen_hold_dir and ask wpe_seen_held_is), or wpe_write_identity_is or _differs with the device and inode as two arguments (pass the WpeSeen of the look)"
      else if (rule == "r24g")
        print "offender: " $1 " " $2 ": wpe_seen_into_stat is called outside we_seen.c.  It hands a record to the mode owner as a device and inode, which a file system gives back: the mode owner takes a WpeSeen and asks wpe_seen_judge.  A site still to move is a line of the allow file with its pass"
      else if (rule == "r24f")
        print "offender: " $1 " " $2 ": the file the user was shown is passed to wpe_fileop_chmod_shown or wpe_write_set_perm_seen as something other than NULL or a WpeSeen called shown.  A struct stat there compiles (incompatible pointers are accepted) and is compared by inode number: take a WpeSeen with wpe_seen_look_name next to the dialog'"'"'s stat"
      else if (rule == "r24e")
        print "offender: " $1 " " $2 ": a test asserts \"another inode\" by comparing st_ino with nothing held.  A file system may give the first number to the newcomer: assert it with other_object_while_held (tests/seen_rows.h), which takes the descriptor that holds the first object"
      else if (rule == "r25a")
        print "offender: " $1 " " $2 ": wpe_write_text, wpe_write_stream_open or wpe_write_stream_open_flags writes a name the user typed or opened.  A name the editor chose (a conversation, a before image, a list for git) goes through wpe_write_own_bytes or wpe_write_copy, which never write through a link.  List the function with the way the user named the path, or move it"
      else if (rule == "r25h")
        print "offender: " $1 " " $2 ": wpe_write_bytes opens a name by name with O_TRUNC and follows a link.  It is deleted: write a file of the editor'"'"'s own folder with wpe_write_own_bytes.  No allow line"
      else if (rule == "r25b")
        print "offender: " $1 " " $2 ": XDG_STATE_HOME, XDG_CONFIG_HOME, .local/state or a .config piece outside we_places.c.  The place of the user'"'"'s home, state and configuration is decided in one file: ask wpe_user_home, wpe_state_dir or wpe_config_dir.  A site still to move is a line of the allow file with its pass"
      else if (rule == "r25c")
        print "offender: " $1 " " $2 ": a place of the user'"'"'s falls back on the working folder (home = \".\", ./.local, a snprintf of a dot name beside a getenv).  The folder the editor was started in may belong to a repository the user only cloned: with no home folder there is no place, and the caller says so.  No allow line"
      else if (rule == "r25d")
        print "offender: " $1 " " $2 ": wpe_state_dir is called outside we_places.c, write_own_base (we_write.c), wpe_crash_path_init (we_unix.c) and startcmd_state_dir (we_startcmd.c).  A feature asks wpe_own_dir for its folder, which judges it, and never builds the path itself.  No allow line"
      else if (rule == "r25e")
        print "offender: " $1 " " $2 ": a literal that starts with /tmp/ and is not a mkstemp or mkdtemp template (ending in XXXXXX) is a fixed name in a folder others can write.  A site still to move is a line of the allow file with its pass"
      else if (rule == "r25f")
        print "offender: " $1 " " $2 ": e_ai_resume_session or e_ai_last_session_id is assigned outside wpe_ai_resume_id_take.  The word after --resume is an id or nothing: go through the setter.  No allow line"
      else if (rule == "r25g")
        print "offender: " $1 " " $2 ": wpe_write_own_bytes or wpe_write_own_remove is called with a first argument that is not a WPE_OWN_ kind.  The owner judges its own folder; a variable there could carry a folder nobody judged.  No allow line"
      else if (rule == "r25i")
        print "offender: " $1 " " $2 ": getenv(\"HOME\") or getpwuid( outside we_places.c.  Where the user'"'"'s home is has one owner: ask wpe_user_home (or wpe_user_database_home when the database'"'"'s answer is wanted on purpose).  A use that is not a place of the editor'"'"'s own (the ~ of a path the user typed, the name of a file'"'"'s owner) is a line of the allow file with that reason"
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
