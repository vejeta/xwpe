/* we_layout.h -- boxes have one layout path.
   Copyright (C) 2026 Juan Manuel Méndez Rey
   This is free software; see the file COPYING.

   A box (a dialog, a dropdown, an About box, a picker) says where it wants
   to be as a rule, not as coordinates: centred, below a menu entry, beside
   a text position.  One function turns the rule into a place for the
   screen as it is now.  The owner keeps the stack of open boxes, on the
   screen owner's depth (we_paint.h), and when a display has taken a new
   size it lays every box out again, outermost first, at one point.  A
   background a box saved before the new size is never put back.

   The owner is plain C: it knows the editor only through the hooks of
   WpeLayoutDesk, so it can be tested on its own (tests/test_layout.c). */

#ifndef WE_LAYOUT_H
#define WE_LAYOUT_H

/* A rectangle of screen cells, both ends included.  The field names are
   used by no other structure in the editor, so the fitness check can find
   a box place computed by hand. */
typedef struct
{
 int bx_left, bx_top, bx_right, bx_bottom;
} WpeBoxRect;

typedef enum
{
 WPE_BOX_CENTRED_SCREEN,  /* centred in rows 1..slns-2 (About, System Info) */
 WPE_BOX_TOP_CENTRED,     /* centred across, from row y_hint down (pickers) */
 WPE_BOX_CENTRED_WINDOW,  /* a dialog: centred on the window in `within`, or on the screen for a pane */
 WPE_BOX_FROM_BAR,        /* a dropdown: below the bar at column x_hint, moved left rather than past the edge */
 WPE_BOX_AT_TEXT,         /* a popup beside the text cell (x_hint, y_hint): below, else above */
 WPE_BOX_HINTED,          /* the caller's corner (x_hint, y_hint), moved in so it fits */
 WPE_BOX_FULL_SCREEN,     /* the screen handed to a command */
 WPE_BOX_BAR_ENTRY        /* the menu bar's highlight: row 0 from column x_hint */
} WpeBoxAnchor;

typedef struct
{
 WpeBoxAnchor anchor;
 int w, h;                /* the frame included */
 int x_hint, y_hint;
 int has_within;          /* `within` is a window the rule must stay in */
 int within_is_pane;      /* a docked tool pane: CENTRED_WINDOW uses the screen instead */
 WpeBoxRect within;
} WpeBoxPlace;

/* The rows a box may use: under the bar row, above the status row. */
#define WPE_BOX_FIRST_ROW  1
#define WPE_BOX_STATUS_ROWS 1
/* The smallest a box can be drawn: a frame with one cell inside. */
#define WPE_BOX_MIN_W 3
#define WPE_BOX_MIN_H 3

void wpe_box_place(const WpeBoxPlace *place, int scol, int slns,
                   WpeBoxRect *at, int *fits);

typedef enum
{
 WPE_BOX_DRAWN,
 WPE_BOX_NO_MEMORY,       /* reported once the walk is done; a draw never opens a box */
 WPE_BOX_GONE             /* the box no longer fits: popped, its loop returns */
} WpeBoxDrawn;

struct WpeBox;
typedef WpeBoxDrawn (*WpeBoxDraw)(struct WpeBox *box);

typedef struct WpeBox
{
 const char *what;        /* for the trace: "the About box" */
 WpeBoxPlace place;
 WpeBoxDraw draw;         /* draws the box at box->at; the frame goes through wpe_box_frame */
 void *self;              /* the box's own state */
 WpeBoxRect at;           /* where the owner put it; a loop reads it on every paint */
 int fits;                /* 0 when the owner had to clip below the rule's w x h */
 void *pic;               /* the background the box saved (PIC *); the owner drops it on a resize */
 int moved;               /* set by the walk; wpe_box_getch clears it: the loop refocuses */
 int open;               /* 0 closed, 1 open, 2 gone: the walk found no room, the loop still has to close it */
 int slot;                /* its place in the owner's stack */
} WpeBox;

/* What the owner needs of the editor, given once at start. */
typedef struct
{
 void (*screen)(int *scol, int *slns);                 /* the screen as it is now */
 void (*relayout_windows)(int old_scol, int old_slns); /* the windows' owner */
 void (*forget_box_view)(void);                        /* a graphical display's keep-view */
 void (*repaint_desk)(void);                           /* the display's repaint at the new size */
 void (*drop_pic)(void *pic);                          /* free a saved background, restoring nothing */
 void (*say_no_memory)(const char *what);
} WpeLayoutDesk;

void wpe_layout_desk_set(const WpeLayoutDesk *desk);
/* The editor's side (we_layout_desk.c; e_layout_desk_ready is in edit.h). */
int  wpe_box_frame(WpeBox *box, char *title, int fr, int ft, int fes);

WpeBoxDrawn wpe_box_open(WpeBox *box);
void        wpe_box_close(WpeBox *box);
int         wpe_box_getch(WpeBox *box);
int         wpe_box_depth(void);

/* A display has taken a new grid: stamp every view saved from now as new,
   and remember the size the windows must scale from.  Cheap and safe from
   anywhere; the walk follows at the key reader. */
void wpe_layout_note_resize(int old_scol, int old_slns);
int  wpe_layout_resize_pending(void);
/* The walk, once, at the top of a key reader. */
void wpe_layout_screen_resized(int old_scol, int old_slns);
/* The walk when one is pending; non-zero when it ran. */
int  wpe_layout_run_pending(void);
unsigned wpe_layout_generation(void);

#endif /* WE_LAYOUT_H */
