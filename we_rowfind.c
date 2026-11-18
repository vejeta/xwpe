/* we_rowfind.c -- the one owner of a search over a row.
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later.

   What this file decides, for the Find text of the Find, Replace, Change All
   and Grep dialogs, on one row of text:

   - what the engine sees: always the whole row, never a cut of it, so that
     a word boundary at the edge of a block or of the search's start looks at
     the byte beside it;
   - where a hit may start and end: on a character start of the editor's
     UTF-8 text, inside the scope [from, to];
   - what a case, a word and an empty hit are: "Case sensitive" off is the
     engine's own folding, a word byte is a letter, a digit, `_` or any byte
     from 0x80, and an empty match (`^`, `$`, `x*`) is a position that is
     found once;
   - what a row is: ^ matches at the start of a line and $ at its end, and
     a line longer than Max. Columns is several rows.

   A plain text is escaped and goes through the same engine, so the rules are
   the same with and without "Regular expression".

   The engine reads a row one way only.  A byte that is no UTF-8 (a Latin-1
   file under a UTF-8 locale) is one character of its own to Find, as it is one
   glyph on the screen: before the engine sees the Find text or a row, the
   owner stands a code point of its own in for each such glyph
   (WPE_ROWFIND_STANDIN_BASE), and maps the engine's columns back to the
   row's.  So there is no second engine, no switch of the locale and no
   difference between a system whose engine refuses such a byte, one that
   faults on it and one that matches it as itself. */

#include <ctype.h>
#include <langinfo.h>
#include <limits.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "we_rowfind.h"
#include "we_utf8.h"

#if WPE_ROWFIND_HAS_LOCALES
#define WPE_ROWFIND_LOCALES 1
#include <locale.h>
#endif

/* Only the BSDs' and macOS's multibyte engines have REG_ILLSEQ; the owner
   never hands them a byte that would earn it, and the name is here so that
   ROWFIND_BAD_ANSWER below is checked against it.  Where the name does not
   exist no answer is it. */
#ifndef REG_ILLSEQ
#define REG_ILLSEQ (-1)
#endif

/* The answer the owner gives itself when the engine reports a match outside
   the range it was given.  No REG_* code is this large (glibc's are 0 to 16,
   BSD's up to 256, REG_ILLSEQ is 17 or the fallback -1 above), so it is never
   taken for the engine's own; the check below fails the build if one is. */
#define ROWFIND_BAD_ANSWER 0x4000
typedef char rowfind_bad_answer_is_new[
 (ROWFIND_BAD_ANSWER != REG_NOMATCH && ROWFIND_BAD_ANSWER != REG_ESPACE &&
  ROWFIND_BAD_ANSWER != REG_ILLSEQ && ROWFIND_BAD_ANSWER != 0) ? 1 : -1];

/* The sentences Find says when the owner refuses a text the engine is
   measured to crash on (see we_rowfind.h); under 70 columns. */
#define ROWFIND_UNSAFE_GROUP_SENTENCE \
 "\\b \\B \\< \\> beside | can crash this system's regex: search refused"
#define ROWFIND_UNSAFE_ESCAPE_SENTENCE \
 "\\b \\B \\< \\> can crash this system's regex: search refused"

/* The sentence Find says when an engine that loses the text before a start
   cannot be given it for this Find text: a `)` with no `(`, a lone backslash
   at the end, or a back-reference \9 (see pattern_with_context). */
#define ROWFIND_NO_CONTEXT_SENTENCE \
 "\\b \\B \\< \\> with a lone ) or \\9 not supported here: refused"

/* The character the context expression takes before the start: any one
   character a row can hold.  A row never holds its line break, so the bracket
   leaves it out (the real byte, 0x0A), and a NUL is taken like any other. */
#define ROWFIND_CONSUMER "[^\n]"
#define ROWFIND_CONSUMER_LEN 4

/* A copy of the row from a start that begins with a NUL would be an empty
   string to the engine, so the copy path writes this byte (a non-word byte
   that is not a line break) in its place for the character before a start. */
#define ROWFIND_NUL_STAND_IN 0x01

/* The groups the context call asks for.  The owner reads two: the whole match
   and group 1, the hit itself.  But glibc's regexec answers "no match" for a
   back-reference \k when 2 <= nmatch <= k (measured: "(a)(b)\2" on "abb" fails
   with nmatch 2 and matches with 0, 1 or 3), and the wrapping group shifts the
   text's \1 to \9 up to \2 to \10, so the call asks for group 10 as well:
   nmatch 11 is the least that is right for every back-reference a text can
   hold. */
#define ROWFIND_CONTEXT_GROUPS 11

/* The first byte that is part of a word whatever the locale says: a letter
   beyond ASCII is a word letter, as it was in the Latin-1 locale of the
   original. */
#define WORD_BYTE_HIGH 0x80

#define ROWFIND_WINDOW_FIRST 16     /* bytes a backward search looks at first */
#define ROWFIND_ERROR_MAX 160
#define ROWFIND_FIRST_NON_ASCII 0x80   /* the first byte that is no ASCII character */

/* The Find text, escaped: two bytes for each of the ASCII characters that
   mean something in an expression, and the end. */
#define ROWFIND_ESCAPED_MAX (2 * E_FIND_TEXT_MAX + 2)

/* The Find text as the engine reads it: a stray byte is E_UTF8_MAX_BYTES
   bytes as a stand-in, and an escaped byte is two, never both for one byte of
   the text, so the larger bound is four bytes for each byte of the text and
   the end (513 for a text of E_FIND_TEXT_MAX bytes). */
#define ROWFIND_PATTERN_MAX (E_UTF8_MAX_BYTES * E_FIND_TEXT_MAX + 1)

/* The longest row the owner makes an engine text of: the engine's columns are
   ints and a stand-in row is up to E_UTF8_MAX_BYTES bytes for each byte of
   the row, so a longer row (half a gigabyte) is no hit instead of a wrapped
   size. */
#define ROWFIND_ROW_MAX (INT_MAX / E_UTF8_MAX_BYTES - 1)

/* The row column of an engine column that is inside a stand-in: there is
   none, and an answer that starts or ends there is no answer. */
#define ROWFIND_NO_COLUMN (-1)

/* The one compiled search the owner keeps, keyed by the text and the three
   option bits. */
typedef struct {
 int valid;                         /* text and bits below are what is compiled (or refused) */
 char text[E_FIND_TEXT_MAX + 1];
 unsigned bits;
 int utf8_process;                  /* the process codeset was UTF-8 when it was compiled */
 int reads_utf8;                    /* the engine reads UTF-8 characters: stand-ins are in use */
 int failed;                        /* the engine refused the text */
 int compiled;
 int needs_context;                 /* the engine must be handed the character before a start */
 int ctx_compiled;                  /* ctx_re holds the context expression */
 int cflags;
 char pattern[ROWFIND_PATTERN_MAX];
#ifdef WPE_ROWFIND_LOCALES
 locale_t ctype;                    /* ctype to run the engine under, or 0 for the process's */
#endif
 regex_t re;
 regex_t ctx_re;                    /* ROWFIND_CONSUMER "(" pattern ")", when needs_context */
} RowfindCache;

