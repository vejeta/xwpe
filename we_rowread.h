/* we_rowread.h                                           */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

/* The readers of a row.  A buffer has exactly mxlines rows, and a row holds
   len + 2 bytes from s[0]: the text, the line break, a NUL.  Nothing lies
   before s[0] and nothing behind the NUL.  Every walk of the editor that
   reads the bytes of a row (the bracket matcher, the painters, the syntax
   scanner, the beautifier) asks these functions for them, and a position the
   text does not have reads as "no character": no bracket, no quote, no
   slash, no hash, no space, no letter, so no walk acts on it and a step that
   ran off the text ends the walk instead of reading memory the row does not
   own.

   They are static inline, in a header of their own, so that a unit which
   links one walker (test_brkmatch, test_rowread) needs no object of the
   editor, and so that who may write a row (we_buffer.c) stays one file.
   Each call indexes afresh: the beautifier deletes and inserts inside the row
   it is walking, and a pointer kept across such a call would be stale.
   tests/check-brk-bounds.sh keeps every other file from indexing a row. */

#ifndef WE_ROWREAD_H
#define WE_ROWREAD_H

#include "edit.h"

/* What a read outside the text gives. */
#define WPE_ROW_NONE   (-1)

/**
 * wpe_row_has - Whether the text has a row @y.
 * The one place a walk learns where the text begins and ends: a loop runs
 * while the row it is on exists, and a walk that a skip left above row 0 or
 * below the last row stops at its next test.
 */
static inline int wpe_row_has(const BUFFER *b, int y)
{
 return y >= 0 && y < b->mxlines;
}

/**
 * wpe_rows_exist - Whether the buffer has its rows at all (a window being
 * built or closed has none yet or no more).
 */
static inline int wpe_rows_exist(const BUFFER *b)
{
 return b->bf != NULL;
}

/**
 * wpe_row_stored - Whether row @y exists and holds its bytes.
 * A row a buffer is still building (or has released) has no bytes yet; a dump
 * or a copy of the text asks this before it reads any.
 */
static inline int wpe_row_stored(const BUFFER *b, int y)
{
 return wpe_row_has(b, y) && b->bf[y].s != NULL;
}

/**
 * wpe_row_len - The length of a row's text, 0 when the text has no row @y.
 * A row the text does not have is empty, so a walk left there has nothing to
 * read.
 */
static inline int wpe_row_len(const BUFFER *b, int y)
{
 if (!wpe_row_has(b, y))
  return 0;
 return b->bf[y].len;
}

/**
 * wpe_row_nrc - The bytes of a row's text with its line break, the second
 * count the row owner keeps, 0 when the text has no row @y.
 * A walk that joins rows into lines asks it where a row ends its line (the
 * count is above the length) and where it is only a segment the editor
 * wrapped.
 */
static inline int wpe_row_nrc(const BUFFER *b, int y)
{
 if (!wpe_row_has(b, y))
  return 0;
 return b->bf[y].nrc;
}

/**
 * wpe_row_byte - The byte at (@y, @x), WPE_ROW_NONE when there is none.
 * @x: the column, 0..len+1 to read the text, the line break or the NUL
 * The one way a step looks at a character.  A column before the row or past
 * its NUL, or a row the buffer does not have, is WPE_ROW_NONE: a slash at
 * column 0 cannot close a comment, and a skip that ran off the text has
 * nothing left to read.
 */
static inline int wpe_row_byte(const BUFFER *b, int y, int x)
{
 if (!wpe_row_has(b, y))
  return WPE_ROW_NONE;
 if (x < 0 || x > b->bf[y].len + 1)
  return WPE_ROW_NONE;
 return b->bf[y].s[x];
}

/**
 * wpe_row_text - The text of a row from a column on, as a C string; "" when
 * the text has no such place.
 * For a test that reads a few bytes of a row (ispif, strncmp): it gets "",
 * which is no directive and no word, outside the text.  The result is a
 * row's start for a function that stops at the NUL, not a base for pointer
 * arithmetic.
 */
