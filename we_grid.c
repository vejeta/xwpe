/* we_grid.c -- the screen grid: its size and the memory behind it.
   See we_grid.h.

   Why one owner.  The size and the four planes used to be set in nine
   places and allocated in a dozen, each one resizing the numbers and the
   memory at a different moment.  A refused allocation then left a size
   without its planes (and a NULL pointer behind a repaint), and a product
   of the two sizes in an int wrapped.  Here the size and the planes are one
   object, a take is all or nothing, and every count is a size_t of a size
   already held under a ceiling.

   Invariant, after the first successful take and until exit:
    1. WPE_GRID_MIN_* <= cols, rows <= WPE_GRID_MAX_*.
    2. The four planes exist, each holds at least cols * rows elements (the
       blocks are of exactly that size, except after a refused shrink, where
       the old, larger blocks are kept), and each was zeroed when it was
       made.  A sanitizer sees a write past a plane on the normal path only.
    3. The size and the planes change together, inside this file.
    4. Every count computed here is a size_t, and every allocation is checked
       before anything is swapped.
    5. A take of a size with no more cells than the grid in use cannot fail.

   The file knows no display and no editor structure. */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "we_grid.h"

/* Every index of a cell is an int in the editor (y * MAXSCOL + x, and
   expressions such as (xe - xa + 1) * 2 * (ye - ya + 1)).  The ceiling keeps
   the largest of them, with a factor of eight to spare, inside an int. */
typedef char wpe_grid_ceiling_fits_int[
 ((size_t)WPE_GRID_MAX_COLS * WPE_GRID_MAX_ROWS * sizeof(SCREENCELL) * 8
  <= (size_t)INT_MAX) ? 1 : -1];

/* The four planes of one grid. */
typedef struct {
 SCREENCELL *cells;
 SCREENCELL *shown;
 char *frames;
 char *frames_shown;
} WpeGridPlanes;

static WpeGrid grid = { WPE_GRID_START_COLS, WPE_GRID_START_ROWS,
                        NULL, NULL, NULL, NULL };
const WpeGrid *const wpe_grid_now = &grid;

static void *wpe_grid_malloc(size_t bytes)
{
 return malloc(bytes);
}

static void wpe_grid_free(void *block)
{
 free(block);
}

static const WpeGridAlloc wpe_grid_default_ops = {
 wpe_grid_malloc, wpe_grid_free, NULL
};
static const WpeGridAlloc *ops = &wpe_grid_default_ops;

/* The size last refused (0 x 0 when none): asking for it again answers
   "kept" without asking the allocator, until another size is asked for. */
static int refused_cols, refused_rows;

/* What the user has not been told yet. */
static WpeGridNotice pending;

/**
 * wpe_grid_use_alloc - Replace the memory the grid is made of.
 * @alloc_ops: the allocator, or NULL for the C library's.
 *
 * For a test that must see every request or refuse one; the running editor
 * uses it for the fault a test arms through the environment.
 */
void wpe_grid_use_alloc(const WpeGridAlloc *alloc_ops)
{
 ops = alloc_ops ? alloc_ops : &wpe_grid_default_ops;
}

/**
 * wpe_grid_fit - Bring a size inside the floor and the ceiling.
 * @cols: the columns, changed in place.
 * @rows: the rows, changed in place.
 *
 * For code that must know the grid before it can take it: a window's
 * requested size, the Wayland frame's buffer.  What the user gets: the
 * editor never works on a grid too small to hold its menu bar, or too large
 * for its own counts.
 */
void wpe_grid_fit(int *cols, int *rows)
{
 if (*cols < WPE_GRID_MIN_COLS) *cols = WPE_GRID_MIN_COLS;
 if (*rows < WPE_GRID_MIN_ROWS) *rows = WPE_GRID_MIN_ROWS;
 if (*cols > WPE_GRID_MAX_COLS) *cols = WPE_GRID_MAX_COLS;
 if (*rows > WPE_GRID_MAX_ROWS) *rows = WPE_GRID_MAX_ROWS;
}

/**
 * wpe_grid_cells - How many cells the grid in use has.
 *
 * The one place a whole-grid count is made, in a size_t.
 */
size_t wpe_grid_cells(void)
{
 return (size_t)grid.cols * (size_t)grid.rows;
}