/* The last row the owner made an engine text of, and what it made.  A
   backward search calls the owner once for each hit of one row, so the row
   is compared with the saved copy (a memcmp) instead of being read again. */
typedef struct {
 int valid;                         /* src holds the row the rest was made from */
 int mapped;                        /* some glyph needed a stand-in: eng differs from src */
 unsigned char *src;
 size_t src_size;
 int src_len;
 unsigned char *eng;                /* the engine's bytes, with a zero after them */
 size_t eng_size;
 int eng_len;
 int *to_engine;                    /* row column -> engine column, 0 to src_len */
 size_t to_engine_size;
 int *to_row;                       /* engine column -> row column, or ROWFIND_NO_COLUMN */
 size_t to_row_size;
} EngineText;

static RowfindCache g_cache;
static int g_context_builds;        /* context expressions compiled since the start */
static EngineText g_text;
static char g_error[ROWFIND_ERROR_MAX];
static int g_has_error;
#ifndef WPE_ROWFIND_STARTEND
static char *g_scratch;
static size_t g_scratch_size;
#endif

/* Where the last hit left the cursor. */
static struct { int valid, y, x, backward; } g_resume;

/* What one search over one row works on. */
typedef struct {
 const unsigned char *row;
 int len;
 unsigned kind;
 unsigned how;
 int words;
 const unsigned char *eng;          /* what the engine reads: the row, or its stand-in text */
 int eng_len;
 const int *to_engine;              /* NULL when the engine reads the row itself */
 const int *to_row;
} RowSearch;

/* ---- the ctype the engine runs under ---- */

/**
 * process_codeset_is_utf8 - Does the process's own locale say UTF-8?
 *
 * The editor's text is UTF-8 in every locale; the engine reads characters by
 * the locale it runs under, so when the process runs under another codeset
 * the owner runs the engine under a UTF-8 ctype of its own where the system
 * has the calls, and reads bytes where it has not.
 */
static int process_codeset_is_utf8(void)
{
 const char *cs = nl_langinfo(CODESET);

 return(cs != NULL && (strcasecmp(cs, "UTF-8") == 0 || strcasecmp(cs, "UTF8") == 0));
}

#ifdef WPE_ROWFIND_LOCALES
static locale_t g_utf8_ctype;
static int g_ctype_probed;

/** probe_ctypes - Make the UTF-8 ctype once; 0 where a system has none. */
static void probe_ctypes(void)
{
 if (g_ctype_probed)
  return;
 g_ctype_probed = 1;
/* A UTF-8 ctype is made only where the engine reads characters under it
   (WPE_HAS_REGEX_MULTIBYTE, measured by configure: OpenBSD has a C.UTF-8
   locale and a single-byte engine).  Without it the engine runs under the
   process's own ctype and the owner keeps a hit to whole characters itself.
   The test builds a system that has none. */
#if WPE_HAS_REGEX_MULTIBYTE && !defined(WPE_ROWFIND_NO_UTF8_CTYPE)
 g_utf8_ctype = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
 if (g_utf8_ctype == (locale_t)0)
  g_utf8_ctype = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t)0);
#endif
}

/**
 * ctype_enter - Run what follows under @loc, when there is one.
 * @prev: receives what ctype_leave puts back.
 * Return: 1 when the thread's locale was changed.
 */
static int ctype_enter(locale_t loc, locale_t *prev)
{
 if (loc == (locale_t)0)
  return(0);
 *prev = uselocale(loc);
 return(1);
}

/** ctype_leave - Put back the locale ctype_enter replaced. */
static void ctype_leave(int entered, locale_t prev)
{
 if (entered)
  uselocale(prev);
}
#endif

/* ---- the compiled search ---- */

/** cache_clear - Free the compiled engine and its context expression; the text stays unset. */
static void cache_clear(RowfindCache *c)
{
 if (c->compiled)
  regfree(&c->re);
 if (c->ctx_compiled)
  regfree(&c->ctx_re);
 memset(c, 0, sizeof(*c));
}

/**
 * pattern_escape - The Find text as an expression that matches exactly it.
 * @text: the plain text; @out: ROWFIND_ESCAPED_MAX bytes.
 *
 * A backslash goes before each character that means something in an
 * extended expression, so that "a.b" finds those three characters.  A byte
 * that is no character (Latin-1 from a file) is copied as it is.
 */
static void pattern_escape(const char *text, char *out)
{
 const char *p;
 char *q = out;

 for (p = text; *p != '\0'; p++)
 {
  if (strchr("\\^$.[]|()*+?{}", *p) != NULL)
   *q++ = '\\';
  *q++ = *p;
 }
 *q = '\0';
}

/** error_note - Keep regerror's words for the compile that failed. */
static void error_note(int rc, const regex_t *re)
{
 regerror(rc, re, g_error, sizeof(g_error));
 g_has_error = 1;
}

/** refusal_note - Keep the owner's own sentence for a text it did not compile. */
static void refusal_note(const char *sentence)
{
 snprintf(g_error, sizeof(g_error), "%s", sentence);
 g_has_error = 1;
}

/* ---- what the owner refuses: the text, read as an extended expression ---- */

#define ERE_PLAIN  0   /* nothing the refusal looks for */
#define ERE_BOUND  1   /* \b \B \< or \> */
#define ERE_ALT    2   /* an alternation bar */
#define ERE_OPEN   3   /* a ( that opens a group */
#define ERE_CLOSE  4   /* a ) outside a bracket and not escaped */
#define ERE_BACKREF 5  /* \1 to \9 */
#define ERE_TRAILING 6 /* a backslash that ends the text */

/* FreeBSD's (and, by its sources, NetBSD's and macOS's) libc also spell the
   start and the end of a word as bracket expressions; they reach the same
   engine code as \< and \>, so they count as word-boundary escapes. */
#define ERE_WORD_START "[[:<:]]"
#define ERE_WORD_END   "[[:>:]]"
#define ERE_WORD_EDGE_LEN 7

/**
 * ere_skip_bracket - Step over a bracket expression.
 * @p: points at the `[`.
 * Return: the byte after the closing `]`, or the terminating zero when the
 * bracket is not closed (regcomp refuses such a text anyway).
 *
 * A `]` right after `[` or `[^` is a member; `[:name:]`, `[=x=]` and `[.x.]`
 * are one member each; a backslash inside is a plain character.
 */
static const char *ere_skip_bracket(const char *p)
{
 p++;
 if (*p == '^')
  p++;
 if (*p == ']')
  p++;
 while (*p != '\0')
 {
  if (*p == '[' && (p[1] == ':' || p[1] == '=' || p[1] == '.'))
  {
   char member_end = p[1];

   p += 2;
   while (*p != '\0' && !(*p == member_end && p[1] == ']'))
    p++;
   if (*p == '\0')
    return(p);
   p += 2;
   continue;
  }
  if (*p == ']')
   return(p + 1);
  p++;
 }
 return(p);
}

