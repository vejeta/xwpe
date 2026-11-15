/* we_errpat.c -- the one owner of a compiler's message pattern.
 *
 * Reading the pattern text: the text is cut into rows exactly as the editor
 * has always cut it, each row is walked piece by piece with the same scan
 * the matcher uses, and a text that would make the matcher run off the end
 * of a row is refused with a reason.
 *
 * Matching: the matcher the editor has always had, moved here with its
 * answers unchanged, reads the rows of Messages one at a time, and a walk
 * over those rows turns the messages the pattern recognises into entries of
 * the error list.  Every step is bounded by the rows and by a budget.
 *
 * Libc only.
 */
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "we_errpat.h"

/* Spare bytes a walk's scratch has beyond the longest row, the widest
   pattern row and the source name: the marks a variable adds to its text
   and the terminator. */
#define SCRATCH_SLACK 8
/* How much of a token a diagnostic quotes. */
#define NEAR_BYTES 24
/* A variable name a note quotes. */
#define NAME_BYTES 16
/* The column of a warning test written ${?*:text}: anywhere in the row. */
#define COLUMN_ANYWHERE (-1)

struct WpeErrPat
{
 char *rows[WPE_ERRPAT_ROWS];
 int nrows;
 char **mtext;
 int *mcol;
 int nmark;
 int names_file;
 char notes[WPE_ERRPAT_NOTES];
 int nnotes;
};

/* What one walk over a pattern remembers. */
typedef struct
{
 WpeErrPat *p;
 int row_no;
 int has_line;
 int quiet;
 /* The tails of the row a second walk has already read, by their length:
    the walk from a tail reads the same pieces every time it starts there. */
 unsigned char *walked;
 char diag[WPE_ERRPAT_DIAG];
} Lex;

/* ------------------------------------------------------------------ */
/* the shared scan                                                      */

int wpe_errpat_literal_end(const char *a, int n, int *cut)
{
 int bsl = 0;

 *cut = 0;
 for (;; n++)
 {
  if (bsl)
  {
   if (!a[n])
   {
    *cut = 1;
    return n;
   }
  }
  else if (!a[n] || a[n] == '*' || a[n] == '?' || a[n] == '[' ||
    (a[n] == '$' && a[n + 1] == '{'))
   return n;
  bsl = a[n] == '\\' ? !bsl : 0;
 }
}

void wpe_errpat_file_token(char *out, const char *s)
{
 size_t len;

 while (isspace((unsigned char)*s))
  s++;
 len = strlen(s);
 while (len > 0 && isspace((unsigned char)s[len - 1]))
  len--;
 memcpy(out, s, len);
 out[len] = '\0';
 len = strlen(out);
 if (len >= 2 && out[0] == '"' && out[len - 1] == '"')
 {
  memmove(out, out + 1, len - 2);
  out[len - 2] = '\0';
 }
}

/* ------------------------------------------------------------------ */
/* diagnostics and notes                                                */

/**
 * set_diag_near - Say why a text was refused, quoting the token.
 * @what: the reason; @at: where the token starts.
 *
 * Control bytes are shown as blanks so the reason stays on one status row.
 */
static void set_diag_near(Lex *cx, const char *what, const char *at)
{
 char quote[NEAR_BYTES + 1];
 int i;

 for (i = 0; i < NEAR_BYTES && at[i]; i++)
  quote[i] = (unsigned char)at[i] < ' ' ? ' ' : at[i];
 quote[i] = '\0';
 snprintf(cx->diag, WPE_ERRPAT_DIAG, "%s near \"%s\"", what, quote);
}

/** Refuse with a reason that quotes a token; always returns 1. */
static int refuse_near(Lex *cx, const char *what, const char *at)
{
 set_diag_near(cx, what, at);
 return 1;
}

/**
 * add_note - Record how the editor reads a part of the pattern.
 *
 * Notes tell the user about a reading that differs from what was written.
 * Once the notes buffer is full the rest is only counted.
 */
static void add_note(WpeErrPat *p, const char *fmt, ...)
{
 char one[WPE_ERRPAT_NOTES];
 size_t have = strlen(p->notes);
 va_list ap;

 va_start(ap, fmt);
 vsnprintf(one, sizeof one, fmt, ap);
 va_end(ap);
 p->nnotes++;
 if (have + strlen(one) + 3 > sizeof p->notes)
  return;
 if (have)
  strcat(p->notes, "; ");
 strcat(p->notes, one);
}

/* ------------------------------------------------------------------ */
/* variables                                                            */

typedef enum { VAR_UNKNOWN, VAR_FILE, VAR_LINE, VAR_COLUMN, VAR_CMPTEXT } VarKind;

/**
 * var_name_is - Does the token start with @name and go on with no
 * letter or digit?  This is the test the matcher makes, so "FILE+3" reads
 * as FILE and "FILEX" reads as nothing.
 */
static int var_name_is(const char *var, int len, const char *name)
{
 int nl = (int)strlen(name);

 if (len < nl || strncmp(var, name, nl) != 0)
  return 0;
 return len == nl || !isalnum((unsigned char)var[nl]);
}

/** Which variable a ${...} token names, the way the matcher reads it. */
static VarKind var_kind(const char *var, int len)
{
 if (var_name_is(var, len, "FILE"))
  return VAR_FILE;
 if (var_name_is(var, len, "CMPTEXT"))
  return VAR_CMPTEXT;
 if (var_name_is(var, len, "LINE"))
  return VAR_LINE;
 if (var_name_is(var, len, "COLUMN"))
  return VAR_COLUMN;
 return VAR_UNKNOWN;
}

/**
 * note_variable - Take note of a ${...} token met while walking a row.
 * @var: the text between "${" and "}", @len its length.
 *
 * An unknown name is read as '*' (it matches anything and takes nothing);
 * the user is told so, which is how a typo such as ${CMPTXT} shows up.
 */
