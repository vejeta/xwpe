/* we_brkmatch.c                                          */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

/* The bracket matcher.  "Matching BracKet" jumps from a brace, parenthesis
   or bracket to its partner, and from anything else back to the enclosing
   opener.  On the way it must not be fooled by a bracket inside quoted text,
   a comment, a // line or the unused side of an #else.  e_brk_match() holds
   the two walks (forward from an opener, backward from a closer); each of
   them calls one named step per character, and the steps that skip quoted
   text, a comment or an #else side are shared by both directions.

   The four readers just below (brk_has_row, brk_row_len, brk_char,
   brk_row_text) are the only lines of the file that index a row, index its
   bytes or know how many rows the text has.  A row holds len+2 bytes from
   s[0] (the text, the line break, a NUL) and there is nothing before it;
   a read outside the text is "no character", so a skip that leaves a walk
   off the text ends the walk instead of reading memory the row does not
   own. */

#include <ctype.h>
#include <string.h>

#include "messages.h"
#include "edit.h"

/* What one step of a walk tells the loop that called it. */
#define BRK_MORE       0   /* go on with the next character */
#define BRK_LINE_DONE  1   /* the rest of this line is a // comment */
#define BRK_FOUND      2   /* the partner is at (i, j) */

/* Which way a walk reads: the sign added to the row index. */
#define BRK_FORWARD    1
#define BRK_BACKWARD  (-1)

/* The position of a walk and what it is looking for. */
struct brk_scan {
 BUFFER *b;
 int i, j;      /* the row and column of the character being read */
 int ob, cb;    /* the pair being matched; both 0: any opener or closer */
 int depth;     /* openers minus closers still to be matched */
};

/* What a read outside the text gives: not a bracket, not a quote, not a
   slash, not a hash, not a space, so no step acts on it. */
#define BRK_NONE      (-1)

/**
 * brk_has_row - Whether the text has a row @i.
 * The one place the walks learn where the text begins and ends: the loops
 * run while the row they are on exists, and a skip over an #else side goes
 * on while there is a row beyond the one it reads.  A walk that a skip left
 * above row 0 or below the last row stops at its next test.
 */
static int brk_has_row(BUFFER *b, int i)
{
 return i >= 0 && i < b->mxlines;
}

/**
 * brk_row_len - The length of a row's text, 0 when the text has no row @i.
 * A row the text does not have is empty, so a walk left there has nothing to
 * read.
 */
static int brk_row_len(BUFFER *b, int i)
{
 if (!brk_has_row(b, i))
  return 0;
 return b->bf[i].len;
}

/**
 * brk_char - The byte at (i, j), BRK_NONE when there is none.
 * @j: the column, 0..len+1 to read the text, the line break or the NUL
 * The one way a step looks at a character.  A column before the row or past
 * its NUL, or a row the buffer does not have, is BRK_NONE: a slash at column
 * 0 cannot close a comment, and a skip that ran off the text has nothing
 * left to read.
 */
static int brk_char(BUFFER *b, int i, int j)
{
 if (!brk_has_row(b, i))
  return BRK_NONE;
 if (j < 0 || j > b->bf[i].len + 1)
  return BRK_NONE;
 return b->bf[i].s[j];
}

/**
 * brk_row_text - The text of a row from a column on, as a C string.
 * For the preprocessor tests (ispif, ispelse, ispendif) that read a few bytes
 * of a row; outside the text they get "", which is no directive.
 */
static const char *brk_row_text(BUFFER *b, int i, int j)
{
 if (!brk_has_row(b, i))
  return "";
 if (j < 0 || j > b->bf[i].len + 1)
  return "";
 return (const char *) b->bf[i].s + j;
}

/**
 * brk_pair_of - Name the pair of brackets a character belongs to.
 * @c: the character under the cursor
 * @ob: receives the opening character of the pair
 * @cb: receives the closing character of the pair
 *
 * Lets a jump from "{", "(" or "[" look for "}", ")" or "]" and the other way
 * round.  Return: 1 when @c is one of the six brackets, 0 when it is not.
 */
