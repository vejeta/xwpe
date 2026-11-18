#!/bin/sh
# check-mode-premise.sh -- a test that sets a special bit or a mode that keeps
# a user out states what it assumes about the user running it.
#
# On the BSDs a new file takes its folder's group, and a user outside that
# group is refused the setgid bit.  A test that put a setgid file in /tmp as
# an ordinary user died in its setup with a bare "Operation not permitted"
# and hid every row after it.  The folder, the probe and the SKIP-or-FAIL of
# such a premise come from one helper (tests/testdir.h for C tests,
# tests/mode_premise.py for pytest rows); this check fails a test that sets
# such a mode without it.
#
#   check-mode-premise.sh [--dir TESTSDIR] [--selftest]
#
# Rules (strings and comments are blanked; a literal that follows & or ~ is a
# mask, not a mode being set):
#  1. A tests/*.c with a special-bit mode (a five-character octal literal
#     0[1-7]nnn, or S_ISUID, S_ISGID, S_ISVTX) includes "testdir.h", calls
#     test_premise_special_bits_settable( and does not call mkdtemp itself.
#  2. In such a file a line with such a mode calls none of chmod, fchmod,
#     fchmodat, mkdir, mkdirat, open, openat, creat: the helpers
#     (test_put_mode, test_mkdir_mode, test_chmod_mode), the owner under test
#     and the assertions are what take a mode there.
#  3. A tests/*.py (and tests/x11, tests/wayland) with a special-bit mode
#     (0o[1-7]nnn, or S_ISUID, S_ISGID, S_ISVTX, bare or as stat.S_IS...)
#     calls workdir_in_my_group( and special_bits_settable(.
#  4. A tests/*.c that sets a mode which keeps a user out (0000, 0100 ... 0500
#     in chmod, fchmod, mkdir, open, put, put_file or a test_*_mode call)
#     includes "testdir.h", calls test_premise(, and calls the probe for it:
#     test_premise_file_0200_is_unreadable( for 0000 and 0200,
#     test_premise_folder_refuses_new_files( for the others.  The pytest rows
#     that use 0o444, 0o555 and 0o400 are not covered yet: they are the same
#     premise on the other side (BT1) and wait for their own pass.
#  5. In a tests/*.c the first argument of test_premise( is a call of a
#     test_premise_* probe (the permission the system must honour for this
#     user), an integer literal (the harness testing itself), or an
#     identifier that the same file assigns from such a probe.  Any other
#     expression (a comparison, a call of a function of the test itself) is a
#     way to watch the system, a /proc file or a call a test can intercept,
#     and belongs to test_observation(, which is a SKIP by name whatever the
#     run says of its privileges.  The first argument is read from the line
#     of the call; the identifier is judged by the file, not the function.
#
# What the scan cannot see, and so does not judge: a mode that reaches a call
# through a variable or a macro (mode_t m = 02755; chmod(p, m)), a mode
# written as a decimal or hexadecimal literal, and a file that is neither a
# tests/*.c nor a tests/*.py of the folders above.  The rules name the
# literal, so a test that hides its mode there is outside them.
#
# --selftest feeds fixtures with one offence of each rule, and a file that
# must pass, and fails unless the scan names each offence and only those (a
# scanner that sees nothing is not a check).
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
TESTSDIR=
SELFTEST=0

while [ $# -gt 0 ]; do
  case $1 in
    --dir) shift; TESTSDIR=$1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "check-mode-premise: unknown argument $1" >&2; exit 1 ;;
  esac
  shift
done
[ -n "$TESTSDIR" ] || TESTSDIR=$SRCDIR/tests

. "$(dirname "$0")/workdir.sh"
work_dir mode-premise 1

