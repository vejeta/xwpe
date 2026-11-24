#!/bin/sh
# check-x11-lost-owner.sh -- after the X server is gone, one owner keeps the
# editor off the wire.
#
# Xlib runs the editor's I/O error handler inside whichever call of the
# editor's flushed or waited for a reply, and that call then goes on.  A
# libX11 that keeps sending after the handler returned (the one the CI runner
# has) ends the editor with SIGSEGV in libXrender, and the user loses the .ESV
# save.  So every call that can put a request on the wire, or wait for a
# reply, asks the owner of the connection first (e_x_conn_usable, in
# WeXterm.c beside the handler), and the long loops and the stages of a buffer
# remake ask the silent form (e_x_conn_step_usable) once a step.
#
# This script fails the build when a call that can reach the X server is added
# outside that guard: a macro or a wrapper cannot hide it, because the source
# is scanned and the objects' undefined symbols are checked against the scan.
#
#   check-x11-lost-owner.sh [--strict] [--list] [--allow FILE] [--srcdir DIR]
#                           [--nm NM] [--selftest] [OBJECT...]
#
# Two passes:
#
#  1. The source scan (the pass that matters).  Comments and the contents of
#     strings are blanked (tests/blank-c.awk), then the four X11 files
#     (we_xterm.c, WeXterm.c, we_xbackbuf.c, we_render_cairo.c) are walked
#     function by function, macro by macro.  A function whose body holds a
#     call to a wire name -- X[A-Z]*, Xft*, XRender*, cairo_*, pango_cairo_*,
#     or a call through the renderer table, WpeRender.draw_rect and its
#     fellows, which on X11 draws on the window surface and is judged at the
#     caller rather than where the address was taken -- must be one of:
#
#       owner    it may touch a connection that may be gone, because it is the
#                place that finds out, or the handler itself: the handler
#                functions, the owner's own two predicates and the two waits.
#                An owner may hold only the wire names listed for it below,
#                and only the number of calls of each that the exemption
#                counts; a new name is an offence, so a call added to
#                e_x_getch cannot hide in the exemption.  Each exempt call
#                must also come after the test that ends the wait -- the read
#                of WpeXInfo.lost, or the first call of a helper that asks the
#                silent predicate -- because a call before that test runs on a
#                connection nobody has asked about yet.
#       guarded  its first `if` is `if (!e_x_conn_usable("...")) return ...;`
#                and nothing that can reach the server comes before it.  A
#                bare call whose result is ignored guards nothing and does not
#                count; a `return` that is the start of another name
#                (return_seen = 1) returns nothing and is no guard; and a call
#                of a helper that reaches the server sends just as much as a
#                call of Xlib itself, so it may not come before the guard.
#       listed   a line of the allow file, `file|function|count|state|reason`,
#                whose state says why the function needs no guard of its own:
#
#                  startup  it runs before a display can be lost (WpeXInit and
#                           what it calls), and after a loss the editor exits
#                           at its next wait
#                  local    it reaches no server: client-side Xlib, or a
#                           library that has no connection to talk on.  The
#                           reason is required and says which.
#                  dead     nothing calls it in any build
#                  below    every reference to it in the four files lies in a
#                           function that is an owner, guarded, itself below,
#                           or holds no wire call of its own and is reached
#                           only from such functions.  This is the state the
#                           script verifies rather than trusts: the helpers
#                           behind a guarded owner (the buffer's pixmap calls,
#                           the glyph and scrollbar primitives) are covered by
#                           their callers, and a trace line a glyph would be
#                           noise.  A function registered in a table at file
#                           level names its dispatcher in the reason as
#                           "via NAME", and NAME is verified the same way.
#
#     The count is the number of wire calls in the function and may only fall;
#     a line that matches nothing any more is an error, so the file shrinks as
#     callers move onto the guard and the last pass deletes it.
#
#     The canaries below are run at the start of every pass, not only by
#     --selftest: a scanner that has gone blind fails the build instead of
#     passing everything it looks at.
#
#  2. The symbol scan.  `nm -u` of each of the four objects: every undefined
#     X*, Xft*, XRender*, cairo_* or pango_cairo_* symbol must have a call of
#     that name in the matching source, so a call built by a macro or hidden
#     behind a wrapper is still a call the source scan has seen.
#
# Names that never reach a server are local by name and need no line:
# everything of fontconfig (Fc*, which talks to files, not to X), the
# client-side cairo families (cairo_region_*, cairo_image_surface_*,
# cairo_font_options_*, the cairo_set_* state setters) and the client-side
# Xlib calls listed in the scanner.  Without this the allow file would start
# with dozens of lines that say nothing.
#
# Flags: --strict ignores the allow file except its `local` and `below` lines,
# which are structural rather than work still to do.  --list prints what was
# found as allow lines, to start a file from.  --selftest runs the canaries
# only.  Environment (set by `make check`): NM, srcdir.
#
# Exit: 0 clean, 1 offenders or a blind scanner, 77 no usable nm or no X here.

STRICT=0
LIST=0
SELFTEST=0
ALLOW=
SRCDIR=${srcdir:-.}
NMTOOL=${NM:-nm}
OBJECTS=
HERE=$(dirname "$0")
BAD=1
SKIP=77

while [ $# -gt 0 ]; do
  case $1 in
    --strict) STRICT=1 ;;
    --list) LIST=1 ;;
    --selftest) SELFTEST=1 ;;
    --allow) shift; ALLOW=$1 ;;
    --srcdir) shift; SRCDIR=$1 ;;
    --nm) shift; NMTOOL=$1 ;;
    *) OBJECTS="$OBJECTS $1" ;;
  esac
  shift
done

[ -n "$ALLOW" ] || ALLOW=$HERE/x11-lost-owner.allow

. "$HERE/workdir.sh"
work_dir x11-lost-owner $BAD

# The scan reads C as bytes (see check-wayland-frame.sh).
LC_ALL=C
export LC_ALL
BLANK_C=$(cat "$HERE/blank-c.awk")

# The files that hold the X11 backend, and the header of the glyph shapes it
# shares with the Wayland renderer (a static function of a header is compiled
# into the object that includes it, so the symbol scan of that object finds
# its calls).
X11_SOURCES="we_xterm.c WeXterm.c we_xbackbuf.c we_render_cairo.c we_render_glyphs.h"