static void note_variable(Lex *cx, const char *var, int len)
{
 VarKind kind;
 char name[NAME_BYTES + 1];
 int i;

 if (cx->quiet)
  return;
 kind = var_kind(var, len);
 if (kind == VAR_LINE)
  cx->has_line = 1;
 if (kind != VAR_UNKNOWN)
  return;
 for (i = 0; i < NAME_BYTES && i < len; i++)
  name[i] = var[i];
 name[i] = '\0';
 add_note(cx->p, "unknown variable \"%s\" read as *", name);
}

/* ------------------------------------------------------------------ */
/* walking one row                                                      */

/** The index of the '}' that ends the "${" at @a, or -1. */
static int brace_end(const char *a)
{
 int e;

 for (e = 2; a[e]; e++)
  if (a[e] == '}')
   return e;
 return -1;
}

/**
 * class_close - Find where the [...] class at @a closes.
 * @alt: set to the other index the matcher closes it at for one row byte,
 * or 0.  The byte after "[!" is content even when it is ']', yet the
 * matcher also closes a "[!]...]" class there when the row byte is that
 * ']', so both are walked.  A "[]...]" class has no second closing: the
 * matcher fails it when the row byte is ']'.
 * Return: the index of the closing ']', or -1 when the class is unclosed.
 */
static int class_close(const char *a, int *alt)
{
 int neg = a[1] == '!';
 int start = neg ? 3 : 2;
 int i;

 *alt = 0;
 if (neg && a[2] == ']')
  *alt = 2;
 for (i = 1; a[i]; i++)
  if (i >= start && a[i] == ']')
   return i;
 return -1;
}

static int lex_row(char *s, Lex *cx);

/**
 * walked_before - Has a second walk already read the tail of the row at @t?
 *
 * Every "[!]...]" class can send the matcher a second way through the rest
 * of the row, so a row of many such classes has more ways through it than
 * can be walked one by one.  Each way starts again at a tail of the row, and
 * there are no more tails than bytes, so each tail is read once and the
 * whole row is checked however many classes it holds.  A tail is always the
 * end of the row as written: the '?' a walk takes out lies before it, so a
 * piece is looked up only when no '?' was just taken out of it.
 * Return: 1 when @t was read before; else 0, and @t counts as read.
 */
static int walked_before(Lex *cx, const char *t)
{
 size_t left = strlen(t);

 if (cx->walked[left])
  return 1;
 cx->walked[left] = 1;
 return 0;
}

/**
 * lex_copy - Walk a copy of @text as a row, so the stored row keeps its '?'.
 * @quiet: 1 when the walk only looks for refusals and takes no notes.
 * Return: 1 when the walk refuses the row (the reason is in @cx).
 */
static int lex_copy(Lex *cx, const char *text, int quiet)
{
 char *copy = malloc(strlen(text) + 1);
 int saved = cx->quiet;
 int refused;

 if (!copy)
  return refuse_near(cx, "out of memory", "");
 strcpy(copy, text);
 cx->quiet = quiet;
 refused = lex_row(copy, cx);
 cx->quiet = saved;
 free(copy);
 return refused;
}

/**
 * lex_class - Check the [...] class that starts at @a.
 * @next: set to how far the matcher moves on after the class and the
 * literal run behind it, given the run's end @n.
 * Return: 1 and a reason when the class is unclosed or the walk of its
 * other closing refuses, else 0.
 */
static int lex_class(Lex *cx, const char *a, int n, int *next)
{
 int alt, q = class_close(a, &alt);
 int from;

 if (q < 0)
  return refuse_near(cx, "unclosed [", a);
 if (alt > 0)
 {
  from = n > alt + 1 ? n : alt + 1;
  if (lex_copy(cx, a + from, 1))
   return 1;
 }
 *next = n > q + 1 ? n : q + 1;
 return 0;
}

/**
 * piece_head - Read the head of the piece that starts at @a.
 * @k: out, the length of the head (a ${...} token or a star), 0 for a plain
 * byte; @n: out, where the scan of the literal run behind it starts.
 * Return: 1 and a reason when the head is a ${ with no }, else 0.
 */
static int piece_head(Lex *cx, const char *a, int fresh, int *k, int *n)
{
 int e;

 *k = 0;
 *n = 1;
 if (a[0] == '$' && a[1] == '{')
 {
  e = brace_end(a);
  if (e < 0)
   return refuse_near(cx, "unclosed ${", a);
  if (fresh)
   note_variable(cx, a + 2, e - 2);
  *k = e + 1;
  *n = a[*k] == '\\' ? *k : *k + 1;
 }
 else if (a[0] == '*' && a[1] != '\\')
 {
  *k = 1;
  *n = 2;
 }
 return 0;
}

/**
 * lex_row - Walk a row piece by piece as the matcher will.
 * @s: the row, which the walk may shorten (a '?' behind a variable or a
 * star is taken out, as the matcher does).
 *
 * The user gains a reason for every row the matcher could not read to its
 * end: a ${ with no }, a [ with no ], a backslash with nothing after it.
 * Return: 1 with the reason in @cx when the row is cut, else 0.
 */
