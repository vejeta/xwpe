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
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "we_errpat.h"
#include "test_watchdog.h"

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

  test_watchdog_arm("test_many_classes_in_time: compiling 4000 classes",
                    MANY_CLASSES_ALARM_SECONDS);
  p = accept("4000 classes", text);
  test_watchdog_disarm();
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

  test_watchdog_arm("test_fuzz_compile: the fuzz seeds", FUZZ_ALARM_SECONDS);
  for (seed = 0; seed < FUZZ_SEEDS; seed++)
  {
    fuzz_text(seed, text);
    fuzz_check(seed, text, &accepted);
  }
  test_watchdog_disarm();
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

/* ------------------------------------------------------------------ */
/* pass B: matching and walking                                         */

/* The rows of Messages as the buffer stores them: the text, its line end and
 * a terminator, in a heap block of exactly that size, so a read past the
 * row is a read past the block and the sanitizer says so.  With @ended 0 the
 * row is the last one of a build that printed no final line end: text and
 * terminator only. */
typedef struct
{
  WpeErrRow *rows;
  char **store;
  int n;
} Rows;

static Rows rows_make(const char *const *texts, int n, int ended)
{
  Rows r;
  int i;

  r.n = n;
  r.rows = calloc((size_t)n + 1, sizeof *r.rows);
  r.store = calloc((size_t)n + 1, sizeof *r.store);
  for (i = 0; i < n; i++)
  {
    size_t len = strlen(texts[i]);
    char *s = malloc(len + (ended ? 2 : 1));

    memcpy(s, texts[i], len);
    if (ended)
      s[len++] = '\n';
    s[len] = '\0';
    r.store[i] = s;
    r.rows[i].s = s;
    r.rows[i].len = (int)strlen(texts[i]);
  }
  return r;
}

static void rows_free(Rows *r)
{
  int i;

  for (i = 0; i < r->n; i++)
    free(r->store[i]);
  free(r->store);
  free(r->rows);
}

#define MAX_HITS 4
#define MAX_ROWS 6

/* One entry the table expects. */
typedef struct
{
  const char *file;
  int line, x, kind, stops, row, text_row;
  const char *srch;
} ExpHit;

/* One line of the golden table: a pattern, the rows, the walk from @start,
 * the last row it uses and the entries it makes. */
typedef struct
{
  const char *what, *pattern;
  const char *rows[MAX_ROWS];
  int nrows;
  const char *source;
  int stop_at, start, end, nhits;
  ExpHit hit[MAX_HITS];
} Golden;

#define GNU_TEXT "${?*:warning:}${FILE}:${LINE}:${COLUMN}:*"
#define CC_TEXT "${?*:warning:}\"${FILE}\", line ${LINE}:* at or near * \"${COLUMN=AFTER}\""
#define FPC_TEXT "${?*:Warning:}${FILE}(${LINE},${COLUMN})*"
#define JAVAC_TEXT "${?*:warning:}${FILE}:${LINE}:*"
#define GO_TEXT "${FILE}:${LINE}:*"
#define ONE_ROW_TEXT "${FILE}:${LINE}:*"
#define STAR_FILE_LINE "*${FILE}\n${LINE}"

