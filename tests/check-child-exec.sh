#!/bin/sh
# check-child-exec.sh -- what a program the editor starts may do between the
# fork and the exec: only what takes no lock and allocates nothing.
#
# The graphical editor runs a library thread (Pango's font thread) beside the
# main one.  fork() copies the calling thread alone, so every lock that thread
# held at that instant stays locked in the child with nobody to release it,
# and a child that formats a message, looks up a translation or touches a
# stdio stream can wait forever before its program ever runs.  The owner of
# child starts (we_child.c) therefore keeps the child path to system calls,
# the signal calls, memcpy and execvp, and prepares every byte in the parent.
# This script reads the compiled child path and fails when it reaches
# anything else.
#
#   check-child-exec.sh [--srcdir DIR] [--allow FILE] [--selftest] [--list]
#
# What it does:
#  1. Compiles we_child.c with the build's own flags (MAINT_CC, MAINT_FLAGS)
#     plus -ffunction-sections -fdata-sections -fno-lto last, so every
#     function is a section of its own and a call to a function in the same
#     file is a relocation against that section.  -flto*, -fsanitize*,
#     --coverage, -fprofile*, -ftest-coverage and -fno-plt are removed from
#     the flags and named: they change which calls exist (clang's -fno-plt
#     calls through a register, which the indirect call rule must refuse).
#  2. Reads the object's relocations (objdump -r) and follows them from the
#     entry, the section .text.wpe_child_exec or one of its clones
#     (.isra/.part/.constprop), through every text section, every
#     .data.rel.ro* and .rodata* section (a table of function pointers) and
#     every helper reached, to the symbols that leave the object.  Each must
#     be a line of tests/child-exec-safe.allow:
#
#         symbol|mark|reason[|fact]
#
#     marks SAFE (in signal-safety(7)), SYSCALL (not in that list, a thin
#     wrapper of one system call), EXEC (the exec call), RUNTIME (the
#     compiler's or the libc's own symbol, a libc variable such as environ
#     included) and DATA (a variable of the tree's own objects, read from
#     .data or .bss).  A reached symbol with no line fails; a line nothing
#     reaches is stale and fails.  The optional fact is a preprocessor
#     expression (WPE_HAS_CLOSE_RANGE, defined(__SSP__)...), evaluated with
#     -Werror=undef: it says when the symbol is reached.  When it is false in
#     this build the line is exempt from staleness (printed), a symbol that is
#     reached anyway fails, and a fact that does not evaluate, or the literal
#     0, fails.  A relocation into .data or .bss is a DATA symbol that needs
#     its line: a call through a global hook pointer shows up there.  A
#     relocation into a section of a kind the reader does not know fails.
#  3. On x86-64 no instruction of the closure may be an indirect call or jump
#     (call *, jmp *): the target is invisible to a relocation read, and a
#     tail call is a jmp.  Switch tables are off (-fno-jump-tables).  A call
#     through a slot the linker fills (call *0x0(%rip), what -fno-plt emits)
#     has its relocation read and is not one.  Elsewhere that sub-check says
#     it was skipped.
#  4. The source rule: we_child.c has exactly one fork primitive (fork,
#     vfork, forkpty, _Fork, clone, rfork, also spelt (fork)()), and it reads
#     `pid = fork();` followed at once by `if (pid == 0) wpe_child_exec(
#     names);`, no other statement between, no call in the arguments, no
#     other condition.  Anything else would run in the child outside the part
#     that is read.  No product source calls pthread_atfork: its child
#     handler would run in the child outside both halves.
#
# How it can go blind, and what it does about each:
#  - Code in the child that the object half does not see: a statement between
#    the fork and the entry, a call in the entry's arguments, an extra
#    condition, a second fork primitive, an atfork handler: the source rule
#    fails each of them.
#  - A helper in a section of its own kind (section(".mytext")) that the
#    closure would not follow: a section of an unknown kind is a problem.
#  - An indirect tail call or computed jump (jmp *), or a switch table: jmp *
#    is refused like call *, and jump tables are not generated.
#  - A fact with a misspelt name, a syntax error or the literal 0 that makes a
#    line exempt without a reason: each is a problem, and a symbol reached
#    while its fact is false is one too.
#  - The entry inlined into its caller (a parent function): no entry section,
#    so the script fails as blind.  WPE_CHILD_NOINLINE keeps it.
#  - LTO or bitcode objects have no readable sections: blind.
#  - A compiler or objdump that prints another format: the canaries fail.
#  - Another system's #if arms are not read by a run here, and a
#    different optimisation level or fortify setting changes the set of
#    reached names: the allow lines for those carry a fact.
#  - GNU objdump and llvm-objdump are both read.  A non-ELF object (Mach-O
#    has no .text.<name> sections) is SKIPPED with the reason, and with the
#    note that this system's own #if arms are read by no run.
#
# Canaries: planted sources, compiled with the same flags at -O2 and -O0,
# each named, silent or blind as it must be; a canary that is not seen fails
# the script as blind.  --selftest runs only them.  --list prints what the
# tree's child path reaches, as "kind symbol from" lines.
#
# Environment (set by `make check`): MAINT_CC, MAINT_FLAGS, srcdir, OBJDUMP
# (optional).  Run by hand without MAINT_CC it measures nothing and says to
# use `make check TESTS=tests/check-child-exec.sh` (exit 2).
# Exit: 0 clean, 1 offenders or blind, 2 run outside make check, 77 skipped.

SRCDIR=${srcdir:-.}
ALLOW=
MODE=all
BAD=1
SKIP=77
ENTRY=wpe_child_exec

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --allow) shift; ALLOW=$1 ;;
    --selftest) MODE=selftest ;;
    --list) MODE=list ;;
    *) echo "check-child-exec: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done

[ -n "$ALLOW" ] || ALLOW=$SRCDIR/tests/child-exec-safe.allow

# The object reader: the table of section kinds and the spelling table that
# the closure below judges every relocation by.
HERE=$(dirname "$0")
. "$HERE/objsyms.sh"

if [ -z "${MAINT_CC:-}" ]; then
  echo "check-child-exec: run it as: make check TESTS=tests/check-child-exec.sh" >&2
  echo "  (the flags of the build live in the Makefile)" >&2
  exit 2
fi

# find_objdump - the tool that reads the object: $OBJDUMP, GNU objdump or
# llvm-objdump.
find_objdump() {
  if [ -n "${OBJDUMP:-}" ] && command -v "$OBJDUMP" >/dev/null 2>&1; then
    return 0
  fi
  for cand in objdump llvm-objdump gobjdump; do
    if command -v "$cand" >/dev/null 2>&1; then
      OBJDUMP=$cand
      return 0
    fi
  done
  return 1
}

