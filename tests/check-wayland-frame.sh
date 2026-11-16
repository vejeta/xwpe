#!/bin/sh
# check-wayland-frame.sh -- the Wayland surface has one owner of the size it
# may commit.
#
# The compositor closes an editor whose frame does not fit the configure it
# answers, and a window that nothing acknowledged never appears.  That holds
# only while one piece of code (we_wlframe.c) allocates the buffer, answers a
# configure and commits, and every other file only records what the
# compositor said.  A second piece that commits, or a listener that
# acknowledges, is how a frame once went out at the old size.
#
#   check-wayland-frame.sh [--allow FILE] [--srcdir DIR] [--selftest]
#
# Rules:
#  1. These identifiers occur only in we_wlframe.c: wl_surface_attach,
#     wl_surface_damage, wl_surface_damage_buffer, wl_surface_commit,
#     xdg_surface_ack_configure, xdg_surface_set_window_geometry and the
#     owner's queue functions (wl_proxy_set_queue, wl_display_create_queue,
#     wl_event_queue_destroy, wl_display_dispatch_queue,
#     wl_display_dispatch_queue_pending, wl_display_roundtrip_queue,
#     wl_display_prepare_read_queue, wl_display_prepare_read,
#     wl_display_read_events, wl_display_cancel_read).  The cursor's
#     own surface, which has no xdg role, is a line of the allow file.
#  2. wl_proxy_marshal and its family (wl_proxy_marshal_flags, ...) occur
#     nowhere outside the generated protocol files: nobody bypasses the
#     inline request wrappers.
#  3. The buffer is allocated and its handles written only in we_wlframe.c:
#     WpeWl.width, height, pixels, buffer, stride, shm_fd and shm_size
#     assigned, changed (++, --, op=) or had their address taken;
#     wl_shm_create_pool, wl_shm_pool_create_buffer and wl_buffer_destroy;
#     and a pointer to WpeWlInfo (an alias that would write them).
#  4. The compositor connection has one owner: wl_display_dispatch,
#     wl_display_dispatch_pending, wl_display_roundtrip and wl_display_flush
#     occur only inside wl_conn_dispatch, wl_conn_roundtrip, wl_conn_flush and
#     wpe_wl_conn_flush of we_wayland.c, so a broken connection is noticed in
#     one place and every other function is inert after it.
#  5. The lost display has one owner: XSetIOErrorHandler occurs once, in
#     WpeXInit of WeXterm.c, and e_display_lost is defined once (we_unix.c),
#     declared once (edit.h) and called from exactly two functions
#     (wl_conn_lost, e_x_io_error).  The counted places are lines of the
#     allow file, so a third caller, or a second handler, is named.
#  6. The listeners xdg_surface_configure and xdg_toplevel_configure call
#     nothing but wpe_wlframe_note_configure and wpe_wlframe_note_toplevel.
#  7. Token pasting that builds a wl_ or xdg_ name (a macro that would hide a
#     banned identifier from rules 1 to 3).
#
# Identifiers are matched as whole words, not as calls: a function pointer
# (= wl_surface_commit;) is as much an offender as the call.  Comments and the
# contents of string and character literals are blanked (a name inside a
# string is text, not a use; the canaries clean-comment and clean-string hold
# that), a call split over lines is joined, and what is found is counted per
# function.  The generated protocol files are not scanned.
#
# Canaries (--selftest, and always at the start of a run): an offender per
# rule, on one line and on two, and a clean use of each in the owner's own
# file name; a scanner that does not name every offender, or names a clean
# one, fails the script as blind.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
BAD=1
ALLOW=
SELFTEST=0

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --allow) shift; ALLOW=$1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "check-wayland-frame: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done

[ -n "$ALLOW" ] || ALLOW=$SRCDIR/tests/wayland-frame.allow
if [ ! -f "$ALLOW" ]; then
  echo "check-wayland-frame: no allow file at $ALLOW" >&2
  exit $BAD
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/wayland-frame.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