/**
 * ere_next - Read one piece of an extended expression.
 * @p: where to read, not at the terminating zero.
 * @kind: receives ERE_BOUND for a word-boundary escape (or [[:<:]], [[:>:]]), ERE_ALT for an
 * unescaped bar outside a bracket, ERE_OPEN and ERE_CLOSE for an unescaped
 * parenthesis outside a bracket, ERE_BACKREF for \1 to \9, ERE_TRAILING for a
 * backslash that ends the text, else ERE_PLAIN.
 * Return: where the next piece starts.
 *
 * A backslash and the byte after it are one piece, so an escaped backslash
 * (`\\`) is not the start of an escape, and `\|` is a bar that is no
 * alternation; a backslash before the end stops the text.
 */
static const char *ere_next(const char *p, int *kind)
{
 *kind = ERE_PLAIN;
 if (strncmp(p, ERE_WORD_START, ERE_WORD_EDGE_LEN) == 0 ||
     strncmp(p, ERE_WORD_END, ERE_WORD_EDGE_LEN) == 0)
 {
  *kind = ERE_BOUND;
  return(p + ERE_WORD_EDGE_LEN);
 }
 if (*p == '[')
  return(ere_skip_bracket(p));
 if (*p == '\\')
 {
  if (p[1] == '\0')
  {
   *kind = ERE_TRAILING;
   return(p + 1);
  }
  if (strchr("bB<>", p[1]) != NULL)
   *kind = ERE_BOUND;
  else if (p[1] >= '1' && p[1] <= '9')
   *kind = ERE_BACKREF;
  return(p + 2);
 }
 if (*p == '|')
  *kind = ERE_ALT;
 else if (*p == '(')
  *kind = ERE_OPEN;
 else if (*p == ')')
  *kind = ERE_CLOSE;
 return(p + 1);
}

/** ere_has - Does the expression @text hold a piece of @want outside brackets? */
static int ere_has(const char *text, int want)
{
 const char *p = text;
 int kind;

 while (*p != '\0')
 {
  p = ere_next(p, &kind);
  if (kind == want)
   return(1);
 }
 return(0);
}

/** ere_has_boundary_escape - Is there a \b, \B, \< or \> in the expression? */
static int ere_has_boundary_escape(const char *text)
{
 return(ere_has(text, ERE_BOUND));
}

/** ere_has_alternation - Is there an alternation bar in the expression? */
static int ere_has_alternation(const char *text)
{
 return(ere_has(text, ERE_ALT));
}

/**
 * wpe_rowfind_refuses_when - Would Find refuse @text, given what the engine is
 * measured to survive?
 * @group_safe: the engine survives an escape beside an alternation.
 * @boundary_safe: the engine survives an escape at all.
 *
 * A user searching on a system whose C library crashes on such a pattern gets
 * a sentence instead of a dead editor.  Only a regular expression is refused;
 * a plain text has its backslashes escaped and never reaches this.
 * Return: 0 the text may go to the engine, else WPE_ROWFIND_REFUSES_ESCAPE or
 * WPE_ROWFIND_REFUSES_GROUP.
 */
int wpe_rowfind_refuses_when(const char *text, unsigned sw, int group_safe, int boundary_safe)
{
 if (!(sw & FIND_REGULAR))
  return(0);
 if (!boundary_safe && ere_has_boundary_escape(text))
  return(WPE_ROWFIND_REFUSES_ESCAPE);
 if (!group_safe && ere_has_boundary_escape(text) && ere_has_alternation(text))
  return(WPE_ROWFIND_REFUSES_GROUP);
 return(0);
}

/** wpe_rowfind_refuses - wpe_rowfind_refuses_when with this system's measure. */
int wpe_rowfind_refuses(const char *text, unsigned sw)
{
 return(wpe_rowfind_refuses_when(text, sw, WPE_ROWFIND_BOUNDARY_GROUP_SAFE,
                                 WPE_ROWFIND_BOUNDARY_SAFE));
}

/* ---- the context expression: the character before a start ---- */

/* What pattern_with_context answers when it cannot keep the text's meaning. */
#define ROWFIND_CONTEXT_REFUSED (-1)

/* The context expression: the consumer, "(", the pattern, ")", and the end. */
#define ROWFIND_CONTEXT_PATTERN_MAX (ROWFIND_PATTERN_MAX + ROWFIND_CONSUMER_LEN + 3)

/**
 * context_piece_refused - Would wrapping this piece change what the text means?
 * @p, @kind: the piece, as ere_next read it.
 * @depth: the groups open so far; changed by a ( or a ).
 *
 * A `)` with no `(` is a plain character to some engines (glibc takes it so),
 * and inside the wrapping group it would close that group instead; a lone
 * backslash at the end would escape the wrapping `)`; and \9 cannot be
 * renumbered to \10.
 * Return: 1 refuse the text, 0 the piece can be wrapped.
 */
static int context_piece_refused(const char *p, int kind, int *depth)
{
 if (kind == ERE_TRAILING)
  return(1);
 if (kind == ERE_BACKREF)
  return(p[1] == '9');
 if (kind == ERE_OPEN)
  (*depth)++;
 if (kind == ERE_CLOSE)
 {
  if (*depth == 0)
   return(1);
  (*depth)--;
 }
 return(0);
}

/**
 * context_emit_piece - Append one piece of the pattern to the context expression.
 * @o: the bytes written so far.
 *
 * The wrapping group is group 1, so a back-reference \N of the pattern is \N+1
 * here.
 * Return: the bytes written now.
 */
static int context_emit_piece(char *out, int o, const char *p, const char *next, int kind)
{
 if (kind == ERE_BACKREF)
 {
  out[o++] = '\\';
  out[o++] = (char)(p[1] + 1);
  return(o);
 }
 memcpy(out + o, p, (size_t)(next - p));
 return(o + (int)(next - p));
}

/**
 * pattern_with_context - The expression that finds the pattern one character
 * after where it would start.
 * @pattern: the expression the engine reads; @out: ROWFIND_CONTEXT_PATTERN_MAX bytes.
 *
 * An engine that loses the text before a start judges a \b at that start as if
 * a line began there.  Asked from one character earlier, with this expression,
 * it takes that character and then judges the pattern with it behind: a hit
 * of the pattern is group 1.  The starts map one to one (a match at t here is
 * a match of the pattern at the next character start), so the leftmost and the
 * longest hit are the ones the pattern alone would give.
 * Return: 0 made, ROWFIND_CONTEXT_REFUSED when the text could mean something
 * else once wrapped (see context_piece_refused).
 */
static int pattern_with_context(const char *pattern, char *out)
{
 const char *p = pattern, *next;
 int kind, depth = 0, o = ROWFIND_CONSUMER_LEN + 1;

 memcpy(out, ROWFIND_CONSUMER "(", (size_t)o);
 while (*p != '\0')
 {
  next = ere_next(p, &kind);
  if (context_piece_refused(p, kind, &depth))
   return(ROWFIND_CONTEXT_REFUSED);
  o = context_emit_piece(out, o, p, next, kind);
  p = next;
 }
 if (depth != 0)
  return(ROWFIND_CONTEXT_REFUSED);
 out[o++] = ')';
 out[o] = '\0';
 return(0);
}

/* ---- what the engine reads: a stand-in for each glyph that is no UTF-8 ---- */