/**
 * wpe_grid_ready - Whether a grid has been taken yet.
 *
 * Return: 1 once the planes exist (after the first successful take), else 0.
 */
int wpe_grid_ready(void)
{
 return grid.cells != NULL;
}

/* A zeroed block of the given size, or NULL. */
static void *wpe_grid_zeroed(size_t bytes)
{
 void *block = ops->alloc(bytes);

 if (block != NULL)
  memset(block, 0, bytes);
 return block;
}

static void wpe_grid_planes_release(WpeGridPlanes *planes)
{
 if (planes->cells) ops->release(planes->cells);
 if (planes->shown) ops->release(planes->shown);
 if (planes->frames) ops->release(planes->frames);
 if (planes->frames_shown) ops->release(planes->frames_shown);
 memset(planes, 0, sizeof *planes);
}

/**
 * wpe_grid_planes_make - Make the four zeroed planes of @cells cells.
 *
 * Return: 1 when all four exist; 0 when one was refused (and the ones that
 * were made are released again).
 */
static int wpe_grid_planes_make(WpeGridPlanes *planes, size_t cells)
{
 memset(planes, 0, sizeof *planes);
 planes->cells = wpe_grid_zeroed(cells * sizeof(SCREENCELL));
 planes->shown = planes->cells ? wpe_grid_zeroed(cells * sizeof(SCREENCELL)) : NULL;
 planes->frames = planes->shown ? wpe_grid_zeroed(cells) : NULL;
 planes->frames_shown = planes->frames ? wpe_grid_zeroed(cells) : NULL;
 if (planes->frames_shown != NULL)
  return 1;
 wpe_grid_planes_release(planes);
 return 0;
}

/* Put the planes in the grid and release the ones they replace. */
static void wpe_grid_swap_in(WpeGridPlanes *fresh, int cols, int rows)
{
 WpeGridPlanes old;

 old.cells = grid.cells;
 old.shown = grid.shown;
 old.frames = grid.frames;
 old.frames_shown = grid.frames_shown;
 grid.cols = cols;
 grid.rows = rows;
 grid.cells = fresh->cells;
 grid.shown = fresh->shown;
 grid.frames = fresh->frames;
 grid.frames_shown = fresh->frames_shown;
 memset(fresh, 0, sizeof *fresh);
 wpe_grid_planes_release(&old);
}

static int wpe_grid_has(int cols, int rows)
{
 return grid.cells != NULL && grid.cols == cols && grid.rows == rows;
}

static void wpe_grid_set_notice(WpeGridNoticeKind kind, int asked_cols,
                                int asked_rows)
{
 pending.kind = kind;
 pending.asked_cols = asked_cols;
 pending.asked_rows = asked_rows;
 pending.used_cols = grid.cols;
 pending.used_rows = grid.rows;
}

static int wpe_grid_over_ceiling(int cols, int rows)
{
 return cols > WPE_GRID_MAX_COLS || rows > WPE_GRID_MAX_ROWS;
}

/**
 * wpe_grid_note_clamp - Record that a size was cut down before the grid.
 * @asked_cols: @asked_rows: what the user's display or window asked for.
 * @used_cols: @used_rows: the size the editor will use.
 *
 * For a clamp made by a caller that knows what the owner cannot (the
 * terminal that cannot hold the size, the screen smaller than a requested
 * window).  The editor says so once, when it can show a box.
 */
void wpe_grid_note_clamp(int asked_cols, int asked_rows,
                         int used_cols, int used_rows)
{
 pending.kind = WPE_GRID_NOTICE_CLAMPED;
 pending.asked_cols = asked_cols;
 pending.asked_rows = asked_rows;
 pending.used_cols = used_cols;
 pending.used_rows = used_rows;
}

/**
 * wpe_grid_take_notice - Hand out what the user has not been told, once.
 * @notice: receives the notice when there is one.
 *
 * Return: the kind of the notice, WPE_GRID_NOTICE_NONE when nothing waits.
 */
WpeGridNoticeKind wpe_grid_take_notice(WpeGridNotice *notice)
{
 WpeGridNoticeKind kind = pending.kind;

 *notice = pending;
 pending.kind = WPE_GRID_NOTICE_NONE;
 return kind;
}

