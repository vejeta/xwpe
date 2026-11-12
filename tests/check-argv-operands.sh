#!/bin/sh
# check-argv-operands.sh -- every path a build hands to a program has one owner.
#
# A value a folder's xwperc or a project file sets (Exedir, FILES, LIBNAME,
# the program's name) that reaches a compiler, linker or librarian argv as a
# word starting with '-' or '@' is read as an option or as a response file
# full of options (gcc's -wrapper runs any program).  e_build_operand
# (we_prog.c) is the one join that prevents it.  This check lists every
# line of the build code that hands a path to a program (e_add_arg,
# e_make_library) or names Exedir, and fails when one is not in
# tests/argv-operands.allow -- a new join must say there which function
# built its path -- or when a listed line no longer exists.
top="${srcdir:-.}"
allow="$top/tests/argv-operands.allow"
files="we_prog.c we_debug.c"
pattern='e_prog\.exedir|e_add_arg\(|e_make_library\('
fail=0

# allowed FILE LINE - is the trimmed source line listed for the file?
allowed() {
  while IFS='|' read -r afile aline areason; do
    case "$afile" in ''|\#*) continue ;; esac
    if [ "$afile" = "$1" ] && [ "$aline" = "$2" ]; then
      return 0
    fi
  done < "$allow"
  return 1
}

# trim TEXT - the text without its leading blanks
trim() {
  printf '%s' "$1" | awk '{ sub(/^[ \t]+/, ""); print }'
}

for f in $files; do
  grep -nE "$pattern" "$top/$f" | while IFS= read -r hit; do
    num=${hit%%:*}
    text=$(trim "${hit#*:}")
    case "$text" in
      \**|/\**) continue ;;                     # a comment line
    esac
    case "$text" in
      *e_build_operand\(*) continue ;;          # the owner itself, joining
    esac
    if ! allowed "$f" "$text"; then
      echo "not built by e_build_operand: $f:$num: $text"
      echo "x" >&3
    fi
  done 3>&1 1>&2 | grep -q x && fail=1
done

# every listed line must still exist
while IFS='|' read -r afile aline areason; do
  case "$afile" in ''|\#*) continue ;; esac
  found=0
  grep -nE "$pattern" "$top/$afile" | while IFS= read -r hit; do
    if [ "$(trim "${hit#*:}")" = "$aline" ]; then
      echo found
    fi
  done | grep -q found && found=1
  if [ "$found" = 0 ]; then
    echo "stale entry in tests/argv-operands.allow: $afile: $aline" >&2
    fail=1
  fi
done < "$allow"

if [ "$fail" = 0 ]; then
  echo "check-argv-operands: ok"
  exit 0
fi
echo "check-argv-operands: FAIL" >&2
exit 1
