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
WpeXInfo we_xterm.o WeXterm.o we_render_cairo.o
e_add_undo we_buffer.o we_edit.o
wpe_child_start we_child.o we_startplan.o
wpe_child_capture|wpe_child_probe|wpe_child_run_to_file|wpe_child_capture_plan|wpe_child_probe_plan|wpe_child_run_to_file_plan we_child.o we_startplan.o
regcomp|regexec|regfree|regerror we_rowfind.o
refresh|wrefresh|doupdate|endwin|resizeterm|def_prog_mode|reset_prog_mode|reset_shell_mode we_term.o
tcsetattr|tcgetattr we_term.o we_child.o
e_mouse_tracking_enable|e_mouse_tracking_disable we_term.o
EOF

# Who has the terminal has one owner, the terminal session of we_term.c: the
# editor's screen is written through curses only in e_t_flush, and the
# terminal's settings (the line discipline, the alternate screen, the mouse and
# keyboard modes) are changed only by its transitions, so nothing writes to a
# terminal the editor has handed to a program or given back.  The three rules
# above name the calls: curses' screen and mode calls, the terminal's
# attributes (the child's pty in we_child.c and the debugger's own pty, an
# allow line, are not the user's terminal), and the mouse modes, which only the
# terminal's mode switch and the Run loop's pause write.  The source rules
# below count the rest: refresh and tputs outside the two writers, and the
# terminal reset that used to end the editor.

# A program the editor starts for the user comes from a start plan
# (we_startplan.c), which shows and asks about what a folder decided: only
# the plan's owner starts a child, and the short commands' calls are its
# too once they take plans.  The sites not converted yet are counted lines
# of the allow file, each with the pass that converts it.

# The display's connection has one owner: the X11 backend.  Any other name
# that starts like an Xlib, Xft or Xrm call, a cairo-xlib call or one of the
# backend's own functions is a symbol only that backend may import; what
# shared code needs from the display it asks the display table for
# (edit.h), which every display fills.  "name-prefix-regex  objects allowed
# to import it".  An exact rule above wins for a name it already lists.
cat > "$WORK/prefixrules" <<'EOF'
X[A-Za-z] we_xterm.o WeXterm.o we_render_cairo.o
cairo_xlib_ we_render_cairo.o
e_x_ we_xterm.o WeXterm.o we_render_cairo.o
EOF

