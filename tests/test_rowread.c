/* test_rowread.c -- unit for the readers of a row (we_rowread.h).
 * Always runs; run it under ASan+UBSan too.
 *
 * What the user sees behind each group: Run > Beautify, Matching BracKet, the
 * painters of a window and the syntax colours all walk the rows of a text
 * through these functions.  A walk that steps off the text (the row above
 * the first, the row after the last, the byte before a row, the bytes past a
 * row's NUL) must read "no character", or the editor reads memory the row
 * does not own and the answer depends on what the allocator left there.
 *
 * The rows are built the way the row owner builds them: len+2 bytes from
 * s[0] (the text, a line break, a NUL) and nothing before s[0], in an array
 * of exactly as many rows as the text has.  The reader may touch s[0..len+1]
 * and rows 0..n-1 and nothing else, so under ASan every read outside that is
 * a report.
 *
 * Every row runs twice.  The second run is a hostile frame that makes a read
 * outside the text change the answer in a plain build too, which is the
 * only way a plain `make check` can see it: the bytes before each s[0]
 * alternate '*' and '/' (s[-1] is '\\'), the bytes past each NUL are '{',
 * and the row array has stale slots at [-1] and [n] holding rows of closers
 * with a real length.  A reader that does not test the row index returns a
 * closer; one that does not test the column returns a frame byte.
 */
#include <stdio.h>
#include "linebuf.h"
#include <stdlib.h>
#include <string.h>

#include "../edit.h"
#include "../we_rowread.h"

#define MAX_ROWS     8
#define FRAME_BYTES  16     /* hostile frame: bytes before s[0] and past the NUL */
#define STALE_ROW    "}}}}"  /* hostile frame: the stale slots at [-1] and [n] */
#define LINE_BREAK   '\n'   /* the byte the editor keeps after a row */

struct frame_row {
  const char *text;
};

/* The rows of the text under test: one is empty, one has a multibyte
 * letter, one holds the byte a reader must hand back unchanged. */
static const char *const texts[] = {
  "abc", "", "x{y", "\xc3\xa9t", "  \t"
};
#define NROWS ((int)(sizeof texts / sizeof texts[0]))

static int fails;

static void expect(int ok, const char *what, int hostile, int y, int x)
{
  if (ok)
    return;
  printf("FAIL: %s at row %d column %d (%s frame)\n", what, y, x,
         hostile ? "hostile" : "exact");
  fflush(stdout);   /* a later read may crash the process */
  fails++;
}

static int frame_before(int hostile)
{
  return hostile ? FRAME_BYTES : 0;
}

/* One row of @len text bytes, the row owner's way. */
static void make_row(STRING *row, const char *text, size_t len, int hostile)
{
  int before = frame_before(hostile);
  size_t total = (size_t)before + len + 2 + (hostile ? FRAME_BYTES : 0);
  unsigned char *area = calloc(1, total);
  size_t k;

  for (k = 0; k < (size_t)before; k++)
    area[k] = ((size_t)before - k) % 2 ? '\\' : '/';
  memcpy(area + before, text, len);
  area[before + len] = LINE_BREAK;
  area[before + len + 1] = 0;
  for (k = before + len + 2; k < total; k++)
    area[k] = '{';
  row->s = area + before;
  row->len = (int)len;
  row->nrc = (int)len + 1;
}

static void free_row(STRING *row, int hostile)
{
  free(row->s - frame_before(hostile));
}

/* The buffer: NROWS rows in an array of exactly NROWS entries; in a hostile
 * frame a stale slot of closers at [-1] and at [NROWS] too. */
static void build(BUFFER *b, int hostile)
{
  int slack = hostile ? 1 : 0;
  int i;
  STRING *rows = calloc(NROWS + 2 * slack, sizeof *rows);

  for (i = 0; i < NROWS; i++)
    make_row(&rows[slack + i], texts[i], strlen(texts[i]), hostile);
  if (hostile)
  {
    make_row(&rows[0], STALE_ROW, strlen(STALE_ROW), hostile);
    make_row(&rows[NROWS + 1], STALE_ROW, strlen(STALE_ROW), hostile);
  }
  memset(b, 0, sizeof *b);
  b->bf = rows + slack;
  b->mxlines = NROWS;
}

static void release(BUFFER *b, int hostile)
{
  int i;

  for (i = 0; i < NROWS; i++)
    free_row(&b->bf[i], hostile);
  if (hostile)
  {
    free_row(&b->bf[-1], hostile);
    free_row(&b->bf[NROWS], hostile);
    free(b->bf - 1);
  }
  else
    free(b->bf);
}

/* The rows the text does not have: above the first, below the last, and far
 * out. */
static void check_missing_row(BUFFER *b, int y, int hostile)
{
  int x, cp = 7, cols = 7;

  expect(!wpe_row_has(b, y), "wpe_row_has of a missing row", hostile, y, 0);
  expect(wpe_row_len(b, y) == 0, "wpe_row_len of a missing row", hostile, y, 0);
  for (x = -1; x <= 3; x++)
  {
    expect(wpe_row_byte(b, y, x) == WPE_ROW_NONE,
           "wpe_row_byte of a missing row", hostile, y, x);
    expect(wpe_row_text(b, y, x)[0] == '\0',
           "wpe_row_text of a missing row", hostile, y, x);
  }
  expect(wpe_row_glyph_at(b, y, 0, &cp, &cols) == 1 && cp == 0 && cols == 1,
         "wpe_row_glyph_at of a missing row", hostile, y, 0);
  expect(wpe_row_nrc(b, y) == 0, "wpe_row_nrc of a missing row", hostile, y, 0);
  expect(wpe_row_cstr(b, y) == NULL, "wpe_row_cstr of a missing row", hostile, y, 0);
  expect(wpe_row_find(b, y, "a") == WPE_ROW_NONE,
         "wpe_row_find in a missing row", hostile, y, 0);
}