if ! find_objdump; then
  echo "check-child-exec: SKIP, no objdump or llvm-objdump to read the object"
  exit $SKIP
fi

. "$(dirname "$0")/workdir.sh"
work_dir child-exec $BAD

# --- the flags -----------------------------------------------------------

KEEP=
STRIPPED=

# clean_flags - KEEP is MAINT_FLAGS without the flags that change which calls
# exist; STRIPPED names what was removed.
clean_flags() {
  set -f
  # shellcheck disable=SC2086
  set -- $MAINT_FLAGS
  set +f
  for flag in "$@"; do
    case $flag in
      -flto*|-fsanitize*|--coverage|-fprofile*|-ftest-coverage|-fno-plt)
        STRIPPED="$STRIPPED $flag" ;;
      *) KEEP="$KEEP $flag" ;;
    esac
  done
}

# compile SOURCE OBJECT [FLAG...] - the object the check reads: the build's
# flags, then FLAG..., then the four that make it readable, last (no jump
# table, so a jmp * that remains is a computed jump and not a switch).
compile() {
  cc_src=$1; cc_obj=$2; shift 2
  # shellcheck disable=SC2086
  $MAINT_CC $KEEP "$@" -ffunction-sections -fdata-sections -fno-lto \
    -fno-jump-tables -c "$cc_src" -o "$cc_obj"
}

# find_thunk_flag - the flag that makes this compiler call through a register
# by a thunk (retpoline), or nothing.  OpenBSD's clang does it by default, so
# there every indirect call is "call __llvm_retpoline_r11" and a canary needs
# no flag; elsewhere the thunk canary is compiled with the flag so the thunk
# rule of indirect_calls is proved on any x86-64 system whose compiler has one.
# Each candidate is tried on an empty source and kept only when accepted.
find_thunk_flag() {
  THUNK_FLAG=
  printf 'int canary_probe;\n' > "$WORK/thunk-probe.c"
  for cand in -mindirect-branch=thunk -mretpoline; do
    # shellcheck disable=SC2086
    if $MAINT_CC $cand -c "$WORK/thunk-probe.c" -o "$WORK/thunk-probe.o" > /dev/null 2>&1; then
      THUNK_FLAG=$cand
      return 0
    fi
  done
  return 0
}

# object_is_elf OBJECT
object_is_elf() {
  "$OBJDUMP" -f "$1" 2>/dev/null | grep -Ei 'file format elf' >/dev/null
}

# object_is_x86_64 OBJECT
object_is_x86_64() {
  "$OBJDUMP" -f "$1" 2>/dev/null | grep -Ei 'architecture: *(i386:)?x86[-_]64' >/dev/null
}

# --- the closure ---------------------------------------------------------

# reach OBJECT - the records of what the child path reaches, one per line:
# "ENTRY section" for each entry section found, "LTO" when the object holds
# LTO sections, "S section" for each section of the closure, "U symbol
# section spelling" for a symbol that leaves the object (the symbol as the C
# source spells it, the spelling the object imports) and "D symbol section"
# for a variable of the object.
reach() {
  "$OBJDUMP" -t "$1" > "$WORK/t.txt" 2>/dev/null
  "$OBJDUMP" -h "$1" > "$WORK/h.txt" 2>/dev/null
  "$OBJDUMP" -r "$1" > "$WORK/r.txt" 2>/dev/null
  reach_files "$WORK/t.txt" "$WORK/h.txt" "$WORK/r.txt"
}

# reach_files SYMBOLS SECTIONS RELOCATIONS - reach on the three listings
# (objdump -t, -h, -r) of an object: what the object reader's tables make of
# them (the kind of each section a relocation lands in, the source spelling
# of each symbol that leaves).  The canaries feed it listings of their own.
reach_files() {
  LC_ALL=C awk -v tfile="$1" -v hfile="$2" -v rfile="$3" "$OBJSYMS_AWK_LIB"'
    function addend(t) {
      sub(/[-+](0x[0-9a-fA-F]+|[0-9]+)$/, "", t)
      return t
    }
    function visit(s) {
      if (s in seen) return
      seen[s] = 1
      q[++qt] = s
      print "S " s
    }
    function classify(from, s, name,   kind) {
      if (s == "*COM*") { print "D " name " " from; return }
      kind = objsyms_section_kind(s)
      if (kind == "skip") return
      if (kind == "visit") { visit(s); return }
      if (kind == "data") {
        if (name == "") {
          name = s
          sub(/^\.(data\.rel\.local|data\.rel|data|bss|tdata|tbss)\.?/, "", name)
          if (name == "") name = s
        }
        print "D " name " " from
        return
      }
      # A section of a kind this reader does not know could hold code it
      # does not follow: a problem, never a pass.
      print "X " s " " from
    }
    function follow(from, raw,   t) {
      t = addend(raw)
      if (t == "" || t ~ /^\.L/ || t ~ /^_GLOBAL_OFFSET_TABLE_/ || t ~ /^\.TOC\./)
        return
      if (t in defsec) {
        classify(from, defsec[t], (t ~ /^\./) ? "" : t)
        return
      }
      if (t ~ /^\./) { classify(from, t, ""); return }
      print "U " objsyms_source_name(t) " " from " " t
    }
    FILENAME == tfile {
      if ($1 ~ /^[0-9a-fA-F]+$/ && NF >= 4) {
        sec = ""
        for (i = 2; i < NF; i++)
          if ($i ~ /^[.*]/) { sec = $i; break }
        if (sec != "" && sec != "*UND*" && sec != "*ABS*") defsec[$NF] = sec
      }
      next
    }
    FILENAME == hfile {
      if ($1 ~ /^[0-9]+$/ && $2 ~ /^\./) {
        if ($2 ~ /^\.gnu\.lto_/) lto = 1
        if ($2 ~ /^\.text\.wpe_child_exec(\.(isra|part|constprop)\.[0-9]+)*$/)
          entries[$2] = 1
      }
      next
    }
    FILENAME == rfile {
      if ($0 ~ /^RELOCATION RECORDS FOR \[/) {
        cur = $0
        sub(/^[^[]*\[/, "", cur)
        sub(/\].*/, "", cur)
        next
      }
      if (cur != "" && $1 ~ /^[0-9a-fA-F]+$/ && NF >= 3) {
        n[cur]++
        e[cur, n[cur]] = $3
      }
      next
    }
    END {
      if (lto) print "LTO"
      for (s in entries) { print "ENTRY " s; visit(s) }
      while (qh < qt) {
        s = q[++qh]
        for (i = 1; i <= n[s]; i++) follow(s, e[s, i])
      }
    }' "$1" "$2" "$3" | sort -u
}

# indirect_calls OBJECT SECTION... - each indirect call or jump instruction
# (an indirect tail call is a jmp *) in the sections, as a problem line.
indirect_calls() {
  obj=$1; shift
  for sec in "$@"; do
    "$OBJDUMP" -d -j "$sec" "$obj" 2>/dev/null |
      grep -E '(call|jmp)[a-z]*[[:space:]]+\*' |
      grep -vE '(call|jmp)[a-z]*[[:space:]]+\*(0x)?[0-9a-f]*\(%rip\)' |
      while IFS= read -r ins; do
        echo "indirect call or jump in $sec (a target relocations cannot see):$ins"
      done
    # A call through a register that the compiler made by a thunk
    # (retpoline): no call * in the object, a call to the thunk, either a
    # relocation against it or its body in the object.
    "$OBJDUMP" -dr -j "$sec" "$obj" 2>/dev/null |
      grep -E "R_X86_64_[A-Z0-9]+[[:space:]]+(__llvm_(external_)?retpoline_|__x86_indirect_thunk_)|(call|jmp)[a-z]*[[:space:]]+[0-9a-fx]+[[:space:]]+<(__llvm_(external_)?retpoline_|__x86_indirect_thunk_)" |
      while IFS= read -r ins; do
        echo "indirect call or jump in $sec (a call through a thunk, so a target relocations cannot see):$ins"
      done
  done
}

# fact_holds EXPRESSION - evaluate the fact with the build's flags (config.h
# is among them), an undefined name being an error: returns 0 when it is
# true, 1 when it is false, 2 when it does not evaluate (a misspelt name, a
# syntax error), which the caller reports as a problem.
fact_holds() {
  printf '#if %s\nfactholds\n#endif\n' "$1" > "$WORK/fact.c"
  # shellcheck disable=SC2086
  out=$($MAINT_CC $KEEP -E -P -Werror=undef "$WORK/fact.c" 2>/dev/null) || return 2
  case $out in
    *factholds*) return 0 ;;
    *) return 1 ;;
  esac
}

