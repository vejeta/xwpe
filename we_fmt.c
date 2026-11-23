/* we_fmt.c -- the one owner of a formatted text written into storage
   (see we_fmt.h).  The only file of the editor that calls vsnprintf.
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later. */

#include "we_fmt.h"
#include "we_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The kinds of a text built from pieces. */
#define FMT_KIND_WHOLE 0
#define FMT_KIND_SHOWN 1
#define FMT_KIND_HEAP  2
#define FMT_KIND_BLOCK 3
/* A shown text that was cut and marked. */
#define FMT_CUT        1
/* The first room of a heap text. */
#define FMT_HEAP_FIRST 256
/* The most continuation bytes a character has after its lead byte. */
#define FMT_MAX_TRAIL  (E_UTF8_MAX_BYTES - 1)

/**
 * fmt_clamp - The size the C library may be given: every system takes an
 * int's worth, so every system gives the same answers.
 */
static size_t fmt_clamp(size_t n)
{
 return(n > (size_t) INT_MAX ? (size_t) INT_MAX : n);
}

/**
 * fmt_keep_whole_chars - How many of the first @cap bytes of the @len bytes
 * at @s can stay without splitting a character.
 */
static size_t fmt_keep_whole_chars(const char *s, size_t len, size_t cap)
{
 if (len <= cap)
  return(len);
 if (len > (size_t) INT_MAX)
  len = INT_MAX;
 return((size_t) e_utf8_glyph_floor((const unsigned char *) s, (int) len,
                                    (int) cap));
}

/**
 * fmt_whole_chars_of_prefix - How many of the @have bytes at @s, the start
 * of a longer text, are whole characters: a last character whose lead byte
 * asks for more bytes than were kept is left out.
 */
static size_t fmt_whole_chars_of_prefix(const char *s, size_t have)
{
 const unsigned char *u = (const unsigned char *) s;
 size_t start = have;

 while (start > 0 && have - start < FMT_MAX_TRAIL &&
        e_utf8_is_continuation(u[start - 1]))
  start--;
 if (start == 0)
  return(have);
 start--;
 if (start + (size_t) e_utf8_charlen(u[start]) > have)
  return(start);
 return(have);
}

/**
 * fmt_mark_cut - End the text in @out[@n], which holds its first n - 1
 * bytes, with the cut mark, on a character boundary.
 * A place of exactly the mark's size and a byte holds the mark alone; a
 * smaller one keeps what fits, cut where a character ends, with no mark.
 */
static void fmt_mark_cut(char *out, size_t n)
{
 size_t mark = strlen(WPE_FMT_CUT_MARK), have, keep;

 have = fmt_whole_chars_of_prefix(out, strlen(out));
 out[have] = '\0';
 if (n <= mark)
  return;
 keep = fmt_keep_whole_chars(out, have, n - 1 - mark);
 memcpy(out + keep, WPE_FMT_CUT_MARK, mark + 1);
}

/**
 * fmt_write - The one call of the C library's bounded writer.
 * @need: receives the bytes the whole text takes, without the NUL.
 * Return: WPE_FMT_OK with @out terminated (cut by the library when the text
 * is longer), or WPE_FMT_FAILED with @out "".
 */
static int fmt_write(char *out, size_t n, size_t *need, const char *fmt, va_list ap)
{
 int wanted = vsnprintf(out, fmt_clamp(n), fmt, ap);

 if (wanted < 0)
 {
  if (n > 0)
   out[0] = '\0';
  *need = 0;
  return(WPE_FMT_FAILED);
 }
 *need = (size_t) wanted;
 return(WPE_FMT_OK);
}

int wpe_fmt_vwhole(char *out, size_t n, const char *fmt, va_list ap)
{
 size_t need;
 int how = fmt_write(out, n, &need, fmt, ap);

 if (how != WPE_FMT_OK)
  return(how);
 if (need < fmt_clamp(n))
  return(WPE_FMT_OK);
 if (n > 0)
  out[0] = '\0';
 return(WPE_FMT_NO_ROOM);
}