static int lex_row(char *s, Lex *cx)
{
 int p = 0, k, n, q, cut, again = 0, fresh, headed;
 char *a;

 while (s[p])
 {
  a = s + p;
  fresh = !again;
  again = 0;
  if (cx->quiet && fresh && walked_before(cx, a))
   return 0;
  if (a[0] == '*' && !a[1])
   break;
  if (a[0] == '*' && (a[1] == '*' || a[1] == '$'))
  {
   p++;
   continue;
  }
  if (piece_head(cx, a, fresh, &k, &n))
   return 1;
  headed = a[0] == '*' || a[0] == '$';
  if (a[0] == '$' && a[1] == '{' && !a[k])
   break;
  if (headed && a[k] == '[' && class_close(a + k, &q) < 0)
   return refuse_near(cx, "unclosed [", a + k);
  if (a[0] == '[' && class_close(a, &q) < 0)
   return refuse_near(cx, "unclosed [", a);
  n = wpe_errpat_literal_end(a, n, &cut);
  if (cut)
  {
   snprintf(cx->diag, WPE_ERRPAT_DIAG,
     "nothing after \\ at the end of row %d", cx->row_no);
   return 1;
  }
  if (headed && a[k] == '?')
  {
   memmove(a + k, a + k + 1, strlen(a + k + 1) + 1);
   again = 1;
   continue;
  }
  if (headed)
   p += a[k] == '[' ? k : n;
  else if (a[0] == '[')
  {
   if (lex_class(cx, a, n, &q))
    return 1;
   p += q;
  }
  else
   p += n;
 }
 return 0;
}

/* ------------------------------------------------------------------ */
/* cutting the text into rows                                           */

/** Is @c a row separator?  A carriage return counts as a line end. */
static int is_separator(char c)
{
 return c == '\r' || c == '\n';
}

/** Does the text at @j close a "${?" that began at @j - 2? */
static int at_marker(const char *text, int j)
{
 return j > 1 && text[j] == '?' && text[j - 1] == '{' && text[j - 2] == '$';
}

/**
 * add_marker - Keep one warning test.
 * @col: the column, COLUMN_ANYWHERE for '*'.
 */
static int add_marker(WpeErrPat *p, const char *text, int len, int col)
{
 char **mt = realloc(p->mtext, (p->nmark + 1) * sizeof *mt);
 int *mc;
 char *copy;

 if (!mt)
  return 1;
 p->mtext = mt;
 mc = realloc(p->mcol, (p->nmark + 1) * sizeof *mc);
 if (!mc)
  return 1;
 p->mcol = mc;
 copy = malloc(len + 1);
 if (!copy)
  return 1;
 memcpy(copy, text, len);
 copy[len] = '\0';
 p->mtext[p->nmark] = copy;
 p->mcol[p->nmark] = col;
 p->nmark++;
 return 0;
}

/**
 * note_marker_column - Tell when the column of a warning test is no number.
 * @spec: the text between "${?" and the ':'; @len its length.
 *
 * The editor reads such a column as the number atoi() finds in it, 0 when
 * there is none, so the test would only fire at that column.
 */
static void note_marker_column(Lex *cx, const char *spec, int len, int col)
{
 int i, digits = len > 0;

 if (spec[0] == '*')
  return;
 for (i = 0; i < len; i++)
  if (!isdigit((unsigned char)spec[i]))
   digits = 0;
 if (digits)
  return;
 add_note(cx->p, "warning test column is not a number, read as %d", col);
}

/**
 * read_marker - Take the "${?N:text}" that ends at text[*j] out of a row.
 * @row: the row being built, @l its length so far, which includes the
 * "${?"; @j: in, where the '?' is; out, where the '}' is.
 *
 * Return: 0 when read; 1 when the text is cut (no ':' or no '}'), with the
 * row cut before the "${" so the caller can walk what came before.
 */
static int read_marker(Lex *cx, const char *text, int *j, char *row, int *l)
{
 int colon, end, col;

 colon = *j + 1;
 while (text[colon] && text[colon] != ':')
  colon++;
 end = colon;
 while (text[end] && text[end] != '}')
  end++;
 if (!text[colon] || !text[end])
 {
  row[*l - 2] = '\0';
  return 1;
 }
 col = text[*j + 1] == '*' ? COLUMN_ANYWHERE : atoi(text + *j + 1);
 if (!cx->quiet)
  note_marker_column(cx, text + *j + 1, colon - *j - 1, col);
 if (add_marker(cx->p, text + colon + 1, end - colon - 1, col))
  return 1;
 *j = end;
 *l -= 3;
 return 0;
}

/**
 * cut_row - Build the next row from @text, starting at @*jp.
 * @row: room for strlen(@text) + 2 bytes.
 *
 * The splitter the editor has always had, byte for byte: a '\r' or a '\n'
 * ends a row, and two of them in a row (\r\n, \n\r, \r\r, \n\n) end it with
 * a literal '\n' kept, which is how a pattern's row matches the line end of
 * a Messages row; empty rows stay; the text of a warning test may run over
 * a separator.
 * Return: 0 when the row is built; 1 when a warning test is cut, the reason
 * being in @cx.
 */
static int cut_row(Lex *cx, const char *text, int *jp, char *row)
{
 int j = *jp, l = 0;

 for (;; j++, l++)
 {
  row[l] = text[j];
  if (!text[j])
   break;
  if (at_marker(text, j))
  {
   if (read_marker(cx, text, &j, row, &l))
   {
    *jp = j - 2;
    return 1;
   }
  }
  else if (is_separator(text[j]))
  {
   if (is_separator(text[j + 1]))
   {
    row[l] = '\n';
    row[l + 1] = '\0';
    j++;
   }
   else
    row[l] = '\0';
   j++;
   break;
  }
 }
 *jp = j;
 return 0;
}

/**
 * refuse_cut_marker - Give the reason for a warning test that is cut.
 * @text: the whole pattern, @at where its "${" starts; @row the row up to
 * that point, walked first so the reason comes in text order.
 */
static void refuse_cut_marker(Lex *cx, const char *text, int at, const char *row)
{
 if (lex_copy(cx, row, 0))
  return;
 set_diag_near(cx, "unclosed ${?", text + at);
}

/** Free what a pattern holds. */
void wpe_errpat_free(WpeErrPat *p)
{
 int i;

 if (!p)
  return;
 for (i = 0; i < WPE_ERRPAT_ROWS; i++)
  free(p->rows[i]);
 for (i = 0; i < p->nmark; i++)
  free(p->mtext[i]);
 free(p->mtext);
 free(p->mcol);
 free(p);
}

