#!/bin/sh
# check-grid-size.sh -- the screen grid has one owner of its size and memory.
#
# The editor draws on a grid of MAXSCOL x MAXSLNS cells and keeps four planes
# for it (schirm, altschirm, extbyte, altextbyte).  The size and the planes
# are one object, held and changed only by we_grid.c, so that a refused
# allocation leaves the old grid whole and every count is made once, in a
# size_t, under a ceiling.  The compiler is the first rule: the six names are
# read-only views of the owner's object, so no file can assign one.  This
# script covers what the compiler cannot.
#
#   check-grid-size.sh [--srcdir DIR] [--selftest]
#
# It reads every *.c and *.h of the top folder (a new file is scanned without
# anyone adding it; generated protocol files and config.h are not) and every
# tests/*.c except the owner's own unit (tests/test_grid.c).  Comments, strings and character literals are blanked, and the
# text is scanned by STATEMENT (the tokens up to a ; { or }, a preprocessor
# directive with its continuation lines counting as one), so a product split
# over two lines is read as one.  we_grid.c is the owner; in we_grid.h the
# macro block that defines the six names and the one extern of the owner's
# object are the owner's.  Outside the owner a statement is an offence when:
#  1. a product has MAXSCOL and MAXSLNS each as a DIRECT factor, bare or
#     under a cast or parentheses that hold only that name:
#     (size_t)MAXSCOL * MAXSLNS and MAXSLNS * 2 * MAXSCOL are offences;
#     altschirm[(MAXSLNS - 1) * MAXSCOL + j] and schirm[y * MAXSCOL + x] are
#     not.  An object-like macro whose body is one of the names (bare, cast
#     or parenthesized) is an alias and is read as the name; a function-like
#     macro whose body multiplies two of its parameters, invoked with both
#     sizes, is an offence at the invocation.  A whole-grid count belongs to
#     wpe_grid_cells().
#  2. malloc, calloc, realloc, MALLOC, REALLOC, reallocarray or alloca is
#     called with SCREENCELL, MAXSCOL or MAXSLNS in its arguments, or an array
#     is declared whose bound names MAXSCOL or MAXSLNS and a second factor (a
#     row buffer, char buf[MAXSCOL + 1], is legal and bounded by the
#     ceiling).
#  3. a plane name is passed to free, FREE, realloc or REALLOC, or is the
#     bare first argument of memset, memcpy or memmove (memset(&altschirm[i],
#     ...) is legal).
#  4. it names wpe_grid_now, casts to WpeGrid * (with or without a const), or
#     declares an object of type WpeGrid.
#  5. it defines or undefines one of the six names, or pastes with ## a token
#     that starts with MAXS, schirm, altschirm, extbyte or altextbyte.
#  6. it declares extern one of the six names.
#
# Limits of a scan of the text: a plane pointer copied to a local and freed
# under that name; a count kept in two variables and multiplied later; a
# plane pointer taken as &schirm[0]; a helper that multiplies its own
# parameters and is called with both sizes (vterm_new(MAXSLNS, MAXSCOL) is
# such a call, and legal).  The unit under the sanitizer and the parity rows
# are the check for those.
#
# Canaries (--selftest, and at the start of every run): one offence of each
# kind, among them an alias, a macro with code, a helper planted in a copy of
# a real file, a product split over two lines, a calloc of SCREENCELL, a free
# of a plane, a cast, the same cast with a const, and an extern, and the legal
# lines named above; a scan that does not name every offence, or names a legal
# line, fails the script as blind.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
SELFTEST=0
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "check-grid-size: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done

BLANK_C=$(cat "$SRCDIR/tests/blank-c.awk")

# The one owner of the compiler call (tests/compiler.sh): the read-only
# canaries compile with the compiler configure gave, flags and all.
. "$(dirname "$0")/compiler.sh"