static const Golden golden[] = {
  {"gnu error", GNU_TEXT, {"t.c:3:4: error: x"}, 1, NULL, 0, 0, 0, 1,
   {{"t.c", 3, 4, 0, 1, 0, 0, NULL}}},
  {"gnu warning", GNU_TEXT, {"t.c:3:9: warning: y"}, 1, NULL, 0, 0, 0, 1,
   {{"t.c", 3, 9, 1, 0, 0, 0, NULL}}},
  {"gnu no column", GNU_TEXT, {"t.c:3: error: no column"}, 1, NULL, 0, 0, 0, 1,
   {{"t.c", 3, 0, 0, 1, 0, 0, NULL}}},
  {"gnu blank in path", GNU_TEXT, {"a/b c.c:7:2: error: blank in path"}, 1, NULL, 0, 0, 0, 1,
   {{"a/b c.c", 7, 2, 0, 1, 0, 0, NULL}}},
  {"gnu ld row", GNU_TEXT, {"ld: foo", "noise"}, 2, NULL, 0, 0, 0, 0, {{0}}},
  {"gnu noise row", GNU_TEXT, {"ld: foo", "noise"}, 2, NULL, 0, 1, 1, 0, {{0}}},
  {"gnu warning stops when asked", GNU_TEXT, {"t.c:3:9: warning: y"}, 1, NULL, 1, 0, 0, 1,
   {{"t.c", 3, 9, 1, 1, 0, 0, NULL}}},
  {"cc error", CC_TEXT, {"\"t.c\", line 7: error at or near \"z\""}, 1, NULL, 0, 0, 0, 1,
   {{"t.c", 7, 0, 0, 1, 0, 0, NULL}}},
  {"cc warning", CC_TEXT, {"\"t.c\", line 8: warning: at or near \"q\""}, 1, NULL, 0, 0, 0, 1,
   {{"t.c", 8, 0, 1, 0, 0, 0, NULL}}},
  {"fpc error", FPC_TEXT, {"hello.pas(6,10) Error: Incompatible types"}, 1, NULL, 0, 0, 0, 1,
   {{"hello.pas", 6, 10, 0, 1, 0, 0, NULL}}},
  {"fpc warning", FPC_TEXT, {"hello.pas(7,1) Warning: unused"}, 1, NULL, 0, 0, 0, 1,
   {{"hello.pas", 7, 1, 1, 0, 0, 0, NULL}}},
  {"javac error", JAVAC_TEXT, {"T.java:5: error: ;"}, 1, NULL, 0, 0, 0, 1,
   {{"T.java", 5, 0, 0, 1, 0, 0, NULL}}},
  {"javac warning", JAVAC_TEXT, {"T.java:9: warning: [x] y"}, 1, NULL, 0, 0, 0, 1,
   {{"T.java", 9, 0, 1, 0, 0, 0, NULL}}},
  {"python", "*${FILE}, line ${LINE}", {"  File \"t.py\", line 2"}, 1, NULL, 0, 0, 0, 1,
   {{"File \"t.py\"", 2, 0, 0, 1, 0, 0, NULL}}},
  {"latex", "${FILE}:${LINE}:", {"./t.tex:3: Undefined control sequence."}, 1, NULL, 0, 0, 0, 1,
   {{"./t.tex", 3, 0, 0, 1, 0, 0, NULL}}},
  {"perl", "*at ${FILE} line ${LINE}", {"syntax error at t.pl line 3, near \"x\""}, 1, NULL, 0, 0, 0, 1,
   {{"t.pl", 3, 0, 0, 1, 0, 0, NULL}}},
  {"go error", GO_TEXT, {"t.go:4:2: undefined: x"}, 1, NULL, 0, 0, 0, 1,
   {{"t.go", 4, 0, 0, 1, 0, 0, NULL}}},
  {"go warning word, no marker", GO_TEXT, {"t.cob:3: warning: y"}, 1, NULL, 0, 0, 0, 1,
   {{"t.cob", 3, 0, 1, 1, 0, 0, NULL}}},
  {"rust", "*--> ${FILE}:${LINE}:${COLUMN}*", {" --> t.rs:2:5"}, 1, NULL, 0, 0, 0, 1,
   {{"t.rs", 2, 0, 0, 1, 0, 0, NULL}}},
  {"a68g takes the source", "${LINE} *", {"12 x := y"}, 1, "t.a68", 0, 0, 0, 1,
   {{"t.a68", 12, 0, 0, 1, 0, 0, NULL}}},
  {"pc two entries", PC_TEXT,
   {"e a:b:c d t.p:", "x 12  CMP", "zz-------^", "E foo line 12"}, 4, NULL, 0, 0, 3, 2,
   {{"t.p", 12, 8, 0, 1, 0, 2, "PCMP\n"}, {"t.p", 12, 2, 0, 1, 0, 3, "PCMP\n"}}},
  {"pc cut after the line row", PC_TEXT, {"e a:b:c d t.p:", "x 12  CMP"}, 2, NULL, 0, 0, 1, 1,
   {{"t.p", 12, 0, 0, 1, 0, 1, NULL}}},
  {"empty class matches nothing", "[]a]${FILE}:${LINE}:", {"]t.c:1:"}, 1, NULL, 0, 0, 0, 0, {{0}}},
  {"negated class", "[!x]${FILE}:${LINE}:", {"yt.c:1:"}, 1, NULL, 0, 0, 0, 1,
   {{"yt.c", 1, 0, 0, 1, 0, 0, NULL}}},
  {"negated class refuses its byte", "[!x]${FILE}:${LINE}:", {"xt.c:1:"}, 1, NULL, 0, 0, 0, 0, {{0}}},
  {"class range", "[a-c]${FILE}:${LINE}:", {"bt.c:1:"}, 1, NULL, 0, 0, 0, 1,
   {{"bt.c", 1, 0, 0, 1, 0, 0, NULL}}},
  {"question mark", "?${FILE}:${LINE}:", {"zt.c:1:"}, 1, NULL, 0, 0, 0, 1,
   {{"zt.c", 1, 0, 0, 1, 0, 0, NULL}}},
  {"escaped star", "a\\*b:${FILE}:${LINE}:", {"a\\*b:t.c:1:"}, 1, NULL, 0, 0, 0, 1,
   {{"t.c", 1, 0, 0, 1, 0, 0, NULL}}},
  {"escaped star is no wildcard", "a\\*b:${FILE}:${LINE}:", {"a*b:t.c:1:"}, 1, NULL, 0, 0, 0, 0, {{0}}},
  {"lone dollar", "$x${FILE}:${LINE}:", {"$xt.c:1:"}, 1, NULL, 0, 0, 0, 1,
   {{"t.c", 1, 0, 0, 1, 0, 0, NULL}}},
  {"second row empty", "${FILE}:\n${LINE}", {"t.c:", ""}, 2, NULL, 0, 0, 0, 0, {{0}}},
  {"second row empty, from the empty row", "${FILE}:\n${LINE}", {"t.c:", ""}, 2, NULL, 0, 1, 1, 0, {{0}}},
  {"two rows", STAR_FILE_LINE, {"t.c", "7", "z"}, 3, NULL, 0, 0, 1, 1,
   {{"t.c", 7, 0, 0, 1, 0, 1, NULL}}},
  {"two rows, last row alone", STAR_FILE_LINE, {"t.c", "7", "z"}, 3, NULL, 0, 2, 2, 0, {{0}}},
  {"two rows, two messages", STAR_FILE_LINE, {"t.c", "7", "8", "z"}, 4, NULL, 0, 0, 2, 2,
   {{"t.c", 7, 0, 0, 1, 0, 1, NULL}, {"t.c", 8, 0, 0, 1, 0, 2, NULL}}},
  {"two rows, past the last message", STAR_FILE_LINE, {"t.c", "7", "8", "z"}, 4, NULL, 0, 3, 3, 0, {{0}}},
  {"rows that go on to the last", ONE_ROW_TEXT, {"t.lua:1: x\\", "a\\", "b\\"}, 3, NULL, 0, 0, 2, 1,
   {{"t.lua", 1, 0, 0, 1, 0, 0, NULL}}},
  {"rows that go on, two row pattern", "${FILE}:\n${LINE}", {"t.c:", "x\\", "y\\"}, 3, NULL, 0, 0, 0, 0, {{0}}},
  {"go row led by a word keeps the word", GO_TEXT, {"vet: ./t.go:2:23: undefined: y"}, 1, NULL, 0, 0, 0, 1,
   {{"vet: ./t.go", 2, 0, 0, 1, 0, 0, NULL}}},
  {"gnu row led by a word keeps the word", GNU_TEXT, {"vet: ./t.go:2:23: undefined: y"}, 1, NULL, 0, 0, 0, 1,
   {{"vet: ./t.go", 2, 23, 0, 1, 0, 0, NULL}}},
  {"an empty row is a row of a three row message", "${FILE}\n${LINE}\n*", {"t.c", "7", "", "x"}, 4, NULL, 0, 0, 3, 2,
   {{"t.c", 7, 0, 0, 1, 0, 2, NULL}, {"t.c", 7, 0, 0, 1, 0, 3, NULL}}},
  {"a later row cut off by rows that go on", "${FILE}\n${LINE}\n*", {"t.c", "7\\", "z"}, 3, NULL, 0, 0, 2, 0, {{0}}},
  {"a class the row never holds", "${LINE}[:]", {"12"}, 1, "t.c", 0, 0, 0, 0, {{0}}},
  {"carriage return ends a row", "${FILE}:\r${LINE}", {"t.c:", "9"}, 2, NULL, 0, 0, 1, 1,
   {{"t.c", 9, 0, 0, 1, 0, 1, NULL}}},
  {"a pattern row ending in a line end", "${FILE}:\n\n${LINE}", {"t.c:", "9"}, 2, NULL, 0, 0, 1, 1,
   {{"t.c", 9, 0, 0, 1, 0, 1, NULL}}},
};

