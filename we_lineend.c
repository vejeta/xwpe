/* we_lineend.c -- the one owner of a file's line endings.      */
/* Copyright (C) 1993 Fred Kruse                                */
/* Copyright (C) 2026 Juan Manuel Mendez Rey                    */
/* This is free software; you can redistribute it and/or        */
/* modify it under the terms of the                             */
/* GNU General Public License, see the file COPYING.            */
/*
   The rules, in the words of a user who edits files from other systems:

   1. A buffer has one line-ending style, LF or CRLF, decided when its file
      is read and kept with it.
   2. A file is CRLF only when every line break in it is CR LF.  Otherwise it
      is LF, and every CR in it is a byte of the text (shown ^M), kept in
      its row and written back where it was.  A CR is never a line break on
      its own.
   3. A file with no line break at all (empty, or one line without one)
      takes the buffer's default style, as a new file does.
   4. Saving writes the style after every hard row, so a line the user did
      not change is written as it was read.

   Bytes become rows here and rows become bytes here, nowhere else.  A row
   is a C string, so a NUL byte of a file cannot be held in one: the reader
   drops it and says so in the info it returns. */

#include "edit.h"
#include "we_buffer.h"
#include "we_lineend.h"

/* How many bytes the reader asks the disk for at a time. */
#define E_EOL_READ_CHUNK   65536

/* What the help filter of a manual page is in the middle of. */
#define E_READ_BOLD_NONE   0   /* plain text */
#define E_READ_BOLD_OPEN   1   /* a backspace sequence started a bold run */
#define E_READ_BOLD_PEEKED 2   /* the byte after the run is read: it comes next */

/* The rows a text makes while it is read, and the line being read.  The
   reader only collects a line's bytes; the buffer's owner cuts each line
   into rows of the buffer's width (e_buffer_rows_push), as it does for a
   paste or a block moved. */
typedef struct
{
 STRING *line;           /* the rows made so far */
 int n, cap;
 BUFFER *b;              /* the buffer the rows go to (NULL for plain text) */
 int mx_x;               /* Max. Columns: how wide a row is */
 unsigned char *cur;     /* the bytes of the line being read */
 int cur_n, cur_cap;
} EolRows;

/* The bytes being read, and how far. */
typedef struct
{
 const unsigned char *p;
 size_t n, i;
} EolSrc;

/**
 * eol_src_get - Take the next byte of the source.
 * @s: the source; @c: receives the byte, and is left alone at the end.
 *
 * Return: 1 for a byte, 0 at the end (what fread counts).
 */
static int eol_src_get(EolSrc *s, signed char *c)
{
 if (s->i >= s->n)
  return(0);
 *c = (signed char) s->p[s->i++];
 return(1);
}

/**
 * eol_put - Add one byte to the line being read.
 * @rl: the rows and the line read so far; @c: the byte.
 *
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM with the line as it was.
 */
