/* we_shtok.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

/* The token walkers of the definition finder.  "Find definition" and the
   project scan read a C source one line at a time, and between two tokens
   they must step over blanks, line continuations, comments, quoted text and
   nested braces or parentheses, reading a new line whenever the current one
   runs out.  The five walkers differ only in what they look for and in what
   ends a line; one walk, e_sh_walk(), does the stepping for all of them. */

#include "messages.h"
#include "edit.h"

#define SH_LINE_SIZE 256   /* the size of the caller's line buffer */

/* What each walker looks for.  A walker that scans to a closer counts the
   openers and closers it passes and stops on a @stop character when the
   count is back at @base; a walker that skips blanks has no @stop. */
struct sh_walk_kind {
 int skips_blanks;      /* 1: the next token; 0: scan to a stop character */
 int ends_line_joins;   /* blank walker: the end of a line joins the next */
 int smart_line_end;    /* scan: skip blanks and the unused side of an
                           #else after a line break */
 char open, close;      /* the pair that nests */
 const char *stop;      /* the characters that end a scan at depth @base */
 int base;              /* the depth the scan starts at and stops at */
};

static const struct sh_walk_kind sh_kinds[] = {
 /* E_SH_NEXT_TOKEN: a backslash joins the next line */
 {1, 0, 0, 0, 0, NULL, 0},
 /* E_SH_NEXT_TOKEN_EOL: the end of a line joins the next as well */
 {1, 1, 0, 0, 0, NULL, 0},
 /* E_SH_CLOSE_BRACE */
 {0, 0, 1, '{', '}', "}", 1},
 /* E_SH_LIST_END: the next top-level , ; or ( */
 {0, 0, 0, '{', '}', ",;(", 0},
 /* E_SH_CLOSE_PAREN */
 {0, 0, 1, '(', ')', ")", 1},
};

/**
 * e_sh_next_line - Replace the current line by the next one of the file.
 * @sp: set to the start of the new line.
 * @str: the caller's line buffer; @fp: the open source; @n: lines read so far.
 *
 * Used when a walker reaches the end of a line in the middle of a token
 * list, so that a definition spread over lines is still found.
 *
 * Return: 1, or 0 at the end of the file.
 */
static int e_sh_next_line(char **sp, char *str, E_AFILE *fp, int *n)
{
 (*n)++;
 *sp = str;
 return e_agets(str, SH_LINE_SIZE, fp) != NULL;
}

/**
 * e_sh_skip_comment - Step over a comment, over lines if it spans them.
 * @sp: at the opening slash; left on the closing slash.
 *
 * Return: 1, or 0 when the file ends inside the comment.
 */
static int e_sh_skip_comment(char **sp, char *str, E_AFILE *fp, int *n)
{
 while (!(*sp = strstr(*sp, "*/")))
 {
  if (!e_sh_next_line(sp, str, fp, n))
   return 0;
 }
 (*sp)++;
 return 1;
}

/**
 * e_sh_skip_quoted - Step over a quoted character or string on one line.
 * @sp: at the opening quote; left on the closing quote, or on the last
 *      character when the line ends first.
 * @quote: the quote character.
 *
 * A backslash keeps the next character, so an escaped quote does not end
 * the text.
 */
static void e_sh_skip_quoted(char **sp, int quote)
{
 int bsl = 0;

 while (*++(*sp) && ((*sp)[0] != quote || bsl))
  bsl = **sp == '\\' ? !bsl : 0;
 if (!**sp)
  (*sp)--;
}

/**
 * e_sh_skip_unused_branch - Skip the lines of an #else or #elif branch.
 * @sp: at the first non-blank character of a line; if it is #else or #elif,
 *      the lines up to the matching #endif are read past.
 *
 * Only the branch the first #if took is read, so a brace or parenthesis in
 * the other one does not throw the count off.
 *
 * Return: 1, or 0 when the file ends first.
 */
