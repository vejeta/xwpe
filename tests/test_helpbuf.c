/* test_helpbuf.c -- unit test for the owner of a help or Info token taken
 * into storage (we_helpbuf.c).  Always runs; run it under ASan+UBSan too.
 *
 * What the user sees behind each group:
 *  - a link, node or file name of normal length is taken whole and the link
 *    jumps;
 *  - one too long for its buffer is cut at a character, never half of one,
 *    and the reader is told it was cut, so it refuses the jump instead of
 *    opening a node named by the cut prefix;
 *  - a name the reader needs whole is taken at its real length;
 *  - a cross-reference that does not fit in the line is not marked up, and
 *    the line stays as it was.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../we_helpbuf.h"

#define GUARD 0x55
#define EURO "\xE2\x82\xAC"

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

/* The bytes of a buffer past its size @n are never written. */
static void chk_guard(const char *what, const unsigned char *buf, size_t n, size_t total)
{
  size_t i;

  for (i = n; i < total; i++)
    if (buf[i] != GUARD)
    {
      printf("FAIL: %s: byte %zu past the size was written\n", what, i - n);
      fails++;
      return;
    }
}

/* @scenario SC-help-buffers-07 */
/* A token stops at its delimiter and keeps what fits. */
static void test_token_stops(void)
{
  unsigned char buf[32];
  const char *end = NULL;
  int cut = -1;
  const char *src = "Top, Next: B";
  size_t w;

  memset(buf, GUARD, sizeof buf);
  w = wpe_help_token((char *) buf, 16, src, strlen(src), ",\n", &end, &cut);
  chk_int("stop: written", (long) w, 3);
  chk_str("stop: text", (char *) buf, "Top");
  chk_int("stop: end at the comma", end - src, 3);
  chk_int("stop: not cut", cut, 0);
  chk_guard("stop", buf, 16, sizeof buf);
}

/* A stop at index 0 gives an empty token and an end at the start. */
static void test_token_stop_first(void)
{
  char buf[8];
  const char *end = NULL;
  int cut = -1;
  const char *src = ",x";

  wpe_help_token(buf, sizeof buf, src, strlen(src), ",", &end, &cut);
  chk_str("stop first: text", buf, "");
  chk_int("stop first: end", end - src, 0);
  chk_int("stop first: cut", cut, 0);
}

/* The scan never reads past srclen: the row's line break at s[len] is not
   part of the token, and end is src + srclen when no stop is inside it. */
static void test_token_source_bound(void)
{
  char buf[16];
  const char *end = NULL;
  const char *src = "name\nnext";

  wpe_help_token(buf, sizeof buf, src, 4, "\x05", &end, NULL);
  chk_str("srclen: text", buf, "name");
  chk_int("srclen: end", end - src, 4);
  wpe_help_token(buf, sizeof buf, src, 0, "\x05", &end, NULL);
  chk_str("srclen 0: text", buf, "");
  chk_int("srclen 0: end", end - src, 0);
}

/* A NUL ends a token, and end points at it. */
static void test_token_nul(void)
{
  char buf[16];
  const char *end = NULL;
  const char *src = "abc";

  wpe_help_token(buf, sizeof buf, src, 100, ",", &end, NULL);
  chk_str("nul: text", buf, "abc");
  chk_int("nul: end", end - src, 3);
}

/* A token over the size is cut to size-1 bytes and terminated; end still
   names where the whole token stopped. */
static void test_token_cut(void)
{
  unsigned char buf[16];
  const char *end = NULL;
  int cut = 0;
  const char *src = "abcdefghij,k";
  size_t w;

  memset(buf, GUARD, sizeof buf);
  w = wpe_help_token((char *) buf, 5, src, strlen(src), ",", &end, &cut);
  chk_int("cut: written", (long) w, 4);
  chk_str("cut: text", (char *) buf, "abcd");
  chk_int("cut: flag", cut, 1);
  chk_int("cut: end is the real stop", end - src, 10);
  chk_guard("cut", buf, 5, sizeof buf);
}

/* A token of exactly size-1 bytes fits whole. */
static void test_token_exact(void)
{
  char buf[5];
  int cut = -1;

  wpe_help_token(buf, sizeof buf, "abcd", 4, NULL, NULL, &cut);
  chk_str("exact: text", buf, "abcd");
  chk_int("exact: not cut", cut, 0);
}

/* An euro sign (3 bytes) straddling the cap is dropped whole, whichever of
   its three bytes the cap falls on. */