/**
 * standin_of_glyph - Does the glyph at @s need a stand-in for the engine?
 * @s: the first byte; @avail: the bytes that may be read.
 * @n: receives the bytes of the glyph.
 * @cp: receives the stand-in's code point when one is needed.
 *
 * A Latin-1 byte in a UTF-8 file is one glyph on the screen, and so it is one
 * character to Find: a stray byte becomes WPE_ROWFIND_STANDIN_BASE plus its
 * value, and a sequence of the right shape that is no character (an overlong
 * form, a surrogate, a lead above F4) becomes one WPE_ROWFIND_STANDIN_BAD_GLYPH.
 * Whatever is a character, and ASCII, goes to the engine as it is.  This is
 * the one place that says which glyph is which.
 * Return: 1 a stand-in is needed, 0 the glyph is read as it is.
 */
static int standin_of_glyph(const unsigned char *s, int avail, int *n, int *cp)
{
 int codepoint, cols;

 if (s[0] < ROWFIND_FIRST_NON_ASCII)
 {
  *n = 1;
  return(0);
 }
 *n = e_utf8_glyph_at(s, avail, &codepoint, &cols);
 if (codepoint != 0)
  return(0);
 if (*n == 1)
  *cp = WPE_ROWFIND_STANDIN_BASE + s[0];
 else
  *cp = WPE_ROWFIND_STANDIN_BAD_GLYPH;
 return(1);
}

/**
 * pattern_standins - The Find text as the engine reads it.
 * @in: the escaped text; @out: ROWFIND_PATTERN_MAX bytes.
 *
 * A Find text that holds a Latin-1 byte finds that byte on every system, and
 * `[x]` with such a byte is a bracket of one character, because the byte is
 * replaced wherever it stands: bytes from 0x80 are never expression syntax.
 */
static void pattern_standins(const char *in, char *out)
{
 int len = (int)strlen(in), i = 0, o = 0, n, cp;

 while (i < len)
 {
  if (standin_of_glyph((const unsigned char *)in + i, len - i, &n, &cp))
   o += e_utf8_encode(cp, (unsigned char *)out + o);
  else
  {
   memcpy(out + o, in + i, (size_t)n);
   o += n;
  }
  i += n;
 }
 out[o] = '\0';
}

/**
 * pattern_for_engine - Make the cache's pattern: the Find text, escaped when
 * it is a plain text, and with its stand-ins when the engine reads UTF-8.
 */
static void pattern_for_engine(RowfindCache *c)
{
 char plain[ROWFIND_ESCAPED_MAX];

 if (c->bits & FIND_REGULAR)
  snprintf(plain, sizeof(plain), "%s", c->text);
 else
  pattern_escape(c->text, plain);
 if (c->reads_utf8)
  pattern_standins(plain, c->pattern);
 else
  snprintf(c->pattern, sizeof(c->pattern), "%s", plain);
}

/**
 * grown - A buffer of @want units of @unit bytes, kept when it is big enough.
 * @buf, @have: the buffer and its units; @have is raised on success.
 * Return: the buffer, or NULL when the size does not fit or memory is short
 * (@buf is then still valid).
 */
static void *grown(void *buf, size_t *have, size_t want, size_t unit)
{
 void *bigger;

 if (want == 0)
  want = 1;                /* a buffer even for nothing, so NULL always means failure */
 if (want <= *have)
  return(buf);
 if (want > SIZE_MAX / unit)
  return(NULL);
 bigger = realloc(buf, want * unit);
 if (bigger != NULL)
  *have = want;
 return(bigger);
}

/**
 * standins_measure - The bytes the engine's text of a row has.
 * @count: receives the glyphs that need a stand-in.
 * The row must be no longer than ROWFIND_ROW_MAX.
 */
static int standins_measure(const unsigned char *row, int len, int *count)
{
 int i = 0, e = 0, n, cp;

 *count = 0;
 while (i < len)
 {
  if (standin_of_glyph(row + i, len - i, &n, &cp))
  {
   e += E_UTF8_MAX_BYTES;
   (*count)++;
  }
  else
   e += n;
  i += n;
 }
 return(e);
}

/**
 * standins_write - Write the engine's text of a row and its two column tables.
 *
 * A column inside a glyph that was replaced is the glyph's start for the
 * engine; a column inside a stand-in is no column of the row.  The buffers
 * hold what standins_measure said.
 */
static void standins_write(EngineText *t, const unsigned char *row, int len)
{
 int i = 0, e = 0, n, cp, k, m;
 unsigned char bytes[E_UTF8_MAX_BYTES];

 while (i < len)
 {
  if (standin_of_glyph(row + i, len - i, &n, &cp))
  {
   m = e_utf8_encode(cp, bytes);
   for (k = 0; k < n; k++)
    t->to_engine[i + k] = e;
   for (k = 0; k < m; k++)
   {
    t->eng[e + k] = bytes[k];
    t->to_row[e + k] = (k == 0) ? i : ROWFIND_NO_COLUMN;
   }
   e += m;
  }
  else
  {
   for (k = 0; k < n; k++)
   {
    t->to_engine[i + k] = e + k;
    t->eng[e + k] = row[i + k];
    t->to_row[e + k] = i + k;
   }
   e += n;
  }
  i += n;
 }
 t->to_engine[len] = e;
 t->to_row[e] = len;
 t->eng[e] = 0;
 t->eng_len = e;
}

/** text_keep_source - Save the row, for the next call to compare with. */
static int text_keep_source(EngineText *t, const unsigned char *row, int len)
{
 void *p = grown(t->src, &t->src_size, (size_t)len, 1);

 if (p == NULL)
  return(0);
 t->src = p;
 if (len > 0)
  memcpy(t->src, row, (size_t)len);
 t->src_len = len;
 return(1);
}

/** text_make_engine - Make the engine's text and the tables of a row that has stand-ins. */
static int text_make_engine(EngineText *t, const unsigned char *row, int len, int eng_len)
{
 void *eng = grown(t->eng, &t->eng_size, (size_t)eng_len + 1, 1);
 void *to_engine, *to_row;

 if (eng == NULL)
  return(0);
 t->eng = eng;
 to_engine = grown(t->to_engine, &t->to_engine_size, (size_t)len + 1, sizeof(int));
 if (to_engine == NULL)
  return(0);
 t->to_engine = to_engine;
 to_row = grown(t->to_row, &t->to_row_size, (size_t)eng_len + 1, sizeof(int));
 if (to_row == NULL)
  return(0);
 t->to_row = to_row;
 standins_write(t, row, len);
 return(1);
}

/**
 * text_rebuild - Make the engine's text of a row the saved one is not of.
 * Return: 1 made, 0 when the row is too long for its columns or memory is
 * short: Find then says "no hit on this row" and does not stop.
 */
static int text_rebuild(EngineText *t, const unsigned char *row, int len)
{
 int count, eng_len;

 t->valid = 0;
 if (len > ROWFIND_ROW_MAX)
  return(0);
 eng_len = standins_measure(row, len, &count);
 if (!text_keep_source(t, row, len))
  return(0);
 t->mapped = (count > 0);
 if (t->mapped && !text_make_engine(t, row, len, eng_len))
  return(0);
 t->valid = 1;
 return(1);
}

/** text_is_current - Is the saved engine text the one of exactly this row? */
static int text_is_current(const EngineText *t, const unsigned char *row, int len)
{
 if (!t->valid || t->src_len != len)
  return(0);
 return(len == 0 || memcmp(t->src, row, (size_t)len) == 0);
}

