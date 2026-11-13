/* we_lsp_pos.c -- the language server's columns: the one owner.
   See we_lsp_pos.h. */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <json-c/json.h>

#include "we_lsp_pos.h"
#include "we_utf8.h"

#define LSP_ENC_NAME_UTF8   "utf-8"
#define LSP_ENC_NAME_UTF16  "utf-16"
#define LSP_UNITS_PER_PAIR  2       /* a code point above the BMP in UTF-16 */
#define LSP_SEM_GROUP       5         /* ints per semantic token              */
#define LSP_LINE_END_CHAR   1000000   /* past any line's end: the spec clamps */

/* ---- the type and the encoding ----------------------------------------- */

/**
 * e_lsp_units_make - The wire's number as units.
 *
 * The only place a number becomes units: the JSON a server sends and the
 * semantic deltas are numbers, and so are the literal columns of the tests.
 * A negative count is no column and is 0.
 */
e_lsp_units e_lsp_units_make(int on_wire)
{
 e_lsp_units u;

 u.units_on_wire = on_wire < 0 ? 0 : on_wire;
 return u;
}

/**
 * e_lsp_units_zero - Column 0: where a position stands before an answer
 * gave it, so no file but this one makes units.
 */
e_lsp_units e_lsp_units_zero(void)
{
 return e_lsp_units_make(0);
}

/**
 * e_lsp_units_sum - The end of a span: where it starts plus its length.
 *
 * Saturates instead of overflowing, for a server that sends absurd numbers.
 */
e_lsp_units e_lsp_units_sum(e_lsp_units a, e_lsp_units b)
{
 if (a.units_on_wire > INT_MAX - b.units_on_wire)
  return e_lsp_units_make(INT_MAX);
 return e_lsp_units_make(a.units_on_wire + b.units_on_wire);
}

int e_lsp_units_eq(e_lsp_units a, e_lsp_units b)
{
 return a.units_on_wire == b.units_on_wire;
}

/**
 * e_lsp_encodings_offered - What the editor says it can count in.
 *
 * utf-8 first: with it the server's columns are the editor's bytes and the
 * conversions below are the identity walk.  utf-16 is the one every server
 * must support, so a server that answers nothing is still understood.
 */
struct json_object *e_lsp_encodings_offered(void)
{
 struct json_object *arr = json_object_new_array();

 json_object_array_add(arr, json_object_new_string(LSP_ENC_NAME_UTF8));
 json_object_array_add(arr, json_object_new_string(LSP_ENC_NAME_UTF16));
 return arr;
}

/**
 * e_lsp_encoding_parse - The encoding a server's answer names.
 *
 * "utf-8" is UTF8; an absent answer, "utf-16" and anything else (a value
 * that was not offered) are UTF16, which every server supports.
 */
int e_lsp_encoding_parse(const char *answer)
{
 if (answer && !strcmp(answer, LSP_ENC_NAME_UTF8))
  return E_LSP_ENC_UTF8;
 return E_LSP_ENC_UTF16;
}

/**
 * e_lsp_encoding_answer_is_known - Did the server answer with something the
 * editor offered, or nothing?  Lets the session say once in the wire trace
 * that it fell back to utf-16 for a value it did not offer.
 */
int e_lsp_encoding_answer_is_known(const char *answer)
{
 return !answer || !strcmp(answer, LSP_ENC_NAME_UTF8) ||
        !strcmp(answer, LSP_ENC_NAME_UTF16);
}

const char *e_lsp_encoding_name(int enc)
{
 return enc == E_LSP_ENC_UTF8 ? LSP_ENC_NAME_UTF8 : LSP_ENC_NAME_UTF16;
}

/* ---- the two conversions ----------------------------------------------- */

/**
 * lsp_glyph_units - The units one glyph takes on the wire.
 * @n: its bytes; @cp: its code point, 0 for a byte that is no character.
 *
 * utf-8 counts bytes.  utf-16 counts two for a code point above the BMP and
 * one for anything else; a stray byte is one unit, as the painter draws it
 * as one cell.
 */
static int lsp_glyph_units(int n, int cp, int enc)
{
 if (enc == E_LSP_ENC_UTF8)
  return n;
 if (cp >= E_LSP_UTF16_PAIR_FROM)
  return LSP_UNITS_PER_PAIR;
 return 1;
}

/**
 * e_lsp_units_of - The units of a byte column of one row.
 *
 * Used for every column the editor sends: on a line with an accented letter,
 * a CJK character or an emoji before the cursor, the number the server
 * expects is not the byte the cursor is on.
 */