# exempt_symbols ALLOWFILE - on stdout, one line per allow line whose fact
# does not hold here: the symbol when the fact is false (nothing may reach
# it, and it is exempt from the stale rule), "!" and a problem when the fact
# is the literal 0 (a permanent exemption has no reason to be a fact) or does
# not evaluate.  The false ones are also appended to the file named by NOTES
# (printed at the end of a run, so an exemption is never silent).
exempt_symbols() {
  while IFS='|' read -r sym mark why fact; do
    case $sym in ''|'#'*|' '*) continue ;; esac
    [ -n "$fact" ] || continue
    [ "$fact" != optional ] || continue
    if [ "$fact" = 0 ]; then
      echo "!the fact of $sym is the literal 0, which exempts it for ever"
      continue
    fi
    fact_holds "$fact"
    case $? in
      0) : ;;
      1) echo "$sym"
         echo "$sym" >> "$NOTES" ;;
      *) echo "!the fact of $sym does not evaluate: $fact" ;;
    esac
  done < "$1"
}

# judge ALLOWFILE RECORDS STALE EXEMPT - what is wrong with the reached
# symbols: one problem a line, nothing when all is as the allow file says.
# STALE is 1 to fail a line that nothing reaches.
judge() {
  printf '%s\n' "$2" | awk -v allow="$1" -v stale="$3" -v exempt="$4" '
    BEGIN {
      while ((getline line < allow) > 0) {
        if (line ~ /^[ \t]*(#|$)/) continue
        n = split(line, f, "|")
        if (n < 3 || n > 4 || f[1] == "" || f[3] == "" ||
            f[2] !~ /^(SAFE|SYSCALL|EXEC|RUNTIME|DATA)$/ ||
            (n == 4 && f[4] == "optional" && f[2] != "SAFE")) {
          print "malformed allow line (optional goes only with SAFE): " line
          continue
        }
        if (f[1] in mark) { print "duplicate allow line: " f[1]; continue }
        mark[f[1]] = f[2]
        if (n == 4 && f[4] == "optional") opt[f[1]] = 1
      }
      while ((getline line < exempt) > 0) {
        if (substr(line, 1, 1) == "!") print substr(line, 2)
        else off[line] = 1
      }
    }
    $1 == "X" {
      print "reaches a section of a kind this check does not read: " $2 " (from " $3 ")"
      next
    }
    $1 == "U" || $1 == "D" {
      kind = $1; sym = $2
      if (sym in done) next
      done[sym] = 1
      # A libc-private spelling is judged as the call it stands for and
      # said as both: "fstat (spelt __fstat50)".
      spelt = ""
      if (kind == "U" && $4 != "" && $4 != sym) spelt = " (spelt " $4 ")"
      if (sym in off) {
        print "reaches " sym spelt " (from " $3 ") although its fact is false in this build"
      } else if (!(sym in mark)) {
        if (kind == "D")
          print "reaches the variable " sym " (from " $3 "): not in the allow file as DATA"
        else
          print "reaches " sym spelt " (from " $3 "): not in the allow file"
      } else if (kind == "D" && mark[sym] != "DATA") {
        print "the variable " sym " is marked " mark[sym] ", it must be DATA"
      } else if (kind == "U" && mark[sym] == "DATA") {
        print sym " is marked DATA but is reached as a symbol of another object (from " $3 ")"
      }
    }
    END {
      if (stale + 0 == 0) exit
      for (s in mark)
        if (!(s in done) && !(s in off) && !(s in opt))
          print "stale allow line: " s " (nothing in the child path reaches it)"
    }' | sort
}

# check_object OBJECT ALLOW STALE - the problems of one object, on stdout;
# returns 1 when there are any.  A blind reading is a problem.
check_object() {
  obj=$1; allow=$2; stale=$3
  if ! "$OBJDUMP" -h "$obj" >/dev/null 2>&1; then
    echo "blind: the object cannot be read (LTO bitcode, or an objdump that does not know it)"
    return 1
  fi
  records=$(reach "$obj")
  case $records in
    *LTO*)
      echo "blind: the object holds LTO sections, which carry no readable calls"
      return 1 ;;
  esac
  case $records in
    *ENTRY*) : ;;
    *)
      echo "blind: no section .text.$ENTRY (or clone) in the object: the entry was inlined away or renamed"
      return 1 ;;
  esac
  exempt=$WORK/exempt.txt
  : > "$exempt"
  exempt_symbols "$allow" > "$exempt"
  problems=$(judge "$allow" "$records" "$stale" "$exempt")
  if object_is_x86_64 "$obj"; then
    # shellcheck disable=SC2046
    calls=$(indirect_calls "$obj" $(printf '%s\n' "$records" | awk '$1 == "S" { print $2 }'))
    if [ -n "$calls" ]; then
      problems=$(printf '%s\n%s\n' "$problems" "$calls" | sed -n '/./p')
    fi
  fi
  if [ -n "$problems" ]; then
    printf '%s\n' "$problems"
    return 1
  fi
  return 0
}

