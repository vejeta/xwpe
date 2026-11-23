/* we_grid.h -- the screen grid: its size and the memory behind it, one owner.

   The editor draws into a grid of MAXSCOL x MAXSLNS cells and keeps four
   planes for it: what it drew, what the display last showed, and the same
   two for the window-frame lines.  The size and the planes are one object.
   They change together, in one step, inside we_grid.c: the new planes are
   made first, and only when all of them exist are the size and the pointers
   swapped.  A refused allocation leaves the old grid exactly as it was.

   The six names the rest of the editor reads (MAXSCOL, MAXSLNS, schirm,
   altschirm, extbyte, altextbyte) are read-only views of that object: an
   assignment to one of them anywhere else does not compile.

   This file and we_grid.c know no display and no editor structure, so a unit
   test links them alone. */

#ifndef WE_GRID_H
#define WE_GRID_H

#include <stddef.h>

/* One screen cell: one visual column on screen. */
typedef struct {
 int ch;    /* character (wchar_t or special char code) */
 int attr;  /* color/attribute byte (xwpe format) */
 int flags; /* CELL_WIDE, CELL_WIDE_SPACER */
} SCREENCELL;

#define CELL_WIDE        0x01  /* first cell of a wide character (wcwidth=2) */
#define CELL_WIDE_SPACER 0x02  /* second cell, continuation of wide char */

/* The smallest grid the editor works on (what the terminal and Wayland always
   had), the largest one it keeps (every index is an int: see we_grid.c), and
   the grid it starts on when it knows nothing better. */
#define WPE_GRID_MIN_COLS   30
#define WPE_GRID_MIN_ROWS   6
#define WPE_GRID_MAX_COLS   4096
#define WPE_GRID_MAX_ROWS   4096
#define WPE_GRID_START_COLS 80
#define WPE_GRID_START_ROWS 24

/* The grid in use.  Only we_grid.c writes it. */
typedef struct {
 int cols, rows;            /* the grid in use */
 SCREENCELL *cells;         /* what the editor drew            (schirm)     */
 SCREENCELL *shown;         /* what the display last showed    (altschirm)  */
 char *frames;              /* frame lines per cell            (extbyte)    */
 char *frames_shown;        /* frame lines last shown          (altextbyte) */
} WpeGrid;

extern const WpeGrid *const wpe_grid_now;

/* "+ 0" makes each name a value: MAXSCOL = n, MAXSCOL++, &MAXSCOL and
   schirm = p do not compile.  schirm[i].ch = c and &schirm[i] still do. */
#define MAXSCOL    (wpe_grid_now->cols + 0)
#define MAXSLNS    (wpe_grid_now->rows + 0)
#define schirm     (wpe_grid_now->cells + 0)
#define altschirm  (wpe_grid_now->shown + 0)
#define extbyte    (wpe_grid_now->frames + 0)
#define altextbyte (wpe_grid_now->frames_shown + 0)

/* What a take of a size did. */
typedef enum {
 WPE_GRID_SAME,     /* the grid already had this size and its planes */
 WPE_GRID_CHANGED,  /* the grid has the size (or the part of it that fits) */
 WPE_GRID_KEPT      /* no memory: the grid in use is what it was */
} WpeGridTake;

/* A fact the user should be told once. */
typedef enum {
 WPE_GRID_NOTICE_NONE,
 WPE_GRID_NOTICE_CLAMPED,   /* a size above the ceiling or the display's */
 WPE_GRID_NOTICE_REFUSED    /* the memory for a size was refused */
} WpeGridNoticeKind;

typedef struct {
 WpeGridNoticeKind kind;
 int asked_cols, asked_rows;   /* what was asked for */
 int used_cols, used_rows;     /* what the grid in use is */
} WpeGridNotice;

/* The memory the owner uses: replaceable by a unit test, and by the editor's
   fault seam.  begin_take, when set, is called at the start of every take
   that is going to allocate. */
typedef struct {
 void *(*alloc)(size_t bytes);
 void (*release)(void *block);
 void (*begin_take)(void);
} WpeGridAlloc;

void        wpe_grid_fit(int *cols, int *rows);
WpeGridTake wpe_grid_take(int cols, int rows);
size_t      wpe_grid_cells(void);
int         wpe_grid_ready(void);
void        wpe_grid_wipe(void);
void        wpe_grid_forget_shown(void);
void        wpe_grid_forget_shown_frames(void);
SCREENCELL *wpe_grid_save_rect(int xa, int ya, int xe, int ye);
void        wpe_grid_restore_rect(const SCREENCELL *saved,
                                  int xa, int ya, int xe, int ye);
void        wpe_grid_note_clamp(int asked_cols, int asked_rows,
                                int used_cols, int used_rows);
WpeGridNoticeKind wpe_grid_take_notice(WpeGridNotice *notice);
void        wpe_grid_use_alloc(const WpeGridAlloc *ops);

#endif
