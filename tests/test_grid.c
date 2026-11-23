/* test_grid.c -- unit test for the owner of the screen grid (we_grid.c).
 * Always runs; run it under ASan+UBSan+LSan too.
 *
 * What the user sees behind each group:
 *  - a terminal or window larger than the editor holds starts on the
 *    largest grid it keeps, and is told so once (U-fit, U-count, U-clamp);
 *  - every cell starts blank: nothing of an earlier screen shows (U-zero);
 *  - a resize the machine has no memory for leaves the editor as it was,
 *    with its text, and says so once (U-refuse-grow, U-refuse-repeat);
 *  - a smaller screen is always taken (U-refuse-shrink), and the first
 *    screen of a run is always a usable one (U-first-same, U-refuse-first);
 *  - a menu or dialog saves and restores the cells behind it, also when it
 *    reaches past any side of the screen (U-rect).
 *
 * The owner is given a recording allocator that fills what it hands out with
 * a pattern, counts the blocks alive and can refuse a request.  The rows
 * that need a grid that has never been taken run in a child process.
 */
#include <stdio.h>
#include "linebuf.h"
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../we_grid.h"

#define PATTERN 0xA5
#define MAX_REQUESTS 64
/* No machine hands out a block this large, so the recorder refuses one: a
   row that lets a size above the ceiling through would otherwise wait for
   memory that does not exist instead of failing.  The largest block a row of
   this unit asks for is the ceiling's cell plane (201 MB). */
#define MAX_BLOCK (1024 * 1024 * 1024)

static int fails;

static void chk_int(const char *what, long got, long want)
{
  if (got != want)
  {
    printf("FAIL: %s: got %ld want %ld\n", what, got, want);
    fails++;
  }
}

/* --- the recording allocator ------------------------------------------ */

static int requests;            /* requests since arm */
static size_t asked[MAX_REQUESTS];
static int refuse_at;           /* the request to refuse (1-based), 0 none */
static int refuse_all;          /* refuse every request */
static long blocks_alive;
static int takes_begun;

static void *rec_alloc(size_t bytes)
{
  void *block;

  requests++;
  if (requests <= MAX_REQUESTS)
    asked[requests - 1] = bytes;
  if (refuse_all || requests == refuse_at || bytes > MAX_BLOCK)
    return NULL;
  block = malloc(bytes);
  if (block == NULL)
    return NULL;
  memset(block, PATTERN, bytes);
  blocks_alive++;
  return block;
}

static void rec_release(void *block)
{
  blocks_alive--;
  free(block);
}

static void rec_begin(void)
{
  takes_begun++;
}

static const WpeGridAlloc recorder = { rec_alloc, rec_release, rec_begin };

static void arm(int refuse_nth, int every)
{
  requests = 0;
  takes_begun = 0;
  refuse_at = refuse_nth;
  refuse_all = every;
  memset(asked, 0, sizeof asked);
}

/* --- what a plane holds ------------------------------------------------ */

static int plane_is_zero(const void *plane, size_t bytes)
{
  const unsigned char *p = plane;
  size_t i;

  for (i = 0; i < bytes; i++)
    if (p[i] != 0)
      return 0;
  return 1;
}

static int planes_are_zero(void)
{
  size_t cells = wpe_grid_cells();

  return plane_is_zero(schirm, cells * sizeof(SCREENCELL)) &&
         plane_is_zero(altschirm, cells * sizeof(SCREENCELL)) &&
         plane_is_zero(extbyte, cells) &&
         plane_is_zero(altextbyte, cells);
}

static void mark_cells(void)
{
  size_t i;

  for (i = 0; i < wpe_grid_cells(); i++)
  {
    schirm[i].ch = (int)(i + 1);
    altschirm[i].ch = (int)(i + 1);
    extbyte[i] = 1;
    altextbyte[i] = 1;
  }
}

/* --- rows run in the main process -------------------------------------- */

/* U-fit: the floor and the ceiling, on each side, for odd input too. */
static void row_fit(void)
{
  static const int cases[][4] = {
    { 0, 0, WPE_GRID_MIN_COLS, WPE_GRID_MIN_ROWS },
    { -5, -9, WPE_GRID_MIN_COLS, WPE_GRID_MIN_ROWS },
    { 10, 3, WPE_GRID_MIN_COLS, WPE_GRID_MIN_ROWS },
    { 80, 24, 80, 24 },
    { 30, 6, 30, 6 },
    { 4096, 4096, 4096, 4096 },
    { 4097, 4097, 4096, 4096 },
    { 65535, 65535, 4096, 4096 },
    { 5000, 10, 4096, 10 },
    { 100, 70000, 100, 4096 }
  };
  size_t i;

  for (i = 0; i < sizeof cases / sizeof cases[0]; i++)
  {
    int cols = cases[i][0], rows = cases[i][1];

    wpe_grid_fit(&cols, &rows);
    chk_int("U-fit columns", cols, cases[i][2]);
    chk_int("U-fit rows", rows, cases[i][3]);
  }
}