# The source scan: "word-regex  source files allowed to contain it".
# fd 0 and the display's key getters belong to the displays; the loop's own
# wait helper belongs to the code that waits on a display or the mouse.  The
# key taken without waiting (e_u_key_take) is also the screen owner's, through
# its key hook in we_edit.c.
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
read(0
STDIN_FILENO we_term.c
fileno(stdin) we_term.c
e_u_key_take we_term.c we_xterm.c we_wayland.c we_unix.c we_edit.c
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
KEY_F we_keymap.c
XK_ we_keymap.c
XKB_KEY_ we_keymap.c
e_getch we_paint.c
wpe_modal_active we_paint.c
g_lsp_modal_depth we_paint.c
g_lsp_ui_suspended we_paint.c
wpe_chrome_suppress we_paint.c
e_x_ we_xterm.c WeXterm.c we_render_cairo.c
mxlines_write we_buffer.c
bf_alloc we_buffer.c
pos_field we_buffer.c
pos_prefix we_buffer.c
pos_whole we_buffer.c
pos_address we_buffer.c
term_raw_read
display_key_literal
term_output
ris_literal
mouse_mode_call we_term.c
e_switch_screen we_main.c we_term.c
e_undo_sw
e_redo_sw
lsp_units_field we_lsp_pos.c
e_lsp_units_make we_lsp_pos.c
lsp_char_key we_lsp_pos.c
REG_NOTEOL we_rowfind.c
isalnum we_rowfind.c we_fl_fkt.c we_prog.c we_progn.c
uselocale we_rowfind.c
newlocale we_rowfind.c
EOF

# The language server's columns have one owner (we_lsp_pos.c): the units a
# server counts a line in (UTF-16 code units unless it agreed to UTF-8) are
# made, read and written only there, and every other file holds them as the
# opaque e_lsp_units, so an int cannot be handed where units are meant, and
# units cannot be used as a byte column.  Three rules close the gaps the
# compiler leaves: the field of the type (lsp_units_field), its one
# constructor (e_lsp_units_make, a call form), and the JSON key itself
# (lsp_char_key: the string "character" outside we_lsp_pos.c is a position
# read into an int, which the type alone cannot see; it is counted on the
# source with comments blanked and strings kept).  The rules have no allow
# line, so no allow-file entry can go stale for them; an allow line added
# later needs the stale check of the allow file like any other.

# The text a window shows has one owner (we_buffer.c): every line the editor
# adds or removes goes through its splice, which keeps the window's undo,
# modified mark, block marks, breakpoint rows and colours in step.  The
# symbol rule above names the one undo recorder the editor's char kinds
# still call; the old line primitives and undo functions are in the
# deleted-symbol list further down.  The source rules name a direct write of
# a buffer's line count (mxlines_write), a direct allocation of its line
# table (bf_alloc), and the global covers of the old undo (e_undo_sw,
# e_redo_sw): a cover is the owner's, per buffer (e_buffer_cover_begin), so
# the global one is allowed nowhere.  A new helper that adds lines some other
# way writes mxlines or reallocates bf, and is seen here.  This section is
# strict in every mode: no line of the allow file may stand for it unless
# it is a permanent exception with its reason.

# Where the cursor and the marks stand has one owner as well (we_buffer.c):
# a position is a row and a byte column, and every write of one goes through
# the owner's calls (e_buffer_cursor_set, e_buffer_mark_set, ...), which end
# in the rule that a position is the first byte of a glyph or a row's end.
# Four source rules name the writes: a field of a position assigned or
# stepped (pos_field), stepped with a prefix operator (pos_prefix), a whole
# position assigned (pos_whole), and the address of one handed out
# (pos_address), so a helper that writes through a pointer cannot hide.
# The regexes are in pos_rule_re, spelled with brackets because awk -v eats
# backslashes; a self-test runs them over a fixture before the scan.  Until
# the last writer is converted the offenders are counted lines of the allow
# file; the count may only fall.

# The screen has one owner (we_paint.c): a box, a menu or a prefix that
# reads keys says so to it, and content from outside the user's keys is
# drawn only when it says the editor is idle.  A key loop of its own is an
# e_getch( call (a call form: the macro reads through the display's table);
# a second flag of "a box is up" is one of the old names.  Callbacks need no
# rule of their own: the loop (we_fdloop.c) runs every descriptor's and every
# timer's callback between the two hooks the owner sets, so whoever
# registers one, it is bracketed; the canary below sees the owner stop
# setting them.

# The key table owns the names a platform gives its keys: curses' KEY_F(n)
# (a call form), X's XK_* and xkbcommon's XKB_KEY_* (prefix forms: every
# name that starts so).  A display that spells one of them is decoding a key
# on its own, which is how the three displays came to disagree.  What stays
# in a display is a counted line of the allow file with its reason.

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
# count the places a whole regex matches: regex, stripped source.
count_uses_re() {
  awk -v re="$1" '
    { s = $0; while (match(s, re)) { n++; s = substr(s, RSTART + RLENGTH) } }
    END { print n + 0 }' "$2"
}
count_words() {
  awk -v re="(^|[^A-Za-z0-9_])($1)([^A-Za-z0-9_]|$)" '
    { s = $0; while (match(s, re)) { n++; s = substr(s, RSTART + RLENGTH - 1) } }
    END { print n + 0 }' "$2"
}

# count the places a name is assigned (not compared): name, stripped source.
count_assign() {
  awk -v re="(^|[^A-Za-z0-9_])($1)[ \t]*=[^=]" '
    { s = $0; while (match(s, re)) { n++; s = substr(s, RSTART + RLENGTH) } }
    END { print n + 0 }' "$2"
}

# The regex of one position rule, in the bracket spelling awk -v keeps
# whole: the only backslash is \t inside [ \t], which -v turns into a tab on
# purpose.  POS_IDENT is an identifier that does not end in "fb" (a colour
# table handle, whose "fa" field is a colour and no position).  The cursor
# is written as b, or as a path that ends in ->b, ->bz, ...
POS_IDENT='([A-Za-z0-9_]|[A-Za-z0-9_]*([A-Za-eg-z0-9_][A-Za-z0-9_]|f[A-Zac-z0-9_]))'
POS_NAMES="pos_field pos_prefix pos_whole pos_address"
pos_rule_re() {
  case $1 in
    pos_field) printf '%s\n' '(->|[.])(b|mark_begin|mark_end|ks|fa|fe|pt[[][^]]*[]])[.](x|y)[ \t]*[)]?[ \t]*([+][+]|--|[+]=|-=|=[^=])' ;;
    pos_prefix) printf '%s\n' '([+][+]|--)[ \t]*[(]?[A-Za-z0-9_>.-]*(->|[.])(b|mark_begin|mark_end|ks|fa|fe|pt[[][^]]*[]])[.](x|y)' ;;
    pos_whole) printf '%s\n' "(->|[.])(mark_begin|mark_end|ks)[ \\t]*=[^=]|(^|[^A-Za-z0-9_])([A-Za-z0-9_>.-]*(->|[.]))?$POS_IDENT(->|[.])(fa|fe)[ \\t]*=[^=]|->pt[[][^]]*[]][ \\t]*=[^=]|(^|[^A-Za-z0-9_])(b|[A-Za-z0-9_>.-]*->b[a-z]?)->b[ \\t]*=[^=]" ;;
    pos_address) printf '%s\n' "&[A-Za-z0-9_>.-]*(->|[.])(mark_begin|mark_end|ks)([^A-Za-z0-9_.[]|\$)|&([A-Za-z0-9_>.-]*(->|[.]))?$POS_IDENT(->|[.])(fa|fe)([^A-Za-z0-9_.[]|\$)|&(b|[A-Za-z0-9_>.-]*->b[a-z]?)->b([^A-Za-z0-9_.[]|\$)|&[A-Za-z0-9_>.-]*->pt[[]" ;;
  esac
}

# The field of the language server's units, in the bracket spelling awk -v
# keeps whole: a read or write of it through . or ->.
lsp_units_rule_re() {
  printf '%s\n' '(->|[.])units_on_wire([^A-Za-z0-9_]|$)'
}