# --- the source rule -----------------------------------------------------

# child_branch_problems FILE [ATFORK] - what is wrong with the fork in FILE:
# it must be the only fork primitive, and be followed at once by the child
# branch holding only the call of wpe_child_exec.  Comments and the text of
# strings are blanked.  With ATFORK set it only looks for pthread_atfork.
child_branch_problems() {
  awk -v file="$1" -v atfork="${2:-0}" '
    function blank(line,   out, n, i, c, d, instr, q) {
      q = "\047"; out = ""; n = length(line); i = 1; instr = 0
      while (i <= n) {
        c = substr(line, i, 1); d = substr(line, i, 2)
        if (incom) { if (d == "*/") { incom = 0; i += 2 } else i++; continue }
        if (instr) {
          if (c == "\\") { out = out "  "; i += 2; continue }
          if (c == "\"") { out = out c; instr = 0 } else out = out " "
          i++; continue
        }
        if (d == "/*") { incom = 1; i += 2; continue }
        if (d == "//") break
        if (c == q) {
          out = out c; i++
          while (i <= n && substr(line, i, 1) != q) {
            if (substr(line, i, 1) == "\\") { out = out " "; i++ }
            out = out " "; i++
          }
          out = out q; i++; continue
        }
        if (c == "\"") instr = 1
        out = out c; i++
      }
      return out
    }
    { all = all " " blank($0) }
    END {
      if (atfork + 0 != 0) {
        if (all ~ /pthread_atfork/)
          print file ": pthread_atfork: a handler would run in the child outside both halves of the check"
        exit
      }
      forks = 0
      nprim = split("fork vfork forkpty _Fork clone clone3 __clone rfork", prim, " ")
      for (k = 1; k <= nprim; k++) {
        s = all
        re = "(^|[^A-Za-z0-9_.>])" prim[k] "[ \t]*\\("
        while (match(s, re)) { forks++; s = substr(s, RSTART + RLENGTH) }
        s = all
        re = "\\([ \t]*" prim[k] "[ \t]*\\)[ \t]*\\("
        while (match(s, re)) { forks++; s = substr(s, RSTART + RLENGTH) }
      }
      if (forks != 1)
        print file ": " forks " fork primitives (fork, vfork, forkpty, _Fork, clone, rfork...), there must be exactly one"
      # The fork statement is followed at once by the child branch, and the
      # branch is the call of wpe_child_exec with plain names for arguments:
      # nothing else runs in the child outside the part that is read.
      arg = "&?[A-Za-z_][A-Za-z0-9_]*"
      head = "pid[ \t]*=[ \t]*fork[ \t]*\\([ \t]*\\)[ \t]*;[ \t]*if[ \t]*\\([ \t]*pid[ \t]*==[ \t]*0[ \t]*\\)[ \t]*"
      call = "wpe_child_exec[ \t]*\\([ \t]*" arg "([ \t]*,[ \t]*" arg ")*[ \t]*\\)[ \t]*;"
      pat = head call "|" head "\\{[ \t]*" call "[ \t]*\\}"
      matched = 0; s = all
      while (match(s, pat)) { matched++; s = substr(s, RSTART + RLENGTH) }
      if (matched != 1)
        print file ": the fork must be `pid = fork();` followed at once by `if (pid == 0) wpe_child_exec(names);`: nothing between, no call in the arguments, no other condition"
    }' "$1"
}

