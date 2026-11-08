#!/bin/sh
# check-one-owner.sh -- the editor has one owner of input and of waiting.
#
# The keyboard, the display connection and every child descriptor are read
# only by the main loop (we_fdloop.c) or by a callback it invoked.  A second
# reader is how a key typed while the debugger answers ended up in the wrong
# place, and how a silent child hung the editor.  This script fails the
# build when code outside the owners calls the functions that wait or read
# on their own.
#
#   check-one-owner.sh [--strict] [--allow FILE] [--srcdir DIR] [--nm NM]
#                      [--list] [OBJECT...]
#
# Two parts:
#   1. The symbol scan: `nm -u` of every object of the editor (the compiler
#      produced the list, so a macro or a wrapper cannot hide a call).
#   2. The source scan, for what a symbol cannot show: a descriptor number
#      (fd 0) and a key read through the display's function-pointer table.
#
# Default mode lets the offenders named in the allow file stand, each with
# its number of call sites: the number may only fall, and a line that no
# longer matches anything is an error, so the file shrinks as the offenders
# go and a new one fails at once.  --strict ignores the file (except lines
# whose pass is "keep": a permanent exception, with its reason) and is the
# goal.  --list prints what was found, as allow-file lines to start from.
#
# Environment (set by `make check`): NM, ONE_OWNER_OBJECTS, srcdir.
# Exit: 0 clean, 1 offenders or a blind scanner, 77 no usable nm.

STRICT=0
ALLOW=
LIST=0
SRCDIR=${srcdir:-.}
NMTOOL=${NM:-nm}
OBJECTS=
HERE=$(dirname "$0")
BAD=1
SKIP=77

while [ $# -gt 0 ]; do
  case $1 in
    --strict) STRICT=1 ;;
    --allow) shift; ALLOW=$1 ;;
    --srcdir) shift; SRCDIR=$1 ;;
    --nm) shift; NMTOOL=$1 ;;
    --list) LIST=1 ;;
    *) OBJECTS="$OBJECTS $1" ;;
  esac
  shift
done
[ -n "$OBJECTS" ] || OBJECTS=$ONE_OWNER_OBJECTS
[ -n "$ALLOW" ] || ALLOW=$HERE/one-owner.allow

if [ "$NMTOOL" = ":" ] || ! command -v "$NMTOOL" >/dev/null 2>&1; then
  echo "check-one-owner: SKIP, no nm ($NMTOOL)"
  exit $SKIP
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/one-owner.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

# Who may import what.  "symbol-regex  objects allowed to import it".
# Kept as one table so the rule and the exceptions read together.
cat > "$WORK/rules" <<'EOF'
poll|ppoll|select|pselect|epoll_wait|epoll_pwait|kevent|nanosleep|usleep|sleep we_fdloop.o
getch|wgetch|get_wch|wget_wch|ungetch we_term.o
XNextEvent|XPeekEvent|XWindowEvent|XMaskEvent|XIfEvent|XCheckMaskEvent|XCheckTypedEvent|XCheckTypedWindowEvent|XPutBackEvent we_xterm.o
XPending|XEventsQueued|XCheckWindowEvent|XCheckIfEvent|XFlush we_xterm.o WeXterm.o
wl_display_dispatch|wl_display_dispatch_queue|wl_display_roundtrip|wl_display_dispatch_pending|wl_display_prepare_read|wl_display_read_events we_wayland.o
fork|vfork|forkpty|openpty|posix_spawn|posix_spawnp|popen|pclose|system|execv|execvp|execve|execl|execlp|execle|setsid|setpgid|setpgrp|waitpid|wait|wait3|wait4|pidfd_open|killpg|kill we_child.o
EOF

