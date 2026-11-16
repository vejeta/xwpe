/* test_rowfind.c -- unit test for the owner of a search over a row
 * (we_rowfind.c), linked alone.  Always runs.
 *
 * What the user would see go wrong if a rule broke: Find stopping on half
 * an accented letter, a case insensitive search turning `\w` into `\W`, a
 * word boundary at the edge of a block judged on a cut row, `^` or `$`
 * found in the middle of a long line that the editor wrapped, Change All
 * with `^` or `x*` standing on one place for ever or writing inside its own
 * text, a whole word search that takes `a_foo` for `foo`, Replace All on a
 * row of twenty thousand bytes taking seconds.
 *
 * Built several times from this file:
 *   test_rowfind         the engine runs on the whole row (REG_STARTEND);
 *   test_rowfind_copy    WPE_ROWFIND_NO_STARTEND: the engine runs on a copy
 *                        from the start of each call, which cannot see the
 *                        byte before it; the word boundary patterns differ
 *                        from the reference there and only there;
 *   test_rowfind_reject  WPE_ROWFIND_NO_UTF8_CTYPE: no UTF-8 ctype to run the
 *                        engine under, so a process locale that is not UTF-8
 *                        leaves the engine reading bytes and the owner has
 *                        to reject a match that covers part of a character.
 *   test_rowfind_illseq  WPE_ROWFIND_MOCK_ILLSEQ: a mock engine that refuses
 *                        invalid UTF-8 as FreeBSD's does (REG_ILLSEQ);
 *   test_rowfind_badengine  WPE_ROWFIND_MOCK_BADENGINE and
 *                        WPE_ROWFIND_UNSAFE_BOUNDARY_GROUP: a mock engine
 *                        that crashes on a word-boundary escape beside an
 *                        alternation over a multibyte row (NetBSD 10's) and
 *                        answers outside the row, with the owner's refusal
 *                        forced on;
 *   test_rowfind_badengine_copy  the same on the copy path (no REG_STARTEND),
 *                        where the check of the engine's answer is separate;
 *   test_rowfind_boundary  WPE_ROWFIND_UNSAFE_BOUNDARY: the real engine, with
 *                        the refusal of every word-boundary escape forced on;
 *   test_rowfind_boundary_copy  the same on the copy path
 *                        (WPE_ROWFIND_NO_STARTEND with
 *                        WPE_ROWFIND_UNSAFE_BOUNDARY): no word-boundary
 *                        escape reaches the engine, as on a system whose
 *                        engine is measured unsafe, so the copy path must
 *                        show no look-behind difference at all (the other
 *                        side of the copy build's expectation).
 * Each runs its passes under the C locale, C.UTF-8 and en_US.iso885915 (a
 * pass whose locale is missing says so and is skipped).
 *
 * Rows that can kill or hang the engine run in a child (run_isolated), so a
 * crash is a FAIL that names the row (a search that never ends is the hang
 * guard's line, tests/test_watchdog.h); a row is also run on a row placed so
 * that the byte after it is on an unmapped page (the contract is one byte,
 * the line break), and the byte-behind-the-range read is reported as a note.
 *
 * The differential: for every character start p of a random row the engine
 * is run by hand with rm_so = p and the match taken only if it starts at p;
 * the filters (scope, whole characters, words, empty hits, the row's kind)
 * are applied in plain code below.  The owner, which walks and doubles a
 * window, must give the same first (forward) or last (backward) hit.
 *
 * ROWFIND_ITERS sets the random rows per pass (default 20000);
 * ROWFIND_NO_PERF=1 skips the speed bounds (a sanitizer build). */
/* The scenario tag goes here when the manifest rows are added, with the
   callers' pass. */
#include <ctype.h>
#include <langinfo.h>
#include <locale.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../we_rowfind.h"
#include "../we_utf8.h"
#include "test_watchdog.h"

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

/* newlocale and uselocale are POSIX 2008 and not on every system: NetBSD has
   no uselocale.  The owner then runs the engine under the process locale and
   never under a UTF-8 ctype of its own, so the pass that needs the reference
   to do the same cannot exist: it is skipped by name below, and the build is
   the same as test_rowfind_reject.  The decision is the owner's, read from
   we_rowfind.h. */
#define TEST_HAS_LOCALES WPE_ROWFIND_HAS_LOCALES

/* The owner makes its own UTF-8 ctype only where the engine reads characters
   under it (WPE_HAS_REGEX_MULTIBYTE, measured by configure: OpenBSD has a
   C.UTF-8 locale and a single-byte engine), so the test reads that same fact
   and never infers it from a locale existing.

   Forcing WPE_HAS_REGEX_MULTIBYTE to 0 on glibc (a scratch config.h) gives
   the reject expectations only in the [C] and en_US.iso885915 passes.  Its
   [C.UTF-8] pass fails ("." rejects half a character, "e acute ICASE is
   bytes"), because glibc really reads characters there and the forced 0 is
   false on glibc.  Those failures are the red proof that the fact's gate on
   process_utf8() in engine_reads_chars is live; only OpenBSD, whose engine
   reads bytes under C.UTF-8 as well, runs that pass green. */
#if defined(WPE_ROWFIND_NO_UTF8_CTYPE) || !TEST_HAS_LOCALES || !WPE_HAS_REGEX_MULTIBYTE
#define OWNER_HAS_UTF8_CTYPE 0
#else
#define OWNER_HAS_UTF8_CTYPE 1
#endif

#ifdef WPE_ROWFIND_NO_STARTEND
#define OWNER_SEES_BEFORE 0
#else
#define OWNER_SEES_BEFORE 1
#endif

#define SW_CASE   128
#define SW_WORD    64
#define SW_REGEX   32
#define BWD  WPE_ROWFIND_BACKWARD
#define AFT  WPE_ROWFIND_AFTER_HIT
#define KIND_BOTH (WPE_ROW_STARTS_LINE | WPE_ROW_ENDS_LINE)

static int fails, checks;
static int copy_differences;       /* owner != reference, on a word boundary pattern */
/* A backward search over a row the engine refuses part of: the owner decides
   "bytes from here" at the start of each window it looks at, the reference at
   the first refused start of the whole row, so a word boundary can be judged
   by characters in one and by bytes in the other.  Counted, not failed: which
   of the two is right for the user is an open question for the search owner. */
static int window_differences;
/* glibc judges \B after an atom that may match empty differently when a call
   starts before the place and when it starts there, on ASCII rows too
   (measured with a bare regexec: ".*\B" on " ..e" from 2 gives 3..3, from 3
   no match).  The engine disagrees with itself, so such a row cannot be a
   reference; engine_self_inconsistent finds those rows, which are counted
   and said, and every other row is compared. */
static int byte_engine_quirks;
/* Rows where owner and reference both said "refused" because the engine is
   measured unsafe for the text (wpe_rowfind_refuses): the log shows the path
   ran on a system that has it (NetBSD 10) and 0 on one that has not. */
static int differential_refused;
static const char *pass_name = "";

static void fail(const char *what)
{
 printf("FAIL [%s]: %s\n", pass_name, what);
 fails++;
}

/* ---- a deterministic generator ---- */

static uint64_t rng_state = 88172645463325252ULL;

static unsigned rnd(unsigned n)
{
 rng_state ^= rng_state << 13;
 rng_state ^= rng_state >> 7;
 rng_state ^= rng_state << 17;
 return((unsigned)((rng_state >> 11) % n));
}

/* ---- the process's locale and what the engine will read ---- */

static int process_utf8(void)
{
 const char *cs = nl_langinfo(CODESET);

 return(cs != NULL && (strcasecmp(cs, "UTF-8") == 0 || strcasecmp(cs, "UTF8") == 0));
}

/* Does the engine read characters (the owner made a UTF-8 ctype, or the
   process already has one), or bytes?  An engine that reads bytes whatever
   the locale (OpenBSD) reads bytes under a UTF-8 process locale too, so the
   fact gates both terms. */
static int engine_reads_chars(void)
{
 return(WPE_HAS_REGEX_MULTIBYTE && (OWNER_HAS_UTF8_CTYPE || process_utf8()));
}

/* The reference's ctype: a locale_t where the system has newlocale and
   uselocale, and a plain 0 that is never entered where it has not. */
#if TEST_HAS_LOCALES
typedef locale_t ref_ctype_t;
#else
typedef int ref_ctype_t;
#endif
#define REF_NO_CTYPE ((ref_ctype_t)0)

#if TEST_HAS_LOCALES
/** ref_utf8_ctype - the UTF-8 ctype the reference runs the engine under when
 * the process locale is not UTF-8, so it reads characters as the owner does.
 * Returns REF_NO_CTYPE when the system has no such locale. */
static ref_ctype_t ref_utf8_ctype(void)
{
 static locale_t loc;
 static int made;

 if (!made)
 {
  made = 1;
  loc = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
  if (loc == (locale_t)0)
   loc = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t)0);
 }
 return(loc);
}

/** ref_c_ctype - the C ctype the owner falls back to when the engine refuses
 * a byte (Latin-1 text from a file): the reference must read bytes the same
 * way.  Returns REF_NO_CTYPE when the system cannot make one. */
static ref_ctype_t ref_c_ctype(void)
{
 static locale_t loc;
 static int made;

 if (!made)
 {
  made = 1;
  loc = newlocale(LC_CTYPE_MASK, "C", (locale_t)0);
 }
 return(loc);
}

/** ref_ctype_enter - switch the calling thread to LOC and give back what it
 * ran under, for ref_ctype_leave.  A LOC of REF_NO_CTYPE changes nothing. */
static ref_ctype_t ref_ctype_enter(ref_ctype_t loc)
{
 if (loc == REF_NO_CTYPE)
  return(REF_NO_CTYPE);
 return(uselocale(loc));
}

/** ref_ctype_leave - undo ref_ctype_enter. */
static void ref_ctype_leave(ref_ctype_t loc, ref_ctype_t prev)
{
 if (loc != REF_NO_CTYPE)
  uselocale(prev);
}
#else
static ref_ctype_t ref_utf8_ctype(void)
{
 return(REF_NO_CTYPE);
}

static ref_ctype_t ref_c_ctype(void)
{
 return(REF_NO_CTYPE);
}

static ref_ctype_t ref_ctype_enter(ref_ctype_t loc)
{
 return(loc);
}

static void ref_ctype_leave(ref_ctype_t loc, ref_ctype_t prev)
{
 (void)loc;
 (void)prev;
}
#endif

/* ---- the reference: the engine run by hand on every character start ---- */

static int ref_is_start(const unsigned char *row, int len, int p)
{
 return(e_utf8_glyph_floor(row, len, p) == p);
}

static int ref_word_byte(unsigned char c)
{
 return(isalnum(c) || c == '_' || c >= 0x80);
}