static int brk_pair_of(int c, int *ob, int *cb)
{
 if (c == '{' || c == '}')
 {
  *ob = '{';  *cb = '}';
  return 1;
 }
 if (c == '(' || c == ')')
 {
  *ob = '(';  *cb = ')';
  return 1;
 }
 if (c == '[' || c == ']')
 {
  *ob = '[';  *cb = ']';
  return 1;
 }
 *ob = 0;  *cb = 0;
 return 0;
}

/**
 * brk_is_opener - Tell whether a character opens a bracket pair.
 * @c: the character under the cursor
 *
 * Decides the direction of the jump: an opener looks forward for its closer,
 * anything else looks backward.
 */
static int brk_is_opener(int c)
{
 return c == '{' || c == '(' || c == '[';
}

/**
 * brk_skip_quoted_forward - Step over quoted text so its brackets are ignored.
 * @sc: the walk, standing on the opening quote
 * @quote: the quote character, '"' or '\''
 *
 * A bracket written inside "..." or '...' is not a bracket of the program.
 * A backslash at the end of a line carries the quote on to the next line.
 * The walk is left on the closing quote, or at the end of the row.
 */
static void brk_skip_quoted_forward(struct brk_scan *sc, int quote)
{
 BUFFER *b = sc->b;
 int bsp;

 for (bsp = 0, sc->j++;
   sc->j < brk_row_len(b, sc->i)
     && (brk_char(b, sc->i, sc->j) != quote || bsp);
   sc->j++)
 {
  if (brk_char(b, sc->i, sc->j) == '\\')
   bsp = !bsp;
  else
   bsp = 0;
  if (sc->j == brk_row_len(b, sc->i) - 1 && bsp && brk_has_row(b, sc->i + 1))
  {
   sc->i++;
   sc->j = -1;
   bsp = 0;
  }
 }
}

/**
 * brk_skip_comment_forward - Step over a slash-star comment so its brackets
 *                            are ignored.
 * @sc: the walk, standing on the slash that opens the comment
 *
 * The comment may run over several lines.  The walk is left on the closing
 * star, or at the end of the text when the comment is never closed.
 */
static void brk_skip_comment_forward(struct brk_scan *sc)
{
 BUFFER *b = sc->b;

 for (sc->j += 2;
   brk_char(b, sc->i, sc->j) != '*' || brk_char(b, sc->i, sc->j + 1) != '/';
   sc->j++)
 {
  if (sc->j >= brk_row_len(b, sc->i) - 1)
  {
   if (brk_has_row(b, sc->i + 1))
   {
    sc->i++;
    sc->j = -1;
   }
   else
    break;
  }
 }
}

/**
 * brk_skip_quoted_backward - Step back over quoted text so its brackets are
 *                            ignored.
 * @sc: the walk, standing on the closing quote
 * @quote: the quote character, '"' or '\''
 *
 * The mirror of brk_skip_quoted_forward(): a quote preceded by an odd number
 * of backslashes does not end the text, and a line that ends in a backslash
 * continues on the row above.
 */
static void brk_skip_quoted_backward(struct brk_scan *sc, int quote)
{
 BUFFER *b = sc->b;
 int bsp;

 for (sc->j--; sc->j >= 0; sc->j--)
 {
  if (brk_char(b, sc->i, sc->j) == quote)
  {
   for (bsp = 0, sc->j--;
     sc->j >= 0 && brk_char(b, sc->i, sc->j) == '\\';
     sc->j--)
    bsp = !bsp;
   sc->j++;
   if (!bsp)
    break;
  }
  if (sc->j == 0 && brk_has_row(b, sc->i - 1)
    && brk_char(b, sc->i - 1, brk_row_len(b, sc->i - 1)) == '\\')
  {
   sc->i--;
   sc->j = brk_row_len(b, sc->i);
  }
 }
}

/**
 * brk_skip_comment_backward - Step back over a slash-star comment so its
 *                             brackets are ignored.
 * @sc: the walk, standing on the slash that closes the comment
 *
 * The comment may start on an earlier line.  The walk is left on its opening
 * slash, or at the start of the text when it is never opened.
 */
static void brk_skip_comment_backward(struct brk_scan *sc)
{
 BUFFER *b = sc->b;

 for (sc->j -= 2;
   brk_char(b, sc->i, sc->j) != '*' || brk_char(b, sc->i, sc->j - 1) != '/';
   sc->j--)
 {
  if (sc->j <= 0)
  {
   if (brk_has_row(b, sc->i - 1))
   {
    sc->i--;
    sc->j = brk_row_len(b, sc->i);
   }
   else
    break;
  }
 }
}