/* A refused shrink: the old blocks are large enough, so the grid takes the
   smaller size on them and nothing is lost. */
static WpeGridTake wpe_grid_shrink_in_place(int cols, int rows)
{
 size_t cells = (size_t)cols * (size_t)rows;

 grid.cols = cols;
 grid.rows = rows;
 memset(grid.cells, 0, cells * sizeof(SCREENCELL));
 memset(grid.shown, 0, cells * sizeof(SCREENCELL));
 memset(grid.frames, 0, cells);
 memset(grid.frames_shown, 0, cells);
 return WPE_GRID_CHANGED;
}

/* The very first take was refused: start on the usual 80 x 24 so that a
   usable grid always exists. */
static WpeGridTake wpe_grid_first_fallback(int asked_cols, int asked_rows)
{
 WpeGridPlanes fresh;
 int cols = WPE_GRID_START_COLS;
 int rows = WPE_GRID_START_ROWS;

 if (!wpe_grid_planes_make(&fresh, (size_t)cols * (size_t)rows))
  return WPE_GRID_KEPT;
 wpe_grid_swap_in(&fresh, cols, rows);
 wpe_grid_set_notice(WPE_GRID_NOTICE_REFUSED, asked_cols, asked_rows);
 return WPE_GRID_CHANGED;
}

/* A refused take: the way out that loses nothing. */
static WpeGridTake wpe_grid_refused(int cols, int rows,
                                    int asked_cols, int asked_rows)
{
 refused_cols = cols;
 refused_rows = rows;
 if (grid.cells == NULL)
  return wpe_grid_first_fallback(asked_cols, asked_rows);
 if ((size_t)cols * (size_t)rows <= wpe_grid_cells())
  return wpe_grid_shrink_in_place(cols, rows);
 wpe_grid_set_notice(WPE_GRID_NOTICE_REFUSED, asked_cols, asked_rows);
 return WPE_GRID_KEPT;
}

/**
 * wpe_grid_take - Give the grid a size, and the planes that go with it.
 * @cols: @rows: the size asked for; the floor and the ceiling are applied.
 *
 * All or nothing: the new planes are made first, and only when all four
 * exist are the size and the pointers swapped.  What the user gets: a resize
 * the machine has no memory for leaves the editor as it was, with its text,
 * and says so once.  The first take never answers "same": there are no planes
 * before it.
 *
 * Return: WPE_GRID_SAME (nothing touched), WPE_GRID_CHANGED, or
 * WPE_GRID_KEPT (memory refused: the grid in use is unchanged).  Asking again
 * for a size that was just refused answers KEPT without asking for memory,
 * until another size is asked.
 */
WpeGridTake wpe_grid_take(int cols, int rows)
{
 WpeGridPlanes fresh;
 int asked_cols = cols;
 int asked_rows = rows;

 wpe_grid_fit(&cols, &rows);
 if (wpe_grid_has(cols, rows))
 {
  refused_cols = refused_rows = 0;
  return WPE_GRID_SAME;
 }
 if (cols == refused_cols && rows == refused_rows)
  return WPE_GRID_KEPT;
 refused_cols = refused_rows = 0;
 if (ops->begin_take)
  ops->begin_take();
 if (!wpe_grid_planes_make(&fresh, (size_t)cols * (size_t)rows))
  return wpe_grid_refused(cols, rows, asked_cols, asked_rows);
 wpe_grid_swap_in(&fresh, cols, rows);
 if (wpe_grid_over_ceiling(asked_cols, asked_rows))
  wpe_grid_set_notice(WPE_GRID_NOTICE_CLAMPED, asked_cols, asked_rows);
 return WPE_GRID_CHANGED;
}

/**
 * wpe_grid_wipe - Blank the four planes at the size in use.
 *
 * What the repaint of the whole desk needs: every cell starts again as
 * "nothing drawn, nothing shown".  It allocates nothing, so it cannot fail.
 */
void wpe_grid_wipe(void)
{
 size_t cells = wpe_grid_cells();

 if (grid.cells == NULL)
  return;
 memset(grid.cells, 0, cells * sizeof(SCREENCELL));
 memset(grid.shown, 0, cells * sizeof(SCREENCELL));
 memset(grid.frames, 0, cells);
 memset(grid.frames_shown, 0, cells);
}

