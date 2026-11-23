/* test_numtext.c -- unit test for the numbers the editor shows in places of
 * their own (we_numtext.h): the cursor's line and column in a window's frame
 * and the counts of Search > Replace.
 *
 * What the user sees behind each group:
 *  - the line and column are shown in full whatever the size of the file;
 *  - the replace report shows its counts and its reason in full, or the
 *    builder refuses and no box opens; it never writes past the caller's
 *    storage.
 *
 * Every storage is followed by guard bytes of a known value, so a byte
 * written past its size fails here in a plain build, with no sanitizer.
 */
#include <stdio.h>
#include "linebuf.h"
#include <limits.h>
#include <string.h>

#include "../we_numtext.h"

#define GUARD 0x55
#define GUARD_ROOM 64
#define STORE_MAX 256
/* The two sentences the report's third line takes in the editor. */
#define READ_ONLY_SAY "Read-only window: the text is unchanged"
/* The storage the frame had before the builders (the old caller's). */
#define OLD_LINECOL_SIZE 20
#define OLD_REPORT_SIZE 80

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

static char *fresh(unsigned char *mem, size_t n)
{
  memset(mem, GUARD, n + GUARD_ROOM);
  return((char *) mem);
}

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

/* @scenario SC-fmt-20 */
/* #201: counts of 2000000000 with the out-of-memory sentence take 83 bytes;
   80 bytes refuse, and nothing is written past them. */
static void test_report_in_80_bytes(void)
{
  unsigned char mem[STORE_MAX];
  char *b = fresh(mem, OLD_REPORT_SIZE);
  int rc = e_replace_report_text(b, OLD_REPORT_SIZE, 2000000000, 2000000000, E_REPLACE_SAY_NOMEM);

  chk_int("N1 refused", rc, WPE_FMT_NO_ROOM);
  chk_str("N1 empty", b, "");
  chk_guard("N1 guard", mem, OLD_REPORT_SIZE);
}

/* @scenario SC-fmt-21 */
/* #202: line and column of 1000000000 take 21 bytes; 20 refuse. */
static void test_linecol_in_20_bytes(void)
{
  unsigned char mem[STORE_MAX];
  char *b = fresh(mem, OLD_LINECOL_SIZE);
  int rc = e_linecol_text(b, OLD_LINECOL_SIZE, 1000000000, 1000000000);

  chk_int("N2 refused", rc, WPE_FMT_NO_ROOM);
  chk_str("N2 empty", b, "");
  chk_guard("N2 guard", mem, OLD_LINECOL_SIZE);
}

/* @scenario SC-fmt-22 */
/* In the room the editor gives them the biggest counts are shown in full. */
static void test_report_in_its_room(void)
{
  unsigned char mem[STORE_MAX];
  size_t n = E_REPLACE_REPORT_ROOM(sizeof E_REPLACE_SAY_NOMEM + sizeof READ_ONLY_SAY);
  char *b = fresh(mem, n);

  chk_int("N3 whole", e_replace_report_text(b, n, INT_MAX, INT_MAX, E_REPLACE_SAY_NOMEM), WPE_FMT_OK);
  chk_str("N3 text", b, "Found 2147483647\nReplaced 2147483647\n" E_REPLACE_SAY_NOMEM);
  chk_guard("N3 guard", mem, n);
  b = fresh(mem, n);
  chk_int("N3 read-only", e_replace_report_text(b, n, INT_MAX, 0, READ_ONLY_SAY), WPE_FMT_OK);
  chk_str("N3 read-only text", b, "Found 2147483647\nReplaced 0\n" READ_ONLY_SAY);
  b = fresh(mem, n);
  chk_int("N3 no reason", e_replace_report_text(b, n, 3, 3, NULL), WPE_FMT_OK);
  chk_str("N3 no reason text", b, "Found 3\nReplaced 3");
  chk_guard("N3 end guard", mem, n);
}

/* @scenario SC-fmt-23 */
/* The line and column keep the look of the field: five wide, then four. */
static void test_linecol_in_its_room(void)
{
  unsigned char mem[STORE_MAX];
  char *b = fresh(mem, E_LINECOL_ROOM);

  chk_int("N4 whole", e_linecol_text(b, E_LINECOL_ROOM, 100000, 1), WPE_FMT_OK);
  chk_str("N4 text", b, "100000:1   ");
  b = fresh(mem, E_LINECOL_ROOM);
  chk_int("N4 largest", e_linecol_text(b, E_LINECOL_ROOM, INT_MIN, INT_MIN), WPE_FMT_OK);
  chk_str("N4 largest text", b, "-2147483648:-2147483648");
  chk_guard("N4 guard", mem, E_LINECOL_ROOM);
}

int main(void)
{
  test_report_in_80_bytes();
  test_linecol_in_20_bytes();
  test_report_in_its_room();
  test_linecol_in_its_room();
  if (fails)
  {
    printf("%d failures\n", fails);
    return(1);
  }
  printf("all ok\n");
  return(0);
}