int wpe_fmt_whole(char *out, size_t n, const char *fmt, ...)
{
 va_list ap;
 int how;

 va_start(ap, fmt);
 how = wpe_fmt_vwhole(out, n, fmt, ap);
 va_end(ap);
 return(how);
}

int wpe_fmt_vshown(char *out, size_t n, const char *fmt, va_list ap)
{
 size_t need;
 int how = fmt_write(out, n, &need, fmt, ap);

 if (how != WPE_FMT_OK)
  return(how);
 if (need < fmt_clamp(n))
  return(0);
 if (n > 0)
  fmt_mark_cut(out, fmt_clamp(n));
 return(FMT_CUT);
}

int wpe_fmt_shown(char *out, size_t n, const char *fmt, ...)
{
 va_list ap;
 int how;

 va_start(ap, fmt);
 how = wpe_fmt_vshown(out, n, fmt, ap);
 va_end(ap);
 return(how);
}

int wpe_fmt_shown_tail(char *out, size_t n, const char *s)
{
 size_t len = strlen(s), mark = strlen(WPE_FMT_CUT_MARK), keep, start;
 int steps = 0;

 if (n == 0)
  return(FMT_CUT);
 if (len < n)
 {
  memcpy(out, s, len + 1);
  return(0);
 }
 keep = (n > mark + 1) ? n - 1 - mark : 0;
 if (n <= mark)
  keep = n - 1;
 start = len - keep;
 while (start < len && steps++ < FMT_MAX_TRAIL &&
        e_utf8_is_continuation((unsigned char) s[start]))
  start++;
 out[0] = '\0';
 if (n > mark)
  memcpy(out, WPE_FMT_CUT_MARK, mark + 1);
 memcpy(out + strlen(out), s + start, len - start + 1);
 return(FMT_CUT);
}

char *wpe_fmt_vdup(const char *fmt, va_list ap)
{
 va_list measure;
 size_t need;
 char *text;
 int how;

 va_copy(measure, ap);
 how = fmt_write(NULL, 0, &need, fmt, measure);
 va_end(measure);
 if (how != WPE_FMT_OK || need >= WPE_FMT_HEAP_LIMIT)
  return(NULL);
 text = malloc(need + 1);
 if (!text)
  return(NULL);
 if (wpe_fmt_vwhole(text, need + 1, fmt, ap) != WPE_FMT_OK)
 {
  free(text);
  return(NULL);
 }
 return(text);
}

char *wpe_fmt_dup(const char *fmt, ...)
{
 va_list ap;
 char *text;

 va_start(ap, fmt);
 text = wpe_fmt_vdup(fmt, ap);
 va_end(ap);
 return(text);
}

/**
 * fmt_begin - Set a text built from pieces to empty, of the given kind.
 * @out, @n: the storage of a fixed text (NULL and 0 for a heap one).
 */
static void fmt_begin(WpeFmtText *t, char *out, size_t n, int kind)
{
 t->out = out;
 t->n = n;
 t->len = 0;
 t->kind = kind;
 t->state = WPE_FMT_OK;
 if (out && n > 0)
  out[0] = '\0';
}

void wpe_fmt_begin_whole(WpeFmtText *t, char *out, size_t n)
{
 fmt_begin(t, out, n, FMT_KIND_WHOLE);
}

void wpe_fmt_begin_shown(WpeFmtText *t, char *out, size_t n)
{
 fmt_begin(t, out, n, FMT_KIND_SHOWN);
}

void wpe_fmt_begin_block(WpeFmtText *t, char *out, size_t n)
{
 fmt_begin(t, out, n, FMT_KIND_BLOCK);
}

void wpe_fmt_begin_heap(WpeFmtText *t)
{
 fmt_begin(t, NULL, 0, FMT_KIND_HEAP);
}

/**
 * fmt_resume - Take the text already in @out as the start of a fixed text.
 */
