/* test_fmt.c -- unit test for the owner of a formatted text written into
 * storage (we_fmt.c).  Always runs; run it under ASan+UBSan too.
 *
 * What the user sees behind each group:
 *  - a number in a place of its own (line:column, a count) is always shown
 *    in full; the place is as large as the type, and a place that is not
 *    refuses the text instead of writing past its end;
 *  - a command, a path, a key or a protocol line that does not fit is not
 *    written at all, so it is never run or sent as a prefix of itself;
 *  - a sentence that does not fit ends in "..." cut where a character ends,
 *    never in half a letter;
 *  - a request built in blocks gets every block that fits and never carries
 *    a mark into the request;
 *  - a text built from pieces stops at the first piece that did not fit.
 *
 * Every storage is followed by guard bytes of a known value, so a byte
 * written past its size fails here in a plain build, with no sanitizer.
 */
#include <stdio.h>
#include "linebuf.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "../we_fmt.h"

#define GUARD 0x55
#define GUARD_ROOM 64
#define EURO "\xE2\x82\xAC"
/* The most any row stores. */
#define MEM_MAX (512 + GUARD_ROOM)

static int fails;

static void chk_int(const char *what, long got, long want)
{
  if (got != want)
  {
    printf("FAIL: %s: got %ld want %ld\n", what, got, want);
    fails++;
  }
}

static void chk_str(const char *what, const char *got, const char *want)
{
  if (strcmp(got, want) != 0)
  {
    printf("FAIL: %s: got [%s] want [%s]\n", what, got, want);
    fails++;
  }
}

/* A storage of @n bytes followed by guard bytes, all set before the call. */
static char *fresh(unsigned char *mem, size_t n)
{
  memset(mem, GUARD, n + GUARD_ROOM);
  return((char *) mem);
}

/* The bytes past the storage's size @n were not written. */
static void chk_guard(const char *what, const unsigned char *mem, size_t n)
{
  size_t i;

  for (i = n; i < n + GUARD_ROOM; i++)
    if (mem[i] != GUARD)
    {
      printf("FAIL: %s: byte %zu past the size was written\n", what, i - n);
      fails++;
      return;
    }
}

/* @scenario SC-fmt-01 */
/* #202: the line and column pair is refused in 20 bytes, never written past. */
static void test_pair_refused_at_20(void)
{
  unsigned char mem[MEM_MAX];
  char *b = fresh(mem, 20);

  chk_int("F1 refused", wpe_fmt_whole(b, 20, "%5d:%-4d", 1000000000, 1000000000), WPE_FMT_NO_ROOM);
  chk_str("F1 empty", b, "");
  chk_guard("F1 guard", mem, 20);
}

/* @scenario SC-fmt-02 */
/* The pair's own room holds the largest pair of ints. */
static void test_pair_fits_its_room(void)
{
  unsigned char mem[MEM_MAX];
  size_t n = 2 * WPE_FMT_INT_ROOM + 2;
  char *b = fresh(mem, n);

  chk_int("F2 room", (long) n, 24);
  chk_int("F2 whole", wpe_fmt_whole(b, n, "%5d:%-4d", INT_MIN, INT_MIN), WPE_FMT_OK);
  chk_str("F2 text", b, "-2147483648:-2147483648");
  chk_guard("F2 guard", mem, n);
}

/* @scenario SC-fmt-03 */
/* #201: the 81-byte replace report is refused in 80 bytes. */
static void test_report_refused_at_80(void)
{
  unsigned char mem[MEM_MAX];
  char *b = fresh(mem, 80);

  chk_int("F3 refused", wpe_fmt_whole(b, 80, "Found %d\nReplaced %d\n%s", 2000000000, 2000000000,
                                      "Out of memory: the replacement was not made"),
          WPE_FMT_NO_ROOM);
  chk_str("F3 empty", b, "");
  chk_guard("F3 guard", mem, 80);
}

/* @scenario SC-fmt-04 */
/* A shown sentence that does not fit is cut and marked. */
static void test_shown_cut_and_marked(void)
{
  unsigned char mem[MEM_MAX];
  char *b = fresh(mem, 10);

  chk_int("F4 cut", wpe_fmt_shown(b, 10, "%s", "abcdefghijklmnop"), 1);
  chk_str("F4 text", b, "abcdef...");
  chk_guard("F4 guard", mem, 10);
  b = fresh(mem, 10);
  chk_int("F4 whole", wpe_fmt_shown(b, 10, "%s", "abcdefghi"), 0);
  chk_str("F4 whole text", b, "abcdefghi");
  chk_guard("F4 whole guard", mem, 10);
}