static void ref_escape(const char *text, char *out)
{
 for (; *text; text++)
 {
  if (strchr("\\^$.[]|()*+?{}", *text) != NULL)
   *out++ = '\\';
  *out++ = *text;
 }
 *out = '\0';
}

/* Only the BSDs' and macOS's multibyte engines answer an invalid byte with
   REG_ILLSEQ; glibc matches it as itself.  The owner names it the same way. */
#ifndef REG_ILLSEQ
#define REG_ILLSEQ (-1)
#endif

/* The owner falls back to bytes in two places, and the reference counts each
   time it takes the same one, so a log shows the path ran: the engine
   refused a plain text at compile time, or refused a row at exec time. */
static int ref_compile_fallbacks, ref_exec_fallbacks;

/* Where the owner has a C ctype to fall back to, the reference does too. */
#define REF_HAS_BYTE_FALLBACK TEST_HAS_LOCALES

/** ref_regcomp - Compile @pat as the owner compiles a search with option bits
 * @sw, under the ctype @loc.  The one place the reference compiles, so its
 * flags are the owner's. */
static int ref_regcomp(regex_t *re, const char *pat, unsigned sw, ref_ctype_t loc)
{
 ref_ctype_t prev;
 int rc;

 /* Every compile of the reference comes here, so the reference never hands
    the engine a text the owner refuses for its measured safety: the owner
    is asked, the rule is not restated. */
 if (wpe_rowfind_refuses(pat, sw))
  return(REG_BADPAT);
 prev = ref_ctype_enter(loc);
 rc = regcomp(re, pat, WPE_ROWFIND_CFLAGS(sw));
 ref_ctype_leave(loc, prev);
 return(rc);
}

/** ref_compile - Compile as the owner does, including its first byte
 * fallback: a plain text that a multibyte engine refuses (REG_ILLSEQ, a
 * Latin-1 byte from a file) is compiled again under the C ctype and matched
 * as bytes.  @loc is the ctype the exec then runs under.
 * Return: regcomp's code. */
static int ref_compile(regex_t *re, const char *pat, unsigned sw, ref_ctype_t *loc)
{
 int rc = ref_regcomp(re, pat, sw, *loc);

 if (rc == REG_ILLSEQ && !(sw & SW_REGEX) && REF_HAS_BYTE_FALLBACK && ref_c_ctype() != REF_NO_CTYPE)
 {
  ref_compile_fallbacks++;
  *loc = ref_c_ctype();
  rc = ref_regcomp(re, pat, sw, *loc);
 }
 return(rc);
}

/* The byte engine of the owner's second fallback, compiled when first needed. */
typedef struct {
 regex_t re;
 int tried;
 int ok;
 int from_here;  /* a call was refused: the rest of the row is matched by bytes */
} RefBytes;

/** ref_exec_bytes - The owner's exec fallback: an answer that is neither a
 * match nor "no match" is tried again under the C ctype, and the row is a
 * match only if that matches.
 * Return: regexec's code of the byte engine, REG_NOMATCH when it is not there. */
static int ref_exec_bytes(RefBytes *rb, const char *pat, unsigned sw, const unsigned char *row,
                          int p, int len, int flags, regmatch_t *m)
{
 ref_ctype_t cloc = ref_c_ctype(), prev;
 int rc;

 if (!rb->tried)
 {
  rb->tried = 1;
  rb->ok = (ref_regcomp(&rb->re, pat, sw, cloc) == 0);
 }
 if (!rb->ok)
  return(REG_NOMATCH);
 ref_exec_fallbacks++;
 m->rm_so = p;
 m->rm_eo = len;
 prev = ref_ctype_enter(cloc);
 rc = regexec(&rb->re, (const char *)row, 1, m, flags | REG_STARTEND);
 ref_ctype_leave(cloc, prev);
 return(rc);
}

/** ref_exec - One regexec of the reference from byte @p, as the owner's
 * engine_match answers it.
 *
 * The owner's present rule, which this models and does not judge: a call
 * the engine refuses (neither a match nor "no match") is answered by the
 * byte engine, and that engine looks at the rest of the row from that
 * start, so the reference goes on by bytes from the first refused start
 * and does not decide again at each later start.  Whether that is right
 * for the user (a refused byte early in a row changes how a later start is
 * judged) is an open question for the search owner's design, not for this
 * test.  Revisit when a FreeBSD or macOS log says "exec N" with N > 0 in
 * the "reference: byte fallback taken" line.
 * Return: 1 a match (in @m), 0 none. */
static int ref_exec(regex_t *re, ref_ctype_t loc, RefBytes *rb, const char *pat, unsigned sw,
                    const unsigned char *row, int p, int len, int flags, regmatch_t *m)
{
 int rc;

 if (!rb->from_here)
 {
  m->rm_so = p;
  m->rm_eo = len;
  rc = regexec(re, (const char *)row, 1, m, flags | REG_STARTEND);
  if (rc == 0)
   return(1);
  if (rc == REG_NOMATCH)
   return(0);
  if (!REF_HAS_BYTE_FALLBACK || ref_c_ctype() == REF_NO_CTYPE || loc == ref_c_ctype())
   return(0);
  rb->from_here = 1;
 }
 return(ref_exec_bytes(rb, pat, sw, row, p, len, flags, m) == 0);
}

/* 1 a hit, 0 none, -1 the text does not compile. */
static int ref_find(const FIND *fd, const unsigned char *row, int len, unsigned kind,
                    int from, int to, unsigned how, WpeRowHit *hit)
{
 regex_t re;
 RefBytes rb;
 char pat[2 * E_FIND_TEXT_MAX + 4];
 int rc, p, found = 0, f, t;
 ref_ctype_t loc = (OWNER_HAS_UTF8_CTYPE && !process_utf8()) ? ref_utf8_ctype() : REF_NO_CTYPE;
 ref_ctype_t prev;

 if (fd->search[0] == '\0')
  return(0);
 if (fd->sw & SW_REGEX)
  strcpy(pat, fd->search);
 else
  ref_escape(fd->search, pat);
 memset(&rb, 0, sizeof rb);
 rc = ref_compile(&re, pat, fd->sw, &loc);
 if (rc != 0)
  return(-1);
 prev = ref_ctype_enter(loc);
 if (from < 0)
  from = 0;
 if (to > len)
  to = len;
 f = from;
 while (f < len && !ref_is_start(row, len, f))
  f++;
 t = (to < 0) ? -1 : e_utf8_glyph_floor(row, len, to);
 for (p = f; p <= t; p++)
 {
  regmatch_t m;
  int flags = 0, so, eo;

  if (!ref_is_start(row, len, p))
   continue;
  if (p > 0 || !(kind & WPE_ROW_STARTS_LINE) || ((how & AFT) && !(how & BWD)))
   flags |= REG_NOTBOL;
  if (!(kind & WPE_ROW_ENDS_LINE))
   flags |= REG_NOTEOL;
  if (!ref_exec(&re, loc, &rb, pat, fd->sw, row, p, len, flags, &m))
   continue;
  so = (int)m.rm_so;
  eo = (int)m.rm_eo;
  if (so != p || eo > t || !ref_is_start(row, len, eo))
   continue;
  if (so == eo)
  {
   if (so == 0 && !(kind & WPE_ROW_STARTS_LINE))
    continue;
   if ((how & AFT) && !(how & BWD) && so == f)
    continue;
   if ((how & AFT) && (how & BWD) && so == t)
    continue;
  }
  else if (fd->sw & SW_WORD)
  {
   if (so > 0 && ref_word_byte(row[so - 1]))
    continue;
   if (eo < len && ref_word_byte(row[eo]))
    continue;
  }
  hit->so = so;
  hit->eo = eo;
  found = 1;
  if (!(how & BWD))
   break;
 }
 regfree(&re);
 if (rb.tried && rb.ok)
  regfree(&rb.re);
 ref_ctype_leave(loc, prev);
 return(found);
}

/* ---- helpers for the rows ---- */

static FIND the_fd;

static const FIND *fd_for(const char *text, unsigned sw)
{
 memset(&the_fd, 0, sizeof the_fd);
 strncpy(the_fd.search, text, E_FIND_TEXT_MAX);
 the_fd.sw = sw;
 return(&the_fd);
}

/* A row as the editor holds it: its bytes, then the line break and a NUL, in
   a heap block of exactly that size.  The owner must never take the line
   break for text (a sanitizer's regexec check also reads up to the NUL). */
static unsigned char *row_copy(const void *bytes, int len)
{
 unsigned char *r = malloc((size_t)(len > 0 ? len : 0) + 2);

 if (r == NULL)
 {
  printf("FAIL: out of memory\n");
  exit(1);
 }
 if (len > 0)
  memcpy(r, bytes, (size_t)len);
 r[len > 0 ? len : 0] = '\n';
 r[(len > 0 ? len : 0) + 1] = '\0';
 return(r);
}

/* ---- a row at the edge of a mapping ---- */

/* Where a row's bytes live: a heap block with the line break and a NUL
   behind it (ROW_COPY), or a mapping whose next byte after the line break is
   on an unmapped page (ROW_GUARDED), so a read at row[len + 1] is a fault. */
#define ROW_COPY    0
#define ROW_GUARDED 1
#define ROW_NO_BREAK 2   /* the byte after the range is unmapped too (the diagnostic) */

/* The bytes readable behind a guarded row: the line break, which is the
   contract.  A sanitizer's regexec check also reads up to a NUL (the editor's
   rows are not NUL-terminated in the REG_STARTEND call), so its build keeps
   a NUL after the break, as row_copy does. */
#ifdef __SANITIZE_ADDRESS__
#define GUARD_TAIL 2
#else
#define GUARD_TAIL 1
#endif

static int row_mode = ROW_COPY;

typedef struct {
 unsigned char *bytes;
 void *map;          /* the mapping of a guarded row, else NULL */
 size_t map_len;
} TestRow;

/** test_page_size - The system's page size. */
static size_t test_page_size(void)
{
 long n = sysconf(_SC_PAGESIZE);

 return(n > 0 ? (size_t)n : 4096u);
}

/** row_guarded - Place a row with @tail bytes (1: the line break) behind it
 * and the next page unmapped, so that only the contract's byte is readable.
 * @row: receives the row and what row_drop needs. */
static void row_guarded(TestRow *row, const void *bytes, int len, int tail)
{
 size_t page = test_page_size(), at;
 unsigned char *base;

 base = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
 if (base == MAP_FAILED || mprotect(base + page, page, PROT_NONE) != 0)
 {
  printf("FAIL: cannot map a guard page\n");
  exit(1);
 }
 row->map = base;
 row->map_len = 2 * page;
 at = page - (size_t)len - (size_t)tail;
 row->bytes = base + at;
 if (len > 0)
  memcpy(row->bytes, bytes, (size_t)len);
 if (tail > 0)
  row->bytes[len] = '\n';
 if (tail > 1)
  row->bytes[len + 1] = '\0';
}