/* A row the text has, at the columns around its ends. */
static void check_row(BUFFER *b, int y, int hostile)
{
  int len = (int)strlen(texts[y]), x, n = -1;
  const unsigned char *p;

  expect(wpe_row_has(b, y), "wpe_row_has", hostile, y, 0);
  expect(wpe_row_len(b, y) == len, "wpe_row_len", hostile, y, 0);
  for (x = 0; x < len; x++)
    expect(wpe_row_byte(b, y, x) == (unsigned char)texts[y][x],
           "wpe_row_byte inside the text", hostile, y, x);
  expect(wpe_row_byte(b, y, len) == LINE_BREAK, "the line break", hostile, y, len);
  expect(wpe_row_byte(b, y, len + 1) == 0, "the NUL", hostile, y, len + 1);
  expect(wpe_row_byte(b, y, len + 2) == WPE_ROW_NONE,
         "wpe_row_byte past the NUL", hostile, y, len + 2);
  expect(wpe_row_byte(b, y, len + FRAME_BYTES) == WPE_ROW_NONE,
         "wpe_row_byte far past the NUL", hostile, y, len + FRAME_BYTES);
  expect(wpe_row_byte(b, y, -1) == WPE_ROW_NONE,
         "wpe_row_byte before the row", hostile, y, -1);
  expect(wpe_row_byte(b, y, -FRAME_BYTES) == WPE_ROW_NONE,
         "wpe_row_byte far before the row", hostile, y, -FRAME_BYTES);
  expect(wpe_row_text(b, y, 0) == (const char *)b->bf[y].s, "wpe_row_text start",
         hostile, y, 0);
  expect(wpe_row_text(b, y, len + 1)[0] == '\0', "wpe_row_text at the NUL",
         hostile, y, len + 1);
  expect(wpe_row_text(b, y, len + 2)[0] == '\0', "wpe_row_text past the NUL",
         hostile, y, len + 2);
  expect(wpe_row_text(b, y, -1)[0] == '\0', "wpe_row_text before the row",
         hostile, y, -1);
  p = wpe_row_bytes(b, y, &n);
  expect(p == b->bf[y].s && n == len, "wpe_row_bytes", hostile, y, 0);
  expect(wpe_row_bytes(b, y, NULL) == b->bf[y].s, "wpe_row_bytes without a length",
         hostile, y, 0);
  expect(wpe_row_ends_line(b, y), "wpe_row_ends_line", hostile, y, len);
  expect(wpe_row_nrc(b, y) == len + 1, "wpe_row_nrc", hostile, y, len);
  expect(wpe_row_cstr(b, y) == (const char *)b->bf[y].s, "wpe_row_cstr", hostile, y, 0);
  expect(wpe_row_find(b, y, "\x01zz") == WPE_ROW_NONE,
         "wpe_row_find of a text the row lacks", hostile, y, 0);
  if (len > 0)
    expect(wpe_row_find(b, y, texts[y]) == 0, "wpe_row_find of the row's own text",
           hostile, y, 0);
  if (len > 1)
    expect(wpe_row_find(b, y, texts[y] + 1) == 1, "wpe_row_find of its tail",
           hostile, y, 1);
}

/* The glyph reader: a UTF-8 letter is two bytes, a column of the line break
 * is no glyph of the text. */
static void check_glyph(BUFFER *b, int hostile)
{
  int cp = 0, cols = 0, nb;

  nb = wpe_row_glyph_at(b, 3, 0, &cp, &cols);
  expect(nb == 2 && cp == 0xE9 && cols == 1, "wpe_row_glyph_at on a letter",
         hostile, 3, 0);
  nb = wpe_row_glyph_at(b, 3, 3, &cp, &cols);
  expect(nb == 1 && cp == 0, "wpe_row_glyph_at at the line break", hostile, 3, 3);
  nb = wpe_row_glyph_at(b, 3, -1, &cp, &cols);
  expect(nb == 1 && cp == 0, "wpe_row_glyph_at before the row", hostile, 3, -1);
}

/* A row that ends in no line break does not end a line. */
static void check_soft_row(BUFFER *b, int hostile)
{
  b->bf[0].s[b->bf[0].len] = 0;
  expect(!wpe_row_ends_line(b, 0), "wpe_row_ends_line of a soft row", hostile, 0, 3);
  b->bf[0].s[b->bf[0].len] = LINE_BREAK;
}

static void run_frame(int hostile)
{
  BUFFER b;
  int y;

  build(&b, hostile);
  check_missing_row(&b, -1, hostile);
  check_missing_row(&b, NROWS, hostile);
  check_missing_row(&b, NROWS + 100, hostile);
  check_missing_row(&b, -100, hostile);
  for (y = 0; y < NROWS; y++)
    check_row(&b, y, hostile);
  check_glyph(&b, hostile);
  check_soft_row(&b, hostile);
  release(&b, hostile);
}

int main(void)
{
  run_frame(1);
  run_frame(0);
  if (fails)
    return 1;
  printf("test_rowread: readers OK in both frames\n");
  return 0;
}