static void fmt_resume(WpeFmtText *t, char *out, size_t n, int kind)
{
 const char *end = n ? memchr(out, '\0', n) : NULL;
 size_t have = end ? (size_t) (end - out) : (n ? n - 1 : 0);

 t->out = out;
 t->n = n;
 t->len = have;
 t->kind = kind;
 t->state = WPE_FMT_OK;
 if (n > 0)
  out[have] = '\0';
}

void wpe_fmt_resume_shown(WpeFmtText *t, char *out, size_t n)
{
 size_t mark = strlen(WPE_FMT_CUT_MARK);

 fmt_resume(t, out, n, FMT_KIND_SHOWN);
 /* A sentence already cut and marked is complete: it stays closed. */
 if (t->len >= mark && strcmp(out + t->len - mark, WPE_FMT_CUT_MARK) == 0)
  t->state = FMT_CUT;
}

size_t wpe_fmt_len(const WpeFmtText *t)
{
 return(t->len);
}

int wpe_fmt_failed(const WpeFmtText *t)
{
 return(t->state != WPE_FMT_OK && t->state != FMT_CUT);
}

void wpe_fmt_resume_whole(WpeFmtText *t, char *out, size_t n)
{
 fmt_resume(t, out, n, FMT_KIND_WHOLE);
}

size_t wpe_fmt_room(const WpeFmtText *t)
{
 if (t->state != WPE_FMT_OK || t->kind == FMT_KIND_HEAP || t->n == 0)
  return(0);
 return(t->n - 1 - t->len);
}

/**
 * fmt_heap_room - Make a heap text able to take @more bytes and a NUL.
 * Return: 0, or -1 with the text marked out of memory.
 */
static int fmt_heap_room(WpeFmtText *t, size_t more)
{
 size_t want, room = t->n ? t->n : FMT_HEAP_FIRST;
 char *grown;

 if (more >= WPE_FMT_HEAP_LIMIT || t->len >= WPE_FMT_HEAP_LIMIT - more)
 {
  t->state = WPE_FMT_NO_MEMORY;
  return(-1);
 }
 want = t->len + more + 1;
 if (want <= t->n)
  return(0);
 while (room < want)
  room *= 2;
 grown = realloc(t->out, room);
 if (!grown)
 {
  t->state = WPE_FMT_NO_MEMORY;
  return(-1);
 }
 t->out = grown;
 t->n = room;
 return(0);
}

/**
 * fmt_piece_did_not_fit - Close a fixed text at the piece that was too long.
 * A whole text stops where it was; a shown one keeps the part of the piece
 * the library wrote and gets its mark at the end.
 */
static void fmt_piece_did_not_fit(WpeFmtText *t)
{
 if (t->kind == FMT_KIND_SHOWN)
 {
  fmt_mark_cut(t->out, fmt_clamp(t->n));
  t->len = strlen(t->out);
  t->state = FMT_CUT;
  return;
 }
 t->out[t->len] = '\0';
 t->state = WPE_FMT_NO_ROOM;
}

/**
 * fmt_make_room - Get the storage ready for a piece of @need bytes.
 * A heap text grows; a fixed text without storage fails.
 * Return: 0 when the piece may be written, -1 when it may not.
 */
static int fmt_make_room(WpeFmtText *t, size_t need)
{
 if (t->kind == FMT_KIND_HEAP)
  return(fmt_heap_room(t, need));
 if (t->n == 0)
 {
  t->state = WPE_FMT_NO_ROOM;
  return(-1);
 }
 return(0);
}

/**
 * fmt_vadd_piece - Write one piece after the text's end.
 * @allow_cut: false for a block, which leaves the text as it was when the
 * piece does not fit and stays open.
 * Return: 1 when the piece was added whole, 0 when not.
 */