# The terminal's keyboard has one reader.  In we_term.c a byte or curses key
# code comes off the keyboard in e_t_next_byte (and goes back through
# e_t_push_back) and nowhere else: the decoder, the tail readers, the mouse
# poll and the Hit-CR wait all call those two, so no reader takes the bytes of
# a keyboard protocol's report apart or leaves its end behind as text.  The
# scan reads comment-stripped source and tracks the function a line is in (a
# definition is a line that starts in column 1 and has a "(").
TERM_READ_OWNERS="e_t_next_byte e_t_push_back"
# A read(2) of fd 0 by number, wherever it is: the terminal's bytes taken
# around its reader.
READ_STDIN_RE='(^|[^A-Za-z0-9_])read[ \t]*[(][ \t]*0[ \t]*,'
count_raw_reads() {
  awk -v owners=" $TERM_READ_OWNERS " '
    /^[A-Za-z_]/ && /[(]/ { fn = $0; sub(/[ \t]*[(].*/, "", fn); sub(/.*[ \t*]/, "", fn) }
    {
      s = $0
      while (match(s, /(^|[^A-Za-z0-9_])(fk_getch|getch|wgetch|mvgetch|ungetch|get_wch|wget_wch)[ \t]*[(]/)) {
        if (index(owners, " " fn " ") == 0) n++
        s = substr(s, RSTART + RLENGTH)
      }
    }
    END { print n + 0 }' "$1"
}

# The terminal's output has two writers in we_term.c: e_t_flush puts the
# editor's screen on the terminal (curses' refresh, wrefresh or doupdate,
# stdio's flush without curses) and e_t_putp sends a terminfo string (tputs or
# putp).  Both refuse while the terminal is handed over or given back, so a
# call of curses' refresh or of tputs anywhere else in the file is a write the
# gate does not see: the screen entered again after it was left.  Counted on
# comment-stripped source with the same tracking of the function a line is in
# as the reader rule; e_putp, fk_putp and term_refresh are the callers of the
# two writers, not writers.
TERM_OUTPUT_OWNERS="e_t_flush e_t_putp"
# The mouse modes are written by e_mouse_tracking_enable and _disable, and
# those are called only by the terminal session (we_term.c).  The symbol rule
# sees a call from another object; a call inside we_mouse.c, which defines
# them, is no undefined symbol, so the source rule counts the calls: an
# indented line that names one (a definition starts in column 1).
count_mouse_mode_calls() {
  awk '
    /^[A-Za-z_#]/ { next }
    {
      s = $0
      while (match(s, /(^|[^A-Za-z0-9_])e_mouse_tracking_(enable|disable)[ \t]*[(]/)) {
        n++
        s = substr(s, RSTART + RLENGTH)
      }
    }
    END { print n + 0 }' "$1"
}
# The full terminal reset (ESC c) that ended the editor, spelled in octal or in
# hex, in the bracket spelling awk -v keeps whole (four backslashes in the
# shell become the two of the regex).  The editor gives the terminal back
# string by string; a reset would wipe the shell's own screen and, on a
# graphical display, is a write to a terminal the editor never owned.
RIS_LITERAL_RE='\\\\033c|\\\\x1[bB]c'
count_term_output() {
  awk -v owners=" $TERM_OUTPUT_OWNERS " '
    /^[A-Za-z_]/ && /[(]/ { fn = $0; sub(/[ \t]*[(].*/, "", fn); sub(/.*[ \t*]/, "", fn) }
    {
      s = $0
      while (match(s, /(^|[^A-Za-z0-9_])(refresh|wrefresh|doupdate|tputs|putp)[ \t]*[(]/)) {
        if (index(owners, " " fn " ") == 0) n++
        s = substr(s, RSTART + RLENGTH)
      }
    }
    END { print n + 0 }' "$1"
}

# A display decodes no key by spelling it: an escape-sequence literal (ESC [
# or ESC O, in octal or hex), or a case label of a Ctrl or Alt key code, in
# we_term.c, we_xterm.c, we_wayland.c or WeXterm.c, is a key read on the
# display's own, which is how the displays came to disagree.  What the
# display writes to the terminal is not read, and is a counted line of the
# allow file with its reason.  The source is read with comments blanked and
# strings kept; the pattern is for grep, which keeps the backslashes whole.
DISPLAY_KEY_LITERAL='\\033[[O]|\\x1[bB][[O]|case[[:space:]]+(Ctrl|Alt)[A-Za-z0-9]*[[:space:]]*:'
strip_comments() {
  awk -v sq="'" '
    BEGIN { incom = 0 }
    {
      line = $0; out = ""; n = length(line); i = 1; instr = 0
      while (i <= n) {
        c = substr(line, i, 1); d = substr(line, i, 2)
        if (incom) { if (d == "*/") { incom = 0; i += 2 } else i++; continue }
        if (instr) {
          out = out c
          if (c == "\\") { out = out substr(line, i + 1, 1); i += 2 }
          else { if (c == q) instr = 0; i++ }
          continue
        }
        if (d == "/*") { incom = 1; out = out " "; i += 2; continue }
        if (d == "//") break
        if (c == "\"" || c == sq) { instr = 1; q = c; out = out c; i++; continue }
        out = out c; i++
      }
      print out
    }' "$1"
}
count_display_literals() {
  grep -Eo "$DISPLAY_KEY_LITERAL" "$1" | wc -l | tr -d ' '
}

# the names of the function pointers a header declares with extern, one per
# line: "extern int (*name)(...);".
declared_slots() {
  awk '
    /^[ \t]*extern[ \t]/ {
      s = $0
      while (match(s, /\(\*[A-Za-z_0-9]+\)[ \t]*\(/)) {
        n = substr(s, RSTART + 2, RLENGTH - 2)
        sub(/\).*/, "", n)
        print n
        s = substr(s, RSTART + RLENGTH)
      }
    }' "$1"
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

# --- the self-test of the position rules: a rule that matches nothing is
# indistinguishable from a clean source, so each one is run over a fixture
# first.  "+" lines must match the four rules exactly once in all, "-"
# lines (code that looks like a position write and is not) none, and every
# rule must match at least one "+" line.
cat > "$WORK/posfixture" <<'EOF'
+ b->b.x = 1;
+ f->b->b.y++;
+ (b->b.x)--;
+ mf->b->b.x += 2;
+ --wf->b->b.y;
+ s->mark_begin = p;
+ s->mark_end.x += n;
+ s->fa.y--;
+ sch->ks.y = 0;
+ s->pt[3] = q;
+ b->b = at;
+ op->bz->b = e;
+ f(&sch->mark_begin);
+ g(&b->b);
+ h(&f->s->fe);
+ k(&s->pt[n]);
+ sch->fa = e_set_pnt(0, 0);
+ s->pt[0].x = 1;
+ s->pt[n].y += 2;
+ ++s->pt[1].x;
- if (b->b.x == 1)
- x = s->pt[2].x;
- if (s->pt[0].y == 1)
- x = s->mark_begin.x;
- f->b = MALLOC(n);
- c->b = 4;
- ep->b = NULL;
- d->b = b;
- c = &fb->fa;
- y = fb->fa.fb;
- p = &b->bf[0];
- sscanf(v, "%d", &c->b);
- g(&seg.b);
- c = &f->fb->fa;
- fb->fa = e_s_x_clr(15,5);
- u_fb->fa = e_n_t_clr(A_REVERSE);
- if (b->b.x != len)
- e_set_pnt(b->b.x, b->b.y)
- x = b->b.x - 1;
EOF
: > "$WORK/posmatched"
while IFS= read -r fixline; do
  fixkind=${fixline%% *}
  fixtext=${fixline#* }
  printf '%s\n' "$fixtext" > "$WORK/posline"
  fixsum=0
  for fixrule in $POS_NAMES; do
    fixn=$(count_uses_re "$(pos_rule_re "$fixrule")" "$WORK/posline")
    [ "$fixn" -gt 0 ] && [ "$fixkind" = "+" ] && echo "$fixrule" >> "$WORK/posmatched"
    fixsum=$((fixsum + fixn))
  done
  if [ "$fixkind" = "+" ] && [ "$fixsum" -ne 1 ]; then
    problem "canary: the position rules match \"$fixtext\" $fixsum times, not once (a rule is blind or double)"
  elif [ "$fixkind" = "-" ] && [ "$fixsum" -ne 0 ]; then
    problem "canary: the position rules match \"$fixtext\" $fixsum times, and it is no position write (a rule is too wide)"
  fi
done < "$WORK/posfixture"
for fixrule in $POS_NAMES; do
  grep -qx "$fixrule" "$WORK/posmatched" ||
    problem "canary: position rule $fixrule matches nothing in its fixture (the rule is blind)"
done

# --- the self-test of the units rule: each line is "<matches> <text>", the
# number of times the rule must match the text.  The first column counts the
# whole line, so a line with two reads counts two.  The rule must match at
# least one positive line, or it is blind.
cat > "$WORK/unitsfixture" <<'EOF'
1 u.units_on_wire = 3;
1 p->units_on_wire
1 return u.units_on_wire;
2 if (a.units_on_wire == b.units_on_wire)
0 units_on_wire_count = 0;
0 x.units = 1;
0 wire->units
EOF
unitsmatched=0
while IFS= read -r fixline; do
  fixwant=${fixline%% *}
  fixtext=${fixline#* }
  printf '%s\n' "$fixtext" > "$WORK/unitsline"
  fixn=$(count_uses_re "$(lsp_units_rule_re)" "$WORK/unitsline")
  [ "$fixn" -eq "$fixwant" ] ||
    problem "canary: the units rule matches \"$fixtext\" $fixn times, not $fixwant (the rule is blind or too wide)"
  [ "$fixn" -gt 0 ] && unitsmatched=1
done < "$WORK/unitsfixture"
[ $unitsmatched -eq 1 ] ||
  problem "canary: the units rule matches nothing in its fixture (the rule is blind)"

# --- the self-test of the keyboard rules: the same blindness check.  The
# fixture has two raw reads outside the owners (one of them a call of ungetch)
# and, among the lines that only look like one, an owner's read, a name that
# ends in getch, and a comment.  The display-literal fixture has four hits and
# three lines that are not.
cat > "$WORK/readerfixture.c" <<'EOF'
static int e_t_next_byte(int w)
{
 c = getch();
 return(c);
}
int e_other(void)
{
 c = fk_getch();
 ungetch(c);
 x = e_t_getch();
 /* getch( in a comment */
}
EOF
strip_c "$WORK/readerfixture.c" > "$WORK/readerfixture.stripped"
[ "$(count_raw_reads "$WORK/readerfixture.stripped")" -eq 2 ] ||
  problem "canary: the reader rule counts $(count_raw_reads "$WORK/readerfixture.stripped") raw reads in its fixture, not 2 (the rule is blind or too wide)"
# The output fixture has two writes outside the owners (a refresh and a tputs)
# and, among the lines that only look like one, the owners' own calls, the
# callers of the two writers, a name that ends in refresh and a comment.
cat > "$WORK/outputfixture.c" <<'EOF'
static int e_t_flush(void)
{
 return(refresh());
}
static int e_t_putp(const char *s)
{
 tputs(s, 1, f);
 return(1);
}
int e_other(void)
{
 refresh();
 tputs(x, 1, f);
 e_putp(y);
 term_refresh();
 e_t_refresh();
 /* doupdate( in a comment */
}
EOF
strip_c "$WORK/outputfixture.c" > "$WORK/outputfixture.stripped"
[ "$(count_term_output "$WORK/outputfixture.stripped")" -eq 2 ] ||
  problem "canary: the output rule counts $(count_term_output "$WORK/outputfixture.stripped") writes in its fixture, not 2 (the rule is blind or too wide)"
cat > "$WORK/mousemodefixture.c" <<'EOF'
void e_mouse_tracking_enable(void)
{
 e_mouse_tracking_enable();
 e_t_mouse_tracking_enable();
 /* e_mouse_tracking_disable( in a comment */
 if (x) e_mouse_tracking_disable();
}
EOF
strip_c "$WORK/mousemodefixture.c" > "$WORK/mousemodefixture.stripped"
[ "$(count_mouse_mode_calls "$WORK/mousemodefixture.stripped")" -eq 2 ] ||
  problem "canary: the mouse-mode rule counts $(count_mouse_mode_calls "$WORK/mousemodefixture.stripped") calls in its fixture, not 2 (the rule is blind or too wide)"
cat > "$WORK/risfixture.c" <<'EOF'
 fputs("\033c", stderr);
 write(1, "\x1bc", 2);
 puts("\033[2J");
 /* "\033c" in a comment */
 x = "\033cat";
EOF
strip_comments "$WORK/risfixture.c" > "$WORK/risfixture.stripped"
[ "$(count_uses_re "$RIS_LITERAL_RE" "$WORK/risfixture.stripped")" -eq 3 ] ||
  problem "canary: the reset rule counts $(count_uses_re "$RIS_LITERAL_RE" "$WORK/risfixture.stripped") literals in its fixture, not 3 (the rule is blind or too wide)"
cat > "$WORK/stdinfixture.c" <<'EOF'
 ret = read(0, &c, 1);
 (void)read( 0 , &c, 1);
 n = read(fd, &c, 1);
 n = pread(0, &c, 1, 0);
EOF
[ "$(count_uses_re "$READ_STDIN_RE" "$WORK/stdinfixture.c")" -eq 2 ] ||
  problem "canary: the read(0 rule counts $(count_uses_re "$READ_STDIN_RE" "$WORK/stdinfixture.c") reads of fd 0 in its fixture, not 2 (the rule is blind or too wide)"
cat > "$WORK/literalfixture.c" <<'EOF'
 puts("\033[2J");
 x = "\033OP";
 switch (c) { case CtrlK: break; }
 switch (c) { case AltX : break; }
 c = e_t_getch();
 /* "\033[" in a comment */
 y = Alt1 + CtrlA;
EOF
strip_comments "$WORK/literalfixture.c" > "$WORK/literalfixture.stripped"
[ "$(count_display_literals "$WORK/literalfixture.stripped")" -eq 4 ] ||
  problem "canary: the display-literal rule counts $(count_display_literals "$WORK/literalfixture.stripped") hits in its fixture, not 4 (the rule is blind or too wide)"

mkdir "$WORK/commented"
commented_source() {
  src=$SRCDIR/$1
  out=$WORK/commented/$1
  [ -f "$src" ] || return 1
  [ -f "$out" ] || strip_comments "$src" > "$out"
  echo "$out"
}

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
        case $sym in
          WpeXInfo) count=$(count_words "$pat" "$file") ;;   # data: uses, not calls
          *) count=$(count_uses "$pat" "$file") ;;
        esac
        [ "$count" -gt 0 ] || count=1   # reached through a macro: at least one
      fi
      echo "$base $sym $count" >> "$WORK/found"
    done
  done < "$WORK/rules"
  # the names a prefix rule covers, but not those an exact rule listed
  while read -r prefix allowed; do
    for sym in $(grep -E "^($prefix)" "$WORK/syms"); do
      grep -qx "$base $sym" "$WORK/imports" && continue
      echo "$base $sym" >> "$WORK/imports"
      case " $allowed " in *" $base "*) continue ;; esac
      count=1
      if file=$(stripped_source "$base"); then
        count=$(count_uses "$sym" "$file")
        [ "$count" -gt 0 ] || count=1   # a function pointer's value, not a call
      fi
      echo "$base $sym $count" >> "$WORK/found"
    done
  done < "$WORK/prefixrules"
done

# --- the canary: a scanner that sees nothing must fail, not pass
grep -qx "we_fdloop.o poll" "$WORK/imports" ||
  problem "canary: poll not seen in we_fdloop.o (LTO objects, wrong nm, or an empty object list)"
grep -qx "we_term.o wgetch" "$WORK/imports" ||
  problem "canary: wgetch not seen in we_term.o (LTO objects, wrong nm, or an empty object list)"
grep -qx "we_term.o endwin" "$WORK/imports" ||
  problem "canary: endwin not seen in we_term.o (the terminal-session scan is blind)"
grep -qx "we_term.o tcsetattr" "$WORK/imports" ||
  problem "canary: tcsetattr not seen in we_term.o (the terminal-session scan is blind)"
grep -qx "we_child.o fork" "$WORK/imports" ||
  problem "canary: fork not seen in we_child.o (LTO objects, wrong nm, or an empty object list)"
grep -qx "we_unix.o waitpid" "$WORK/imports" ||
  problem "canary: waitpid not seen in we_unix.o (LTO objects, wrong nm, or an empty object list)"
grep -qx "WeXterm.o XOpenDisplay" "$WORK/imports" ||
  problem "canary: XOpenDisplay not seen in WeXterm.o (the prefix scan of the X11 connection is blind)"

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
# Every display decodes its keys through the one table: a display that does
# not import wpe_key_code has gone back to a ladder of its own, which the
# name scan below would not see while it spells no new name.
for display in we_term.o we_xterm.o we_wayland.o; do
  for obj in $OBJECTS; do
    [ "$(basename "$obj")" = "$display" ] || continue
    [ -f "$obj" ] || continue
    "$NMTOOL" -u "$obj" 2>/dev/null | normalise > "$WORK/display"
    grep -qx wpe_key_code "$WORK/display" ||
      problem "canary: $display does not import wpe_key_code (keys are decoded some other way)"
  done
done
# The screen's owner: the key loop shows what waited behind a box through
# it, and the dialog engines tell it they own the screen.  An object that
# stops importing these has gone back to a flag or a replay of its own.
canary_import() {
  for obj in $OBJECTS; do
    [ "$(basename "$obj")" = "$1" ] || continue
    [ -f "$obj" ] || continue
    "$NMTOOL" -u "$obj" 2>/dev/null | normalise | grep -qx "$2" ||
      problem "canary: $1 does not import $2 ($3)"
  done
}
canary_import we_edit.o wpe_paint_replay_if_idle "what came behind a box is shown some other way"
canary_import we_paint.o wpe_loop_hooks_set "the loop's callbacks are not bracketed by the owner"
# Which key was pressed has one owner (we_keymap.c): the terminal reads an
# escape sequence's tail, the keyboard protocols' reports among them, through
# its decode, and X11 and Wayland turn a chord into a code through its
# character table.
canary_import we_term.o wpe_key_decode_csi "the terminal reads a key report some other way"
canary_import we_xterm.o wpe_key_code_char "X11 decides a Ctrl or Alt chord some other way"
canary_import we_wayland.o wpe_key_code_char "Wayland decides a Ctrl or Alt chord some other way"
for engine in we_menue.o we_ai_ui.o; do
  canary_import $engine wpe_paint_modal_enter "a box owns the screen without telling its owner"
done
# A box that moved onto the layout owner (we_layout.c) opens through
# wpe_box_open, which tells the screen's owner for it.
for engine in we_opt.o we_wind.o; do
  canary_import $engine wpe_box_open "a box owns the screen without opening through the layout owner"
done
# The menu bar, the prefixes, the move/size loop and every box (dialogs and
# their text fields, dropdowns, error boxes, pickers, the colour menu, the
# assistant's boxes and its changeset review) read their keys through the
# owner's bracket; the Run loop and a short command's Esc take a key
# through the owner's one key peek, which every display answers alike.
for loop in we_menue.o we_edit.o we_debug.o we_wind.o we_opt.o we_e_aus.o \
            we_fl_fkt.o we_unix.o we_ai_ui.o we_ai_ws.o; do
  canary_import $loop wpe_paint_modal_getch "a key loop reads its keys around the owner"
done
# Gone for good: the second flags, idle tests and replays the screen's owner
# replaced, and the old key test each display answered its own way.  An
# object that exports one again has brought back a flag or a replay of its
# own beside the owner.  (A replay body the owner runs may keep its name as
# a file's own static function: it is not exported.)
DELETED_SYMBOLS="e_build_ui_idle e_lsp_ui_safe ai_editor_idle
  e_lsp_modal_enter e_lsp_modal_leave e_lsp_modal_sync_chrome
  wpe_ai_deliver_held_reply e_build_deliver_held e_d_start_deliver_held
  e_d_run_deliver_held_stop e_u_kbhit"
defined_names() {
  "$NMTOOL" -g --defined-only "$1" 2>/dev/null | awk '
    NF >= 2 && $(NF-1) != "U" {
      n = $NF
      sub(/@.*/, "", n)
      sub(/\$.*/, "", n)
      print n
      if (n ~ /^_/) { m = n; sub(/^_/, "", m); print m }
    }'
}
for obj in $OBJECTS; do
  [ -f "$obj" ] || continue
  defined_names "$obj" > "$WORK/defined"
  for sym in $DELETED_SYMBOLS; do
    grep -qx "$sym" "$WORK/defined" &&
      problem "deleted symbol: $(basename "$obj") defines $sym again (the screen has one owner, we_paint.c)"
  done
done
# The canary of that rule: a scanner that lists no definitions sees nothing.
for obj in $OBJECTS; do
  [ "$(basename "$obj")" = we_paint.o ] || continue
  [ -f "$obj" ] || continue
  defined_names "$obj" | grep -qx wpe_paint_modal_getch ||
    problem "canary: wpe_paint_modal_getch not seen defined in we_paint.o (the deleted-symbol scan is blind)"
done
for taker in we_prog.o we_unix.o; do
  canary_import $taker wpe_paint_take_key "a key is taken without waiting some other way"
done
# The terminal's mode owner: the Run loop turns the mouse reports off and on
# through the terminal session, which writes them only while the terminal is
# the editor's.  A loop that goes back to the mouse module's own calls has
# stopped asking the owner (the symbol rule above would also name it).
canary_import we_prog.o e_t_mouse_tracking_pause "the Run loop's mouse modes are written some other way"
# The display's colours: the User Screen asks the display table for the
# palette, and the Cairo renderer reads the colours the X server allocated.
# Neither asks the server again, so no object imports XQueryColor, which
# would read the default colormap and miss a private one.
canary_import we_vterm.o e_u_palette_rgb "the User Screen reads the palette some other way"
canary_import we_render_cairo.o e_x_palette_rgb16 "the Cairo renderer reads the colours some other way"
# The debugger asks the display table to give the editor the keyboard at a
# start and to raise it at a stop; it names no display's function.
canary_import we_debug.o e_u_focus_editor "a debug start gives the keyboard back some other way"
canary_import we_debug.o e_u_raise_editor "a debug stop raises the editor some other way"
# Alt-Insert, Alt-Delete and a dialog field's Alt-paste go through the one
# clipboard path (Paste and Copy), which every display fills; the editor
# keys and the field name those functions and no display's copy of them.
canary_import we_edit.o e_edt_einf "Alt-Insert pastes some other way than Paste"
canary_import we_edit.o e_edt_copy "Alt-Delete copies some other way than Copy"
canary_import we_e_aus.o e_clip_pull_os_into_buffer "a field pastes the selection some other way"
# The text's owner: Enter and the line wrap add their lines through it, and
# the character and block kinds record through it.  The owner itself is
# pure by its imports: an editor symbol in it is a display, a dialog or the
# debugger pulled into the one file the C test links alone.
canary_import we_edit.o e_buffer_split_line "Enter adds lines some other way"
canary_import we_edit.o e_buffer_insert_blank "the line wrap adds lines some other way"
canary_import we_edit.o e_buffer_record_push "the char kinds record some other way"
canary_import we_edit.o e_buffer_undo "Undo replays some other way"
canary_import we_block.o e_buffer_txn_commit "the block code moves lines some other way"
canary_import we_block.o e_buffer_record_push "the block kinds record some other way"
canary_import we_block.o e_buffer_shell_new "a block delete makes its shell some other way"
canary_import we_edit.o e_buffer_record_text "a char record copies its text some other way"
# The positions' owner: the painter puts a cursor that is off a glyph on one
# through it.  It proves a call exists, not that no raw write is left; that
# is the self-test's and the counted lines' work.  The block code and the
# mouse join this list when they stop writing the cursor in place.
canary_import we_edit.o e_buffer_cursor_set "the cursor is put on a glyph some other way"
BUFFER_OWNER_SEEN=0
for obj in $OBJECTS; do
  [ "$(basename "$obj")" = we_buffer.o ] || continue
  [ -f "$obj" ] || continue
  BUFFER_OWNER_SEEN=1
  "$NMTOOL" -u "$obj" 2>/dev/null | normalise > "$WORK/owner"
  for sym in e_error e_schirm e_refresh WpeEditor e_sc_nw_txt e_sc_txt e_brk_recalc \
             e_make_breakpoint e_getch e_switch_window e_set_pnt; do
    grep -qx "$sym" "$WORK/owner" &&
      problem "we_buffer.o imports $sym (the buffer owner is no longer pure)"
  done
  grep -qx malloc "$WORK/owner" ||
    problem "canary: malloc not seen in we_buffer.o (the purity scan is blind)"
done
[ $BUFFER_OWNER_SEEN -eq 1 ] ||
  problem "canary: we_buffer.o is not among the objects (the text's owner is not built)"
# Gone for good: the line primitives, the whole-buffer rebuilds, the output
# appenders and the global undo covers the text's owner replaced.  No object
# of the editor defines one again (`nm -g --defined-only`): a function that
# adds or removes lines on its own beside the owner has come back.  A name
# may stay as a file's own static function; it is not exported.
DELETED_BUFFER_SYMBOLS="e_new_line e_del_line e_buffer_set_text
  e_buffer_insert_raw e_replace_buffer_undoable e_rudo_whole_buffer
  e_p_red_buffer print_to_end_of_buffer e_build_drop_last_row e_brk_recalc
  e_remove_undo e_undo_sw e_redo_sw"
for obj in $OBJECTS; do
  [ -f "$obj" ] || continue
  defined_names "$obj" > "$WORK/defined"
  for sym in $DELETED_BUFFER_SYMBOLS; do
    grep -qx "$sym" "$WORK/defined" &&
      problem "deleted symbol: $(basename "$obj") defines $sym again (the text has one owner, we_buffer.c)"
  done
done
# The canary of that rule: a scanner that lists no definitions sees nothing.
for obj in $OBJECTS; do
  [ "$(basename "$obj")" = we_buffer.o ] || continue
  [ -f "$obj" ] || continue
  defined_names "$obj" | grep -qx e_buffer_splice ||
    problem "canary: e_buffer_splice not seen defined in we_buffer.o (the deleted-symbol scan of the text is blind)"
done
DELETED_DISPLAY_SYMBOLS="e_x_clear_area e_x_vterm_user_screen e_x_vterm_load_palette
  e_x_change e_d_x_focus_to_editor e_x_reclaim_focus
  e_x_cp_X_to_buffer e_x_copy_X_buffer e_x_paste_X_buffer
  e_t_copy_X_buffer e_t_paste_X_buffer"
for obj in $OBJECTS; do
  [ -f "$obj" ] || continue
  defined_names "$obj" > "$WORK/defined"
  for sym in $DELETED_DISPLAY_SYMBOLS; do
    grep -qx "$sym" "$WORK/defined" &&
      problem "deleted symbol: $(basename "$obj") defines $sym again (shared code asks the display table, edit.h)"
  done
  "$NMTOOL" -u "$obj" 2>/dev/null | normalise | grep -qx XQueryColor &&
    problem "deleted symbol: $(basename "$obj") imports XQueryColor (the editor's colours come from the display's palette)"
done
# The fill scan: every display assigns every slot of the display table, with
# an implementation or a named function that does nothing, so no slot is
# empty when shared code calls it.  A slot one kind of display reads is an
# exception with its kind: "graphical" ones must be assigned by X11 and
# Wayland, "console" ones by the terminal, "none" by no display.  A slot the
# headers declare and no table lists is a problem too: a new service needs
# its fills before it is used.
FILLS=$(echo fk_u_locate fk_u_cursor fk_u_putchar u_bioskey e_frb_u_menue \
  e_s_u_clr e_n_u_clr e_pr_u_col_kasten fk_mouse fk_u_grab_pointer \
  fk_u_drag_next e_u_refresh e_u_getch e_u_sys_ini e_u_sys_end \
  WpeMouseChangeShape WpeMouseRestoreShape e_u_d_switch_out \
  e_u_switch_screen e_u_deb_out e_u_system e_u_key_take WpeDisplayEnd \
  e_clip_os_set e_u_focus_editor e_u_raise_editor e_u_palette_rgb \
  e_u_serve_display e_u_repaint_resized e_clip_os_get WpeDisplayReleaseFromHandler)
# "kind  slot  reason"
cat > "$WORK/fillexcept" <<'EOF'
graphical e_u_ini_size read under WpeIsXwin (we_opt.c) and by the X11 backend
graphical e_u_setlastpic read under WpeIsXwin (we_wind.c) and by the X11 backend
console e_u_s_sys_ini read under !WpeIsXwin (we_fl_unix.c) and from e_system (we_prog.c)
console e_u_s_sys_end read under !WpeIsXwin (we_fl_unix.c) and from e_system (we_prog.c)
none e_paste_end_hook not a display slot: the hook the paste owner sets
EOF
fill_check() {   # source file, slot
  if [ -f "$SRCDIR/$1" ]; then
    [ -f "$WORK/stripped/$1" ] || strip_c "$SRCDIR/$1" > "$WORK/stripped/$1"
    [ "$(count_assign "$2" "$WORK/stripped/$1")" -gt 0 ] ||
      problem "fill: $1 does not assign $2"
  else
    problem "canary: $1 not found under $SRCDIR (the fill scan is blind)"
  fi
}
for slot in $FILLS; do
  for display in we_term.c we_xterm.c we_wayland.c; do
    fill_check $display $slot
  done
done
while read -r kind slot reason; do
  case $kind in
    graphical) for display in we_xterm.c we_wayland.c; do fill_check $display $slot; done ;;
    console) fill_check we_term.c $slot ;;
  esac
done < "$WORK/fillexcept"
: > "$WORK/declared"
for header in edit.h we_clip.h; do
  if [ -f "$SRCDIR/$header" ]; then
    strip_c "$SRCDIR/$header" > "$WORK/stripped/$header"
    declared_slots "$WORK/stripped/$header" >> "$WORK/declared"
  else
    problem "canary: $header not found under $SRCDIR (the fill scan is blind)"
  fi
done
[ "$(wc -l < "$WORK/declared" | tr -d ' ')" -ge 20 ] ||
  problem "canary: fewer than 20 function-pointer slots found in edit.h and we_clip.h (the slot scan is blind)"
while read -r slot; do
  case " $FILLS " in *" $slot "*) continue ;; esac
  grep -qE "^[a-z]+ $slot " "$WORK/fillexcept" && continue
  problem "fill: $slot is declared as a slot but is in no table (dead, or a new service without its fills)"
done < "$WORK/declared"
# The language server's columns: the owner is built and defines its
# conversions, the bridge and the engine import them (a file that stops
# importing one has gone back to reading a column on its own), and each of
# the three source rules sees what it guards in the owner itself.
LSP_OWNER_SEEN=0
for obj in $OBJECTS; do
  [ "$(basename "$obj")" = we_lsp_pos.o ] || continue
  [ -f "$obj" ] || continue
  LSP_OWNER_SEEN=1
  defined_names "$obj" > "$WORK/defined"
  for sym in e_lsp_units_of e_lsp_col_of e_lsp_units_read e_lsp_position_json \
             e_lsp_text_offset e_lsp_text_index_col e_lsp_semantic_decode; do
    grep -qx "$sym" "$WORK/defined" ||
      problem "canary: $sym not defined in we_lsp_pos.o (the owner of the server's columns is incomplete)"
  done
done
[ $LSP_OWNER_SEEN -eq 1 ] ||
  problem "canary: we_lsp_pos.o is not among the objects (the owner of the server's columns is not built)"
canary_import we_debug.o e_lsp_units_of "the bridge no longer converts the server's columns"
canary_import we_debug.o e_lsp_text_index_col "the bridge no longer converts the server's columns"
canary_import we_lsp.o e_lsp_units_read "the engine reads a position without the owner"
canary_import we_lsp.o e_lsp_position_json "the engine writes a position without the owner"
canary_import we_lsp.o e_lsp_text_index_offset "the engine edits text by a position without the owner"
canary_import we_lsp.o e_lsp_encoding_parse "the engine reads the negotiated encoding without the owner"
if lsppos=$(stripped_source we_lsp_pos.o); then
  [ "$(count_uses e_lsp_units_make "$lsppos")" -gt 0 ] ||
    problem "canary: the source scan sees no e_lsp_units_make( in we_lsp_pos.c (the call-form rule is blind)"
  [ "$(count_uses_re "$(lsp_units_rule_re)" "$lsppos")" -gt 0 ] ||
    problem "canary: the source scan sees no units_on_wire in we_lsp_pos.c (the field rule is blind)"
else
  problem "canary: we_lsp_pos.c not found under $SRCDIR"
fi
if lsppos=$(commented_source we_lsp_pos.c); then
  [ "$(count_uses_re '"character"' "$lsppos")" -gt 0 ] ||
    problem "canary: the source scan sees no \"character\" key in we_lsp_pos.c (the key rule is blind)"
else
  problem "canary: we_lsp_pos.c not found under $SRCDIR"
fi

# The search over a row has one owner (we_rowfind.c): the regular expression
# engine is run only there (the symbol rule), so no caller decides what the
# engine sees, what a character, a word or an empty hit is; REG_NOTEOL, the
# flag that keeps $ for the end of a line and not of a wrapped row, and the
# word-byte test (isalnum) are the owner's, as are the locale calls that give
# the engine a UTF-8 ctype.  The canaries: the symbol scan must see regexec
# imported by the owner's object, and the source scan must see the flag in
# its source, or a blind scanner would pass for a clean tree.
grep -qx "we_rowfind.o regexec" "$WORK/imports" ||
  problem "canary: regexec not seen in we_rowfind.o (the search owner is not built, or the symbol scan is blind)"
if rowfind=$(stripped_source we_rowfind.o); then
  [ "$(count_words REG_NOTEOL "$rowfind")" -gt 0 ] ||
    problem "canary: the source scan sees no REG_NOTEOL in we_rowfind.c (the source rules of the search owner are blind)"
  [ "$(count_words isalnum "$rowfind")" -gt 0 ] ||
    problem "canary: the source scan sees no isalnum in we_rowfind.c (the word rule of the search owner is blind)"
else
  problem "canary: we_rowfind.c not found under $SRCDIR"
fi

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
      'read(0') n=$(count_uses_re "$READ_STDIN_RE" "$file") ;;
      'fileno(stdin)') n=$(count_uses "fileno[ \t]*[(][ \t]*stdin" "$file") ;;
      wpe_fd_wait_for|wpe_fd_end_wait|wpe_fd_wait_serving|e_shell_run|write|fdopen|KEY_F|e_getch|e_lsp_units_make) n=$(count_uses "$word" "$file") ;;
      lsp_units_field) n=$(count_uses_re "$(lsp_units_rule_re)" "$file") ;;
      lsp_char_key) n=$(count_uses_re '"character"' "$(commented_source "$c")") ;;
      XK_|XKB_KEY_|e_x_) n=$(count_words "$word[A-Za-z0-9_]+" "$file") ;;
      mxlines_write) n=$(count_uses_re 'mxlines[ \t]*[)]?[ \t]*([+][+]|--|[+]=|-=|=[^=])|([+][+]|--)[ \t]*[(]?[A-Za-z0-9_>.-]*mxlines' "$file") ;;
      pos_field|pos_prefix|pos_whole|pos_address) n=$(count_uses_re "$(pos_rule_re "$word")" "$file") ;;
      bf_alloc) n=$(count_uses_re 'bf[ \t]*=[ \t]*([(][ \t]*STRING[ \t]*[*][ \t]*[)][ \t]*)?(MALLOC|malloc|REALLOC|realloc|calloc)[ \t]*[(]|(REALLOC|realloc)[ \t]*[(][^,]*bf[ \t]*,' "$file") ;;
      term_raw_read)
        [ "$c" = we_term.c ] || continue
        n=$(count_raw_reads "$file") ;;
      display_key_literal)
        case $c in we_term.c|we_xterm.c|we_wayland.c|WeXterm.c) ;; *) continue ;; esac
        n=$(count_display_literals "$(commented_source "$c")") ;;
      term_output)
        [ "$c" = we_term.c ] || continue
        n=$(count_term_output "$file") ;;
      ris_literal) n=$(count_uses_re "$RIS_LITERAL_RE" "$(commented_source "$c")") ;;
      mouse_mode_call) n=$(count_mouse_mode_calls "$file") ;;
      *) n=$(count_words "$word" "$file") ;;
    esac
    [ "$n" -gt 0 ] && echo "$base $word $n" >> "$WORK/found"
  done < "$WORK/srcrules"