/* @scenario SC-fmt-05 */
/* A character across the cut is dropped whole, in every place of the cut. */
static void test_shown_cut_at_a_character(void)
{
  unsigned char mem[MEM_MAX];
  char *b;
  size_t n, body;

  for (n = 5; n <= 14; n++)
  {
    b = fresh(mem, n);
    chk_int("F5 cut", wpe_fmt_shown(b, n, "ab" EURO EURO EURO EURO), 1);
    chk_str("F5 mark", b + strlen(b) - 3, "...");
    body = strlen(b) - 3;
    chk_int("F5 body ends a character", body < 2 || (body - 2) % 3 == 0, 1);
    chk_guard("F5 guard", mem, n);
  }
  b = fresh(mem, 9);
  chk_int("F5 exact", wpe_fmt_shown(b, 9, "ab" EURO EURO EURO EURO), 1);
  chk_str("F5 exact text", b, "ab" EURO "...");
}

/* @scenario SC-fmt-06 */
/* The mark at the smallest places: a place of the mark's size and a byte
   holds the mark; a smaller one keeps what fits and no half character. */
static void test_shown_smallest_places(void)
{
  unsigned char mem[MEM_MAX];
  char *b = fresh(mem, 4);

  chk_int("F6a cut", wpe_fmt_shown(b, 4, "%s", "abcdef"), 1);
  chk_str("F6a the mark alone", b, "...");
  chk_guard("F6a guard", mem, 4);
  b = fresh(mem, 3);
  chk_int("F6b cut", wpe_fmt_shown(b, 3, "%s", "abcdef"), 1);
  chk_str("F6b no mark", b, "ab");
  chk_guard("F6b guard", mem, 3);
  b = fresh(mem, 3);
  chk_int("F6c cut", wpe_fmt_shown(b, 3, "a" EURO), 1);
  chk_str("F6c no half character", b, "a");
  chk_guard("F6c guard", mem, 3);
  b = fresh(mem, 1);
  chk_int("F6d cut", wpe_fmt_shown(b, 1, "%s", "abc"), 1);
  chk_str("F6d empty", b, "");
  chk_guard("F6d guard", mem, 1);
}

/* @scenario SC-fmt-07 */
/* A text of no known bound is whole, in memory of its own. */
static void test_heap_dup(void)
{
  char *p = wpe_fmt_dup("%s/%s%d", "folder", "file", 42);

  chk_str("F7 text", p ? p : "(null)", "folder/file42");
  free(p);
  p = wpe_fmt_dup("%s", "");
  chk_str("F7 empty", p ? p : "(null)", "");
  free(p);
}

/* @scenario SC-fmt-08 */
/* Pieces of a whole text: after the first that does not fit nothing is added
   and the text is "" with the refusal said at the end. */
static void test_pieces_whole(void)
{
  unsigned char mem[MEM_MAX];
  char *b = fresh(mem, 16);
  WpeFmtText t;
  size_t len = 99;

  wpe_fmt_begin_whole(&t, b, 16);
  wpe_fmt_add(&t, "files: ");
  wpe_fmt_add(&t, "%s", "one two three four");
  chk_int("F8 room after the refusal", (long) wpe_fmt_room(&t), 0);
  wpe_fmt_add(&t, "%s", "x");
  chk_int("F8 end", wpe_fmt_end(&t, &len), WPE_FMT_NO_ROOM);
  chk_int("F8 len", (long) len, 0);
  chk_str("F8 text", b, "");
  chk_guard("F8 guard", mem, 16);
}

/* @scenario SC-fmt-09 */
/* Pieces of a shown text: the piece that does not fit is cut and marked. */
static void test_pieces_shown(void)
{
  unsigned char mem[MEM_MAX];
  char *b = fresh(mem, 16);
  WpeFmtText t;
  size_t len = 0;

  wpe_fmt_begin_shown(&t, b, 16);
  wpe_fmt_add(&t, "files: ");
  wpe_fmt_add(&t, "%s", "one two three four");
  wpe_fmt_add(&t, "%s", "never added");
  chk_int("F9 end", wpe_fmt_end(&t, &len), 1);
  chk_str("F9 text", b, "files: one t...");
  chk_int("F9 len", (long) len, 15);
  chk_guard("F9 guard", mem, 16);
}

/* @scenario SC-fmt-10 */
/* A heap text of 120 pieces (it grows past its first room) is as long as the
   pieces are. */
