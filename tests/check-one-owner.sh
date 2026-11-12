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
EOF

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
read(0 we_term.c
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
e_undo_sw
e_redo_sw
EOF

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
for engine in we_opt.o we_menue.o we_wind.o we_ai_ui.o; do
  canary_import $engine wpe_paint_modal_enter "a box owns the screen without telling its owner"
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
  e_u_serve_display e_clip_os_get)
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
      wpe_fd_wait_for|wpe_fd_end_wait|wpe_fd_wait_serving|e_shell_run|write|fdopen|KEY_F|e_getch) n=$(count_uses "$word" "$file") ;;
      XK_|XKB_KEY_|e_x_) n=$(count_words "$word[A-Za-z0-9_]+" "$file") ;;
      mxlines_write) n=$(count_uses_re 'mxlines[ \t]*[)]?[ \t]*([+][+]|--|[+]=|-=|=[^=])|([+][+]|--)[ \t]*[(]?[A-Za-z0-9_>.-]*mxlines' "$file") ;;
      bf_alloc) n=$(count_uses_re 'bf[ \t]*=[ \t]*([(][ \t]*STRING[ \t]*[*][ \t]*[)][ \t]*)?(MALLOC|malloc|REALLOC|realloc|calloc)[ \t]*[(]|(REALLOC|realloc)[ \t]*[(][^,]*bf[ \t]*,' "$file") ;;
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
  e_remove_undo e_undo_sw e_redo_sw mxlines_write bf_alloc"
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