/** text_forget - Drop the saved engine text; its buffers stay for the next row. */
static void text_forget(EngineText *t)
{
 t->valid = 0;
}

/** text_release - Free the engine text's buffers. */
static void text_release(EngineText *t)
{
 free(t->src);
 free(t->eng);
 free(t->to_engine);
 free(t->to_row);
 memset(t, 0, sizeof(*t));
}

/**
 * row_for_engine - Set what the engine reads of a row, once for the search.
 * @x: receives the engine's text and, when it is not the row, the two column
 * tables.
 *
 * Every call of the engine on the row, every window of a backward search,
 * reads this one text, so a row never reads two ways.  A row with no glyph that
 * needs a stand-in, and every row where the engine reads bytes, is read as it
 * is.
 * Return: 1 ready, 0 no hit on this row (too long, or memory is short).
 */
static int row_for_engine(const RowfindCache *c, const unsigned char *row, int len, RowSearch *x)
{
 x->eng = row;
 x->eng_len = len;
 x->to_engine = NULL;
 x->to_row = NULL;
 if (!c->reads_utf8)
  return(1);
 if (!text_is_current(&g_text, row, len) && !text_rebuild(&g_text, row, len))
  return(0);
 if (!g_text.mapped)
  return(1);
 x->eng = g_text.eng;
 x->eng_len = g_text.eng_len;
 x->to_engine = g_text.to_engine;
 x->to_row = g_text.to_row;
 return(1);
}

/* ---- the compiled search ---- */

/**
 * engine_choose_reading - Decide how the engine reads, once for a compiled search.
 *
 * It reads UTF-8 characters where the system's engine does (configure
 * measured it) and either the process locale says UTF-8 or the owner has a
 * UTF-8 ctype to run it under.  Under a process locale that is not UTF-8 the
 * owner runs the engine under a UTF-8 ctype of its own where the system has
 * one.  The answer is kept in the cache and never asked again per row.
 */
static void engine_choose_reading(RowfindCache *c)
{
 int own_ctype = 0;

 c->utf8_process = process_codeset_is_utf8();
#ifdef WPE_ROWFIND_LOCALES
 probe_ctypes();
 if (!c->utf8_process)
  c->ctype = g_utf8_ctype;
 own_ctype = (c->ctype != (locale_t)0);
#endif
 c->reads_utf8 = WPE_HAS_REGEX_MULTIBYTE && (c->utf8_process || own_ctype);
}

/**
 * engine_compile_pattern - Compile @pattern into @re with the cache's flags,
 * under the ctype the engine runs under.
 * Return: 0 compiled, else regcomp's code.
 */
static int engine_compile_pattern(RowfindCache *c, regex_t *re, const char *pattern)
{
 int rc;
#ifdef WPE_ROWFIND_LOCALES
 locale_t prev = (locale_t)0;
 int entered = ctype_enter(c->ctype, &prev);
#endif

 rc = regcomp(re, pattern, c->cflags);
#ifdef WPE_ROWFIND_LOCALES
 ctype_leave(entered, prev);
#endif
 return(rc);
}

/**
 * engine_compile - Compile the cache's pattern.
 * Return: 0 compiled, else regcomp's code.
 */
static int engine_compile(RowfindCache *c)
{
 int rc = engine_compile_pattern(c, &c->re, c->pattern);

 if (rc == 0)
  c->compiled = 1;
 else
  error_note(rc, &c->re);
 return(rc);
}

/**
 * context_compile - Compile the expression that hands the engine the
 * character before a start.
 *
 * Done with the Find text, not at the first row, so a text the owner cannot
 * wrap is refused once and for every row, like a text the engine refuses: Find
 * never gives a hit for a text and a refusal for the same text later.
 * Return: 0 compiled, else ROWFIND_CONTEXT_REFUSED with the refusal's sentence
 * kept as the error (also when the wrapped text does not compile although the
 * text alone did).
 */
static int context_compile(RowfindCache *c)
{
 char wrapped[ROWFIND_CONTEXT_PATTERN_MAX];

 g_context_builds++;
 if (pattern_with_context(c->pattern, wrapped) != 0 ||
     engine_compile_pattern(c, &c->ctx_re, wrapped) != 0)
 {
  refusal_note(ROWFIND_NO_CONTEXT_SENTENCE);
  return(ROWFIND_CONTEXT_REFUSED);
 }
 c->ctx_compiled = 1;
 return(0);
}

/**
 * context_prepare - Make the context expression when this Find text needs one.
 *
 * Only a regular expression with a word-boundary escape reads the text before
 * a start, and only an engine that loses that text needs it handed over
 * (WPE_ROWFIND_START_CONTEXT is 0): everything else keeps the one compiled
 * expression and the one call per start.
 * Return: 0 ready or not needed, else context_compile's refusal.
 */
static int context_prepare(RowfindCache *c)
{
 c->needs_context = !WPE_ROWFIND_START_CONTEXT && (c->bits & FIND_REGULAR) &&
                    ere_has_boundary_escape(c->text);
 if (!c->needs_context)
  return(0);
 return(context_compile(c));
}

/** wpe_rowfind_context_builds - See we_rowfind.h. */
int wpe_rowfind_context_builds(void)
{
 return(g_context_builds);
}

/**
 * cache_build - Compile the Find text of @fd into the cache.
 *
 * A regular expression gets the platform's escapes flag; a plain text is
 * escaped.  Where the engine reads UTF-8 the text carries a stand-in for each
 * glyph that is no UTF-8, so a Latin-1 byte in the Find text is found on every
 * system.  A regular expression the engine is measured to crash on (see
 * wpe_rowfind_refuses) is not compiled: the refusal is kept as the error and
 * the text is cached as failed, so Change All over a hundred thousand rows
 * asks once.  Where the engine loses the text before a start, a Find text
 * with a word-boundary escape also gets its context expression here
 * (context_prepare), or a refusal when the text could not be wrapped.
 * Return: 1 when the engine is there, 0 when the text does not compile or is
 * refused.
 */
static int cache_build(RowfindCache *c, const FIND *fd)
{
 int rc, refusal;

 cache_clear(c);
 text_forget(&g_text);
 g_has_error = 0;
 strncpy(c->text, fd->search, E_FIND_TEXT_MAX);
 c->text[E_FIND_TEXT_MAX] = '\0';
 c->bits = fd->sw & FIND_COMPILE_BITS;
 c->valid = 1;
 c->cflags = WPE_ROWFIND_CFLAGS(c->bits);
 engine_choose_reading(c);
 pattern_for_engine(c);
 refusal = wpe_rowfind_refuses(c->text, c->bits);
 if (refusal != 0)
 {
  if (refusal == WPE_ROWFIND_REFUSES_ESCAPE)
   refusal_note(ROWFIND_UNSAFE_ESCAPE_SENTENCE);
  else
   refusal_note(ROWFIND_UNSAFE_GROUP_SENTENCE);
  c->failed = 1;
  return(0);
 }
 rc = engine_compile(c);
 if (rc == 0)
  rc = context_prepare(c);
 c->failed = (rc != 0);
 return(rc == 0);
}