# atfork_problems DIR - a pthread_atfork anywhere in the product sources
# (DIR/*.c): its child handler would run between the fork and the exec,
# outside both halves of the check.
atfork_problems() {
  for src in "$1"/*.c; do
    [ -f "$src" ] || continue
    child_branch_problems "$src" 1
  done
}

# --- the planted sources -------------------------------------------------

PRELUDE='struct _IO_FILE;
extern int fprintf(struct _IO_FILE *, const char *, ...);
extern int __fprintf_chk(struct _IO_FILE *, int, const char *, ...);
extern long write(int, const void *, unsigned long);
extern void _exit(int);
extern int execve(const char *, char *const *, char *const *);
extern int execvp(const char *, char *const *);
extern void wpe_other_owner(int);
#define NOINLINE __attribute__((noinline))'

# plant NAME BODY... - write PRELUDE and the lines of BODY to $WORK/NAME.c.
plant() {
  name=$1; shift
  printf '%s\n' "$PRELUDE" "$@" > "$WORK/$name.c"
}

plant_all() {
  plant helper-inlined \
    'static void helper(struct _IO_FILE *f, int n) { fprintf(f, "%d", n); }' \
    'NOINLINE void wpe_child_exec(struct _IO_FILE *f, int n) { helper(f, n); _exit(1); }'
  plant helper-noinline \
    'static NOINLINE void helper(struct _IO_FILE *f, int n) { fprintf(f, "%d", n); }' \
    'NOINLINE void wpe_child_exec(struct _IO_FILE *f, int n) { helper(f, n); _exit(1); }'
  plant unreached \
    'void other(struct _IO_FILE *f, int n) { fprintf(f, "%d", n); }' \
    'NOINLINE void wpe_child_exec(int n) { write(2, "x", n); _exit(1); }'
  plant table \
    'static NOINLINE void ha(struct _IO_FILE *f, int n) { fprintf(f, "%d", n); }' \
    'static NOINLINE void hb(struct _IO_FILE *f, int n) { write(2, f, n); }' \
    'static void (*const tab[2])(struct _IO_FILE *, int) = { ha, hb };' \
    'NOINLINE void wpe_child_exec(struct _IO_FILE *f, int n) { tab[n & 1](f, n); _exit(1); }'
  plant safe \
    'NOINLINE void wpe_child_exec(int n) { write(2, "x", n); execve("/bin/true", 0, 0); _exit(1); }'
  plant execvp \
    'NOINLINE void wpe_child_exec(const char *a) { execvp(a, 0); _exit(1); }'
  plant inlined-away \
    'static inline __attribute__((always_inline)) void wpe_child_exec(int n) { write(2, "x", n); _exit(1); }' \
    'void parent(int n) { wpe_child_exec(n); }'
  plant clone \
    'static NOINLINE void wpe_child_exec(const int *p) { write(2, "x", *p); _exit(1); }' \
    'void parent(void) { int x = 1; wpe_child_exec(&x); }'
  plant hook \
    'void (*hook)(void);' \
    'NOINLINE void wpe_child_exec(void) { if (hook) hook(); _exit(1); }'
  plant data \
    'static int counter;' \
    'NOINLINE void wpe_child_exec(void) { counter++; _exit(counter); }'
  plant fortified \
    'NOINLINE void wpe_child_exec(struct _IO_FILE *f, int n) { __fprintf_chk(f, 1, "%d", n); _exit(1); }'
  plant foreign \
    'NOINLINE void wpe_child_exec(int n) { wpe_other_owner(n); _exit(1); }'
  plant indirect \
    'NOINLINE void wpe_child_exec(void (*f)(void)) { f(); _exit(1); }'
  plant tailcall \
    'NOINLINE void wpe_child_exec(char *const *a) { ((void (*)(void))(void *)a[1])(); }'
  plant asmjump \
    'NOINLINE void wpe_child_exec(void *p) { __asm__ volatile("jmp *%0" : : "r"(p)); }'
  plant section \
    'static NOINLINE __attribute__((section(".mytext"))) void sec(struct _IO_FILE *f, int n) { fprintf(f, "%d", n); }' \
    'NOINLINE void wpe_child_exec(struct _IO_FILE *f, int n) { sec(f, n); _exit(1); }'
  plant copy \
    'extern void *memcpy(void *, const void *, unsigned long);' \
    'NOINLINE void wpe_child_exec(char *d, const char *s, unsigned long n) { memcpy(d, s, n); _exit(1); }'
  plant environ \
    'extern char **environ;' \
    'NOINLINE void wpe_child_exec(void) { if (environ == 0) _exit(3); _exit(1); }'
  # What the owner does for a contained child: store its environment, then
  # lower a limit; each must be named unless the allow file lists it.
  plant environ-store \
    'extern char **environ;' \
    'NOINLINE void wpe_child_exec(char **e) { environ = e; _exit(1); }'
  plant setrlimit \
    'extern int setrlimit(int, const void *);' \
    'NOINLINE void wpe_child_exec(const void *l) { if (setrlimit(1, l) != 0) _exit(3); _exit(1); }'
}

# --- the canaries --------------------------------------------------------

BLINDNESS=0

# allow_lines FILE LINE... - an allow file for a canary.
allow_lines() {
  name=$1; shift
  printf '%s\n' "$@" > "$WORK/$name"
}

# canary LABEL SOURCE ALLOW STALE LEVEL EXPECT [SYMBOL] - compile SOURCE at
# LEVEL and judge it against ALLOW.  EXPECT is named (the problems include
# SYMBOL), silent (none) or blind (the reading refuses).
canary() {
  c_label=$1; c_src=$2; c_allow=$3; c_stale=$4; c_level=$5; c_expect=$6
  c_sym=${7:-}
  c_obj=$WORK/$c_label.o
  # The planted sources are checked for what they call, not for what the
  # build's hardening adds around it.
  # shellcheck disable=SC2086
  if ! compile "$c_src" "$c_obj" "$c_level" -w -fno-stack-protector $CANARY_FLAGS \
      > "$WORK/cc.out" 2>&1; then
    echo "canary $c_label: the planted source did not compile:"
    sed 's/^/  /' "$WORK/cc.out"
    BLINDNESS=1
    return
  fi
  c_got=$(check_object "$c_obj" "$c_allow" "$c_stale")
  case $c_expect in
    named)
      case $c_got in
        *"$c_sym"*) : ;;
        *) echo "canary $c_label: $c_sym was not named; got: ${c_got:-nothing}"
           BLINDNESS=1 ;;
      esac ;;
    silent)
      if [ -n "$c_got" ]; then
        echo "canary $c_label: a path that is fine was named:"
        printf '%s\n' "$c_got" | sed 's/^/  /'
        BLINDNESS=1
      fi ;;
    blind)
      case $c_got in
        blind:*) : ;;
        *) echo "canary $c_label: the blind object was not refused; got: ${c_got:-nothing}"
           BLINDNESS=1 ;;
      esac ;;
  esac
}

# canary_both LABEL SOURCE ALLOW STALE EXPECT [SYMBOL] - at -O2 and at -O0.
canary_both() {
  b_label=$1; b_src=$2; b_allow=$3; b_stale=$4; b_expect=$5; b_sym=${6:-}
  canary "$b_label-O2" "$b_src" "$b_allow" "$b_stale" -O2 "$b_expect" "$b_sym"
  canary "$b_label-O0" "$b_src" "$b_allow" "$b_stale" -O0 "$b_expect" "$b_sym"
}

run_canaries() {
  CANARY_FLAGS=
  find_thunk_flag
  plant_all
  allow_lines min '_exit|SAFE|canary' 'write|SAFE|canary' 'execve|EXEC|canary'
  allow_lines min-stale '_exit|SAFE|canary' 'write|SAFE|canary' 'execve|EXEC|canary' \
    'nosuch|SAFE|canary'
  allow_lines min-fact0 '_exit|SAFE|canary' 'write|SAFE|canary' 'execve|EXEC|canary' \
    'nosuch|SAFE|canary|defined(WPE_NEVER_SET)'
  allow_lines min-fact-literal0 '_exit|SAFE|canary' 'write|SAFE|canary' \
    'execve|EXEC|canary' 'nosuch|SAFE|canary|0'
  allow_lines min-fact-misspelt '_exit|SAFE|canary' 'write|SAFE|canary' \
    'execve|EXEC|canary' 'nosuch|SAFE|canary|WPE_NO_SUCH_FACT_NAME > 0'
  allow_lines min-fact-syntax '_exit|SAFE|canary' 'write|SAFE|canary' \
    'execve|EXEC|canary' 'nosuch|SAFE|canary|((('
  allow_lines min-fact-reached '_exit|SAFE|canary' \
    'write|SAFE|canary|defined(WPE_NEVER_SET)' 'execve|EXEC|canary'
  allow_lines min-optional-syscall '_exit|SAFE|canary' 'write|SAFE|canary' \
    'execve|EXEC|canary' 'ioctl|SYSCALL|canary|optional'
  allow_lines min-optional-unreached '_exit|SAFE|canary' 'write|SAFE|canary' \
    'execve|EXEC|canary' 'memcpy|SAFE|canary|optional'
  allow_lines min-optional-reached '_exit|SAFE|canary' 'memcpy|SAFE|canary|optional'
  allow_lines min-environ-data '_exit|SAFE|canary' 'environ|DATA|canary'
  allow_lines min-environ-runtime '_exit|SAFE|canary' 'environ|RUNTIME|canary'
  allow_lines min-setrlimit '_exit|SAFE|canary' 'setrlimit|SYSCALL|canary'
  allow_lines min-fact1 '_exit|SAFE|canary' 'write|SAFE|canary' 'execve|EXEC|canary' \
    'nosuch|SAFE|canary|1'
  allow_lines min-data '_exit|SAFE|canary' 'counter|DATA|canary'
  allow_lines min-datasafe '_exit|SAFE|canary' 'counter|SAFE|canary'
  allow_lines min-exit '_exit|SAFE|canary' 'write|SAFE|canary'

  canary_both helper-inlined "$WORK/helper-inlined.c" "$WORK/min" 0 named fprintf
  canary_both helper-noinline "$WORK/helper-noinline.c" "$WORK/min" 0 named fprintf
  canary_both unreached "$WORK/unreached.c" "$WORK/min-exit" 0 silent
  canary_both table "$WORK/table.c" "$WORK/min" 0 named fprintf
  canary_both safe "$WORK/safe.c" "$WORK/min" 1 silent
  canary_both execvp "$WORK/execvp.c" "$WORK/min" 0 named execvp
  canary_both inlined-away "$WORK/inlined-away.c" "$WORK/min" 0 blind
  canary_both clone "$WORK/clone.c" "$WORK/min-exit" 1 silent
  canary_both hook "$WORK/hook.c" "$WORK/min" 0 named hook
  canary_both data "$WORK/data.c" "$WORK/min" 0 named counter
  canary_both data-allowed "$WORK/data.c" "$WORK/min-data" 1 silent
  canary_both data-marked-safe "$WORK/data.c" "$WORK/min-datasafe" 0 named counter
  canary_both fortified "$WORK/fortified.c" "$WORK/min" 0 named __fprintf_chk
  canary_both foreign "$WORK/foreign.c" "$WORK/min" 0 named wpe_other_owner
  canary_both stale-line "$WORK/safe.c" "$WORK/min-stale" 1 named nosuch
  canary_both stale-fact-false "$WORK/safe.c" "$WORK/min-fact0" 1 silent
  canary_both stale-fact-true "$WORK/safe.c" "$WORK/min-fact1" 1 named nosuch
  canary_both fact-literal-0 "$WORK/safe.c" "$WORK/min-fact-literal0" 1 named "literal 0"
  canary_both fact-misspelt "$WORK/safe.c" "$WORK/min-fact-misspelt" 1 named "does not evaluate"
  canary_both fact-syntax "$WORK/safe.c" "$WORK/min-fact-syntax" 1 named "does not evaluate"
  canary_both fact-false-but-reached "$WORK/safe.c" "$WORK/min-fact-reached" 1 named "although its fact is false"
  canary_both optional-on-syscall "$WORK/safe.c" "$WORK/min-optional-syscall" 1 named "optional goes only with SAFE"
  canary_both optional-unreached "$WORK/safe.c" "$WORK/min-optional-unreached" 1 silent
  canary_both optional-reached "$WORK/copy.c" "$WORK/min-optional-reached" 1 silent
  canary_both libc-variable-as-data "$WORK/environ.c" "$WORK/min-environ-data" 1 named "marked DATA"
  canary_both libc-variable-as-runtime "$WORK/environ.c" "$WORK/min-environ-runtime" 1 silent
  canary_both environ-store-unlisted "$WORK/environ-store.c" "$WORK/min" 0 named environ
  canary_both environ-store-listed "$WORK/environ-store.c" "$WORK/min-environ-runtime" 1 silent
  canary_both setrlimit-unlisted "$WORK/setrlimit.c" "$WORK/min" 0 named setrlimit
  canary_both setrlimit-listed "$WORK/setrlimit.c" "$WORK/min-setrlimit" 1 silent
  canary_both unknown-section "$WORK/section.c" "$WORK/min" 0 named ".mytext"

  # An entry the compiler turned into a clone must still be read: at -O2 the
  # planted entry takes its pointer argument by value as a .isra clone.
  "$OBJDUMP" -h "$WORK/clone-O2.o" 2>/dev/null |
    grep -E 'text\.wpe_child_exec\.(isra|part|constprop)' >/dev/null ||
    echo "note: this compiler made no clone of the planted entry; the clone canary read the plain section"

  if object_is_x86_64 "$WORK/safe-O2.o"; then
    canary_both indirect "$WORK/indirect.c" "$WORK/min" 0 named "indirect call or jump"
    canary_both indirect-tail-call "$WORK/tailcall.c" "$WORK/min" 0 named "indirect call or jump"
    canary_both inline-asm-jump "$WORK/asmjump.c" "$WORK/min" 0 named "indirect call or jump"
    if [ -n "$THUNK_FLAG" ]; then
      # The same indirect call, made through the compiler's thunk: no call *
      # is left in the object, only a call to __x86_indirect_thunk_* or
      # __llvm_retpoline_*, and that is the same indirect call.
      CANARY_FLAGS=$THUNK_FLAG
      canary_both indirect-thunk "$WORK/indirect.c" "$WORK/min" 0 named "indirect call or jump"
      CANARY_FLAGS=
    else
      echo "note: this compiler has no flag for calling through a thunk, the thunk canary is skipped"
    fi
  else
    echo "note: not x86-64, the indirect call sub-check is skipped"
  fi

  # LTO: an object that holds no readable sections is refused, not passed.
  # shellcheck disable=SC2086
  if $MAINT_CC $KEEP -w -O2 -flto -c "$WORK/helper-inlined.c" -o "$WORK/lto.o" \
      > "$WORK/cc.out" 2>&1; then
    got=$(check_object "$WORK/lto.o" "$WORK/min" 0)
    case $got in
      blind:*) : ;;
      *) echo "canary lto: an LTO object was not refused; got: ${got:-nothing}"
         BLINDNESS=1 ;;
    esac
  else
    echo "note: this compiler does not take -flto, the LTO canary was not run"
  fi

  run_reader_canaries
  run_source_canaries
}

# fixture_problems ALLOW STALE TARGET... - judge the child path of a listing
# written by hand: the entry section holds one relocation to each TARGET.
# What the objects of the systems this does not run on put in them (OpenBSD's
# retguard cookies, NetBSD's versioned system calls, each libc's errno) is
# read here from the names, so the reader is proved on every system.
fixture_problems() {
  fx_allow=$1; fx_stale=$2; shift 2
  printf '%s\n' 'SYMBOL TABLE:' \
    '0000000000000000 g     F .text.wpe_child_exec	0000001b wpe_child_exec' \
    > "$WORK/fx-t.txt"
  printf '%s\n' 'Sections:' \
    'Idx Name          Size      VMA               LMA               File off  Algn' \
    '  0 .text.wpe_child_exec 0000001b  0000000000000000  0000000000000000  00000040  2**4' \
    > "$WORK/fx-h.txt"
  printf '%s\n' 'RELOCATION RECORDS FOR [.text.wpe_child_exec]:' \
    'OFFSET           TYPE              VALUE' > "$WORK/fx-r.txt"
  fx_off=16
  for fx_target in "$@"; do
    printf '%016x R_X86_64_PLT32    %s-0x0000000000000004\n' "$fx_off" "$fx_target" \
      >> "$WORK/fx-r.txt"
    fx_off=$((fx_off + 16))
  done
  : > "$WORK/fx-exempt.txt"
  fx_records=$(reach_files "$WORK/fx-t.txt" "$WORK/fx-h.txt" "$WORK/fx-r.txt")
  judge "$fx_allow" "$fx_records" "$fx_stale" "$WORK/fx-exempt.txt"
}

# reader_canary LABEL EXPECT TEXT ALLOW STALE TARGET... - fixture_problems must
# name TEXT (EXPECT named) or nothing (EXPECT silent).
reader_canary() {
  rc_label=$1; rc_expect=$2; rc_text=$3; rc_allow=$4; rc_stale=$5
  shift 5
  rc_got=$(fixture_problems "$rc_allow" "$rc_stale" "$@")
  case $rc_expect in
    named)
      case $rc_got in
        *"$rc_text"*) : ;;
        *) echo "canary reader $rc_label: \"$rc_text\" was not named; got: ${rc_got:-nothing}"
           BLINDNESS=1 ;;
      esac ;;
    silent)
      if [ -n "$rc_got" ]; then
        echo "canary reader $rc_label: a path that is fine was named:"
        printf '%s\n' "$rc_got" | sed 's/^/  /'
        BLINDNESS=1
      fi ;;
  esac
}

# run_reader_canaries - the section kinds and the spellings of every system.
run_reader_canaries() {
  allow_lines rd-none '_exit|SAFE|canary'
  allow_lines rd-all '_exit|SAFE|canary' \
    'errno_location|RUNTIME|canary' 'fstat|SAFE|canary' \
    'sigprocmask|SAFE|canary' 'sigaction|SAFE|canary' 'stat|SAFE|canary'
  allow_lines rd-no-fstat '_exit|SAFE|canary' 'errno_location|RUNTIME|canary'
  allow_lines rd-memcpy '_exit|SAFE|canary' 'memcpy|SAFE|canary'
  # a cookie of OpenBSD's retguard holds no code and no state
  reader_canary retguard-cookie silent "" "$WORK/rd-none" 0 \
    .openbsd.randomdata.retguard.1 .openbsd.randomdata.retguard.2246
  # a section of a kind the table does not know is a problem
  reader_canary unknown-section named ".mytext" "$WORK/rd-none" 0 .mytext
  # the general random data is writable: a variable, judged as data
  reader_canary randomdata-is-data named "the variable" "$WORK/rd-none" 0 \
    .openbsd.randomdata
  # one allow line per call, whatever each libc calls it
  for rd_errno in __errno_location __error __errno; do
    reader_canary "errno-$rd_errno" silent "" "$WORK/rd-all" 0 "$rd_errno"
  done
  reader_canary netbsd-versioned silent "" "$WORK/rd-all" 0 \
    __errno __fstat50 __stat50 __sigprocmask14 __sigaction_siginfo
  reader_canary spelling-said named "fstat (spelt __fstat50)" "$WORK/rd-no-fstat" 0 \
    __fstat50
  reader_canary errno-said named "errno_location (spelt __error)" "$WORK/rd-none" 0 \
    __error
  # a fortified twin is its own entry point and is not folded
  reader_canary fortified-twin named "__memcpy_chk" "$WORK/rd-memcpy" 0 __memcpy_chk
  # the spelling table itself, as the scripts ask it
  for rd_pair in __error:errno_location __errno:errno_location \
      __errno_location:errno_location __fstat50:fstat __sigprocmask14:sigprocmask \
      __sigaction_siginfo:sigaction __open_2:__open_2 __memcpy_chk:__memcpy_chk \
      poll:poll; do
    rd_got=$(symbol_source_name "${rd_pair%%:*}")
    if [ "$rd_got" != "${rd_pair#*:}" ]; then
      echo "canary reader spelling: ${rd_pair%%:*} reads as $rd_got, want ${rd_pair#*:}"
      BLINDNESS=1
    fi
  done
  for rd_pair in .openbsd.randomdata.retguard.7:skip .openbsd.randomdata:data \
      .text.f:visit .rodata.str1.1:skip .rodata:visit .data.rel.ro.x:visit \
      .bss.x:data .mytext:unknown; do
    rd_got=$(section_kind "${rd_pair%%:*}")
    if [ "$rd_got" != "${rd_pair#*:}" ]; then
      echo "canary reader section: ${rd_pair%%:*} is $rd_got, want ${rd_pair#*:}"
      BLINDNESS=1
    fi
  done
}

# source_canary LABEL EXPECT TEXT... - child_branch_problems on TEXT.
source_canary() {
  label=$1; expect=$2; shift 2
  printf '%s\n' "$@" > "$WORK/src-$label.c"
  got=$(child_branch_problems "$WORK/src-$label.c")
  if [ "$expect" = silent ] && [ -n "$got" ]; then
    echo "canary source $label: a child branch that is fine was named: $got"
    BLINDNESS=1
  fi
  if [ "$expect" = named ] && [ -z "$got" ]; then
    echo "canary source $label: a child branch that is wrong was not named"
    BLINDNESS=1
  fi
}

run_source_canaries() {
  source_canary plain silent \
    'void f(void) {' ' pid = fork();' ' if (pid == 0)' \
    '  wpe_child_exec(a, b);' ' g();' '}'
  source_canary braced silent \
    'void f(void) {' ' pid = fork();' ' if (pid == 0) { wpe_child_exec(a, b); }' \
    ' g();' '}'
  source_canary comment-and-string silent \
    'void f(void) {' ' puts("pid = vfork(); clone(");' ' pid = fork(); /* x */' \
    ' if (pid == 0) /* setup(); */ wpe_child_exec(a, &b);' '}'
  source_canary code-before named \
    'void f(void) {' ' pid = fork();' \
    ' if (pid == 0) { setup(); wpe_child_exec(a, b); }' '}'
  source_canary other-call named \
    'void f(void) {' ' pid = fork();' ' if (pid == 0)' ' other_child(a, b);' '}'
  source_canary no-branch named \
    'void f(void) {' ' pid = fork();' ' if (pid > 0) g();' '}'
  source_canary two-forks named \
    'void f(void) {' ' pid = fork();' ' if (pid == 0) wpe_child_exec(a);' \
    ' pid = fork();' ' if (pid == 0) wpe_child_exec(b);' '}'
  source_canary no-fork named \
    'void f(void) {' ' g();' '}'
  # A call in an argument, a statement between the fork and the branch, an
  # extra condition, a brace block with more: all run in the child unread.
  source_canary call-in-argument named \
    'void f(void) {' ' pid = fork();' \
    ' if (pid == 0) wpe_child_exec(a, (fputc(1, stderr), editor));' '}'
  source_canary statement-before-branch named \
    'void f(void) {' ' pid = fork();' ' fflush(stderr);' \
    ' if (pid == 0) wpe_child_exec(a, editor);' '}'
  source_canary extra-condition named \
    'void f(void) {' ' pid = fork();' \
    ' if (pid == 0 && fputc(1, stderr) >= 0) wpe_child_exec(a, editor);' '}'
  source_canary braced-with-more named \
    'void f(void) {' ' pid = fork();' \
    ' if (pid == 0) {' '  wpe_child_exec(a, editor);' '  fflush(stderr);' ' }' '}'
  # Every other way to make a child.
  source_canary vfork named \
    'void f(void) {' ' pid = vfork();' ' if (pid == 0) wpe_child_exec(a);' '}'
  source_canary forkpty named \
    'void f(void) {' ' pid = forkpty(&m, 0, 0, 0);' ' if (pid == 0) wpe_child_exec(a);' '}'
  source_canary parenthesised-fork named \
    'void f(void) {' ' pid = fork();' ' if (pid == 0) wpe_child_exec(a);' \
    ' q = (fork)();' '}'
  source_canary second-fork-clone named \
    'void f(void) {' ' pid = fork();' ' if (pid == 0) wpe_child_exec(a);' \
    ' q = clone(fn, stack, 0, 0);' '}'
  source_canary second-fork-rfork named \
    'void f(void) {' ' pid = fork();' ' if (pid == 0) wpe_child_exec(a);' \
    ' q = rfork(RFPROC);' '}'
  atfork_canary
}