e_lsp_units e_lsp_units_of(const unsigned char *row, int len, int x, int enc)
{
 int i = 0, sum = 0;

 if (x <= 0 || !row || len <= 0)
  return e_lsp_units_make(0);
 if (x > len)
  x = len;
 while (i < len)
 {
  int cp, cols;
  int n = e_utf8_glyph_at(row + i, len - i, &cp, &cols);

  if (x < i + n)                  /* x is this glyph's start or inside it */
   break;
  sum += lsp_glyph_units(n, cp, enc);
  i += n;
 }
 return e_lsp_units_make(sum);
}

/**
 * e_lsp_col_of - The byte column of units over one row.
 *
 * Used for every column a server sends: the byte the cursor, a mark or a
 * paint can be put on.  A unit inside a surrogate pair goes back to the
 * glyph's start, so a range end there shrinks and never widens.
 */
int e_lsp_col_of(const unsigned char *row, int len, e_lsp_units u, int enc)
{
 int i = 0, sum = 0;

 if (!row || len <= 0)
  return 0;
 while (i < len)
 {
  int cp, cols;
  int n = e_utf8_glyph_at(row + i, len - i, &cp, &cols);
  int us = lsp_glyph_units(n, cp, enc);

  if (u.units_on_wire < sum + us)
   return i;
  sum += us;
  i += n;
 }
 return len;
}

/**
 * e_lsp_col_unread_row - The byte column of units when the row cannot be
 * had (a file that cannot be read): the units taken as bytes, as for ASCII.
 */
int e_lsp_col_unread_row(e_lsp_units u)
{
 return u.units_on_wire;
}

/* ---- the wire's helpers ------------------------------------------------- */

/**
 * e_lsp_units_read - Read a position's character.
 * Return: 1 when the key was there, 0 when it was not (then 0 units).
 */
int e_lsp_units_read(struct json_object *pos, e_lsp_units *out)
{
 struct json_object *v = NULL;

 *out = e_lsp_units_make(0);
 if (!pos || !json_object_object_get_ex(pos, "character", &v))
  return 0;
 *out = e_lsp_units_make(json_object_get_int(v));
 return 1;
}

/**
 * e_lsp_position_json - A new {line, character} object.
 */
struct json_object *e_lsp_position_json(int line, e_lsp_units u)
{
 struct json_object *pos = json_object_new_object();

 json_object_object_add(pos, "line", json_object_new_int(line));
 json_object_object_add(pos, "character",
                        json_object_new_int(u.units_on_wire));
 return pos;
}

/**
 * e_lsp_line_range - {start: (start_line, 0), end: (end_line, line end)}.
 *
 * The line end is a number past any line's length: the protocol clamps a
 * character past the line to the line's length, in every encoding.
 */
struct json_object *e_lsp_line_range(int start_line, int end_line)
{
 struct json_object *rng = json_object_new_object();

 json_object_object_add(rng, "start",
                        e_lsp_position_json(start_line, e_lsp_units_make(0)));
 json_object_object_add(rng, "end",
                        e_lsp_position_json(end_line,
                          e_lsp_units_make(LSP_LINE_END_CHAR)));
 return rng;
}

/**
 * e_lsp_semantic_decode - Absolute tokens from the server's delta data.
 *
 * Positions come in the session's units: a delta on the same line adds to
 * the previous start, a delta on a new line starts from 0.
 */
int e_lsp_semantic_decode(const int *data, int n, e_lsp_sem_token *out, int max)
{
 int i, count = 0, line = 0;
 e_lsp_units start = e_lsp_units_make(0);

 if (!data || n < 0)
  return 0;
 for (i = 0; i + LSP_SEM_GROUP - 1 < n; i += LSP_SEM_GROUP)
 {                                      /* groups of 5; ignore a short tail */
  int dline = data[i];
  e_lsp_units dchar = e_lsp_units_make(data[i + 1]);

  if (dline == 0)                       /* same line: char delta is relative */
   start = e_lsp_units_sum(start, dchar);
  else
  {  line += dline;  start = dchar;  }  /* new line: char delta is absolute  */
  if (count >= max)
   continue;                            /* keep advancing line/start, just skip */
  out[count].line = line;
  out[count].start = start;
  out[count].length = e_lsp_units_make(data[i + 2]);
  out[count].type = data[i + 3];
  out[count].modifiers = data[i + 4];
  count++;
 }
 return count;
}

/* ---- the rows of a text ------------------------------------------------- */

/**
 * e_lsp_wire_text - The text as the server is sent it: each CR a space.
 *
 * The editor's copy keeps its CRs (edits are spliced into it and written
 * back); only the copy on the wire loses them, so the server's lines are the
 * editor's rows.
 */