/**
 * cache_ensure - Make the cache hold the search @fd asks for.
 *
 * The text, the three option bits and the process's codeset are the key; a
 * Change All over a hundred thousand rows compiles once.  The codeset is part
 * of the key on every system, since it decides how the engine reads.
 * Return: 1 when the search can run, 0 when the text does not compile.
 */
static int cache_ensure(const FIND *fd)
{
 RowfindCache *c = &g_cache;
 unsigned bits = fd->sw & FIND_COMPILE_BITS;
 int same;

 same = c->valid && c->bits == bits && strncmp(c->text, fd->search, E_FIND_TEXT_MAX) == 0;
 if (same && c->utf8_process != process_codeset_is_utf8())
  same = 0;
 if (!same)
  return(cache_build(c, fd));
 return(!c->failed);
}

/* ---- one call of the engine ---- */

/**
 * answer_in_range - Is a match [@so, @eo) inside [@lowest, @highest]?
 *
 * Find never uses an answer the engine gives outside the range it was asked
 * to look in: such an answer is no hit, never a replacement of a negative
 * length and never a search that stands still on one place.  The values are
 * the engine's own type (regoff_t is wider than int on some systems), so a
 * wild value cannot be cut into the range by a cast.
 */
static int answer_in_range(regoff_t so, regoff_t eo, regoff_t lowest, regoff_t highest)
{
 return(so >= lowest && so <= eo && eo <= highest);
}

/** engine_column - The engine's column of the row's column @pos. */
static int engine_column(const RowSearch *x, int pos)
{
 if (x->to_engine == NULL)
  return(pos);
 return(x->to_engine[pos]);
}

/**
 * answer_to_row - Turn the engine's match into columns of the row.
 * @base: the engine column the match's columns count from (0 with a range, the
 * start of the copy without one).
 *
 * An engine column inside a stand-in has no column of the row: an answer that
 * starts or ends there is no answer, so a hit never lies inside a character the
 * owner made up.  The columns are in the range the caller checked.
 * Return: 0 the columns are in @so and @eo, else ROWFIND_BAD_ANSWER.
 */
static int answer_to_row(const RowSearch *x, int base, const regmatch_t *m, int *so, int *eo)
{
 int ecol_so = base + (int)m->rm_so, ecol_eo = base + (int)m->rm_eo;

 if (x->to_row != NULL)
 {
  *so = x->to_row[ecol_so];
  *eo = x->to_row[ecol_eo];
  if (*so == ROWFIND_NO_COLUMN || *eo == ROWFIND_NO_COLUMN)
   return(ROWFIND_BAD_ANSWER);
  return(0);
 }
 *so = ecol_so;
 *eo = ecol_eo;
 return(0);
}

/* What the byte of the engine's text at the start of a call is. */
#define ROWFIND_START_IS_TEXT    0   /* the first byte of what the pattern reads */
#define ROWFIND_START_IS_CONTEXT 1   /* the character the context expression takes */

#ifdef WPE_ROWFIND_STARTEND
/**
 * engine_regexec - One regexec over the whole engine text, from @start.
 * @base: receives 0, the engine column the answer's columns count from.
 * @lead: not read here; the engine sees the byte at @start as it is.
 *
 * The engine reads the row whole and is told where to begin, so it may judge
 * a \b at @start by the byte before it (WPE_ROWFIND_START_CONTEXT says
 * whether it does).
 */
static int engine_regexec(const regex_t *re, const RowSearch *x, int start, int flags,
                          int lead, size_t nmatch, regmatch_t *m, int *base)
{
 (void)lead;
 *base = 0;
 m[0].rm_so = start;
 m[0].rm_eo = x->eng_len;
 return(regexec(re, (const char *)x->eng, nmatch, m, flags | REG_STARTEND));
}
#else
/**
 * engine_regexec - One regexec over a NUL-terminated copy of the engine text
 * from @start.
 * @base: receives @start, the engine column the answer's columns count from.
 * @lead: ROWFIND_START_IS_CONTEXT when the byte at @start is the character the
 * context expression takes.
 *
 * The byte before @start is out of the engine's sight, so a \b or \< at
 * @start would read as the start of the text: the context expression is what
 * gives it back.
 * A NUL at @start would end the copy before it begins, and every hit after
 * it would be lost, so the character that stands there is written as a byte
 * that is no word and no line break.
 */
static int engine_regexec(const regex_t *re, const RowSearch *x, int start, int flags,
                          int lead, size_t nmatch, regmatch_t *m, int *base)
{
 size_t tail = (size_t)(x->eng_len - start);
 void *copy = grown(g_scratch, &g_scratch_size, tail + 1, 1);

 if (copy == NULL)
  return(REG_ESPACE);
 g_scratch = copy;
 memcpy(g_scratch, x->eng + start, tail);
 g_scratch[tail] = '\0';
 if (lead == ROWFIND_START_IS_CONTEXT && tail > 0 && g_scratch[0] == '\0')
  g_scratch[0] = ROWFIND_NUL_STAND_IN;
 *base = start;
 return(regexec(re, g_scratch, nmatch, m, flags));
}
#endif

/**
 * engine_exec - The one place the engine is called: one regexec from the
 * engine column @start, under the engine's ctype.
 * @re: the compiled search, or its context expression.
 * @flags: REG_NOTBOL and REG_NOTEOL as the row's kind says.
 * @lead: what the byte at @start is (ROWFIND_START_IS_TEXT or _CONTEXT).
 * @nmatch, @m: how many groups to ask for and where they are put; their
 * columns count from @base.
 * @base: receives the engine column the columns of @m count from.
 * Return: regexec's code (0 a match, REG_NOMATCH, or a refusal).
 */
static int engine_exec(const RowfindCache *c, const regex_t *re, const RowSearch *x, int start,
                       int flags, int lead, size_t nmatch, regmatch_t *m, int *base)
{
 int rc;
#ifdef WPE_ROWFIND_LOCALES
 locale_t prev = (locale_t)0;
 int entered = ctype_enter(c->ctype, &prev);
#endif

 rc = engine_regexec(re, x, start, flags, lead, nmatch, m, base);
#ifdef WPE_ROWFIND_LOCALES
 ctype_leave(entered, prev);
#else
 (void)c;
#endif
 return(rc);
}

/**
 * engine_match_direct - Ask the engine for the first match from byte @pos.
 * @so, @eo: receive the match's columns of the whole row.
 * Return: 0 a match, else regexec's code, or ROWFIND_BAD_ANSWER when the
 * engine said it matched outside the range or inside a stand-in.
 *
 * The engine is asked from @pos itself: at column 0, with no \b to read, or
 * where it still sees the text before the start.
 */
static int engine_match_direct(RowfindCache *c, const RowSearch *x, int pos, int flags,
                               int *so, int *eo)
{
 regmatch_t m[1];
 int base, rc, epos = engine_column(x, pos);

 rc = engine_exec(c, &c->re, x, epos, flags, ROWFIND_START_IS_TEXT, 1, m, &base);
 /* the check is on the engine's own type, before the cast to int */
 if (rc == 0 && !answer_in_range(m[0].rm_so, m[0].rm_eo, epos - base, x->eng_len - base))
  rc = ROWFIND_BAD_ANSWER;
 if (rc == 0)
  rc = answer_to_row(x, base, &m[0], so, eo);
 return(rc);
}