/** Free what an entry holds. */
static void hit_free(WpeErrHit *h)
{
  free(h->file);
  free(h->srch);
}

/** Check one entry against the table's. */
static void check_hit(const char *what, int i, const WpeErrHit *h, const ExpHit *e)
{
  char tag[160];

  snprintf(tag, sizeof tag, "%s: entry %d", what, i);
  chk_str(tag, h->file, e->file);
  chk_int(tag, h->line, e->line);
  chk_int(tag, h->x, e->x);
  chk_int(tag, h->kind, e->kind);
  chk_int(tag, h->stops, e->stops);
  chk_int(tag, h->row, e->row);
  chk_int(tag, h->text_row, e->text_row);
  if (!e->srch)
    chk_int(tag, h->srch == NULL, 1);
  else
    chk_str(tag, h->srch, e->srch);
}

/* The most steps any golden row took, so the budget is shown to be far above it. */
static long golden_peak;

/** Run one line of the golden table and check every field. */
static void run_golden(const Golden *g)
{
  Rows r = rows_make(g->rows, g->nrows, 1);
  WpeErrPat *p = accept(g->what, g->pattern);
  WpeErrWalk *w;
  WpeErrHit *hits;
  int nh = 0, end, i;

  if (!p)
  {
    rows_free(&r);
    return;
  }
  w = wpe_errpat_walk_begin(p, r.rows, r.n, g->source, g->stop_at);
  hits = calloc((size_t)(r.n - g->start), sizeof *hits);
  end = wpe_errpat_walk_from(w, g->start, hits, &nh);
  chk_int(g->what, end, g->end);
  chk_int(g->what, nh, g->nhits);
  for (i = 0; i < nh && i < g->nhits; i++)
    check_hit(g->what, i, &hits[i], &g->hit[i]);
  for (i = 0; i < nh; i++)
    hit_free(&hits[i]);
  if (wpe_errpat_walk_peak_steps(w) > golden_peak)
    golden_peak = wpe_errpat_walk_peak_steps(w);
  chk_int(g->what, wpe_errpat_walk_costly_row(w), -1);
  free(hits);
  wpe_errpat_walk_end(w);
  wpe_errpat_free(p);
  rows_free(&r);
}