static int fmt_vadd_piece(WpeFmtText *t, const char *fmt, va_list ap, int allow_cut)
{
 va_list measure;
 size_t need, room;
 int how;

 if (t->state != WPE_FMT_OK)
  return(0);
 va_copy(measure, ap);
 how = fmt_write(NULL, 0, &need, fmt, measure);
 va_end(measure);
 if (how != WPE_FMT_OK)
 {
  t->state = how;
  return(0);
 }
 if (fmt_make_room(t, need) != 0)
  return(0);
 room = t->n - t->len;
 how = fmt_write(t->out + t->len, room, &need, fmt, ap);
 if (how != WPE_FMT_OK)
 {
  t->out[t->len] = '\0';
  t->state = how;
  return(0);
 }
 if (need < fmt_clamp(room))
 {
  t->len += need;
  return(1);
 }
 if (allow_cut)
  fmt_piece_did_not_fit(t);
 else
  t->out[t->len] = '\0';
 return(0);
}

void wpe_fmt_vadd(WpeFmtText *t, const char *fmt, va_list ap)
{
 (void) fmt_vadd_piece(t, fmt, ap, t->kind != FMT_KIND_BLOCK);
}

void wpe_fmt_add(WpeFmtText *t, const char *fmt, ...)
{
 va_list ap;

 va_start(ap, fmt);
 wpe_fmt_vadd(t, fmt, ap);
 va_end(ap);
}

int wpe_fmt_add_block(WpeFmtText *t, const char *fmt, ...)
{
 va_list ap;
 int added;

 va_start(ap, fmt);
 added = fmt_vadd_piece(t, fmt, ap, 0);
 va_end(ap);
 return(added);
}

void wpe_fmt_add_bytes(WpeFmtText *t, const char *bytes, size_t len)
{
 size_t keep;

 if (t->state != WPE_FMT_OK)
  return;
 if (fmt_make_room(t, len) != 0)
  return;
 if (len < t->n - t->len)
 {
  memcpy(t->out + t->len, bytes, len);
  t->len += len;
  t->out[t->len] = '\0';
  return;
 }
 if (t->kind == FMT_KIND_BLOCK)
  return;
 if (t->kind == FMT_KIND_SHOWN)
 {
  keep = t->n - 1 - t->len;
  memcpy(t->out + t->len, bytes, keep);
  t->out[t->len + keep] = '\0';
 }
 fmt_piece_did_not_fit(t);
}

void wpe_fmt_add_shown(WpeFmtText *t, size_t room, const char *s)
{
 char *piece;
 size_t need = strlen(s) + 1;

 if (t->state != WPE_FMT_OK)
  return;
 if (need <= room + 1)
 {
  wpe_fmt_add_bytes(t, s, need - 1);
  return;
 }
 piece = malloc(room + 1);
 if (!piece)
 {
  t->state = WPE_FMT_NO_MEMORY;
  return;
 }
 (void) wpe_fmt_shown(piece, room + 1, "%s", s);
 wpe_fmt_add_bytes(t, piece, strlen(piece));
 free(piece);
}

int wpe_fmt_end(WpeFmtText *t, size_t *len)
{
 if (t->kind == FMT_KIND_HEAP)
 {
  if (len)
   *len = 0;
  return(WPE_FMT_FAILED);
 }
 if (t->state == WPE_FMT_NO_ROOM || t->state == WPE_FMT_FAILED)
 {
  if (t->n > 0)
   t->out[0] = '\0';
  t->len = 0;
 }
 if (len)
  *len = t->len;
 return(t->state);
}

char *wpe_fmt_end_heap(WpeFmtText *t, size_t *len)
{
 char *text = t->out;

 if (t->kind != FMT_KIND_HEAP)
 {
  if (len)
   *len = 0;
  return(NULL);
 }
 if (t->state == WPE_FMT_OK && !text)
  text = calloc(1, 1);
 if (t->state != WPE_FMT_OK || !text)
 {
  free(t->out);
  text = NULL;
 }
 if (len)
  *len = text ? t->len : 0;
 t->out = NULL;
 t->n = 0;
 t->len = 0;
 return(text);
}

int wpe_fmt_cut_len(const char *s, size_t max)
{
 return((int) fmt_keep_whole_chars(s, strlen(s), max));
}