done

sort -u "$WORK/found" -o "$WORK/found"

if [ $LIST -eq 1 ]; then
  cat "$WORK/found"
  awk '{ print "# " $0 }' "$WORK/problems"   # a comment line in an allow file
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

# The screen's section is strict in every mode: no box, menu or flag of its
# own waits for a later pass, so an allow line for one of its names must be
# a permanent exception with its reason.
SCREEN_WORDS="e_getch wpe_modal_active g_lsp_modal_depth g_lsp_ui_suspended wpe_chrome_suppress"
awk -v words=" $SCREEN_WORDS " '
  $4 != "keep" && index(words, " " $2 " ") {
    print "the screen section is strict: the allow line \"" $1 " " $2 "\" (" $4 ") may only be a keep line"
  }' "$WORK/allowed" >> "$WORK/problems"

# The text's section is strict in every mode as well: every name of the
# owner's rules and deleted list is allowed only as a permanent exception.
BUFFER_WORDS="e_new_line e_del_line e_buffer_set_text e_buffer_insert_raw
  e_replace_buffer_undoable e_rudo_whole_buffer e_p_red_buffer
  print_to_end_of_buffer e_build_drop_last_row e_brk_recalc e_add_undo
  e_remove_undo e_undo_sw e_redo_sw mxlines_write bf_alloc
  pos_field pos_prefix pos_whole pos_address"
awk -v words=" $(echo $BUFFER_WORDS) " '
  $4 != "keep" && index(words, " " $2 " ") {
    print "the text section is strict: the allow line \"" $1 " " $2 "\" (" $4 ") may only be a keep line"
  }' "$WORK/allowed" >> "$WORK/problems"

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