/* @scenario SC-errpat-03 */
/* Every row of the golden table, measured on the matcher the editor had
 * before the pattern had an owner, gives the same entries now: file, line,
 * column, kind, whether it stops the build, the rows it names and the last
 * row it uses. */
static void test_golden_table(void)
{
  size_t i;

  for (i = 0; i < sizeof golden / sizeof golden[0]; i++)
    run_golden(&golden[i]);
  printf("golden peak steps: %ld\n", golden_peak);
  chk_int("the budget is 1000 times the golden table's peak",
          golden_peak * 1000 <= WPE_ERRPAT_BUDGET, 1);
}

/** Walk @text over @texts from @start; return the last row; set *nh. */
static int walk_once(const char *text, const char *const *texts, int n, int start,
  int *nh)
{
  Rows r = rows_make(texts, n, 1);
  WpeErrPat *p = accept(text, text);
  WpeErrWalk *w;
  WpeErrHit *hits;
  int end = -1, i;

  *nh = 0;
  if (p)
  {
    w = wpe_errpat_walk_begin(p, r.rows, r.n, NULL, 0);
    hits = calloc((size_t)(r.n - start), sizeof *hits);
    end = wpe_errpat_walk_from(w, start, hits, nh);
    for (i = 0; i < *nh; i++)
      hit_free(&hits[i]);
    free(hits);
    wpe_errpat_walk_end(w);
    wpe_errpat_free(p);
  }
  rows_free(&r);
  return end;
}

/* @scenario SC-errpat-04 */
/* The walk never stands still and never leaves the rows: from every start
 * the last row used is at least the start and before the end, whichever
 * row matches the pattern's first row. */
static void test_walk_always_advances(void)
{
  static const char *const a[] = {"t.c", "7", "z"};
  static const char *const b[] = {"noise", "t.c"};
  static const char *const c[] = {"t.c:", "x\\", "y\\"};
  static const char *const d[] = {"t.c:", "", "7"};
  int start, nh, end;

  test_watchdog_arm("test_walk_always_advances", FUZZ_ALARM_SECONDS);
  for (start = 0; start < 3; start++)
  {
    end = walk_once(STAR_FILE_LINE, a, 3, start, &nh);
    chk_int("a: end >= start", end >= start, 1);
    chk_int("a: end < n", end < 3, 1);
    end = walk_once(STAR_FILE_LINE, c, 3, start, &nh);
    chk_int("c star: end >= start", end >= start && end < 3, 1);
    end = walk_once("${FILE}:\n${LINE}", c, 3, start, &nh);
    chk_int("c: end >= start", end >= start && end < 3, 1);
    end = walk_once("${FILE}:\n${LINE}", d, 3, start, &nh);
    chk_int("d: end >= start", end >= start && end < 3, 1);
  }
  for (start = 0; start < 2; start++)
  {
    end = walk_once(STAR_FILE_LINE, b, 2, start, &nh);
    chk_int("b: end >= start", end >= start && end < 2, 1);
  }
  end = walk_once(PC_TEXT, a, 3, 0, &nh);
  chk_int("pc over short rows", end >= 0 && end < 3, 1);
  test_watchdog_disarm();
}

/** Free-standing rows with nothing in them: a row of length 0 does not continue. */
static void test_empty_row_does_not_continue(void)
{
  static const char *const rows[] = {"t.c:1:", "", ""};
  int nh;
  int end = walk_once(ONE_ROW_TEXT, rows, 3, 0, &nh);

  chk_int("one entry from a row followed by empty rows", nh, 1);
  chk_int("the empty rows are not skipped", end, 0);
}

/* @scenario SC-errpat-05 */
/* A warning test at a column past the end of the row never reads past the
 * row: the row is a block of exactly its own size, and the entry is an error
 * that stops the build. */
static void test_marker_past_the_row(void)
{
  static const char *const rows[] = {"t.c:1:"};
  Rows r = rows_make(rows, 1, 1);
  WpeErrPat *p = accept("marker at 40", "${?40:x}${FILE}:${LINE}:");
  WpeErrWalk *w;
  WpeErrHit hit;
  int nh = 0;

  if (p)
  {
    w = wpe_errpat_walk_begin(p, r.rows, r.n, NULL, 0);
    memset(&hit, 0, sizeof hit);
    wpe_errpat_walk_from(w, 0, &hit, &nh);
    chk_int("one entry", nh, 1);
    chk_int("it stops the build", hit.stops, 1);
    chk_int("it is an error", hit.kind, WPE_ERRPAT_ERROR);
    if (nh)
      hit_free(&hit);
    wpe_errpat_walk_end(w);
    wpe_errpat_free(p);
  }
  rows_free(&r);
  r = rows_make(rows, 1, 0);
  p = accept("marker at 6", "${?6:x}${FILE}:${LINE}:");
  if (p)
  {
    w = wpe_errpat_walk_begin(p, r.rows, r.n, NULL, 0);
    wpe_errpat_walk_from(w, 0, &hit, &nh);
    if (nh)
      hit_free(&hit);
    wpe_errpat_walk_end(w);
    wpe_errpat_free(p);
  }
  rows_free(&r);
}

