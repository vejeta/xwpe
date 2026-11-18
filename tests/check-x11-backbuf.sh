#!/bin/sh
# check-x11-backbuf.sh -- the X11 back buffer has one owner.
#
# The window is only ever given pixels by the owner of the back buffer
# (we_xbackbuf.c) or by a renderer painting into the owner's target.  That
# holds only while one file creates, frees and copies the buffer: a second
# place that copies from "the buffer" is how an editor that found no Xft font
# came to copy from pixmap 0 and end with BadDrawable at start-up, and how a
# build without Xft and Cairo stopped compiling.
#
#   check-x11-backbuf.sh [--allow FILE] [--srcdir DIR] [--selftest]
#
# Rules:
#  1. XCopyArea, XCreatePixmap and XFreePixmap occur only in we_xbackbuf.c
#     (and its header): in code, in a macro, or as a function pointer.
#  2. The buffer's handle is not a member of anything: no `.backbuf` or
#     `->backbuf`, and the word `backbuf` does not occur in WeXterm.h.  The
#     compiler then refuses a bypass in every configuration.
#  3. Expose is handled in one function, e_x_answer_expose of we_xterm.c: the
#     words Expose, XExposeEvent and xexpose occur only there, except that the
#     two dispatch sites (e_x_serve_display and e_x_getch) may name Expose to
#     call e_x_answer_expose and nothing else.  Whole words only, so
#     GraphicsExpose, NoExpose and ExposureMask are not this rule's.
#  4. Token pasting that builds an X name or an Expose name (a macro that
#     would hide a banned identifier from rules 1 to 3).  A name pasted in a
#     way this scan cannot read is caught by the symbol rule of
#     tests/check-one-owner.sh, which names who may import XCopyArea,
#     XCreatePixmap and XFreePixmap from the objects; both are needed.
#
# Comments and the contents of string and character literals are blanked, so
# a name in prose is not a use; the headers are scanned as well as the .c
# files.  What is found is counted per function.  The counted exceptions are
# lines of the allow file ("file|rN:function|count|keep|reason"); the count may
# only fall, and a line that matches nothing is an error.
#
# Canaries (--selftest, and always at the start of a run): an offender per
# rule, a clean use of each, and the allow file's own arithmetic; a scanner
# that does not name every offender, or names a clean one, fails the script as
# blind.
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
    *) echo "check-x11-backbuf: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done

[ -n "$ALLOW" ] || ALLOW=$SRCDIR/tests/x11-backbuf.allow
if [ ! -f "$ALLOW" ]; then
  echo "check-x11-backbuf: no allow file at $ALLOW" >&2
  exit $BAD
fi

. "$(dirname "$0")/workdir.sh"
work_dir x11-backbuf $BAD

# The scan reads C as bytes (see check-wayland-frame.sh).
LC_ALL=C
export LC_ALL
BLANK_C=$(cat "$(dirname "$0")/blank-c.awk")