/**
 * brk_skip_conditional - Step over the other side of an #if block.
 * @sc: the walk, standing in a row that starts #else or #elif
 * @dir: BRK_FORWARD to the matching #endif below, BRK_BACKWARD to the #if above
 *
 * Only one side of an #if is compiled, so a bracket in the other side must not
 * be counted: a jump from the "{" before an #else lands on the "}" of the same
 * side, not on one past the #endif.  Nested #if blocks are skipped whole.
 * The skip reads a row only while a row lies beyond it, so the first or last
 * row is left to the walk.  With no #endif (or #if) in the way the walk is
 * left off the text and ends there.
 */
static void brk_skip_conditional(struct brk_scan *sc, int dir)
{
 BUFFER *b = sc->b;
 int nif = 1;

 for (sc->i += dir; brk_has_row(b, sc->i + dir); sc->i += dir)
 {
  for (sc->j = 0; isspace(brk_char(b, sc->i, sc->j)); sc->j++)
   ;
  if (ispendif(brk_row_text(b, sc->i, sc->j)))
   nif -= dir;
  else if (ispif(brk_row_text(b, sc->i, sc->j)))
   nif += dir;
  if (!nif)
   break;
 }
}

/**
 * brk_step_forward - Read one character of the walk from an opener to its
 *                    closer.
 * @sc: the walk; its position is the character to read and it moves on
 *
 * Skips quoted text, comments and the unused side of an #else, and counts
 * the pair being matched.  Return: BRK_FOUND when the closer that balances
 * the opener is reached, BRK_LINE_DONE when the rest of the line is a //
 * comment, BRK_MORE otherwise.
 */
static int brk_step_forward(struct brk_scan *sc)
{
 BUFFER *b = sc->b;
 int c = brk_char(b, sc->i, sc->j);

 if (c == '\"' || c == '\'')
  brk_skip_quoted_forward(sc, c);
 else if (c == '/' && brk_char(b, sc->i, sc->j + 1) == '*')
  brk_skip_comment_forward(sc);
 else if (c == '/' && brk_char(b, sc->i, sc->j + 1) == '/')
  return BRK_LINE_DONE;
 else if (c == '#' && ispelse(brk_row_text(b, sc->i, 0)))
  brk_skip_conditional(sc, BRK_FORWARD);
 else if (c == sc->ob)
  sc->depth++;
 else if (c == sc->cb)
 {
  sc->depth--;
  if (!sc->depth)
   return BRK_FOUND;
 }
 return BRK_MORE;
}

/**
 * brk_step_backward_pair - Count one character of a backward walk that looks
 *                          for one named pair.
 * @sc: the walk
 * @c: the character read
 *
 * Return: BRK_FOUND when the opener that balances the closer is reached,
 * BRK_MORE otherwise.
 */
static int brk_step_backward_pair(struct brk_scan *sc, int c)
{
 if (c == sc->ob)
 {
  sc->depth++;
  if (!sc->depth)
   return BRK_FOUND;
 }
 else if (c == sc->cb)
  sc->depth--;
 return BRK_MORE;
}

/**
 * brk_step_backward_any - Count one character of a backward walk from a
 *                         character that is not a bracket.
 * @sc: the walk
 * @c: the character read
 *
 * Any opener of any pair that is not closed on the way is the enclosing one;
 * when there is none the walk ends at the first character of the text.
 * Return: BRK_FOUND at that opener or at the start of the text, BRK_MORE
 * otherwise.
 */
static int brk_step_backward_any(struct brk_scan *sc, int c)
{
 if (brk_is_opener(c))
 {
  sc->depth++;
  if (!sc->depth)
   return BRK_FOUND;
 }
 else if (c == '}' || c == ')' || c == ']')
  sc->depth--;
 else if (sc->i == 0 && sc->j == 0)
  return BRK_FOUND;
 return BRK_MORE;
}

/**
 * brk_step_backward - Read one character of the walk from a closer (or from
 *                     any other character) back to an opener.
 * @sc: the walk; its position is the character to read and it moves back
 *
 * Skips quoted text, comments and the unused side of an #else, and counts
 * the pair being matched.  Return: BRK_FOUND when the partner is reached,
 * BRK_MORE otherwise.
 */