/** A string of @n copies of @unit, for the long rows of the cost tests. */
static char *repeated(const char *unit, int n)
{
  size_t ul = strlen(unit);
  char *s = malloc(ul * (size_t)n + 1);
  int i;

  for (i = 0; i < n; i++)
    memcpy(s + (size_t)i * ul, unit, ul);
  s[ul * (size_t)n] = '\0';
  return s;
}

#define COST_ROW_LENGTH 400
#define COST_ALARM_SECONDS 30

/** Walk @pattern over one row of @unit repeated and check it ends in time. */
static void check_costly(const char *what, const char *pattern, const char *unit,
  int copies)
{
  char *text = repeated(unit, copies);
  const char *texts[1];
  Rows r;
  WpeErrPat *p = accept(what, pattern);
  WpeErrWalk *w;
  WpeErrHit *hits;
  int nh = 0, end;

  texts[0] = text;
  r = rows_make(texts, 1, 1);
  if (p)
  {
    test_watchdog_arm(what, COST_ALARM_SECONDS);
    w = wpe_errpat_walk_begin(p, r.rows, r.n, NULL, 0);
    hits = calloc(1, sizeof *hits);
    end = wpe_errpat_walk_from(w, 0, hits, &nh);
    test_watchdog_disarm();
    chk_int(what, end, 0);
    chk_int(what, nh, 0);
    chk_int(what, wpe_errpat_walk_costly_row(w), 0);
    free(hits);
    wpe_errpat_walk_end(w);
    wpe_errpat_free(p);
  }
  rows_free(&r);
  free(text);
}

/* @scenario SC-errpat-04 */
/* A pattern that could backtrack for hours costs the user one row that does
 * not match, not the editor: the row spends its budget and is dropped. */
static void test_backtracking_is_bounded(void)
{
  check_costly("five stars on 400 bytes", "*a*a*a*a*b:${FILE}:${LINE}", "a", COST_ROW_LENGTH);
  check_costly("pc on 400 separators", PC_TEXT, ": ", COST_ROW_LENGTH);
}

/* @scenario SC-errpat-04 */
/* A row of 4000 bytes costs the built-in patterns nothing near the budget. */
static void test_long_rows_are_cheap(void)
{
  static const char *const builtin_texts[] = {
    GNU_TEXT, CC_TEXT, FPC_TEXT, JAVAC_TEXT, GO_TEXT, "*${FILE}, line ${LINE}",
    "*at ${FILE} line ${LINE}", "*--> ${FILE}:${LINE}:${COLUMN}*"
  };
  char *text = repeated("t.c:1: ", 600);
  const char *texts[1];
  Rows r;
  size_t i;

  texts[0] = text;
  r = rows_make(texts, 1, 1);
  for (i = 0; i < sizeof builtin_texts / sizeof builtin_texts[0]; i++)
  {
    WpeErrPat *p = accept(builtin_texts[i], builtin_texts[i]);
    WpeErrWalk *w;
    WpeErrHit hit;
    int nh = 0;

    if (!p)
      continue;
    w = wpe_errpat_walk_begin(p, r.rows, r.n, NULL, 0);
    memset(&hit, 0, sizeof hit);
    wpe_errpat_walk_from(w, 0, &hit, &nh);
    if (nh)
      hit_free(&hit);
    chk_int(builtin_texts[i], wpe_errpat_walk_costly_row(w), -1);
    chk_int(builtin_texts[i], wpe_errpat_walk_peak_steps(w) * 100 <= WPE_ERRPAT_BUDGET, 1);
    wpe_errpat_walk_end(w);
    wpe_errpat_free(p);
  }
  rows_free(&r);
  free(text);
}

/* The rows the walk fuzz matches the fuzz patterns against: what a build
 * prints, the shapes the evidence found and rows too long for a fixed buffer. */
static const char *const fuzz_rows[] = {
  "t.c:3:4: error: x", "t.c:3:9: warning: y", "t.c", "7", "z", "noise", "",
  "x\\", "a\\", "]t.c:1:", "$xt.c:1:", "a\\*b:t.c:1:", "  File \"t.py\", line 2",
  "e a:b:c d t.p:", "x 12  CMP", "zz-------^", "E foo line 12", " --> t.rs:2:5",
  "[ab", "${FILE}:1:", "\\", "?*:"
};
#define NFUZZ_ROWS ((int)(sizeof fuzz_rows / sizeof fuzz_rows[0]))
#define FUZZ_LONG_ROWS 2