/** row_make - A row of the current mode's kind holding @len bytes. */
static void row_make(TestRow *row, const void *bytes, int len)
{
 row->map = NULL;
 row->map_len = 0;
 if (row_mode == ROW_GUARDED)
  row_guarded(row, bytes, len > 0 ? len : 0, GUARD_TAIL);
 else if (row_mode == ROW_NO_BREAK)
  row_guarded(row, bytes, len > 0 ? len : 0, 0);
 else
  row->bytes = row_copy(bytes, len);
}

/** row_drop - Free what row_make made. */
static void row_drop(TestRow *row)
{
 if (row->map != NULL)
  munmap(row->map, row->map_len);
 else
  free(row->bytes);
}

/* ---- a child for the rows that may kill or hang ---- */

#define ISOLATED_SECONDS 5   /* a row that has not ended by then is a hang */

/** spawn_body - Run @body in a child and wait for it.
 * @name: what the child runs, for the hang guard's line.
 * @st: receives the wait status; the child's exit status is 1 when a check
 * failed in it (the body's own FAIL lines are printed there) or the guard
 * ended it, else 0.  A child that has not ended in ISOLATED_SECONDS is ended
 * by the one hang guard of the C tests, which prints "FAIL: @name did not
 * finish within 5 s".
 * Return: 0 the child ran and was waited for, -1 fork or waitpid failed. */
static int spawn_body(const char *name, void (*body)(void), int *st)
{
 int before = fails;
 pid_t pid;

 fflush(stdout);
 pid = fork();
 if (pid < 0)
  return(-1);
 if (pid == 0)
 {
  test_watchdog_arm(name, ISOLATED_SECONDS);
  body();
  fflush(stdout);
  _exit(fails > before ? 1 : 0);
 }
 if (waitpid(pid, st, 0) != pid)
  return(-1);
 return(0);
}

/** run_isolated - Run @body in a child and fail @name when the child dies or fails.
 *
 * A crash of the engine, or a search that never ends, must be a FAIL that
 * names the row, not the end of the test.  The child's stdout is flushed
 * before the fork and its status says whether any check in it failed.  Any
 * non-zero status fails, whatever the signal or the exit code (a sanitizer
 * ends a crash with exit 1).
 * Return: 0 the child ended cleanly, else the wait status. */
static int run_isolated(const char *name, void (*body)(void))
{
 int st;
 char msg[300];

 checks++;
 if (spawn_body(name, body, &st) != 0)
 {
  fail("fork or waitpid failed");
  return(-1);
 }
 if (WIFSIGNALED(st))
  snprintf(msg, sizeof msg, "%s: the engine or the owner died with signal %d", name, WTERMSIG(st));
 else if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
  snprintf(msg, sizeof msg, "%s: a check failed in the child or it did not end in %d s (exit %d; its own line says which)",
           name, ISOLATED_SECONDS, WEXITSTATUS(st));
 else
  return(0);
 fail(msg);
 return(st != 0 ? st : -1);
}

/* One search; the result is rc, and the columns when rc == 1. */
static int run(const char *text, unsigned sw, const char *row, unsigned kind,
               int from, int to, unsigned how, int *so, int *eo)
{
 WpeRowHit h = { -7, -7 };
 int len = (int)strlen(row), rc;
 TestRow r;

 row_make(&r, row, len);
 rc = wpe_rowfind_row(fd_for(text, sw), r.bytes, len, kind, from, to < 0 ? len : to, how, &h);
 row_drop(&r);
 if (so != NULL)
  *so = h.so;
 if (eo != NULL)
  *eo = h.eo;
 return(rc);
}

static void expect(const char *name, const char *text, unsigned sw, const char *row,
                   unsigned kind, int from, int to, unsigned how, int want_rc,
                   int want_so, int want_eo)
{
 int so = -7, eo = -7, rc = run(text, sw, row, kind, from, to, how, &so, &eo);
 char msg[400];

 checks++;
 if (rc != want_rc || (rc == 1 && (so != want_so || eo != want_eo)))
 {
  snprintf(msg, sizeof msg, "%s: Find \"%s\" on \"%s\": got rc %d %d..%d, want rc %d %d..%d",
           name, text, row, rc, so, eo, want_rc, want_so, want_eo);
  fail(msg);
 }
}

/* ---- the named rows ---- */

/** skip_row - Say a named row did not run because the system lacks what it
 * needs, by its name, so a log shows what was not proved here. */
static void skip_row(const char *name, const char *needs)
{
 printf("SKIP [%s]: %s: needs %s\n", pass_name, name, needs);
}

/** gnu_escape_rows - The rows that use \w and \b in a regular expression.
 * They run where configure measured that regcomp understands the escapes
 * (WPE_HAS_REGEX_GNU_ESCAPES): FreeBSD's engine refuses them and OpenBSD's
 * takes them as letters, and there Find does the same by design. */
static void gnu_escape_rows(void)
{
 if (!WPE_HAS_REGEX_GNU_ESCAPES)
 {
  skip_row("w ICASE", "\\w in the C library's regex (WPE_HAS_REGEX_GNU_ESCAPES)");
  skip_row("ba from 1", "\\b in the C library's regex (WPE_HAS_REGEX_GNU_ESCAPES)");
  skip_row("b boundary at to", "\\b in the C library's regex (WPE_HAS_REGEX_GNU_ESCAPES)");
  return;
 }
 /* case: a regular expression keeps its meaning with Case sensitive off */
 expect("w ICASE", "\\w+", SW_REGEX, "ab cd", KIND_BOTH, 0, -1, 0, 1, 0, 2);
 /* On an engine measured unsafe for any word-boundary escape the owner
    refuses these two texts (rc -1, the escape sentence); the engine is not asked. */
 if (!WPE_ROWFIND_BOUNDARY_SAFE)
 {
  expect("ba from 1", "\\ba", SW_REGEX | SW_CASE, "xab", KIND_BOTH, 1, -1, 0, -1, 0, 0);
  expect("b boundary at to", "b\\b", SW_REGEX | SW_CASE, "abc", KIND_BOTH, 0, 2, 0, -1, 0, 0);
  return;
 }
 /* look-behind at the start of the call: the owner sees the byte before */
 expect("ba from 1", "\\ba", SW_REGEX | SW_CASE, "xab", KIND_BOTH, 1, -1, 0,
        OWNER_SEES_BEFORE ? 0 : 1, 1, 2);
 /* the block's end is a range, not the end of the text */
 expect("b boundary at to", "b\\b", SW_REGEX | SW_CASE, "abc", KIND_BOTH, 0, 2, 0, 0, 0, 0);
}

