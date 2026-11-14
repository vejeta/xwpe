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
   the same with and without "Regular expression". */

#include <ctype.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

#include "we_rowfind.h"
#include "we_utf8.h"

#if defined(HAVE_NEWLOCALE) && defined(HAVE_USELOCALE)
#define WPE_ROWFIND_LOCALES 1
#include <langinfo.h>
#include <locale.h>
#include <strings.h>
#endif

/* The engine runs REG_STARTEND (the row, whole, with the range to look in)
   where the C library has it.  WPE_ROWFIND_NO_STARTEND forces the copy path
   that a C library without it (musl) needs, so the test can build it. */
#if defined(HAVE_DECL_REG_STARTEND) && HAVE_DECL_REG_STARTEND && \
    !defined(WPE_ROWFIND_NO_STARTEND)
#define WPE_ROWFIND_STARTEND 1
#endif

/* The escapes `\b \< \> \w \s` are on by a flag in some libraries. */
#ifdef REG_ENHANCED
#define ENGINE_ESCAPES REG_ENHANCED
#elif defined(REG_GNU)
#define ENGINE_ESCAPES REG_GNU
#else
#define ENGINE_ESCAPES 0
#endif

/* Only the BSDs' and macOS's multibyte engines answer an invalid byte with
   REG_ILLSEQ; glibc matches it as itself.  Where the name does not exist no
   answer is it. */
#ifndef REG_ILLSEQ
#define REG_ILLSEQ (-1)
#endif

/* The option bits of FIND.sw this owner reads. */
#define FIND_CASE_SENSITIVE 128
#define FIND_WHOLE_WORDS     64
#define FIND_REGULAR         32
#define FIND_COMPILE_BITS (FIND_CASE_SENSITIVE | FIND_WHOLE_WORDS | FIND_REGULAR)

/* The first byte that is part of a word whatever the locale says: a letter
   beyond ASCII is a word letter, as it was in the Latin-1 locale of the
   original. */
#define WORD_BYTE_HIGH 0x80

#define ROWFIND_WINDOW_FIRST 16     /* bytes a backward search looks at first */
#define ROWFIND_ESCAPED_MAX (2 * E_FIND_TEXT_MAX + 2)
#define ROWFIND_ERROR_MAX 160

#define ENGINE_PRIMARY 0   /* the ctype the editor's text asks for */
#define ENGINE_BYTES   1   /* the C ctype: byte matching, for a row the first refuses */
#define ENGINE_COUNT   2

/* The one compiled search the owner keeps, keyed by the text and the three
   option bits. */
typedef struct {
 int valid;                         /* text and bits below are what is compiled (or refused) */
 char text[E_FIND_TEXT_MAX + 1];
 unsigned bits;
 int utf8_process;                  /* the process codeset was UTF-8 when it was compiled */
 int failed;                        /* the primary engine refused the text */
 int compiled[ENGINE_COUNT];
 int cflags;
 char pattern[ROWFIND_ESCAPED_MAX];
#ifdef WPE_ROWFIND_LOCALES
 locale_t ctype[ENGINE_COUNT];      /* ctype to run the engine under, or 0 for the process's */
#endif
 regex_t re[ENGINE_COUNT];
} RowfindCache;

static RowfindCache g_cache;
static char g_error[ROWFIND_ERROR_MAX];
static int g_has_error;
#ifndef WPE_ROWFIND_STARTEND
static char *g_scratch;
static int g_scratch_size;
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
} RowSearch;

/* ---- the ctype the engine runs under ---- */

#ifdef WPE_ROWFIND_LOCALES
static locale_t g_utf8_ctype, g_c_ctype;
static int g_ctype_probed;

/**
 * process_codeset_is_utf8 - Does the process's own locale say UTF-8?
 *
 * The editor's text is UTF-8 in every locale; the engine reads characters by
 * the locale it runs under, so when the process runs under another codeset
 * the owner runs the engine under a UTF-8 ctype of its own.
 */
static int process_codeset_is_utf8(void)
{
 const char *cs = nl_langinfo(CODESET);

 return(cs != NULL && (strcasecmp(cs, "UTF-8") == 0 || strcasecmp(cs, "UTF8") == 0));
}