static int brk_step_backward(struct brk_scan *sc)
{
 BUFFER *b = sc->b;
 int c = brk_char(b, sc->i, sc->j);

 if (c == '\"' || c == '\'')
  brk_skip_quoted_backward(sc, c);
 else if (c == '/' && brk_char(b, sc->i, sc->j - 1) == '*')
  brk_skip_comment_backward(sc);
 else if (c == '#' && ispelse(brk_row_text(b, sc->i, 0)))
  brk_skip_conditional(sc, BRK_BACKWARD);
 else if (!sc->ob)
  return brk_step_backward_any(sc, c);
 else
  return brk_step_backward_pair(sc, c);
 return BRK_MORE;
}

/**
 * brk_code_end - Find where the code of a row ends and a // comment begins.
 * @b: the buffer
 * @i: the row
 * @limit: do not look at or past this column
 *
 * A backward walk reads only the code of a row, so a bracket written in a
 * trailing // comment is never taken as the partner.  Return: the column of
 * the first slash of the comment, or @limit when there is none before it.
 */
static int brk_code_end(BUFFER *b, int i, int limit)
{
 int j;

 for (j = 0;
   j < limit && (brk_char(b, i, j) != '/' || brk_char(b, i, j + 1) != '/');
   j++)
  ;
 return j;
}

/**
 * brk_walk_forward - Find the closer that balances the opener under the
 *                    cursor.
 * @sc: the walk, set up on the opener's row with its pair and depth 1
 * @x: the column of the opener
 *
 * Return: 0 with the position left in @sc, -1 when no closer follows.
 */
static int brk_walk_forward(struct brk_scan *sc, int x)
{
 BUFFER *b = sc->b;
 int y = sc->i;

 for (sc->depth = 1, sc->i = y; brk_has_row(b, sc->i); sc->i++)
  for (sc->j = sc->i == y ? x + 1 : 0; sc->j < brk_row_len(b, sc->i); sc->j++)
  {
   int step = brk_step_forward(sc);

   if (step == BRK_FOUND)
    return 0;
   if (step == BRK_LINE_DONE)
    break;
  }
 return -1;
}

/**
 * brk_walk_backward - Find the opener that balances the closer under the
 *                     cursor, or the enclosing opener for any other character.
 * @sc: the walk, set up with its pair (or none) and depth -1
 * @y: the row of the cursor
 * @x: the column of the cursor
 *
 * Return: 0 with the position left in @sc, -1 when there is none.
 */
static int brk_walk_backward(struct brk_scan *sc, int y, int x)
{
 BUFFER *b = sc->b;

 for (sc->depth = -1, sc->i = y; brk_has_row(b, sc->i); sc->i--)
 {
  sc->j = brk_code_end(b, sc->i, sc->i == y ? x : brk_row_len(b, sc->i));
  for (sc->j--; sc->j >= 0; sc->j--)
  {
   if (brk_step_backward(sc) == BRK_FOUND)
    return 0;
  }
 }
 return -1;
}

/**
 * e_brk_match - Find the bracket that goes with the one under the cursor.
 * @b: the text, with the cursor in @b->b
 * @my: receives the row of the partner
 * @mx: receives the column of the partner
 *
 * Behind "Matching BracKet": from "{", "(" or "[" it finds the closer, from
 * "}", ")" or "]" the opener, and from any other character the enclosing
 * opener (or the start of the text), so the cursor can be sent to the top of
 * the block it is in.  Quoted text, comments and the unused side of an #else
 * do not count.  Nothing in @b changes.
 *
 * Return: 0 when the partner is found, -1 when there is none.
 */
int e_brk_match(BUFFER *b, int *my, int *mx)
{
 struct brk_scan sc;
 int c = brk_char(b, b->b.y, b->b.x);
 int found;

 sc.b = b;
 sc.i = b->b.y;
 sc.j = b->b.x;
 brk_pair_of(c, &sc.ob, &sc.cb);
 if (brk_is_opener(c))
  found = brk_walk_forward(&sc, b->b.x);
 else
  found = brk_walk_backward(&sc, b->b.y, b->b.x);
 if (found == 0)
 {
  *my = sc.i;
  *mx = sc.j;
 }
 return found;
}
