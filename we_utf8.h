/* we_utf8.h -- what a character of a row is: the one rule.
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later.

   The keys, the screen, the mouse and the owner of the text's positions all
   read a row of bytes by this rule, so a key step, a drawn cell, a clicked
   cell and a cursor column always cover the same bytes.  The functions are
   static inline, one source for every file that includes this header, so the
   owner of the text (we_buffer.c) and its C test can use the rule without
   linking the editor. */

#ifndef WE_UTF8_H
#define WE_UTF8_H

#include <wchar.h>

/* The C test that links the owner of the text alone is built before the
   feature macros that declare wcwidth are set; the declaration is the
   library's own, so repeating it is harmless where it is already there. */
int wcwidth(wchar_t c);

/* True for a UTF-8 lead byte (start of a 2..4 byte sequence). */
#define UTF8_IS_LEAD(c) ((c) >= 0xC0 && (c) < 0xFE)

/* The most bytes one UTF-8 character takes. */
#define E_UTF8_MAX_BYTES 4

/* The smallest code point each sequence length may carry (a shorter form of
   it is an overlong encoding), the surrogates, and the last code point. */
#define E_UTF8_MIN_2_BYTES   0x80
#define E_UTF8_MIN_3_BYTES   0x800
#define E_UTF8_MIN_4_BYTES   0x10000
#define E_UTF8_SURROGATE_LO  0xD800
#define E_UTF8_SURROGATE_HI  0xDFFF
#define E_UTF8_LAST_CODEPOINT 0x10FFFF

/**
 * e_utf8_charlen - Bytes a character takes by its first byte alone: 1 for
 * ASCII and for a byte that starts no character, 2 to 4 for a lead byte.
 */
static inline int e_utf8_charlen(unsigned char c)
{
 if (c < 0x80) return 1;
 if ((c & 0xE0) == 0xC0) return 2;
 if ((c & 0xF0) == 0xE0) return 3;
 if ((c & 0xF8) == 0xF0) return 4;
 return 1;
}

/**
 * e_utf8_seq_len - Bytes of the character that starts at @s, when only @avail
 * bytes of the string may be read.
 * @s: the first byte of the character; @avail: the bytes it may read, or
 * E_UTF8_UNBOUNDED for a string that ends in a NUL byte.
 *
 * A text that ends inside a character (a file, a field or a paste cut after
 * the first byte of an `€`) never makes a caller read or step past its end:
 * a lead byte that is not followed by all its continuation bytes is one byte
 * long, and is drawn as the replacement glyph.
 * Return: 1 to 4.
 */
static inline int e_utf8_seq_len(const unsigned char *s, int avail)
{
 int n = e_utf8_charlen(s[0]), i;

 if (n > avail)
  return(1);
 for (i = 1; i < n; i++)
  if ((s[i] & 0xC0) != 0x80)
   return(1);
 return(n);
}

/**
 * e_utf8_is_continuation - Is @c a continuation byte (10xxxxxx), never the
 * start of a character?
 */
static inline int e_utf8_is_continuation(unsigned char c)
{
 return((c & 0xC0) == 0x80);
}

/**
 * e_utf8_glyph_floor - The first byte of the glyph that covers column @x of
 * the row @s of @len bytes: @x itself when a glyph starts there or the row
 * ends there.
 *
 * A byte that is no continuation byte always starts a glyph.  A
 * continuation byte starts one unless a lead byte up to three bytes before
 * it has all its continuation bytes in the row and so covers it: the
 * answer e_utf8_char_before gives going left, so the cursor's steps and this
 * test agree byte for byte.  A column past the row's end is the end.  The
 * owner of the text's positions and the owner of a search both ask it where
 * a character starts, so a mark and a hit never disagree about one.
 * Return: the column of the glyph's first byte, 0 to @len.
 */
static inline int e_utf8_glyph_floor(const unsigned char *s, int len, int x)
{
 int k, lead;

 if (x >= len)
  return(len);
 if (x <= 0 || !e_utf8_is_continuation(s[x]))
  return(x < 0 ? 0 : x);
 for (k = 1; k < E_UTF8_MAX_BYTES && k <= x; k++)
 {
  lead = x - k;
  if (e_utf8_seq_len(s + lead, len - lead) > k)
   return(lead);
 }
 return(x);
}

/**
 * e_utf8_to_codepoint - The code point of the @len bytes at @buf, or -1 when
 * they are not a sequence of a lead byte and its continuation bytes.
 */