# scan FILE... - "file:line: rN function identifier" for each offender.
#
# Each file is read once, character by character on its blanked lines, with
# the brace depth and the name of the function the depth belongs to, so every
# identifier is reported with the function it stands in ("-" at file level).
scan() {
  awk "$BLANK_C"'
    function banned_x(id) { return (id == "XCopyArea" || id == "XCreatePixmap" || id == "XFreePixmap") }
    function banned_expose(id) { return (id == "Expose" || id == "XExposeEvent" || id == "xexpose") }
    # funcname(head) - the first "identifier (" of the text before a brace.
    function funcname(head) {
      if (match(head, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) {
        head = substr(head, RSTART, RLENGTH)
        sub(/[ \t]*\($/, "", head)
        return head
      }
      return ""
    }
    function say(n, rule, fnname, tok) {
      if (fnname == "") fnname = "-"
      print file ":" n ": " rule " " fnname " " tok
    }
    # follow(n, col) - the blanked text from column col of line n and the two
    # lines after it, in one string.
    function follow(n, col) {
      return substr(code[n], col) " " code[n + 1] " " code[n + 2]
    }
    # judge_token(n, col, id, fnname) - one identifier of code.  col is the
    # column just past it.
    function judge_token(n, col, id, fnname,   rest) {
      if (banned_x(id) && !owner) say(n, "r1", fnname, id)
      if (id == "backbuf") {
        if (base == "WeXterm.h") say(n, "r2", fnname, "backbuf")
        else if (prev2 == "->" || substr(prev2, 2, 1) == ".") say(n, "r2", fnname, "member-backbuf")
      }
      if (!banned_expose(id)) return
      if (base == "we_xterm.c" && fnname == "e_x_answer_expose") return
      if (id == "Expose" && base == "we_xterm.c" && (fnname == "e_x_serve_display" || fnname == "e_x_getch")) {
        rest = follow(n, col)
        if (rest ~ /^[ \t]*[:)][ \t]*e_x_answer_expose[ \t]*\(/) return
        say(n, "r3", fnname, "Expose-dispatch-does-more")
        return
      }
      say(n, "r3", fnname, id)
    }
    # macro_line(n) - a preprocessor line: names in it are reported as
    # (macro) uses, and token pasting that would build one is rule 4.
    function macro_line(n,   line, rest, id) {
      line = code[n]
      if (line ~ /(^|[^A-Za-z0-9_])X[ \t]*##/ || line ~ /##[ \t]*[A-Za-z0-9_]*(CopyArea|CreatePixmap|FreePixmap|Expose)/)
        print file ":" n ": r4 (macro) ##"
      rest = line
      while (match(rest, /[A-Za-z_][A-Za-z0-9_]*/)) {
        id = substr(rest, RSTART, RLENGTH)
        if (banned_x(id) && !owner) print file ":" n ": r1 (macro) " id
        if (banned_expose(id)) print file ":" n ": r3 (macro) " id
        rest = substr(rest, RSTART + RLENGTH)
      }
    }
    # code_line(n) - one blanked line of code: identifiers, braces, and the
    # two characters before each token (to tell a member access).
    function code_line(n,   line, len, i, j, c, id) {
      line = code[n]; len = length(line); i = 1
      while (i <= len) {
        c = substr(line, i, 1)
        if (c ~ /[A-Za-z_]/) {
          j = i
          while (j <= len && substr(line, j, 1) ~ /[A-Za-z0-9_]/) j++
          id = substr(line, i, j - i)
          judge_token(n, j, id, (depth > 0) ? curfn : "-")
          head = head id
          prev2 = "  "
          i = j
          continue
        }
        if (c == "{") {
          if (depth == 0) curfn = funcname(head)
          depth++
        } else if (c == "}") {
          depth--
          if (depth == 0) head = ""
        } else if (c == ";" && depth == 0) head = ""
        else if (depth == 0) head = head c
        if (c !~ /[ \t]/) prev2 = substr(prev2, 2, 1) c
        i++
      }
      if (depth == 0) head = head " "
    }
    function finish(   n) {
      depth = 0; head = ""; curfn = ""; prev2 = "  "
      for (n = 1; n <= nl; n++) {
        if (macro[n]) macro_line(n)
        else code_line(n)
      }
    }
    FNR == 1 {
      if (file != "") finish()
      file = FILENAME; base = file; sub(/^.*\//, "", base)
      owner = (base == "we_xbackbuf.c" || base == "we_xbackbuf.h")
      delete code; delete macro; nl = 0; blank_incom = 0; inmacro = 0
    }
    {
      line = blank_c($0, 0)
      nl++
      code[nl] = line
      macro[nl] = (inmacro || line ~ /^[ \t]*#/) ? 1 : 0
      if (macro[nl]) inmacro = ($0 ~ /\\[ \t]*$/)
    }
    END { if (file != "") finish() }' "$@"
}

# judge ALLOWFILE FOUND - what is left of the scan's FOUND lines once the
# counted exceptions of ALLOWFILE are taken off.  One problem a line.
judge() {
  printf '%s\n' "$2" | awk -v allow="$1" '
    function base(s) { sub(/:[0-9]+: .*$/, "", s); sub(/^.*\//, "", s); return s }
    function rulefunc(s,   t, p) { t = s; sub(/^[^ ]*: /, "", t); split(t, p, " "); return p[1] ":" p[2] }
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
      if (!(key in al)) print "outside the back buffer owner: " $0
    }
    END {
      for (k in seen) if (k in al) {
        if (seen[k] > al[k] + 0) print "more than allowed: " k " has " seen[k] ", the allow file says " al[k]
        else if (seen[k] < al[k] + 0) print "fewer than the allow file says: " k " has " seen[k] ", lower the line"
      }
      for (k in al) if (!(k in seen)) print "stale allow line, delete it: " k
    }' | sort
}

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
# rule 1: each name, on one line and split, as a pointer, in a macro, in a header
canary r1-copy c.c 'void f(void) { XCopyArea(d, a, b, g, 0, 0, 1, 1, 0, 0); }' ' r1 f XCopyArea' || blind=1
canary r1-create c.c 'void f(void) { p = XCreatePixmap(d, w, 1, 1, 24); }' ' r1 f XCreatePixmap' || blind=1
canary r1-free c.c 'void f(void) { XFreePixmap(d, p); }' ' r1 f XFreePixmap' || blind=1
canary r1-split c.c 'void f(void) { XCopyArea
   (d, a, b, g, 0, 0, 1, 1, 0, 0); }' ' r1 f XCopyArea' || blind=1
canary r1-pointer c.c 'int (*g)(void) = XCopyArea; void f(void) { }' ' r1 - XCopyArea' || blind=1
canary r1-macro c.c '#define BLIT(a) XCopyArea(a)
void f(void) { }' ' r1 \(macro\) XCopyArea' || blind=1
canary r1-header h.h 'static inline void f(void) { XFreePixmap(d, p); }' ' r1 f XFreePixmap' || blind=1
# rule 2: a member access (also split over two lines), the word in WeXterm.h
canary r2-arrow c.c 'void f(void) { use(WpeXInfo->backbuf); }' ' r2 f member-backbuf' || blind=1
canary r2-dot c.c 'void f(void) { use(WpeXInfo.backbuf); }' ' r2 f member-backbuf' || blind=1
canary r2-spaced c.c 'void f(void) { use(WpeXInfo .
   backbuf); }' ' r2 f member-backbuf' || blind=1
canary r2-member WeXterm.h 'typedef struct { Pixmap backbuf; } S;' ' r2 - backbuf' || blind=1
# rule 3: handling Expose elsewhere, a second handler, a dispatch that does more
canary r3-case we_xterm.c 'int e_x_getch(void) { switch (t) { case Expose: do_it(); break; } return 0; }' ' r3 e_x_getch Expose-dispatch-does-more' || blind=1
canary r3-other we_xterm.c 'void other(void) { if (r.type == Expose) x(); }' ' r3 other Expose' || blind=1
canary r3-event we_xterm.c 'void other(XEvent *e) { XExposeEvent *x = (XExposeEvent *)e; use(e->xexpose.x); }' ' r3 other XExposeEvent' || blind=1
canary r3-other-file we_render_cairo.c 'void f(XEvent *e) { if (e->type == Expose) g(); }' ' r3 f Expose' || blind=1
canary r3-macro c.c '#define IS_EXPOSE(e) ((e)->type == Expose)
void f(void) { }' ' r3 \(macro\) Expose' || blind=1
# rule 4
canary r4-paste c.c '#define OP(n) X##n(d)
void f(void) { }' ' r4 \(macro\) ##' || blind=1
canary r4-paste-rhs c.c '#define OP(n) n##CopyArea(d)
void f(void) { }' ' r4 \(macro\) ##' || blind=1
# clean uses: nothing named
canary clean-owner we_xbackbuf.c 'void f(void) { XCreatePixmap(d, w, 1, 1, 24); XCopyArea(d, a, b, g, 0, 0, 1, 1, 0, 0); XFreePixmap(d, p); }' none || blind=1
canary clean-owner-header we_xbackbuf.h 'static inline void f(void) { XCopyArea(d, a, b, g, 0, 0, 1, 1, 0, 0); }' none || blind=1
canary clean-comment c.c 'void f(void) { /* XCopyArea(d); WpeXInfo.backbuf; Expose */ }' none || blind=1
canary clean-line-comment c.c 'void f(void) { // XFreePixmap(d, p);
}' none || blind=1
canary clean-string c.c 'void f(void) { trace("XCreatePixmap WpeXInfo.backbuf Expose"); }' none || blind=1
canary clean-handler we_xterm.c 'static void e_x_answer_expose(XEvent *r) { XExposeEvent *e = (XExposeEvent *)r; use(e->x); do { use(r->xexpose.x); } while (more(Expose, r)); }' none || blind=1
canary clean-dispatch we_xterm.c 'int e_x_getch(void) { switch (t) { case Expose:
     e_x_answer_expose(&report);
     break; } return 0; }