/**
 * wpe_grid_forget_shown - Forget what the display last showed.
 *
 * The next refresh then sends every cell again.
 */
void wpe_grid_forget_shown(void)
{
 if (grid.shown != NULL)
  memset(grid.shown, 0, wpe_grid_cells() * sizeof(SCREENCELL));
}

/**
 * wpe_grid_forget_shown_frames - Forget the window-frame lines last shown.
 */
void wpe_grid_forget_shown_frames(void)
{
 if (grid.frames_shown != NULL)
  memset(grid.frames_shown, 0, wpe_grid_cells());
}

/* The size of a saved rectangle, bounded: a side of zero or less is no side,
   and a side above the ceiling is cut to it.  Saving and restoring use the
   same function, so both read the saved block with the same stride. */
static void wpe_grid_rect_dims(int xa, int ya, int xe, int ye,
                               size_t *width, size_t *height)
{
 long w = (long)xe - (long)xa + 1;
 long h = (long)ye - (long)ya + 1;

 if (w < 0) w = 0;
 if (h < 0) h = 0;
 if (w > WPE_GRID_MAX_COLS) w = WPE_GRID_MAX_COLS;
 if (h > WPE_GRID_MAX_ROWS) h = WPE_GRID_MAX_ROWS;
 *width = (size_t)w;
 *height = (size_t)h;
}

/* Which columns (or rows) of a rectangle that starts at @start and is
   @length long lie inside a grid side of @side: [*first, *last). */
static void wpe_grid_rect_inside(int start, size_t length, int side,
                                 size_t *first, size_t *last)
{
 long lo = 0;
 long hi = (long)length;

 if ((long)start < 0)
  lo = -(long)start;
 if ((long)start + hi > (long)side)
  hi = (long)side - (long)start;
 if (hi < lo)
  hi = lo;
 *first = (size_t)lo;
 *last = (size_t)hi;
}

/**
 * wpe_grid_save_rect - Copy the cells behind a popup.
 * @xa: @ya: @xe: @ye: the rectangle, corners included; it may reach past any
 * side of the grid.
 *
 * What the user gets: a menu or dialog can put the screen back as it was.
 * The part outside the grid is saved as blank cells and never read.
 *
 * Return: the block (width * height cells), or NULL when memory is refused.
 */
SCREENCELL *wpe_grid_save_rect(int xa, int ya, int xe, int ye)
{
 size_t w, h, cx0, cx1, cy0, cy1, i, j;
 SCREENCELL *saved;

 wpe_grid_rect_dims(xa, ya, xe, ye, &w, &h);
 saved = wpe_grid_zeroed((w * h > 0 ? w * h : 1) * sizeof(SCREENCELL));
 if (saved == NULL || grid.cells == NULL)
  return saved;
 wpe_grid_rect_inside(xa, w, grid.cols, &cx0, &cx1);
 wpe_grid_rect_inside(ya, h, grid.rows, &cy0, &cy1);
 for (j = cy0; j < cy1; j++)
  for (i = cx0; i < cx1; i++)
   saved[j * w + i] = grid.cells[((size_t)ya + j) * (size_t)grid.cols
                                 + (size_t)xa + i];
 return saved;
}

/**
 * wpe_grid_restore_rect - Put back the cells wpe_grid_save_rect saved.
 * @saved: the block it returned.
 * @xa: @ya: @xe: @ye: the same rectangle.
 *
 * Only the part that lies inside the grid now is written, on every side.
 */
void wpe_grid_restore_rect(const SCREENCELL *saved,
                           int xa, int ya, int xe, int ye)
{
 size_t w, h, cx0, cx1, cy0, cy1, i, j;

 if (saved == NULL || grid.cells == NULL)
  return;
 wpe_grid_rect_dims(xa, ya, xe, ye, &w, &h);
 wpe_grid_rect_inside(xa, w, grid.cols, &cx0, &cx1);
 wpe_grid_rect_inside(ya, h, grid.rows, &cy0, &cy1);
 for (j = cy0; j < cy1; j++)
  for (i = cx0; i < cx1; i++)
   grid.cells[((size_t)ya + j) * (size_t)grid.cols + (size_t)xa + i]
    = saved[j * w + i];
}