# The source scan: "word-regex  source files allowed to contain it".
# fd 0 and the display's key getters belong to the displays; the loop's own
# wait helper belongs to the code that waits on a display or the mouse.
# Bytes for a child go through wpe_fd_write (we_fdloop.c): a plain write(
# is allowed only where it writes no child's input -- the signal handlers'
# wake pipes and the crash log, which must stay async-signal-safe (we_unix.c,
# we_child.c), and OSC 52 to the terminal (we_clip.c).  fdopen( on a child's
# descriptor would be the way round that rule with stdio, so it is allowed
# nowhere.  `write` and `fdopen` are call forms: counted where the source
# calls them, so lsp_write_all( or fwrite( is not one.
# wpe_fd_wait_serving is the wait of a command that has the editor's
# terminal or window until it ends; e_shell_run is its one entry, so every
# place that hands the user's terminal or window to a command is a counted
# line of the allow file, with its reason.
cat > "$WORK/srcrules" <<'EOF'
read(0 we_term.c
STDIN_FILENO we_term.c
fileno(stdin) we_term.c
e_u_kbhit we_term.c we_xterm.c we_wayland.c we_unix.c
e_kbhit we_term.c we_xterm.c we_wayland.c
e_t_kbhit we_term.c
e_x_kbhit we_xterm.c
e_w_kbhit we_wayland.c
wpe_fd_wait_for we_xterm.c we_wayland.c we_gpm.c we_fdloop.c
wpe_fd_end_wait we_fdloop.c
wpe_fd_wait_serving we_fdloop.c
e_shell_run
SYS_pidfd_open
write we_fdloop.c we_unix.c we_child.c we_clip.c
fdopen
EOF

# normalise: the name as the source spells it.  One leading "_" (Mach-O),
# a "$..." suffix (macOS versioned names), "@..." (versioned ELF), and the
# fortified "__poll_chk" form.  Reads imports on stdin, prints names.
normalise() {
  awk '
    NF >= 2 && $(NF-1) == "U" {
      n = $NF
      sub(/@.*/, "", n)
      sub(/\$.*/, "", n)
      if (n ~ /^__.*_chk$/) { sub(/^__/, "", n); sub(/_chk$/, "", n) }
      print n
      if (n ~ /^_/) { m = n; sub(/^_/, "", m); print m }
    }'
}

# blank C comments and string literals, so a word in prose is not a call.
strip_c() {
  awk -v sq="'" '
    BEGIN { incom = 0; instr = 0 }
    {
      line = $0; out = ""; n = length(line); i = 1; instr = 0
      while (i <= n) {
        c = substr(line, i, 1); d = substr(line, i, 2)
        if (incom) { if (d == "*/") { incom = 0; i += 2 } else i++; continue }
        if (instr) {
          if (c == "\\") i += 2
          else { if (c == q) instr = 0; i++ }
          continue
        }
        if (d == "/*") { incom = 1; out = out " "; i += 2; continue }
        if (d == "//") break
        if (c == "\"" || c == sq) { instr = 1; q = c; out = out " "; i++; continue }
        out = out c; i++
      }
      print out
    }' "$1"
}

# count the places a word is used: symbol-regex, stripped source.
count_uses() {
  awk -v re="(^|[^A-Za-z0-9_])($1)[ \t]*[(]" '
    { s = $0; while (match(s, re)) { n++; s = substr(s, RSTART + RLENGTH) } }
    END { print n + 0 }' "$2"
}
count_words() {
  awk -v re="(^|[^A-Za-z0-9_])($1)([^A-Za-z0-9_]|$)" '
    { s = $0; while (match(s, re)) { n++; s = substr(s, RSTART + RLENGTH - 1) } }
    END { print n + 0 }' "$2"
}

# a call of the import, as the source writes it: a function of the getch
# family is mostly used through ncurses macros and the editor's fk_getch.
call_pattern() {
  case $1 in
    getch|wgetch) echo "(fk_|mv|mvw)?w?getch" ;;
    get_wch|wget_wch) echo "(mv|mvw)?w?get_wch" ;;
    *) echo "$1" ;;
  esac
}

: > "$WORK/imports"     # object symbol   (every import of a ruled name)
: > "$WORK/found"       # object symbol count   (the offenders)
: > "$WORK/problems"
mkdir "$WORK/stripped"

problem() { echo "$*" >> "$WORK/problems"; }

stripped_source() {
  src=$SRCDIR/${1%.o}.c
  out=$WORK/stripped/${1%.o}.c
  [ -f "$src" ] || return 1
  [ -f "$out" ] || strip_c "$src" > "$out"
  echo "$out"
}

# --- 1. the symbol scan
for obj in $OBJECTS; do
  base=$(basename "$obj")
  if [ ! -f "$obj" ]; then
    problem "missing object $obj: build the editor first"
    continue
  fi
  "$NMTOOL" -u "$obj" 2>/dev/null | normalise | sort -u > "$WORK/syms"
  while read -r re allowed; do
    for sym in $(printf '%s\n' "$re" | tr '|' ' '); do
      grep -qx "$sym" "$WORK/syms" || continue
      echo "$base $sym" >> "$WORK/imports"
      case " $allowed " in *" $base "*) continue ;; esac
      pat=$(call_pattern "$sym")
      count=1
      if file=$(stripped_source "$base"); then
        count=$(count_uses "$pat" "$file")
        [ "$count" -gt 0 ] || count=1   # reached through a macro: at least one
      fi
      echo "$base $sym $count" >> "$WORK/found"
    done
  done < "$WORK/rules"
done

# --- the canary: a scanner that sees nothing must fail, not pass
grep -qx "we_fdloop.o poll" "$WORK/imports" ||
  problem "canary: poll not seen in we_fdloop.o (LTO objects, wrong nm, or an empty object list)"
grep -qx "we_term.o wgetch" "$WORK/imports" ||
  problem "canary: wgetch not seen in we_term.o (LTO objects, wrong nm, or an empty object list)"
grep -qx "we_child.o fork" "$WORK/imports" ||
  problem "canary: fork not seen in we_child.o (LTO objects, wrong nm, or an empty object list)"
grep -qx "we_unix.o waitpid" "$WORK/imports" ||
  problem "canary: waitpid not seen in we_unix.o (LTO objects, wrong nm, or an empty object list)"