# The scan reads C as bytes: in a UTF-8 locale substr and length count
# characters, which costs the size of the file at every step on some awks.
LC_ALL=C
export LC_ALL
# The one blanker of comments and literals (tests/blank-c.awk).
BLANK_C=$(cat "$(dirname "$0")/blank-c.awk")

# scan FILE... - "file:line: rN function identifier" for each offender.
#
# The text of a file is one string, `all`, split once into the array `ch` of
# its characters when the file ends; every walk indexes that array.
scan() {
  awk "$BLANK_C"'
    function report(at, rule, tok,   lo, hi, mid, f, name) {
      lo = 1; hi = nl
      while (lo < hi) {
        mid = int((lo + hi + 1) / 2)
        if (start[mid] <= at) lo = mid; else hi = mid - 1
      }
      f = funcat(at)
      name = (f > 0) ? fn[f] : "-"
      if (name == "") name = "-"
      print file ":" lo ": " rule " " name " " tok
    }
    function say_macro(line, rule, tok) {
      print file ":" FNR ": " rule " (macro) " tok
    }
    function funcat(at,   k) {
      for (k = 1; k <= nf; k++) if (at > fs[k] && at < fe[k]) return k
      return 0
    }
    function funcname(seg) {
      if (match(seg, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) {
        seg = substr(seg, RSTART, RLENGTH)
        sub(/[ \t]*\($/, "", seg)
        return seg
      }
      return ""
    }
    # next_call(from, last) - the first "identifier (" in ch[from..last]:
    # sets call_id, call_at (where the identifier starts) and call_end (just
    # past the "("), and returns 1; or returns 0 when there is none.
    function next_call(from, last,   i, j, k, m) {
      i = from
      while (i <= last) {
        if (ch[i] !~ /[A-Za-z0-9_]/) { i++; continue }
        j = i
        while (j <= last && ch[j] ~ /[A-Za-z0-9_]/) j++
        k = i
        while (k < j && ch[k] ~ /[0-9]/) k++
        m = j
        while (m <= last && ch[m] ~ /[ \t]/) m++
        if (k < j && m <= last && ch[m] == "(") {
          call_id = chars_span(ch, k, j - 1); call_at = k; call_end = m + 1
          return 1
        }
        i = j
      }
      return 0
    }
    function banned1(id) {
      return (id in r1)
    }
    function banned3(id) {
      return (id in r3)
    }
    function listener(name) {
      return (name == "xdg_surface_configure" || name == "xdg_toplevel_configure")
    }
    function allowed_in_listener(id) {
      return (id == "wpe_wlframe_note_configure" || id == "wpe_wlframe_note_toplevel" || id == "if" || id == "while" || id == "for" || id == "return" || id == "sizeof" || id == "switch")
    }
    function address_of(at,   j) {
      j = at - 1
      while (j >= 1 && ch[j] ~ /[ \t]/) j--
      if (j < 1 || ch[j] != "&") return 0
      return ch[j - 1] != "&"
    }
    function conn_function(at,   k) {
      k = funcat(at)
      if (k <= 0) return 0
      return (base == "we_wayland.c" && (fn[k] in conn_ok))
    }
    function check(id, at, after) {
      if (id in r4 && !conn_function(at)) report(at, "r4", id)
      if (id in r5) report(at, "r5", id)
      if (!owner && banned1(id)) report(at, "r1", id)
      if (id ~ /^wl_proxy_marshal/) report(at, "r2", id)
      if (!owner && banned3(id)) report(at, "r3", id)
      if (!owner && id == "WpeWl") {
        if (after ~ field_write) report(at, "r3", "WpeWl.write")
        else if (address_of(at) && after ~ field_any) report(at, "r3", "&WpeWl.field")
      }
      if (!owner && id == "WpeWlInfo" && after ~ /^[ \t]*\*/) report(at, "r3", "WpeWlInfo*")
    }
    function scan_listener(k,   from) {
      from = fs[k] + 1
      while (next_call(from, fe[k] - 1)) {
        if (!allowed_in_listener(call_id)) report(call_at, "r6", call_id)
        from = call_end
      }
    }
    # watched(id) - an identifier some rule has an opinion on.
    function watched(id) {
      return (id in r1 || id in r3 || id in r4 || id in r5 || id ~ /^wl_proxy_marshal/ || id == "WpeWlInfo" || id == "WpeWl")
    }
    function finish(   i, j, n, c, depth, head, segstart, id, k, seen_s, seen_t) {
      if (!mentioned && base != "we_wayland.c") return
      n = chars_load(seg, nl, ch)
      depth = 0; segstart = 1; nf = 0
      for (i = 1; i <= n; i++) {
        c = ch[i]
        if (c == "{") {
          if (depth == 0) {
            head = chars_span(ch, segstart, i - 1)
            nf++; fs[nf] = i; fn[nf] = funcname(head); fe[nf] = n + 1
          }
          depth++
        } else if (c == "}") {
          depth--
          if (depth == 0) { fe[nf] = i; segstart = i + 1 }
        } else if (c == ";" && depth == 0) segstart = i + 1
      }
      i = 1
      while (i <= n) {
        if (ch[i] !~ /[A-Za-z0-9_]/) { i++; continue }
        j = i; id = ""
        while (j <= n && ch[j] ~ /[A-Za-z0-9_]/) { id = id ch[j]; j++ }
        if (watched(id)) check(id, i, chars_span(ch, j, j + 159))
        i = j
      }
      for (k = 1; k <= nf; k++) if (listener(fn[k])) scan_listener(k)
      if (base == "we_wayland.c") {
        seen_s = seen_t = 0
        for (k = 1; k <= nf; k++) {
          if (fn[k] == "xdg_surface_configure") seen_s = 1
          if (fn[k] == "xdg_toplevel_configure") seen_t = 1
        }
        if (!seen_s) print file ":0: r6 - listener-xdg_surface_configure-not-found"
        if (!seen_t) print file ":0: r6 - listener-xdg_toplevel_configure-not-found"
      }
    }
    BEGIN {
      split("wl_surface_attach wl_surface_damage wl_surface_damage_buffer wl_surface_commit xdg_surface_ack_configure xdg_surface_set_window_geometry wl_proxy_set_queue wl_display_create_queue wl_event_queue_destroy wl_display_dispatch_queue wl_display_dispatch_queue_pending wl_display_roundtrip_queue wl_display_prepare_read_queue wl_display_prepare_read wl_display_read_events wl_display_cancel_read", l1, " ")
      for (k in l1) r1[l1[k]] = 1
      split("wl_shm_create_pool wl_shm_pool_create_buffer wl_buffer_destroy", l3, " ")
      for (k in l3) r3[l3[k]] = 1
      split("wl_display_dispatch wl_display_dispatch_pending wl_display_roundtrip wl_display_flush", l4, " ")
      for (k in l4) r4[l4[k]] = 1
      split("wl_conn_dispatch wl_conn_roundtrip wl_conn_flush wpe_wl_conn_flush", l4f, " ")
      for (k in l4f) conn_ok[l4f[k]] = 1
      split("XSetIOErrorHandler e_display_lost", l5, " ")
      for (k in l5) r5[l5[k]] = 1
      fields = "(width|height|pixels|buffer|stride|shm_fd|shm_size)"
      field_any = "^[ \t]*\\.[ \t]*" fields "([^A-Za-z0-9_]|$)"
      field_write = "^[ \t]*\\.[ \t]*" fields "[ \t]*(=([^=]|$)|\\+\\+|--|[-+*/|&^]=|<<=|>>=)"
    }
    # may_mention(line) - the line may hold a watched identifier or a
    # listener: every one of them starts with one of these five prefixes.
    # A file with no such line has nothing for finish() to find, so it is
    # not walked.  (Plain index(): a regular expression held in a variable
    # is compiled again at every line by some awks.)
    function may_mention(line) {
      return (index(line, "wl_") || index(line, "xdg_") || index(line, "WpeWl") || index(line, "XSetIOErrorHandler") || index(line, "e_display_lost"))
    }
    FNR == 1 {
      if (file != "") finish()
      file = FILENAME; base = file; sub(/^.*\//, "", base)
      owner = (base == "we_wlframe.c" || base == "we_wlframe.h")
      delete seg; alln = 0; nl = 0; blank_incom = 0; inmacro = 0; nf = 0; mentioned = 0
    }
    {
      line = blank_c($0, 0)
      if (may_mention(line)) mentioned = 1
      nl++
      start[nl] = alln + 1
      if (inmacro || line ~ /^[ \t]*#/) {
        macro_line(line)
        inmacro = ($0 ~ /\\[ \t]*$/)
        seg[nl] = "; "
      } else {
        seg[nl] = line " "
      }
      alln += length(seg[nl])
    }
    function macro_line(line,   rest, id, pos, n) {
      pos = 1; n = length(line)
      if (line ~ /(wl|xdg)_[A-Za-z0-9_]*[ \t]*##/ || line ~ /##[ \t]*[A-Za-z0-9_]*(wl|xdg)_/) say_macro(line, "r7", "##")
      while (pos <= n) {
        rest = substr(line, pos)
        if (!match(rest, /[A-Za-z_][A-Za-z0-9_]*/)) break
        id = substr(rest, RSTART, RLENGTH)
        pos = pos + RSTART - 1 + RLENGTH
        if (id in r4) say_macro(line, "r4", id)
        if (id in r5) say_macro(line, "r5", id)
        if (!owner && banned1(id)) say_macro(line, "r1", id)
        if (id ~ /^wl_proxy_marshal/) say_macro(line, "r2", id)
        if (!owner && banned3(id)) say_macro(line, "r3", id)
      }
    }
    END { if (file != "") finish() }' "$@"
}

# judge ALLOWFILE FOUND - what is left of the scan's FOUND lines once the
# counted exceptions of ALLOWFILE are taken off: an identifier the file does
# not list, more than it says, fewer than it says (lower or delete the line),
# and a line that matches nothing.  Prints one problem a line.
judge() {
  printf '%s\n' "$2" | awk -v allow="$1" '
    function base(s) { sub(/:[0-9]+: .*$/, "", s); sub(/^.*\//, "", s); return s }
    function explain(s) {
      if (s ~ / r4 /) return "libwayland connection I/O outside the wl_conn functions"
      if (s ~ / r5 /) return "lost-display handler or caller not on the allow file"
      return "outside the frame owner"
    }
    function rulefunc(s,   t) { t = s; sub(/^[^ ]*: /, "", t); split(t, p, " "); return p[1] ":" p[2] }
    BEGIN {
      while ((getline line < allow) > 0) {
        if (line ~ /^[ \t]*(#|$)/) continue
        n = split(line, f, "|")
        if (n < 5 || f[3] !~ /^[0-9]+$/ || f[4] == "" || f[5] == "") { print "bad line in the allow file: " line; continue }
        key = f[1] "|" f[2]
        if (key in al) { print "duplicate allow line for " key; continue }
        al[key] = f[3]
      }
    }
    NF == 0 { next }
    {
      key = base($0) "|" rulefunc($0)
      seen[key]++
      if (!(key in al)) print explain($0) ": " $0
    }
    END {
      for (k in seen) if (k in al) {
        if (seen[k] > al[k] + 0) print "more than allowed: " k " has " seen[k] ", the allow file says " al[k]
        else if (seen[k] < al[k] + 0) print "fewer than the allow file says: " k " has " seen[k] ", lower the line"
      }
      for (k in al) if (!(k in seen)) print "stale allow line, delete it: " k
    }' | sort
}

# A canary is a file planted under a given name: EXPECT is the pattern a line
# of the scan must match ("rN function identifier"), or none when the scan must
# stay silent about this rule.
planted() {
  mkdir -p "$WORK/plant"
  printf '%s\n' "$3" > "$WORK/plant/$2"
  scan "$WORK/plant/$2"
}

# canary NAME FILE CODE EXPECT - EXPECT is an extended regular expression a
# line of the scan must match, or the word none for a file the scan must not
# name at all.
canary() {
  got=$(planted "$1" "$2" "$3")
  if [ "$4" = none ]; then
    if [ -n "$got" ]; then
      echo "canary $1: a use that is fine was named: $got"
      return 1
    fi
    return 0
  fi
  if ! printf '%s\n' "$got" | grep -Eq "$4"; then
    echo "canary $1: a planted offender was not found (wanted $4, got: $got)"
    return 1
  fi
  return 0
}

blind=0
# rule 1, one line and split; and as a function pointer, not a call
canary r1-commit c.c 'void f(void) { wl_surface_commit(s); }' ' r1 f wl_surface_commit' || blind=1
canary r1-split c.c 'void f(void) { wl_surface_attach(s,
   b, 0, 0); }' ' r1 f wl_surface_attach' || blind=1
canary r1-pointer c.c 'void (*g)(struct wl_surface *) = wl_surface_commit; void f(void) { }' ' r1 - wl_surface_commit' || blind=1
canary r1-ack c.c 'void f(void) { xdg_surface_ack_configure(x, 1); }' ' r1 f xdg_surface_ack_configure' || blind=1
canary r1-damage c.c 'void f(void) { wl_surface_damage_buffer(s, 0, 0, 1, 1); wl_surface_damage(s, 0, 0, 1, 1); }' ' r1 f wl_surface_damage_buffer' || blind=1
canary r1-queue c.c 'void f(void) { wl_proxy_set_queue(p, q); wl_display_dispatch_queue_pending(d, q); }' ' r1 f wl_display_dispatch_queue_pending' || blind=1
canary r1-read c.c 'void f(void) { if (wl_display_prepare_read(d) == 0) wl_display_read_events(d); }' ' r1 f wl_display_read_events' || blind=1
canary r1-geometry c.c 'void f(void) { xdg_surface_set_window_geometry(x, 0, 0, 1, 1); }' ' r1 f xdg_surface_set_window_geometry' || blind=1
# rule 2, also in the owner's own file
canary r2-marshal c.c 'void f(void) { wl_proxy_marshal(p, 1); }' ' r2 f wl_proxy_marshal' || blind=1
canary r2-flags we_wlframe.c 'void f(void) { wl_proxy_marshal_flags(p, 1, 0, 1, 0); }' ' r2 f wl_proxy_marshal_flags' || blind=1
# rule 3
canary r3-assign c.c 'void f(void) { WpeWl.width = 3; }' ' r3 f WpeWl.write' || blind=1
canary r3-split c.c 'void f(void) { WpeWl
   .pixels = 0; }' ' r3 f WpeWl.write' || blind=1
canary r3-incr c.c 'void f(void) { WpeWl.stride++; WpeWl.shm_size += 4; }' ' r3 f WpeWl.write' || blind=1
canary r3-address c.c 'void f(int **p) { *p = &WpeWl.height; }' ' r3 f &WpeWl.field' || blind=1
canary r3-alloc c.c 'void f(void) { wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, 1); }' ' r3 f wl_shm_pool_create_buffer' || blind=1
canary r3-alias c.c 'void f(void) { WpeWlInfo *w = &WpeWl; w->width = 1; }' ' r3 f WpeWlInfo\*' || blind=1
# rule 6
canary r6-listener we_wayland.c 'static void xdg_surface_configure(void *d, struct xdg_surface *x, unsigned s) { xdg_surface_ack_configure(x, s); }
static void xdg_toplevel_configure(void *d, void *t, int w, int h, void *st) { wpe_wlframe_note_toplevel(w, h, st); }' ' r6 xdg_surface_configure xdg_surface_ack_configure' || blind=1
canary r6-other we_wayland.c 'static void xdg_surface_configure(void *d, struct xdg_surface *x, unsigned s) { wpe_wlframe_note_configure(s); }
static void xdg_toplevel_configure(void *d, void *t, int w, int h, void *st) { helper(w, h); wpe_wlframe_note_toplevel(w, h, st); }' ' r6 xdg_toplevel_configure helper' || blind=1
canary r6-blind we_wayland.c 'static void other(void) { }' ' r6 - listener-xdg_surface_configure-not-found' || blind=1
# rule 4: a call outside the connection functions, on one line and split, in
# another function of we_wayland.c, and in any other file
canary r4-flush we_wayland.c 'static void other(void) { wl_display_flush(d); }' ' r4 other wl_display_flush' || blind=1
canary r4-split we_wayland.c 'static void other(void) { wl_display_roundtrip
   (d); }' ' r4 other wl_display_roundtrip' || blind=1
canary r4-dispatch c.c 'void f(void) { wl_display_dispatch(d); wl_display_dispatch_pending(d); }' ' r4 f wl_display_dispatch_pending' || blind=1
canary r4-pointer we_wayland.c 'static int (*g)(struct wl_display *) = wl_display_flush; static void wl_conn_flush(void) { }' ' r4 - wl_display_flush' || blind=1
canary r4-other-file we_wlframe.c 'void wl_conn_flush(void) { wl_display_flush(d); }' ' r4 wl_conn_flush wl_display_flush' || blind=1
# rule 5: every use is named, the allow file counts the meant ones
canary r5-handler c.c 'void f(void) { XSetIOErrorHandler(h); }' ' r5 f XSetIOErrorHandler' || blind=1
canary r5-split c.c 'void f(void) { XSetIOErrorHandler
   (h); }' ' r5 f XSetIOErrorHandler' || blind=1
canary r5-lost c.c 'void f(void) { e_display_lost("x", "y"); }' ' r5 f e_display_lost' || blind=1
# rule 7
canary r7-paste c.c '#define COMMIT(x) wl_surface_##x(s)
void f(void) { }' ' r7 \(macro\) ##' || blind=1
# clean uses: nothing named
canary clean-conn we_wayland.c 'static void wl_conn_flush(void) { wl_display_flush(d); }
static void wl_conn_roundtrip(void) { wl_display_roundtrip(d); }
static void wl_conn_dispatch(int b) { wl_display_dispatch_pending(d); wl_display_dispatch(d); }
static void xdg_surface_configure(void *d, struct xdg_surface *x, unsigned s) { wpe_wlframe_note_configure(s); }
static void xdg_toplevel_configure(void *d, void *t, int w, int h, struct wl_array *st) { wpe_wlframe_note_toplevel(w, h, st); }' none || blind=1
got=$(planted clean-conn-name c.c 'void f(void) { wl_display_get_fd(d); wl_display_disconnect(d); e_display_lost_exit(); XSetIOErrorExitHandler(d, h, 0); }')
[ -z "$got" ] || { echo "canary clean-conn-name: a neighbouring name was taken for a connection call or a handler: $got"; blind=1; }
canary clean-listener we_wayland.c 'static void xdg_surface_configure(void *d, struct xdg_surface *x, unsigned s) { (void)d; (void)x; wpe_wlframe_note_configure(s); }
static void xdg_toplevel_configure(void *d, void *t, int w, int h, struct wl_array *st) { (void)d; (void)t; wpe_wlframe_note_toplevel(w, h, st); }' none || blind=1
canary clean-owner we_wlframe.c 'void f(void) { wl_surface_attach(s, b, 0, 0); wl_surface_damage_buffer(s, 0, 0, 1, 1); wl_surface_commit(s); xdg_surface_ack_configure(x, 1); WpeWl.width = 3; WpeWl.pixels = p; wl_proxy_set_queue(p, q); }' none || blind=1
canary clean-comment c.c 'void f(void) { /* wl_surface_commit(s); WpeWl.width = 3; */ }' none || blind=1
canary clean-string c.c 'void f(void) { trace("wl_surface_commit WpeWl.width = 3"); }' none || blind=1
canary clean-compare c.c 'int f(void) { return WpeWl.width == 3 && WpeWl.pixels != 0; }' none || blind=1
canary clean-read c.c 'void f(void) { int w = WpeWl.width; w += WpeWl.height; }' none || blind=1
canary clean-name c.c 'void f(void) { my_wl_surface_commit(s); wl_surface_commit_extra(s); wl_surface_destroy(s); }' none || blind=1
canary clean-typedef c.c 'typedef struct WpeWlInfo { int width; } WpeWlInfo; extern WpeWlInfo WpeWl;' none || blind=1
canary clean-memset c.c 'void f(void) { memset(&WpeWl, 0, sizeof WpeWl); }' none || blind=1
# the counted exception: found by the scan, silent with its allow line, named
# without it, and named when the file allows fewer or more than it finds
mkdir -p "$WORK/plant"
printf '%s\n' 'static void wl_cursor_apply(void) { wl_surface_attach(s, b, 0, 0); wl_surface_damage(s, 0, 0, 1, 1); wl_surface_commit(s); }' > "$WORK/plant/we_wayland.c"
found=$(scan "$WORK/plant/we_wayland.c")
printf '%s\n' 'we_wayland.c|r1:wl_cursor_apply|3|keep|planted' > "$WORK/allow3"
printf '%s\n' 'we_wayland.c|r1:wl_cursor_apply|2|keep|planted' > "$WORK/allow2"
printf '%s\n' 'we_wayland.c|r1:wl_cursor_apply|4|keep|planted' > "$WORK/allow4"
printf '%s\n' 'we_other.c|r1:wl_cursor_apply|3|keep|planted' > "$WORK/allowx"
printf '%s\n' '# nothing' > "$WORK/allow0"
# the planted file lacks the listeners: the scan names that too, so judge only
# the identifier lines
only_r1=$(printf '%s\n' "$found" | grep ' r1 ')
[ -n "$only_r1" ] && [ -z "$(judge "$WORK/allow3" "$only_r1")" ] || { echo "canary allow: a listed identifier was named or not found"; blind=1; }
[ -n "$(judge "$WORK/allow0" "$only_r1")" ] || { echo "canary allow: an identifier with no allow line was not named"; blind=1; }
[ -n "$(judge "$WORK/allow2" "$only_r1")" ] || { echo "canary allow: a count below the identifiers was not named"; blind=1; }
[ -n "$(judge "$WORK/allow4" "$only_r1")" ] || { echo "canary allow: a count above the identifiers was not named"; blind=1; }
[ -n "$(judge "$WORK/allowx" "$only_r1")" ] || { echo "canary allow: an identifier in another file was not named"; blind=1; }
[ -n "$(judge "$WORK/allow3" "")" ] || { echo "canary allow: a stale line was not named"; blind=1; }
# rule 5's counted places: one handler install is meant, a second is named
printf '%s\n' 'WeXterm.c|r5:WpeXInit|1|keep|planted' > "$WORK/allow5"
printf '%s\n' 'WeXterm.c:10: r5 WpeXInit XSetIOErrorHandler' > "$WORK/one5"
printf '%s\n' 'WeXterm.c:10: r5 WpeXInit XSetIOErrorHandler' 'WeXterm.c:20: r5 WpeXInit XSetIOErrorHandler' > "$WORK/two5"
printf '%s\n' 'other.c:10: r5 other XSetIOErrorHandler' > "$WORK/else5"
[ -z "$(judge "$WORK/allow5" "$(cat "$WORK/one5")")" ] || { echo "canary rule 5: the meant handler was named"; blind=1; }
[ -n "$(judge "$WORK/allow5" "$(cat "$WORK/two5")")" ] || { echo "canary rule 5: a second handler was not named"; blind=1; }
[ -n "$(judge "$WORK/allow5" "$(cat "$WORK/else5")")" ] || { echo "canary rule 5: a handler in another function was not named"; blind=1; }
if [ $blind -ne 0 ]; then
  echo "check-wayland-frame: the scanner is blind"
  exit $BAD
fi
if [ $SELFTEST -eq 1 ]; then
  echo "check-wayland-frame: canaries ok"
  exit 0
fi

files=$(ls "$SRCDIR"/*.c "$SRCDIR"/*.h 2>/dev/null | grep -v -e '-protocol\.[ch]$')
if [ -z "$files" ]; then
  echo "check-wayland-frame: no sources under $SRCDIR" >&2
  exit $BAD
fi
# shellcheck disable=SC2086
found=$(scan $files)
left=$(judge "$ALLOW" "$found")
if [ -n "$left" ]; then
  echo "check-wayland-frame: a rule is broken (surface, connection or lost display), or an allow line is off:"
  echo "$left"
  exit 1
fi
echo "check-wayland-frame: clean"
exit 0