/**
 * engine_char_before - The engine column of the character before @epos.
 *
 * One character to an engine that reads characters (a glyph of its text, a
 * stand-in whole), one byte to an engine that reads bytes.
 */
static int engine_char_before(const RowfindCache *c, const RowSearch *x, int epos)
{
 if (!c->reads_utf8)
  return(epos - 1);
 return(e_utf8_glyph_floor(x->eng, x->eng_len, epos - 1));
}

/** engine_char_bytes - The bytes of the character of the engine's text at @col. */
static int engine_char_bytes(const RowfindCache *c, const RowSearch *x, int col)
{
 if (!c->reads_utf8)
  return(1);
 return(e_utf8_seq_len(x->eng + col, x->eng_len - col));
}

/**
 * context_answer_valid - Is the context expression's answer the shape it must be?
 * @base: the engine column the columns of @m count from; @epos: where the
 * pattern may begin.
 *
 * The whole match is one character and then the hit, so group 1 must end where
 * the match ends, begin one character after it begins, and lie in the range
 * the engine was asked to look in.  Any other shape is an engine that reports
 * a group wrongly, and it is no hit, as an answer outside the range is.
 * Return: 1 valid, 0 not.
 */
static int context_answer_valid(const RowfindCache *c, const RowSearch *x, int base, int epos,
                                const regmatch_t *m)
{
 if (m[0].rm_so < 0 || m[1].rm_so < 0)
  return(0);
 if (!answer_in_range(m[1].rm_so, m[1].rm_eo, epos - base, x->eng_len - base))
  return(0);
 if (m[1].rm_eo != m[0].rm_eo || m[0].rm_so >= x->eng_len - base)
  return(0);
 return(m[1].rm_so == m[0].rm_so + engine_char_bytes(c, x, base + (int)m[0].rm_so));
}

/**
 * engine_match_with_context - Ask the engine for the first match from byte
 * @pos, with the character before it in sight.
 * @so, @eo: receive the match's columns of the whole row.
 * Return: as engine_match_direct.
 *
 * The call starts one character earlier, with the context expression, and
 * the hit is its group 1: a \b at @pos is judged with that character beside
 * it on every engine, so Find from the middle of "xab" does not take the
 * `a` for the start of a word.  The row is past its start (@pos > 0), so
 * REG_NOTBOL always holds; a `^` of the text cannot match behind the
 * character the expression takes either.
 */
static int engine_match_with_context(RowfindCache *c, const RowSearch *x, int pos, int flags,
                                     int *so, int *eo)
{
 regmatch_t m[ROWFIND_CONTEXT_GROUPS];
 int base, rc, epos = engine_column(x, pos), start = engine_char_before(c, x, epos);

 rc = engine_exec(c, &c->ctx_re, x, start, flags | REG_NOTBOL, ROWFIND_START_IS_CONTEXT,
                  ROWFIND_CONTEXT_GROUPS, m, &base);
 if (rc == 0 && !context_answer_valid(c, x, base, epos, m))
  rc = ROWFIND_BAD_ANSWER;
 if (rc == 0)
  rc = answer_to_row(x, base, &m[1], so, eo);
 return(rc);
}

/** goes_on_forward - Is this a forward search going on from a hit? */
static int goes_on_forward(const RowSearch *x)
{
 return((x->how & WPE_ROWFIND_AFTER_HIT) && !(x->how & WPE_ROWFIND_BACKWARD));
}

/**
 * engine_match - The first match that starts at or after byte @pos.
 *
 * ^ is kept for the start of a line and $ for its end by the row's kind.  A
 * forward search that goes on from a hit has passed the start of the line,
 * so ^ is not there for it either: Change All of `^a` with nothing on "aab"
 * leaves "ab", as sed and Vim do, and `^.` takes one character, not the line.
 * Any answer that is neither a match nor "no match" (a match outside the row
 * or inside a stand-in) is no match on this row: the engine reads the row one
 * way, so there is no other reading to try.
 * A \b, \B, \< or \> at a start inside the row is judged with the character
 * before it on every system: where the engine loses that text the owner
 * hands it over (engine_match_with_context), so the answer at a start is the
 * answer the engine gives reading the row from its first column.
 * Return: 1 a match, 0 none.
 */
static int engine_match(RowfindCache *c, const RowSearch *x, int pos, int *so, int *eo)
{
 int flags = 0, rc;

 if (pos > 0 || !(x->kind & WPE_ROW_STARTS_LINE) || goes_on_forward(x))
  flags |= REG_NOTBOL;
 if (!(x->kind & WPE_ROW_ENDS_LINE))
  flags |= REG_NOTEOL;
 if (pos > 0 && c->needs_context)
  rc = engine_match_with_context(c, x, pos, flags, so, eo);
 else
  rc = engine_match_direct(c, x, pos, flags, so, eo);
 if (rc != 0)
  *so = *eo = 0;
 return(rc == 0);
}

/* ---- what a hit may be ---- */

/** is_char_start - Is @p the first byte of a character of the row, or its end? */
static int is_char_start(const RowSearch *x, int p)
{
 return(e_utf8_glyph_floor(x->row, x->len, p) == p);
}

/** char_start_up - The first character start at or after @p. */
static int char_start_up(const RowSearch *x, int p)
{
 int f;

 if (p <= 0)
  return(0);
 if (p >= x->len)
  return(x->len);
 f = e_utf8_glyph_floor(x->row, x->len, p);
 if (f == p)
  return(p);
 return(f + e_utf8_seq_len(x->row + f, x->len - f));
}

/** next_start_after - The first character start behind the byte @p. */
static int next_start_after(const RowSearch *x, int p)
{
 int f, n;

 if (p >= x->len)
  return(x->len + 1);
 f = e_utf8_glyph_floor(x->row, x->len, p);
 n = f + e_utf8_seq_len(x->row + f, x->len - f);
 return(n > p ? n : p + 1);
}

/** is_word_byte - A letter, a digit, `_`, or a byte of a character beyond ASCII. */
static int is_word_byte(unsigned char c)
{
 return(isalnum(c) || c == '_' || c >= WORD_BYTE_HIGH);
}

/** words_ok - Do no word bytes touch the hit's two edges? */
static int words_ok(const RowSearch *x, int so, int eo)
{
 if (so > 0 && is_word_byte(x->row[so - 1]))
  return(0);
 if (eo < x->len && is_word_byte(x->row[eo]))
  return(0);
 return(1);
}

/**
 * zero_width_ok - May an empty match at @so be a hit?
 *
 * Not at column 0 of a row that continues a line (that place is the end of
 * the row before, found there once), and not where the search starts when it
 * goes on from a hit, so a search never stands on one place.
 */
static int zero_width_ok(const RowSearch *x, int so, int from, int to)
{
 if (so == 0 && !(x->kind & WPE_ROW_STARTS_LINE))
  return(0);
 if (!(x->how & WPE_ROWFIND_AFTER_HIT))
  return(1);
 if (x->how & WPE_ROWFIND_BACKWARD)
  return(so != to);
 return(so != from);
}