static inline int e_utf8_to_codepoint(unsigned char *buf, int len)
{
 if (len >= 2 && (buf[0] & 0xE0) == 0xC0)
  return ((buf[0] & 0x1F) << 6) | (buf[1] & 0x3F);
 if (len >= 3 && (buf[0] & 0xF0) == 0xE0)
  return ((buf[0] & 0x0F) << 12) | ((buf[1] & 0x3F) << 6) | (buf[2] & 0x3F);
 if (len >= 4 && (buf[0] & 0xF8) == 0xF0)
  return ((buf[0] & 0x07) << 18) | ((buf[1] & 0x3F) << 12) |
         ((buf[2] & 0x3F) << 6) | (buf[3] & 0x3F);
 return -1;
}

/**
 * e_utf8_scalar_is_valid - Is @cp, decoded from @n bytes, a character UTF-8
 * allows in that many bytes?
 *
 * Bytes of the right shape can still be no character: an overlong form
 * (C0 80), a surrogate (ED A0 80) or a lead above F4.  They are not shown
 * as a character.
 */
static inline int e_utf8_scalar_is_valid(int cp, int n)
{
 if (n == 2)
  return(cp >= E_UTF8_MIN_2_BYTES);
 if (n == 3)
  return(cp >= E_UTF8_MIN_3_BYTES &&
         (cp < E_UTF8_SURROGATE_LO || cp > E_UTF8_SURROGATE_HI));
 return(cp >= E_UTF8_MIN_4_BYTES && cp <= E_UTF8_LAST_CODEPOINT);
}

/**
 * e_utf8_glyph_at - The glyph that starts at @s: its bytes, what to draw for
 * it and how many columns it takes.
 * @s: the first byte; @avail: the bytes that may be read.
 * @codepoint: receives the character, or 0 when the bytes are no character
 * the terminal can show (a stray byte, a cut sequence, or bytes of a good
 * shape that are invalid as UTF-8: an overlong form, a surrogate, a lead
 * above F4); the caller then draws its replacement.
 * @cols: receives the columns the glyph takes, at least 1.
 *
 * The one place that decides what a glyph is, for the keys, the screen, the
 * cursor and the mouse alike: the bytes are one glyph exactly when
 * e_utf8_seq_len says so, so a key step, a drawn cell and a clicked cell
 * always cover the same bytes, whatever the file holds.
 * Return: the bytes of the glyph, 1 to 4.
 */
static inline int e_utf8_glyph_at(const unsigned char *s, int avail,
                                  int *codepoint, int *cols)
{
 int n = e_utf8_seq_len(s, avail);
 int cp, w;

 *codepoint = 0;
 *cols = 1;
 if (n < 2)
  return(n);
 cp = e_utf8_to_codepoint((unsigned char *)s, n);
 if (!e_utf8_scalar_is_valid(cp, n))
  return(n);
 *codepoint = cp;
 w = wcwidth((wchar_t)cp);
 if (w > 1)
  *cols = w;
 return(n);
}

/**
 * e_utf8_visual_step - Walk one glyph of a row while counting screen columns.
 * @s: the row; @i: the byte the glyph starts at; @len: the row's bytes.
 * @j: the columns the row has taken beyond one per byte so far.
 *
 * A multi-byte glyph is one cursor step per byte but only its display width
 * wide, and a byte that is no character is one column: this adds the
 * glyph's (columns - bytes) to *@j.
 * Return: the number of continuation bytes the caller's loop skips, 0 for a
 * glyph of one byte.
 */
static inline int e_utf8_visual_step(unsigned char *s, int i, int len, int *j)
{
 int codepoint, cols;
 int nb = e_utf8_glyph_at(s + i, len - i, &codepoint, &cols);

 *j += cols - nb;
 return nb - 1;
}

/**
 * e_utf8_char_before - Where the character that ends just before @pos
 * starts, read by the same rule as e_utf8_seq_len.
 * @s: the text; @pos: a character boundary, 1 or more; @len: the bytes of
 * @s that may be read.
 *
 * Left and Backspace step over exactly what Right and Delete step over
 * and what the screen draws as one glyph: a whole character, or one byte
 * of a cut one (a lead byte without all its continuation bytes, a stray
 * continuation byte, a Latin-1 letter).  Every byte that is not a
 * continuation starts a character, so the walk looks back at most to the
 * lead byte the character could have.
 * Return: the first byte of that character, from 0 to @pos - 1.
 */
static inline int e_utf8_char_before(const unsigned char *s, int pos, int len)
{
 int lead = pos - 1;

 while (lead > 0 && pos - lead < E_UTF8_MAX_BYTES && e_utf8_is_continuation(s[lead]))
  lead--;
 if (e_utf8_is_continuation(s[lead]))
  return(pos - 1);
 if (lead + e_utf8_seq_len(s + lead, len - lead) < pos)
  return(pos - 1);
 return(lead);
}

#endif