/* U-count: a size of 65535 x 65535 is held at the ceiling, and the
   allocator is never asked for more than that, nor for a count that is not
   cols * rows of the fitted size. */
static void row_count(void)
{
  size_t cells = (size_t)WPE_GRID_MAX_COLS * WPE_GRID_MAX_ROWS;
  int i;

  arm(0, 0);
  chk_int("U-count take", wpe_grid_take(65535, 65535), WPE_GRID_CHANGED);
  chk_int("U-count columns", MAXSCOL, WPE_GRID_MAX_COLS);
  chk_int("U-count rows", MAXSLNS, WPE_GRID_MAX_ROWS);
  chk_int("U-count requests", requests, 4);
  for (i = 0; i < requests && i < 4; i++)
  {
    size_t want = i < 2 ? cells * sizeof(SCREENCELL) : cells;

    if (asked[i] != want)
    {
      printf("FAIL: U-count request %d asked %zu bytes, want %zu\n", i + 1,
             asked[i], want);
      fails++;
    }
  }
}

/* U-clamp: a size above the ceiling is noted once, with what was asked. */
static void row_clamp_notice(void)
{
  WpeGridNotice notice;

  wpe_grid_take_notice(&notice);            /* drop what U-count left */
  arm(0, 0);
  chk_int("U-clamp take", wpe_grid_take(5000, 10), WPE_GRID_CHANGED);
  chk_int("U-clamp kind", wpe_grid_take_notice(&notice), WPE_GRID_NOTICE_CLAMPED);
  chk_int("U-clamp asked columns", notice.asked_cols, 5000);
  chk_int("U-clamp asked rows", notice.asked_rows, 10);
  chk_int("U-clamp used columns", notice.used_cols, 4096);
  chk_int("U-clamp used rows", notice.used_rows, 10);
  chk_int("U-clamp once", wpe_grid_take_notice(&notice), WPE_GRID_NOTICE_NONE);
}

/* U-zero: every plane is zero after a take and after a wipe, although the
   allocator hands out a pattern. */
static void row_zero(void)
{
  arm(0, 0);
  chk_int("U-zero take", wpe_grid_take(100, 40), WPE_GRID_CHANGED);
  chk_int("U-zero planes after a take", planes_are_zero(), 1);
  mark_cells();
  wpe_grid_wipe();
  chk_int("U-zero planes after a wipe", planes_are_zero(), 1);
  mark_cells();
  wpe_grid_forget_shown();
  chk_int("U-zero shown forgotten", plane_is_zero(altschirm,
          wpe_grid_cells() * sizeof(SCREENCELL)), 1);
  chk_int("U-zero drawn kept", schirm[3].ch, 4);
  wpe_grid_forget_shown_frames();
  chk_int("U-zero shown frames forgotten", plane_is_zero(altextbyte,
          wpe_grid_cells()), 1);
  chk_int("U-zero frames kept", extbyte[3], 1);
}

/* U-same: a take of the size the grid has asks for nothing. */
static void row_same(void)
{
  arm(0, 0);
  chk_int("U-same take", wpe_grid_take(100, 40), WPE_GRID_SAME);
  chk_int("U-same requests", requests, 0);
  chk_int("U-same takes begun", takes_begun, 0);
}

/* U-refuse-grow: each of the four requests refused in turn leaves the size,
   the four pointers and the text as they were, leaks nothing, and says so. */