# atfork_canary - a pthread_atfork in a product source is named, a mention in
# a comment or a string is not.
atfork_canary() {
  mkdir "$WORK/atfork" "$WORK/atfork-ok"
  printf '%s\n' 'void g(void) { pthread_atfork(0, 0, child_handler); }' \
    > "$WORK/atfork/we_x.c"
  printf '%s\n' '/* pthread_atfork is not used */' \
    'void g(void) { puts("pthread_atfork"); }' > "$WORK/atfork-ok/we_x.c"
  if [ -z "$(atfork_problems "$WORK/atfork")" ]; then
    echo "canary atfork: a pthread_atfork in a source was not named"
    BLINDNESS=1
  fi
  if [ -n "$(atfork_problems "$WORK/atfork-ok")" ]; then
    echo "canary atfork: a mention in a comment or string was named"
    BLINDNESS=1
  fi
}

# --- the run -------------------------------------------------------------

NOTES=$WORK/notes.txt
: > "$NOTES"
clean_flags

if [ ! -f "$SRCDIR/we_child.c" ]; then
  echo "check-child-exec: no we_child.c in $SRCDIR" >&2
  exit $BAD
fi
if [ ! -f "$ALLOW" ]; then
  echo "check-child-exec: no allow file at $ALLOW" >&2
  exit $BAD