/** The fuzz rows plus two long ones, in @texts (room for NFUZZ_ROWS + 2). */
static void fuzz_row_texts(const char **texts, char **longs)
{
  int i;

  for (i = 0; i < NFUZZ_ROWS; i++)
    texts[i] = fuzz_rows[i];
  longs[0] = repeated("a", COST_ROW_LENGTH);
  longs[1] = repeated(": ", COST_ROW_LENGTH);
  texts[NFUZZ_ROWS] = longs[0];
  texts[NFUZZ_ROWS + 1] = longs[1];
}

/** Walk @p from every start of @r; check the walk's promises. */
static void fuzz_walk(unsigned long seed, const WpeErrPat *p, const Rows *r)
{
  WpeErrWalk *w = wpe_errpat_walk_begin(p, r->rows, r->n, NULL, 0);
  WpeErrHit *hits = calloc((size_t)r->n, sizeof *hits);
  int start, nh, end, i;

  for (start = 0; start < r->n; start++)
  {
    nh = 0;
    end = wpe_errpat_walk_from(w, start, hits, &nh);
    if (end < start || end >= r->n || nh > r->n - start)
    {
      printf("FAIL: seed %lu: start %d gave end %d with %d entries\n", seed, start, end, nh);
      fails++;
    }
    for (i = 0; i < nh; i++)
      hit_free(&hits[i]);
  }
  free(hits);
  wpe_errpat_walk_end(w);
}

/** One seed: the walk over both row layouts, then over a pattern nobody checked. */
static void fuzz_walk_seed(unsigned long seed, const char *text, const char **texts)
{
  char diag[WPE_ERRPAT_DIAG];
  WpeErrPat *p = compile_exact(text, diag);
  WpeErrPat *u = wpe_errpat_unchecked(text);
  int ended;
  Rows r;

  for (ended = 0; ended < 2; ended++)
  {
    r = rows_make(texts, NFUZZ_ROWS + FUZZ_LONG_ROWS, ended);
    if (p)
      fuzz_walk(seed, p, &r);
    if (u)
      fuzz_walk(seed, u, &r);
    rows_free(&r);
  }
  wpe_errpat_free(p);
  wpe_errpat_free(u);
}

/* @scenario SC-errpat-07 */
/* Whatever the pattern holds, walking it ends, stays inside the rows and
 * makes no more entries than there are rows.  The same seeds also run
 * through patterns compile refused, handed to the matcher unchecked: the
 * matcher alone must stay inside a row too. */
static void test_fuzz_walk(void)
{
  char text[FUZZ_TEXT], *longs[FUZZ_LONG_ROWS];
  const char *texts[NFUZZ_ROWS + FUZZ_LONG_ROWS];
  unsigned long seed;
  int i;

  fuzz_row_texts(texts, longs);
  test_watchdog_arm("test_fuzz_walk: the fuzz seeds", FUZZ_ALARM_SECONDS);
  for (seed = 0; seed < FUZZ_SEEDS; seed++)
  {
    fuzz_text(seed, text);
    fuzz_walk_seed(seed, text, texts);
  }
  test_watchdog_disarm();
  for (i = 0; i < FUZZ_LONG_ROWS; i++)
    free(longs[i]);
}

/* @scenario SC-errpat-07 */
/* The shapes the evidence found, walked by name: each ends and stays inside
 * its rows, whether compile accepts the pattern or the matcher gets it raw. */
static void test_evidence_walks(void)
{
  static const unsigned long seeds[] = {52, 100, 113, 176, 424, 491, 699, 867, 910,
    1009, 1081, 1538, 1632, 1826, 1831, 2694, 2837};
  static const char *const raw[] = {
    "${?*:warn", "x\\", "${FILE}\\", "[ab", "${FILE}:[ab", "${}", "\\\\", "\\[ab",
    "${FILE", "${LINE", "$", "$*", "[", "[!", "\\", "${?", "*\\", "${LINE}\\"
  };
  char text[FUZZ_TEXT], *longs[FUZZ_LONG_ROWS];
  const char *texts[NFUZZ_ROWS + FUZZ_LONG_ROWS];
  size_t i;
  int j;

  fuzz_row_texts(texts, longs);
  test_watchdog_arm("test_evidence_walks", FUZZ_ALARM_SECONDS);
  for (i = 0; i < sizeof seeds / sizeof seeds[0]; i++)
  {
    fuzz_text(seeds[i], text);
    fuzz_walk_seed(seeds[i], text, texts);
  }
  for (i = 0; i < sizeof raw / sizeof raw[0]; i++)
    fuzz_walk_seed(100000 + i, raw[i], texts);
  test_watchdog_disarm();
  for (j = 0; j < FUZZ_LONG_ROWS; j++)
    free(longs[j]);
}

/* How many repeats of a separator one row may hold and still be matched, and
 * the first count that is not (the depth cap, in repeats of a two or three
 * byte unit), and a count far past what the old matcher survived. */