# judge ALLOWFILE SOURCE... - one problem a line, or nothing when clean.
# With LIST=1 it prints an allow line for every function that needs one.
judge() {
  allowfile=$1
  shift
  awk -v allowfile="$allowfile" -v strictmode="$STRICT" -v listmode="$LIST" \
      -v namedump="$WORK/srcnames" "$BLANK_C"'
    # wire_name(id) - can a call of this name reach the X server?  The
    # families that cannot are local by name (see the header).  A call through
    # the renderer table (WpeRender.draw_rect) reaches it just as much: on X11
    # that table holds the functions that draw on the window surface.
    function wire_name(id) {
      if (id ~ /^Fc/) return 0
      if (id ~ /^cairo_region_/) return 0
      if (id ~ /^cairo_image_surface_/) return 0
      if (id ~ /^cairo_font_options_/) return 0
      if (id ~ /^cairo_set_/) return 0
      if (id in local_x) return 0
      if (id ~ /^WpeRender\./) return 1
      if (id ~ /^X[A-Z]/) return 1
      if (id ~ /^Xft/) return 1
      if (id ~ /^XRender/) return 1
      if (id ~ /^cairo_/) return 1
      if (id ~ /^pango_cairo_/) return 1
      return 0
    }
    function is_owner(fn) { return fn in owner_wire }
    # owner_name(spec), owner_want(spec) - the two halves of one exemption,
    # "NAME:count": the wire name an owner may hold, and how many calls of it.
    function owner_name(spec,   p) {
      p = index(spec, ":")
      return p ? substr(spec, 1, p - 1) : spec
    }
    function owner_want(spec,   p) {
      p = index(spec, ":")
      return p ? substr(spec, p + 1) + 0 : 0
    }
    # owner_extra(fn, id) - a wire name the exemption of this owner does not
    # name.
    function owner_extra(fn, id,   i, n, a) {
      n = split(owner_wire[fn], a, " ")
      for (i = 1; i <= n; i++) if (owner_name(a[i]) == id) return ""
      return id
    }
    # funcname(head) - the first "identifier (" of the text before a brace,
    # skipping the words that are a type and not the name of the function
    # (a function returning a pointer to a function reads "char (*name(...))"
    # and its name is not "char").
    function funcname(head,   rest, id) {
      rest = head
      while (match(rest, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) {
        id = substr(rest, RSTART, RLENGTH)
        sub(/[ \t]*\($/, "", id)
        if (!(id in c_keyword)) return id
        rest = substr(rest, RSTART + RLENGTH)
      }
      return ""
    }
    # dataname(head) - the identifier a file-level initializer assigns to.
    function dataname(head,   h) {
      h = head
      sub(/=[^=]*$/, "", h)
      if (match(h, /[A-Za-z_][A-Za-z0-9_]*[ \t]*$/))
        return substr(h, RSTART, RLENGTH)
      return ""
    }
    # paren_ahead(fi, n, col) - is the next thing after column col an open
    # paren, so the identifier that just ended is a call and not a type or a
    # member?  The call may start on a line after the name.
    function paren_ahead(fi, n, col,   rest) {
      rest = substr(fcode[fi, n], col) " " fcode[fi, n + 1] " " fcode[fi, n + 2]
      return rest ~ /^[ \t]*\(/
    }
    # rest_of(fi, n, col) - the blanked text from column col of line n and the
    # two lines after it, as one line with single spaces.
    function rest_of(fi, n, col,   s) {
      s = substr(fcode[fi, n], col) " " fcode[fi, n + 1] " " fcode[fi, n + 2]
      gsub(/[ \t]+/, " ", s)
      return s
    }
    # call_at(fi, n, j, id) - the name this identifier calls, or "" when it
    # calls nothing.  A call through the renderer table reads
    # "WpeRender.draw_rect(...)" and is judged at the caller, not where the
    # address was taken: what it reaches draws on the window surface, so it is
    # a wire call of the function that makes it.
    function call_at(fi, n, j, id,   rest, m) {
      if (paren_ahead(fi, n, j)) return id
      if (id != "WpeRender") return ""
      rest = rest_of(fi, n, j)
      if (!match(rest, /^\.[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) return ""
      m = substr(rest, RSTART, RLENGTH)
      gsub(/^[. \t]+/, "", m)
      gsub(/[ \t]*\($/, "", m)
      return "WpeRender." m
    }
    # pos_after(a, b) - is the position a (line:column) after the position b?
    function pos_after(a, b,   pa, pb) {
      split(a, pa, ":")
      split(b, pb, ":")
      if (pa[1] + 0 != pb[1] + 0) return pa[1] + 0 > pb[1] + 0
      return pa[2] + 0 > pb[2] + 0
    }
    # note_call(id) - a call of this name was seen in this file, whether or
    # not it can reach the server: the symbol scan of the objects asks every
    # X, Xft, cairo or pango symbol to be a call the source scan has seen.
    function note_call(id) {
      if (id ~ /^(X[A-Z]|Xft|XRender|cairo_|pango_cairo_)/) srcname[base, id] = 1
    }
    # note_wire(fn, id) - one call of a wire name inside fn.  The count is
    # rebuilt in END, once every name defined in the four files is known: a
    # helper of the renderer itself that happens to start like a cairo call
    # (cairo_init_colors) is ours and reaches nothing by itself.
    function note_wire(fn, id) {
      wcntof[fn, id]++
      if (wfile[fn] == "") wfile[fn] = base
    }
    # note_call_at(fn, id, n, col) - one call inside fn, with the place it is
    # made.  The exempt calls of an owner are judged by their place (only
    # after the test that ends the wait is one exempt), and a call that comes
    # before a guard is what the guard was meant to stop.
    function note_call_at(fn, id, n, col) {
      allpos[fn] = allpos[fn] "\034" id "=" n ":" col
      if (wire_name(id)) wposof[fn] = wposof[fn] "\034" id "=" n ":" col
      if (id == "e_x_conn_step_usable") note_lost_test(fn, n, col)
    }
    # note_lost_test(fn, n, col) - fn asks here whether the connection is
    # gone: a read of the flag the owner holds, or a call of its silent
    # predicate.  The earliest one in the function is the one that counts.
    function note_lost_test(fn, n, col,   at) {
      lostread[fn] = 1
      at = n ":" col
      if (!(fn in lostat) || pos_after(lostat[fn], at)) lostat[fn] = at
    }
    # note_pre(fn, id) - fn calls id before its first if, where a guard would
    # have to be.
    function note_pre(fn, id) {
      if (fn in seenif) return
      precall[fn] = precall[fn] "\034" id
    }
    # note_ref(ctx, id) - ctx refers to id (a call or an address taken).  A
    # reference from a file-level initializer is one from the table it fills,
    # and the dispatcher of the table is named on the allow line of any of its
    # entries (a plain entry has no allow line of its own).
    function note_ref(ctx, id,   v) {
      if (id == ctx || ctx == "") return
      if (ctx ~ /^TABLE:/) {
        v = substr(ctx, 7)
        if (!(((v) SUBSEP id) in tabseen)) {
          tabseen[v, id] = 1
          tabentries[v] = tabentries[v] " " id
        }
      }
      if ((ctx SUBSEP id) in refseen) return
      refseen[ctx, id] = 1
      refby[id] = refby[id] "\034" ctx
      if (ctx !~ /^TABLE:/) calls[ctx] = calls[ctx] "\034" id
    }
    # macro_names(n, name, which) - the identifiers on a preprocessor line,
    # counted against the macro they belong to.
    function macro_names(fi, n, name, which,   rest, id) {
      rest = fcode[fi, n]
      while (match(rest, /[A-Za-z_][A-Za-z0-9_]*/)) {
        id = substr(rest, RSTART, RLENGTH)
        rest = substr(rest, RSTART + RLENGTH)
        if (which == 1) {
          if (rest ~ /^[ \t]*\(/) {
            note_call(id)
            if (wire_name(id)) note_wire(name, id)
          }
        } else if (id in defined && id != name) note_ref(name, id)
      }
    }
    # walk(fi, which) - one pass over the stored lines of file fi.  Pass 1
    # collects the definitions, the wire calls and the guards; pass 2 collects
    # the references to what pass 1 defined.
    function walk(fi, which,   n, len, i, j, c, id, ctx, callid, keep) {
      base = fbase[fi]
      depth = 0; head = ""; curfn = ""; indata = 0; datavar = ""
      macroname = ""; inmacro = 0
      for (n = 1; n <= fnl[fi]; n++) {
        if (fmacro[fi, n]) {
          if (which == 1 &&
              match(fraw[fi, n], /^[ \t]*#[ \t]*include[ \t]*"[^"]+"/)) {
            h = substr(fraw[fi, n], RSTART, RLENGTH)
            sub(/^[ \t]*#[ \t]*include[ \t]*"/, "", h)
            sub(/"$/, "", h)
            sub(/^.*\//, "", h)
            inchdr[base] = inchdr[base] " " h
          }
          if (inmacro == 0) {
            if (match(fcode[fi, n], /^[ \t]*#[ \t]*define[ \t]+[A-Za-z_][A-Za-z0-9_]*/)) {
              id = substr(fcode[fi, n], RSTART, RLENGTH)
              sub(/^[ \t]*#[ \t]*define[ \t]+/, "", id)
              macroname = id
              if (which == 1) defined[id] = base
            } else macroname = "(macro)"
          }
          macro_names(fi, n, macroname, which)
          inmacro = (fraw[fi, n] ~ /\\[ \t]*$/) ? 1 : 0
          if (inmacro == 0) macroname = ""
          continue
        }
        len = length(fcode[fi, n]); i = 1
        while (i <= len) {
          c = substr(fcode[fi, n], i, 1)
          if (c ~ /[A-Za-z_]/) {
            j = i
            while (j <= len && substr(fcode[fi, n], j, 1) ~ /[A-Za-z0-9_]/) j++
            id = substr(fcode[fi, n], i, j - i)
            if (depth > 0) {
              if (which == 1) {
                callid = call_at(fi, n, j, id)
                if (callid != "") {
                  note_call(callid)
                  if (!indata && curfn != "") {
                    if (wire_name(callid)) note_wire(curfn, callid)
                    note_call_at(curfn, callid, n, j)
                    note_pre(curfn, callid)
                  }
                }
                if (!indata && curfn != "") {
                  if (id == "WpeXInfo" && rest_of(fi, n, j) ~ /^\.[ \t]*lost/)
                    note_lost_test(curfn, n, j)
                  if (!(curfn in seenif) && id == "if") {
                    seenif[curfn] = 1
                    firstif[curfn] = substr(rest_of(fi, n, j - 2), 1, 160)
                  }
                }
              } else {
                ctx = indata ? "TABLE:" datavar : curfn
                if (id in defined) note_ref(ctx, id)
              }
            } else if (which == 1) {
              callid = call_at(fi, n, j, id)
              if (callid != "") note_call(callid)
            }
            if (depth == 0) head = head id
            i = j
            continue
          }
          if (c == "{") {
            if (depth == 0) {
              if (head ~ /=/) { indata = 1; datavar = dataname(head) }
              else {
                indata = 0
                curfn = funcname(head)
                if (curfn != "" && which == 1) {
                  defined[curfn] = base
                  if (wfile[curfn] == "") wfile[curfn] = base
                }
              }
            }
            depth++
          } else if (c == "}") {
            depth--
            if (depth == 0) { head = ""; curfn = ""; indata = 0; datavar = "" }
          } else if (c == ";" && depth == 0) head = ""
          else if (depth == 0) head = head c
          i++
        }
        if (depth == 0) head = head " "
      }
    }
    # guard_shape(s) - does the first if of a function read as the guard of
    # the owner, and does it return?  A name that starts with the word is not
    # a return: "return_seen = 1" assigns, stays in the function, and guards
    # nothing.
    function guard_shape(s) {
      return s ~ /^if ?\( ?! ?e_x_conn_usable ?\([^)]*\) ?\) ?return([ \t]*;|[ \t]*\(|[ \t]+[^ \t;])/
    }
    # reaches_wire(fn) - does a call of fn, or of anything fn calls, put a
    # request on the wire?  Memoised; a cycle is taken as reaching, so a
    # recursive pair is not waved through.
    function reaches_wire(fn,   n, i, a) {
      if (fn in reachmemo) return reachmemo[fn]
      if (fn in reachbusy) return 1
      if (wire_name(fn) || (fn in wcnt)) { reachmemo[fn] = 1; return 1 }
      if (!(fn in calls)) { reachmemo[fn] = 0; return 0 }
      reachbusy[fn] = 1
      n = split(calls[fn], a, "\034")
      for (i = 1; i <= n; i++) {
        if (a[i] == "" || a[i] == fn) continue
        if (reaches_wire(a[i])) {
          delete reachbusy[fn]
          reachmemo[fn] = 1
          return 1
        }
      }
      delete reachbusy[fn]
      reachmemo[fn] = 0
      return 0
    }
    # pre_wire(fn) - the first call of fn that can reach the server and comes
    # before its first if, or "" when there is none.  A call of a helper below
    # a guard sends as much as a call of Xlib itself, so it counts here too.
    function pre_wire(fn,   n, i, a) {
      if (!(fn in precall)) return ""
      n = split(precall[fn], a, "\034")
      for (i = 1; i <= n; i++) {
        if (a[i] == "") continue
        if (wire_name(a[i]) || reaches_wire(a[i])) return a[i]
      }
      return ""
    }
    # asks_lost(fn) - does fn ask the owner of the connection whether it is
    # gone, or call something that does?  The key reader asks through a helper
    # of its own, so the test that ends its wait is found through its calls.
    function asks_lost(fn,   n, i, a) {
      if (fn in lostmemo) return lostmemo[fn]
      if (fn in lostbusy) { lostmemo[fn] = 0; return 0 }
      if (fn in lostread) { lostmemo[fn] = 1; return 1 }
      if (!(fn in calls)) { lostmemo[fn] = 0; return 0 }
      lostbusy[fn] = 1
      n = split(calls[fn], a, "\034")
      for (i = 1; i <= n; i++) {
        if (a[i] == "" || a[i] == fn) continue
        if (asks_lost(a[i])) {
          delete lostbusy[fn]
          lostmemo[fn] = 1
          return 1
        }
      }
      delete lostbusy[fn]
      lostmemo[fn] = 0
      return 0
    }
    # owner_lost_at(fn) - where an owner first asks whether the connection is
    # gone: its own read of the flag, or its first call of something that
    # asks.  "" when nothing in it asks.
    function owner_lost_at(fn,   n, i, a, p, at) {
      at = (fn in lostat) ? lostat[fn] : ""
      if (!(fn in allpos)) return at
      n = split(allpos[fn], a, "\034")
      for (i = 1; i <= n; i++) {
        if (a[i] == "") continue
        p = index(a[i], "=")
        if (!asks_lost(substr(a[i], 1, p - 1))) continue
        if (at == "" || pos_after(at, substr(a[i], p + 1)))
          at = substr(a[i], p + 1)
      }
      return at
    }
    # is_guarded(fn) - is the first if of fn the guard of the owner, with
    # nothing that can reach the server before it?
    function is_guarded(fn) {
      if (!(fn in firstif)) return 0
      if (!guard_shape(firstif[fn])) return 0
      if (pre_wire(fn) != "") return 0
      return 1
    }
    # safe(fn) - may this function run once the connection is gone?  A
    # function is safe when it is an owner, when it is guarded, when its allow
    # line says it is out of the way of a loss (startup, local, dead), or when
    # its line says below and every reference to it lies in a function that is
    # itself safe.  Anything else, a helper nobody declared included, is not
    # safe: a wire call below it has no guard above it that this check can
    # name.  Memoised; a cycle is taken as safe so a recursive pair ends.
    function safe(fn,   n, i, a, ctx, d, st, tvar) {
      if (fn in safememo) return safememo[fn]
      if (fn in busy) return 1
      if (is_owner(fn)) { safememo[fn] = 1; return 1 }
      if (fn in guardedf) { safememo[fn] = 1; return 1 }
      st = (fn in astate) ? astate[fn] : ""
      if (st == "startup" || st == "local" || st == "dead") { safememo[fn] = 1; return 1 }
      if (st != "below") { safememo[fn] = 0; return 0 }
      if (!(fn in refby)) {
        why[fn] = "is declared below, but nothing in the four files calls it"
        safememo[fn] = 0
        return 0
      }
      busy[fn] = 1
      n = split(refby[fn], a, "\034")
      for (i = 1; i <= n; i++) {
        ctx = a[i]
        if (ctx == "") continue
        if (ctx ~ /^TABLE:/) {
          tvar = substr(ctx, 7)
          d = (fn in avia) ? avia[fn] : ""
          if (d == "") d = (tvar in viavar) ? viavar[tvar] : ""
          if (d == "") {
            why[fn] = "is registered in the table " tvar \
                      ", and no allow line of that table names a dispatcher (via NAME)"
            delete busy[fn]; safememo[fn] = 0; return 0
          }
          if (!safe(d)) {
            why[fn] = "is registered in the table " tvar \
                      ", and the dispatcher it names, " d ", is not itself guarded"
            delete busy[fn]; safememo[fn] = 0; return 0
          }
          continue
        }
        if (!safe(ctx)) {
          if (why[fn] == "")
            why[fn] = "is reached from " ctx \
                      ", which is neither guarded nor below a guard"
          delete busy[fn]; safememo[fn] = 0; return 0
        }
      }
      delete busy[fn]
      safememo[fn] = 1
      return 1
    }
    # declared(fn) - is this function an owner, guarded, or the subject of an
    # allow line?  A plain helper that is not is what --list reports, so the
    # file can be written in one pass instead of one error at a time.
    function declared(fn,   st) {
      if (is_owner(fn) || (fn in guardedf) || (fn in astate)) return 1
      return 0
    }
    # list_callers(fn) - print every undeclared caller of a function whose
    # callers this check judges, and follow them.  The callers of an owner, a
    # guarded function or one the allow file puts out of the way of a loss are
    # not judged, so they are not listed.
    function list_callers(fn,   n, i, a, ctx, tvar, st) {
      if (is_owner(fn) || (fn in guardedf)) return
      st = (fn in astate) ? astate[fn] : ""
      if (st == "startup" || st == "local" || st == "dead") return
      if (!(fn in refby)) return
      n = split(refby[fn], a, "\034")
      for (i = 1; i <= n; i++) {
        ctx = a[i]
        if (ctx == "") continue
        if (ctx ~ /^TABLE:/) {
          tvar = substr(ctx, 7)
          if (!(tvar in viavar)) {
            print "LIST-TABLE " tvar " needs a dispatcher named (via NAME) on the line of one of its entries"
            continue
          }
          if (!declared(viavar[tvar]) && !(viavar[tvar] in listed)) {
            listed[viavar[tvar]] = 1
            print "LIST " wfile[viavar[tvar]] "|" viavar[tvar] \
                  "|0|?|? no wire call of its own, the dispatcher the table " tvar " names"
          }
          list_callers(viavar[tvar])
          continue
        }
        if (declared(ctx) || (ctx in listed)) continue
        listed[ctx] = 1
        print "LIST " wfile[ctx] "|" ctx "|0|?|? no wire call of its own, a caller of " fn
        list_callers(ctx)
      }
    }
    BEGIN {
      # The words that read as "identifier (" in a definition and are a type
      # or a qualifier, not the name of the function.
      split("char int void unsigned signed long short double float struct" \
            " union enum static const register extern sizeof return if while" \
            " for switch do case", kw, " ")
      for (i in kw) c_keyword[kw[i]] = 1
      # The client-side Xlib calls: no request, no reply (see the header).
      split("XKeysymToKeycode XStringToKeysym XPutBackEvent XGetPixel XPutPixel" \
            " XAddPixel XDestroyImage XFree XSetErrorHandler XCreateImage" \
            " XDefaultDepth XDisplayName XInitImage", lx, " ")
      for (i in lx) local_x[lx[i]] = 1
      # The owners, and the only wire calls each of them may hold, as
      # "NAME:count" a name.  The two waits are the places that find out the
      # connection is gone, so they are exempt call by call and not as whole
      # functions: anything else added to them is an offence, so is another
      # call of an exempt name, and so is an exempt call placed before the
      # test that ends the wait -- that one runs on a connection nobody has
      # asked about yet, which is the crash the owner exists to prevent.
      owner_wire["e_x_conn_lost"] = ""
      owner_wire["e_x_conn_usable"] = ""
      owner_wire["e_x_conn_step_usable"] = ""
      owner_wire["e_x_io_error"] = ""
      owner_wire["e_x_io_exit"] = ""
      owner_wire["e_x_wait_for_event"] = "XPending:1 XFlush:1"
      owner_wire["e_x_getch"] = "XPending:2 XFlush:1 XNextEvent:1"
      # The allow file: file|function|count|state|reason.
      while ((getline line < allowfile) > 0) {
        if (line ~ /^[ \t]*(#|$)/) continue
        n = split(line, fld, "|")
        if (n != 5 || fld[2] == "" || fld[3] !~ /^[0-9]+$/ || fld[5] == "") {
          print "bad line in the allow file (want file|function|count|state|reason): " line
          continue
        }
        if (fld[4] != "startup" && fld[4] != "local" && fld[4] != "dead" && fld[4] != "below") {
          print "bad state in the allow file (want startup, local, dead or below): " line
          continue
        }
        key = fld[2]
        if (key in astate) { print "duplicate allow line for " key; continue }
        afile[key] = fld[1]; acount[key] = fld[3] + 0
        astate[key] = fld[4]; areason[key] = fld[5]
        if (match(fld[5], /via [A-Za-z_][A-Za-z0-9_]*/))
          avia[key] = substr(fld[5], RSTART + 4, RLENGTH - 4)
        if (strictmode == 1 && (fld[4] == "startup" || fld[4] == "dead"))
          print "strict: this line is work still to do, not an exception: " line
      }
      close(allowfile)
    }
    FNR == 1 {
      blank_incom = 0; inmacro = 0
      nfiles++
      fnl[nfiles] = 0
      fbase[nfiles] = FILENAME
      sub(/^.*\//, "", fbase[nfiles])
    }
    {
      n = ++fnl[nfiles]
      fraw[nfiles, n] = $0
      fcode[nfiles, n] = blank_c($0, 0)
      fmacro[nfiles, n] = (inmacro || fcode[nfiles, n] ~ /^[ \t]*#/) ? 1 : 0
      inmacro = fmacro[nfiles, n] ? ($0 ~ /\\[ \t]*$/) : 0
    }
    END {
      for (p = 1; p <= 2; p++)
        for (fi = 1; fi <= nfiles; fi++)
          walk(fi, p)
      # The wire calls of each function, without the names that are functions
      # of the backend itself, and in a fixed order for --list.
      for (k in wcntof) {
        split(k, kp, SUBSEP)
        if (kp[2] in defined) continue
        wcnt[kp[1]] += wcntof[k]
        wsort[kp[1], ++wsn[kp[1]]] = kp[2]
      }
      for (f in wsn) {
        for (i = 2; i <= wsn[f]; i++) {
          v = wsort[f, i]
          for (j = i - 1; j >= 1 && wsort[f, j] > v; j--) wsort[f, j + 1] = wsort[f, j]
          wsort[f, j + 1] = v
        }
        for (i = 1; i <= wsn[f]; i++)
          wlist[f] = (wlist[f] == "" ? wsort[f, i] : wlist[f] "," wsort[f, i])
      }
      # The dispatcher of a table: the one the allow lines of its entries name.
      for (v in tabentries) {
        tn = split(tabentries[v], ta, " ")
        for (ti = 1; ti <= tn; ti++)
          if (ta[ti] in avia) { viavar[v] = avia[ta[ti]]; break }
      }
      # An owner is exempt call by call, so a call its exemption does not name
      # is an offence: an exemption of the whole function would hide it.  So
      # is a count that is not the one the exemption names, and so is an
      # exempt call that
      # comes before the test that ends the wait.
      for (f in owner_wire) {
        for (i = 1; i <= wsn[f]; i++)
          if (owner_extra(f, wsort[f, i]) != "")
            print "owner " f " holds " wsort[f, i] ", which its exemption does not name"
        if (wsn[f] == 0) continue
        o_n = split(owner_wire[f], o_a, " ")
        for (i = 1; i <= o_n; i++) {
          o_id = owner_name(o_a[i])
          o_got = wcntof[f, o_id] + 0
          if (o_got != owner_want(o_a[i]))
            print "owner " f " holds " o_got " " o_id " calls, its exemption counts " \
                  owner_want(o_a[i])
        }
        o_at = owner_lost_at(f)
        if (o_at == "") {
          print "owner " f " holds exempt wire calls, and nothing in it asks the owner" \
                " of the connection first"
          continue
        }
        w_n = split(wposof[f], w_a, "\034")
        for (i = 1; i <= w_n; i++) {
          if (w_a[i] == "") continue
          w_p = index(w_a[i], "=")
          if (!pos_after(substr(w_a[i], w_p + 1), o_at))
            print "owner " f " calls " substr(w_a[i], 1, w_p - 1) \
                  " before the test that ends the wait, where its exemption does not reach"
        }
      }
      for (f in defined) if (is_guarded(f)) guardedf[f] = 1
      # A guard with something that can send before it guards nothing, whether
      # or not the function has a wire call of its own.
      for (f in firstif) {
        if ((f in guardedf) || !guard_shape(firstif[f])) continue
        late = pre_wire(f)
        if (late == "") continue
        lateguard[f] = late
        print "the guard of " f " comes after " late ", which can reach the server: " \
              wfile[f] " " f
      }
      for (f in wcnt)
        if (!(f in owner_wire) && !(f in guardedf) && !(f in lateguard)) wired[f] = 1
      for (f in wired) if (!safe(f))
        print (why[f] == "" \
                ? "reaches the X server with no owner asked about the connection" \
                : why[f]) ": " wfile[f] " " f " (" wcnt[f] " wire call(s): " wlist[f] ")"
      for (k in astate) {
        if (k in owner_wire) {
          print "allow line for an owner, delete it: " k
          continue
        }
        if (k in guardedf) {
          print "allow line for a guarded function, delete it: " k
          continue
        }
        if (!(k in defined)) {
          print "stale allow line, delete it: " afile[k] "|" k
          continue
        }
        if (k in wcnt) {
          if (wcnt[k] > acount[k])
            print "more wire calls than the allow file says: " k " has " wcnt[k] ", the line says " acount[k]
          else if (wcnt[k] < acount[k])
            print "fewer wire calls than the allow file says: " k " has " wcnt[k] ", lower the line"
        } else if (acount[k] != 0) {
          print "the allow file counts " acount[k] " wire calls in " k ", which makes none" \
                " (a line for a function with no call of its own counts 0)"
        }
        if (afile[k] != "" && wfile[k] != "" && afile[k] != wfile[k])
          print "allow line names the wrong file: " k " is in " wfile[k] ", the line says " afile[k]
      }
      if (listmode == 1) {
        for (f in wcnt)
          if (!(f in owner_wire) && !(f in guardedf))
            print "LIST " wfile[f] "|" f "|" wcnt[f] "|?|? " wlist[f]
        for (f in wired) list_callers(f)
        for (f in astate) if (astate[f] == "below") list_callers(f)
      }
      for (k in srcname) {
        split(k, kp, SUBSEP)
        print "S " kp[1] " " kp[2] > namedump
      }
      for (f in inchdr) {
        hn = split(inchdr[f], ha, " ")
        for (hi = 1; hi <= hn; hi++) if (ha[hi] != "") print "I " f " " ha[hi] > namedump
      }
      close(namedump)
    }' "$@"
}

planted() {
  mkdir -p "$WORK/plant"
  printf '%s\n' "$2" > "$WORK/plant/$1"
}

# canary NAME ALLOW SOURCE EXPECT - EXPECT is an extended regular expression
# a line of the judge must match, or the word none for a source the judge must
# not name at all.  A canary is judged as a plain run: --list would print the
# lines it suggests for clean code too, and the canary would read them as
# offences.
canary() {
  planted canary.c "$3"
  printf '%s\n' "$2" > "$WORK/canary.allow"
  list_saved=$LIST
  LIST=0
  got=$(judge "$WORK/canary.allow" "$WORK/plant/canary.c" 2>&1)
  LIST=$list_saved
  if [ "$4" = none ]; then
    if [ -n "$got" ]; then
      echo "canary $1: clean code was named: $got"
      return 1
    fi
    return 0
  fi
  if ! printf '%s\n' "$got" | grep -Eq "$4"; then
    echo "canary $1: an offender was not found (wanted $4, got: $got)"
    return 1
  fi
  return 0
}

# The canaries run at the start of every run, as the other check scripts run
# theirs: a scanner that has gone blind fails the build instead of passing
# everything it looks at.  --selftest runs the canaries and nothing else.
selftest() {
  blind=0
  # a wire call with nothing asking about the connection
  canary unguarded '' 'void some_paint(void) { XFillRectangle(d, w, g, 0, 0, 1, 1); }' \
    'no owner asked.*some_paint' || blind=1
  # a call through the renderer's table is judged at the caller, not where the
  # address was taken: on X11 every one of them draws on the window's surface
  canary render-pointer '' 'void some_paint(void) { WpeRender.draw_rect(0, 0, 1, 1, 0); }' \
    'no owner asked.*some_paint' || blind=1
  canary render-pointer-guarded '' \
    'void some_paint(void) { if (!e_x_conn_usable("the painted frame")) return; WpeRender.draw_rect(0, 0, 1, 1, 0); }' \
    none || blind=1
  canary render-pointer-not-a-call '' \
    'void some_paint(void) { if (WpeRender.draw_rect) helper(); }' none || blind=1
  # a bare call of the owner, whose answer is thrown away, guards nothing
  canary bare-call '' 'void some_paint(void) { e_x_conn_usable("the paint"); XFillRectangle(d, w, g, 0, 0, 1, 1); }' \
    'no owner asked.*some_paint' || blind=1
  # the guard after a wire call guards nothing, and is named as what it is
  canary guard-late '' 'void some_paint(void) { XFillRectangle(d, w, g, 0, 0, 1, 1); if (!e_x_conn_usable("the paint")) return; }' \
    'the guard of some_paint comes after XFillRectangle' || blind=1
  # the guard in the wrong shape
  canary guard-shape '' 'void some_paint(void) { if (e_x_conn_usable("the paint")) XFillRectangle(d, w, g, 0, 0, 1, 1); }' \
    'no owner asked.*some_paint' || blind=1
  # a guard whose "return" is the start of another name returns nothing
  canary guard-assignment '' \
    'int some_paint(void) { int return_seen = 0; if (!e_x_conn_usable("the paint")) return_seen = 1; XFillRectangle(d, w, g, 0, 0, 1, 1); return return_seen; }' \
    'no owner asked.*some_paint' || blind=1
  # declarations may come before the guard, a wire call may not
  canary clean-guarded '' 'void some_paint(void) { int x = 0; Pixmap p = q;
 if (!e_x_conn_usable("the paint"))
  return;
 XFillRectangle(d, p, g, x, 0, 1, 1); }' none || blind=1
  canary clean-guarded-expr '' 'int some_paint(void) { if (!e_x_conn_usable("the paint")) return 0; return XSync(d, 0); }' \
    none || blind=1
  # the local families need no line
  canary clean-local '' 'void f(void) { FcFontMatch(0, p, &r); cairo_region_create();
 cairo_image_surface_create(0, 1, 2); cairo_font_options_create();
 cairo_set_source_rgb(c, 0, 0, 0); XKeysymToKeycode(d, k); XDestroyImage(i); XFree(p); }' \
    none || blind=1
  canary clean-comment '' 'void f(void) { /* XFillRectangle(d, w, g, 0, 0, 1, 1); */ }
// XSync(d, 0);' none || blind=1
  canary clean-string '' 'void f(void) { trace("XFillRectangle cairo_fill"); }' none || blind=1
  canary clean-type '' 'void f(XEvent *e) { XExposeEvent *x = (XExposeEvent *)e; use(x->xexpose.x); }' none || blind=1
  # below: a helper of a guarded owner is covered by its caller
  canary below-ok 'canary.c|bb_helper|1|below|the pixmap call behind the guarded owner' \
    'void bb_helper(void) { XFreePixmap(d, p); }
void owner_call(void) { if (!e_x_conn_usable("the picture copy")) return; bb_helper(); }' none || blind=1
  canary below-bad 'canary.c|bb_helper|1|below|the pixmap call behind the guarded owner' \
    'void bb_helper(void) { XFreePixmap(d, p); }
void other_call(void) { other_helper(); }
void other_helper(void) { bb_helper(); }' 'reached from other_helper' || blind=1
  # a call of a helper that reaches the server counts as a wire call for
  # "before the guard": the guard has to be the first thing that can send
  canary below-before-guard 'canary.c|bb_copy_to_window|1|below|the copy of a guarded present' \
    'void bb_copy_to_window(void) { XCopyArea(d, s, t, g, 0, 0, 1, 1, 0); }
void e_x_backbuf_present(void) { bb_copy_to_window(); if (!e_x_conn_usable("the picture copy")) return; }' \
    'the guard of e_x_backbuf_present comes after bb_copy_to_window' || blind=1
  canary below-after-guard 'canary.c|bb_copy_to_window|1|below|the copy of a guarded present' \
    'void bb_copy_to_window(void) { XCopyArea(d, s, t, g, 0, 0, 1, 1, 0); }
void e_x_backbuf_present(void) { if (!e_x_conn_usable("the picture copy")) return; bb_copy_to_window(); }' \
    none || blind=1
  # a new helper of a guarded owner is below; the same helper with no line is not
  canary below-new 'canary.c|bb_helper|1|below|a new helper of a guarded owner' \
    'void bb_helper(void) { XBell(d, 0); }
void owner_call(void) { if (!e_x_conn_usable("the picture copy")) return; bb_helper(); }' none || blind=1
  canary unlisted-helper '' \
    'void bb_helper(void) { XBell(d, 0); }
void owner_call(void) { if (!e_x_conn_usable("the picture copy")) return; bb_helper(); }' \
    'no owner asked.*bb_helper' || blind=1
  canary below-uncalled 'canary.c|bb_helper|1|below|nothing calls it' \
    'void bb_helper(void) { XFreePixmap(d, p); }
void owner_call(void) { if (!e_x_conn_usable("the picture copy")) return; }' \
    'nothing in the four files calls it' || blind=1
  # a table registration names its dispatcher, and the dispatcher is judged;
  # a plain step between the two is declared below like any other helper
  canary table-ok 'canary.c|sink_present|1|below|via cr_flush: the sink table
canary.c|cr_flush|0|below|the sink dispatcher, registered in WpeRender
canary.c|cr_set|0|below|fills the WpeRender table
canary.c|WpeXInit|1|startup|the display is opened here' \
    'static const struct cr_sink cr_sink_direct = { sink_present };
void sink_present(void) { cairo_surface_flush(s); }
void cr_flush(void) { cr_sink->present(0, 0, 1, 1); }
void cr_set(void) { WpeRender.flush = cr_flush; }
int WpeXInit(void) { XOpenDisplay(0); cr_set(); return 0; }' none || blind=1
  canary table-no-via 'canary.c|sink_present|1|below|the sink table
canary.c|cr_flush|0|below|the sink dispatcher
canary.c|WpeXInit|1|startup|the display is opened here' \
    'static const struct cr_sink cr_sink_direct = { sink_present };
void sink_present(void) { cairo_surface_flush(s); }
void cr_flush(void) { cr_sink->present(0, 0, 1, 1); }
int WpeXInit(void) { XOpenDisplay(0); cr_flush(); return 0; }' 'names a dispatcher' || blind=1
  canary table-bad-via 'canary.c|sink_present|1|below|via cr_flush: the sink table
canary.c|WpeXInit|1|startup|the display is opened here' \
    'static const struct cr_sink cr_sink_direct = { sink_present };
void sink_present(void) { cairo_surface_flush(s); }
void cr_flush(void) { XSync(d, 0); cr_sink->present(0, 0, 1, 1); }
int WpeXInit(void) { XOpenDisplay(0); cr_flush(); return 0; }' 'the dispatcher it names, cr_flush, is not itself guarded' || blind=1
  # the dispatcher of a table may be named on the line of any of its entries,
  # so a plain entry of the same table is covered too
  canary table-via-plain 'canary.c|sink_cleanup|1|below|via cr_cleanup: the sink table entry
canary.c|sink_init|0|below|a plain entry of the same table
canary.c|cr_cleanup|0|below|the sink dispatcher
canary.c|cr_set|0|below|fills the WpeRender table
canary.c|WpeXInit|1|startup|the display is opened here' \
    'static const struct cr_sink cr_sink_direct = { sink_init, sink_cleanup };
void sink_init(void) { sink_cleanup(); }
void sink_cleanup(void) { cairo_surface_destroy(s); }
void cr_cleanup(void) { cr_sink->cleanup(); }
void cr_set(void) { WpeRender.cleanup = cr_cleanup; }
int WpeXInit(void) { XOpenDisplay(0); cr_set(); return 0; }' none || blind=1
  # a line for a function with no wire call of its own counts 0
  canary count-none 'canary.c|plain_helper|1|dead|nothing calls it' \
    'void plain_helper(void) { helper2(); }' 'which makes none' || blind=1
  canary count-none-ok 'canary.c|plain_helper|0|dead|nothing calls it' \
    'void plain_helper(void) { helper2(); }
void other(void) { plain_helper(); }' none || blind=1
  # startup, local and dead are accepted with their count, and the count is
  # checked in both directions
  canary startup 'canary.c|WpeXInit|1|startup|the display is opened here' \
    'int WpeXInit(void) { XOpenDisplay(0); return 0; }' none || blind=1
  canary count-low 'canary.c|WpeXInit|1|startup|the display is opened here' \
    'int WpeXInit(void) { XOpenDisplay(0); XSync(d, 0); return 0; }' 'more wire calls than the allow file' || blind=1
  canary count-high 'canary.c|WpeXInit|3|startup|the display is opened here' \
    'int WpeXInit(void) { XOpenDisplay(0); return 0; }' 'fewer wire calls than the allow file' || blind=1
  canary stale 'canary.c|gone_now|1|startup|nothing left to say' \
    'int WpeXInit(void) { return 0; }' 'stale allow line' || blind=1
  canary guarded-line 'canary.c|some_paint|1|startup|a line for a function that is guarded' \
    'void some_paint(void) { if (!e_x_conn_usable("the paint")) return; XSync(d, 0); }' \
    'allow line for a guarded function' || blind=1
  canary bad-line 'canary.c|f|x|startup|a count that is not a number' \
    'void f(void) { XSync(d, 0); }' 'bad line in the allow file' || blind=1
  canary local-no-reason 'canary.c|f|1|local|' \
    'void f(void) { XSync(d, 0); }' 'bad line in the allow file' || blind=1
  canary bad-state 'canary.c|f|1|maybe|a state that is not one' \
    'void f(void) { XSync(d, 0); }' 'bad state in the allow file' || blind=1
  canary wrong-file 'other.c|f|1|startup|a line that names the wrong file' \
    'void f(void) { XSync(d, 0); }' 'allow line names the wrong file' || blind=1
  # the owners: exempt call by call, so a new one is an offence; the exempt
  # calls are counted, and each of them sits after the test that ends the
  # wait, so a call added before that test is an offence too
  canary owner-ok '' \
    'void e_x_wait_for_event(void) { while (!WpeXInfo.lost && !XPending(d)) { XFlush(d); if (WpeXInfo.lost) break; wait(); } }' \
    none || blind=1
  canary owner-extra '' \
    'void e_x_wait_for_event(void) { while (!WpeXInfo.lost && !XPending(d)) { XFlush(d); wait(); } XBell(d, 0); }' \
    'owner e_x_wait_for_event holds XBell' || blind=1
  canary owner-no-lost-test '' \
    'void e_x_wait_for_event(void) { while (!XPending(d)) { XFlush(d); wait(); } }' \
    'nothing in it asks the owner of the connection' || blind=1
  canary owner-count '' \
    'void e_x_wait_for_event(void) { while (!WpeXInfo.lost && !XPending(d)) { XFlush(d); XFlush(d); wait(); } }' \
    'owner e_x_wait_for_event holds 2 XFlush calls' || blind=1
  # the key reader asks the silent predicate through a helper of its own, so
  # the test that ends the wait is found through the call graph
  canary getch-ok '' \
    'int e_x_getch(void) { if (e_x_wait_once_lost(0)) return 0; while (1) { if (!XPending(d)) { XFlush(d); wait(); } if (!XPending(d)) continue; XNextEvent(d, &r); } }
int e_x_wait_once_lost(int w) { if (e_x_conn_step_usable()) return 0; wait(); return 1; }' \
    none || blind=1
  canary getch-extra '' \
    'int e_x_getch(void) { if (e_x_wait_once_lost(0)) return 0; while (1) { if (!XPending(d)) { XFlush(d); wait(); } if (!XPending(d)) continue; XNextEvent(d, &r); XQueryPointer(d, w); } }
int e_x_wait_once_lost(int w) { if (e_x_conn_step_usable()) return 0; wait(); return 1; }' \
    'owner e_x_getch holds XQueryPointer' || blind=1
  canary getch-early-read '' \
    'int e_x_getch(void) { XNextEvent(d, &r); if (e_x_wait_once_lost(0)) return 0; while (1) { if (!XPending(d)) { XFlush(d); wait(); } if (!XPending(d)) continue; } }
int e_x_wait_once_lost(int w) { if (e_x_conn_step_usable()) return 0; wait(); return 1; }' \
    'owner e_x_getch calls XNextEvent before the test' || blind=1
  canary getch-extra-wait '' \
    'int e_x_getch(void) { if (e_x_wait_once_lost(0)) return 0; while (1) { if (!XPending(d)) { XFlush(d); wait(); } if (!XPending(d)) { XFlush(d); wait(); } XNextEvent(d, &r); } }
int e_x_wait_once_lost(int w) { if (e_x_conn_step_usable()) return 0; wait(); return 1; }' \
    'owner e_x_getch holds 2 XFlush calls' || blind=1
  # a wire call hidden in a macro is counted against the macro
  canary macro 'canary.c|e_print_xrect|2|below|via fk_show_cursor: the box segments' \
    '#define e_print_xrect(x, y, n) XSetForeground(d, g, c); XDrawLine(d, w, g, x, y, x, y);
void fk_show_cursor(void) { if (!e_x_conn_usable("the refresh")) return 0; e_print_xrect(1, 2, 3); }' none || blind=1
  canary macro-unguarded '' \
    '#define e_print_xrect(x, y, n) XSetForeground(d, g, c); XDrawLine(d, w, g, x, y, x, y);
void fk_show_cursor(void) { e_print_xrect(1, 2, 3); }' 'no owner asked.*e_print_xrect' || blind=1
  if [ $blind -ne 0 ]; then
    echo "check-x11-lost-owner: the scanner is blind"
    exit $BAD
  fi
  echo "check-x11-lost-owner: canaries ok"
}

selftest
[ $SELFTEST -eq 1 ] && exit 0

if [ ! -f "$ALLOW" ]; then
  echo "check-x11-lost-owner: no allow file at $ALLOW" >&2
  exit $BAD
fi

# What this build contains: a build without X has no X11 backend to scan.
[ $LIST -eq 1 ] && WPE_BUILD_QUIET=1
. "$HERE/build-config.sh"
if ! wpe_built with-x; then
  echo "check-x11-lost-owner: SKIP, $(wpe_not_built_reason with-x)"
  exit $SKIP
fi

files=
missing=
for f in $X11_SOURCES; do
  if [ -f "$SRCDIR/$f" ]; then files="$files $SRCDIR/$f"; else missing="$missing $f"; fi
done
if [ -n "$missing" ]; then
  echo "check-x11-lost-owner: no source under $SRCDIR for:$missing" >&2
  exit $BAD
fi
# shellcheck disable=SC2086
found=$(judge "$ALLOW" $files 2>&1)
if [ $LIST -eq 1 ]; then
  printf '%s\n' "$found" | grep '^LIST ' | sed 's/^LIST //' | sort
  printf '%s\n' "$found" | grep '^LIST-TABLE ' | sed 's/^LIST-TABLE //' | sort -u
  printf '%s\n' "$found" | grep -v '^LIST'
  exit 0
fi
if [ -n "$found" ]; then
  echo "check-x11-lost-owner: a call can reach an X server that is gone, or an allow line is off:"
  printf '%s\n' "$found" | sort
  exit $BAD
fi

# Pass 2: the objects' undefined symbols against the source scan.
if [ "$NMTOOL" = ":" ] || ! command -v "$NMTOOL" >/dev/null 2>&1; then
  echo "check-x11-lost-owner: SKIP, no nm ($NMTOOL)"
  exit $SKIP
fi
[ -n "$OBJECTS" ] || OBJECTS="we_xterm.o WeXterm.o we_xbackbuf.o we_render_cairo.o"
for obj in $OBJECTS; do
  [ -f "$obj" ] || continue
  src=$(basename "$obj" .o).c
  "$NMTOOL" -u "$obj" 2>/dev/null | awk '{ print $NF }' |
    grep -E '^(X[A-Z]|Xft|XRender|cairo_|pango_cairo_)' | sort -u > "$WORK/sym-$src"
  [ -s "$WORK/sym-$src" ] || continue
  awk -v src="$src" -v obj="$obj" -v symfile="$WORK/sym-$src" '
    $1 == "S" { seen[$2, $3] = 1 }
    $1 == "I" && $2 == src { nh++; hdrs[nh] = $3 }
    END {
      while ((getline sym < symfile) > 0) {
        if ((src, sym) in seen) continue
        found = 0
        for (i = 1; i <= nh; i++) if ((hdrs[i], sym) in seen) { found = 1; break }
        if (!found)
          print "check-x11-lost-owner: " obj " imports " sym \
                ", which no call in " src " or in a header it includes names"
      }
      close(symfile)
    }' "$WORK/srcnames"
done > "$WORK/syms"
if [ -s "$WORK/syms" ]; then
  cat "$WORK/syms"
  exit $BAD
fi
echo "check-x11-lost-owner: clean (the X11 connection has one owner, WeXterm.c)"
exit 0