# The awk program.  MODE is "alias" (print the names and macros a file adds to
# the scanned set) or "scan" (print the offences).  EXTRA carries what the
# alias pass found: lines "W name", "H name", "P name" and "M name".
PROGRAM='
function ident(t) { return (t ~ /^[A-Za-z_][A-Za-z0-9_]*$/) }
function member(i) {
  if (i > 1 && tok[i - 1] == ".") return 1
  if (i > 2 && tok[i - 1] == ">" && tok[i - 2] == "-") return 1
  return 0
}
function tokenise(s,   t) {
  t = s
  gsub(/[A-Za-z_][A-Za-z0-9_]*/, " & ", t)
  gsub("[][+*/%(),=<>!&|^~?:.#;{}-]", " & ", t)
  return split(t, tok, " ")
}
function interesting(s,   k) {
  for (k = 1; k <= nnames; k++) if (index(s, names[k])) return 1
  return 0
}
function is_boundary(t) {
  return (t == "+" || t == "-" || t == "," || t == "=" || t == "<" || t == ">" \
          || t == "?" || t == ":" || t == "&" || t == "|" || t == "^" \
          || t == "!" || t == "~" || t == ";" || t == "{" || t == "}")
}
function seg_hit(d) { return (sw[d] && sh[d] && ss[d]) }
# product_found(n, Wset, Hset): a product whose direct factors include a name
# of Wset and a name of Hset.
function product_found(n, Wset, Hset,   i, t, d, found, pw, ph, nm, kw) {
  d = 0; found = 0
  sw[0] = sh[0] = ss[0] = 0; gc[0] = 0; gone[0] = ""
  for (i = 1; i <= n; i++) {
    t = tok[i]
    if (t == "(" || t == "[") {
      d++; sw[d] = sh[d] = ss[d] = 0; gc[d] = 0; gone[d] = ""
      gopen[d] = t; gcall[d] = (i > 1 && ident(tok[i - 1]) && !(tok[i - 1] in KW))
      continue
    }
    if (t == ")" || t == "]") {
      if (seg_hit(d)) found = 1
      if (d == 0) continue
      pw = ph = 0; nm = gone[d]
      if (gopen[d] == "(" && !gcall[d] && gc[d] == 1) {
        if (nm in Wset) pw = 1
        else if (nm in Hset) ph = 1
      }
      d--
      gc[d]++
      gone[d] = (gc[d] == 1 && (pw || ph)) ? nm : ""
      if (pw) sw[d] = 1
      if (ph) sh[d] = 1
      continue
    }
    if (is_boundary(t)) {
      if (seg_hit(d)) found = 1
      sw[d] = sh[d] = ss[d] = 0; gc[d]++; gone[d] = ""
      continue
    }
    if (t == "*") { ss[d] = 1; gc[d]++; gone[d] = ""; continue }
    if (ident(t) && !member(i)) {
      if (t in Wset) { sw[d] = 1; gone[d] = (gc[d] == 0) ? t : "" }
      else if (t in Hset) { sh[d] = 1; gone[d] = (gc[d] == 0) ? t : "" }
      else gone[d] = ""
    } else gone[d] = ""
    gc[d]++
  }
  if (seg_hit(d)) found = 1
  return found
}
function has_name(set, n,   i) {
  for (i = 1; i <= n; i++) if ((tok[i] in set) && !member(i)) return 1
  return 0
}
function has_word(w, n,   i) {
  for (i = 1; i <= n; i++) if (tok[i] == w && !member(i)) return 1
  return 0
}
function report(rule, s,   t) {
  t = s; gsub(/[ \t]+/, " ", t)
  print file ":" line ": " rule ": " substr(t, 1, 70)
}
# rule 1, macros: an invocation of a product macro with both sizes.
function macro_product(n,   i, j, depth, arg, k, hasw, hasq) {
  for (i = 1; i < n; i++) {
    if (!((tok[i] in PM) && tok[i + 1] == "(" && !member(i))) continue
    depth = 0; arg = 1; hasw = ""; hasq = ""
    for (j = i + 1; j <= n; j++) {
      if (tok[j] == "(") { depth++; continue }
      if (tok[j] == ")") { depth--; if (depth == 0) break; continue }
      if (depth == 1 && tok[j] == ",") { arg++; continue }
      if (tok[j] in WS) hasw = hasw "," arg ","
      if (tok[j] in HS) hasq = hasq "," arg ","
    }
    for (k = 1; k <= arg; k++)
      if (index(hasw, "," k ",")) {
        for (j = 1; j <= arg; j++)
          if (j != k && index(hasq, "," j ",")) return 1
      }
  }
  return 0
}
function rule_alloc(n,   i, j, depth, hit) {
  for (i = 1; i < n; i++) {
    if (!((tok[i] in ALLOC) && tok[i + 1] == "(" && !member(i))) continue
    depth = 0
    for (j = i + 1; j <= n; j++) {
      if (tok[j] == "(") depth++
      else if (tok[j] == ")") { depth--; if (depth == 0) break }
      else if (tok[j] == "SCREENCELL" || (tok[j] in WS) || (tok[j] in HS)) return 1
    }
  }
  return 0
}
# rule 2, arrays: TYPE NAME[ ...bound with a size name and a * ... ]
function rule_array(n,   i, j, depth, hasname, hasstar, k) {
  for (i = 1; i < n; i++) {
    if (tok[i + 1] != "[" || !ident(tok[i]) || (tok[i] in KW) || (tok[i] in PS)) continue
    k = i - 1
    if (k >= 1 && tok[k] == "*") k--
    if (k < 1 || !ident(tok[k]) || (tok[k] in KW2)) continue
    depth = 0; hasname = 0; hasstar = 0
    for (j = i + 1; j <= n; j++) {
      if (tok[j] == "[") depth++
      else if (tok[j] == "]") { depth--; if (depth == 0) break }
      else if ((tok[j] in WS) || (tok[j] in HS)) hasname = 1
      else if (tok[j] == "*") hasstar = 1
    }
    if (hasname && hasstar) return 1
  }
  return 0
}
function skip_cast(j,   m) {
  if (tok[j] != "(") return j
  for (m = j + 1; tok[m] != ")" && m <= ntok; m++)
    if (!(ident(tok[m]) || tok[m] == "*")) return j
  return m + 1
}
function rule_free(n,   i, j) {
  for (i = 1; i < n; i++) {
    if (!(tok[i + 1] == "(" && !member(i))) continue
    if (tok[i] in FREEFN) {
      j = skip_cast(i + 2)
      if ((tok[j] in PS) && (tok[j + 1] == "," || tok[j + 1] == ")")) return 1
    }
    if (tok[i] in FILLFN) {
      j = skip_cast(i + 2)
      if ((tok[j] in PS) && tok[j + 1] == ",") return 1
    }
  }
  return 0
}
function handle(s, ln, prep,   n, t, name, k, own) {
  line = ln
  if (!interesting(s) && !(mode == "alias" && prep)) return
  if (file ~ /we_grid\.c$/ || file ~ /tests\/test_grid\.c$/) return
  n = tokenise(s); ntok = n
  if (file ~ /we_grid\.h$/) {
    if (prep && (n >= 3 && tok[2] == "define") && ((tok[3] in SIX))) return
    if (index(s, "extern") && index(s, "wpe_grid_now")) return
  }
  if (prep && n >= 3 && (tok[2] == "define" || tok[2] == "undef")) {
    name = tok[3]
    if (name in SIX) { report("rule 5 (names the owner defines)", s); return }
  }
  if (mode == "alias") { alias_of(s, n); return }
  if (product_found(n, WS, HS)) report("rule 1 (product of the two sizes)", s)
  else if (macro_product(n)) report("rule 1 (product macro of the two sizes)", s)
  if (rule_alloc(n) || rule_array(n)) report("rule 2 (allocation sized from the grid)", s)
  if (rule_free(n)) report("rule 3 (a plane freed or filled whole)", s)
  if (has_word("wpe_grid_now", n)) report("rule 4 (the owner object named)", s)
  else if (s ~ /\([ \t]*(const[ \t]+)?WpeGrid[ \t]*\*[ \t]*\)/) report("rule 4 (cast to the owner type)", s)
  else if (s ~ /(^|[^A-Za-z0-9_])WpeGrid[ \t]+[A-Za-z_]/) report("rule 4 (an object of the owner type)", s)
  if (s ~ /(MAXS[A-Za-z0-9_]*|[a-z]*schirm|[a-z]*extbyte)[ \t]*##/ || s ~ /##[ \t]*(MAXS|[a-z]*schirm|[a-z]*extbyte)/)
    report("rule 5 (pasting a grid name)", s)
  if (n >= 1 && tok[1] == "extern" && has_name(SIX, n)) report("rule 6 (extern of a grid name)", s)
}
# The alias pass: object-like macros that are one of the names, and
# function-like macros that multiply two parameters.
function alias_of(s, n,   i, name, last, ok, np, p, j, body, a, b, nb) {
  if (!(prep_now && n >= 3 && tok[2] == "define")) return
  name = tok[3]
  if (match(s, /#[ \t]*define[ \t]+[A-Za-z_][A-Za-z0-9_]*\(/)) {
    # function-like: parameters up to ), body after
    np = 0; i = 5
    while (i <= n && tok[i] != ")") { if (ident(tok[i])) pn[++np] = tok[i]; i++ }
    nb = 0
    for (j = i + 1; j <= n; j++) btok[++nb] = tok[j]
    if (np >= 2) {
      for (a = 1; a < np; a++) for (b = a + 1; b <= np; b++) {
        delete PA; delete PB; PA[pn[a]] = 1; PB[pn[b]] = 1
        for (j = 1; j <= nb; j++) tok[j] = btok[j]
        if (product_found(nb, PA, PB)) { print "M " name; return }
      }
    }
    return
  }
  ok = 1; last = ""
  for (i = 4; i <= n; i++) {
    if (tok[i] == "(" || tok[i] == ")") continue
    if (ident(tok[i])) { if (last != "") { if (!(last in CASTW)) ok = 0 }; last = tok[i]; continue }
    ok = 0
  }
  if (!ok || last == "") return
  if (last in W0) print "W " name
  else if (last in H0) print "H " name
  else if (last in P0) print "P " name
}
BEGIN {
  W0["MAXSCOL"] = 1; H0["MAXSLNS"] = 1
  P0["schirm"] = 1; P0["altschirm"] = 1; P0["extbyte"] = 1; P0["altextbyte"] = 1
  SIX["MAXSCOL"] = 1; SIX["MAXSLNS"] = 1; SIX["schirm"] = 1
  SIX["altschirm"] = 1; SIX["extbyte"] = 1; SIX["altextbyte"] = 1
  for (k in W0) WS[k] = 1
  for (k in H0) HS[k] = 1
  for (k in P0) PS[k] = 1
  n = split(ENVIRON["GRID_SIZE_EXTRA"], e, "\n")
  for (k = 1; k <= n; k++) {
    split(e[k], p, " ")
    if (p[1] == "W") { WS[p[2]] = 1 } else if (p[1] == "H") { HS[p[2]] = 1 }
    else if (p[1] == "P") { PS[p[2]] = 1 } else if (p[1] == "M") { PM[p[2]] = 1 }
  }
  nnames = 0
  for (k in WS) names[++nnames] = k
  for (k in HS) names[++nnames] = k
  for (k in PS) names[++nnames] = k
  for (k in PM) names[++nnames] = k
  names[++nnames] = "wpe_grid_now"; names[++nnames] = "WpeGrid"
  names[++nnames] = "SCREENCELL"
  n = split("size_t int long unsigned char short ptrdiff_t ssize_t SCREENCELL const", e, " ")
  for (k = 1; k <= n; k++) CASTW[e[k]] = 1
  n = split("if while for switch return sizeof else case do goto", e, " ")
  for (k = 1; k <= n; k++) KW[e[k]] = 1
  n = split("return case goto else sizeof do", e, " ")
  for (k = 1; k <= n; k++) KW2[e[k]] = 1
  n = split("malloc calloc realloc MALLOC REALLOC reallocarray alloca", e, " ")
  for (k = 1; k <= n; k++) ALLOC[e[k]] = 1
  FREEFN["free"] = 1; FREEFN["FREE"] = 1; FREEFN["realloc"] = 1; FREEFN["REALLOC"] = 1
  FILLFN["memset"] = 1; FILLFN["memcpy"] = 1; FILLFN["memmove"] = 1
}
FNR == 1 {
  blank_incom = 0; prepcont = 0; cur = ""; dir = ""; file = FILENAME
}
{
  code = blank_c($0, 2)
  if (prepcont || code ~ /^[ \t]*#/) {
    if (!prepcont) { dir = ""; dirline = FNR }
    piece = code
    cont = (piece ~ /\\[ \t]*$/)
    sub(/\\[ \t]*$/, "", piece)
    dir = dir " " piece
    prepcont = cont
    if (!cont) { prep_now = 1; handle(dir, dirline, 1); prep_now = 0; dir = "" }
    next
  }
  rest = code
  if (cur == "") curline = FNR
  while (match(rest, /[;{}]/)) {
    cur = cur " " substr(rest, 1, RSTART - 1)
    handle(cur, curline, 0)
    cur = ""; curline = FNR
    rest = substr(rest, RSTART + 1)
  }
  if (rest !~ /^[ \t]*$/) { if (cur == "") curline = FNR; cur = cur " " rest }
}
'

# files FOLDER: every source the scan reads, one per line.
files() {
  for f in "$1"/*.c "$1"/*.h "$1"/tests/*.c; do
    [ -f "$f" ] || continue
    case $f in
      */*-protocol.c|*/*-protocol.h|*/config.h) continue ;;
    esac
    printf '%s\n' "$f"
  done
}

# run_scan LIST: the alias pass, then the scan, over the files of LIST (one
# per line): one "FILE:LINE: rule: statement" per offence.  The alias pass's
# names reach the scan through the environment, not -v: they are one per
# line, and the BSDs' awk refuses a -v value with a newline in it ("newline
# in string"), so the scan there named nothing and the selftest failed.
run_scan() {
  ex=$(GRID_SIZE_EXTRA="" awk -v mode=alias "$BLANK_C$PROGRAM" $1 | sort -u)
  GRID_SIZE_EXTRA="$ex" awk -v mode=scan "$BLANK_C$PROGRAM" $1
}

# plant FOLDER: a copy of the sources with one offence of each kind, and the
# legal lines the scan must leave alone.
plant() {
  mkdir -p "$1/tests"
  cp "$SRCDIR/we_grid.h" "$SRCDIR/we_grid.c" "$SRCDIR/we_wind.c" "$1/"
  cp "$SRCDIR/tests/blank-c.awk" "$1/tests/"
  {
    printf '\n#define SCRW MAXSCOL\n'
    printf 'static int planted_alias(int y) { return SCRW * MAXSLNS + y; }\n'
    printf '#define GRIDBYTES (MAXSCOL * MAXSLNS)\n'
    printf 'static size_t planted_helper(void)\n{\n return MAXSLNS * MAXSCOL;\n}\n'
    printf 'static int planted_split(void)\n{\n return (size_t)MAXSCOL\n    * MAXSLNS;\n}\n'
    printf 'static void planted_alloc(void)\n{\n'
    printf ' SCREENCELL *p = calloc(100, sizeof(SCREENCELL));\n (void)p;\n}\n'
    printf 'static void planted_free(void)\n{\n free(schirm);\n}\n'
    printf 'static void planted_fill(void)\n{\n memset(altschirm, 0, 10);\n}\n'
    printf 'static void planted_cast(void)\n{\n WpeGrid *g = (WpeGrid *)0;\n (void)g;\n}\n'
    printf 'static void planted_cast_const(void)\n{\n const WpeGrid *g = (const WpeGrid *)0;\n (void)g;\n}\n'
    printf 'static void planted_now(void)\n{\n (void)wpe_grid_now;\n}\n'
    printf 'extern int MAXSCOL;\n'
    printf '#undef MAXSLNS\n'
    printf '#define AREA(a, b) ((a) * (b))\n'
    printf 'static int planted_area(void)\n{\n return AREA(MAXSCOL, MAXSLNS);\n}\n'
    printf 'static void planted_array(void)\n{\n char grid[MAXSCOL * 2];\n (void)grid;\n}\n'
    printf '#define GLUE(a) MAXSCOL ## a\n'
    printf 'static int legal_index(int y, int x)\n{\n return schirm[y * MAXSCOL + x].ch;\n}\n'
    printf 'static int legal_row(int j)\n{\n return altschirm[(MAXSLNS - 1) * MAXSCOL + j].ch;\n}\n'
    printf 'static void legal_buf(void)\n{\n char buf[MAXSCOL + 1];\n (void)buf;\n}\n'
    printf 'static void legal_fill(int row, int col)\n{\n'
    printf ' memset(&altschirm[row * MAXSCOL + col], 0, sizeof(SCREENCELL));\n}\n'
    printf 'static int legal_pixels(int w, int fw)\n{\n return w == MAXSCOL * fw;\n}\n'
    printf 'static int legal_ratio(void)\n{\n return MAXSCOL / MAXSLNS;\n}\n'
  } >> "$1/we_wind.c"
}

# readonly_canaries FOLDER: the six names are read-only views, so a file that
# assigns, increments or takes the address of one does not compile, and one
# that writes a cell through a plane does.  Fails when the compiler accepts
# an assignment (the "+ 0" of the macros was taken out).
readonly_canaries() {
  status=0
  test_cc_require
  for body in 'MAXSCOL = 1;' 'MAXSLNS++;' 'schirm = 0;' 'altschirm = 0;' \
              'extbyte = 0;' 'altextbyte = 0;' 'int *p = &MAXSCOL; (void)p;'; do
    printf '#include "we_grid.h"\nvoid f(void)\n{\n %s\n}\n' "$body" > "$1/ro.c"
    if test_cc -fsyntax-only -I"$1" "$1/ro.c" > /dev/null 2>&1; then
      echo "check-grid-size: SELFTEST FAILED, this compiles: $body"
      status=1
    fi
  done
  printf '#include "we_grid.h"\nvoid f(void)\n{\n schirm[1].ch = 2;\n}\n' > "$1/ro.c"
  if ! test_cc -fsyntax-only -I"$1" "$1/ro.c" > /dev/null 2>&1; then
    echo "check-grid-size: SELFTEST FAILED, a write through a plane does not compile"
    status=1
  fi
  return $status
}

selftest() {
  . "$(dirname "$0")/workdir.sh"
  work_dir grid-size 1
  plant "$WORK"
  run_scan "$(files "$WORK")" > "$WORK/found.txt"
  missing=0
  for want in "SCRW \* MAXSLNS" "GRIDBYTES (MAXSCOL" "return MAXSLNS \* MAXSCOL" \
              "(size_t)MAXSCOL" "calloc(100, sizeof(SCREENCELL))" "free(schirm)" \
              "memset(altschirm, 0, 10)" "(WpeGrid \*)0" \
              "(const WpeGrid \*)0" "(void)wpe_grid_now" \
              "extern int MAXSCOL" "undef MAXSLNS" "return AREA(MAXSCOL, MAXSLNS)" \
              "char grid\[MAXSCOL \* 2\]" "MAXSCOL ## a"; do
    if ! grep -q -- "$want" "$WORK/found.txt"; then
      echo "check-grid-size: SELFTEST FAILED, not named: $want"
      missing=1
    fi
  done
  for legal in "schirm\[y \* MAXSCOL + x\]" "altschirm\[(MAXSLNS - 1)" "char buf\[MAXSCOL + 1\]" \
               "memset(&altschirm" "w == MAXSCOL \* fw" "MAXSCOL / MAXSLNS"; do
    if grep -q -- "$legal" "$WORK/found.txt"; then
      echo "check-grid-size: SELFTEST FAILED, a legal line was named: $legal"
      missing=1
    fi
  done
  readonly_canaries "$WORK" || missing=1
  if [ $missing -ne 0 ]; then
    cat "$WORK/found.txt"
    exit 1
  fi
  echo "check-grid-size: selftest ok ($(wc -l < "$WORK/found.txt" | tr -d ' ') planted offences named)"
}

selftest
if [ $SELFTEST -eq 1 ]; then
  exit 0
fi

# judge ALLOWFILE FOUND: what is left of the scan's lines once the lines of
# ALLOWFILE ("file|fragment|reason", the fragment a piece of the statement)
# are taken off.  An allow line that matches nothing is stale and named.
judge() {
  printf '%s\n' "$2" | awk -v allow="$1" '
    BEGIN {
      while ((getline line < allow) > 0) {
        if (line ~ /^[ \t]*#/ || line ~ /^[ \t]*$/) continue
        n = split(line, f, "|")
        if (n < 3 || f[1] == "" || f[2] == "" || f[3] == "") { print "bad line in the allow file: " line; continue }
        na++; afile[na] = f[1]; afrag[na] = f[2]; aline[na] = line
      }
    }
    NF == 0 { next }
    {
      hit = 0
      for (k = 1; k <= na; k++)
        if (index($0, afile[k] ":") && index($0, afrag[k])) { used[k] = 1; hit = 1 }
      if (!hit) print
    }
    END { for (k = 1; k <= na; k++) if (!used[k]) print "stale allow line, delete it: " aline[k] }'
}

OFFENDERS=$(judge "$SRCDIR/tests/grid-size.allow" "$(run_scan "$(files "$SRCDIR")")")
if [ -n "$OFFENDERS" ]; then
  echo "check-grid-size: code other than the grid owner (we_grid.c) sizes, allocates or names the grid:"
  echo "$OFFENDERS"
  exit 1
fi
echo "check-grid-size: ok, only we_grid.c sizes and allocates the grid"