static void test_pieces_heap(void)
{
  WpeFmtText t;
  char *p;
  size_t len = 0, i, want = 0;
  char piece[16];

  wpe_fmt_begin_heap(&t);
  for (i = 0; i < 120; i++)
  {
    want += (size_t) snprintf(piece, sizeof piece, "%zu,", i);
    wpe_fmt_add(&t, "%zu,", i);
  }
  p = wpe_fmt_end_heap(&t, &len);
  chk_int("F10 text", p != NULL, 1);
  chk_int("F10 len", (long) len, (long) want);
  chk_int("F10 strlen", p ? (long) strlen(p) : -1, (long) want);
  free(p);
  wpe_fmt_begin_heap(&t);
  p = wpe_fmt_end_heap(&t, &len);
  chk_str("F10 empty heap", p ? p : "(null)", "");
  free(p);
}

/* @scenario SC-fmt-11 */
/* The array faces give the array's own size. */
static void test_array_faces(void)
{
  struct { char a[8]; unsigned char guard[GUARD_ROOM]; } s;

  memset(&s, GUARD, sizeof s);
  chk_int("F11 fits", WPE_FMT_WHOLE(s.a, "%s", "1234567"), WPE_FMT_OK);
  chk_str("F11 fits text", s.a, "1234567");
  chk_int("F11 refuses", WPE_FMT_WHOLE(s.a, "%s", "12345678"), WPE_FMT_NO_ROOM);
  chk_str("F11 refuses text", s.a, "");
  chk_int("F11 shows", WPE_FMT_SHOWN(s.a, "%s", "1234567890"), 1);
  chk_str("F11 shows text", s.a, "1234...");
  chk_guard("F11 guard", (unsigned char *) &s, sizeof s.a);
}

/* @scenario SC-fmt-12 */
/* A block is added whole or not at all; the next block still gets its turn
   and no mark enters the text. */
static void test_blocks(void)
{
  unsigned char mem[MEM_MAX];
  char *b = fresh(mem, 24);
  WpeFmtText t;
  size_t len = 0;

  wpe_fmt_begin_block(&t, b, 24);
  chk_int("B1 first", wpe_fmt_add_block(&t, "%s", "system prompt"), 1);
  chk_int("B2 too long", wpe_fmt_add_block(&t, "%s", "open files: a.c b.c c.c"), 0);
  chk_str("B2 text unchanged", b, "system prompt");
  chk_int("B3 the next block fits", wpe_fmt_add_block(&t, "|%s", "sel"), 1);
  chk_int("B3 end", wpe_fmt_end(&t, &len), WPE_FMT_OK);
  chk_str("B3 text", b, "system prompt|sel");
  chk_int("B3 len", (long) len, 17);
  chk_int("B3 no dots", strstr(b, "...") == NULL, 1);
  chk_guard("B guard", mem, 24);
}

/* @scenario SC-fmt-13 */
/* A path cut at its front keeps the end of the name, whole characters. */
static void test_shown_tail(void)
{
  unsigned char mem[MEM_MAX];
  char *b = fresh(mem, 12);

  chk_int("T1 cut", wpe_fmt_shown_tail(b, 12, "/home/user/project/file.c"), 1);
  chk_str("T1 text", b, "...t/file.c");
  chk_guard("T1 guard", mem, 12);
  b = fresh(mem, 12);
  chk_int("T2 whole", wpe_fmt_shown_tail(b, 12, "a/b.c"), 0);
  chk_str("T2 text", b, "a/b.c");
  b = fresh(mem, 8);
  chk_int("T3 cut", wpe_fmt_shown_tail(b, 8, "/dir/" EURO EURO EURO ".c"), 1);
  chk_int("T3 starts a character", (unsigned char) b[3] != 0x82 && (unsigned char) b[3] != 0xAC, 1);
  chk_guard("T3 guard", mem, 8);
}

/* @scenario SC-fmt-14 */
/* A name in a place of its own inside a longer sentence. */
static void test_add_shown(void)
{
  unsigned char mem[MEM_MAX];
  char *b = fresh(mem, 64);
  WpeFmtText t;

  wpe_fmt_begin_shown(&t, b, 64);
  wpe_fmt_add(&t, "name: ");
  wpe_fmt_add_shown(&t, 10, "abcdefghijklmnop");
  wpe_fmt_add(&t, " end");
  chk_int("S1 end", wpe_fmt_end(&t, NULL), WPE_FMT_OK);
  chk_str("S1 text", b, "name: abcdefg... end");
  b = fresh(mem, 64);
  wpe_fmt_begin_shown(&t, b, 64);
  wpe_fmt_add_shown(&t, 10, "short");
  chk_str("S2 text", b, "short");
  chk_guard("S guard", mem, 64);
}

