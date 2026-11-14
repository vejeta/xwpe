/* test_errpat.c -- unit test for the owner of a compiler's message pattern
 * (we_errpat.c), the compile half.  Always runs; run it under ASan+UBSan too.
 *
 * What the user sees behind each group:
 *  - every compiler the editor knows keeps working: its pattern is read,
 *    with the rows, warning tests and ${FILE} it always had;
 *  - a pattern cut inside a ${...} token, a [...] class or a backslash is
 *    refused with a reason that names the token, instead of the editor
 *    reading past the end of the text;
 *  - a pattern the editor reads in a surprising way is accepted and says how
 *    it read it (an unknown variable, rows past the fourth);
 *  - whatever bytes the pattern holds, reading it never reads outside it.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../we_errpat.h"

static int fails;

static void chk_int(const char *what, long got, long want)
{
  if (got != want)
  {
    printf("FAIL: %s: got %ld want %ld\n", what, got, want);
    fails++;
  }
}

static void chk_str(const char *what, const char *got, const char *want)
{
  if (!got || strcmp(got, want) != 0)
  {
    printf("FAIL: %s: got [%s] want [%s]\n", what, got ? got : "(null)", want);
    fails++;
  }
}

static void chk_has(const char *what, const char *got, const char *want)
{
  if (!got || !strstr(got, want))
  {
    printf("FAIL: %s: [%s] lacks [%s]\n", what, got ? got : "(null)", want);
    fails++;
  }
}

/* The pattern text in a heap block of exactly its size, so a read past the
 * text is a read past the block and the sanitizer says so. */
static WpeErrPat *compile_exact(const char *text, char diag[WPE_ERRPAT_DIAG])
{
  size_t n = strlen(text) + 1;
  char *copy = malloc(n);
  WpeErrPat *p;

  memcpy(copy, text, n);
  p = wpe_errpat_compile(copy, diag);
  free(copy);
  return p;
}

/* Compile @text, expect it accepted; the caller frees the pattern. */
static WpeErrPat *accept(const char *what, const char *text)
{
  char diag[WPE_ERRPAT_DIAG];
  WpeErrPat *p = compile_exact(text, diag);

  if (!p)
  {
    printf("FAIL: %s: refused: %s\n", what, diag);
    fails++;
  }
  return p;
}

/* Compile @text, expect it refused with @reason in the diagnostic. */
static void refuse(const char *what, const char *text, const char *reason)
{
  char diag[WPE_ERRPAT_DIAG];
  WpeErrPat *p = compile_exact(text, diag);

  if (p)
  {
    printf("FAIL: %s: accepted [%s]\n", what, text);
    wpe_errpat_free(p);
    fails++;
    return;
  }
  chk_has(what, diag, reason);
}

/* ------------------------------------------------------------------ */
/* 01: the built-in patterns                                            */

typedef struct
{
  const char *symbol, *text;
  int rows, markers, names_file;
} Builtin;

#define PC_TEXT "${?0:e}${?0:w}${?0:s}*:*:* * ${FILE}:\n\n* ${LINE}  ${CMPTEXT}\n*-------${COLUMN=PREVIOUS?^+14}\n[EWew] * line*"

static const Builtin builtins[] = {
  {"gnu_intstr", "${?*:warning:}${FILE}:${LINE}:${COLUMN}:*", 1, 1, 1},
  {"cc_intstr", "${?*:warning:}\"${FILE}\", line ${LINE}:* at or near * \"${COLUMN=AFTER}\"", 1, 1, 1},
  {"A68G_INTSTR", "${LINE} *", 1, 0, 0},
  {"fpc", "${?*:Warning:}${FILE}(${LINE},${COLUMN})*", 1, 1, 1},
  {"javac/scalac", "${?*:warning:}${FILE}:${LINE}:*", 1, 1, 1},
  {"python", "*${FILE}, line ${LINE}", 1, 0, 1},
  {"latex", "${FILE}:${LINE}:", 1, 0, 1},
  {"perl", "*at ${FILE} line ${LINE}", 1, 0, 1},
  {"go/cobc", "${FILE}:${LINE}:*", 1, 0, 1},
  {"rust", "*--> ${FILE}:${LINE}:${COLUMN}*", 1, 0, 1},
  {"pc", PC_TEXT, 4, 3, 1},
};