static void named_rows(void)
{
 const char *e_acute = "\xC3\xA9", *E_acute = "\xC3\x89";
 int so, eo, rc, i;

 gnu_escape_rows();
 expect("b at to", "b", SW_CASE, "abc", KIND_BOTH, 0, 2, 0, 1, 1, 2);
 expect("c at to", "c", SW_CASE, "abc", KIND_BOTH, 0, 2, 0, 0, 0, 0);
 /* the row's kind */
 expect("c$", "c$", SW_REGEX | SW_CASE, "abc", KIND_BOTH, 0, -1, 0, 1, 2, 3);
 expect("c$ not the end of the line", "c$", SW_REGEX | SW_CASE, "abc", WPE_ROW_STARTS_LINE,
        0, -1, 0, 0, 0, 0);
 expect("c$ past to", "c$", SW_REGEX | SW_CASE, "abc", KIND_BOTH, 0, 2, 0, 0, 0, 0);
 expect("^a not the start of the line", "^a", SW_REGEX | SW_CASE, "abc", WPE_ROW_ENDS_LINE,
        0, -1, 0, 0, 0, 0);
 /* an empty match is a position, found once */
 expect("^ fresh", "^", SW_REGEX, "abc", KIND_BOTH, 0, -1, 0, 1, 0, 0);
 expect("^ after a hit", "^", SW_REGEX, "abc", KIND_BOTH, 0, -1, AFT, 0, 0, 0);
 expect("^ on a continuation row", "^", SW_REGEX, "abc", WPE_ROW_ENDS_LINE, 0, -1, 0, 0, 0, 0);
 expect("$ fresh", "$", SW_REGEX, "abc", KIND_BOTH, 0, 3, 0, 1, 3, 3);
 expect("$ after a hit, backward", "$", SW_REGEX, "abc", KIND_BOTH, 0, 3, AFT | BWD, 0, 0, 0);
 expect("x* fresh", "x*", SW_REGEX, "abc", KIND_BOTH, 0, -1, 0, 1, 0, 0);
 expect("x* after a hit from 1", "x*", SW_REGEX, "abc", KIND_BOTH, 1, -1, AFT, 1, 2, 2);
 expect("x* on a continuation row", "x*", SW_REGEX, "abc", WPE_ROW_ENDS_LINE, 0, -1, 0, 1, 1, 1);
 expect("x* backward fresh", "x*", SW_REGEX, "ab", KIND_BOTH, 0, -1, BWD, 1, 2, 2);
 expect("x* backward after a hit", "x*", SW_REGEX, "ab", KIND_BOTH, 0, -1, BWD | AFT, 1, 1, 1);
 expect("a* hit with text, from 0", "a*", SW_REGEX, "aab", KIND_BOTH, 0, -1, AFT, 1, 0, 2);
 /* going on from a hit at a line's start (Change All with nothing for New
    Text) has passed the ^: `^a` on "aab" is replaced once, `^.` once */
 expect("^a after a hit at column 0", "^a", SW_REGEX, "ab", KIND_BOTH, 0, -1, AFT, 0, 0, 0);
 expect("^. after a hit at column 0", "^.", SW_REGEX, "bc", KIND_BOTH, 0, -1, AFT, 0, 0, 0);
 expect("^a fresh", "^a", SW_REGEX, "ab", KIND_BOTH, 0, -1, 0, 1, 0, 1);
 expect("^a backward after a hit", "^a", SW_REGEX, "aab", KIND_BOTH, 0, 2, BWD | AFT, 1, 0, 1);
 /* whole characters */
 if (engine_reads_chars())
 {
  expect(". is a character", ".", SW_REGEX, "\xC3\xA9" "a", KIND_BOTH, 0, -1, 0, 1, 0, 2);
  expect("e acute ICASE finds E acute", e_acute, 0, E_acute, KIND_BOTH, 0, -1, 0, 1, 0, 2);
  expect("e acute ICASE finds E acute, regular", e_acute, SW_REGEX, E_acute, KIND_BOTH, 0, -1, 0, 1, 0, 2);
 }
 else
 {
  expect(". rejects half a character", ".", SW_REGEX, "\xC3\xA9" "a", KIND_BOTH, 0, -1, 0, 1, 2, 3);
  expect("e acute ICASE is bytes", e_acute, 0, E_acute, KIND_BOTH, 0, -1, 0, 0, 0, 0);
 }
 expect("[e acute] on A tilde", "[\xC3\xA9]", SW_REGEX, "\xC3\x83", KIND_BOTH, 0, -1, 0, 0, 0, 0);
 expect("[copyright] on e acute", "[\xC2\xA9]", SW_REGEX, "\xC3\xA9", KIND_BOTH, 0, -1, 0, 0, 0, 0);
 expect("e acute ICASE is no fold of U+3A40", e_acute, 0, "\xE3\xA9\x80", KIND_BOTH, 0, -1, 0, 0, 0, 0);
 /* plain text goes through the engine as text */
 expect("plain a.b", "a.b", SW_CASE, "axb a.b", KIND_BOTH, 0, -1, 0, 1, 4, 7);
 expect("plain a*b", "a*b", SW_CASE, "aab a*b", KIND_BOTH, 0, -1, 0, 1, 4, 7);
 expect("plain ICASE", "AbC", 0, "xaBc", KIND_BOTH, 0, -1, 0, 1, 1, 4);
 expect("plain ASCII on a lone 0xE9", "a", SW_CASE, "\xE9" "a", KIND_BOTH, 0, -1, 0, 1, 1, 2);
 expect("plain 0xE9", "\xE9", SW_CASE, "x\xE9y", KIND_BOTH, 0, -1, 0, 1, 1, 2);
 expect("plain 0xFF", "\xFF", SW_CASE, "x\xFFy", KIND_BOTH, 0, -1, 0, 1, 1, 2);
 /* an empty text finds nothing; a bad expression is an error */
 expect("empty text", "", 0, "abc", KIND_BOTH, 0, -1, 0, 0, 0, 0);
 expect("empty text, expression", "", SW_REGEX, "abc", KIND_BOTH, 0, -1, 0, 0, 0, 0);
 expect("bad expression", "(", SW_REGEX | SW_WORD, "abc", KIND_BOTH, 0, -1, 0, -1, 0, 0);
 checks++;
 if (wpe_rowfind_error() == NULL || wpe_rowfind_error()[0] == '\0')
  fail("bad expression: wpe_rowfind_error says nothing");
 expect("good text clears the error", "a", SW_CASE, "abc", KIND_BOTH, 0, -1, 0, 1, 0, 1);
 checks++;
 if (wpe_rowfind_error() != NULL)
  fail("a text that compiles leaves an error");
 /* whole words */
 expect("word: _ is a word byte", "foo", SW_CASE | SW_WORD, "a_foo foo", KIND_BOTH, 0, -1, 0, 1, 6, 9);
 expect("word: accented letter is a word letter", "caf", SW_CASE | SW_WORD, "caf\xC3\xA9", KIND_BOTH,
        0, -1, 0, 0, 0, 0);
 expect("word: space then e acute", "caf", SW_CASE | SW_WORD, "caf \xC3\xA9", KIND_BOTH, 0, -1, 0, 1, 0, 3);
 expect("word: a later one, not the first only", "foo", SW_CASE | SW_WORD, "foo_x foo", KIND_BOTH,
        0, -1, 0, 1, 6, 9);
 expect("word: regular expression", "fo+", SW_CASE | SW_WORD | SW_REGEX, "xfoo fo", KIND_BOTH,
        0, -1, 0, 1, 5, 7);
 /* backward */
 expect("backward b", "b", SW_CASE, "abab", KIND_BOTH, 0, 4, BWD, 1, 3, 4);
 expect("backward b to 3", "b", SW_CASE, "abab", KIND_BOTH, 0, 3, BWD, 1, 1, 2);
 expect("backward none", "b", SW_CASE, "abab", KIND_BOTH, 0, 1, BWD, 0, 0, 0);
 expect("backward ab overlaps", "aba", SW_CASE, "ababa", KIND_BOTH, 0, 5, BWD, 1, 2, 5);
 expect("backward longest at the last start", "b+", SW_CASE | SW_REGEX, "abbb", KIND_BOTH, 0, 4, BWD, 1, 3, 4);
 /* the range */
 expect("from > to", "a", SW_CASE, "aaa", KIND_BOTH, 2, 1, 0, 0, 0, 0);
 expect("from > to backward", "a", SW_CASE, "aaa", KIND_BOTH, 2, 1, BWD, 0, 0, 0);
 expect("from inside a character", "a", SW_CASE, "\xC3\xA9" "a", KIND_BOTH, 1, -1, 0, 1, 2, 3);
 expect("to inside a character", ".", SW_REGEX, "a\xC3\xA9", KIND_BOTH, 0, 2, 0, 1, 0, 1);
 /* the look-behind probe: reports, and asserts what each build must say */
 rc = run("\\<b", SW_REGEX | SW_CASE, "ab", KIND_BOTH, 1, -1, 0, &so, &eo);
 checks++;
 if (rc == 1 && OWNER_SEES_BEFORE)
  fail("look-behind: \\<b on \"ab\" from 1 hit although the engine sees the byte before");
 if (rc == 1)
  printf("note [%s]: engine without look-behind at the start of a call (copy path)\n", pass_name);
 /* a window that has to double: the hit is at the far start of a long row */
 {
  char row[2100];

  row[0] = 'a';
  for (i = 1; i < 2000; i++)
   row[i] = 'b';
  row[2000] = '\0';
  expect("backward doubles to the far start", "a", SW_CASE, row, KIND_BOTH, 0, -1, BWD, 1, 0, 1);
  expect("backward doubles, regular", "a", SW_CASE | SW_REGEX, row, KIND_BOTH, 0, -1, BWD, 1, 0, 1);
  expect("backward doubles, none", "x", SW_CASE | SW_REGEX, row, KIND_BOTH, 0, -1, BWD, 0, 0, 0);
 }
}

/* ---- the engine's safety: what the owner refuses, a crash in a child ---- */

/* What the user reads when the owner refuses a text (user-visible, so the
   rows pin the words). */
#define TEST_GROUP_SENTENCE "\\b \\B \\< \\> beside | can crash this system's regex: search refused"
#define TEST_ESCAPE_SENTENCE "\\b \\B \\< \\> can crash this system's regex: search refused"
#define GROUP WPE_ROWFIND_REFUSES_GROUP
#define ESCAPE WPE_ROWFIND_REFUSES_ESCAPE

/** refuses_row - One row of the refusal predicate, whatever this system's engine.
 * @group_safe, @boundary_safe: the two decisions the predicate is asked with. */
static void refuses_row(const char *name, const char *text, unsigned sw, int group_safe,
                        int boundary_safe, int want)
{
 int got = wpe_rowfind_refuses_when(text, sw, group_safe, boundary_safe);
 char msg[300];

 checks++;
 if (got != want)
 {
  snprintf(msg, sizeof msg, "refusal %s: \"%s\" sw %u (group_safe %d, boundary_safe %d): got %d, want %d",
           name, text, sw, group_safe, boundary_safe, got, want);
  fail(msg);
 }
}

/** refusal_predicate_rows - The edges of "an escape beside |": each is a text
 * the user could type, and what Find does with it when the engine is unsafe. */
static void refusal_predicate_rows(void)
{
 /* the class: an escape and an alternation outside a bracket (group unsafe) */
 refuses_row("B group", "\\B(a|b)[ab]", SW_REGEX, 0, 1, GROUP);
 refuses_row("b group", "\\b(a|b)[ab]", SW_REGEX, 0, 1, GROUP);
 refuses_row("group B", "(a|b)\\B", SW_REGEX, 0, 1, GROUP);
 refuses_row("top alternation", "\\Ba|\\Bb", SW_REGEX, 0, 1, GROUP);
 refuses_row("lt group gt", "\\<(a|b)\\>", SW_REGEX, 0, 1, GROUP);
 refuses_row("escape then a bare bar", "\\b|", SW_REGEX, 0, 1, GROUP);
 refuses_row("bar then gt", "a|\\>", SW_REGEX, 0, 1, GROUP);
 refuses_row("case sensitive", "\\b(a|b)", SW_REGEX | SW_CASE, 0, 1, GROUP);
 /* the bracket spellings of the word edges are the same operators */
 refuses_row("word start bracket, group", "[[:<:]](a|b)", SW_REGEX, 0, 1, GROUP);
 refuses_row("word end bracket, group", "(a|b)[[:>:]]", SW_REGEX, 0, 1, GROUP);
 refuses_row("word start bracket alone", "[[:<:]]b", SW_REGEX, 0, 1, 0);
 refuses_row("word start bracket, any escape", "[[:<:]]b", SW_REGEX, 0, 0, ESCAPE);
 refuses_row("word end bracket, any escape", "[[:>:]].", SW_REGEX, 0, 0, ESCAPE);
 refuses_row("word end bracket, safe", "[[:>:]].", SW_REGEX, 1, 1, 0);
 /* not the class */
 refuses_row("group without a bar", "\\B(b)b", SW_REGEX, 0, 1, 0);
 refuses_row("a bar only", "(a|b)[ab]", SW_REGEX, 0, 1, 0);
 refuses_row("bar in a bracket", "[|]\\b", SW_REGEX, 0, 1, 0);
 refuses_row("escaped backslash then b", "\\\\b|x", SW_REGEX, 0, 1, 0);
 refuses_row("escaped bar", "\\|\\b", SW_REGEX, 0, 1, 0);
 refuses_row("plain text is escaped", "\\b|", 0, 0, 0, 0);
 refuses_row("plain text, word", "\\b|", SW_WORD, 0, 0, 0);
 /* brackets: a backslash inside is a plain character, a leading ] is a member */
 refuses_row("backslash closes nothing", "[\\]|\\b", SW_REGEX, 0, 1, GROUP);
 refuses_row("escape inside brackets", "[\\b]|x", SW_REGEX, 0, 1, 0);
 refuses_row("leading ] is a member", "[]|]\\b", SW_REGEX, 0, 1, 0);
 refuses_row("negated leading ]", "[^]|]\\b|x", SW_REGEX, 0, 1, GROUP);
 refuses_row("character class", "[[:alpha:]|]\\b", SW_REGEX, 0, 1, 0);
 refuses_row("equivalence class", "[[=a=]|]\\b", SW_REGEX, 0, 1, 0);
 refuses_row("collating symbol", "[[.-.]|]\\b", SW_REGEX, 0, 1, 0);
 refuses_row("backslash before the end", "a|\\", SW_REGEX, 0, 1, 0);
 refuses_row("unclosed bracket", "\\b[|", SW_REGEX, 0, 1, 0);
 /* any escape, when a control of the probe failed (boundary unsafe) */
 refuses_row("escape alone", "\\bfoo", SW_REGEX, 0, 0, ESCAPE);
 refuses_row("escape alone, B", "x\\B", SW_REGEX, 0, 0, ESCAPE);
 refuses_row("no escape", "foo|bar", SW_REGEX, 0, 0, 0);
 refuses_row("escape in a bracket", "[\\b]", SW_REGEX, 0, 0, 0);
 refuses_row("escaped backslash", "\\\\b", SW_REGEX, 0, 0, 0);
 refuses_row("escape alone, plain text", "\\bfoo", 0, 0, 0, 0);
 /* the engine is safe: nothing is refused */
 refuses_row("safe, class", "\\B(a|b)[ab]", SW_REGEX, 1, 1, 0);
 refuses_row("safe, escape", "\\b", SW_REGEX, 1, 1, 0);
}