#define REPEATS_MATCHED 9990
#define REPEATS_COSTLY 10010
#define REPEATS_HUGE 60000
#define REAL_TAIL "t.c:1:2: error"

/** One walk of @pattern over one row made of @unit @copies times and @tail. */
static void check_repeats(const char *what, const char *pattern, const char *unit,
  int copies, int want_match)
{
  char *body = repeated(unit, copies);
  char *text = malloc(strlen(body) + strlen(REAL_TAIL) + 1);
  const char *texts[1];
  Rows r;
  WpeErrPat *p = accept(what, pattern);
  WpeErrWalk *w;
  WpeErrHit *hits;
  int nh = 0, end;

  strcpy(text, body);
  strcat(text, REAL_TAIL);
  texts[0] = text;
  r = rows_make(texts, 1, 1);
  if (p)
  {
    test_watchdog_arm(what, COST_ALARM_SECONDS);
    w = wpe_errpat_walk_begin(p, r.rows, r.n, NULL, 0);
    hits = calloc(1, sizeof *hits);
    end = wpe_errpat_walk_from(w, 0, hits, &nh);
    test_watchdog_disarm();
    chk_int(what, end, 0);
    chk_int(what, nh, want_match ? 1 : 0);
    chk_int(what, wpe_errpat_walk_costly_row(w), want_match ? -1 : 0);
    if (want_match && nh)
    {
      chk_int(what, hits[0].line, 1);
      chk_int(what, strncmp(hits[0].file, unit, strlen(unit)), 0);
    }
    if (nh)
      hit_free(&hits[0]);
    free(hits);
    wpe_errpat_walk_end(w);
    wpe_errpat_free(p);
  }
  rows_free(&r);
  free(body);
  free(text);
}

/* @scenario SC-errpat-04 */
/* A row that nests fewer than WPE_ERRPAT_DEPTH candidate positions is
 * matched as it always was; one that nests more is not recognised and makes
 * no entry, however far past the cap it goes (60,000 repeats crashed the
 * matcher the editor had). */
static void test_depth_cap(void)
{
  static const char *const gnu = GNU_TEXT;
  static const char *const star = "*: ${FILE}:${LINE}:*";

  check_repeats("gnu, below the cap", gnu, "x:", REPEATS_MATCHED, 1);
  check_repeats("one row pattern, below the cap", ONE_ROW_TEXT, "x:", REPEATS_MATCHED, 1);
  check_repeats("star pattern, below the cap", star, "a: ", REPEATS_MATCHED, 1);
  check_repeats("gnu, past the cap", gnu, "x:", REPEATS_COSTLY, 0);
  check_repeats("one row pattern, past the cap", ONE_ROW_TEXT, "x:", REPEATS_COSTLY, 0);
  check_repeats("star pattern, past the cap", star, "a: ", REPEATS_COSTLY, 0);
  check_repeats("gnu, far past the cap", gnu, "x:", REPEATS_HUGE, 0);
  check_repeats("star pattern, far past the cap", star, "a: ", REPEATS_HUGE, 0);
}

/* A real g++ row of 472 bytes: a candidate of a failed template call. */
#define GPP_CANDIDATE \
  "    \xe2\x80\xa2 candidate 1: \xe2\x80\x98std::map<_Key, _Tp, _Compare, _Alloc>::mapped_type& " \
  "std::map<_Key, _Tp, _Compare, _Alloc>::operator[](const key_type&) [with _Key = " \
  "std::vector<int>; _Tp = std::map<int, std::vector<std::map<int, int> > >; _Compare = " \
  "std::less<std::vector<int> >; _Alloc = std::allocator<std::pair<const std::vector<int>, " \
  "std::map<int, std::vector<std::map<int, int> > > > >; mapped_type = std::map<int, " \
  "std::vector<std::map<int, int> > >; key_type = std::vector<int>]\xe2\x80\x99"

/* @scenario SC-errpat-04 */
/* The most expensive real row measured (the pc pattern over a g++ candidate
 * row) is far inside the budget, and a 4,000 byte g++ error row is matched
 * by the GNU pattern with no note.  The budget read here is the library's
 * own, so lowering it fails these rows. */
static void test_real_rows_are_inside_the_budget(void)
{
  static const char *const texts[1] = {GPP_CANDIDATE};
  Rows r = rows_make(texts, 1, 1);
  WpeErrPat *p = accept("pc", PC_TEXT);
  WpeErrWalk *w;
  WpeErrHit hit;
  int nh = 0;

  if (p)
  {
    w = wpe_errpat_walk_begin(p, r.rows, r.n, NULL, 0);
    memset(&hit, 0, sizeof hit);
    wpe_errpat_walk_from(w, 0, &hit, &nh);
    printf("pc over a g++ candidate row: %ld steps\n", wpe_errpat_walk_peak_steps(w));
    chk_int("pc on a real g++ row is not costly", wpe_errpat_walk_costly_row(w), -1);
    chk_int("pc on a real g++ row stays under half the budget",
            wpe_errpat_walk_peak_steps(w) * 2 < WPE_ERRPAT_BUDGET, 1);
    if (nh)
      hit_free(&hit);
    wpe_errpat_walk_end(w);
    wpe_errpat_free(p);
  }
  rows_free(&r);
}