static void test_token_euro_straddle(void)
{
  char buf[16];
  const char *src = "ab" EURO "cd";
  size_t n;

  /* A buffer of 3 keeps 2 bytes: the first byte not kept is the euro's first. */
  n = wpe_help_token(buf, 3, src, strlen(src), NULL, NULL, NULL);
  chk_int("euro cap on byte 1: written", (long) n, 2);
  chk_str("euro cap on byte 1", buf, "ab");
  /* A buffer of 4 keeps 3: the first byte not kept is its second. */
  n = wpe_help_token(buf, 4, src, strlen(src), NULL, NULL, NULL);
  chk_int("euro cap on byte 2: written", (long) n, 2);
  chk_str("euro cap on byte 2", buf, "ab");
  /* A buffer of 5 keeps 4: the first byte not kept is its last. */
  n = wpe_help_token(buf, 5, src, strlen(src), NULL, NULL, NULL);
  chk_int("euro cap on byte 3: written", (long) n, 2);
  chk_str("euro cap on byte 3", buf, "ab");
  /* A buffer of 6 keeps 5: the euro fits whole. */
  n = wpe_help_token(buf, 6, src, strlen(src), NULL, NULL, NULL);
  chk_int("euro fits: written", (long) n, 5);
  chk_str("euro fits", buf, "ab" EURO);
}

/* A lead byte alone at the cap, and a stray continuation at the cap, are
   cut at the cap: nothing covers them, so nothing is dropped with them. */
static void test_token_lone_bytes(void)
{
  char buf[16];
  size_t n;

  n = wpe_help_token(buf, 4, "ab\xE2z", 4, NULL, NULL, NULL);
  chk_int("lone lead at the cap: written", (long) n, 3);
  chk_str("lone lead at the cap", buf, "ab\xE2");
  n = wpe_help_token(buf, 4, "abc\x82z", 5, NULL, NULL, NULL);
  chk_int("stray continuation at the cap: written", (long) n, 3);
  chk_str("stray continuation at the cap", buf, "abc");
}

/* n of 0 writes nothing at all; n of 1 writes only the terminator. */
static void test_token_tiny_sizes(void)
{
  unsigned char buf[4];
  int cut = -1;
  size_t w;

  memset(buf, GUARD, sizeof buf);
  w = wpe_help_token((char *) buf, 0, "abc", 3, NULL, NULL, &cut);
  chk_int("n=0: written", (long) w, 0);
  chk_int("n=0: cut", cut, 1);
  chk_int("n=0: byte 0 untouched", buf[0], GUARD);
  w = wpe_help_token((char *) buf, 1, "abc", 3, NULL, NULL, &cut);
  chk_int("n=1: written", (long) w, 0);
  chk_int("n=1: terminator", buf[0], 0);
  chk_int("n=1: byte 1 untouched", buf[1], GUARD);
  chk_int("n=1: cut", cut, 1);
  w = wpe_help_token((char *) buf, 1, "", 0, NULL, NULL, &cut);
  chk_int("n=1 empty: cut", cut, 0);
}

/* A key is taken whole at its real length, far past a fixed buffer. */
static void test_token_dup(void)
{
  char longname[600];
  char *copy;
  const char *end = NULL;
  char *src;

  memset(longname, 'n', sizeof longname - 1);
  longname[sizeof longname - 1] = '\0';
  src = malloc(sizeof longname + sizeof ",rest");
  strcpy(src, longname);
  strcat(src, ",rest");
  copy = wpe_help_token_dup(src, strlen(src), ",", &end);
  if (copy == NULL)
  {
    printf("FAIL: dup: no memory\n");
    fails++;
  }
  else
  {
    chk_int("dup: length", (long) strlen(copy), (long) strlen(longname));
    chk_str("dup: text", copy, longname);
    chk_int("dup: end", end - src, (long) strlen(longname));
    free(copy);
  }
  copy = wpe_help_token_dup(",x", 2, ",", NULL);
  if (copy)
  {
    chk_str("dup empty", copy, "");
    free(copy);
  }
  free(src);
}