fi

REALOBJ=$WORK/we_child.o
if ! compile "$SRCDIR/we_child.c" "$REALOBJ" > "$WORK/cc.out" 2>&1; then
  echo "check-child-exec: we_child.c did not compile with the build's flags:"
  sed 's/^/  /' "$WORK/cc.out"
  exit $BAD
fi
if ! object_is_elf "$REALOBJ"; then
  echo "check-child-exec: SKIP, the object is not ELF, so it has no .text.<function> sections to read."
  echo "  The ELF systems check the arms they compile.  The #if arms of this system"
  echo "  (the Darwin descriptor loop, no parent-death signal) are read by no run."
  exit $SKIP
fi

if [ "$MODE" = list ]; then
  reach "$REALOBJ"
  exit 0
fi

NOTES=/dev/null          # the canaries' own exemptions are not the tree's
run_canaries
NOTES=$WORK/notes.txt
if [ $BLINDNESS -ne 0 ]; then
  echo "check-child-exec: the check is blind"
  exit $BAD
fi
if [ "$MODE" = selftest ]; then
  echo "check-child-exec: canaries fire"
  exit 0
fi

status=0
problems=$(check_object "$REALOBJ" "$ALLOW" 1)
if [ -n "$problems" ]; then
  echo "check-child-exec: the child path of we_child.c is not what it must be:"
  printf '%s\n' "$problems" | sed 's/^/  /'
  status=1
fi
branch=$(child_branch_problems "$SRCDIR/we_child.c"; atfork_problems "$SRCDIR")
if [ -n "$branch" ]; then
  echo "check-child-exec: the fork in we_child.c:"
  printf '%s\n' "$branch" | sed 's/^/  /'
  status=1
fi

[ -z "$STRIPPED" ] || echo "note: flags removed because they change which calls exist:$STRIPPED"
if [ -s "$NOTES" ]; then
  echo "note: exempt from the stale rule, their fact is false in this build: $(sort -u "$NOTES" | tr '\n' ' ')"
fi
echo "note: this run reads the #if arms this build compiles; another system's arms are read by its own run"
[ $status -eq 0 ] && echo "check-child-exec: clean"
exit $status