static void e_x_serve_display(void) { if (report.type == Expose)
   e_x_answer_expose(&report); }' none || blind=1
canary clean-names c.c 'void f(void) { use(GraphicsExpose); use(NoExpose); use(ExposureMask); my_XCopyArea(a); XCopyAreaX(b); e_x_backbuf_present(0, 0, 1, 1); use(x_backbuf); }' none || blind=1
canary clean-header-name WeXterm.h 'void e_x_backbuf_target(void); int has_backbuffer;' none || blind=1
canary clean-block c.c 'struct s { int a; };
void f(void) { if (x) { g(); } }
static void h(void) { }' none || blind=1
# the counted exception: found by the scan, silent with its allow line, named
# without it, and named when the file allows fewer or more than it finds
mkdir -p "$WORK/plant"
printf '%s\n' 'static void later(void) { XCopyArea(d, a, b, g, 0, 0, 1, 1, 0, 0); XFreePixmap(d, p); }' > "$WORK/plant/we_other.c"
found=$(scan "$WORK/plant/we_other.c")
printf '%s\n' 'we_other.c|r1:later|2|keep|planted' > "$WORK/allow2"
printf '%s\n' 'we_other.c|r1:later|1|keep|planted' > "$WORK/allow1"
printf '%s\n' 'we_other.c|r1:later|3|keep|planted' > "$WORK/allow3"
printf '%s\n' 'we_else.c|r1:later|2|keep|planted' > "$WORK/allowx"
printf '%s\n' '# nothing' > "$WORK/allow0"
[ -n "$found" ] && [ -z "$(judge "$WORK/allow2" "$found")" ] || { echo "canary allow: a listed use was named or not found"; blind=1; }
[ -n "$(judge "$WORK/allow0" "$found")" ] || { echo "canary allow: a use with no allow line was not named"; blind=1; }
[ -n "$(judge "$WORK/allow1" "$found")" ] || { echo "canary allow: a count below the uses was not named"; blind=1; }
[ -n "$(judge "$WORK/allow3" "$found")" ] || { echo "canary allow: a count above the uses was not named"; blind=1; }
[ -n "$(judge "$WORK/allowx" "$found")" ] || { echo "canary allow: a use in another file was not named"; blind=1; }
[ -n "$(judge "$WORK/allow2" "")" ] || { echo "canary allow: a stale line was not named"; blind=1; }
if [ $blind -ne 0 ]; then
  echo "check-x11-backbuf: the scanner is blind"
  exit $BAD
fi
if [ $SELFTEST -eq 1 ]; then
  echo "check-x11-backbuf: canaries ok"
  exit 0
fi

files=$(ls "$SRCDIR"/*.c "$SRCDIR"/*.h 2>/dev/null | grep -v -e '-protocol\.[ch]$')
if [ -z "$files" ]; then
  echo "check-x11-backbuf: no sources under $SRCDIR" >&2
  exit $BAD
fi
# shellcheck disable=SC2086
found=$(scan $files)
left=$(judge "$ALLOW" "$found")
if [ -n "$left" ]; then
  echo "check-x11-backbuf: a rule is broken (the X11 back buffer has one owner, we_xbackbuf.c), or an allow line is off:"
  echo "$left"
  exit 1
fi
echo "check-x11-backbuf: clean"
exit 0