/**
 * read_rows - Cut @text into rows and walk each one.
 * Return: 0 when every row is read; 1 with the reason in @cx.
 */
static int read_rows(Lex *cx, const char *text)
{
 WpeErrPat *p = cx->p;
 size_t len = strlen(text);
 int j = 0, n;
 char *row;

 for (n = 0; n < WPE_ERRPAT_ROWS && text[j]; n++)
 {
  row = malloc(len + 2);
  if (!row)
   return refuse_near(cx, "out of memory", "");
  p->rows[n] = row;
  cx->row_no = n + 1;
  memset(cx->walked, 0, len + 2);
  if (cut_row(cx, text, &j, row))
  {
   refuse_cut_marker(cx, text, j, row);
   return 1;
  }
  p->nrows = n + 1;
  if (lex_copy(cx, row, 0))
   return 1;
 }
 if (text[j])
  add_note(p, "rows after the fourth are ignored");
 return 0;
}

WpeErrPat *wpe_errpat_unchecked(const char *text)
{
 WpeErrPat *p = calloc(1, sizeof *p);
 Lex cx;
 size_t len = strlen(text);
 int j = 0, n;
 char *row;

 if (!p)
  return NULL;
 memset(&cx, 0, sizeof cx);
 cx.p = p;
 cx.quiet = 1;
 for (n = 0; n < WPE_ERRPAT_ROWS && (text[j] || n == 0); n++)
 {
  row = calloc(len + 2, 1);
  if (!row)
   break;
  p->rows[n] = row;
  p->nrows = n + 1;
  if (cut_row(&cx, text, &j, row))
   break;
 }
 p->names_file = strstr(text, "${FILE}") != NULL;
 return p;
}

WpeErrPat *wpe_errpat_compile(const char *text, char diag[WPE_ERRPAT_DIAG])
{
 WpeErrPat *p = calloc(1, sizeof *p);
 Lex cx;

 diag[0] = '\0';
 if (!p)
  return NULL;
 memset(&cx, 0, sizeof cx);
 cx.p = p;
 cx.walked = malloc(strlen(text) + 2);
 if (!cx.walked)
 {
  strcpy(diag, "out of memory");
  wpe_errpat_free(p);
  return NULL;
 }
 if (read_rows(&cx, text))
 {
  strcpy(diag, cx.diag);
  free(cx.walked);
  wpe_errpat_free(p);
  return NULL;
 }
 free(cx.walked);
 if (!cx.has_line)
 {
  strcpy(diag, text[0] ? "no ${LINE} in the pattern" : "empty pattern");
  wpe_errpat_free(p);
  return NULL;
 }
 p->names_file = strstr(text, "${FILE}") != NULL;
 return p;
}

int wpe_errpat_names_file(const WpeErrPat *p)
{
 return p->names_file;
}

int wpe_errpat_row_count(const WpeErrPat *p)
{
 return p->nrows;
}

const char *wpe_errpat_notes(const WpeErrPat *p)
{
 return p->notes;
}

int wpe_errpat_note_count(const WpeErrPat *p)
{
 return p->nnotes;
}

const char *wpe_errpat_row(const WpeErrPat *p, int i)
{
 if (i < 0 || i >= p->nrows)
  return NULL;
 return p->rows[i];
}

int wpe_errpat_marker_count(const WpeErrPat *p)
{
 return p->nmark;
}

const char *wpe_errpat_marker_text(const WpeErrPat *p, int i)
{
 if (i < 0 || i >= p->nmark)
  return NULL;
 return p->mtext[i];
}

int wpe_errpat_marker_column(const WpeErrPat *p, int i)
{
 if (i < 0 || i >= p->nmark)
  return COLUMN_ANYWHERE;
 return p->mcol[i];
}

/* ------------------------------------------------------------------ */
/* pass B: matching one row                                             */


/* What the matcher answers when a row matches to its end, and when the
   pattern's last piece takes the rest of the row. */
#define MATCHED 2

/* The scratch and the verdict of matching one row against one pattern row.
   file, search and cmp are what the pattern's variables took from the row. */
typedef struct
{
 char *search, *file, *cmp;
 int y, x;
 long steps;
 int depth;
 int exhausted;
} Match;

static int match(Match *m, const char *a, const char *b, const char *c);

/** A copy of s[from..to), or NULL when memory is short. */
static char *copy_range(const char *s, int from, int to)
{
 char *copy = malloc((size_t)(to - from) + 1);

 if (!copy)
  return NULL;
 memcpy(copy, s + from, (size_t)(to - from));
 copy[to - from] = '\0';
 return copy;
}

/** A copy of @a without the byte at @k, or NULL when memory is short. */
static char *copy_without(const char *a, int k)
{
 size_t len = strlen(a);
 char *copy = malloc(len);

 if (!copy)
  return NULL;
 memcpy(copy, a, (size_t)k);
 memcpy(copy + k, a + k + 1, len - (size_t)k);
 return copy;
}

/**
 * apply_offset - Move a line or a column by the "+N" or "-N" a token ends in.
 * @v: the value; @suffix: the text after the variable's name.  A token with
 * no sign leaves the value alone.  The result never wraps around.
 */
static void apply_offset(int *v, const char *suffix)
{
 long long r = *v;
 long long delta;

 if (suffix[0] != '+' && suffix[0] != '-')
  return;
 delta = strtol(suffix + 1, NULL, 10);
 if (delta > INT_MAX)
  delta = INT_MAX;
 if (delta < INT_MIN)
  delta = INT_MIN;
 if (suffix[0] == '+')
  r += delta;
 else if (suffix[0] == '-')
  r -= delta;
 else
  return;
 if (r > INT_MAX)
  r = INT_MAX;
 if (r < INT_MIN)
  r = INT_MIN;
 *v = (int)r;
}