char *e_lsp_wire_text(const char *text)
{
 char *copy = text ? strdup(text) : NULL;
 char *p;

 for (p = copy; p && (p = strchr(p, '\r')); p++)
  *p = ' ';
 return copy;
}

/**
 * e_lsp_text_row - Row @line of a '\n'-joined document.
 *
 * One server line is one buffer row, and the document is the rows joined by
 * '\n': a row never holds a '\n', and the text sent to the server has no '\r'
 * (e_lsp_wire_text), so this split is the server's.
 */
const char *e_lsp_text_row(const char *text, int line, int *len)
{
 const char *p = text, *nl;

 if (!text || line < 0)
  return NULL;
 while (line > 0)
 {
  nl = strchr(p, '\n');
  if (!nl)
   return NULL;
  p = nl + 1;
  line--;
 }
 nl = strchr(p, '\n');
 *len = nl ? (int)(nl - p) : (int)strlen(p);
 return p;
}

/**
 * e_lsp_text_offset - Byte offset of a server position in a text.
 *
 * Used where the editor edits text by a server's position (rename, format,
 * a code action): the edit lands on the bytes the server meant.
 */
size_t e_lsp_text_offset(const char *text, int line, e_lsp_units u, int enc)
{
 int len = 0;
 const char *row = e_lsp_text_row(text, line, &len);

 if (!row)
  return strlen(text);
 return (size_t)(row - text) +
        (size_t)e_lsp_col_of((const unsigned char *)row, len, u, enc);
}

struct e_lsp_text_index {
 const char *text;     /* by reference                                   */
 int         nlines;   /* rows (a text without '\n' has one)             */
 size_t     *off;      /* offset of each row's first byte                */
};

/**
 * e_lsp_text_index_new - Find the rows of a text once.
 *
 * A references answer converts up to 128 columns and a semantic overlay up
 * to 8192 tokens against the same text: each conversion then walks one row,
 * not the text up to it.
 */
e_lsp_text_index *e_lsp_text_index_new(const char *text)
{
 e_lsp_text_index *ix = calloc(1, sizeof(*ix));
 const char *p;
 int n = 1, i = 0;

 if (!ix)
  return NULL;
 ix->text = text;
 if (!text)
  return ix;
 for (p = text; (p = strchr(p, '\n')); p++)
  n++;
 ix->off = malloc((size_t)n * sizeof(*ix->off));
 if (!ix->off)
 {
  free(ix);
  return NULL;
 }
 ix->off[i++] = 0;
 for (p = text; (p = strchr(p, '\n')); )
 {
  p++;
  ix->off[i++] = (size_t)(p - text);
 }
 ix->nlines = n;
 return ix;
}

void e_lsp_text_index_free(e_lsp_text_index *ix)
{
 if (!ix)
  return;
 free(ix->off);
 free(ix);
}

/**
 * lsp_index_row - Row @line of an index: its bytes and length, or NULL.
 */
static const unsigned char *lsp_index_row(const e_lsp_text_index *ix, int line,
                                          int *len)
{
 const char *row;
 size_t end;

 if (!ix || !ix->text || line < 0 || line >= ix->nlines)
  return NULL;
 row = ix->text + ix->off[line];
 if (line + 1 < ix->nlines)
  end = ix->off[line + 1] - 1;           /* the '\n' */
 else
  end = strlen(ix->text);
 *len = (int)(end - ix->off[line]);
 return (const unsigned char *)row;
}

const char *e_lsp_text_index_row(const e_lsp_text_index *ix, int line, int *len)
{
 return (const char *)lsp_index_row(ix, line, len);
}

/**
 * e_lsp_text_index_col - The byte column of a server position.
 *
 * Without a text (nothing synced yet) the units are taken as bytes; a line
 * the text does not have is an empty row, column 0.
 */
int e_lsp_text_index_col(const e_lsp_text_index *ix, int line, e_lsp_units u,
                         int enc)
{
 int len = 0;
 const unsigned char *row;

 if (!ix || !ix->text)
  return e_lsp_col_unread_row(u);
 row = lsp_index_row(ix, line, &len);
 if (!row)
  return 0;
 return e_lsp_col_of(row, len, u, enc);
}

size_t e_lsp_text_index_offset(const e_lsp_text_index *ix, int line,
                               e_lsp_units u, int enc)
{
 int len = 0;
 const unsigned char *row = lsp_index_row(ix, line, &len);

 if (!row)
  return ix && ix->text ? strlen(ix->text) : 0;
 return ix->off[line] + (size_t)e_lsp_col_of(row, len, u, enc);
}