/* The call that killed the editor on NetBSD 10 (the 9-byte row, Case
   sensitive off, backward after a hit): a \B before an alternation over a row
   that holds a multibyte character.  Its engine faults in mbrtowc. */
static const unsigned char nb_row[] = { 0x09, 0x20, 0xC3, 0x83, 0x62, 0x62, 0x20, 0x62, 0x61 };
#define NB_ROW_LEN 9

/** nb_expect - Check one call of the NetBSD row against what this build owes.
 * @sw: Find's option bits. */
static void nb_expect(unsigned sw)
{
 WpeRowHit h = { -7, -7 };
 TestRow r;
 const char *err;
 int rc;

 row_make(&r, nb_row, NB_ROW_LEN);
 rc = wpe_rowfind_row(fd_for("\\B(a|b)[ab]", sw), r.bytes, NB_ROW_LEN, WPE_ROW_STARTS_LINE, 1, 3,
                      BWD | AFT, &h);
 row_drop(&r);
 err = wpe_rowfind_error();
 checks++;
#if WPE_ROWFIND_BOUNDARY_GROUP_SAFE
 if (rc != 0)
  fail("nb_row: the engine is measured safe, and the row \\B(a|b)[ab] over the NetBSD bytes gave a hit");
#else
 if (rc != -1)
  fail("nb_row: the owner did not refuse \\B(a|b)[ab]");
 else if (err == NULL)
  fail("nb_row: the refusal says nothing");
 else if (strcmp(err, WPE_ROWFIND_BOUNDARY_SAFE ? TEST_GROUP_SENTENCE : TEST_ESCAPE_SENTENCE) != 0)
  fail("nb_row: the refusal's words are not the sentence Find shows");
#endif
}

/** body_nb_row - The NetBSD row, Case sensitive off and on, in the child. */
static void body_nb_row(void)
{
 nb_expect(SW_REGEX);
 nb_expect(SW_REGEX | SW_CASE);
}

/** expect_refused - A text Find must refuse here, with the sentence for this build. */
static void expect_refused(const char *name, const char *text)
{
 const char *err, *want;
 int rc = run(text, SW_REGEX, "ab", KIND_BOTH, 0, -1, 0, NULL, NULL);
 char msg[300];

 if (WPE_ROWFIND_BOUNDARY_SAFE)
  want = TEST_GROUP_SENTENCE;
 else
  want = TEST_ESCAPE_SENTENCE;
 err = wpe_rowfind_error();
 checks++;
 if (rc != -1 || err == NULL || strcmp(err, want) != 0)
 {
  snprintf(msg, sizeof msg, "refused %s: Find \"%s\": rc %d, error \"%s\"", name, text, rc,
           err != NULL ? err : "(none)");
  fail(msg);
 }
}

/** safe_boundary_rows - The engine is measured safe for an escape beside |:
 * the call that killed NetBSD runs on the real engine, and its forward twin hits. */
static void safe_boundary_rows(void)
{
 if (!engine_reads_chars())
 {
  skip_row("forward B group over a multibyte row", "an engine that reads characters here");
  return;
 }
 expect("forward B group over a multibyte row", "\\B(a|b)b", SW_REGEX | SW_CASE, "\x20\xC3\x83" "bb",
        KIND_BOTH, 0, -1, 0, 1, 3, 5);
}

/** boundary_rows - The engine's safety rows of this build.
 *
 * Where configure measured the engine safe for an escape beside |, the
 * NetBSD call runs on the engine and the owner finds nothing there; where it
 * measured it unsafe (NetBSD 10, or the build that forces it) the owner
 * refuses the text with the sentence and never calls the engine, whatever
 * the row.  The call runs in a child, so a crash is a FAIL by name. */
static void boundary_rows(void)
{
 run_isolated("nb_row (the call that killed the editor on NetBSD 10)", body_nb_row);
 refusal_predicate_rows();
#if WPE_ROWFIND_BOUNDARY_GROUP_SAFE
 safe_boundary_rows();
 skip_row("refusal of an escape beside |", "an engine measured unsafe (WPE_HAS_REGEX_SAFE_BOUNDARY_GROUP is 0, or test_rowfind_badengine)");
#else
 skip_row("forward twin on the engine", "an engine measured safe for an escape beside |");
 expect_refused("B group", "\\B(a|b)[ab]");
 expect_refused("B group, no bracket", "\\B(a|b)b");
 expect_refused("b group", "\\b(a|b)[ab]");
 expect_refused("group B", "(a|b)\\B");
 expect_refused("top alternation", "\\Ba|\\Bb");
 expect_refused("lt group gt", "\\<(a|b)\\>");
 expect_refused("bar then gt", "a|\\>");
#if WPE_ROWFIND_BOUNDARY_SAFE
 /* not refused: no bar, a bar in a bracket, an escaped backslash */
 checks++;
 if (run("\\B(b)b", SW_REGEX, "ab", KIND_BOTH, 0, -1, 0, NULL, NULL) == -1)
  fail("refused \\B(b)b: it has no alternation");
 checks++;
 if (run("[|]\\b", SW_REGEX, "ab", KIND_BOTH, 0, -1, 0, NULL, NULL) == -1)
  fail("refused [|]\\b: the bar is in a bracket");
 checks++;
 if (run("\\\\b|x", SW_REGEX, "ab", KIND_BOTH, 0, -1, 0, NULL, NULL) == -1)
  fail("refused \\\\b|x: the backslash is escaped");
#else
 /* a failed control: any word-boundary escape is refused, with no bar needed */
 expect_refused("escape alone", "\\bfoo");
 expect_refused("gt then dot", "\\>.");
 expect_refused("lt then b", "\\<b");
 expect_refused("word start bracket", "[[:<:]]b");
 expect_refused("word end bracket", "[[:>:]].");
 checks++;
 if (run("[|]x", SW_REGEX, "ab", KIND_BOTH, 0, -1, 0, NULL, NULL) == -1)
  fail("refused [|]x: it has no word-boundary escape");
#endif
#endif
}

/** diag_body_word - \b at the end of a row that has no byte behind it. */
static void diag_body_word(void)
{
 WpeRowHit h = { -7, -7 };
 TestRow r;

 row_make(&r, "ab", 2);
 wpe_rowfind_row(fd_for("b\\b", SW_REGEX | SW_CASE), r.bytes, 2, KIND_BOTH, 0, 2, 0, &h);
 _exit(0);
}

/** diag_body_group - \B before a group over the NetBSD bytes, no byte behind the row. */
static void diag_body_group(void)
{
 WpeRowHit h = { -7, -7 };
 TestRow r;

 row_make(&r, "\x20\xC3\x83" "bb", 5);
 wpe_rowfind_row(fd_for("\\B(a|b)[ab]", SW_REGEX | SW_CASE), r.bytes, 5, KIND_BOTH, 0, 5, 0, &h);
 _exit(0);
}

/** diag_one - Run a body on a row with no byte behind it; 1 when it died. */
static int diag_one(void (*body)(void))
{
 int st;

 row_mode = ROW_NO_BREAK;
 if (spawn_body("the byte behind the range", body, &st) != 0)
  st = 0;
 row_mode = ROW_COPY;
 return(WIFSIGNALED(st));
}

/** diagnostic_reads_behind - The byte behind the range, as a note.
 *
 * The C library's engine may read row[rm_eo] although REG_STARTEND gives it
 * the range [rm_so, rm_eo) (NetBSD 10 does).  A row placed so that the byte
 * after it is unmapped shows it: when the child dies the note says so, and on
 * a system whose engine reads nothing there the line stays silent.  This is a
 * report, not a FAIL: the contract (we_rowfind.h) is that the byte exists. */
static void diagnostic_reads_behind(void)
{
 int died = 0;

#ifdef __SANITIZE_ADDRESS__
 skip_row("the byte behind the range", "a build without a sanitizer (its regexec check reads to a NUL)");
 return;
#endif
 if (WPE_HAS_REGEX_GNU_ESCAPES)
  died |= diag_one(diag_body_word);
 if (WPE_HAS_REGEX_GNU_ESCAPES && !wpe_rowfind_refuses("\\B(a|b)[ab]", SW_REGEX))
  died |= diag_one(diag_body_group);
 row_mode = ROW_COPY;
 if (died)
  printf("note [%s]: the engine reads the byte behind the range (rm_eo); the row's line break must stand there\n",
         pass_name);
}

#ifdef WPE_ROWFIND_MOCK_BADENGINE
/* ---- the mock engine's answers outside the row ---- */

static const char *marker_row;

/** body_marker_row - Find on a row the mock answers outside the range for.
 *
 * Plain and regular, forward and backward, from the start and from 3 (the
 * marker starts at 3, so it is in the range of both): the answer is no hit
 * and the search ends (a hang is the hang guard's line from the child). */
static void body_marker_row(void)
{
 static const char *texts[] = { "a", "x+" };
 static const unsigned sws[] = { SW_CASE, SW_CASE | SW_REGEX };
 int t, backward, start;

 for (t = 0; t < 2; t++)
  for (backward = 0; backward < 2; backward++)
   for (start = 0; start <= 3; start += 3)
   {
    int rc = run(texts[t], sws[t], marker_row, KIND_BOTH, start, -1, backward ? BWD : 0, NULL, NULL);
    char msg[300];

    checks++;
    if (rc != 0)
    {
     snprintf(msg, sizeof msg, "answer outside the range: Find \"%s\" %s from %d on \"%s\": rc %d, want 0",
              texts[t], backward ? "backward" : "forward", start, marker_row, rc);
     fail(msg);
    }
   }
}

/** run_marker - body_marker_row on one marker row, in a child named by the row. */
static void run_marker(const char *row)
{
 marker_row = row;
 run_isolated(row, body_marker_row);
}