static void row_refuse_grow(void)
{
  int nth;

  for (nth = 1; nth <= 4; nth++)
  {
    SCREENCELL *cells, *shown;
    char *frames, *frames_shown;
    long alive = blocks_alive;
    WpeGridNotice notice;

    schirm[5].ch = 777;
    cells = schirm;
    shown = altschirm;
    frames = extbyte;
    frames_shown = altextbyte;
    arm(nth, 0);
    chk_int("U-refuse-grow take", wpe_grid_take(200 + nth, 80), WPE_GRID_KEPT);
    chk_int("U-refuse-grow columns", MAXSCOL, 100);
    chk_int("U-refuse-grow rows", MAXSLNS, 40);
    chk_int("U-refuse-grow cells pointer", schirm == cells, 1);
    chk_int("U-refuse-grow shown pointer", altschirm == shown, 1);
    chk_int("U-refuse-grow frames pointer", extbyte == frames, 1);
    chk_int("U-refuse-grow shown frames pointer", altextbyte == frames_shown, 1);
    chk_int("U-refuse-grow text kept", schirm[5].ch, 777);
    chk_int("U-refuse-grow leaks", blocks_alive, alive);
    chk_int("U-refuse-grow kind", wpe_grid_take_notice(&notice),
            WPE_GRID_NOTICE_REFUSED);
    chk_int("U-refuse-grow asked", notice.asked_cols, 200 + nth);
    chk_int("U-refuse-grow used", notice.used_cols, 100);
  }
}

/* U-refuse-repeat: the size just refused is not asked for again, and not
   announced again, until another size is asked for. */
static void row_refuse_repeat(void)
{
  WpeGridNotice notice;

  arm(1, 0);
  chk_int("U-refuse-repeat first", wpe_grid_take(300, 90), WPE_GRID_KEPT);
  wpe_grid_take_notice(&notice);
  arm(0, 0);
  chk_int("U-refuse-repeat again", wpe_grid_take(300, 90), WPE_GRID_KEPT);
  chk_int("U-refuse-repeat requests", requests, 0);
  chk_int("U-refuse-repeat notice", wpe_grid_take_notice(&notice),
          WPE_GRID_NOTICE_NONE);
  chk_int("U-refuse-repeat another size", wpe_grid_take(301, 90),
          WPE_GRID_CHANGED);
  chk_int("U-refuse-repeat the size is taken", MAXSCOL, 301);
  chk_int("U-refuse-repeat the old size can be asked", wpe_grid_take(300, 90),
          WPE_GRID_CHANGED);
}

/* U-refuse-shrink: a refused shrink still changes the size, on the old
   blocks, and zeroes the part in use. */
static void row_refuse_shrink(void)
{
  SCREENCELL *cells = schirm;
  WpeGridNotice notice;

  wpe_grid_take_notice(&notice);
  mark_cells();
  arm(1, 0);
  chk_int("U-refuse-shrink take", wpe_grid_take(100, 40), WPE_GRID_CHANGED);
  chk_int("U-refuse-shrink columns", MAXSCOL, 100);
  chk_int("U-refuse-shrink rows", MAXSLNS, 40);
  chk_int("U-refuse-shrink reuses the blocks", schirm == cells, 1);
  chk_int("U-refuse-shrink zero", planes_are_zero(), 1);
  chk_int("U-refuse-shrink says nothing", wpe_grid_take_notice(&notice),
          WPE_GRID_NOTICE_NONE);
}

/* U-rect: a saved rectangle that reaches past every side, and one that is
   turned round, touch nothing outside the grid or the saved block. */
static void rect_round_trip(int xa, int ya, int xe, int ye)
{
  size_t i, cells = wpe_grid_cells();
  SCREENCELL *copy = malloc(cells * sizeof(SCREENCELL));
  SCREENCELL *saved;

  memcpy(copy, schirm, cells * sizeof(SCREENCELL));
  saved = wpe_grid_save_rect(xa, ya, xe, ye);
  if (saved == NULL)
  {
    printf("FAIL: U-rect save %d,%d-%d,%d gave no block\n", xa, ya, xe, ye);
    fails++;
    free(copy);
    return;
  }
  for (i = 0; i < cells; i++)
    schirm[i].ch = -1;
  wpe_grid_restore_rect(saved, xa, ya, xe, ye);
  for (i = 0; i < cells; i++)
  {
    int x = (int)(i % (size_t)MAXSCOL), y = (int)(i / (size_t)MAXSCOL);
    int inside = x >= xa && x <= xe && y >= ya && y <= ye;
    int want = inside ? copy[i].ch : -1;

    if (schirm[i].ch != want)
    {
      printf("FAIL: U-rect %d,%d-%d,%d cell %d,%d: got %d want %d\n",
             xa, ya, xe, ye, x, y, schirm[i].ch, want);
      fails++;
      break;
    }
  }
  memcpy(schirm, copy, cells * sizeof(SCREENCELL));
  rec_release(saved);
  free(copy);
}