# Every child talker must send through the loop's writer: one that does not
# import wpe_fd_write is talking to its child some other way.  And it must
# close its child's input through wpe_fd_close, which forgets the queue:
# a plain close() leaves queued bytes for whatever next gets that number.
for talker in we_debug.o we_lsp.o we_dap.o we_bsp.o; do
  for obj in $OBJECTS; do
    [ "$(basename "$obj")" = "$talker" ] || continue
    [ -f "$obj" ] || continue
    "$NMTOOL" -u "$obj" 2>/dev/null | normalise > "$WORK/talker"
    grep -qx wpe_fd_write "$WORK/talker" ||
      problem "canary: $talker does not import wpe_fd_write (a child is written some other way)"
    grep -qx wpe_fd_close "$WORK/talker" ||
      problem "canary: $talker does not import wpe_fd_close (a child's input is closed with its bytes still queued)"
  done
done
# The call-form branch of the source scan must see the writer's own write(.
if fdloop=$(stripped_source we_fdloop.o); then
  [ "$(count_uses write "$fdloop")" -gt 0 ] ||
    problem "canary: the source scan sees no write( in we_fdloop.c (the call-form branch is blind)"
else
  problem "canary: we_fdloop.c not found under $SRCDIR"
fi

# --- 2. the source scan
for obj in $OBJECTS; do
  base=$(basename "$obj")
  file=$(stripped_source "$base") || continue
  c=${base%.o}.c
  while read -r word allowed; do
    case " $allowed " in *" $c "*) continue ;; esac
    case $word in
      'read(0') n=$(count_uses "read[ \t]*[(][ \t]*0[ \t]*," "$file") ;;
      'fileno(stdin)') n=$(count_uses "fileno[ \t]*[(][ \t]*stdin" "$file") ;;
      wpe_fd_wait_for|wpe_fd_end_wait|wpe_fd_wait_serving|e_shell_run|write|fdopen) n=$(count_uses "$word" "$file") ;;
      *) n=$(count_words "$word" "$file") ;;
    esac
    [ "$n" -gt 0 ] && echo "$base $word $n" >> "$WORK/found"
  done < "$WORK/srcrules"
done

sort -u "$WORK/found" -o "$WORK/found"

if [ $LIST -eq 1 ]; then
  cat "$WORK/found"
  exit 0
fi

# --- compare with the allow file
: > "$WORK/allowed"
if [ -f "$ALLOW" ]; then
  awk '
    /^[ \t]*(#|$)/ { next }
    {
      if (NF < 4 || $3 !~ /^[0-9]+$/) { print "bad line " NR " in the allow file: " $0 > "/dev/stderr"; bad = 1; next }
      key = $1 " " $2
      if (key in seen) { print "duplicate allow line for " key > "/dev/stderr"; bad = 1; next }
      seen[key] = 1
      print $1, $2, $3, $4
    }
    END { exit bad }' "$ALLOW" > "$WORK/allowed" ||
    problem "the allow file $ALLOW has malformed or duplicate lines"
else
  [ $STRICT -eq 1 ] || problem "no allow file at $ALLOW"
fi

# In strict mode only permanent exceptions stand.
if [ $STRICT -eq 1 ]; then
  awk '$4 == "keep"' "$WORK/allowed" > "$WORK/allowed.keep"
  mv "$WORK/allowed.keep" "$WORK/allowed"
fi

awk '
  FILENAME == ARGV[1] { al[$1 " " $2] = $3; pass[$1 " " $2] = $4; next }
  {
    key = $1 " " $2; seen[key] = 1
    if (!(key in al)) {
      print "offender: " $1 " uses " $2 " (" $3 " site(s))"; bad = 1
    } else if ($3 + 0 > al[key] + 0) {
      print "more than allowed: " $1 " " $2 " has " $3 " site(s), the allow file says " al[key] " (" pass[key] ")"; bad = 1
    } else if ($3 + 0 < al[key] + 0) {
      print "fewer than the allow file says: " $1 " " $2 " has " $3 " site(s), lower the line from " al[key] " (" pass[key] ")"; bad = 1
    }
  }
  END {
    for (k in al) if (!(k in seen)) { print "stale entry, delete it: " k " (" pass[k] ")"; bad = 1 }
    exit bad
  }' "$WORK/allowed" "$WORK/found" > "$WORK/report"
status=$?

if [ -s "$WORK/problems" ] || [ $status -ne 0 ]; then
  [ $STRICT -eq 1 ] && echo "check-one-owner --strict: FAIL"
  [ $STRICT -eq 0 ] && echo "check-one-owner: FAIL"
  cat "$WORK/problems" "$WORK/report" 2>/dev/null
  if [ $STRICT -eq 1 ]; then
    echo "objects with an offender:"
    awk '{ print $1 }' "$WORK/found" | sort -u | tr '\n' ' '
    echo
  fi
  exit $BAD
fi
echo "check-one-owner: ok ($(wc -l < "$WORK/found" | tr -d ' ') allowed offender line(s))"
exit 0