/** badengine_rows - An engine that answers a match outside the row is no hit.
 *
 * The three markers make mock_badengine answer far past the row, one byte
 * before where the search starts, with the end before the start, and with the
 * end one byte past the row (the check on the end alone): Find
 * says "nothing here" for each, never a hit with a negative length and never
 * a search that stands on one place. */
static void badengine_rows(void)
{
 run_marker("aaa<<FAR>> aaaa");
 run_marker("aaa<<PRE>> aaaa");
 run_marker("aaa<<NEG>> aaaa");
 run_marker("aaa<<END>> aaaa");
}
#endif

/* ---- the differential and the invariants ---- */

static const char *ALPHA = "abAB x_.\t-1";
static const char *UNITS[] = { "\xC3\xA9", "\xC3\x89", "\xE2\x86\x92", "\xFF", "\xE9", "\xC3\x83" };
#define UNIT_COUNT 6

static int make_row(unsigned char *out, int max_len, int long_row)
{
 int n = long_row ? 1000 + (int)rnd(1000) : (int)rnd(15);
 int len = 0, k;

 if (n > max_len - 8)
  n = max_len - 8;
 while (len < n)
 {
  if (rnd(6) == 0)
  {
   const char *u = UNITS[rnd(UNIT_COUNT)];

   k = (int)strlen(u);
   memcpy(out + len, u, (size_t)k);
   len += k;
  }
  else
   out[len++] = (unsigned char)ALPHA[rnd((unsigned)strlen(ALPHA))];
 }
 return(len);
}

static const char *TOKENS[] = {
 "a", "b", "A", "x", "\xC3\xA9", "\xC3\x89", ".", "*", "+", "?", "^", "$", "\\w", "\\b",
 "\\<", "\\>", "[ab]", "(a|b)", "a*", "[\xC3\xA9]", "\\s", "_", " ", ".*", "x*", "\\B"
};
#define TOKEN_COUNT 26

/* The tokens a random expression is made of: all of them where the engine
   understands \w \b \s \B, and without those four where it does not (a
   system whose engine refuses or reads them as letters: the owner and the
   reference then agree on them, and a row made of them tests nothing).  The
   order of the rest is kept, so a system with the escapes draws the same
   expressions as before. */
static const char *token_list[TOKEN_COUNT];
static int token_count;
static int tokens_dropped;

/** token_is_gnu - Is @t one of the escapes WPE_HAS_REGEX_GNU_ESCAPES covers? */
static int token_is_gnu(const char *t)
{
 return(strcmp(t, "\\w") == 0 || strcmp(t, "\\b") == 0 || strcmp(t, "\\s") == 0 ||
        strcmp(t, "\\B") == 0);
}

/** tokens_choose - Fill token_list from TOKENS by what the engine understands
 * and say which was chosen, once. */
static void tokens_choose(void)
{
 int i;

 token_count = 0;
 tokens_dropped = 0;
 for (i = 0; i < TOKEN_COUNT; i++)
 {
  if (!WPE_HAS_REGEX_GNU_ESCAPES && token_is_gnu(TOKENS[i]))
   tokens_dropped++;
  else
   token_list[token_count++] = TOKENS[i];
 }
 if (tokens_dropped > 0)
  printf("tokens: gnu escapes off (%d of %d dropped)\n", tokens_dropped, TOKEN_COUNT);
 else
  printf("tokens: gnu escapes on (all %d)\n", TOKEN_COUNT);
}

static const char *PLAIN[] = {
 "a", "b", "ab", "A", "ab ", " ", "\xC3\xA9", "\xC3\x89", "x_", ".", "a.b", "(", "*", "\xE9", "\xFF", "-1"
};
#define PLAIN_COUNT 16

static void make_text(char *out, unsigned *sw)
{
 *sw = rnd(2) ? SW_CASE : 0;
 if (rnd(2))
  *sw |= SW_WORD;
 out[0] = '\0';
 if (rnd(2))
 {
  int n = 1 + (int)rnd(4);

  *sw |= SW_REGEX;
  while (n-- > 0)
   strcat(out, token_list[rnd((unsigned)token_count)]);
 }
 else
  strcpy(out, PLAIN[rnd(PLAIN_COUNT)]);
}

static int has_boundary_escape(const char *text, unsigned sw)
{
 return((sw & SW_REGEX) &&
        (strstr(text, "\\b") || strstr(text, "\\B") || strstr(text, "\\<") || strstr(text, "\\>")));
}

/* One bare call of the engine from @p with the owner's flags for the row.
   1 a match (its columns in *so, *eo), 0 none. */
static int bare_match(regex_t *re, const unsigned char *row, int len, unsigned kind,
                      int p, int *so, int *eo)
{
 regmatch_t m;
 int flags = 0;

 if (p > 0 || !(kind & WPE_ROW_STARTS_LINE))
  flags |= REG_NOTBOL;
 if (!(kind & WPE_ROW_ENDS_LINE))
  flags |= REG_NOTEOL;
 m.rm_so = p;
 m.rm_eo = len;
 if (regexec(re, (const char *)row, 1, &m, flags | REG_STARTEND) != 0)
  return(0);
 *so = (int)m.rm_so;
 *eo = (int)m.rm_eo;
 return(1);
}

/* Does the engine disagree with itself on this row: a call from p gives a
   match at s, and a call from s gives another match (or none)?  On such a row
   the answer depends on where a call starts, so no walk can be compared with
   the reference. */
static int engine_self_inconsistent(const FIND *fd, const unsigned char *row, int len,
                                    unsigned kind)
{
 regex_t re;
 char pat[2 * E_FIND_TEXT_MAX + 4];
 int p, so, eo, so2, eo2, odd = 0;
 ref_ctype_t loc = (OWNER_HAS_UTF8_CTYPE && !process_utf8()) ? ref_utf8_ctype() : REF_NO_CTYPE;
 ref_ctype_t prev;

 if (!(fd->sw & SW_REGEX))
  return(0);
 strcpy(pat, fd->search);
 prev = ref_ctype_enter(loc);
 if (ref_regcomp(&re, pat, fd->sw, REF_NO_CTYPE) == 0)
 {
  for (p = 0; p <= len && !odd; p++)
  {
   if (!bare_match(&re, row, len, kind, p, &so, &eo))
    continue;
   if (!bare_match(&re, row, len, kind, so, &so2, &eo2) || so2 != so || eo2 != eo)
    odd = 1;
  }
  regfree(&re);
 }
 ref_ctype_leave(loc, prev);
 return(odd);
}

/* A row glibc's \B disagrees with itself on, in plain ASCII: the
   differential must skip it by that measure and not by its bytes. */
static void engine_quirk_rows(void)
{
 static const char row[] = " ..e";

 checks++;
 if (!engine_self_inconsistent(fd_for(".*\\B", SW_REGEX | SW_CASE), (const unsigned char *)row,
                               (int)strlen(row), KIND_BOTH))
  printf("note [%s]: the engine is consistent on \".*\\B\" over \" ..e\" (not glibc's)\n", pass_name);
}

static int word_bytes_ok(const unsigned char *row, int len, int so, int eo)
{
 if (so > 0 && ref_word_byte(row[so - 1]))
  return(0);
 if (eo < len && ref_word_byte(row[eo]))
  return(0);
 return(1);
}

/* Invariants of a hit that hold whatever the engine did. */
static void check_hit(const char *label, const FIND *fd, const unsigned char *row,
                      int len, int from, int to, unsigned how, const WpeRowHit *h)
{
 int f = from < 0 ? 0 : from, t = to > len ? len : to;

 while (f < len && !ref_is_start(row, len, f))
  f++;
 t = e_utf8_glyph_floor(row, len, t);
 if (h->so < f || h->so > h->eo || h->eo > t)
  fail(label), printf("   hit %d..%d outside [%d,%d]\n", h->so, h->eo, f, t);
 if (!ref_is_start(row, len, h->so) || !ref_is_start(row, len, h->eo))
  fail(label), printf("   hit %d..%d is not on character starts\n", h->so, h->eo);
 if (h->so < h->eo && (fd->sw & SW_WORD) && !word_bytes_ok(row, len, h->so, h->eo))
  fail(label), printf("   whole word hit %d..%d has a word byte beside it\n", h->so, h->eo);
 if (h->so == h->eo && (how & AFT) && !(how & BWD) && h->so == f)
  fail(label), printf("   empty hit stands where the search starts\n");
 if (h->so == h->eo && (how & AFT) && (how & BWD) && h->so == t)
  fail(label), printf("   empty hit stands where the backward search starts\n");
}

/* One case of the differential: the owner against the reference, and the
   invariants of the owner's hit. */
static void compare_case(const char *text, unsigned sw, const unsigned char *tmp, int len,
                         unsigned kind, int from, int to, unsigned how, int it)
{
 unsigned char *row, *before;
 int rc_own, rc_ref, exec_before = ref_exec_fallbacks;
 WpeRowHit own = { -1, -1 }, ref = { -1, -1 };
 const FIND *fd;
 char label[300];
 TestRow placed;

 row_make(&placed, tmp, len);
 row = placed.bytes;
 before = row_copy(tmp, len);
 fd = fd_for(text, sw);
 rc_own = wpe_rowfind_row(fd, row, len, kind, from, to, how, &own);
 rc_ref = ref_find(fd, row, len, kind, from, to, how, &ref);
 checks++;
 snprintf(label, sizeof label, "differential: Find \"%s\" sw %u kind %u [%d,%d] how %u on %d bytes (it %d)",
          text, sw, kind, from, to, how, len, it);
 if (memcmp(row, before, (size_t)(len > 0 ? len : 0)) != 0)
  fail("the owner changed the row");
 if (rc_own != rc_ref || (rc_own == 1 && (own.so != ref.so || own.eo != ref.eo)))
 {
  if (!OWNER_SEES_BEFORE && has_boundary_escape(text, sw))
   copy_differences++;
  else if (ref_exec_fallbacks > exec_before && (how & BWD) && has_boundary_escape(text, sw) &&
           rc_own == rc_ref)
  {
   /* two hits at different places: the windows' byte rule; a hit on one
      side and none on the other is a real difference and fails below */
   window_differences++;
   printf("exempt [%s]: %s (owner %d..%d, reference %d..%d)\n", pass_name, label,
          own.so, own.eo, ref.so, ref.eo);
  }
  else if (has_boundary_escape(text, sw) && engine_self_inconsistent(fd, row, len, kind))
   byte_engine_quirks++;
  else
  {
   int b;

   fail(label);
   printf("   owner rc %d %d..%d, reference rc %d %d..%d\n   row:", rc_own, own.so, own.eo,
          rc_ref, ref.so, ref.eo);
   for (b = 0; b < len && b < 40; b++)
    printf(" %02X", row[b]);
   printf("\n");
   if (fails > 20)
    exit(1);
  }
 }
 if (rc_own == -1 && rc_ref == -1 && wpe_rowfind_refuses(text, sw))
  differential_refused++;
 if (rc_own == 1)
  check_hit(label, fd, row, len, from, to, how, &own);
 row_drop(&placed);
 free(before);
}