static inline const char *wpe_row_text(const BUFFER *b, int y, int x)
{
 if (!wpe_row_has(b, y))
  return "";
 if (x < 0 || x > b->bf[y].len + 1)
  return "";
 return (const char *) b->bf[y].s + x;
}

/**
 * wpe_row_bytes - A row's bytes, and its length in @len when that is not NULL.
 * For a function that takes the row and its length (the syntax scanner, a
 * search); "" and 0 when the text has no row @y.  A caller that has its own
 * wpe_row_len passes NULL.
 */
static inline const unsigned char *wpe_row_bytes(const BUFFER *b, int y, int *len)
{
 if (len)
  *len = wpe_row_len(b, y);
 return (const unsigned char *) wpe_row_text(b, y, 0);
}

/**
 * wpe_row_from - A row's bytes from column @x on, for a function that takes
 * them with their count (what follows a cut, the tail of a line).
 * The count is wpe_row_len(b, y) - @x; "" when @x is outside the text.  Like
 * wpe_row_bytes it is handed on whole and never indexed: to a function, or
 * into the one piece of a line the row owner is about to build.
 */
static inline const unsigned char *wpe_row_from(const BUFFER *b, int y, int x)
{
 return (const unsigned char *) wpe_row_text(b, y, x);
}

/**
 * wpe_row_cstr - A row as a C string for an array of rows that a function
 * joins or walks, NULL when the row has no bytes.
 * For the language server's copy of the text: the array holds one entry per
 * row and the join stops each at its line break.  The entry is the row's own
 * bytes, good until the text changes, and it is const, so an array that keeps
 * it can read the text but cannot change it.
 */
static inline const char *wpe_row_cstr(const BUFFER *b, int y)
{
 if (!wpe_row_stored(b, y))
  return NULL;
 return (const char *) b->bf[y].s;
}

/**
 * wpe_row_find - The column where @needle first occurs in row @y, WPE_ROW_NONE
 * when it does not (or the text has no such row).
 * For a jump to the word a compiler message quoted: the cursor goes to the
 * column, and a row without the word leaves the caller to choose.
 */
static inline int wpe_row_find(const BUFFER *b, int y, const char *needle)
{
 const char *row = wpe_row_text(b, y, 0);
 const char *hit = strstr(row, needle);

 if (hit == NULL)
  return WPE_ROW_NONE;
 return (int) (hit - row);
}

/**
 * wpe_row_view - A row as a walker takes it: its bytes in @s and its length
 * in @len ("" and 0 outside the text).
 * For a walk that keeps one entry for each row of the text (the compiler
 * message matcher) and reads them later; the bytes are the row's own, so the
 * entry is good until the text changes.  Because the row's address leaves the
 * statement here, the walk that takes it is one the row check names: nothing
 * else may keep a row.
 */
static inline void wpe_row_view(const BUFFER *b, int y, const char **s, int *len)
{
 *s = wpe_row_text(b, y, 0);
 *len = wpe_row_len(b, y);
}

/**
 * wpe_row_ends_line - Whether row @y ends in a line break (not a soft row
 * the next one continues).  A row the text does not have ends nothing.
 */
static inline int wpe_row_ends_line(const BUFFER *b, int y)
{
 return wpe_row_byte(b, y, wpe_row_len(b, y)) == WPE_WR;
}

/**
 * wpe_row_glyph_at - The glyph of a row that starts at column @x, by the
 * rule the keys, the cursor and the screen share.
 * @cp: receives the character to draw, 0 when the bytes are none the
 * terminal can show; @cols: receives the columns it takes.
 * The painters ask it for a UTF-8 lead byte, so a glyph is never read past
 * the end of the row's text.
 * Return: the bytes of the glyph, 1 when @x is not inside the row's text.
 */
static inline int wpe_row_glyph_at(const BUFFER *b, int y, int x, int *cp, int *cols)
{
 if (!wpe_row_has(b, y) || x < 0 || x >= b->bf[y].len)
 {
  *cp = 0;
  *cols = 1;
  return 1;
 }
 return e_utf8_glyph_at(b->bf[y].s + x, b->bf[y].len - x, cp, cols);
}

#endif /* WE_ROWREAD_H */