/* @scenario SC-errpat-01 */
/* Every pattern of a compiler the editor knows is read, with the rows,
 * warning tests and file naming it always had. */
static void test_builtins(void)
{
  size_t i;

  for (i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
  {
    const Builtin *b = &builtins[i];
    WpeErrPat *p = accept(b->symbol, b->text);

    if (!p)
      continue;
    chk_int(b->symbol, wpe_errpat_row_count(p), b->rows);
    chk_int(b->symbol, wpe_errpat_marker_count(p), b->markers);
    chk_int(b->symbol, wpe_errpat_names_file(p), b->names_file);
    chk_int(b->symbol, wpe_errpat_note_count(p), 0);
    chk_str(b->symbol, wpe_errpat_notes(p), "");
    wpe_errpat_free(p);
  }
}

/* @scenario SC-errpat-01 */
/* The rows and warning tests the GNU and the pc patterns are cut into. */
static void test_builtin_rows(void)
{
  WpeErrPat *p = accept("gnu rows", builtins[0].text);

  if (p)
  {
    chk_str("gnu row 0", wpe_errpat_row(p, 0), "${FILE}:${LINE}:${COLUMN}:*");
    chk_str("gnu marker text", wpe_errpat_marker_text(p, 0), "warning:");
    chk_int("gnu marker column", wpe_errpat_marker_column(p, 0), -1);
    wpe_errpat_free(p);
  }
  p = accept("pc rows", PC_TEXT);
  if (p)
  {
    chk_str("pc row 0", wpe_errpat_row(p, 0), "*:*:* * ${FILE}:\n");
    chk_str("pc row 1", wpe_errpat_row(p, 1), "* ${LINE}  ${CMPTEXT}");
    chk_str("pc row 2", wpe_errpat_row(p, 2), "*-------${COLUMN=PREVIOUS?^+14}");
    chk_str("pc row 3", wpe_errpat_row(p, 3), "[EWew] * line*");
    chk_str("pc marker 2", wpe_errpat_marker_text(p, 2), "s");
    chk_int("pc marker 2 column", wpe_errpat_marker_column(p, 2), 0);
    if (wpe_errpat_row(p, 4))
    {
      printf("FAIL: pc has a fifth row\n");
      fails++;
    }
    wpe_errpat_free(p);
  }
  p = accept("column marker", "${?12:late}${FILE}:${LINE}");
  if (p)
  {
    chk_int("numbered marker column", wpe_errpat_marker_column(p, 0), 12);
    chk_int("numbered marker: no note", wpe_errpat_note_count(p), 0);
    wpe_errpat_free(p);
  }
}

/* Every `intstr = WpeStrdup("...")` literal of we_prog.c must be a text of
 * the list above: a compiler added there without being added here would
 * ship a pattern this test never read. */
static int in_builtin_list(const char *text)
{
  size_t i;

  for (i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
    if (!strcmp(builtins[i].text, text))
      return 1;
  return 0;
}

/* Copy the C string literal that starts after the quote at @s into @out,
 * undoing \", \\ and \n; stop at the closing quote.  A backslash that ends
 * the line continues the literal on the next line of @fp.  @room is the size
 * of @line, which the continuation reuses. */
static void unescape_literal(const char *s, char *out, FILE *fp, char *line,
  int room)
{
  size_t o = 0;

  while (*s && *s != '"')
  {
    if (*s == '\\' && s[1] == '\n')
    {
      if (!fgets(line, room, fp))
        break;
      s = line;
    }
    else if (*s == '\\' && s[1] == 'n')
    {
      out[o++] = '\n';
      s += 2;
    }
    else if (*s == '\\' && (s[1] == '"' || s[1] == '\\'))
    {
      out[o++] = s[1];
      s += 2;
    }
    else
      out[o++] = *s++;
  }
  out[o] = '\0';
}

/* Where the literal of a built-in pattern starts in @line, or NULL: a
 * language's `intstr = WpeStrdup("...")`, a `char *x_intstr = "..."`, or
 * the `#define ..._INTSTR "..."` a language takes its pattern from. */
static const char *builtin_literal_at(const char *line)
{
  static const char *const leads[] = {
    "intstr = WpeStrdup(\"", "_intstr = \"", "_INTSTR \""
  };
  const char *at;
  size_t i;

  for (i = 0; i < sizeof leads / sizeof leads[0]; i++)
  {
    at = strstr(line, leads[i]);
    if (at)
      return at + strlen(leads[i]);
  }
  return NULL;
}

/* @scenario SC-errpat-01 */
/* A pattern literal in we_prog.c that the list lacks fails this row, the
 * ones kept in a named string or a macro as well as those written inline. */
static void test_builtin_list_is_complete(void)
{
  const char *dir = getenv("srcdir");
  char path[512], line[1024], lit[1024];
  const char *at;
  FILE *fp;
  int seen = 0;

  snprintf(path, sizeof path, "%s/we_prog.c", dir ? dir : ".");
  fp = fopen(path, "r");
  if (!fp)
  {
    printf("FAIL: cannot open %s\n", path);
    fails++;
    return;
  }
  while (fgets(line, sizeof line, fp))
  {
    at = builtin_literal_at(line);
    if (!at)
      continue;
    unescape_literal(at, lit, fp, line, (int)sizeof line);
    seen++;
    if (!in_builtin_list(lit))
    {
      printf("FAIL: we_prog.c names a pattern the list lacks: [%s]\n", lit);
      fails++;
    }
  }
  fclose(fp);
  chk_int("literals found in we_prog.c", seen >= 12, 1);
}

/* ------------------------------------------------------------------ */
/* 02: refusals and notes                                               */

/* @scenario SC-errpat-02 */
/* A ${ with no } is refused, naming the token. */
static void test_refuse_unclosed_brace(void)
{
  refuse("R1 ${FILE}:${LINE", "${FILE}:${LINE", "unclosed ${ near \"${LINE\"");
  refuse("R1 ${FILE", "${FILE", "unclosed ${ near \"${FILE\"");
  refuse("R1 in row 2", "${FILE}:${LINE}\n*${X", "unclosed ${ near \"${X\"");
}

/* @scenario SC-errpat-02 */
/* A warning test cut before its ':' or its '}' is refused. */
static void test_refuse_cut_marker(void)
{
  refuse("R2 no brace", "${FILE}:${?*:warn", "unclosed ${? near \"${?*:warn\"");
  refuse("R2 no colon", "${FILE}:${?5", "unclosed ${? near \"${?5\"");
  refuse("R2 empty", "${?", "unclosed ${? near");
  refuse("R2 text order: R1 before the marker", "${FILE:${?*:w",
    "unclosed ${ near \"${FILE:\"");
  refuse("R2 earlier cut token first", "[ab${?*:w", "unclosed [ near \"[ab\"");
}

/* @scenario SC-errpat-02 */
/* A [ with no ] is refused, wherever its run begins. */
static void test_refuse_unclosed_class(void)
{
  refuse("R3 [ab", "[ab${FILE}:${LINE}:", "unclosed [ near \"[ab");
  refuse("R3 after a variable", "${FILE}:${LINE}:[ab", "unclosed [ near \"[ab\"");
  refuse("R3 [!", "${FILE}:${LINE}:[!x", "unclosed [");
  refuse("R3 lone [", "${FILE}:${LINE}:[", "unclosed [");
  refuse("R3 empty class", "${FILE}:${LINE}:[]", "unclosed [");
  refuse("R3 escaped [", "\\[ab${FILE}:${LINE}:", "unclosed [ near \"[ab");
  refuse("R3 star then [", "*[ab${FILE}:${LINE}:", "unclosed [");
}

/* @scenario SC-errpat-02 */
/* A backslash the matcher's own scan would run past the row on is refused,
 * including one at the start of a row, where a backslash escapes nothing. */
static void test_refuse_trailing_backslash(void)
{
  refuse("R4 x\\", "${FILE}:${LINE}:x\\", "nothing after \\ at the end of row 1");
  refuse("R4 ${FILE}\\", "${LINE}:${FILE}\\", "nothing after \\");
  refuse("R4 row 2", "${FILE}:${LINE}\nx\\", "end of row 2");
  refuse("R4 \\\\ at a row start", "\\\\", "nothing after \\");
  refuse("R4 \\\\ at a row start, row 2", "${FILE}:${LINE}\n\\\\",
    "end of row 2");
  refuse("R4 star then backslash", "${FILE}:${LINE}:*\\\\\\", "nothing after \\");
}

/* @scenario SC-errpat-02 */
/* No ${LINE} anywhere, and the empty text, are refused with a reason. */
static void test_refuse_no_line(void)
{
  refuse("R8 empty", "", "empty pattern");
  refuse("R8 no LINE", "${FILE}:*", "no ${LINE}");
  refuse("R8 LINE spelt wrong", "${FILE}:${LINEX}", "no ${LINE}");
  refuse("R8 LINE in an escaped token", "${FILE}:\\${LINE}", "no ${LINE}");
  refuse("R8 LINE in a class", "${FILE}:[${LINE}]", "no ${LINE}");
  refuse("R8 LINE past the fourth row", "${FILE}\na\nb\nc\n${LINE}",
    "no ${LINE}");
  refuse("R8 only a separator", "\n", "no ${LINE}");
  refuse("R8 after R7", "${FILE}\n${X}\n${Y}\n${Z}\n${W}", "no ${LINE}");
}

/* @scenario SC-errpat-02 */
/* What is read in a surprising way is accepted, with a note. */
static void test_notes(void)
{
  WpeErrPat *p = accept("unknown variable", "${FILE}:${LINE}:${CMPTXT}");

  if (p)
  {
    chk_int("unknown variable notes", wpe_errpat_note_count(p), 1);
    chk_has("unknown variable note", wpe_errpat_notes(p),
      "unknown variable \"CMPTXT\" read as *");
    wpe_errpat_free(p);
  }
  p = accept("empty variable", "${FILE}:${LINE}:${}");
  if (p)
  {
    chk_has("empty variable note", wpe_errpat_notes(p), "unknown variable \"\"");
    wpe_errpat_free(p);
  }
  p = accept("FILEX", "${FILEX}:${LINE}");
  if (p)
  {
    chk_has("FILEX note", wpe_errpat_notes(p), "\"FILEX\"");
    chk_int("FILEX names no file", wpe_errpat_names_file(p), 0);
    wpe_errpat_free(p);
  }
  p = accept("five rows", "${FILE}:${LINE}\na\nb\nc\nd");
  if (p)
  {
    chk_int("five rows: four kept", wpe_errpat_row_count(p), 4);
    chk_has("five rows note", wpe_errpat_notes(p), "rows after the fourth");
    wpe_errpat_free(p);
  }
  p = accept("four rows", "${FILE}:${LINE}\na\nb\nc\n");
  if (p)
  {
    chk_int("four rows and a trailing separator: no note",
      wpe_errpat_note_count(p), 0);
    wpe_errpat_free(p);
  }
  p = accept("column not a number", "${?x:e}${FILE}:${LINE}:");
  if (p)
  {
    chk_has("column note", wpe_errpat_notes(p), "not a number, read as 0");
    chk_int("column read as 0", wpe_errpat_marker_column(p, 0), 0);
    wpe_errpat_free(p);
  }
  p = accept("marker without digits", "${?:e}${FILE}:${LINE}:");
  if (p)
  {
    chk_int("empty column: noted", wpe_errpat_note_count(p), 1);
    wpe_errpat_free(p);
  }
}

/* @scenario SC-errpat-02 */
/* The readings of a variable's suffix that exist today are kept, silently. */
static void test_suffix_readings_frozen(void)
{
  WpeErrPat *p = accept("suffixes", "${FILE+3}:${LINE+x}:${COLUMN=FOO}");

  if (p)
  {
    chk_int("suffixes: no note", wpe_errpat_note_count(p), 0);
    chk_int("${FILE+3} is not the ${FILE} token", wpe_errpat_names_file(p), 0);
    wpe_errpat_free(p);
  }
  p = accept("line offsets", "${FILE}:${LINE-2}:${COLUMN+1}:${COLUMN=AFTER-1}");
  if (p)
  {
    chk_int("offsets: no note", wpe_errpat_note_count(p), 0);
    wpe_errpat_free(p);
  }
}

/* @scenario SC-errpat-02 */
/* The row splitter keeps its readings: a '\r' ends a row, a doubled
 * separator ends it with a literal line end, empty rows stay, and the text
 * of a warning test may run over a separator. */
static void test_row_splitter(void)
{
  WpeErrPat *p = accept("cr rows", "${FILE}:${LINE}\r*x");

  if (p)
  {
    chk_int("cr: two rows", wpe_errpat_row_count(p), 2);
    chk_str("cr row 1", wpe_errpat_row(p, 1), "*x");
    wpe_errpat_free(p);
  }
  p = accept("crlf", "${FILE}:${LINE}\r\n*x");
  if (p)
  {
    chk_str("crlf row 0 keeps a line end", wpe_errpat_row(p, 0), "${FILE}:${LINE}\n");
    chk_str("crlf row 1", wpe_errpat_row(p, 1), "*x");
    wpe_errpat_free(p);
  }
  p = accept("lfcr and crcr", "${FILE}:${LINE}\n\r*x\r\ra");
  if (p)
  {
    chk_int("lfcr crcr: three rows", wpe_errpat_row_count(p), 3);
    chk_str("crcr row 1", wpe_errpat_row(p, 1), "*x\n");
    wpe_errpat_free(p);
  }
  p = accept("leading separator", "\n${FILE}:${LINE}");
  if (p)
  {
    chk_int("leading separator: two rows", wpe_errpat_row_count(p), 2);
    chk_str("leading separator: empty first row", wpe_errpat_row(p, 0), "");
    wpe_errpat_free(p);
  }
  p = accept("marker over a separator", "${?*:a\nb}${FILE}:${LINE}");
  if (p)
  {
    chk_int("marker over a separator: one row", wpe_errpat_row_count(p), 1);
    chk_str("marker text", wpe_errpat_marker_text(p, 0), "a\nb");
    wpe_errpat_free(p);
  }
}

/* @scenario SC-errpat-02 */
/* The literal scan is one primitive with one rule about escapes. */
static void test_literal_end(void)
{
  int cut = -1;

  chk_int("stops at a star", wpe_errpat_literal_end("ab*c", 1, &cut), 2);
  chk_int("star: not cut", cut, 0);
  chk_int("escaped star is kept", wpe_errpat_literal_end("a\\*b*", 1, &cut), 4);
  chk_int("stops at ${", wpe_errpat_literal_end("a${X}", 1, &cut), 1);
  chk_int("lone $ is literal", wpe_errpat_literal_end("a$b", 1, &cut), 3);
  chk_int("ends at the terminator", wpe_errpat_literal_end("abc", 1, &cut), 3);
  chk_int("escaped backslash", wpe_errpat_literal_end("a\\\\b", 1, &cut), 4);
  chk_int("trailing backslash", wpe_errpat_literal_end("ab\\", 1, &cut), 3);
  chk_int("trailing backslash is cut", cut, 1);
  chk_int("first byte escapes nothing, then \\\\ eats the end",
    wpe_errpat_literal_end("\\\\", 1, &cut), 2);
  chk_int("that is cut", cut, 1);
  chk_int("a backslash first byte lets the star stop the run",
    wpe_errpat_literal_end("\\*", 1, &cut), 1);
  chk_int("not cut", cut, 0);
}

/* ------------------------------------------------------------------ */
/* 06: a '$' that is not a token                                        */

/* @scenario SC-errpat-06 */
/* A '$' not followed by '{' is an ordinary byte, also at a piece start. */
static void test_lone_dollar(void)
{
  WpeErrPat *p = accept("$x", "$x${FILE}:${LINE}:");

  if (p)
  {
    chk_int("$x: one row", wpe_errpat_row_count(p), 1);
    wpe_errpat_free(p);
  }
  p = accept("$*", "$*${FILE}:${LINE}");
  if (p)
  {
    chk_int("$* with a line: no note", wpe_errpat_note_count(p), 0);
    wpe_errpat_free(p);
  }
  refuse("$* without a line", "$*${FILE}", "no ${LINE}");
  p = accept("trailing $", "${FILE}:${LINE}:$");
  if (p)
    wpe_errpat_free(p);
  p = accept("escaped dollar", "a\\${FILE}:${LINE}:");
  if (p)
    wpe_errpat_free(p);
}

/* @scenario SC-errpat-06 */
/* Classes and escapes the matcher reads today are accepted unchanged. */
static void test_classes_and_escapes(void)
{
  static const char *const ok[] = {
    "[]a]${FILE}:${LINE}:", "[!x]${FILE}:${LINE}:", "[a-c]${FILE}:${LINE}:",
    "?${FILE}:${LINE}:", "a\\*b:${FILE}:${LINE}:", "[!]]${FILE}:${LINE}:",
    "${FILE}?:${LINE}", "*?${FILE}:${LINE}", "${FILE}??:${LINE}",
    "${FILE}?[ab]:${LINE}", "*?\\x${FILE}:${LINE}", "${LINE}*", "**${LINE}",
    "${FILE}[]a]:${LINE}"
  };
  size_t i;

  for (i = 0; i < sizeof ok / sizeof ok[0]; i++)
  {
    WpeErrPat *p = accept(ok[i], ok[i]);

    if (p)
      wpe_errpat_free(p);
  }
  /* The '?' behind a variable is taken out; the backslash behind it then
   * starts the run, and nothing follows it. */
  refuse("a '?' behind a variable exposes a backslash",
    "${LINE}:${FILE}??\\", "nothing after \\");
  refuse("a '?' behind a star exposes a backslash", "${LINE}:*??\\",
    "nothing after \\");
}

/* How many "[!]a]" classes stand before the cut one in the row below: more
 * than any small number a walk might be limited to. */
#define MANY_CLASSES 200
/* A row of thousands of classes is read well within this. */
#define MANY_CLASSES_ALARM_SECONDS 10

/* Build "${FILE}:${LINE}:" + @count times @cls + @tail into a heap text. */
static char *classes_text(int count, const char *cls, const char *tail)
{
  const char *head = "${FILE}:${LINE}:";
  char *text = malloc(strlen(head) + count * strlen(cls) + strlen(tail) + 1);
  int i;

  strcpy(text, head);
  for (i = 0; i < count; i++)
    strcat(text, cls);
  strcat(text, tail);
  return text;
}

/* @scenario SC-errpat-06 */
/* A "[!]...]" class closes at its first ']' when the row byte is ']', so
 * the matcher can take a second way through the rest of the row.  A cut on
 * that second way is refused however many such classes come before it; the
 * verdict never depends on how many there are. */
static void test_second_way_through_classes(void)
{
  char *text;

  refuse("cut on the second way of one class",
    "${FILE}:${LINE}:[!]*]\\", "nothing after \\");
  text = classes_text(31, "[!]a]", "[!]*]\\");
  refuse("cut behind 31 classes", text, "nothing after \\");
  free(text);
  text = classes_text(32, "[!]a]", "[!]*]\\");
  refuse("cut behind 32 classes", text, "nothing after \\");
  free(text);
  text = classes_text(MANY_CLASSES, "[!]a]", "[!]*]\\");
  refuse("cut behind many classes", text, "nothing after \\");
  free(text);
}

/* @scenario SC-errpat-06 */
/* A "[]...]" class has no second closing (the matcher fails it on a ']'
 * row byte), so what only that closing would reach is not read, and a
 * pattern the matcher reads safely is not refused for it. */
static void test_no_second_way_through_open_classes(void)
{
  WpeErrPat *p = accept("[]*]\\ after the class", "${FILE}:${LINE}:[]*]\\");

  if (p)
    wpe_errpat_free(p);
  p = accept("[]${] inside the class", "${LINE}[]${]");
  if (p)
    wpe_errpat_free(p);
}

/* @scenario SC-errpat-07 */
/* Many "[!]" classes in one row are checked in time that grows with the
 * row, not with the number of ways through it. */
static void test_many_classes_in_time(void)
{
  char *text = classes_text(4000, "[!]a]", "");
  WpeErrPat *p;

  alarm(MANY_CLASSES_ALARM_SECONDS);
  p = accept("4000 classes", text);
  alarm(0);
  if (p)
    wpe_errpat_free(p);
  free(text);
}

/* ------------------------------------------------------------------ */
/* 07: fuzz                                                             */

/* The pieces patterns are built from: every token the matcher reads, every
 * way to cut one, escapes at a token start, separators and markers. */
static const char *const toks[] = {
  "${FILE}", "${LINE}", "${COLUMN}", "${CMPTEXT}", "${COLUMN=AFTER}",
  "${COLUMN=BEFORE}", "${COLUMN=PREVIOUS?x}", "${LINE+1}", "${?*:w}",
  "${?3:w}", "${?*:warn", "${?x:e}", "${", "${FILE", "${?", "*", "?",
  "[ab]", "[ab", "[!x]", "[!", "[]a]", "[", "\\", "\\\\", "\\*", "\\[",
  "\\$", "[\\]", "$", "$*", "a", ":", " ", "\n", "\r", "\r\n", "\n\n", "}",
  "x\\", "${CMPTXT}", "${}", "^M", "]"
};
#define NTOKS ((int)(sizeof toks / sizeof toks[0]))
#define FUZZ_SEEDS 4000
#define FUZZ_MAX_TOKENS 8
#define FUZZ_ALARM_SECONDS 60
#define FUZZ_TEXT 512

static unsigned long rng_state;

static unsigned long rng_next(void)
{
  rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
  return rng_state >> 33;
}

/* Build the pattern of one seed: 0 to 8 tokens. */
static void fuzz_text(unsigned long seed, char *out)
{
  int n, i;

  rng_state = seed * 2654435761UL + 12345UL;
  n = (int)(rng_next() % (FUZZ_MAX_TOKENS + 1));
  out[0] = '\0';
  for (i = 0; i < n; i++)
    strcat(out, toks[rng_next() % NTOKS]);
}

/* Check what must hold of every outcome, accepted or not. */
static void fuzz_check(unsigned long seed, const char *text, int *accepted)
{
  char diag[WPE_ERRPAT_DIAG], again[WPE_ERRPAT_DIAG];
  WpeErrPat *p = compile_exact(text, diag);
  WpeErrPat *q = compile_exact(text, again);

  if ((p == NULL) != (q == NULL))
  {
    printf("FAIL: seed %lu: compile is not deterministic\n", seed);
    fails++;
  }
  if (!p)
  {
    if (!diag[0] || strcmp(diag, again))
    {
      printf("FAIL: seed %lu: refusal without a stable reason [%s]\n", seed, diag);
      fails++;
    }
    return;
  }
  (*accepted)++;
  if (wpe_errpat_row_count(p) < 1 || wpe_errpat_row_count(p) > WPE_ERRPAT_ROWS)
  {
    printf("FAIL: seed %lu: %d rows\n", seed, wpe_errpat_row_count(p));
    fails++;
  }
  if (wpe_errpat_names_file(p) != (strstr(text, "${FILE}") != NULL))
  {
    printf("FAIL: seed %lu: names_file disagrees with the text\n", seed);
    fails++;
  }
  wpe_errpat_free(p);
  wpe_errpat_free(q);
}

/* @scenario SC-errpat-07 */
/* Whatever the pattern holds, reading it reads nothing outside it, ends,
 * and says the same thing twice. */
static void test_fuzz_compile(void)
{
  char text[FUZZ_TEXT];
  unsigned long seed;
  int accepted = 0;

  alarm(FUZZ_ALARM_SECONDS);
  for (seed = 0; seed < FUZZ_SEEDS; seed++)
  {
    fuzz_text(seed, text);
    fuzz_check(seed, text, &accepted);
  }
  alarm(0);
  chk_int("some patterns are accepted", accepted > 100, 1);
  chk_int("some patterns are refused", accepted < FUZZ_SEEDS - 100, 1);
}

/* @scenario SC-errpat-07 */
/* The shapes the evidence found, by name. */
static void test_evidence_texts(void)
{
  static const char *const cut[] = {
    "${?*:warn", "x\\", "${FILE}\\", "$*${FILE}", "[ab", "${FILE}:[ab",
    "${}", "${CMPTXT}", "${?x:e}", "", "${FILE}:${?5\n", "\\\\", "\\[ab"
  };
  char diag[WPE_ERRPAT_DIAG];
  size_t i;

  for (i = 0; i < sizeof cut / sizeof cut[0]; i++)
  {
    WpeErrPat *p = compile_exact(cut[i], diag);

    if (p)
      wpe_errpat_free(p);
    else if (!diag[0])
    {
      printf("FAIL: evidence %zu refused with no reason\n", i);
      fails++;
    }
  }
}

/* A long row with no separator: reading it takes time in its length only. */
static void test_long_text(void)
{
  size_t n = 20000, i;
  char *text = malloc(n + 1), diag[WPE_ERRPAT_DIAG];
  WpeErrPat *p;

  for (i = 0; i < n; i++)
    text[i] = i % 7 == 0 ? '?' : 'a';
  text[n] = '\0';
  p = wpe_errpat_compile(text, diag);
  chk_int("a long text with no ${LINE} is refused", p == NULL, 1);
  free(text);
}

int main(void)
{
  test_builtins();
  test_builtin_rows();
  test_builtin_list_is_complete();
  test_refuse_unclosed_brace();
  test_refuse_cut_marker();
  test_refuse_unclosed_class();
  test_refuse_trailing_backslash();
  test_refuse_no_line();
  test_notes();
  test_suffix_readings_frozen();
  test_row_splitter();
  test_literal_end();
  test_lone_dollar();
  test_classes_and_escapes();
  test_second_way_through_classes();
  test_no_second_way_through_open_classes();
  test_many_classes_in_time();
  test_fuzz_compile();
  test_evidence_texts();
  test_long_text();
  if (fails)
  {
    printf("%d check(s) failed\n", fails);
    return 1;
  }
  printf("test_errpat: all checks passed\n");
  return 0;
}