static int e_sh_skip_unused_branch(char **sp, char *str, E_AFILE *fp, int *n)
{
 if ((*sp)[0] != '#' || !ispelse(*sp))
  return 1;
 do
 {
  if (!e_sh_next_line(sp, str, fp, n))
   return 0;
  if (isspace(**sp))
   while (isspace(*++(*sp)))
    ;
 } while ((*sp)[0] != '#' || !ispendif(*sp));
 return 1;
}

/**
 * e_sh_line_break - Move a scan on to the next line of the source.
 * @k: the kind of walk; @sp: at the end of the line.
 *
 * Return: 1, or 0 when the file ends.
 */
static int e_sh_line_break(const struct sh_walk_kind *k, char **sp,
                           char *str, E_AFILE *fp, int *n)
{
 if (!e_sh_next_line(sp, str, fp, n))
  return 0;
 if (k->smart_line_end)
 {
  if (isspace(**sp))
   while (isspace(*++(*sp)))
    ;
  if (!e_sh_skip_unused_branch(sp, str, fp, n))
   return 0;
 }
 (*sp)--;
 return 1;
}

/**
 * e_sh_skip_blank - Find the next token, over blanks, comments and joins.
 * @k: the kind of walk; @sp: just before where to look.
 *
 * Return: the start of the token, or NULL when the file ends.
 */
static char *e_sh_skip_blank(const struct sh_walk_kind *k, char *sp,
                             char *str, E_AFILE *fp, int *n)
{
 while (1)
 {
  while (isspace(*++sp))
   ;
  if (sp[0] == '\\' || (k->ends_line_joins && (!sp[0] || sp[0] == '\n')))
  {
   if (!e_sh_line_break(k, &sp, str, fp, n))
    return NULL;
  }
  else if (sp[0] == '/' && sp[1] == '*')
  {
   if (!e_sh_skip_comment(&sp, str, fp, n))
    return NULL;
  }
  else
   break;
 }
 return sp;
}

/**
 * e_sh_scan_step - Step over the one construct the scan is standing on.
 * @k: the kind of walk; @sp: at the character; @depth: the nesting count.
 *
 * Return: 1, or 0 when the file ends.
 */
static int e_sh_scan_step(const struct sh_walk_kind *k, char **sp, int *depth,
                          char *str, E_AFILE *fp, int *n)
{
 if (!(*sp)[0] || (*sp)[0] == '\n')
  return e_sh_line_break(k, sp, str, fp, n);
 if ((*sp)[0] == '/' && (*sp)[1] == '*')
  return e_sh_skip_comment(sp, str, fp, n);
 if ((*sp)[0] == k->open)
  (*depth)++;
 else if ((*sp)[0] == k->close)
  (*depth)--;
 else if ((*sp)[0] == '\'' || (*sp)[0] == '\"')
  e_sh_skip_quoted(sp, (*sp)[0]);
 return 1;
}

/**
 * e_sh_scan - Find the character that ends a list, outside nesting and text.
 * @k: the kind of walk; @sp: just before where to look.
 *
 * Return: the stop character, or NULL when the file ends.
 */
static char *e_sh_scan(const struct sh_walk_kind *k, char *sp, char *str,
                       E_AFILE *fp, int *n)
{
 int depth = k->base;

 while ((!*++sp || !strchr(k->stop, *sp)) || depth != k->base)
 {
  if (!e_sh_scan_step(k, &sp, &depth, str, fp, n))
   return NULL;
 }
 return sp;
}

/**
 * e_sh_walk - Walk C text from a point to the next thing a caller needs.
 * @kind: one of E_SH_*, what to look for.
 * @sp: just before where to start; @str: the caller's line buffer; @fp: the
 *      open source; @n: counts the lines read.
 *
 * Whatever the kind, the walk reads on into following lines when the current
 * one runs out, so the user finds a definition however its text is laid out.
 *
 * Return: where the walk stopped, or NULL when the file ends first.
 */
char *e_sh_walk(int kind, char *sp, char *str, E_AFILE *fp, int *n)
{
 const struct sh_walk_kind *k = &sh_kinds[kind];

 if (k->skips_blanks)
  return e_sh_skip_blank(k, sp, str, fp, n);
 return e_sh_scan(k, sp, str, fp, n);
}