/* The names and spellings of the tokens the matcher reads inside ${...}. */
#define LINE_NAME "LINE"
#define COLUMN_NAME "COLUMN"
#define BEFORE_SPELLING "=BEFORE"
#define AFTER_SPELLING "=AFTER"
#define PREVIOUS_SPELLING "=PREVIOUS?"
/* The first byte of the search text says what it is for: text that starts
   at the column, text that ends there, the compiler's own text. */
#define SEARCH_BEFORE 'B'
#define SEARCH_AFTER 'A'
#define SEARCH_PREVIOUS 'P'

/** The length of a token's spelling. */
static int spelling_len(const char *spelling)
{
 return (int)strlen(spelling);
}

/** ${LINE}: the digits the row holds there.  Return: 1 when it is no number. */
static int take_line(Match *m, const char *var, const char *str)
{
 if (!isdigit((unsigned char)str[0]))
  return 1;
 m->y = atoi(str);
 apply_offset(&m->y, var + spelling_len(LINE_NAME));
 return 0;
}

/**
 * take_column_text - ${COLUMN=PREVIOUS?STR}: the column is where STR starts.
 * @var: the token; @rest: out, where its "+N" or "-N" begins.
 * Return: 1 when the row holds no text there.
 */
static int take_column_text(Match *m, const char *var, const char *str,
  const char **rest)
{
 const char *cp;
 int i;

 int head = spelling_len(COLUMN_NAME) + spelling_len(PREVIOUS_SPELLING);

 if (!str[0])
  return 1;
 for (i = 0; (m->search[i] = var[head + i]) && m->search[i] != '+' && m->search[i] != '-'; i++)
  ;
 m->search[i] = '\0';
 *rest = var + head + i;
 cp = strstr(str, m->search);
 m->x = cp ? (int)(cp - str) : 0;
 m->search[0] = SEARCH_PREVIOUS;
 m->search[1] = '\0';
 return 0;
}

/**
 * take_column - ${COLUMN}, ${COLUMN=BEFORE}, ${COLUMN=AFTER} or
 * ${COLUMN=PREVIOUS?STR}: where in the source line the error is.
 * Return: 1 when the row holds no column there.
 */
static int take_column(Match *m, const char *var, const char *str)
{
 const char *name_end = var + spelling_len(COLUMN_NAME);
 const char *rest = name_end;

 if (!strncmp(name_end, BEFORE_SPELLING, spelling_len(BEFORE_SPELLING)))
 {
  m->search[0] = SEARCH_BEFORE;
  strcpy(m->search + 1, str);
  m->x = 0;
  rest = name_end + spelling_len(BEFORE_SPELLING);
 }
 else if (!strncmp(name_end, AFTER_SPELLING, spelling_len(AFTER_SPELLING)))
 {
  m->search[0] = SEARCH_AFTER;
  strcpy(m->search + 1, str);
  m->x = (int)strlen(str);
  rest = name_end + spelling_len(AFTER_SPELLING);
 }
 else if (!strncmp(name_end, PREVIOUS_SPELLING, spelling_len(PREVIOUS_SPELLING)))
 {
  if (take_column_text(m, var, str, &rest))
   return 1;
 }
 else if (!isdigit((unsigned char)str[0]))
  return 1;
 else
 {
  m->x = atoi(str);
  m->search[0] = '\0';
 }
 apply_offset(&m->x, rest);
 return 0;
}

/**
 * take_variable - Keep the text a ${...} token matched.
 * @var: the text between "${" and "}"; @str: what the row holds there.
 * Return: 1 when the text cannot be what the variable names (a line that is
 * no number), so the row does not match.
 */
static int take_variable(Match *m, const char *var, const char *str)
{
 int len = (int)strlen(var);

 switch (var_kind(var, len))
 {
 case VAR_FILE:
  wpe_errpat_file_token(m->file, str);
  return 0;
 case VAR_CMPTEXT:
  strcpy(m->cmp, str);
  return 0;
 case VAR_LINE:
  return take_line(m, var, str);
 case VAR_COLUMN:
  return take_column(m, var, str);
 default:
  return 0;
 }
}

/**
 * positive_class_end - Find where a [abc] class closes when @b[0] is in it.
 * @a: the class; @fail: out, what the matcher answers when it is not.
 * Return: the index of the closing ']', or -1 when @b[0] is not in the class
 * or the class is not closed.
 */
static int positive_class_end(const char *a, const char *b, int *fail)
{
 int k;

 *fail = 0;
 for (k = 1; a[k] && (a[k] != ']' || k == 1) && a[k] != b[0]; k++)
  if (a[k + 1] == '-' && b[0] >= a[k] && b[0] <= a[k + 2])
   break;
 if (a[k] == ']' || a[k] == '\0')
  return -1;
 for (; a[k] && a[k] != ']'; k++)
  ;
 if (!a[k])
  return -1;
 return k;
}

/**
 * negated_class_end - Find where a [!abc] class closes when @b[0] is not in it.
 * @a: the class; @fail: out, what the matcher answers when it is.
 * Return: the index of the closing ']', or -1.
 */
static int negated_class_end(const char *a, const char *b, int *fail)
{
 int k;

 *fail = -b[0];
 for (k = 2; a[k] && (a[k] != ']' || k == 2) && a[k] != b[0]; k++)
  if (a[k + 1] == '-' && b[0] >= a[k] && b[0] <= a[k + 2])
   return -1;
 if (a[k] != ']')
  return -1;
 return k;
}

/** Find where the class at @a closes for the row byte @b[0] (see above). */
static int class_match_end(const char *a, const char *b, int *fail)
{
 if (a[1] == '!')
  return negated_class_end(a, b, fail);
 return positive_class_end(a, b, fail);
}

/**
 * match_literal - Match the next @n bytes of the pattern exactly, then go on.
 * A run that is not positive has nothing to compare and goes straight on.
 */