static void row_rect(void)
{
  arm(0, 0);
  chk_int("U-rect grid", wpe_grid_take(100, 40) != WPE_GRID_KEPT, 1);
  mark_cells();
  blocks_alive = 0;
  rect_round_trip(10, 5, 20, 9);       /* inside */
  rect_round_trip(-4, 5, 6, 9);        /* past the left */
  rect_round_trip(95, 5, 110, 9);      /* past the right */
  rect_round_trip(10, -3, 20, 2);      /* above */
  rect_round_trip(10, 36, 20, 50);     /* below */
  rect_round_trip(-9, -9, 200, 200);   /* past every side */
  rect_round_trip(-30, 5, -10, 9);     /* wholly left */
  rect_round_trip(300, 5, 310, 9);     /* wholly right */
  rect_round_trip(10, -30, 20, -10);   /* wholly above */
  rect_round_trip(10, 90, 20, 99);     /* wholly below */
  rect_round_trip(20, 9, 10, 5);       /* turned round: no cell */
  chk_int("U-rect blocks released", blocks_alive, 0);
  chk_int("U-rect restore of nothing", (wpe_grid_restore_rect(NULL, 0, 0, 5, 5), 1), 1);
}

/* --- rows that need a grid that was never taken (child processes) ------ */

/* U-first-same: the first take of 80 x 24, the size the object starts with,
   is a change: there are no planes before it. */
static void child_first_same(void)
{
  arm(0, 0);
  chk_int("U-first-same take", wpe_grid_take(80, 24), WPE_GRID_CHANGED);
  chk_int("U-first-same cells", schirm != NULL, 1);
  chk_int("U-first-same shown", altschirm != NULL, 1);
  chk_int("U-first-same frames", extbyte != NULL, 1);
  chk_int("U-first-same shown frames", altextbyte != NULL, 1);
  chk_int("U-first-same requests", requests, 4);
  chk_int("U-first-same then same", wpe_grid_take(80, 24), WPE_GRID_SAME);
}

/* U-refuse-first: a first take that is refused starts on 80 x 24 and notes
   the size that was refused, once. */
static void child_refuse_first(void)
{
  WpeGridNotice notice;

  arm(1, 0);
  chk_int("U-refuse-first take", wpe_grid_take(300, 100), WPE_GRID_CHANGED);
  chk_int("U-refuse-first columns", MAXSCOL, WPE_GRID_START_COLS);
  chk_int("U-refuse-first rows", MAXSLNS, WPE_GRID_START_ROWS);
  chk_int("U-refuse-first planes", schirm != NULL && altschirm != NULL &&
          extbyte != NULL && altextbyte != NULL, 1);
  chk_int("U-refuse-first kind", wpe_grid_take_notice(&notice),
          WPE_GRID_NOTICE_REFUSED);
  chk_int("U-refuse-first asked columns", notice.asked_cols, 300);
  chk_int("U-refuse-first asked rows", notice.asked_rows, 100);
  chk_int("U-refuse-first once", wpe_grid_take_notice(&notice),
          WPE_GRID_NOTICE_NONE);
}

/* U-refuse-none: no memory even for 80 x 24 leaves no planes and says KEPT:
   the caller ends the editor with a sentence. */
static void child_refuse_none(void)
{
  arm(0, 1);
  chk_int("U-refuse-none take", wpe_grid_take(300, 100), WPE_GRID_KEPT);
  chk_int("U-refuse-none no planes", schirm == NULL && altschirm == NULL, 1);
}

static void run_in_child(const char *name, void (*row)(void))
{
  pid_t pid;
  int status;

  fflush(stdout);
  pid = fork();
  if (pid == 0)
  {
    wpe_grid_use_alloc(&recorder);
    row();
    fflush(stdout);
    _exit(fails ? 1 : 0);
  }
  if (pid < 0 || waitpid(pid, &status, 0) != pid ||
      !WIFEXITED(status) || WEXITSTATUS(status) != 0)
  {
    printf("FAIL: %s (child did not finish cleanly)\n", name);
    fails++;
  }
}

int main(void)
{
  run_in_child("U-first-same", child_first_same);
  run_in_child("U-refuse-first", child_refuse_first);
  run_in_child("U-refuse-none", child_refuse_none);
  wpe_grid_use_alloc(&recorder);
  row_fit();
  row_count();
  row_clamp_notice();
  row_zero();
  row_same();
  row_refuse_grow();
  row_refuse_repeat();
  row_refuse_shrink();
  row_rect();
  if (fails)
  {
    printf("test_grid: %d failure(s)\n", fails);
    return 1;
  }
  printf("test_grid: ok\n");
  return 0;
}