# The scanner.  kind is c or py; the output is one line per offence,
# "file:line: what".  Comments and strings are blanked first, so a mode that
# only a sentence names is not a mode that is set.
SCAN_AWK='
function blank_c(s,   out, i, n, c, d, q) {
  out = ""; n = length(s); i = 1
  while (i <= n) {
    c = substr(s, i, 1); d = substr(s, i, 2)
    if (in_cmt) {
      if (d == "*/") { in_cmt = 0; i += 2; out = out "  " } else { i++; out = out " " }
      continue
    }
    if (d == "/*") { in_cmt = 1; i += 2; out = out "  "; continue }
    if (d == "//") break
    if (c == "\"" || c == "\047") {
      q = c; out = out q; i++
      while (i <= n && substr(s, i, 1) != q) {
        if (substr(s, i, 1) == "\\") { i++; out = out " " }
        i++; out = out " "
      }
      out = out q; i++
      continue
    }
    out = out c; i++
  }
  return out
}
function blank_py(s,   out, i, n, c, d, q) {
  out = ""; n = length(s); i = 1
  while (i <= n) {
    c = substr(s, i, 1); d = substr(s, i, 3)
    if (tq != "") {
      if (d == tq) { tq = ""; i += 3; out = out "   " } else { i++; out = out " " }
      continue
    }
    if (d == "\"\"\"" || d == "\047\047\047") { tq = d; i += 3; out = out "   "; continue }
    if (c == "#") break
    if (c == "\"" || c == "\047") {
      q = c; out = out q; i++
      while (i <= n && substr(s, i, 1) != q) {
        if (substr(s, i, 1) == "\\") { i++; out = out " " }
        i++; out = out " "
      }
      out = out q; i++
      continue
    }
    out = out c; i++
  }
  return out
}
# 1 when s holds a match of re whose previous non-blank character is neither
# & nor ~: a literal after those is a mask, not a mode being set.
function unmasked(s, re,   t, off, st, before, pc) {
  t = s; off = 0
  while (match(t, re)) {
    st = RSTART
    if (substr(t, st, 1) !~ /[A-Za-z0-9_]/) st++
    before = substr(s, 1, off + st - 1)
    sub(/[ \t]+$/, "", before)
    pc = (before == "") ? "" : substr(before, length(before), 1)
    if (pc != "&" && pc != "~") return 1
    t = substr(t, st + 1); off += st
  }
  return 0
}
function calls(s, re) { return match(s, re) > 0 }
# The text of the first argument of the call whose "(" is just before
# position p of s: up to the first comma or closing paren of its own level.
function first_arg(s, p,   depth, i, n, c) {
  depth = 0; n = length(s)
  for (i = p; i <= n; i++) {
    c = substr(s, i, 1)
    if (c == "(") depth++
    else if (c == ")") { if (depth == 0) break; depth-- }
    else if (c == "," && depth == 0) break
  }
  return substr(s, p, i - p)
}
# Rule 5 for one first argument of test_premise(.  A probe call is the
# permission the premise states; an integer literal is the harness testing
# itself; an identifier is judged at the end, when every assignment of the
# file is known.  Anything else (a comparison, a call of a function of the
# test itself) is a
# way to watch the system, which is test_observation(.
function premise_arg_ok(a,   v) {
  gsub(/^[ \t]+/, "", a); gsub(/[ \t]+$/, "", a)
  if (a ~ /^[0-9]+$/) return 1
  if (a ~ /^test_premise_[A-Za-z0-9_]*[ \t]*[(]/) return 1
  if (a ~ /^[A-Za-z_][A-Za-z0-9_]*$/) {
    if (!(a in used_at)) used_at[a] = FNR
    return 1
  }
  return 0
}
# Rule 5 for one line: every test_premise( call on it, and every identifier
# assigned from a probe.
function note_premises(line,   t, off, a) {
  t = line
  while (match(t, /(^|[^A-Za-z0-9_])[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*test_premise_[A-Za-z0-9_]*[ \t]*[(]/)) {
    a = substr(t, RSTART, RLENGTH); sub(/[ \t]*=.*/, "", a); sub(/^[^A-Za-z_]/, "", a)
    from_probe[a] = 1
    t = substr(t, RSTART + RLENGTH)
  }
  t = line
  while (match(t, /(^|[^A-Za-z0-9_])test_premise[ \t]*[(]/)) {
    off = RSTART + RLENGTH
    a = first_arg(t, off)
    if (!premise_arg_ok(a))
      say("rule 5: test_premise( takes a probe of a permission (test_premise_*); a way to watch the system is test_observation(", FNR)
    t = substr(t, off)
  }
}
function say(msg, ln) { print file ":" ln ": " msg; bad++ }
BEGIN {
  if (kind == "c") {
    spec_re = "(^|[^A-Za-z0-9_])(0[1-7][0-7][0-7][0-7]|S_ISUID|S_ISGID|S_ISVTX)([^A-Za-z0-9_]|$)"
    deny_re = "(^|[^A-Za-z0-9_])0[0-5]00([^A-Za-z0-9_]|$)"
    raw_re = "(^|[^A-Za-z0-9_])(chmod|fchmod|fchmodat|mkdir|mkdirat|open|openat|creat)[ \t]*[(]"
    set_re = "(^|[^A-Za-z0-9_])(chmod|fchmod|fchmodat|mkdir|mkdirat|open|openat|creat|put|put_file|test_put_mode|test_mkdir_mode|test_chmod_mode)[ \t]*[(]"
    mkd_re = "(^|[^A-Za-z0-9_])mkdtemp[ \t]*[(]"
  } else {
    spec_re = "(^|[^A-Za-z0-9_])(0o[1-7][0-7][0-7][0-7]|S_ISUID|S_ISGID|S_ISVTX)([^A-Za-z0-9_]|$)"
    grp_re = "(^|[^A-Za-z0-9_])workdir_in_my_group[ \t]*[(]"
    bit_re = "(^|[^A-Za-z0-9_])special_bits_settable[ \t]*[(]"
  }
}
{
  raw = $0
  if (kind == "c") {
    if (raw ~ /^[ \t]*#[ \t]*include[ \t]*"testdir[.]h"/) has_include = 1
    line = blank_c(raw)
  } else {
    line = blank_py(raw)
  }
  if (kind == "c") {
    if (unmasked(line, spec_re)) {
      if (!has_special) first_special = FNR
      has_special = 1
      if (calls(line, raw_re)) say("rule 2: a special-bit mode is set by a bare chmod, mkdir, open or creat; use test_put_mode, test_mkdir_mode or test_chmod_mode", FNR)
    }
    if (calls(line, mkd_re)) { mkd_lines = mkd_lines " " FNR }
    if (unmasked(line, deny_re) && calls(line, set_re)) {
      has_deny = 1
      if (line ~ /(^|[^0-9A-Za-z_])0[02]00([^0-9A-Za-z_]|$)/) need_file = 1
      if (line ~ /(^|[^0-9A-Za-z_])0[1345]00([^0-9A-Za-z_]|$)/) need_folder = 1
    }
    note_premises(line)
    if (line ~ /(^|[^A-Za-z0-9_])test_premise[ \t]*[(]/) has_premise = 1
    if (line ~ /(^|[^A-Za-z0-9_])test_premise_special_bits_settable[ \t]*[(]/) has_bits_probe = 1
    if (line ~ /(^|[^A-Za-z0-9_])test_premise_file_0200_is_unreadable[ \t]*[(]/) has_file_probe = 1
    if (line ~ /(^|[^A-Za-z0-9_])test_premise_folder_refuses_new_files[ \t]*[(]/) has_folder_probe = 1
  } else {
    if (unmasked(line, spec_re)) { if (!has_special) first_special = FNR; has_special = 1 }
    if (calls(line, grp_re)) has_grp = 1
    if (calls(line, bit_re)) has_bit = 1
  }
}
END {
  if (kind == "c") {
    for (v in used_at)
      if (!(v in from_probe))
        say("rule 5: test_premise( is given " v ", which is not assigned from a test_premise_* probe in this file; a way to watch the system is test_observation(", used_at[v])
    if (has_special && !has_include) say("rule 1: sets a special-bit mode and does not include testdir.h", first_special)
    if (has_special && !has_bits_probe) say("rule 1: sets a special-bit mode and never calls test_premise_special_bits_settable(", first_special)
    if (has_special && mkd_lines != "") say("rule 1: calls mkdtemp itself (line" mkd_lines "); use test_dir_make or test_dir_make_under", 0)
    if (has_deny && !has_include) say("rule 4: sets a mode that keeps a user out and does not include testdir.h", 0)
    if (has_deny && !has_premise) say("rule 4: sets a mode that keeps a user out and never calls test_premise(", 0)
    if (need_file && !has_file_probe) say("rule 4: sets 0000 or 0200 and never calls test_premise_file_0200_is_unreadable(", 0)
    if (need_folder && !has_folder_probe) say("rule 4: sets 0100 to 0500 and never calls test_premise_folder_refuses_new_files(", 0)
  } else {
    if (has_special && !has_grp) say("rule 3: sets a special-bit mode and never calls workdir_in_my_group(", first_special)
    if (has_special && !has_bit) say("rule 3: sets a special-bit mode and never calls special_bits_settable(", first_special)
  }
  exit bad ? 1 : 0
}
'

# scan_dir DIR: every file of the rules, offences on stdout; the number of
# files read is the first line of $WORK/count.
scan_dir() {
  n=0
  for f in "$1"/*.c; do
    [ -f "$f" ] || continue
    n=$((n + 1))
    awk -v kind=c -v file="${f#"$1"/}" "$SCAN_AWK" "$f"
  done
  for f in "$1"/*.py "$1"/x11/*.py "$1"/wayland/*.py; do
    [ -f "$f" ] || continue
    case $f in */mode_premise.py) continue ;; esac
    n=$((n + 1))
    awk -v kind=py -v file="${f#"$1"/}" "$SCAN_AWK" "$f"
  done
  echo "$n" > "$WORK/count"
}

# selftest_expect WHAT OUTPUT PATTERN...: OUTPUT holds every PATTERN.
selftest_failed=0
selftest_expect() {
  what=$1 out=$2
  shift 2
  ok=1
  for pat in "$@"; do
    printf '%s\n' "$out" | grep -qF -- "$pat" || { ok=0; echo "  output lacks: $pat"; }
  done
  if [ $ok = 1 ]; then
    echo "ok: $what"
  else
    echo "FAIL: $what"
    printf '%s\n' "$out" | sed 's/^/    | /'
    selftest_failed=1
  fi
}

selftest() {
  fx=$WORK/fx
  mkdir -p "$fx"
  printf '%s\n' '#include <sys/stat.h>' 'static void row(void) { chmod("x", 02755); }' > "$fx/bad_none.c"
  printf '%s\n' '#include "testdir.h"' 'static void row(void) { chmod("x", 02755); }' > "$fx/bad_raw.c"
  printf '%s\n' '#include "testdir.h"' 'static void row(char *d) { test_premise(test_premise_special_bits_settable(d, S_ISGID), "r", "m"); test_chmod_mode(d, "x", 02755); }' > "$fx/good_helper.c"
  printf '%s\n' '#include "testdir.h"' 'static void row(char *d) { test_chmod_mode(d, "x", 02755); }' > "$fx/bad_noprobe.c"
  printf '%s\n' 'import os' 'from stat import S_ISGID' 'def test_a(p):' '    os.chmod(p, 0o644 | S_ISGID)' > "$fx/bad_py_bare.py"
  printf '%s\n' '#include <sys/stat.h>' 'static int m(struct stat *s) { return s->st_mode & 07777; }' \
    'static void row(void) { /* a 02755 file */ chmod("x", 0644); puts("0500"); }' > "$fx/good_mask.c"
  printf '%s\n' '#include "testdir.h"' 'static void row(char *d) { char b[9]; mkdtemp(b); test_chmod_mode(d, "x", 04755); }' > "$fx/bad_mkdtemp.c"
  printf '%s\n' '#include "testdir.h"' 'static void row(char *d) { test_put_mode(d, "x", "t", 0200); }' > "$fx/bad_deny.c"
  printf '%s\n' '#include "testdir.h"' \
    'static void row(char *d) { test_premise(test_premise_file_0200_is_unreadable(d), "r", "m"); test_put_mode(d, "x", "t", 0200);' \
    'if (test_premise(test_premise_folder_refuses_new_files(d), "r", "m")) chmod(d, 0500); }' > "$fx/good_deny.c"
  printf '%s\n' 'import os' 'def test_a(tmp_path):' '    os.chmod(str(tmp_path), 0o2755)' > "$fx/bad_py.py"
  printf '%s\n' 'import os, stat' 'from mode_premise import special_bits_settable, workdir_in_my_group' \
    'def test_a(tmp_path):' '    workdir_in_my_group(str(tmp_path))' '    special_bits_settable(str(tmp_path), stat.S_ISGID)' \
    '    os.chmod(str(tmp_path), 0o2755)' > "$fx/good_py.py"
  printf '%s\n' '"""A 0o2755 folder, said in words."""' 'import os' 'def test_a(p):' '    return os.stat(p).st_mode & 0o7777' > "$fx/good_py_mask.py"
  printf '%s\n' '#include "testdir.h"' \
    'static void row(char *p) { if (!test_premise(fopen(p, "r") != NULL, "r", "m")) return; }' > "$fx/bad_observation.c"
  printf '%s\n' '#include "testdir.h"' \
    'static void row(char *d, int seen) { int ok = test_premise_special_bits_settable(d, S_ISGID);' \
    'test_premise(ok, "r", "m"); test_premise(1, "r", "m"); test_premise(test_premise_file_0200_is_unreadable(d), "r", "m");' \
    'if (!test_premise(test_premise_folder_refuses_new_files(d), "r", "m")) return; test_observation(seen, "r", "m"); }' > "$fx/good_premise_forms.c"
  printf '%s\n' '#include "testdir.h"' \
    'static void row(int seen) { test_premise(seen, "r", "m"); }' > "$fx/bad_identifier.c"
  out=$(scan_dir "$fx")
  selftest_expect "a premise that watches the system" "$out" "bad_observation.c:2: rule 5"
  selftest_expect "a premise given an identifier no probe assigned" "$out" "bad_identifier.c:2: rule 5"
  selftest_expect "a special-bit chmod with no helper" "$out" \
    "bad_none.c:2: rule 2" "bad_none.c:2: rule 1: sets a special-bit mode and does not include testdir.h"
  selftest_expect "a bare chmod with the include" "$out" "bad_raw.c:2: rule 2"
  selftest_expect "a special bit with no probe" "$out" \
    "bad_noprobe.c:2: rule 1: sets a special-bit mode and never calls test_premise_special_bits_settable("
  selftest_expect "a bare S_ISGID in a pytest row" "$out" \
    "bad_py_bare.py:2: rule 3: sets a special-bit mode and never calls workdir_in_my_group("
  selftest_expect "a mkdtemp of its own" "$out" "bad_mkdtemp.c:0: rule 1: calls mkdtemp itself"
  selftest_expect "a deny mode with no premise" "$out" \
    "bad_deny.c:0: rule 4: sets a mode that keeps a user out and never calls test_premise(" \
    "bad_deny.c:0: rule 4: sets 0000 or 0200 and never calls test_premise_file_0200_is_unreadable("
  selftest_expect "a pytest row with no helper" "$out" \
    "bad_py.py:3: rule 3: sets a special-bit mode and never calls workdir_in_my_group(" \
    "bad_py.py:3: rule 3: sets a special-bit mode and never calls special_bits_settable("
  for good in good_helper.c good_mask.c good_deny.c good_py.py good_py_mask.py good_premise_forms.c; do
    if printf '%s\n' "$out" | grep -q "^$good:"; then
      echo "FAIL: $good must pass, and the scan named it:"
      printf '%s\n' "$out" | grep "^$good:" | sed 's/^/    | /'
      selftest_failed=1
    else
      echo "ok: $good passes"
    fi
  done
  if [ $selftest_failed = 0 ]; then
    echo "check-mode-premise: selftest ok"
    return 0
  fi
  echo "check-mode-premise: FAIL (selftest)"
  return 1
}

if [ $SELFTEST = 1 ]; then
  selftest
  exit $?
fi

out=$(scan_dir "$TESTSDIR")
scanned=$(cat "$WORK/count")
status=0
if [ "${scanned:-0}" -eq 0 ]; then
  echo "check-mode-premise: FAIL: no test file was read under $TESTSDIR (the scan is blind)"
  status=1
fi
if [ -n "$out" ]; then
  echo "check-mode-premise: FAIL: a test sets a special-bit or deny mode without stating its premise:"
  printf '%s\n' "$out" | sed 's/^/  /'
  status=1
fi
sh "$0" --selftest > /dev/null 2>&1 || { echo "check-mode-premise: FAIL: canary: the scan is blind (run $0 --selftest)"; status=1; }
[ $status = 0 ] && echo "check-mode-premise: ok ($scanned files)"
exit $status
