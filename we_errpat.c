/* we_errpat.c -- the one owner of a compiler's message pattern.
 *
 * Pass A: reading the pattern text.  The text is cut into rows exactly as
 * the editor has always cut it, each row is walked piece by piece with the
 * same scan the matcher uses, and a text that would make the matcher run off
 * the end of a row is refused with a reason.  Libc only.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "we_errpat.h"

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