static void differential(int iterations, int long_rows)
{
 int it;

 for (it = 0; it < iterations; it++)
 {
  unsigned char tmp[2100];
  char text[200];
  unsigned sw, kind = rnd(4), how = 0;
  int len, from, to;

  len = make_row(tmp, (int)sizeof tmp, long_rows);
  make_text(text, &sw);
  if (rnd(2))
   how |= BWD;
  if (rnd(3) == 0)
   how |= AFT;
  from = (int)rnd((unsigned)len + 3) - 1;
  to = (rnd(4) == 0) ? (int)rnd((unsigned)len + 3) - 1 : len - (int)rnd((unsigned)len + 1);
  if (to < from && rnd(4) != 0)
  {
   int room = len + 1 - (from < 0 ? 0 : from);

   to = from + (int)rnd((unsigned)(room > 0 ? room : 1));
  }
  compare_case(text, sw, tmp, len, kind, from, to, how, it);
 }
}

/* Pinned rows of the differential: glibc's \B over plain ASCII disagrees
   with itself, so the skip must be by that measure and not by the row's
   bytes (a skip by bytes fails this row although the owner is right). */
static void pinned_rows(void)
{
 static const char row[] = " ..e";

 engine_quirk_rows();
 compare_case(".*\\B", SW_REGEX | SW_CASE, (const unsigned char *)row, (int)strlen(row),
              KIND_BOTH, 0, (int)strlen(row), BWD, -1);
}

/* ---- progress: Search Again and Change All on rows wrapped at a width ---- */

#define LINE_MAX_BYTES 600
#define LINES_MAX 4
#define ROWS_MAX 256

typedef struct {
 unsigned char t[LINE_MAX_BYTES];
 int orig[LINE_MAX_BYTES];       /* the old byte a byte came from, -1 for the New Text */
 int len, oldlen;
 unsigned char placed[LINE_MAX_BYTES + 2]; /* old places an empty hit was found at */
} Line;

typedef struct { int line, start, len; unsigned kind; } Row;

typedef struct {
 Line line[LINES_MAX];
 int nlines, width;
 Row row[ROWS_MAX];
 int nrows;
} Model;

static int model_cut(const Line *l, int start, int width)
{
 int n = l->len - start, cut;

 if (n <= width)
  return(n);
 cut = e_utf8_glyph_floor(l->t + start, n, width);
 return(cut > 0 ? cut : width);
}

static void model_wrap(Model *m)
{
 int i;

 m->nrows = 0;
 for (i = 0; i < m->nlines; i++)
 {
  const Line *l = &m->line[i];
  int start = 0;

  do {
   int n = model_cut(l, start, m->width);
   Row *r = &m->row[m->nrows++];

   r->line = i;
   r->start = start;
   r->len = n;
   r->kind = (start == 0 ? WPE_ROW_STARTS_LINE : 0u) | (start + n >= l->len ? WPE_ROW_ENDS_LINE : 0u);
   start += n;
  } while (start < l->len && m->nrows < ROWS_MAX);
 }
}

static void model_make(Model *m, int width)
{
 int i, k;

 memset(m, 0, sizeof *m);
 m->width = width;
 m->nlines = 1 + (int)rnd(LINES_MAX);
 for (i = 0; i < m->nlines; i++)
 {
  Line *l = &m->line[i];

  int target = (rnd(3) == 0) ? (int)rnd(8) : (int)rnd(3 * (unsigned)width);

  l->len = 0;
  while (l->len < target)
  {
   if (rnd(6) == 0)
   {
    const char *u = UNITS[rnd(UNIT_COUNT)];
    int n = (int)strlen(u);

    memcpy(l->t + l->len, u, (size_t)n);
    l->len += n;
   }
   else
    l->t[l->len++] = (unsigned char)ALPHA[rnd((unsigned)strlen(ALPHA))];
  }
  for (k = 0; k < l->len; k++)
   l->orig[k] = k;
  l->oldlen = l->len;
 }
 model_wrap(m);
}

/* Normal name of the place (row, col) in the model. */
static void model_place(const Model *m, int row, int col, int *py, int *px)
{
 const Row *r = &m->row[row];

 *py = row;
 *px = col;
 wpe_rowfind_text_place(py, px, r->len, r->kind);
}

/* The row and column a logical offset of a line is shown at; at a soft edge
   either name, by a coin. */
static void model_locate(const Model *m, int line, int off, int *row, int *col)
{
 int i;

 for (i = 0; i < m->nrows; i++)
 {
  const Row *r = &m->row[i];

  if (r->line != line)
   continue;
  if (off >= r->start && off < r->start + r->len)
  {
   *row = i;
   *col = off - r->start;
   return;
  }
  if (off == r->start + r->len && ((r->kind & WPE_ROW_ENDS_LINE) || rnd(2)))
  {
   *row = i;
   *col = off - r->start;
   return;
  }
 }
 for (i = 0; i < m->nrows; i++)       /* off at the end of a soft row, coin said the next */
  if (m->row[i].line == line && off == m->row[i].start)
  {
   *row = i;
   *col = 0;
   return;
  }
 *row = m->nrows - 1;
 *col = m->row[*row].len;
}

static int model_total_bytes(const Model *m)
{
 int i, n = 0;

 for (i = 0; i < m->nlines; i++)
  n += m->line[i].len;
 return(n);
}

/* One search from the cursor, as e_find_next goes over the rows.  The place
   a search starts at on each row is asked of the owner as the resume place. */
static int model_find(const Model *m, const FIND *fd, int backward, int y, int x, int *hrow, WpeRowHit *h)
{
 int j;

 if (!backward)
 {
  for (j = y; j < m->nrows; j++)
  {
   const Row *r = &m->row[j];
   int from = (j == y) ? x : 0, py, px, rc;
   unsigned how = 0;
   unsigned char *copy = row_copy(m->line[r->line].t + r->start, r->len);

   model_place(m, j, from, &py, &px);
   if (wpe_rowfind_resumes_at(py, px, 0))
    how |= AFT;
   rc = wpe_rowfind_row(fd, copy, r->len, r->kind, from, r->len, how, h);
   free(copy);
   if (rc == 1)
   {
    *hrow = j;
    return(1);
   }
  }
  return(0);
 }
 for (j = y; j >= 0; j--)
 {
  const Row *r = &m->row[j];
  int to = (j == y) ? x : r->len, py, px, rc;
  unsigned how = BWD;
  unsigned char *copy = row_copy(m->line[r->line].t + r->start, r->len);

  model_place(m, j, to, &py, &px);
  if (wpe_rowfind_resumes_at(py, px, 1))
   how |= AFT;
  rc = wpe_rowfind_row(fd, copy, r->len, r->kind, 0, to, how, h);
  free(copy);
  if (rc == 1)
  {
   *hrow = j;
   return(1);
  }
 }
 return(0);
}

static void model_note(const Model *m, int row, int col, int backward)
{
 int py, px;

 model_place(m, row, col, &py, &px);
 wpe_rowfind_note_resume(py, px, backward);
}

static void model_progress(int search_again, int backward, int width)
{
 Model m;
 char text[200], label[300];
 unsigned sw;
 const FIND *fd;
 int y, x, steps = 0, bound, prev_line = -1, prev_so = 0, prev_eo = 0, first = 1;
 int rn = (int)rnd(4);
 char news[64];
 int i;

 model_make(&m, width);
 make_text(text, &sw);
 if (rnd(3) != 0)
  sw &= ~(unsigned)SW_WORD;
 fd = fd_for(text, sw);
 snprintf(label, sizeof label, "progress: %s %s Find \"%s\" sw %u width %d",
          search_again ? "Search Again" : "Change All", backward ? "backward" : "forward",
          text, sw, width);
 if (rn == 3)
  rn = width;
 if (rn > 60)
  rn = 60;
 for (i = 0; i < rn; i++)
  news[i] = 'Z';
 news[rn] = '\0';
 for (i = 0; i < m.nlines; i++)
  memset(m.line[i].placed, 0, sizeof m.line[i].placed);
 wpe_rowfind_forget_resume();
 if (backward)
 {
  y = m.nrows - 1;
  x = m.row[y].len;
 }
 else
 {
  y = 0;
  x = 0;
 }
 bound = 2 * (model_total_bytes(&m) + m.nlines) + m.nrows + 8;
 for (;;)
 {
  WpeRowHit h;
  int hrow, line, so_off, eo_off, k, nrow, ncol;
  Line *l;

  if (!model_find(&m, fd, backward, y, x, &hrow, &h))
   break;
  if (++steps > bound)
  {
   fail(label);
   printf("   no end within %d steps\n", bound);
   return;
  }
  line = m.row[hrow].line;
  l = &m.line[line];
  so_off = m.row[hrow].start + h.so;
  eo_off = m.row[hrow].start + h.eo;
  if (search_again)
  {
   /* the hits strictly advance along the text */
   if (!first && prev_line == line &&
       (backward ? !(eo_off <= prev_so && (so_off < prev_so || eo_off < prev_eo))
                 : !(so_off >= prev_eo && (so_off > prev_so || eo_off > prev_eo))))
   {
    fail(label);
    printf("   hit %d..%d does not advance from %d..%d\n", so_off, eo_off, prev_so, prev_eo);
    return;
   }
   if (!first && (backward ? line > prev_line : line < prev_line))
   {
    fail(label);
    printf("   hit went to an earlier place\n");
    return;
   }
   first = 0;
   prev_line = line;
   prev_so = so_off;
   prev_eo = eo_off;
   if (backward)
   {
    y = hrow;
    x = h.so;
    model_note(&m, y, x, 1);
   }
   else
   {
    y = hrow;
    x = h.eo;
    model_note(&m, y, x, 0);
   }
   continue;
  }
  /* Change All: the hit lies in old text, an empty hit once per old place */
  for (k = so_off; k < eo_off; k++)
   if (l->orig[k] < 0)
   {
    fail(label);
    printf("   hit %d..%d covers text the replacement made\n", so_off, eo_off);
    return;
   }
  if (so_off == eo_off)
  {
   int place = l->oldlen;

   for (k = so_off; k < l->len; k++)
    if (l->orig[k] >= 0)
    {
     place = l->orig[k];
     break;
    }
   if (l->placed[place])
   {
    fail(label);
    printf("   an empty hit found twice at old place %d of line %d\n", place, line);
    return;
   }
   l->placed[place] = 1;
  }
  if (l->len - (eo_off - so_off) + rn >= LINE_MAX_BYTES)
   break;
  memmove(l->t + so_off + rn, l->t + eo_off, (size_t)(l->len - eo_off));
  memmove(l->orig + so_off + rn, l->orig + eo_off, (size_t)(l->len - eo_off) * sizeof(int));
  memcpy(l->t + so_off, news, (size_t)rn);
  for (k = 0; k < rn; k++)
   l->orig[so_off + k] = -1;
  l->len += rn - (eo_off - so_off);
  model_wrap(&m);
  model_locate(&m, line, backward ? so_off : so_off + rn, &nrow, &ncol);
  y = nrow;
  x = ncol;
  model_note(&m, y, x, backward);
 }
}