/** hit_acceptable - Is the match [@so, @eo) a hit inside [@from, @to]? */
static int hit_acceptable(const RowSearch *x, int so, int eo, int from, int to)
{
 if (so < from || eo > to)
  return(0);
 if (!is_char_start(x, so) || !is_char_start(x, eo))
  return(0);
 if (so == eo)
  return(zero_width_ok(x, so, from, to));
 if (x->words && !words_ok(x, so, eo))
  return(0);
 return(1);
}

/**
 * walk - Visit the matches that start at or after @start, one per start.
 * @keep_last: 0 stops at the first hit, 1 goes on and keeps the last.
 *
 * After a match that is no hit the walk goes on at the next character start,
 * so every start of the row is seen once, as a search by hand would.
 * Return: 1 when a hit was found.
 */
static int walk(RowfindCache *c, const RowSearch *x, int start, int from, int to,
                int keep_last, WpeRowHit *hit)
{
 int pos = start, so, eo, found = 0;

 while (pos <= x->len && pos <= to)
 {
  if (!engine_match(c, x, pos, &so, &eo))
   break;
  if (so > to)
   break;
  if (hit_acceptable(x, so, eo, from, to))
  {
   hit->so = so;
   hit->eo = eo;
   found = 1;
   if (!keep_last)
    break;
  }
  pos = next_start_after(x, so);
 }
 return(found);
}

/**
 * search_backward - The hit that starts last in [@from, @to].
 *
 * Walks forward over a window that ends at @to and doubles until it holds a
 * hit or reaches @from: a hit in a narrow window is final, so a Change All
 * from the end of a long row costs the window and not the row each step.
 */
static int search_backward(RowfindCache *c, const RowSearch *x, int from, int to,
                           WpeRowHit *hit)
{
 int width = ROWFIND_WINDOW_FIRST, start;

 for (;;)
 {
  if (to - from <= width)
   start = from;
  else
  {
   start = e_utf8_glyph_floor(x->row, x->len, to - width);
   if (start < from)
    start = from;
  }
  if (walk(c, x, start, from, to, 1, hit))
   return(1);
  if (start <= from)
   return(0);
  width *= 2;
 }
}

/* ---- the owner's calls ---- */

/**
 * wpe_rowfind_row - Find the Find text of @fd on one row.
 * @row, @row_len: the row's bytes, without its line break.  The byte at
 * row[row_len] must exist and be readable (the row's line break, or a zero):
 * the C library's engine may look at the byte behind the range it is given
 * (NetBSD 10 does), and the owner never reads past it.
 * @kind: WPE_ROW_STARTS_LINE and WPE_ROW_ENDS_LINE as they hold for this row.
 * @from, @to: the byte columns a hit must lie in (the cursor and the block's
 * edges); a hit starts at or after @from and ends at or before @to.
 * @how: WPE_ROWFIND_BACKWARD for the hit that starts last, and
 * WPE_ROWFIND_AFTER_HIT when the search goes on from a hit.
 * @hit: receives the columns of the hit.
 *
 * A user sees the Find text found where it is a word, a character and a case
 * of its own, never inside an accented letter, never across the edge of a
 * block, and `^` and `$` only at the ends of a line.  An empty Find text
 * finds nothing, and neither does a row too long, or memory too short, to
 * make the text the engine reads of it (a row with a Latin-1 byte in it is
 * read with a stand-in for each, see row_for_engine).
 * Return: 1 a hit, 0 none on this row, -1 the text does not compile as an
 * expression (wpe_rowfind_error says why).
 */
int wpe_rowfind_row(const FIND *fd, const unsigned char *row, int row_len,
                    unsigned kind, int from, int to, unsigned how, WpeRowHit *hit)
{
 RowSearch x;

 if (fd->search[0] == '\0')
 {
  cache_clear(&g_cache);   /* no compile error of an earlier text speaks for this one */
  g_has_error = 0;
  return(0);
 }
 if (!cache_ensure(fd))
  return(-1);
 if (row_len < 0)
  row_len = 0;
 x.row = row;
 x.len = row_len;
 x.kind = kind;
 x.how = how;
 x.words = (fd->sw & FIND_WHOLE_WORDS) != 0;
 if (from < 0)
  from = 0;
 if (to > row_len)
  to = row_len;
 if (from > to)
  return(0);
 from = char_start_up(&x, from);
 to = e_utf8_glyph_floor(row, row_len, to);
 if (from > to)
  return(0);
 if (!row_for_engine(&g_cache, row, row_len, &x))
  return(0);
 if (how & WPE_ROWFIND_BACKWARD)
  return(search_backward(&g_cache, &x, from, to, hit));
 return(walk(&g_cache, &x, from, from, to, 0, hit));
}

/**
 * wpe_rowfind_error - The words that say why the last Find text did not
 * compile, for the status row.
 * Return: regerror's text, or NULL when the last compile worked.
 */
const char *wpe_rowfind_error(void)
{
 return(g_has_error && g_cache.failed ? g_error : NULL);
}

/**
 * wpe_rowfind_release - Free what the owner keeps: the compiled search, the
 * engine's text of the last row, the copy row and the ctype.  The editor
 * calls it once when it ends.
 */
void wpe_rowfind_release(void)
{
 cache_clear(&g_cache);
 text_release(&g_text);
 g_has_error = 0;
 g_resume.valid = 0;
#ifndef WPE_ROWFIND_STARTEND
 free(g_scratch);
 g_scratch = NULL;
 g_scratch_size = 0;
#endif
#ifdef WPE_ROWFIND_LOCALES
 if (g_utf8_ctype != (locale_t)0)
  freelocale(g_utf8_ctype);
 g_utf8_ctype = (locale_t)0;
 g_ctype_probed = 0;
#endif
}

/**
 * wpe_rowfind_text_place - Give a place in the text its one name.
 * @y, @x: a row and a byte column; @row_len and @kind are the row's.
 *
 * The end of a row that continues a line and the start of the next row are
 * one place in the text.  Its name is the second one, so a cursor that the
 * rewrap of an insertion moved from one to the other still stands on the
 * place the last hit left it on.
 */
void wpe_rowfind_text_place(int *y, int *x, int row_len, unsigned kind)
{
 if (!(kind & WPE_ROW_ENDS_LINE) && *x >= row_len)
 {
  *y += 1;
  *x = 0;
 }
}

/** wpe_rowfind_note_resume - Remember the place a hit left the cursor at. */
void wpe_rowfind_note_resume(int y, int x, int backward)
{
 g_resume.valid = 1;
 g_resume.y = y;
 g_resume.x = x;
 g_resume.backward = backward != 0;
}

/**
 * wpe_rowfind_resumes_at - Does a search from this place go on from a hit?
 *
 * Search Again from where the last hit left the cursor does not find an
 * empty hit on that place again.  The note says only where the hit left the
 * cursor: a cursor that moved away and came back stands there again, so the
 * caller forgets the note when the cursor moves by other means than the
 * search, if the empty hit there is to be found again.
 */
int wpe_rowfind_resumes_at(int y, int x, int backward)
{
 return(g_resume.valid && g_resume.y == y && g_resume.x == x &&
        g_resume.backward == (backward != 0));
}

/** wpe_rowfind_forget_resume - A miss or a new dialog ends "from a hit". */
void wpe_rowfind_forget_resume(void)
{
 g_resume.valid = 0;
}
