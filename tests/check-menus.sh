#!/bin/sh
# check-menus.sh -- the menus of the menu bar have one owner, the table.
#
# What a dropdown shows is a function of the table (we_menus.c, wpe_menu_rows)
# and of five facts (key style, block mode, editor mode, display, assistant).
# The engine that draws and runs a dropdown (we_menue.c) is blind to all of
# them.  A site that builds an item by hand, an engine that asks which mode it
# is in, or a second writer of the bar's columns is a second menu: it drifts
# from the table, and the manual and the palette describe the wrong menu.
#
#   check-menus.sh [--allow FILE] [--srcdir DIR]
#
# Three rules, each with a canary: the script plants an offender in a copy of
# the sources and fails when the rule does not name it.
#
#  1. WpeFillSubmenuItem and WpeFillBoundItem are called only in we_menus.c
#     and in the two prefix dropdown builders (the assistant's Alt-G menu in
#     we_ai_ui.c, the language server's Alt-Q menu in we_debug.c), which hang
#     from no bar entry.  Their counts, and the definition, the prototype and
#     the extern, are named in tests/check-menus.allow (`file count reason`);
#     the counts are exact, so a stale line fails too.
#  2. we_menue.c names none of WpeIsProg, WpeIsXwin, wpe_binding_style, ED_,
#     e_hlp_str, #ifdef PROG, #ifdef DEBUGGER, #ifdef WPE_AI, #ifdef NEWSTYLE:
#     the engine is mode-blind.
#  3. MENOPT, e_mn_men and the .s and .as of the bar's opt[] are written only
#     in we_menus.c.
#
# Exit: 0 clean, 1 offenders, a stale allow line or a blind scanner.

ALLOW=
SRCDIR=${srcdir:-.}
BAD=1
HERE=$(dirname "$0")

while [ $# -gt 0 ]; do
  case $1 in
    --allow) shift; ALLOW=$1 ;;
    --srcdir) shift; SRCDIR=$1 ;;
    *) echo "check-menus: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/check-menus.allow

WORK=$(mktemp -d "${TMPDIR:-/tmp}/menus.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

# scan DIR - "rule|file:line|text" for every offence in the sources of DIR.
# One awk pass over the files: comments are blanked, then each rule is a
# pattern on the line.
scan() {
  dir=$1
  for f in "$dir"/*.c "$dir"/*.h; do
    [ -f "$f" ] || continue
    awk -v base="$(basename "$f")" '
      BEGIN { incom = 0 }
      {
        line = $0; out = ""; n = length(line); i = 1
        while (i <= n) {
          d = substr(line, i, 2)
          if (incom) { if (d == "*/") { incom = 0; i += 2 } else i++; continue }
          if (d == "/*") { incom = 1; i += 2; continue }
          out = out substr(line, i, 1); i++
        }
        if (base != "we_menus.c" && out ~ /WpeFill(Submenu|Bound)Item(Sep)?[ \t]*\(/)
          print "1|" base ":" NR "|" out
        if (base == "we_menue.c" &&
            out ~ /(WpeIsProg|WpeIsXwin|wpe_binding_style|e_hlp_str)|(^|[^A-Za-z0-9_])ED_[A-Z_]+|^[ \t]*#[ \t]*if(def)?[ \t]+(PROG|DEBUGGER|WPE_AI|NEWSTYLE)([^A-Za-z0-9_]|$)/)
          print "2|" base ":" NR "|" out
        if (base != "we_menus.c" &&
            (out ~ /(^|[^A-Za-z0-9_])(MENOPT|e_mn_men)[ \t]*(=[^=]|\+\+|--|\+=|-=)/ ||
             out ~ /(^|[^A-Za-z0-9_])opt\[[^]]*\]\.(s|as)[ \t]*=[^=]/))
          print "3|" base ":" NR "|" out
      }' "$f"
  done
}

# counts - "file count" of the rule-1 offences read from stdin.
counts() {
  awk -F'|' '$1 == 1 { split($2, a, ":"); c[a[1]]++ } END { for (f in c) print f, c[f] }' | sort
}

# The rule-1 sites that are allowed: file, exact count, reason.
allowed() {
  [ -f "$ALLOW" ] || return
  grep -v '^[ \t]*#' "$ALLOW" | awk 'NF >= 3 { print $1, $2 }' | sort
}

fail=
real=$(scan "$SRCDIR")

# rules 2 and 3 admit nothing; rule 1 admits what the allow file counts.
rest=$(printf '%s\n' "$real" | awk -F'|' '$1 != 1 && NF >= 3')
if [ -n "$rest" ]; then
  echo "check-menus: offenders"
  printf '%s\n' "$rest"
  fail=1
fi
printf '%s\n' "$real" | counts > "$WORK/found"
allowed > "$WORK/allowed"
if ! diff "$WORK/allowed" "$WORK/found" > "$WORK/diff"; then
  echo "check-menus: the menu items built outside we_menus.c differ from $ALLOW"
  echo "(< the allow file, > what was found):"
  cat "$WORK/diff"
  fail=1
fi

# canaries: a copy of the sources with one offender per rule must be named.
mkdir "$WORK/c" || exit $BAD
cp "$SRCDIR"/*.c "$SRCDIR"/*.h "$WORK/c/"
echo 'static void canary_one(void) { WpeFillSubmenuItem("x", 0, 0, 0); }' >> "$WORK/c/we_wind.c"
echo 'static int canary_two(void) { return WpeIsProg(); }' >> "$WORK/c/we_menue.c"
echo 'static void canary_three(void) { MENOPT = 3; }' >> "$WORK/c/we_unix.c"
seen=$(scan "$WORK/c")
for r in 1 2 3; do
  if ! printf '%s\n' "$seen" | grep -q "^$r|"; then
    echo "check-menus: blind scanner: the canary of rule $r was not named"
    fail=1
  fi
done
# a second canary for rule 3 in the shape the bar is written
echo 'static void canary_four(void) { opt[5].as = 0; }' >> "$WORK/c/we_unix.c"
if ! scan "$WORK/c" | grep -q "^3|we_unix.c:[0-9]*|static void canary_four"; then
  echo "check-menus: blind scanner: opt[n].as written outside we_menus.c was not named"
  fail=1
fi

[ -n "$fail" ] && exit $BAD
echo "check-menus: clean"
exit 0