/* lcpy and lcat cut at a character, terminate, and write nothing past n. */
static void test_lcpy_lcat(void)
{
  unsigned char buf[16];
  size_t len;

  memset(buf, GUARD, sizeof buf);
  len = wpe_help_lcpy((char *) buf, 4, "abcdef");
  chk_int("lcpy: written", (long) len, 3);
  chk_str("lcpy: text", (char *) buf, "abc");
  chk_guard("lcpy", buf, 4, sizeof buf);

  memset(buf, GUARD, sizeof buf);
  len = wpe_help_lcpy((char *) buf, 5, "ab" EURO);
  chk_int("lcpy euro: written", (long) len, 2);
  chk_str("lcpy euro: text", (char *) buf, "ab");

  memset(buf, GUARD, sizeof buf);
  wpe_help_lcpy((char *) buf, 8, "Node: ");
  len = wpe_help_lcat((char *) buf, 8, "Topmost");
  chk_int("lcat: length", (long) len, 7);
  chk_str("lcat: text", (char *) buf, "Node: T");
  chk_guard("lcat", buf, 8, sizeof buf);

  memset(buf, GUARD, sizeof buf);
  wpe_help_lcpy((char *) buf, 8, "Node: ");
  len = wpe_help_lcat((char *) buf, 8, "A");
  chk_int("lcat fits: length", (long) len, 7);
  chk_str("lcat fits: text", (char *) buf, "Node: A");

  memset(buf, GUARD, sizeof buf);
  wpe_help_lcpy((char *) buf, 8, "ab");
  len = wpe_help_lcat((char *) buf, 6, "c" EURO EURO);
  chk_int("lcat euro: length", (long) len, 3);
  chk_str("lcat euro: text", (char *) buf, "abc");

  len = wpe_help_lcat((char *) buf, 0, "x");
  chk_int("lcat n=0", (long) len, 0);
}

/* A mark inserted into a line shifts the tail and keeps the terminator. */
static void test_insert_ok(void)
{
  char line[16] = "see x end";
  int r;

  r = wpe_help_line_insert(line, sizeof line, 4, "\x01", 1);
  chk_int("insert: result", r, 0);
  chk_str("insert middle", line, "see \x01x end");
  r = wpe_help_line_insert(line, sizeof line, 0, "\x02", 1);
  chk_int("insert at 0: result", r, 0);
  chk_str("insert at 0", line, "\x02see \x01x end");
  r = wpe_help_line_insert(line, sizeof line, (int) strlen(line), "!", 1);
  chk_int("insert at the end: result", r, 0);
  chk_str("insert at the end", line, "\x02see \x01x end!");
}

/* A full line refuses the insert and stays exactly as it was. */
static void test_insert_refuses(void)
{
  unsigned char buf[12];
  char *line = (char *) buf;
  int r;

  memset(buf, GUARD, sizeof buf);
  strcpy(line, "abcdefg");           /* 7 bytes, room for one more: n = 9 */
  r = wpe_help_line_insert(line, 9, 3, "\x01\x02", 2);
  chk_int("full: refused", r, -1);
  chk_str("full: unchanged", line, "abcdefg");
  r = wpe_help_line_insert(line, 9, 3, "\x01", 1);
  chk_int("last byte of room: inserted", r, 0);
  chk_str("last byte of room", line, "abc\x01" "defg");
  r = wpe_help_line_insert(line, 9, 3, "\x01", 1);
  chk_int("no room left: refused", r, -1);
  chk_str("no room left: unchanged", line, "abc\x01" "defg");
  chk_guard("insert", buf, 9, sizeof buf);
}

/* Odd arguments are refused, not trusted. */
static void test_insert_bad_args(void)
{
  char line[8] = "abc";

  chk_int("at past the end", wpe_help_line_insert(line, sizeof line, 4, "x", 1), -1);
  chk_int("at negative", wpe_help_line_insert(line, sizeof line, -1, "x", 1), -1);
  chk_int("n = 0", wpe_help_line_insert(line, 0, 0, "x", 1), -1);
  chk_int("negative length", wpe_help_line_insert(line, sizeof line, 0, "x", -1), -1);
  chk_str("bad args: unchanged", line, "abc");
  memset(line, 'z', sizeof line);
  chk_int("unterminated line", wpe_help_line_insert(line, sizeof line, 0, "x", 1), -1);
}

int main(void)
{
  test_token_stops();
  test_token_stop_first();
  test_token_source_bound();
  test_token_nul();
  test_token_cut();
  test_token_exact();
  test_token_euro_straddle();
  test_token_lone_bytes();
  test_token_tiny_sizes();
  test_token_dup();
  test_lcpy_lcat();
  test_insert_ok();
  test_insert_refuses();
  test_insert_bad_args();
  if (fails)
  {
    printf("%d check(s) failed\n", fails);
    return 1;
  }
  printf("test_helpbuf: all checks passed\n");
  return 0;
}