static int match_literal(Match *m, const char *a, const char *b, const char *c, int n)
{
 if (n <= 0)
  return match(m, a, b, c);
 if (strncmp(a, b, (size_t)n) != 0)
  return 0;
 return match(m, a + n, b + n, c + n);
}

/**
 * match_plain - Match a piece that starts with a plain byte, '?' or a class.
 * A '$' that is not followed by '{' is a plain byte.
 */
static int match_plain(Match *m, const char *a, const char *b, const char *c)
{
 int n, k, fail, cut;

 n = wpe_errpat_literal_end(a, 1, &cut);
 if (a[0] == '?')
  return match_literal(m, a + 1, b + 1, c, n - 1);
 if (a[0] == '[')
 {
  k = class_match_end(a, b, &fail);
  if (k < 0)
   return fail;
  return match_literal(m, a + k + 1, b + 1, c, n - (k + 1));
 }
 return match_literal(m, a, b, c, n);
}

/**
 * match_skip_one - The '?' behind a variable or a star: take it out of the
 * pattern and let the row's next byte go first.
 */
static int match_skip_one(Match *m, const char *a, int k, const char *b, const char *c)
{
 char *cp = copy_without(a, k);
 int r;

 if (!cp)
  return 0;
 r = match(m, cp, b + 1, c + 1);
 free(cp);
 return r;
}

/**
 * match_class_after - A variable or a star followed by a [...] class: try the
 * class at each byte of the row, and keep what lies before the hit.
 */
static int match_class_after(Match *m, const char *a, int k, const char *var,
  const char *b, const char *c)
{
 int i, r = 0;
 char *str;

 for (i = 0; b[i] && !(r = match(m, a + k, b + i, c + i)); i++)
  ;
 if (!b[i])
  return 0;
 if (var)
 {
  str = copy_range(b, 0, i);
  if (!str)
   return 0;
  take_variable(m, var, str);
  free(str);
 }
 return r;
}

/**
 * match_found_run - A variable or a star followed by a literal run: find the
 * run in the row, keep what lies before it, and match the rest of the
 * pattern after it; when that fails and no error was made, look for the run
 * again further on.
 * @k: where the run starts in @a; @n: where it ends.
 */
static int match_found_run(Match *m, const char *a, int k, int n, const char *var,
  const char *b, const char *c)
{
 char *run, *str;
 const char *cp;
 int i;

 n -= k;
 run = copy_range(a, k, k + n);
 if (!run)
  return 0;
 cp = strstr(b, run);
 free(run);
 if (!cp)
  return 0;
 if (var)
 {
  str = copy_range(c, 0, (int)(cp - c));
  if (!str)
   return 0;
  i = take_variable(m, var, str);
  free(str);
  if (i)
   return 0;
 }
 if (!a[k + n] && !cp[n])
  return MATCHED;
 if (!a[k + n])
  return match(m, a, cp + 1, cp + 1);
 i = match(m, a + k + n, cp + n, cp + n);
 if (i)
  return i;
 if (m->file[0] && m->y > -1)
  return 0;
 if (var)
  return match(m, a, cp + 1, c);
 return match(m, a, cp + 1, cp + 1);
}

/**
 * match_wild - A piece that starts with a variable or a star.
 * @k: how long the head is; @n: where the scan of the literal behind it
 * starts; @var: the variable's text, NULL for a star.
 */
static int match_wild(Match *m, const char *a, int k, int n, const char *var,
  const char *b, const char *c)
{
 int cut;

 n = wpe_errpat_literal_end(a, n, &cut);
 if (a[k] == '?')
  return match_skip_one(m, a, k, b, c);
 if (a[k] == '[')
  return match_class_after(m, a, k, var, b, c);
 return match_found_run(m, a, k, n, var, b, c);
}

/** A piece that starts with "${": cut the variable out and match what follows. */
static int match_variable(Match *m, const char *a, const char *b, const char *c)
{
 int k, n, r;
 char *var;

 for (k = 2; a[k] && a[k] != '}'; k++)
  ;
 var = copy_range(a, 2, k);
 if (!var)
  return 0;
 if (a[k])
  k++;
 if (!a[k])
  r = !take_variable(m, var, b);
 else
 {
  n = a[k] == '\\' ? k : k + 1;
  r = match_wild(m, a, k, n, var, b, c);
 }
 free(var);
 return r;
}

/**
 * match_step - Match the pattern piece at @a against the row at @b.
 * @c: where the text a variable takes starts, which lags behind @b after a
 * '?' went first.
 * Return: 0 when it does not match, MATCHED or another non-zero value when
 * it does.  A class that excludes the byte answers with that byte negated,
 * a non-zero value the walk has always read as "matched".
 */
static int match_step(Match *m, const char *a, const char *b, const char *c)
{
 if (c > b)
  return 0;
 if (a[0] == '*' && !a[1])
  return MATCHED;
 if (!a[0] && !b[0])
  return MATCHED;
 if (!a[0] || !b[0])
  return 0;
 if (a[0] == '*' && (a[1] == '*' || a[1] == '$'))
  return match(m, a + 1, b, c);
 if (a[0] == '$' && a[1] == '{')
  return match_variable(m, a, b, c);
 if (a[0] == '*' && a[1] != '\\')
  return match_wild(m, a, 1, 2, NULL, b, c);
 if (a[0] == '*')
  return match_wild(m, a, 0, 1, NULL, b, c);
 return match_plain(m, a, b, c);
}

/**
 * match - Match one pattern piece, within the step and depth budget of the row.
 * A row that spends the budget is marked and answers "no match" from then on,
 * so a pattern that backtracks without end costs the user one note, not the
 * editor.
 */
static int match(Match *m, const char *a, const char *b, const char *c)
{
 int r;

 if (m->exhausted)
  return 0;
 if (--m->steps < 0 || m->depth >= WPE_ERRPAT_DEPTH)
 {
  m->exhausted = 1;
  return 0;
 }
 m->depth++;
 r = match_step(m, a, b, c);
 m->depth--;
 return r;
}