/** probe_ctypes - Make the UTF-8 and the C ctype once; 0 where a system has none. */
static void probe_ctypes(void)
{
 if (g_ctype_probed)
  return;
 g_ctype_probed = 1;
#ifndef WPE_ROWFIND_NO_UTF8_CTYPE   /* the test builds a system that has none */
 g_utf8_ctype = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
 if (g_utf8_ctype == (locale_t)0)
  g_utf8_ctype = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t)0);
#endif
 g_c_ctype = newlocale(LC_CTYPE_MASK, "C", (locale_t)0);
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

/** cache_clear - Free the compiled engines; the text stays unset. */
static void cache_clear(RowfindCache *c)
{
 int i;

 for (i = 0; i < ENGINE_COUNT; i++)
  if (c->compiled[i])
   regfree(&c->re[i]);
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

/**
 * engine_compile - Compile the cache's pattern for one engine.
 * @which: ENGINE_PRIMARY or ENGINE_BYTES.
 * Return: 0 compiled, else regcomp's code.
 */
static int engine_compile(RowfindCache *c, int which)
{
 int rc;
#ifdef WPE_ROWFIND_LOCALES
 locale_t prev = (locale_t)0;
 int entered = ctype_enter(c->ctype[which], &prev);
#endif

 rc = regcomp(&c->re[which], c->pattern, c->cflags);
#ifdef WPE_ROWFIND_LOCALES
 ctype_leave(entered, prev);
#endif
 if (rc == 0)
  c->compiled[which] = 1;
 else if (which == ENGINE_PRIMARY)
  error_note(rc, &c->re[which]);
 return(rc);
}

/**
 * cache_build - Compile the Find text of @fd into the cache.
 *
 * A regular expression gets the platform's escapes flag; a plain text is
 * escaped.  Under a process locale that is not UTF-8 the primary engine runs
 * under a UTF-8 ctype of the owner's where the system has one.  A plain text
 * that a multibyte engine refuses (Latin-1 bytes from a file, REG_ILLSEQ) is
 * compiled under the C ctype and matched as bytes, as it always was.
 * Return: 1 when the primary engine is there, 0 when the text does not compile.
 */
static int cache_build(RowfindCache *c, const FIND *fd)
{
 int rc;

 cache_clear(c);
 g_has_error = 0;
 strncpy(c->text, fd->search, E_FIND_TEXT_MAX);
 c->text[E_FIND_TEXT_MAX] = '\0';
 c->bits = fd->sw & FIND_COMPILE_BITS;
 c->valid = 1;
 c->cflags = REG_EXTENDED | ((c->bits & FIND_CASE_SENSITIVE) ? 0 : REG_ICASE);
 if (c->bits & FIND_REGULAR)
 {
  strcpy(c->pattern, c->text);
  c->cflags |= ENGINE_ESCAPES;
 }
 else
  pattern_escape(c->text, c->pattern);
#ifdef WPE_ROWFIND_LOCALES
 probe_ctypes();
 c->utf8_process = process_codeset_is_utf8();
 if (!c->utf8_process)
  c->ctype[ENGINE_PRIMARY] = g_utf8_ctype;
 c->ctype[ENGINE_BYTES] = g_c_ctype;
#endif
 rc = engine_compile(c, ENGINE_PRIMARY);
#ifdef WPE_ROWFIND_LOCALES
 if (rc == REG_ILLSEQ && !(c->bits & FIND_REGULAR) && g_c_ctype != (locale_t)0)
 {
  c->ctype[ENGINE_PRIMARY] = g_c_ctype;
  g_has_error = 0;
  rc = engine_compile(c, ENGINE_PRIMARY);
 }
#endif
 c->failed = (rc != 0);
 return(rc == 0);
}

/**
 * cache_ensure - Make the cache hold the search @fd asks for.
 *
 * The text, the three option bits and the process's codeset are the key; a
 * Change All over a hundred thousand rows compiles once.
 * Return: 1 when the search can run, 0 when the text does not compile.
 */
static int cache_ensure(const FIND *fd)
{
 RowfindCache *c = &g_cache;
 unsigned bits = fd->sw & FIND_COMPILE_BITS;
 int same;

 same = c->valid && c->bits == bits && strncmp(c->text, fd->search, E_FIND_TEXT_MAX) == 0;
#ifdef WPE_ROWFIND_LOCALES
 if (same && c->utf8_process != process_codeset_is_utf8())
  same = 0;
#endif
 if (!same)
  return(cache_build(c, fd));
 return(!c->failed);
}

/* ---- one call of the engine ---- */

/**
 * engine_exec - Match from byte @pos of the row with one engine.
 * @flags: REG_NOTBOL and REG_NOTEOL as the row's kind says.
 * @so, @eo: receive the match's columns of the whole row.
 * Return: 0 a match, else regexec's code (REG_NOMATCH or a refusal).
 */
static int engine_exec(RowfindCache *c, int which, const RowSearch *x, int pos,
                       int flags, int *so, int *eo)
{
 regmatch_t m;
 int rc;
#ifdef WPE_ROWFIND_LOCALES
 locale_t prev = (locale_t)0;
 int entered = ctype_enter(c->ctype[which], &prev);
#endif

#ifdef WPE_ROWFIND_STARTEND
 m.rm_so = pos;
 m.rm_eo = x->len;
 rc = regexec(&c->re[which], (const char *)x->row, 1, &m, flags | REG_STARTEND);
 *so = (int)m.rm_so;
 *eo = (int)m.rm_eo;
#else
 /* No REG_STARTEND: the engine sees a NUL-terminated copy from @pos on.  The
    byte before @pos is out of its sight, so a \b or \< at @pos is judged
    without it. */
 if (g_scratch_size < x->len + 1)
 {
  char *grown = realloc(g_scratch, x->len + 1);

  if (grown == NULL)
  {
   *so = *eo = 0;
   rc = REG_ESPACE;
   goto done;
  }
  g_scratch = grown;
  g_scratch_size = x->len + 1;
 }
 memcpy(g_scratch, x->row, x->len);
 g_scratch[x->len] = '\0';
 rc = regexec(&c->re[which], g_scratch + pos, 1, &m, flags);
 *so = pos + (int)m.rm_so;
 *eo = pos + (int)m.rm_eo;
done:
#endif
#ifdef WPE_ROWFIND_LOCALES
 ctype_leave(entered, prev);
#endif
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
 * An answer that is neither a match nor "no match" (a multibyte engine
 * refusing a byte, REG_ILLSEQ) is no "nothing here": the row is tried again
 * by bytes.
 * Return: 1 a match, 0 none.
 */
static int engine_match(RowfindCache *c, const RowSearch *x, int pos, int *so, int *eo)
{
 int flags = 0, rc;

 if (pos > 0 || !(x->kind & WPE_ROW_STARTS_LINE) || goes_on_forward(x))
  flags |= REG_NOTBOL;
 if (!(x->kind & WPE_ROW_ENDS_LINE))
  flags |= REG_NOTEOL;
 rc = engine_exec(c, ENGINE_PRIMARY, x, pos, flags, so, eo);
 if (rc == 0)
  return(1);
 if (rc == REG_NOMATCH)
  return(0);
#ifdef WPE_ROWFIND_LOCALES
 if (c->ctype[ENGINE_BYTES] == (locale_t)0 || c->ctype[ENGINE_PRIMARY] == c->ctype[ENGINE_BYTES])
  return(0);
 if (!c->compiled[ENGINE_BYTES] && engine_compile(c, ENGINE_BYTES) != 0)
  return(0);
 return(engine_exec(c, ENGINE_BYTES, x, pos, flags, so, eo) == 0);
#else
 return(0);
#endif
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
 * @row, @row_len: the row's bytes, without its line break.
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
 * finds nothing.
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
 * copy row and the ctypes.  The editor calls it once when it ends.
 */
void wpe_rowfind_release(void)
{
 cache_clear(&g_cache);
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
 if (g_c_ctype != (locale_t)0)
  freelocale(g_c_ctype);
 g_utf8_ctype = g_c_ctype = (locale_t)0;
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