/* @scenario SC-fmt-15 */
/* The finish of the wrong kind is refused. */
static void test_end_of_the_wrong_kind(void)
{
  unsigned char mem[MEM_MAX];
  char *b = fresh(mem, 16);
  WpeFmtText t;

  wpe_fmt_begin_heap(&t);
  wpe_fmt_add(&t, "x");
  chk_int("K1 end of a heap text", wpe_fmt_end(&t, NULL), WPE_FMT_FAILED);
  free(wpe_fmt_end_heap(&t, NULL));
  wpe_fmt_begin_whole(&t, b, 16);
  chk_int("K2 end_heap of a fixed text", wpe_fmt_end_heap(&t, NULL) == NULL, 1);
}

/* @scenario SC-fmt-16 */
/* The cut length of a slice never splits a character. */
static void test_cut_len(void)
{
  chk_int("C1 plain", wpe_fmt_cut_len("abcdef", 3), 3);
  chk_int("C2 across a character", wpe_fmt_cut_len("a" EURO "b", 3), 1);
  chk_int("C3 at its end", wpe_fmt_cut_len("a" EURO "b", 4), 4);
  chk_int("C4 shorter than the limit", wpe_fmt_cut_len("ab", 9), 2);
}

/* @scenario SC-fmt-17 */
/* A heap text past the one limit of every system is refused. */
static void test_heap_limit(void)
{
  WpeFmtText t;
  size_t i;
  char big[WPE_FMT_HEAP_LIMIT + 8];

  for (i = 0; i < sizeof big - 1; i++)
    big[i] = 'x';
  big[sizeof big - 1] = '\0';
  chk_int("L1 dup past the limit", wpe_fmt_dup("%s", big) == NULL, 1);
  wpe_fmt_begin_heap(&t);
  wpe_fmt_add(&t, "%s", big);
  chk_int("L2 heap text past the limit", wpe_fmt_end_heap(&t, NULL) == NULL, 1);
}

/* @scenario SC-fmt-18 */
/* A text that was cut and marked stays closed: resuming it and adding a
   piece does not write after the mark. */
static void test_cut_text_stays_closed(void)
{
  unsigned char mem[MEM_MAX];
  char *b = fresh(mem, 10);
  WpeFmtText t;

  chk_int("R1 cut", wpe_fmt_shown(b, 10, "%s", "abc" EURO EURO EURO), 1);
  chk_str("R1 marked", b, "abc" EURO "...");
  b = fresh(mem, 10);
  chk_int("R2 cut", wpe_fmt_shown(b, 7, "%s", "abc\xF0\x9F\x98\x80\xF0\x9F\x98\x80"), 1);
  chk_str("R2 marked", b, "abc...");
  wpe_fmt_resume_shown(&t, b, 10);
  wpe_fmt_add(&t, "; x");
  chk_str("R3 closed after the mark", b, "abc...");
  chk_int("R3 length", (long) wpe_fmt_len(&t), 6);
  chk_int("R3 not failed", wpe_fmt_failed(&t), 0);
  chk_guard("R guard", mem, 10);
  b = fresh(mem, 16);
  b[0] = '\0';
  wpe_fmt_resume_shown(&t, b, 16);
  wpe_fmt_add(&t, "one");
  wpe_fmt_add(&t, "; two");
  chk_str("R4 open text goes on", b, "one; two");
  chk_int("R4 length", (long) wpe_fmt_len(&t), 8);
  wpe_fmt_begin_whole(&t, b, 4);
  wpe_fmt_add(&t, "toolong");
  chk_int("R5 failed", wpe_fmt_failed(&t), 1);
  chk_int("R5 end", wpe_fmt_end(&t, NULL), WPE_FMT_NO_ROOM);
}

int main(void)
{
  test_cut_text_stays_closed();
  test_pair_refused_at_20();
  test_pair_fits_its_room();
  test_report_refused_at_80();
  test_shown_cut_and_marked();
  test_shown_cut_at_a_character();
  test_shown_smallest_places();
  test_heap_dup();
  test_pieces_whole();
  test_pieces_shown();
  test_pieces_heap();
  test_array_faces();
  test_blocks();
  test_shown_tail();
  test_add_shown();
  test_end_of_the_wrong_kind();
  test_cut_len();
  test_heap_limit();
  if (fails)
  {
    printf("%d failures\n", fails);
    return(1);
  }
  printf("all ok\n");
  return(0);
}