/* ------------------------------------------------------------------ */
/* pass B: what a matched message says                                  */

/** The word a compiler's row says when the pattern carries no warning test. */
#define WARNING_WORD "warning"

/**
 * marker_matches - Does warning test @i hold of this row?
 * @row: the Messages row, terminated.  A test at a column holds only when
 * the row reaches that column, and never reads past the row's end.
 */
static int marker_matches(const WpeErrPat *p, int i, const char *row)
{
 int col = p->mcol[i];
 const char *text = p->mtext[i];

 if (col == COLUMN_ANYWHERE)
  return strstr(row, text) != NULL;
 if (col < 0)
  return 0;
 return (int)strlen(row) > col && !strncmp(row + col, text, strlen(text));
}

/** Does any of the pattern's warning tests hold of the row? */
static int row_marked(const WpeErrPat *p, const char *row)
{
 int i;

 for (i = 0; i < p->nmark; i++)
  if (marker_matches(p, i, row))
   return 1;
 return 0;
}

/**
 * row_kind - Did the compiler call this row a warning?
 * A pattern with no warning test falls back to the word itself, in any case,
 * so the user sees which rows only advise.
 */
static int row_kind(const WpeErrPat *p, const char *row)
{
 size_t n = strlen(WARNING_WORD);
 const char *s;

 if (row_marked(p, row))
  return WPE_ERRPAT_WARNING;
 if (p->nmark > 0)
  return WPE_ERRPAT_ERROR;
 for (s = row; *s; s++)
  if (!strncasecmp(s, WARNING_WORD, n))
   return WPE_ERRPAT_WARNING;
 return WPE_ERRPAT_ERROR;
}

/* ------------------------------------------------------------------ */
/* pass B: the walk over the Messages rows                              */

struct WpeErrWalk
{
 const WpeErrPat *p;
 const WpeErrRow *rows;
 int n;
 const char *source;
 int stop_at_warnings;
 Match m;
 /* the message being followed */
 int start, i, iy, iorig;
 WpeErrHit *hits;
 int nhits, cap;
 /* the cost of the rows so far */
 int costly_row;
 long peak;
};

WpeErrWalk *wpe_errpat_walk_begin(const WpeErrPat *p, const WpeErrRow *rows,
  int n, const char *source, int stop_at_warnings)
{
 WpeErrWalk *w = calloc(1, sizeof *w);
 size_t longest = 0, widest = 0, room;
 int i;

 if (!w)
  return NULL;
 for (i = 0; i < n; i++)
  if ((size_t)rows[i].len > longest)
   longest = (size_t)rows[i].len;
 for (i = 0; i < p->nrows; i++)
  if (strlen(p->rows[i]) > widest)
   widest = strlen(p->rows[i]);
 room = longest + widest + (source ? strlen(source) : 0) + SCRATCH_SLACK;
 w->p = p;
 w->rows = rows;
 w->n = n;
 w->source = source;
 w->stop_at_warnings = stop_at_warnings;
 w->costly_row = -1;
 w->m.search = calloc(room, 1);
 w->m.file = calloc(room, 1);
 w->m.cmp = calloc(room, 1);
 if (!w->m.search || !w->m.file || !w->m.cmp)
 {
  wpe_errpat_walk_end(w);
  return NULL;
 }
 return w;
}

void wpe_errpat_walk_end(WpeErrWalk *w)
{
 if (!w)
  return;
 free(w->m.search);
 free(w->m.file);
 free(w->m.cmp);
 free(w);
}

int wpe_errpat_walk_costly_row(const WpeErrWalk *w)
{
 return w->costly_row;
}

long wpe_errpat_walk_peak_steps(const WpeErrWalk *w)
{
 return w->peak;
}

/** Forget what the last message took: a new message starts from nothing. */
static void clear_message(WpeErrWalk *w)
{
 Match *m = &w->m;

 m->search[0] = m->cmp[0] = m->file[0] = '\0';
 if (!wpe_errpat_names_file(w->p) && w->source)
  strcpy(m->file, w->source);
 m->y = -1;
 m->x = 0;
}

/**
 * drop_costly_row - A row that spent its budget did not match.
 * The message loses its line, so no entry is made from it, and a first row
 * loses its file too.  The first Messages row that did this is remembered
 * so one note can name it.
 */
static void drop_costly_row(WpeErrWalk *w, int pat_row, int msg_row)
{
 if (w->costly_row < 0)
  w->costly_row = msg_row;
 w->m.y = -1;
 if (pat_row == 0)
  w->m.file[0] = '\0';
}

/**
 * match_row - Match pattern row @pat_row against Messages row @msg_row.
 * Return: the matcher's answer, 0 for a row that was too costly.
 */
static int match_row(WpeErrWalk *w, int pat_row, int msg_row)
{
 Match *m = &w->m;
 const char *row = w->rows[msg_row].s;
 long used;
 int r;

 m->steps = WPE_ERRPAT_BUDGET;
 m->depth = 0;
 m->exhausted = 0;
 r = match(m, wpe_errpat_row(w->p, pat_row), row, row);
 used = WPE_ERRPAT_BUDGET - m->steps;
 if (used > w->peak)
  w->peak = used;
 if (!m->exhausted)
  return r;
 drop_costly_row(w, pat_row, msg_row);
 return 0;
}

/** Does Messages row @i end in a backslash, so the next row goes on with it? */
static int row_continues(const WpeErrWalk *w, int i)
{
 return w->rows[i].len > 0 && w->rows[i].s[w->rows[i].len - 1] == '\\';
}

/** Is there a Messages row after @i? */
static int has_next_row(const WpeErrWalk *w, int i)
{
 return i + 1 < w->n;
}

/** Move over the rows that go on with the one at @i; never past the last. */
static int skip_continuation(const WpeErrWalk *w, int i)
{
 while (has_next_row(w, i) && row_continues(w, i))
  i++;
 return i;
}