static int eol_put(EolRows *rl, unsigned char c)
{
 if (e_buffer_bytes_reserve(&rl->cur, &rl->cur_cap, rl->cur_n + 1) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 rl->cur[rl->cur_n++] = c;
 return(E_BUFFER_OK);
}

/**
 * eol_line_end - The line being read is complete: the owner makes its
 * rows, and the next line starts empty.
 * @rl: the rows and the line read so far; @term: WPE_WR for a line that
 * ends with a line break, '\0' for one that does not.
 *
 * A line of exactly Max. Columns bytes is one row with its line break; a
 * longer one is cut after the last blank or dash that fits.
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM.
 */
static int eol_line_end(EolRows *rl, int term)
{
 int r = e_buffer_rows_push(&rl->line, &rl->n, &rl->cap, rl->mx_x, rl->cur, rl->cur_n, term);

 rl->cur_n = 0;
 return(r);
}

/**
 * eol_bold_start - A manual page's backspace sequence makes the byte
 * before it the start of a bold run.
 * @rl: the line being read.
 */
static void eol_bold_start(EolRows *rl)
{
 if (rl->cur_n > 0)
  rl->cur[rl->cur_n - 1] = HBB;
}

/**
 * eol_overstrike - A second backspace in a bold run: the byte after it
 * takes the place of the one before it.
 * @rl: the line being read.
 */
static void eol_overstrike(EolRows *rl)
{
 if (rl->cur_n > 0)
  rl->cur_n--;
}

/**
 * eol_file_end - The text ended: its last line, unless it ended on a line
 * break, becomes rows, and @end says where its text ends.
 * @rl: the rows and the line read so far; @y: the row of the buffer the text
 * goes to; @end: receives the end, as a row of the buffer once the text is
 * in (the row after the last, at column 0 after a final line break); may be
 * NULL.
 *
 * A last line without a line break gets one, as the editor has always
 * written a file; an empty file read at the top of a window is one empty
 * line.  Return: E_BUFFER_OK, or E_BUFFER_NOMEM.
 */
static int eol_file_end(EolRows *rl, int y, POINT *end)
{
 int ended_on_break = (rl->cur_n == 0 && y + rl->n > 0);

 if (!ended_on_break && eol_line_end(rl, WPE_WR) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 if (end != NULL)
 {
  end->x = 0;
  if (!ended_on_break)
   end->x = rl->line[rl->n - 1].len;
  end->y = y + rl->n;
 }
 return(E_BUFFER_OK);
}

/**
 * eol_help_byte - Filter one byte of a manual page through its backspace
 * sequences: "x\bx" makes a bold run, which the window shows highlighted.
 * @src: the text; @n: what the read of @c gave (1, or 0 at the end); @c: the
 * byte just read, replaced by the byte the line gets next; @cc: the byte
 * read ahead; @hb: the filter's state (E_READ_BOLD_*); @rl: the line being
 * read.
 *
 * Return: 1 while the text goes on, 0 at its end, -1 when memory ran out.
 */
static int eol_help_byte(EolSrc *src, int n, signed char *c, signed char *cc, int *hb,
                         EolRows *rl)
{
 if (*c == CtrlH)
 {
  if (*hb == E_READ_BOLD_NONE)
  {
   eol_bold_start(rl);
   *hb = E_READ_BOLD_OPEN;
  }
  else
   eol_overstrike(rl);
  return(eol_src_get(src, c));
 }
 if (*hb != E_READ_BOLD_OPEN)
  return(n);
 n = eol_src_get(src, cc);
 if (*cc != HBS)
 {
  if (eol_put(rl, HED) != E_BUFFER_OK)
   return(-1);
  *hb = E_READ_BOLD_PEEKED;
  return(n);
 }
 return(eol_src_get(src, c));
}

/**
 * eol_classify - Count what the line breaks of some bytes are.
 * @p, @n: the bytes; @info: receives the counts and the style they decide.
 *
 * The file is CRLF only when every line break in it is CR LF and there is at
 * least one; one bare LF makes it LF, and then every CR is text.
 */
static void eol_classify(const unsigned char *p, size_t n, WpeEolInfo *info)
{
 size_t i;
 int crlf = 0, bare = 0;

 memset(info, 0, sizeof(*info));
 for (i = 0; i < n; i++)
 {
  if (p[i] == WPE_WR)
  {
   if (i > 0 && p[i - 1] == WPE_CR)
    crlf++;
   else
    bare++;
  }
  else if (p[i] == WPE_CR && (i + 1 >= n || p[i + 1] != WPE_WR))
   info->lone_cr++;
 }
 info->breaks = crlf + bare;
 info->mixed = (crlf > 0 && bare > 0);
 info->eol = (crlf > 0 && bare == 0) ? E_EOL_CRLF : E_EOL_LF;
 info->final_break = (n > 0 && p[n - 1] == WPE_WR) ? E_FINAL_BREAK_HAD : E_FINAL_BREAK_NONE;
}

/**
 * eol_cr_is_break - Is the carriage return just read part of a line break?
 * @src: the text, positioned after that CR; @crlf: the file's style is CRLF;
 * @manpage: the text is a manual page.
 *
 * Only in a CRLF file, and only when an LF follows.  Every other CR is a byte
 * of the text.  A manual page has no use for one and drops it.
 * Return: 1 when the CR is left out of the line.
 */
static int eol_cr_is_break(const EolSrc *src, int crlf, int manpage)
{
 if (manpage)
  return(1);
 return(crlf && src->i < src->n && src->p[src->i] == WPE_WR);
}

/**
 * eol_read_bytes - Make the rows of a file's bytes, one line at a time.
 * @src: the bytes; @rl: the rows (with the front of the row the text goes
 * into already in the line being read); @crlf: the style the bytes have;
 * @manpage: filter the text as a manual page; @info: counts the NULs dropped;
 * @y, @end: as eol_file_end.
 *
 * Lines break at the file's line breaks, and the owner cuts a line wider
 * than the buffer at the last blank or dash that fits.  The buffer is not
 * touched.  Return: E_BUFFER_OK, or E_BUFFER_NOMEM.
 */
static int eol_read_bytes(EolSrc *src, EolRows *rl, int crlf, int manpage, WpeEolInfo *info,
                          int y, POINT *end)
{
 int n = 1, hb = E_READ_BOLD_NONE, r = E_BUFFER_OK;
 signed char cc = 0, c = 0;

 for (;;)
 {
  if (hb != E_READ_BOLD_PEEKED)
   n = eol_src_get(src, &c);
  else
  {
   c = cc;
   hb = E_READ_BOLD_NONE;
  }
  if (n == 1 && c == '\0')
  {
   info->nul++;
   continue;
  }
  if (n == 1 && c == WPE_CR && eol_cr_is_break(src, crlf, manpage))
   continue;
  if (manpage)
   n = eol_help_byte(src, n, &c, &cc, &hb, rl);
  if (n < 0)
   return(E_BUFFER_NOMEM);
  if (n != 1)
   break;
  if (c == WPE_WR)
   r = eol_line_end(rl, WPE_WR);
  else
   r = eol_put(rl, (unsigned char) c);
  if (r != E_BUFFER_OK)
   return(r);
 }
 return(eol_file_end(rl, y, end));
}

/**
 * eol_head_line - Start the first line of a file with the front of the
 * line it is put into.
 * @rl: the rows and the line read, empty so far; @y, @x: the line of the
 * buffer and the column the file goes in at.
 *
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM.
 */
static int eol_head_line(EolRows *rl, int y, int x)
{
 if (x == 0)
  return(E_BUFFER_OK);          /* the file starts its line: no front to keep */
 if (e_buffer_bytes_reserve(&rl->cur, &rl->cur_cap, x) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 memcpy(rl->cur, rl->b->bf[y].s, (size_t)x);
 rl->cur_n = x;
 return(E_BUFFER_OK);
}

/**
 * eol_tail_line - Make a row of the part of line @y of the buffer from
 * column @x on, ending as that row ends: what follows a file put in the
 * middle of a line.
 * @rl: the rows read.
 *
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM.
 */
static int eol_tail_line(EolRows *rl, int y, int x)
{
 STRING *row = &rl->b->bf[y];

 return(e_buffer_rows_push(&rl->line, &rl->n, &rl->cap, rl->mx_x, row->s + x,
                           row->len - x, row->s[row->len]));
}

/**
 * eol_slurp - Read the rest of a file into memory.
 * @fp: the file; @bytes, @n: receive the bytes (the caller frees them) and
 * how many.
 *
 * The style of a file is known only after all of it has been seen, so it is
 * read whole before its lines are made.  Return: E_BUFFER_OK, or
 * E_BUFFER_NOMEM.
 */
static int eol_slurp(FILE *fp, unsigned char **bytes, size_t *n)
{
 int cap = 0;
 size_t used = 0, got;

 *bytes = NULL;
 for (;;)
 {
  if (used > (size_t) INT_MAX - E_EOL_READ_CHUNK - 1 ||
      e_buffer_bytes_reserve(bytes, &cap, (int) used + E_EOL_READ_CHUNK + 1) != E_BUFFER_OK)
  {
   free(*bytes);
   *bytes = NULL;
   return(E_BUFFER_NOMEM);
  }
  got = fread(*bytes + used, sizeof(char), E_EOL_READ_CHUNK, fp);
  if (got == 0)
   break;
  used += got;
 }
 *n = used;
 return(E_BUFFER_OK);
}

/**
 * eol_free_rows - Free what a failed read made.
 * @rl: the rows and the line read so far.
 */
static void eol_free_rows(EolRows *rl)
{
 free(rl->cur);
 e_buffer_lines_free(rl->line, rl->n);
}

/**
 * e_eol_read_file_into - Put the lines of a file into a buffer at a place.
 * @fp: the file; @b: the buffer; @x, @y: the column and the line of the
 * cursor, where the file goes in; @manpage: filter the file as a manual page
 * (backspace sequences make bold runs, carriage returns are dropped);
 * @info: receives what the bytes said of their line endings (may be NULL);
 * @end: where the file's text ends in the buffer.
 *
 * Opened at its first line, a file arrives whole.  Put in the middle of a
 * line (Block > Read), it splits that line, and the lines of the file stand
 * between the two halves.  The window's undo takes the insert back in one
 * step, its modified mark shows it, and the breakpoints below it move down.
 * A CRLF file keeps its style in @info and a carriage return that is not
 * part of a CR LF break stays in its row, so saving the file writes back
 * what was read.  A refusal for want of memory leaves the buffer as it was.
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM.
 */
int e_eol_read_file_into(FILE *fp, BUFFER *b, int x, int y, int manpage, WpeEolInfo *info,
                         POINT *end)
{
 EolRows rl;
 EolSrc src;
 WpeEolInfo mine;
 unsigned char *bytes;
 size_t n;
 int r, n_del = 0;

 if (info == NULL)
  info = &mine;
 memset(&rl, 0, sizeof(rl));
 rl.b = b;
 rl.mx_x = b->mx.x;
 if (x > b->bf[y].len)
  x = b->bf[y].len;
 if (eol_slurp(fp, &bytes, &n) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 eol_classify(bytes, n, info);
 src.p = bytes;
 src.n = n;
 src.i = 0;
 if (eol_head_line(&rl, y, x) != E_BUFFER_OK ||
     eol_read_bytes(&src, &rl, info->eol == E_EOL_CRLF, manpage, info, y, end) != E_BUFFER_OK ||
     (x > 0 && eol_tail_line(&rl, y, x) != E_BUFFER_OK))
 {
  free(bytes);
  eol_free_rows(&rl);
  return(E_BUFFER_NOMEM);
 }
 free(bytes);
 free(rl.cur);
 if (x > 0)
  n_del = 1;
 r = e_buffer_splice(b, y, n_del, rl.line, rl.n, NULL, 0, E_BUFFER_STEP_NEW);
 if (r == E_BUFFER_OK)
  FREE(rl.line);                /* the lines are the buffer's now */
 else
  e_buffer_lines_free(rl.line, rl.n);
 return(r);
}

/**
 * eol_rows_from_text - The lines of text the buffer's own rows made, split on
 * LF only.
 * @bytes, @n: the text; @mx_x: Max. Columns; @count: receives the number of
 * lines.
 *
 * The first NUL ends the text, as it ends a row.  Return: as
 * e_buffer_lines_from_text.
 */
static STRING *eol_rows_from_text(const unsigned char *bytes, size_t n, int mx_x, int *count)
{
 STRING *lines;
 char *text = malloc(n + 1);

 if (text == NULL)
 {
  *count = -1;
  return(NULL);
 }
 memcpy(text, bytes, n);
 text[n] = '\0';
 lines = e_buffer_lines_from_text(text, mx_x, count);
 free(text);
 return(lines);
}

/**
 * e_eol_lines - Make the rows of some bytes.
 * @bytes, @n: the bytes; @mx_x: Max. Columns, how wide a row is; @policy: how
 * the bytes were made (E_EOL_KEEP for a file's bytes, E_EOL_ROWS for text
 * the buffer's rows made); @info: receives what E_EOL_KEEP found (may be
 * NULL); @count: receives the number of rows.
 *
 * E_EOL_ROWS is the exact inverse of e_eol_text with E_EOL_LF: a text the
 * buffer made goes back into the buffer with every carriage return where it
 * was, so a whole-text replace does not change a line nobody touched.
 * Return: rows the caller owns until a splice adopts them (NULL with
 * *count == 0 for empty text; *count < 0 for no memory).
 */
STRING *e_eol_lines(const unsigned char *bytes, size_t n, int mx_x, WpeEolPolicy policy,
                    WpeEolInfo *info, int *count)
{
 EolRows rl;
 EolSrc src;
 WpeEolInfo mine;

 *count = 0;
 if (policy == E_EOL_ROWS)
  return(eol_rows_from_text(bytes, n, mx_x, count));
 if (info == NULL)
  info = &mine;
 memset(&rl, 0, sizeof(rl));
 rl.mx_x = mx_x;
 eol_classify(bytes, n, info);
 src.p = bytes;
 src.n = n;
 src.i = 0;
 if (eol_read_bytes(&src, &rl, info->eol == E_EOL_CRLF, 0, info, 0, NULL) != E_BUFFER_OK)
 {
  eol_free_rows(&rl);
  *count = -1;
  return(NULL);
 }
 free(rl.cur);
 *count = rl.n;
 return(rl.line);
}

/* Where the bytes of rows go: a file, or memory. */
typedef struct
{
 FILE *fp;
 unsigned char *buf;
 size_t n, cap;
 int failed;
} EolSink;

/**
 * eol_sink_put - Add bytes to a sink.
 * @k: the sink; @p, @n: the bytes.
 *
 * A failure is remembered, and later bytes are not tried.
 */
static void eol_sink_put(EolSink *k, const unsigned char *p, size_t n)
{
 unsigned char *more;

 if (k->failed || n == 0)
  return;
 if (k->fp != NULL)
 {
  if (fwrite(p, sizeof(char), n, k->fp) != n)
   k->failed = 1;
  return;
 }
 if (k->n + n + 1 > k->cap)
 {
  size_t cap = (k->n + n + 1) * 2;

  if ((more = realloc(k->buf, cap)) == NULL)
  {
   k->failed = 1;
   return;
  }
  k->buf = more;
  k->cap = cap;
 }
 memcpy(k->buf + k->n, p, n);
 k->n += n;
}

/**
 * eol_break_put - Add one line break in a style to a sink.
 * @k: the sink; @eol: E_EOL_LF or E_EOL_CRLF.
 */
static void eol_break_put(EolSink *k, int eol)
{
 static const unsigned char crlf[2] = { WPE_CR, WPE_WR };

 if (eol == E_EOL_CRLF)
  eol_sink_put(k, crlf, 2);
 else
  eol_sink_put(k, crlf + 1, 1);
}

/**
 * eol_row_out - Add the bytes of one row, from column @from up to column
 * @limit, and its line break when the row is hard and the break is in reach.
 * @k: the sink; @b: the buffer; @row: the row; @from, @limit: the columns
 * (@limit exclusive); @eol: the style of the break; @skip_break: leave out
 * the row's break (the file's last, when it had none).
 *
 * A row's bytes are written whatever they are, up to its length.  A soft row
 * (the buffer's wrap of a long line) has no break after it.
 */
static void eol_row_out(EolSink *k, BUFFER *b, STRING *row, int from, int limit, int eol,
                        int skip_break)
{
 int stop = row->len;

 if (row->s == NULL)
  return;
 if (from < 0)
  from = 0;
 if (stop > limit)
  stop = limit;
 if (stop > b->mx.x + 1)
  stop = b->mx.x + 1;
 if (stop > from)
  eol_sink_put(k, row->s + from, (size_t)(stop - from));
 if (from <= row->len && row->len < limit && row->len <= b->mx.x &&
     row->s[row->len] == WPE_WR && !skip_break)
  eol_break_put(k, eol);
}

/**
 * eol_walk - Hand the rows of a range of the buffer to a sink, in a style.
 * @k: the sink; @b: the buffer; @ya, @xa, @ye, @xe: the range as e_write has
 * always taken it: the rows from @ya (from column @xa on, when the range
 * goes on to a later row) up to row @ye, which is written up to column @xe;
 * @eol: the style; @keep_final: leave out the break of the buffer's last
 * row when the file had none.
 */
static void eol_walk(EolSink *k, BUFFER *b, int ya, int xa, int ye, int xe, int eol,
                     int keep_final)
{
 int j, last = b->mxlines - 1;
 int omit = (keep_final && b->final_break == E_FINAL_BREAK_NONE);

 for (j = ya; j < ye && j < b->mxlines; j++)
  eol_row_out(k, b, &b->bf[j], (j == ya) ? xa : 0, INT_MAX, eol, omit && j == last);
 if (j < b->mxlines)
  eol_row_out(k, b, &b->bf[j], 0, xe, eol, omit && j == last);
}

/**
 * e_eol_text - The rows of a range of the buffer as bytes.
 * @b: the buffer; @ya, @xa, @ye, @xe: the range (see eol_walk); @eol: the
 * line-break style of the text; @keep_final: as eol_walk; @out: receives the
 * text, NUL-terminated, malloc'd (NULL when memory ran out).
 *
 * With E_EOL_LF it is what E_EOL_ROWS reads back into the same rows.
 * Return: the number of bytes (not counting the terminator); a caller that
 * treats the text as a C string stops at a NUL, as a row does.
 */
size_t e_eol_text(BUFFER *b, int ya, int xa, int ye, int xe, int eol, int keep_final,
                  char **out)
{
 EolSink k;

 memset(&k, 0, sizeof(k));
 eol_walk(&k, b, ya, xa, ye, xe, eol, keep_final);
 if (k.failed)
 {
  free(k.buf);
  *out = NULL;
  return(0);
 }
 if (k.buf == NULL && (k.buf = malloc(1)) == NULL)
 {
  *out = NULL;
  return(0);
 }
 k.buf[k.n] = '\0';
 *out = (char *) k.buf;
 return(k.n);
}

/**
 * e_eol_write - Write a range of the buffer to a file in the buffer's own
 * line-break style.
 * @fp: the file; @b: the buffer; @ya, @xa, @ye, @xe: the range (see
 * eol_walk); @keep_final: leave out the last row's break when the file had
 * none.
 *
 * Saving changes only the lines the user changed: a CRLF file is written
 * CRLF, an LF file LF, a carriage return kept as text where it was.
 * Return: 0, or -1 when a write failed.
 */
int e_eol_write(FILE *fp, BUFFER *b, int ya, int xa, int ye, int xe, int keep_final)
{
 EolSink k;

 memset(&k, 0, sizeof(k));
 k.fp = fp;
 eol_walk(&k, b, ya, xa, ye, xe, b->eol, keep_final);
 return(k.failed ? -1 : 0);
}

/**
 * e_eol_default - The style a new file gets.
 * @cn: the editor.
 *
 * What xwperc's Data key says (109, the original's MS-DOS file, is CRLF);
 * anything else is LF.
 */
int e_eol_default(const ECNT *cn)
{
 return(cn->eol_default == E_EOL_CRLF ? E_EOL_CRLF : E_EOL_LF);
}

/**
 * e_eol_set_default - Take the Data key of an options file as the default
 * style.
 * @cn: the editor; @data_key: the number after Data.
 *
 * The key never reaches a window's kind: a value that is not the original's
 * MS-DOS file is LF, so a hand-edited file cannot make a window a file
 * manager.
 */
void e_eol_set_default(ECNT *cn, int data_key)
{
 cn->eol_default = (data_key == DTMD_MSDOS) ? E_EOL_CRLF : E_EOL_LF;
}

/**
 * e_eol_data_key - The Data key to write to an options file for the
 * default style, as the original wrote it, so it reads the same there.
 * @cn: the editor.
 */
int e_eol_data_key(const ECNT *cn)
{
 return(e_eol_default(cn) == E_EOL_CRLF ? DTMD_MSDOS : DTMD_NORMAL);
}

/**
 * e_eol_buffer_fresh - Give a buffer made for a file that does not exist
 * the default style and no recorded final break.
 * @b: the buffer; @cn: the editor.
 */
void e_eol_buffer_fresh(BUFFER *b, const ECNT *cn)
{
 b->eol = e_eol_default(cn);
 b->final_break = E_FINAL_BREAK_NONE;
}

/**
 * e_eol_buffer_adopt - Keep what a file's bytes said of their line endings
 * with the buffer they were read into.
 * @b: the buffer; @cn: the editor; @info: what the read found.
 *
 * A file with no line break at all (empty, or one line without one) says
 * nothing of a style and takes the default, as a new file does.
 */
void e_eol_buffer_adopt(BUFFER *b, const ECNT *cn, const WpeEolInfo *info)
{
 if (info->breaks == 0)
  b->eol = e_eol_default(cn);
 else
  b->eol = info->eol;
 b->final_break = info->final_break;
}

/**
 * e_eol_name - The style of a buffer as a word for the test dump.
 * @b: the buffer.
 */
const char *e_eol_name(const BUFFER *b)
{
 return(b->eol == E_EOL_CRLF ? "crlf" : "lf");
}