static void progress_models(int iterations)
{
 static const int widths[] = { 8, 9, 12, 20 };
 int it;

 for (it = 0; it < iterations; it++)
 {
  int width = widths[rnd(4)];

  checks++;
  model_progress(it % 2, (it / 2) % 2, width);
  if (fails > 20)
   exit(1);
 }
}

/* ---- speed ---- */

static double now_ms(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return(ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6);
}

static int perf_wanted(void)
{
 const char *e = getenv("ROWFIND_NO_PERF");

#if defined(__SANITIZE_ADDRESS__) || defined(WPE_ROWFIND_MOCK_ILLSEQ) || defined(WPE_ROWFIND_MOCK_BADENGINE)
 return(0);   /* the mock engine reads each row to check it, which is no speed to bound */
#endif
 return(!(e != NULL && e[0] == '1'));
}

/* Backward over a whole row to the last hit, one call per hit. */
static int exhaust_backward(const FIND *fd, const unsigned char *row, int len, int *calls)
{
 int to = len, hits = 0;
 unsigned how = BWD;

 for (;;)
 {
  WpeRowHit h;
  int rc = wpe_rowfind_row(fd, row, len, KIND_BOTH, 0, to, how, &h);

  (*calls)++;
  if (rc != 1)
   return(hits);
  hits++;
  to = h.so;
  how = (h.so == h.eo) ? (BWD | AFT) : BWD;
 }
}

static void perf_rows(void)
{
 unsigned char *row;
 int i, len, calls, hits;
 double t0, ms;
 char msg[200];

 /* the shape the review measured: 20000 bytes, 4000 hits, backward to the start */
 len = 20000;
 row = malloc((size_t)len + 2);
 for (i = 0; i < len; i++)
  row[i] = (i % 5 == 4) ? 'a' : 'b';
 row[len] = '\n';
 row[len + 1] = '\0';
 {
  static const unsigned sws[] = { SW_CASE, SW_CASE | SW_REGEX, 0 };
  static const char *names[] = { "plain", "regular", "plain ICASE" };

  for (i = 0; i < 3; i++)
  {
   calls = 0;
   t0 = now_ms();
   hits = exhaust_backward(fd_for("a", sws[i]), row, len, &calls);
   ms = now_ms() - t0;
   printf("perf [%s]: backward to exhaustion, 20000 bytes, %s: %d hits in %.1f ms\n",
          pass_name, names[i], hits, ms);
   checks++;
   if (hits != 4000)
    fail("perf: wrong number of hits");
   if (perf_wanted() && ms > 50.0)
   {
    snprintf(msg, sizeof msg, "perf: backward %s took %.1f ms, bound 50", names[i], ms);
    fail(msg);
   }
  }
 }
 free(row);
 /* the 2000-byte shape: every byte a hit of `.`, and `a*` on a row of a */
 len = 2000;
 row = malloc((size_t)len + 2);
 memset(row, 'a', (size_t)len);
 row[len] = '\n';
 row[len + 1] = '\0';
 {
  static const char *pats[] = { ".", "a*" };

  for (i = 0; i < 2; i++)
  {
   calls = 0;
   t0 = now_ms();
   hits = exhaust_backward(fd_for(pats[i], SW_CASE | SW_REGEX), row, len, &calls);
   ms = now_ms() - t0;
   printf("perf [%s]: backward, 2000 bytes, `%s`: %d hits in %.1f ms\n", pass_name, pats[i], hits, ms);
   checks++;
   if (perf_wanted() && ms > 500.0)
   {
    snprintf(msg, sizeof msg, "perf: backward `%s` took %.1f ms, bound 500", pats[i], ms);
    fail(msg);
   }
  }
 }
 free(row);
 /* forward: Change All over 100000 rows of 80 bytes */
 {
  unsigned char r80[82];
  int rows = 100000, n = 0;
  static const unsigned sws[] = { SW_CASE, 0 };

  memset(r80, 'x', 80);
  memcpy(r80 + 10, "foo", 3);
  memcpy(r80 + 40, "foo", 3);
  r80[80] = '\n';
  r80[81] = '\0';
  for (i = 0; i < 2; i++)
  {
   int j;

   n = 0;
   t0 = now_ms();
   for (j = 0; j < rows; j++)
   {
    WpeRowHit h;
    int from = 0;

    while (wpe_rowfind_row(fd_for("foo", sws[i]), r80, 80, KIND_BOTH, from, 80, 0, &h) == 1)
    {
     n++;
     from = h.eo;
    }
   }
   ms = now_ms() - t0;
   printf("perf [%s]: forward, 100000 rows of 80 bytes, plain%s: %d hits in %.1f ms\n",
          pass_name, i ? " ICASE" : "", n, ms);
   checks++;
   if (perf_wanted() && ms > 2000.0)
    fail("perf: forward over 100000 rows took more than 2 s");
  }
 }
}

/* ---- the passes ---- */

#define GUARDED_DIFFERENTIAL_ROWS 500

/** differential_guarded - The differential on rows with only the line break
 * readable behind them: a read at row[len + 1] by the owner, the engine or the
 * reference is a fault that stops the test. */
static void differential_guarded(int iterations)
{
 row_mode = ROW_GUARDED;
 differential(iterations, 0);
 row_mode = ROW_COPY;
}

static void one_pass(const char *locale, int with_perf, int iterations)
{
 pass_name = locale;
 if (setlocale(LC_ALL, locale) == NULL)
 {
  printf("skip: locale %s is not installed\n", locale);
  return;
 }
 wpe_rowfind_release();
 rng_state = 88172645463325252ULL;
 named_rows();
 row_mode = ROW_GUARDED;
 named_rows();
 row_mode = ROW_COPY;
 boundary_rows();
#ifdef WPE_ROWFIND_MOCK_BADENGINE
 badengine_rows();
#endif
 diagnostic_reads_behind();
 pinned_rows();
 differential(iterations, 0);
 differential(iterations / 200 + 5, 1);
 progress_models(iterations / 4);
 differential_guarded(GUARDED_DIFFERENTIAL_ROWS);
 if (with_perf)
  perf_rows();
 wpe_rowfind_release();
}

/* Whether the copy build must differ from the reference.  The copy path
   differs from the REG_STARTEND path in one thing only: the byte before the
   call's start, which only \b \B \< \> read.  So the difference exists only
   where such an escape reaches the engine: the engine understands the word
   escapes at all (WPE_HAS_REGEX_WORD_ESCAPES) and the owner does not refuse
   them (WPE_ROWFIND_BOUNDARY_SAFE).  Everywhere else the two paths are the
   same by design and the copy build must show no difference. */
#if !OWNER_SEES_BEFORE && WPE_HAS_REGEX_WORD_ESCAPES && WPE_ROWFIND_BOUNDARY_SAFE
#define COPY_DIFFERENCE_EXPECTED 1
#else
#define COPY_DIFFERENCE_EXPECTED 0
#endif

/** check_copy_difference - Prove the copy path's look-behind difference where it can exist.
 *
 * Find on a system without REG_STARTEND runs the engine on a copy of the row,
 * so a word boundary at the start of a call is judged without the byte before
 * it.  This says, at the end of the run, whether the build showed that
 * difference when it had to, and showed none when no word-boundary escape
 * could reach the engine, so a copy path that drifts from the reference where
 * nothing should differ is a failure too and not a silence.  Reads
 * copy_differences, counted by the differential. */
static void check_copy_difference(void)
{
 if (OWNER_SEES_BEFORE)
  return;
 if (COPY_DIFFERENCE_EXPECTED)
 {
  if (copy_differences == 0)
  {
   printf("FAIL: the copy build shows no look-behind difference (the expected one is untested)\n");
   fails++;
  }
  else
   printf("copy path: %d differential rows differ on a word boundary, as expected\n", copy_differences);
  return;
 }
 printf("SKIP: the copy build's look-behind difference: needs a word-boundary escape the engine is measured "
        "safe for (WPE_HAS_REGEX_WORD_ESCAPES, WPE_ROWFIND_BOUNDARY_SAFE)\n");
 if (copy_differences != 0)
 {
  printf("FAIL: the copy build differs from the reference on %d rows with no word-boundary escape in play\n",
         copy_differences);
  fails++;
 }
 else
  printf("copy path: 0 differential rows differ on a word boundary, as expected\n");
}

int main(void)
{
 const char *e = getenv("ROWFIND_ITERS");
 int iterations = (e != NULL && atoi(e) > 0) ? atoi(e) : 20000;

 if (!TEST_HAS_LOCALES)
  printf("skip: the pass that runs the reference under its own UTF-8 ctype "
         "(this system has no uselocale, as NetBSD); the owner uses the process locale here\n");
 tokens_choose();
 printf("engine: reads UTF-8 characters %s, understands \\b \\B \\w \\s %s\n",
        WPE_HAS_REGEX_MULTIBYTE ? "yes" : "no", WPE_HAS_REGEX_GNU_ESCAPES ? "yes" : "no");
 one_pass("C", 0, iterations);
 one_pass("C.UTF-8", 1, iterations);
 one_pass("en_US.iso885915", 0, iterations);
 check_copy_difference();
 printf("reference: byte fallback taken %d times (compile %d, exec %d)\n",
        ref_compile_fallbacks + ref_exec_fallbacks, ref_compile_fallbacks, ref_exec_fallbacks);
 printf("refused: %d differential rows (the engine is measured unsafe for them)\n",
        differential_refused);
 if (window_differences > 0)
  printf("owner rule: %d backward word-boundary rows differ where the engine refused part of the row "
         "(the owner goes by bytes from each window's start; open question for the search owner)\n",
         window_differences);
 if (byte_engine_quirks > 0)
  printf("engine quirk: %d differential rows skipped, the engine disagrees with itself on a word boundary\n",
         byte_engine_quirks);
 if (fails == 0)
  printf("test_rowfind: %d checks, all green\n", checks);
 else
  printf("test_rowfind: %d FAILED of %d checks\n", fails, checks);
 return(fails == 0 ? 0 : 1);
}
