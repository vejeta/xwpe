# blank-c.awk -- the one blanker of C comments and literals for the check
# scripts.  Not run by itself: a script reads this file into a shell variable
# and puts it in front of its own awk program,
#
#     BLANK_C=$(cat "$(dirname "$0")/blank-c.awk")
#     awk "$BLANK_C"'  { line = blank_c($0, 0) ... }' FILE...
#
# so the same text runs under gawk, mawk, busybox awk and the one-true-awk of
# the BSDs (one program text, no second -f file).
#
# blank_c(line, mode) - the line with its comments blanked and the contents of
# its string and character literals handled by MODE:
#
#   0  the contents are dropped, the quotes stay:       f("abc", 'x') -> f("", '')
#   1  the contents stay (comments only are blanked):   f("abc", 'x') -> f("abc", 'x')
#   2  each literal becomes one blank:                  f("abc", 'x') -> f( ,  )
#
# A block comment becomes one blank, so `a/**/b` stays two tokens; a line
# comment ends the line.  The output has the same number of lines as the
# input: the caller counts the lines of the file, not of the result.
#
# The state that crosses lines (inside a block comment) is `blank_incom`; the
# caller sets it to 0 at the start of each file.  A literal does not cross a
# line.  Every loop is over one line and takes `length` once, so the cost is
# linear in the size of the file on any awk.  A scan that needs the whole file
# as one string joins these lines, then splits that string once with
# split(all, ch, "") and indexes the array (tests/check-sh-idiom.sh holds the
# rule).
function blank_c(line, mode,   out, n, i, c, d, instr, q) {
  # Most lines hold nothing to blank, or are wholly inside a comment: no
  # walk over their characters.
  if (blank_incom) { if (!index(line, "*/")) return "" }
  else if (!index(line, "/") && !index(line, "\"") && !index(line, "\047")) return line
  out = ""; n = length(line); i = 1; instr = 0
  while (i <= n) {
    c = substr(line, i, 1); d = substr(line, i, 2)
    if (blank_incom) { if (d == "*/") { blank_incom = 0; i += 2 } else i++; continue }
    if (instr) {
      if (c == "\\") { if (mode == 1) out = out substr(line, i, 2); i += 2; continue }
      if (c == q) { if (mode != 2) out = out c; instr = 0 }
      else if (mode == 1) out = out c
      i++; continue
    }
    if (d == "/*") { blank_incom = 1; out = out " "; i += 2; continue }
    if (d == "//") break
    if (c == "\"" || c == "\047") {
      instr = 1; q = c; i++
      if (mode == 2) out = out " "; else out = out c
      continue
    }
    out = out c; i++
  }
  return out
}

# chars_load(pieces, npieces, chars) - the text a scan walks, as an array of
# characters: pieces[1..npieces] are the blanked lines (each with its own
# separator, as the caller wants the lines joined), put in chars[1..n], one
# character each; returns n.  The pieces are never joined into one string:
# appending to a long string copies it on some awks, once per line, which
# made a scan of a 1 MB file take seconds.  A caller walks chars[i] for
# i = 1..n, and takes n from here once.
function chars_load(pieces, npieces, chars,   k, j, m, n, parts) {
  delete chars
  n = 0
  for (k = 1; k <= npieces; k++) {
    m = split(pieces[k], parts, "")
    for (j = 1; j <= m; j++) chars[++n] = parts[j]
  }
  return n
}

# chars_span(chars, from, to) - characters from..to of the loaded text,
# joined; it stops at the end of the text.
function chars_span(chars, from, to,   s, k) {
  s = ""
  for (k = from; k <= to; k++) {
    if (!(k in chars)) break
    s = s chars[k]
  }
  return s
}