#define GPP_ARGUMENT "std::pair<int, std::string>, "
#define GPP_ARGUMENTS 135

/* @scenario SC-errpat-04 */
static void test_long_gpp_error_row(void)
{
  char *args = repeated(GPP_ARGUMENT, GPP_ARGUMENTS);
  char *text = malloc(strlen(args) + 128);
  const char *texts[1];
  Rows r;
  WpeErrPat *p = accept("gnu", GNU_TEXT);
  WpeErrWalk *w;
  WpeErrHit hit;
  int nh = 0;

  sprintf(text, "t.cc:10:5: error: no matching function for call to 'f(%s)'", args);
  chk_int("the row is about 4,000 bytes", strlen(text) > 3900 && strlen(text) < 4100, 1);
  texts[0] = text;
  r = rows_make(texts, 1, 1);
  if (p)
  {
    w = wpe_errpat_walk_begin(p, r.rows, r.n, NULL, 0);
    memset(&hit, 0, sizeof hit);
    wpe_errpat_walk_from(w, 0, &hit, &nh);
    chk_int("one entry", nh, 1);
    chk_int("line", hit.line, 10);
    chk_int("column", hit.x, 5);
    chk_int("not costly", wpe_errpat_walk_costly_row(w), -1);
    chk_int("far inside the budget", wpe_errpat_walk_peak_steps(w) * 10 < WPE_ERRPAT_BUDGET, 1);
    if (nh)
      hit_free(&hit);
    wpe_errpat_walk_end(w);
    wpe_errpat_free(p);
  }
  rows_free(&r);
  free(args);
  free(text);
}

/** The line of the one entry @pattern makes over @row; -1 when it makes none. */
static int line_of(const char *pattern, const char *row)
{
  const char *texts[1];
  Rows r;
  WpeErrPat *p = accept(pattern, pattern);
  WpeErrWalk *w;
  WpeErrHit hit;
  int nh = 0, line = -1;

  texts[0] = row;
  r = rows_make(texts, 1, 1);
  if (p)
  {
    w = wpe_errpat_walk_begin(p, r.rows, r.n, NULL, 0);
    memset(&hit, 0, sizeof hit);
    wpe_errpat_walk_from(w, 0, &hit, &nh);
    if (nh)
    {
      line = hit.line;
      hit_free(&hit);
    }
    wpe_errpat_walk_end(w);
    wpe_errpat_free(p);
  }
  rows_free(&r);
  return line;
}

/* @scenario SC-errpat-03 */
/* A line offset that does not fit a line number is held at the largest
 * number, never wrapped round to a small or negative one. */
static void test_line_offsets_do_not_wrap(void)
{
  chk_int("offset to the largest line", line_of("${FILE}:${LINE+2147483647}", "t.c:7"), INT_MAX);
  chk_int("offset past what an int holds", line_of("${FILE}:${LINE+99999999999}", "t.c:7"), INT_MAX);
  chk_int("offset near what a long holds", line_of("${FILE}:${LINE+9223372036854775806}", "t.c:7"), INT_MAX);
  chk_int("offset past what a long holds", line_of("${FILE}:${LINE+99999999999999999999}", "t.c:7"), INT_MAX);
  chk_int("a negative offset subtracted, past what a long holds",
          line_of("${FILE}:${LINE--99999999999999999999}", "t.c:7"), INT_MAX);
  chk_int("offset past 32 bits", line_of("${FILE}:${LINE+4294967297}", "t.c:7"), INT_MAX);
  chk_int("a small offset", line_of("${FILE}:${LINE+2}", "t.c:7"), 9);
  chk_int("a line pushed below zero makes no entry", line_of("${FILE}:${LINE-99999999999}", "t.c:7"), -1);
}

/* @scenario SC-errpat-07 */
/* An unclosed "[!" in a pattern nobody checked answers "no match" and stays
 * inside the row, from every start. */
static void test_unclosed_negated_class(void)
{
  static const char *const texts[] = {":", "hello.pas(6,10) Error: x", "a\\"};
  WpeErrPat *u = wpe_errpat_unchecked("[a-c]*?[![!\n\\[\r*[!");
  int ended;
  Rows r;

  for (ended = 0; ended < 2; ended++)
  {
    r = rows_make(texts, 3, ended);
    if (u)
      fuzz_walk(1, u, &r);
    rows_free(&r);
  }
  wpe_errpat_free(u);
}

int main(void)
{
  test_depth_cap();
  test_real_rows_are_inside_the_budget();
  test_long_gpp_error_row();
  test_line_offsets_do_not_wrap();
  test_unclosed_negated_class();
  test_golden_table();
  test_walk_always_advances();
  test_empty_row_does_not_continue();
  test_marker_past_the_row();
  test_backtracking_is_bounded();
  test_long_rows_are_cheap();
  test_fuzz_walk();
  test_evidence_walks();
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