/** Has the message a file and a line, so it can make an entry? */
static int has_message(const WpeErrWalk *w)
{
 return w->m.file[0] && w->m.y >= 0;
}

/**
 * search_text - The text the entry looks for in the source line, and the
 * column that goes with it.
 * @w: the walk; the column is changed to be relative to the compiler's text
 * when the pattern gave one.
 * Return: a new string, or NULL when the entry looks for nothing.
 */
static char *search_text(WpeErrWalk *w)
{
 Match *m = &w->m;
 const char *row = w->rows[w->iy].s;
 const char *cp;
 char *srch;

 if (m->search[0] == SEARCH_PREVIOUS)
 {
  cp = strstr(row, m->cmp);
  if (!cp)
   m->x = 0;
  else
   m->x -= (int)(cp - row);
  srch = malloc(strlen(m->cmp) + 2);
  if (!srch)
   return NULL;
  srch[0] = SEARCH_PREVIOUS;
  strcpy(srch + 1, m->cmp);
  return srch;
 }
 if (m->search[0])
 {
  srch = malloc(strlen(m->search) + 1);
  if (srch)
   strcpy(srch, m->search);
  return srch;
 }
 return NULL;
}

/** Does this entry fail the build?  A warning the pattern marks does not. */
static int hit_stops(const WpeErrWalk *w, const char *row)
{
 if (w->stop_at_warnings || w->p->nmark == 0)
  return 1;
 return !row_marked(w->p, row);
}

/** Make an entry of the message just matched. */
static void file_hit(WpeErrWalk *w)
{
 WpeErrHit *h;
 const char *row = w->rows[w->i].s;
 char *file = NULL;

 if (w->nhits >= w->cap)
  return;
 file = copy_range(w->m.file, 0, (int)strlen(w->m.file));
 if (!file)
  return;
 h = &w->hits[w->nhits];
 memset(h, 0, sizeof *h);
 h->file = file;
 h->line = w->m.y;
 h->srch = search_text(w);
 h->x = w->m.x;
 h->row = w->iorig;
 h->text_row = w->i;
 h->kind = row_kind(w->p, row);
 h->stops = hit_stops(w, row);
 w->nhits++;
}

/**
 * take_second_row - Match the pattern's second row against the row after the
 * first one (and the rows that go on with it).  When no row is left it does
 * not match: the message has no line.
 */
static void take_second_row(WpeErrWalk *w)
{
 w->m.y = -1;
 w->i = skip_continuation(w, w->i);
 if (!has_next_row(w, w->i))
  return;
 w->i++;
 match_row(w, 1, w->i);
 w->iy = w->i;
}

/**
 * take_later_row - Match the pattern's third row, or the fourth, against the
 * row after the last one used.
 * @advanced: out, 1 when a row was taken, so the caller can give it back.
 * Return: non-zero when it matched, or when the pattern has no later row to
 * ask for (the message is complete as it stands).
 */
static int take_later_row(WpeErrWalk *w, int *advanced)
{
 int l;

 *advanced = 0;
 if (wpe_errpat_row_count(w->p) <= 2 || !has_message(w) || !has_next_row(w, w->i))
  return 1;
 w->i = skip_continuation(w, w->i);
 if (!has_next_row(w, w->i))
  return 0;
 w->i++;
 *advanced = 1;
 l = match_row(w, 2, w->i);
 if (!l && wpe_errpat_row_count(w->p) > 3)
  l = match_row(w, 3, w->i);
 return l;
}

/**
 * take_entries - Make an entry for the message, and for each further row the
 * pattern's later rows keep matching.
 * After a later row that does not match, the row is given back so the
 * walk looks at it again as the start of the next message.
 */
static void take_entries(WpeErrWalk *w)
{
 int l, advanced;

 do
 {
  l = take_later_row(w, &advanced);
  if (has_message(w) && l != 0)
  {
   file_hit(w);
   w->i = skip_continuation(w, w->i);
  }
 } while (wpe_errpat_row_count(w->p) > 2 && has_message(w) && l != 0 &&
   has_next_row(w, w->i));
 if (wpe_errpat_row_count(w->p) > 2 && has_message(w) && l == 0 && advanced)
  w->i--;
}

/** Is there another message to read from the rows after the entry just made? */
static int wants_more(const WpeErrWalk *w)
{
 return wpe_errpat_row_count(w->p) > 1 && has_message(w) && has_next_row(w, w->i);
}

/** Does the message's second row have to be read? */
static int wants_second_row(const WpeErrWalk *w)
{
 return wpe_errpat_row_count(w->p) > 1 && w->m.file[0] && has_next_row(w, w->i);
}

/**
 * follow_message - Read the message that starts at the walk's first row.
 *
 * The pattern's first row is tried on the first Messages row and the later
 * rows on the rows after it.  Each time the pattern's rows all match, one
 * entry is made and the walk looks for the pattern again in the rows that
 * follow.  When a second row fails the row is given back, but never further
 * than where the message started, so the walk always moves on.
 */
static void follow_message(WpeErrWalk *w)
{
 do
 {
  if (wants_second_row(w))
   take_second_row(w);
  take_entries(w);
 } while (wants_more(w));
 if (wpe_errpat_row_count(w->p) > 1 && w->m.file[0] && w->m.y < 0 && w->i > w->start)
  w->i--;
}

int wpe_errpat_walk_from(WpeErrWalk *w, int start, WpeErrHit *hits, int *nhits)
{
 *nhits = 0;
 if (start < 0 || start >= w->n)
  return start;
 w->start = w->i = w->iy = w->iorig = start;
 w->hits = hits;
 w->nhits = 0;
 w->cap = w->n - start;
 clear_message(w);
 match_row(w, 0, start);
 follow_message(w);
 *nhits = w->nhits;
 return w->i;
}
