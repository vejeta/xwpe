/* we_wind.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include "we_buffer.h"
#include "we_testdump.h"
#include "we_paint.h"
#include "we_layout.h"
#include "we_startcmd.h"
#include "we_startplan.h"
#include <stdarg.h>
#include <wchar.h>
#ifdef WPE_AI
#include "we_ai.h"
#endif

#define MAXSVSTR 20

/* Place one decoded character into the screen-cell grid at (sx,sy) with
   attribute frb.  A wide (2-column) glyph also marks its own cell CELL_WIDE,
   and -- when has_room is true -- writes a space in the next cell tagged
   CELL_WIDE_SPACER, returning the extra column (cw-1) the caller's column
   counter must advance.  Returns 0 for a normal-width glyph.  This owns the
   wide-cell lead/spacer invariant for both editor and project render loops. */
int e_put_wide_cell(int sx, int sy, int wc, int cw, int frb, int has_room)
{
 e_pr_char(sx, sy, wc, frb);
 if (cw > 1 && has_room)
 {
  e_pt_flags(sx, sy, CELL_WIDE);
  e_pr_char(sx + 1, sy, ' ', frb);
  e_pt_flags(sx + 1, sy, CELL_WIDE_SPACER);
  return cw - 1;
 }
 return 0;
}

int e_make_xr_rahmen(int xa, int ya, int xe, int ye, int sw);
static void e_draw_titlebar_buttons(int xa, int ya, int xe, int frb, int fes);
static void e_draw_window_buttons(FENSTER *f);
static void e_draw_dialog_close_button(FENSTER *f);
static void e_clear_titlebar_buttons(FENSTER *f);
extern void e_lsp_bar_label(FENSTER *f);   /* name the LSP bar after the server (we_debug.c) */

/* >=0: override colour for the title TEXT only (not the border) on the next
   e_std_rahmen call -- used to draw a read-only window's name dimmed.  Reset to
   -1 by the caller right after. */
static int g_rahmen_hdr_frb = -1;

static int e_scale_y(int val, int old_slns, int new_slns)
{
 int old_range = old_slns - 2;
 int new_range = new_slns - 2;
 if (old_range <= 0) return val;
 return 1 + ((val - 1) * new_range + old_range / 2) / old_range;
}

void e_free_all_pics(ECNT *cn)
{
 int i;
 for (i = 0; i <= cn->mxedt; i++)
 {
  FENSTER *fw = cn->f[i];
  if (fw->pic && fw->pic->buf)
  { free((SCREENCELL *)fw->pic->buf); fw->pic->buf = NULL; }
  if (fw->pic)
  { FREE(fw->pic); fw->pic = NULL; }
 }
}

void e_repaint_desk_nopic(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i, sw;
 if (cn->mxedt < 1)
 {
  e_cls(f->fb->df.fb, f->fb->dc);
  e_ini_desk(f->ed);
  e_refresh();
  return;
 }
 ini_repaint(cn);
 e_abs_refr();
 for (i = 1; i <= cn->mxedt; i++)
 {
  sw = (i == cn->mxedt) ? 1 : 0;
  e_ed_rahmen(cn->f[i], sw);
  e_schirm(cn->f[i], sw);
 }
 e_refresh();
}

void e_relayout_windows(ECNT *cn, int old_scol, int old_slns)
{
 int i;
 /* Snapshot original a.y/e.y so the overlap-resolution pass below can
    distinguish "newly overlapping after min-height expansion" (needs
    pushing) from "intentionally cascaded by the user" (must be left
    alone). */
 int orig_ay[MAXEDT + 1], orig_ey[MAXEDT + 1];
 for (i = 0; i <= cn->mxedt; i++)
 {
  orig_ay[i] = cn->f[i]->a.y;
  orig_ey[i] = cn->f[i]->e.y;
 }
 for (i = 0; i <= cn->mxedt; i++)
 {
  FENSTER *fw = cn->f[i];
  int at_left   = (fw->a.x <= 0);
  int at_top    = (fw->a.y <= 1);
  int at_right  = (fw->e.x >= old_scol - 1);
  int at_bottom = (fw->e.y >= old_slns - 2);

  fw->a.x = at_left   ? 0          : fw->a.x * MAXSCOL / old_scol;
  fw->e.x = at_right  ? MAXSCOL - 1 : fw->e.x * MAXSCOL / old_scol;
  fw->a.y = at_top    ? 1           : e_scale_y(fw->a.y, old_slns, MAXSLNS);
  fw->e.y = at_bottom ? MAXSLNS - 2 : e_scale_y(fw->e.y, old_slns, MAXSLNS);

  if (fw->e.x >= MAXSCOL) fw->e.x = MAXSCOL - 1;
  if (fw->e.y >= MAXSLNS - 1) fw->e.y = MAXSLNS - 2;
  if (fw->a.y < 1) fw->a.y = 1;
  if (fw->a.x < 0) fw->a.x = 0;
  if (fw->e.y - fw->a.y < 3)
  {
   if (at_top)
    fw->e.y = fw->a.y + 3;
   else
   {
    fw->a.y = fw->e.y - 3;
    if (!at_top && fw->a.y < 2) fw->a.y = 2;
   }
  }
  if (fw->e.x - fw->a.x < 26)
  {
   if (at_left)
    fw->e.x = fw->a.x + 26;
   else
    fw->a.x = fw->e.x - 26;
  }
  if (fw->e.y >= MAXSLNS - 1) fw->e.y = MAXSLNS - 2;
  if (fw->e.x >= MAXSCOL) fw->e.x = MAXSCOL - 1;
  if (fw->a.y < 1) fw->a.y = 1;
  if (fw->a.x < 0) fw->a.x = 0;

  if (fw->zoom)
  {
   fw->sa = fw->a;
   fw->se = fw->e;
  }
 }
 /* Separate stacked top-anchored text editors that ended up overlapping
    only because the minimum-height expansion grew them past their
    neighbours. Two cases must NOT trigger a push:
      - Popup-style windows (FM, Data, dropdowns) are intentionally
        drawn on top of editors.
      - Editors that the user has cascaded on top of another editor
        were already overlapping in the original layout, so the new
        overlap is not a side-effect of expansion. */
 for (i = 0; i <= cn->mxedt; i++)
 {
  FENSTER *fw = cn->f[i];
  int j;
  if (!DTMD_ISTEXT(fw->dtmd)) continue;
  for (j = 0; j <= cn->mxedt; j++)
  {
   FENSTER *other = cn->f[j];
   if (j == i) continue;
   if (!DTMD_ISTEXT(other->dtmd)) continue;
   if (fw->a.y <= 1 && other->a.y > 1 && fw->e.y >= other->a.y)
   {
    int new_ay = fw->e.y + 1;
    /* Skip when the two windows were already overlapping before the
       relayout -- that's a user-chosen cascade, not collateral damage. */
    if (orig_ey[i] >= orig_ay[j]) continue;
    if (new_ay <= other->e.y - 3)
     other->a.y = new_ay;
   }
  }
 }

 /* Proportional scaling drifts the bottom-docked Messages window: a deep
    shrink+grow leaves it a row below the editor, and in a small grid off the
    screen entirely (hidden).  Re-dock it with its own splitter, which recomputes
    the editor/messages split from the current editor extents rather than scaling
    the old (squashed) position -- so the layout recovers no matter how far the
    window was shrunk.  Shared by all three backends. */
 for (i = 1; i <= cn->mxedt; i++)
  if (!strcmp(cn->f[i]->datnam, "Messages"))
  {
   e_position_messages_window(cn->f[i], cn);
   break;
  }
 for (i = 1; i <= cn->mxedt; i++)
  if (!strcmp(cn->f[i]->datnam, "AI"))
  {
   e_position_ai_window(cn->f[i], cn);
   break;
  }
}

/* The windows that live in the bottom strip and keep their place when the
   file windows are placed, tiled or cascaded. */
#define STRIP_WINDOW_MESSAGES "Messages"
#define STRIP_WINDOW_WATCHES  "Watches"
#define STRIP_WINDOW_AI       "AI"

/* The strip's default split row: the lower third for Messages alone, the
   bottom half for the AI pane (a conversation is tall). */
#define STRIP_SPLIT_MESSAGES  (2 * MAXSLNS / 3)
#define STRIP_SPLIT_AI        (MAXSLNS / 2)
/* The editor keeps at least this many rows above the strip ... */
#define STRIP_SPLIT_MIN       4
/* ... and the strip keeps at least this many rows for itself. */
#define STRIP_SPLIT_MAX       (MAXSLNS - 5)
/* At a very small height the two clamps above conflict (MAXSLNS-5 < 4), which
   would put the strip's top (split+1) below its bottom (MAXSLNS-2): an
   inverted, corrupt window that does not recover on a later grow.  Cap the
   split so the strip keeps at least one row (a.y <= e.y). */
#define STRIP_SPLIT_LAST      (MAXSLNS - 3)
/* The strip's last row: the row above the status line. */
#define STRIP_BOTTOM_ROW      (MAXSLNS - 2)

/**
 * e_win_is_bottom_docked - Is this window one of the bottom strip's panes?
 * @f: the window.
 *
 * Messages, Watches and the AI pane dock along the bottom; the file windows
 * are placed, clamped, tiled and cascaded around them.  Every function that
 * must leave them alone asks here, so adding a docked window is one change
 * and the compiler's output and the conversation stay put in every command.
 * Return: 1 for a docked pane, 0 for a file window or any other window.
 */
static int e_win_is_bottom_docked(const FENSTER *f)
{
 if (f == NULL || f->datnam == NULL)
  return 0;
 if (!strcmp(f->datnam, STRIP_WINDOW_MESSAGES))
  return 1;
 if (!strcmp(f->datnam, STRIP_WINDOW_WATCHES))
  return 1;
 if (!strcmp(f->datnam, STRIP_WINDOW_AI))
  return 1;
 return 0;
}

/**
 * e_strip_split_row - The row the bottom strip starts under.
 * @cn: the desktop.
 * @self: the window being placed; its own extent is not yet meaningful.
 * @default_split: the split used when no file window ends above it.
 *
 * A file window the user made short keeps its height (the strip starts under
 * it); otherwise the default applies.  The clamps keep both the editor and the
 * strip usable at any terminal size, so a shrink never produces an inverted
 * window.
 * Return: the split row; the strip's first row is the one below it.
 */
static int e_strip_split_row(ECNT *cn, const FENSTER *self, int default_split)
{
 int j, lowest_editor = -1, split_y;

 for (j = 1; j <= cn->mxedt; j++)
 {
  if (cn->f[j] == self) continue;
  if (e_win_is_bottom_docked(cn->f[j])) continue;
  if (cn->f[j]->e.y > lowest_editor)
   lowest_editor = cn->f[j]->e.y;
 }

 if (lowest_editor > 0 && lowest_editor <= default_split)
  split_y = lowest_editor;
 else
  split_y = default_split;

 if (split_y > STRIP_SPLIT_MAX)
  split_y = STRIP_SPLIT_MAX;
 if (split_y < STRIP_SPLIT_MIN)
  split_y = STRIP_SPLIT_MIN;
 if (split_y > STRIP_SPLIT_LAST)
  split_y = STRIP_SPLIT_LAST;
 return split_y;
}

/**
 * e_strip_clamp_editors - Keep every file window above the strip.
 * @cn: the desktop.
 * @self: the window being placed; it is never clamped.
 * @split_y: the strip's split row.
 *
 * Only file windows are clamped, so the text never hides under the strip.
 * The docked panes are placed by the strip itself and must be skipped exactly
 * as in the split computation: clamping one of them (positioning Watches with
 * Messages open) would collapse it to e.y = split_y with a.y = split_y + 1, an
 * inverted one-row window.
 */
static void e_strip_clamp_editors(ECNT *cn, const FENSTER *self, int split_y)
{
 int j;

 for (j = 1; j <= cn->mxedt; j++)
 {
  if (cn->f[j] == self) continue;
  if (e_win_is_bottom_docked(cn->f[j])) continue;
  if (cn->f[j]->e.y > split_y)
   cn->f[j]->e.y = split_y;
 }
}

/**
 * e_strip_find_window - The open window of that name, or NULL.
 * @cn: the desktop.
 * @self: the window being placed; never returned, so a window does not find
 *        itself when it is created under a docked pane's name.
 * @name: the window's name as shown in its title.
 */
static FENSTER *e_strip_find_window(ECNT *cn, const FENSTER *self,
                                    const char *name)
{
 int j;

 for (j = 1; j <= cn->mxedt; j++)
 {
  if (cn->f[j] == self) continue;
  if (cn->f[j]->datnam == NULL) continue;
  if (!strcmp(cn->f[j]->datnam, name))
   return cn->f[j];
 }
 return NULL;
}

/**
 * e_place_strip_full - Give one docked window the whole strip.
 * @f: the window.
 * @split_y: the strip's split row.
 */
static void e_place_strip_full(FENSTER *f, int split_y)
{
 f->a = e_set_pnt(0, split_y + 1);
 f->e = e_set_pnt(MAXSCOL - 1, STRIP_BOTTOM_ROW);
}

/**
 * e_place_strip_pair - Share the strip between Messages and the AI pane.
 * @msg: the Messages window, placed on the left half.
 * @ai: the AI pane, placed on the right half.
 * @split_y: the strip's split row, the same for both.
 *
 * The one geometry for the two windows, whichever of them opened second: a
 * compile started with the chat open never covers the conversation and a chat
 * opened during a build never covers the compiler's output.
 */
static void e_place_strip_pair(FENSTER *msg, FENSTER *ai, int split_y)
{
 int mid = MAXSCOL / 2;

 msg->a = e_set_pnt(0, split_y + 1);
 msg->e = e_set_pnt(mid - 1, STRIP_BOTTOM_ROW);
 ai->a = e_set_pnt(mid, split_y + 1);
 ai->e = e_set_pnt(MAXSCOL - 1, STRIP_BOTTOM_ROW);
}

/**
 * e_position_messages_window - Dock Messages (or Watches) along the bottom.
 * @msg: the window being placed.  It may still be under construction, so
 *       only its pointer is trusted, never its name or extent.
 * @cn: the desktop.
 *
 * Alone, the window takes the full width under the lower third of the
 * screen, or under a shorter file window, and the file windows are clamped
 * above it so the compiler's output never hides the text being edited.  When
 * the AI pane is open the two share the strip from the pane's split row,
 * Messages on the left and the pane on the right: a compile started with the
 * chat open shows its output without covering the conversation, and the
 * editors are not pushed up further than the pane already did.
 */
void e_position_messages_window(FENSTER *msg, ECNT *cn)
{
 FENSTER *ai = e_strip_find_window(cn, msg, STRIP_WINDOW_AI);
 int split_y;

 if (ai)
 {
  split_y = e_strip_split_row(cn, msg, STRIP_SPLIT_AI);
  e_place_strip_pair(msg, ai, split_y);
  /* The pane was drawn at the full width: its save-under holds that strip
     and the next repaint would restore it over Messages.  Drop it, so the
     repaint opens a fresh view at the pane's new rectangle. */
  e_free_view(&ai->pic);
 }
 else
 {
  split_y = e_strip_split_row(cn, msg, STRIP_SPLIT_MESSAGES);
  e_place_strip_full(msg, split_y);
 }
 e_strip_clamp_editors(cn, msg, split_y);
}

/**
 * e_position_ai_window - Dock the AI chat pane along the bottom.
 * @ai: the pane.
 * @cn: the desktop.
 *
 * Like Messages, the pane never overlaps the editor: the editor keeps the
 * top, the conversation reads down the bottom half (a conversation is tall,
 * so it gets the half rather than the Messages third).  When Messages is
 * also open the two share the strip side by side, Messages on the left and
 * the pane on the right, so neither hides the other.
 */
void e_position_ai_window(FENSTER *ai, ECNT *cn)
{
 int split_y = e_strip_split_row(cn, ai, STRIP_SPLIT_AI);
 FENSTER *msg = e_strip_find_window(cn, ai, STRIP_WINDOW_MESSAGES);

 if (msg)
  e_place_strip_pair(msg, ai, split_y);
 else
  e_place_strip_full(ai, split_y);
 e_strip_clamp_editors(cn, ai, split_y);
}

/**
 * e_invalidate_area - Mark a screen rectangle for full repaint.
 * @xa: Left column.
 * @ya: Top row.
 * @xe: Right column.
 * @ye: Bottom row.
 *
 * Invalidates altschirm cells so e_refresh repaints them, clears
 * extbyte border flags so NEWSTYLE XDrawLine segments get removed,
 * and in X11 mode clears the pixel area to remove stale graphics.
 */
void e_invalidate_area(int xa, int ya, int xe, int ye)
{
 extern SCREENCELL *altschirm;
 int i, j;

 if (xa < 0) xa = 0;
 if (ya < 0) ya = 0;
 if (xe >= MAXSCOL) xe = MAXSCOL - 1;
 if (ye >= MAXSLNS) ye = MAXSLNS - 1;

 for (j = ya; j <= ye; ++j)
  for (i = xa; i <= xe; ++i)
   altschirm[j * MAXSCOL + i].ch = -1;

#if !defined(NO_XWINDOWS) && defined(NEWSTYLE)
 if (WpeIsXwin())
 {
  extern char *extbyte, *altextbyte;
  int had_borders = 0;
  for (j = ya; j <= ye; ++j)
   for (i = xa; i <= xe; ++i)
   {
    if (extbyte[j * MAXSCOL + i]) had_borders = 1;
    extbyte[j * MAXSCOL + i] = 0;
    if (altextbyte) altextbyte[j * MAXSCOL + i] = 0;
   }
  { int cxa = xa > 0 ? xa - 1 : 0;
    int cya = ya > 0 ? ya - 1 : 0;
    int cxe = xe + 1 < MAXSCOL ? xe + 2 : MAXSCOL;
    int cye = ye + 1 < MAXSLNS ? ye + 2 : MAXSLNS;
    for (j = cya; j < cye; ++j)
     for (i = cxa; i < cxe; ++i)
      altschirm[j * MAXSCOL + i].ch = -1;
  }
 }
#endif
}

/* break string into multiple line to fit into windows 

   REM: Caller has to free returned vector!!! 
*/
char **StringToStringArray(char *str, int *maxLen, int minWidth, int *anzahl)
{
 int i, j, k, anz = 0, mxlen = 0, max = 0.8 * MAXSCOL;
 char **s = MALLOC(sizeof(char*));

 for (k = 0, i = 0; str[i]; i++)
 {
  if((i-k) == max || str[i] == '\n')
  {
   j = i-1;
   if (str[i] != '\n') 
    for (; j > 0 && !isspace(str[j]); j--)
     ;
   if(j > k)
    i = j;
   anz++;
   s = REALLOC(s, anz * sizeof(char *));
   s[anz-1] = MALLOC((i - k + 2) * sizeof(char));
   for (j = k; j <= i; j++)
    s[anz-1][j-k] = str[j];
   if (isspace(str[j-1]))
    j--;
   if (mxlen < (j-k))
    mxlen = j - k;
   s[anz-1][j-k] = '\0';
   k = i+1;
  }
 }
 anz++;
 s = REALLOC(s, anz * sizeof(char *));
 s[anz-1] = MALLOC((i - k + 2) * sizeof(char));
 for (j = k; j <= i; j++)
  s[anz-1][j-k] = str[j];
 if (mxlen < (j-k))
  mxlen = j - k;
 if (mxlen < minWidth)
  mxlen = minWidth;

 *maxLen = mxlen;
 *anzahl = anz;

 return (s);
}

/* The error box: its frame is the text's width plus E_ERROR_FRAME_COLS
   columns and E_ERROR_FRAME_ROWS rows.  It is centred as a box one column
   narrower and one row lower, as it has been drawn since 1.5.30a. */
#define E_ERROR_MIN_TEXT     20
#define E_ERROR_FRAME_COLS   5
#define E_ERROR_FRAME_ROWS   7
#define E_ERROR_CENTRE_COLS  (E_ERROR_FRAME_COLS - 1)
#define E_ERROR_CENTRE_ROWS  6

/* What the error box's draw hook needs. */
typedef struct
{
 char *text;
 char *header;
 int sw;          /* -1 message, 0 error, 1 serious, 2 fatal */
 FARBE *f;
} E_ERROR_BOX;

/**
 * e_error_frame - Draw the frame of an error box and keep what it covers.
 * @box: the box, placed by the owner.
 *
 * A fatal error, or a frame that could not be saved for want of memory,
 * is drawn plain: the box that reports a lack of memory must still show.
 * Without a saved background the box clears the screen when it closes.
 */
static void e_error_frame(WpeBox *box)
{
 E_ERROR_BOX *e = box->self;
 int failed = 1;

 if (e->sw < 2)
  failed = (wpe_box_frame(box, e->header, e->f->nr.fb, e->f->nt.fb, e->f->ne.fb) != 0);
 if (failed)
 {
  box->pic = e_open_view(box->at.bx_left, box->at.bx_top, box->at.bx_right,
                         box->at.bx_bottom, 0, 0);
  e_std_rahmen(box->at.bx_left, box->at.bx_top, box->at.bx_right,
               box->at.bx_bottom, e->header, 1, 0, 0);
 }
}

/**
 * e_error_box_draw - Draw an error box: its frame, its text and its OK.
 * @box: the box, placed by the owner for the screen as it is now.
 * Return: WPE_BOX_DRAWN always: a box that cannot save its background is
 * still drawn, because it is the one that reports the lack of memory.
 */
static WpeBoxDrawn e_error_box_draw(WpeBox *box)
{
 E_ERROR_BOX *e = box->self;
 int xa = box->at.bx_left, xe = box->at.bx_right, ya = box->at.bx_top;

 e_error_frame(box);
 if (e->sw < 2)
 {
  e_pr_str((xe + xa - e_str_len((unsigned char *)e->text))/2,
    ya + 2, e->text, e->f->nt.fb, 0, 0, 0, 0);
  e_pr_str((xe + xa - 4)/2, ya + 4, " OK ", e->f->nz.fb, 1, -1,
    e->f->ns.fb, e->f->nt.fb);
 }
 else
 {
  e_pr_str((xe + xa - e_str_len((unsigned char *)e->text))/2,
    ya + 2, e->text, 112, 0, 0, 0, 0);
  e_pr_str((xe + xa - 4)/2, ya + 4, " OK ", 32, 1, -1, 46, 112);
 }
 return(WPE_BOX_DRAWN);
}

/**
 * e_error_box_init - Give the error box its rule: centred, as a box of the
 * text's width, from the row a six-row box would have.
 * @box: receives the box; @e: its text, header, kind and colours; @len: the
 * width the text takes.
 */
static void e_error_box_init(WpeBox *box, E_ERROR_BOX *e, int len)
{
 memset(box, 0, sizeof *box);
 box->what = "an error box";
 box->self = e;
 box->draw = e_error_box_draw;
 box->place.anchor = WPE_BOX_CENTRED_SCREEN;
 box->place.w = len + E_ERROR_FRAME_COLS;
 box->place.h = E_ERROR_FRAME_ROWS;
 box->place.centre_w = len + E_ERROR_CENTRE_COLS;
 box->place.centre_h = E_ERROR_CENTRE_ROWS;
}

/**
 * e_error_read_answer - Read the key that answers an error box.
 * @box: the open box.
 * Return: Esc, Return or O to leave; WPE_RESIZE when the screen changed
 * (the owner has drawn the box again); anything else otherwise.
 */
static int e_error_read_answer(WpeBox *box)
{
 int i;

#if  MOUSE
 if ((i = e_toupper(wpe_box_getch(box))) == -1)
 {
  extern struct mouse e_mouse;
  int xa = box->at.bx_left, xe = box->at.bx_right, ya = box->at.bx_top;

  i = e_er_mouse(xa+3, ya,(xe+xa-4)/2, ya+4);
  if (i == 0 && e_mouse.y == ya && e_mouse.x == xe-2)
   i = WPE_ESC;   /* clicked the title-bar [X] close box -> dismiss */
 }
#else
 i = e_toupper(wpe_box_getch(box));
#endif
 return(i);
}

/*
      Print error message        */
static int e_error_run(char *text, int sw, FARBE *f)
{
 WpeBox box;
 E_ERROR_BOX e;
 int len, i;

 fk_cursor(0);
 WpeMouseChangeShape(WpeErrorShape);
 if ((len = strlen((char *)text)) < E_ERROR_MIN_TEXT) len = E_ERROR_MIN_TEXT;
 e.text = text;
 e.sw = sw;
 e.f = f;
 e.header = NULL;
 if (sw == -1) e.header = "Message";
 else if (sw == 0) e.header = "Error";
 else if (sw == 1) e.header = "Serious Error";
 else if (sw == 2) e.header = "Fatal Error";

 e_error_box_init(&box, &e, len);
 wpe_box_open(&box);
 do
  i = e_error_read_answer(&box);
 while (i != WPE_ESC && i != WPE_CR && i != 'O');
 WpeMouseRestoreShape();
 if (box.pic != NULL) e_close_view(box.pic, 1);
 else e_cls(0, ' ');
 box.pic = NULL;
 wpe_box_close(&box);
 fk_cursor(1);
 if (sw == 1) e_quit(WpeEditor->f[WpeEditor->mxedt]);
 if (sw > 0) WpeExit(sw);
 return(sw);
}

/* e_error - public entry to the message box (errors and plain "Message" notes,
   e.g. an LSP action's "No references found", shown right after the action while
   the server may still be streaming).  It runs its own getch loop over a box that
   pushes no window; the box is opened through the layout owner (wpe_box_open),
   which brackets the async LSP fd-loop's painting with the modal-depth guard, so
   a server update arriving behind the box cannot draw under it and corrupt it,
   and lays the box out again when the screen changes size.  A fatal error
   (sw >= 1) closes its box and exits from e_error_run. */
int e_error(char *text, int sw, FARBE *f)
{
 int ret;
#ifdef UNIX
 /* The editor is ending and writing its .ESV files: nobody can answer the
    box, and its terminal may be gone.  The save reports the failure. */
 if (e_emergency_save_running())
  return(sw);
#endif
 wpe_test_ctx_enter(WPE_TEST_CTX_MESSAGE, NULL);
 ret = e_error_run(text, sw, f);
 wpe_test_ctx_leave();
 return(ret);
}

/* The rows of a question box that are not text (its frame, the blanks and
   the buttons: six) and the two the screen keeps for the menu bar and the
   status row: the text gets what is left. */
#define E_MESSAGE_FRAME_ROWS  8
#define E_MESSAGE_ELLIPSIS    "..."

/**
 * e_message_fit_rows - Cut the text of a question to the rows the screen has.
 * @s: the text's lines, as StringToStringArray made them; @anz: their count,
 * lowered to what fits.
 *
 * A path or a command of thousands of characters wraps into more lines than
 * the screen has rows, and the box then had no room left for its buttons: the
 * user could only press Esc.  The lines that do not fit are left out and the
 * last one shown says so with "...", so Yes and No are always on the screen.
 */
static void e_message_fit_rows(char **s, int *anz)
{
 int room = MAXSLNS - E_MESSAGE_FRAME_ROWS;
 int i;

 if (room < 1)
  room = 1;
 if (*anz <= room)
  return;
 for (i = room; i < *anz; i++)
  FREE(s[i]);
 FREE(s[room - 1]);
 s[room - 1] = WpeStrdup(E_MESSAGE_ELLIPSIS);
 *anz = room;
}

/*   message with selection        */
int e_message(int sw, char *str, FENSTER *f)
{
 int i, ret, mxlen = 0, anz = 0;
 char **s;
 W_OPTSTR *o = e_init_opt_kst(f);

 if (!o)
  return(-1);

 s = StringToStringArray(str, &mxlen, 22, &anz);
 e_message_fit_rows(s, &anz);

 o->ye = MAXSLNS - 6;
 o->ya = o->ye - anz - 5;
 o->xa = (MAXSCOL - mxlen - 6)/2;
 o->xe = o->xa + mxlen + 6;

 o->bgsw = 0;
 o->name = "Message";
 for (i = 0; i < anz; i++)
 {
  e_add_txtstr((o->xe-o->xa-strlen(s[i]))/2, 2+i, s[i], o);
  FREE(s[i]);
 }
 FREE(s);
 if (!sw)
 {
  o->crsw = AltO;
  e_add_bttstr((o->xe-o->xa-4)/2, o->ye-o->ya-2, 0, AltO, "Ok", NULL, o);
 }
 else
 {
  o->crsw = AltY;
  e_add_bttstr(4, o->ye-o->ya-2, 0, AltY, "Yes", NULL, o);
  e_add_bttstr((o->xe-o->xa-2)/2, o->ye-o->ya-2, 0, AltN, "No", NULL, o);
  e_add_bttstr(o->xe-o->xa-9, o->ye-o->ya-2, -1, WPE_ESC, "Cancel", NULL, o);
 }
 ret = e_opt_kst_message(o);
 freeostr(o);
 return(ret == WPE_ESC ? WPE_ESC : (ret == AltN ? 'N' : 'Y'));
}

/* The question a folder's command gets: its frame, and the room it takes. */
#define E_STARTCMD_TITLE_FMT    "Command from %s"
#define E_STARTCMD_HEAD_FMT     "%s sets the %s to:"
#define E_STARTCMD_OWN_FMT      "Your own setting: %s"
#define E_STARTCMD_OWN_NONE     "Your own setting: (none)"
#define E_STARTCMD_FOLDER_FMT   "Folder: %s"
#define E_STARTCMD_LONG_FMT     "the command is %lu characters; read it in %s"
#define E_STARTCMD_CUT_MARK     "..."
#define E_STARTCMD_BOX_MAX_W    76   /* the widest the box gets            */
#define E_STARTCMD_MARGIN       3    /* columns from the frame to the text */
#define E_STARTCMD_INDENT       2    /* the command's own indent           */
#define E_STARTCMD_FIXED_ROWS   11   /* frame, sentences, blanks, buttons  */
#define E_STARTCMD_SCREEN_ROWS  2    /* the menu bar and the status row    */
#define E_STARTCMD_LINE_MAX     1024 /* a sentence of the box             */
#define E_STARTCMD_OWN_ROWS_MAX 6    /* the most rows the user's own setting takes */
#define E_STARTCMD_STATUS_MAX   512  /* a status line about the answer    */

/* How a line below the command is cut when it is wider than the box. */
#define E_STARTCMD_EXTRA_START  0    /* keep its start                     */
#define E_STARTCMD_EXTRA_END    1    /* keep its end, where a path's file is */
#define E_STARTCMD_EXTRA_WHOLE  2    /* a command that runs: cut, Run it is
                                        not offered                        */

/* What the status row says after each answer. */
#define E_STARTCMD_RAN_FMT      "running %s's %s (remembered for this folder)"
#define E_STARTCMD_MINE_FMT     "using your own %s (%s); %s's was not run"
#define E_STARTCMD_CANCEL_FMT   "%s's %s was not run"
#define E_STARTCMD_STORE_BAD    "could not read the remembered commands: asking again"
#define E_STARTCMD_NOT_KEPT     "could not remember the answer: the next run asks again"

/* The question's buttons, and what each one answers. */
#define E_STARTCMD_RUN_KEY  AltR
#define E_STARTCMD_MINE_KEY AltU

/* The store could not be read: said once a session. */
static int e_startcmd_store_said;

/* What ends a row of the command when the command goes on below it. */
#define E_STARTCMD_WRAP_MARK "\\"
#define E_STARTCMD_WRAP_ROOM 1

/**
 * e_startcmd_rows - Cut a shown command into rows of the box's width.
 * @shown: the command in its printable form; @width: characters per row;
 * @rows: receives the malloc'd rows (the caller frees each and the list).
 *
 * The cut is by characters, not words, so every byte of the command is on
 * screen in the order it runs, a long option included.  A row the command
 * goes on after ends with E_STARTCMD_WRAP_MARK, so a blank right at the
 * cut is seen before the mark and not lost at the row's end (the printable
 * form doubles every backslash of the command, so a single one is never
 * part of it).
 * Return: the number of rows, 0 out of memory.
 */
static int e_startcmd_rows(const char *shown, int width, char ***rows)
{
 const char *p = shown;
 int n = 0, chars, len, room = width - E_STARTCMD_WRAP_ROOM;
 size_t take;
 char **grown;

 *rows = NULL;
 if (room < 1)
  room = 1;
 while (*p)
 {
  const char *start = p;
  for (chars = 0; *p && chars < room; chars++)
  {
   len = mblen(p, MB_CUR_MAX);
   if (len < 1) len = 1;
   p += len;
  }
  if (!(grown = REALLOC(*rows, (n + 1) * sizeof(char *))))
   return(n);
  *rows = grown;
  take = p - start;
  if (!((*rows)[n] = MALLOC(take + E_STARTCMD_WRAP_ROOM + 1)))
   return(n);
  memcpy((*rows)[n], start, take);
  (*rows)[n][take] = '\0';
  if (*p)
   strcat((*rows)[n], E_STARTCMD_WRAP_MARK);
  n++;
 }
 return(n);
}

/** e_startcmd_free_rows - Free what e_startcmd_rows made. */
static void e_startcmd_free_rows(char **rows, int n)
{
 int i;

 for (i = 0; i < n; i++)
  FREE(rows[i]);
 if (rows)
  FREE(rows);
}

/**
 * e_startcmd_fit - A sentence cut to the box's width.
 * @text: the sentence; @width: the room; @out/@sz: receive it.
 * @keep_end: 1 to keep the end (a folder), 0 to keep the start.
 */
static void e_startcmd_fit(const char *text, int width, char *out, size_t sz,
                           int keep_end)
{
 int len = (int)strlen(text), mark = (int)strlen(E_STARTCMD_CUT_MARK);

 if (len <= width || width <= mark)
  snprintf(out, sz, "%s", text);
 else if (keep_end)
  snprintf(out, sz, "%s%s", E_STARTCMD_CUT_MARK, text + len - (width - mark));
 else
  snprintf(out, sz, "%.*s%s", width - mark, text, E_STARTCMD_CUT_MARK);
}

/**
 * e_startcmd_shown - The printable form of a command, malloc'd.
 * @text: the command.
 * Return: the form (escapes written out), or NULL out of memory.
 */
static char *e_startcmd_shown(const char *text)
{
 size_t need = wpe_startcmd_printable(text, NULL, 0) + 1;
 char *shown = MALLOC(need);

 if (shown)
  wpe_startcmd_printable(text, shown, need);
 return(shown);
}

/**
 * e_startcmd_add_line - Put one sentence in the box, cut to its width.
 * @o: the box; @y: its row; @text: the sentence; @width: the room;
 * @keep_end: as e_startcmd_fit.
 */
static void e_startcmd_add_line(W_OPTSTR *o, int y, const char *text, int width,
                                int keep_end)
{
 char line[E_STARTCMD_LINE_MAX];

 e_startcmd_fit(text, width, line, sizeof line, keep_end);
 e_add_txtstr(E_STARTCMD_MARGIN, y, line, o);
}

/**
 * e_startcmd_own_rows - The user's own setting as the rows the box draws.
 * @own: the user's own command, or NULL; @width: characters per row;
 * @rows: receives the malloc'd rows (e_startcmd_free_rows frees them).
 *
 * What "Use mine" starts is read whole, like the command: a line wider than
 * the box goes on in the next row instead of being cut at the edge.
 * Return: the number of rows, 0 out of memory.
 */
static int e_startcmd_own_rows(const char *own, int width, char ***rows)
{
 char line[E_STARTCMD_LINE_MAX];
 char *shown;

 *rows = NULL;
 if (!own || !*own)
  return(e_startcmd_rows(E_STARTCMD_OWN_NONE, width, rows));
 shown = e_startcmd_shown(own);
 if (!shown)
  return(0);
 snprintf(line, sizeof line, E_STARTCMD_OWN_FMT, shown);
 FREE(shown);
 return(e_startcmd_rows(line, width, rows));
}

/**
 * e_startcmd_add_own - The user's own setting, as the box names it.
 * @o: the box; @y: its first row; @rows/@n: the rows e_startcmd_own_rows made.
 */
static void e_startcmd_add_own(W_OPTSTR *o, int y, char **rows, int n)
{
 int i;

 for (i = 0; i < n; i++)
  e_add_txtstr(E_STARTCMD_MARGIN, y + i, rows[i], o);
}

/**
 * e_startcmd_add_command - The command's rows, or as many as fit.
 * @o: the box; @y: the first row; @rows/@nrows: the cut command;
 * @room: the rows the screen leaves; @c: the command; @width: the room.
 * Return: 1 when the whole command is shown, 0 when it was cut and the
 * last row says so.
 */
static int e_startcmd_add_command(W_OPTSTR *o, int y, char **rows, int nrows,
                                  int room, const E_STARTCMD *c, int width)
{
 char line[E_STARTCMD_LINE_MAX];
 int i;

 if (nrows <= room)
 {
  for (i = 0; i < nrows; i++)
   e_add_txtstr(E_STARTCMD_MARGIN + E_STARTCMD_INDENT, y + i, rows[i], o);
  return(1);
 }
 for (i = 0; i < room - 1; i++)
  e_add_txtstr(E_STARTCMD_MARGIN + E_STARTCMD_INDENT, y + i, rows[i], o);
 snprintf(line, sizeof line, E_STARTCMD_LONG_FMT,
          (unsigned long)strlen(c->text), c->source);
 e_startcmd_add_line(o, y + room - 1, line, width, 0);
 return(0);
}

/**
 * e_startcmd_add_buttons - Run it (when everything is shown), Use mine (when
 * the user has his own), Cancel; the focus starts on Use mine, else on
 * Cancel, never on Run it.
 * @o: the box; @y: the buttons' row; @offer_run: 1 to offer Run it;
 * @offer_mine: 1 to offer Use mine.
 */
static void e_startcmd_add_buttons(W_OPTSTR *o, int y, int offer_run, int offer_mine)
{
 int w = o->xe - o->xa;

 if (offer_run)
  e_add_bttstr(4, y, 0, E_STARTCMD_RUN_KEY, "Run it", NULL, o);
 if (offer_mine)
  e_add_bttstr(w / 2 - 5, y, 0, E_STARTCMD_MINE_KEY, "Use mine", NULL, o);
 e_add_bttstr(w - 11, y, -1, WPE_ESC, "Cancel", NULL, o);
 if (offer_mine)
 {
  o->crsw = E_STARTCMD_MINE_KEY;      /* Enter on no button */
  o->bgsw = E_STARTCMD_MINE_KEY;      /* the focus as the box opens */
  return;
 }
 o->crsw = WPE_ESC;
 o->bgsw = WPE_ESC;
}

/* What the box shows: the shared form of a folder's command and of a plan. */
typedef struct {
 const char *source;   /* the title's file: "./xwperc", "demo.prj"             */
 const char *head;     /* "This folder's xwperc sets the C compiler to:"       */
 const char *text;     /* the command, shown whole in rows                     */
 const char *own;      /* the user's own, or NULL                              */
 const char *folder;   /* the folder the answer is kept for                    */
 const char *key;      /* the trace's key                                      */
 char **extra;         /* the plan's lines below the command, or NULL          */
 const int *extra_end; /* per line: E_STARTCMD_EXTRA_*, how it is cut        */
 int nextra;
 int offer_mine;       /* the user has a plan of his own "Use mine" can start  */
 int withhold_run;     /* something the plan runs is not shown whole            */
} e_startcmd_view;

/* A row between the plan's lines and the user's own setting. */
#define E_STARTCMD_EXTRA_GAP 1
/* The fewest rows the command keeps when the plan's lines are many. */
#define E_STARTCMD_CMD_MIN_ROWS 2
#define E_STARTCMD_MORE_FMT "... %d more lines (not all of the plan fits)"

/**
 * e_startcmd_extra_rows - How many of the plan's lines the screen can show.
 * @v: the view; @avail: the rows left for the command and the lines.
 * Return: the lines shown (all, or as many as leave the command its rows);
 * fewer than all withholds Run it, as a command that does not fit does.
 */
static int e_startcmd_extra_rows(const e_startcmd_view *v, int avail)
{
 int fit;

 if (v->nextra == 0)
  return(0);
 fit = avail - E_STARTCMD_CMD_MIN_ROWS - E_STARTCMD_EXTRA_GAP;
 if (fit < 1)
  fit = 1;
 if (v->nextra <= fit)
  return(v->nextra);
 return(fit);
}

/**
 * e_startcmd_add_extra - The plan's lines: where each folder value came
 * from, what lies outside the folder, what was not removed.
 * @o: the box; @y: the first row; @v: the view, its lines already box rows
 * in their printable form (e_startcmd_wrap_extra); @shown: how many fit;
 * @width: the room.
 */
static void e_startcmd_add_extra(W_OPTSTR *o, int y, const e_startcmd_view *v, int shown,
                                 int width)
{
 char line[E_STARTCMD_LINE_MAX];
 int i;

 for (i = 0; i < shown; i++)
 {
  if (shown < v->nextra && i == shown - 1)
  {
   snprintf(line, sizeof line, E_STARTCMD_MORE_FMT, v->nextra - i);
   e_startcmd_add_line(o, y + i, line, width, 0);
   break;
  }
  if (v->extra_end && v->extra_end[i] == E_STARTCMD_EXTRA_END)
   e_startcmd_add_line(o, y + i, v->extra[i], width, 1);
  else
   e_startcmd_add_line(o, y + i, v->extra[i], width, 0);
 }
}

/**
 * e_startcmd_extra_push - Append one box row to the wrapped lines.
 * @w: the view being filled; @row: the row, taken (freed on failure);
 * @keep: its E_STARTCMD_EXTRA_*.
 * Return: 0, or -1 out of memory.
 */
static int e_startcmd_extra_push(e_startcmd_view *w, char *row, int keep)
{
 char **grown;
 int *grown_keep;

 if (!(grown = REALLOC(w->extra, (w->nextra + 1) * sizeof(char *))))
 {
  FREE(row);
  return(-1);
 }
 w->extra = grown;
 if (!(grown_keep = REALLOC((int *)w->extra_end, (w->nextra + 1) * sizeof(int))))
 {
  FREE(row);
  return(-1);
 }
 w->extra_end = grown_keep;
 w->extra[w->nextra] = row;
 grown_keep[w->nextra] = keep;
 w->nextra++;
 return(0);
}

/**
 * e_startcmd_extra_push_wrapped - Append a line the user must read whole,
 * cut into rows of the box's width, each row but the last ending with the
 * wrap mark.
 * @w: the view being filled; @shown: the line in its printable form;
 * @width: characters per row.
 * Return: 0, or -1 out of memory.
 */
static int e_startcmd_extra_push_wrapped(e_startcmd_view *w, const char *shown, int width)
{
 char **rows;
 int i, n = e_startcmd_rows(shown, width, &rows), rc = 0;

 if (n == 0 && *shown)
  return(-1);
 for (i = 0; i < n; i++)
 {
  if (rc == 0)
   rc = e_startcmd_extra_push(w, rows[i], E_STARTCMD_EXTRA_START);
  else
   FREE(rows[i]);
 }
 if (rows)
  FREE(rows);
 return(rc);
}

/**
 * e_startcmd_wrap_extra - The plan's lines as the rows the box draws.
 * @v: the view as built; @width: characters per row; @w: receives @v with
 * its lines replaced by rows in their printable form.
 *
 * A line of a file the tool runs at its start (E_STARTCMD_EXTRA_WHOLE) is
 * wrapped like the command, never cut: a command past the box's edge
 * would otherwise run with the user having seen only its harmless start.
 * When the wrapped rows do not all fit, Run it is withheld as for any plan
 * that does not fit.  Other lines stay one row each, cut at the box's edge.
 * Return: 0, or -1 out of memory (the rows made so far are in @w).
 */
static int e_startcmd_wrap_extra(const e_startcmd_view *v, int width, e_startcmd_view *w)
{
 char *shown;
 int i, keep;

 *w = *v;
 w->extra = NULL;
 w->extra_end = NULL;
 w->nextra = 0;
 for (i = 0; i < v->nextra; i++)
 {
  if (!(shown = e_startcmd_shown(v->extra[i])))
   return(-1);
  keep = E_STARTCMD_EXTRA_START;
  if (v->extra_end)
   keep = v->extra_end[i];
  if (keep == E_STARTCMD_EXTRA_WHOLE)
  {
   if (e_startcmd_extra_push_wrapped(w, shown, width) < 0)
   {
    FREE(shown);
    return(-1);
   }
   FREE(shown);
  }
  else if (e_startcmd_extra_push(w, shown, keep) < 0)
   return(-1);
 }
 return(0);
}

/**
 * e_startcmd_wrap_free - Free the rows e_startcmd_wrap_extra made.
 */
static void e_startcmd_wrap_free(e_startcmd_view *w)
{
 int i;

 for (i = 0; i < w->nextra; i++)
  FREE(w->extra[i]);
 if (w->extra)
  FREE(w->extra);
 if (w->extra_end)
  FREE((int *)w->extra_end);
 w->extra = NULL;
 w->extra_end = NULL;
 w->nextra = 0;
}

/**
 * e_startcmd_box_width - How wide the box is on this screen.
 */
static int e_startcmd_box_width(void)
{
 int width = MAXSCOL - 4;

 if (width > E_STARTCMD_BOX_MAX_W)
  width = E_STARTCMD_BOX_MAX_W;
 return(width);
}

/**
 * e_startcmd_show_rows - Draw the box and ask, the plan's lines already rows.
 * @f: the window it was asked from; @v: what to show, as
 * e_startcmd_wrap_extra made it.
 * Return: as e_startcmd_show.
 */
static int e_startcmd_show_rows(FENSTER *f, const e_startcmd_view *v)
{
 W_OPTSTR *o;
 char title[E_STARTCMD_LINE_MAX], line[E_STARTCMD_LINE_MAX];
 char *shown, **rows = NULL, **own_text = NULL;
 int width, text_w, nrows, room, shown_rows, whole, y, ret, nextra, extra_rows;
 int own_rows, own_shown;
 E_STARTCMD c;

 if (!(shown = e_startcmd_shown(v->text)))
  return(WPE_STARTCMD_CANCEL);
 if (!(o = e_init_opt_kst(f)))
 {
  FREE(shown);
  return(WPE_STARTCMD_CANCEL);
 }
 width = e_startcmd_box_width();
 text_w = width - 2 * E_STARTCMD_MARGIN;
 nrows = e_startcmd_rows(shown, text_w - E_STARTCMD_INDENT, &rows);
 own_rows = e_startcmd_own_rows(v->own, text_w, &own_text);
 own_shown = own_rows < E_STARTCMD_OWN_ROWS_MAX ? own_rows : E_STARTCMD_OWN_ROWS_MAX;
 if (own_shown < 1)
  own_shown = 1;
 room = MAXSLNS - E_STARTCMD_SCREEN_ROWS - E_STARTCMD_FIXED_ROWS - (own_shown - 1);
 nextra = e_startcmd_extra_rows(v, room);
 extra_rows = nextra ? nextra + E_STARTCMD_EXTRA_GAP : 0;
 room -= extra_rows;
 if (room < 1)
  room = 1;
 shown_rows = nrows < room ? nrows : room;
 if (shown_rows < 1)
  shown_rows = 1;
 o->xa = (MAXSCOL - width) / 2;
 o->xe = o->xa + width;
 o->ya = (MAXSLNS - shown_rows - extra_rows - E_STARTCMD_FIXED_ROWS - (own_shown - 1)) / 2;
 if (o->ya < 1)
  o->ya = 1;
 o->ye = o->ya + shown_rows + extra_rows + E_STARTCMD_FIXED_ROWS + (own_shown - 1) - 1;
 o->bgsw = 0;
 snprintf(title, sizeof title, E_STARTCMD_TITLE_FMT, v->source);
 o->name = title;
 e_startcmd_add_line(o, 2, v->head, text_w, 0);
 c.text = v->text;
 c.source = v->source;
 whole = e_startcmd_add_command(o, 4, rows, nrows, room, &c, text_w);
 if (nextra < v->nextra || v->withhold_run)
  whole = 0;
 y = 4 + shown_rows + 1;
 e_startcmd_add_extra(o, y, v, nextra, text_w);
 y += extra_rows;
 if (own_shown < own_rows)
  e_startcmd_add_own(o, y, own_text, own_shown);
 else
  e_startcmd_add_own(o, y, own_text, own_rows);
 snprintf(line, sizeof line, E_STARTCMD_FOLDER_FMT, v->folder);
 e_startcmd_add_line(o, y + own_shown, line, text_w, 1);
 e_startcmd_add_buttons(o, o->ye - o->ya - 2, whole, v->offer_mine);
 wpe_ui_trace("startcmd ask %s %s%s", v->key, shown, whole ? "" : " (too long)");
 ret = e_opt_kst_message(o);
 freeostr(o);
 e_startcmd_free_rows(rows, nrows);
 e_startcmd_free_rows(own_text, own_rows);
 FREE(shown);
 if (ret == E_STARTCMD_RUN_KEY && whole)
  return(WPE_STARTCMD_RUN);
 if (ret == E_STARTCMD_MINE_KEY && v->offer_mine)
  return(WPE_STARTCMD_USE_OWN);
 return(WPE_STARTCMD_CANCEL);
}

/**
 * e_startcmd_show - Show a folder's command, or a start plan, and ask.
 * @f: the window it was asked from; @v: what to show.
 *
 * The command is shown whole, every control byte written out, so what the
 * user approves is what runs; a plan's lines follow it, and the lines of a
 * file the tool runs at its start are wrapped like the command, never cut.
 * When the command or the lines do not fit on the screen, Run it is not
 * offered at all.
 * Return: WPE_STARTCMD_RUN, WPE_STARTCMD_USE_OWN or WPE_STARTCMD_CANCEL.
 */
static int e_startcmd_show(FENSTER *f, const e_startcmd_view *v)
{
 e_startcmd_view w;
 int ret = WPE_STARTCMD_CANCEL;

 if (e_startcmd_wrap_extra(v, e_startcmd_box_width() - 2 * E_STARTCMD_MARGIN, &w) == 0)
  ret = e_startcmd_show_rows(f, &w);
 e_startcmd_wrap_free(&w);
 return(ret);
}

/**
 * e_startcmd_ask - Show a folder's command and ask whether to run it.
 * @f: the window the command was asked from; @c: the command.
 * Return: as e_startcmd_show.
 */
static int e_startcmd_ask(FENSTER *f, const E_STARTCMD *c)
{
 e_startcmd_view v;
 char head[E_STARTCMD_LINE_MAX];

 snprintf(head, sizeof head, E_STARTCMD_HEAD_FMT, c->who, c->what);
 memset(&v, 0, sizeof v);
 v.source = c->source;
 v.head = head;
 v.text = c->text;
 v.own = c->own;
 v.folder = c->folder;
 v.key = c->key;
 v.offer_mine = 1;
 return(e_startcmd_show(f, &v));
}

/**
 * e_startcmd_say - Put a line about the answer on the status row.
 * @f: the window; @text: the line.
 */
static void e_startcmd_say(FENSTER *f, const char *text)
{
 wpe_paint_trace_say(text);
 e_status_hold(text);
 e_status_paint(f->ed->fb, text);
 e_refresh();
}

/**
 * e_startcmd_say_answer - Tell the user which command now runs, and why.
 * @f: the window; @c: the command; @verdict: the answer;
 * @remembered: 1 when the answer was just kept for this folder.
 */
static void e_startcmd_say_answer(FENSTER *f, const E_STARTCMD *c, int verdict,
                                  int remembered)
{
 char line[E_STARTCMD_STATUS_MAX], own[E_STARTCMD_STATUS_MAX];

 own[0] = '\0';
 if (c->own)
  wpe_startcmd_printable(c->own, own, sizeof own);
 if (verdict != WPE_STARTCMD_CANCEL && !remembered)
  snprintf(line, sizeof line, "%s", E_STARTCMD_NOT_KEPT);
 else if (verdict == WPE_STARTCMD_RUN)
  snprintf(line, sizeof line, E_STARTCMD_RAN_FMT, c->source, c->what);
 else if (verdict == WPE_STARTCMD_USE_OWN)
  snprintf(line, sizeof line, E_STARTCMD_MINE_FMT, c->what, own, c->source);
 else
  snprintf(line, sizeof line, E_STARTCMD_CANCEL_FMT, c->source, c->what);
 e_startcmd_say(f, line);
}

/** e_startcmd_answer_word - The trace's word for an answer. */
static const char *e_startcmd_answer_word(int verdict)
{
 if (verdict == WPE_STARTCMD_RUN)
  return("run");
 if (verdict == WPE_STARTCMD_USE_OWN)
  return("own");
 return("cancel");
}

/**
 * e_startcmd_say_store_bad - Say once that the remembered answers could not
 * be read, so the user knows why a folder he answered for asks again.
 * @f: the window.
 */
static void e_startcmd_say_store_bad(FENSTER *f)
{
 if (e_startcmd_store_said)
  return;
 if (wpe_startcmd_last_store_state() != WPE_STARTCMD_STORE_BAD)
  return;
 e_startcmd_store_said = 1;
 e_startcmd_say(f, E_STARTCMD_STORE_BAD);
}

/**
 * e_startcmd_approve - Ask before running a command a project folder names.
 * @f: the window the command was asked from; @c: the command, the folder
 * and the file that named it, and the user's own command.
 *
 * Called by every site that starts a program a start-folder xwperc or a
 * project file can name (the print command, a language's compile and link
 * line, the debugger), just before it starts the child.  A cloned project
 * could otherwise run a program of its own on the user's first F9: the user
 * now sees the command and chooses.  His own command, the built-in default
 * and a command he answered for in this folder run without a question; a
 * changed command asks again.  The answer is kept in his state folder.
 * Return: WPE_STARTCMD_RUN (start the folder's command),
 * WPE_STARTCMD_USE_OWN (start @c->own instead) or WPE_STARTCMD_CANCEL
 * (start nothing).
 */
int e_startcmd_approve(FENSTER *f, const E_STARTCMD *c)
{
 int verdict, answer, kept;

 if (!c || !c->folder || !c->text)
  return(WPE_STARTCMD_RUN);
 verdict = wpe_startcmd_check(c->folder, c->key, c->text);
 if (verdict == WPE_STARTCMD_RUN)
 {
  wpe_ui_trace("startcmd run %s", c->key);
  return(verdict);
 }
 if (verdict == WPE_STARTCMD_USE_OWN)
 {
  wpe_ui_trace("startcmd remembered %s own", c->key);
  e_startcmd_say_answer(f, c, verdict, 1);
  return(verdict);
 }
 e_startcmd_say_store_bad(f);
 verdict = e_startcmd_ask(f, c);
 wpe_ui_trace("startcmd answer %s %s", c->key, e_startcmd_answer_word(verdict));
 if (verdict == WPE_STARTCMD_CANCEL)
 {
  e_startcmd_say_answer(f, c, verdict, 0);
  return(verdict);
 }
 if (verdict == WPE_STARTCMD_RUN)
  answer = WPE_STARTCMD_YES;
 else
  answer = WPE_STARTCMD_NO;
 kept = wpe_startcmd_remember(c->folder, c->key, c->text, answer) == 0;
 e_startcmd_say_answer(f, c, verdict, kept);
 return(verdict);
}

/**
 * e_startcmd_typed - A command the user set in Options counts as his own.
 * @folder: the folder whose file the options came from, or NULL;
 * @key: the key; @text: the command he set.
 *
 * Saving the Programming, Print or Debugger options with a command the
 * user typed records a yes for it, in this folder: he is not asked about
 * what he has just written himself, now or after the next start.
 */
void e_startcmd_typed(const char *folder, const char *key, const char *text)
{
 if (!key || !text)
  return;
 wpe_startcmd_note_own(key, text);
 if (folder && *text)
  wpe_startcmd_remember(folder, key, text, WPE_STARTCMD_YES);
}

/* The plan's lines in the box, and what the status row says about it. */
#define E_STARTPLAN_FROM_FMT     "From %s: %s = %s"
#define E_STARTPLAN_FROM_EXP_FMT "From %s: %s = %s -> %s"
#define E_STARTPLAN_WRITES       "Writes outside this folder:"
#define E_STARTPLAN_READS        "Reads outside this folder:"
#define E_STARTPLAN_PATH_FMT     "  %s"
#define E_STARTPLAN_DROPPED_FMT  "not removed: %s (outside this folder)"
#define E_STARTPLAN_INSIDE_FMT   "%d files from %s, all inside this folder"
#define E_STARTPLAN_REFUSED_FMT  "%s's %s was not run: %s"
#define E_STARTPLAN_RUNS_FMT     "Runs at start: %s (%d line%s)"
#define E_STARTPLAN_RUNS_LONG_FMT "Runs at start: %s (%ld bytes, cannot be shown whole)"
/* A debugger that starts apart runs nothing of the folder's at start: the
   files it lists are what Run it makes the editor load. */
#define E_STARTPLAN_WITH_RUN_FMT "With Run it: %s (%d line%s)"
#define E_STARTPLAN_WITH_RUN_LONG_FMT "With Run it: %s (%ld bytes, cannot be shown whole)"
#define E_STARTPLAN_DEBUGS_FMT   "Debugs: %s"
#define E_STARTPLAN_NO_RUN_IT_FMT "Run it is not offered: %s"
#define E_STARTPLAN_RUNS_LINE_FMT "    %s"
#define E_STARTPLAN_THEN_FMT     "Then runs: %s"
#define E_STARTPLAN_CHOSE_FMT    "Program chosen by %s: %s"
#define E_STARTPLAN_HONEST       "This runs the folder's own program."
#define E_STARTPLAN_HONEST_MAKE  "This runs the folder's own Makefile."
#define E_STARTPLAN_LINES_MAX    (WPE_STARTPLAN_WORDS_MAX + 8)

/* The lines a plan adds below its command, as they are collected. */
typedef struct {
 char **line;
 int *keep_end;        /* E_STARTPLAN_KEEP_*: how the box cuts each line        */
 int n;
} e_startplan_lines;

/* How a line of the plan is cut when it is wider than the box. */
#define E_STARTPLAN_KEEP_START E_STARTCMD_EXTRA_START
#define E_STARTPLAN_KEEP_END   E_STARTCMD_EXTRA_END
#define E_STARTPLAN_KEEP_WHOLE E_STARTCMD_EXTRA_WHOLE

/**
 * e_startplan_line_add - Append one line of the plan's part of the box.
 * @l: the lines; @keep_end: E_STARTPLAN_KEEP_*; @fmt: its format, and its
 * arguments.
 *
 * Each line also goes to the trace whole, so a test reads a long path the
 * box had to cut.
 */
static void e_startplan_line_add(e_startplan_lines *l, int keep_end, const char *fmt, ...)
{
 char text[E_STARTCMD_LINE_MAX];
 char **grown;
 int *grown_end;
 va_list ap;

 if (l->n >= E_STARTPLAN_LINES_MAX)
  return;
 va_start(ap, fmt);
 vsnprintf(text, sizeof text, fmt, ap);
 va_end(ap);
 if (!(grown = REALLOC(l->line, (l->n + 1) * sizeof(char *))))
  return;
 l->line = grown;
 if (!(grown_end = REALLOC(l->keep_end, (l->n + 1) * sizeof(int))))
  return;
 l->keep_end = grown_end;
 if ((l->line[l->n] = WpeStrdup(text)) == NULL)
  return;
 l->keep_end[l->n] = keep_end;
 l->n++;
 wpe_ui_trace("startplan line %s", text);
}

/** e_startplan_lines_free - Free the plan's lines. */
static void e_startplan_lines_free(e_startplan_lines *l)
{
 int i;

 for (i = 0; i < l->n; i++)
  FREE(l->line[i]);
 if (l->line)
  FREE(l->line);
 if (l->keep_end)
  FREE(l->keep_end);
 l->line = NULL;
 l->keep_end = NULL;
 l->n = 0;
}

/**
 * e_startplan_lines_values - "From <file>: KEY = value" for each value a
 * folder file set, its expansion beside it.
 */
static void e_startplan_lines_values(e_startplan_lines *l, const WpeStartPlan *p)
{
 int i;

 for (i = 0; i < p->nvalue; i++)
 {
  const WpeStartValue *v = &p->value[i];
  if (v->expanded)
   e_startplan_line_add(l, E_STARTPLAN_KEEP_START, E_STARTPLAN_FROM_EXP_FMT, v->file, v->key,
                        v->raw, v->expanded);
  else
   e_startplan_line_add(l, E_STARTPLAN_KEEP_START, E_STARTPLAN_FROM_FMT, v->file, v->key, v->raw);
 }
}

/**
 * e_startplan_lines_outside - Each path a folder value puts outside its
 * folder, under the heading of what the start does with it.
 * @l: the lines; @p: the plan; @role: WPE_PATH_WRITE or WPE_PATH_INPUT;
 * @heading: the line above them.
 */
static void e_startplan_lines_outside(e_startplan_lines *l, const WpeStartPlan *p, int role,
                                      const char *heading)
{
 int i, said = 0;

 for (i = 0; i < p->npath; i++)
 {
  const WpeStartPath *s = &p->path[i];
  if (s->role != role || s->from != WPE_FROM_FOLDER || s->inside)
   continue;
  if (!said)
   e_startplan_line_add(l, E_STARTPLAN_KEEP_START, "%s", heading);
  said = 1;
  e_startplan_line_add(l, E_STARTPLAN_KEEP_END, E_STARTPLAN_PATH_FMT, s->resolved);
 }
}

/** e_startplan_lines_dropped - A line for each removal outside, not done. */
static void e_startplan_lines_dropped(e_startplan_lines *l, const WpeStartPlan *p)
{
 int i;

 for (i = 0; i < p->npath; i++)
  if (p->path[i].dropped)
   e_startplan_line_add(l, E_STARTPLAN_KEEP_END, E_STARTPLAN_DROPPED_FMT, p->path[i].resolved);
}

/**
 * e_startplan_text_lines - How many lines a start-up file's text has.
 * @text: the text; a last line without its line end counts.
 */
static int e_startplan_text_lines(const char *text)
{
 int n = 0;

 for (; text && *text; text++)
  if (*text == '\n' || !text[1])
   n++;
 return(n);
}

/**
 * e_startplan_lines_file_text - A start-up file's lines, each indented,
 * every control byte written out when the box shows it.
 * @l: the lines; @text: the file's text.
 *
 * Each line must be read whole: one wider than the box (a harmless command,
 * blanks, then another one past the edge) makes the box withhold Run it.
 * The box is never wider than a line can be kept here.
 */
static void e_startplan_lines_file_text(e_startplan_lines *l, const char *text)
{
 char line[E_STARTCMD_LINE_MAX];
 const char *end;
 size_t len;

 while (text && *text)
 {
  end = strchr(text, '\n');
  len = end ? (size_t)(end - text) : strlen(text);
  if (len >= sizeof line)
   len = sizeof line - 1;
  memcpy(line, text, len);
  line[len] = '\0';
  e_startplan_line_add(l, E_STARTPLAN_KEEP_WHOLE, E_STARTPLAN_RUNS_LINE_FMT, line);
  if (!end)
   break;
  text = end + 1;
 }
}

/**
 * e_startplan_lines_runs - Each file the tool runs from the folder at its
 * start (./.pdbrc, ./jdb.ini, ./.gdbinit), with its lines.
 * @l: the lines; @p: the plan.
 *
 * Starting a debugger in a cloned folder used to run whatever commands the
 * folder's start-up file held, unseen: the user now reads them first, and
 * the condition under which the tool runs one when it does not always.
 */
static void e_startplan_lines_runs(e_startplan_lines *l, const WpeStartPlan *p)
{
 const char *fmt = E_STARTPLAN_RUNS_FMT, *long_fmt = E_STARTPLAN_RUNS_LONG_FMT;
 int i, n;

 if (wpe_startplan_is_apart(p))
 {
  fmt = E_STARTPLAN_WITH_RUN_FMT;
  long_fmt = E_STARTPLAN_WITH_RUN_LONG_FMT;
 }
 for (i = 0; i < p->npath; i++)
 {
  const WpeStartPath *s = &p->path[i];
  if (s->role != WPE_PATH_READS)
   continue;
  n = e_startplan_text_lines(s->content);
  if (s->too_long)
   e_startplan_line_add(l, E_STARTPLAN_KEEP_START, long_fmt, s->path, s->length);
  else if (n == 1)
   e_startplan_line_add(l, E_STARTPLAN_KEEP_START, fmt, s->path, n, "");
  else
   e_startplan_line_add(l, E_STARTPLAN_KEEP_START, fmt, s->path, n, "s");
  if (s->note)
   e_startplan_line_add(l, E_STARTPLAN_KEEP_START, "  %s", s->note);
  e_startplan_lines_file_text(l, s->content);
 }
}

/**
 * e_startplan_lines_input - What the editor will write to the tool once it
 * runs, built from a value ("Then runs: r --verbose").
 * @l: the lines; @p: the plan.
 */
static void e_startplan_lines_input(e_startplan_lines *l, const WpeStartPlan *p)
{
 int i;

 for (i = 0; i < p->ninput; i++)
  if (p->input[i].from != WPE_FROM_EDITOR)
   e_startplan_line_add(l, E_STARTPLAN_KEEP_START, E_STARTPLAN_THEN_FMT, p->input[i].text);
}

/**
 * e_startplan_lines_debugs - The program a debugger that starts apart is
 * told to load, in a line of its own.
 * @l: the lines; @p: the plan.
 *
 * It is no word of the command (the debugger starts with none), so the box
 * names it here: the user reads which program the session runs before he
 * says Run it.
 */
static void e_startplan_lines_debugs(e_startplan_lines *l, const WpeStartPlan *p)
{
 const char *program = wpe_startplan_program_path(p);

 if (program && wpe_startplan_is_apart(p))
  e_startplan_line_add(l, E_STARTPLAN_KEEP_END, E_STARTPLAN_DEBUGS_FMT, program);
}

/**
 * e_startplan_honest_line - What the box says a start runs of the folder's
 * own: its Makefile for Execute Make, its program for every other.
 * @p: the plan.
 */
static const char *e_startplan_honest_line(const WpeStartPlan *p)
{
 if (p->kind == WPE_START_MAKE)
  return(E_STARTPLAN_HONEST_MAKE);
 return(E_STARTPLAN_HONEST);
}

/**
 * e_startplan_line_chose - Say which keys of the folder's file chose the
 * program.
 * @l: the lines; @p: the plan.
 *
 * The program lies inside the folder, so it is the keys that tell the user
 * why he is asked: "Program chosen by ./xwperc: CompilerSwitch = 1".
 */
static void e_startplan_line_chose(e_startplan_lines *l, const WpeStartPlan *p)
{
 const char *file = E_STARTCMD_XWPERC;

 if (p->file)
  file = p->file;
 e_startplan_line_add(l, E_STARTPLAN_KEEP_START, E_STARTPLAN_CHOSE_FMT, file, p->chose);
}

/**
 * e_startplan_lines_of - Everything the box says about a plan below its command.
 * @l: receives the lines; @p: the plan, judged.
 *
 * The values a folder set and where they came from, what the start writes
 * and reads outside the folder, what it will not remove, how many of the
 * folder's own files it builds, the files the tool runs from the folder at
 * its start and what the editor then writes to it: the user sees the whole
 * start, not only the program's name.
 */
static void e_startplan_lines_of(e_startplan_lines *l, const WpeStartPlan *p)
{
 int outside, dropped, inside;

 e_startplan_lines_values(l, p);
 e_startplan_lines_outside(l, p, WPE_PATH_WRITE, E_STARTPLAN_WRITES);
 e_startplan_lines_outside(l, p, WPE_PATH_INPUT, E_STARTPLAN_READS);
 e_startplan_lines_dropped(l, p);
 wpe_startplan_counts(p, &outside, &dropped, &inside);
 if (inside > 0 && p->file)
  e_startplan_line_add(l, E_STARTPLAN_KEEP_START, E_STARTPLAN_INSIDE_FMT, inside, p->file);
 e_startplan_lines_debugs(l, p);
 e_startplan_lines_runs(l, p);
 if (p->load_refused)
  e_startplan_line_add(l, E_STARTPLAN_KEEP_START, E_STARTPLAN_NO_RUN_IT_FMT, p->load_refused);
 e_startplan_lines_input(l, p);
 if (p->runs_code)
  e_startplan_line_add(l, E_STARTPLAN_KEEP_START, "%s", p->runs_code);
 if (p->chose)
  e_startplan_line_chose(l, p);
 if (p->honest)
  e_startplan_line_add(l, E_STARTPLAN_KEEP_START, "%s", e_startplan_honest_line(p));
}

/**
 * e_startplan_cmd_of - The P0b view of a plan, for the status lines.
 * @c: filled in; @p: the plan; @show: what the site says about it;
 * @key: the plan's key.
 */
static void e_startplan_cmd_of(E_STARTCMD *c, const WpeStartPlan *p,
                               const E_STARTPLAN_SHOW *show, const char *key)
{
 c->folder = p->folder;
 c->source = p->file ? p->file : "";
 c->who = show->who;
 c->key = key;
 c->what = show->what;
 c->text = show->shown;
 c->own = show->own;
}

/**
 * e_startplan_ask - Show a plan and ask whether to run it.
 * @f: the window; @p: the plan, judged; @show: its command and the user's
 * own; @key: the plan's key; @heading: the sentence above the command, or
 * NULL for "<who> sets the <what> to:".
 * Return: WPE_STARTCMD_RUN, WPE_STARTCMD_USE_OWN or WPE_STARTCMD_CANCEL.
 */
static int e_startplan_ask(FENSTER *f, WpeStartPlan *p, const E_STARTPLAN_SHOW *show,
                           const char *key, const char *heading)
{
 e_startplan_lines l = { NULL, NULL, 0 };
 e_startcmd_view v;
 char head[E_STARTCMD_LINE_MAX];
 int ret;

 e_startplan_lines_of(&l, p);
 if (heading)
  snprintf(head, sizeof head, "%s", heading);
 else
  snprintf(head, sizeof head, E_STARTCMD_HEAD_FMT, show->who, show->what);
 memset(&v, 0, sizeof v);
 v.source = p->file ? p->file : "";
 v.head = head;
 v.text = show->shown;
 v.own = show->own;
 v.folder = p->folder ? p->folder : "";
 v.key = key;
 v.extra = l.line;
 v.extra_end = l.keep_end;
 v.nextra = l.n;
 v.offer_mine = wpe_startplan_own_usable(p);
 v.withhold_run = wpe_startplan_reads_too_long(p);
 ret = e_startcmd_show(f, &v);
 e_startplan_lines_free(&l);
 return(ret);
}

/**
 * e_startplan_say_refused - A plan a rule refused: nothing runs, and why.
 */
static void e_startplan_say_refused(FENSTER *f, const WpeStartPlan *p,
                                    const E_STARTPLAN_SHOW *show)
{
 char line[E_STARTCMD_STATUS_MAX];
 const char *file = "", *why = "";

 if (p->file)
  file = p->file;
 if (p->refused)
  why = p->refused;
 snprintf(line, sizeof line, E_STARTPLAN_REFUSED_FMT, file, show->what, why);
 e_startcmd_say(f, line);
}

/**
 * e_startplan_approve - Judge the plan of a user action, and ask when a
 * folder decided something the user has not seen.
 * @f: the window the key was pressed in; @plan: the action's plan, whole;
 * @show: the folder's command line and the user's own, as the box shows them.
 *
 * Called once per action (F9, Alt-C, a project build) before its first
 * child starts.  A plan in which no folder chose anything runs; the user's
 * own plan, or one he said yes to in this folder, runs; one he answered
 * "Use mine" for starts his own; any other is shown, with every value a
 * folder file set, every path it puts outside the folder and every removal
 * that will not be done, and the answer is kept for that folder and that
 * very plan.  A plan a rule refused runs nothing.
 * Return: WPE_STARTCMD_RUN, WPE_STARTCMD_USE_OWN or WPE_STARTCMD_CANCEL;
 * the plan's verdict says the same for wpe_startplan_start.
 */
int e_startplan_approve(FENSTER *f, struct WpeStartPlan *plan, const E_STARTPLAN_SHOW *show)
{
 return(e_startplan_approve_headed(f, plan, show, NULL));
}

/**
 * e_startplan_approve_headed - e_startplan_approve, with the box's own
 * sentence above the command.
 * @f/@plan/@show: as e_startplan_approve; @heading: the sentence ("Opening
 * this file starts its language server:"), or NULL for "<who> sets the
 * <what> to:", which fits a value a folder file set.
 * Return: as e_startplan_approve.
 */
int e_startplan_approve_headed(FENSTER *f, struct WpeStartPlan *plan,
                               const E_STARTPLAN_SHOW *show, const char *heading)
{
 char key[WPE_STARTPLAN_KEY_MAX];
 E_STARTCMD c;
 int verdict;

 if (!plan || !show || wpe_startplan_key(plan, key, sizeof key) < 0)
  return(WPE_STARTCMD_CANCEL);
 verdict = wpe_startplan_judge(plan);
 e_startplan_cmd_of(&c, plan, show, key);
 if (verdict == WPE_PLAN_REFUSED)
 {
  e_startplan_say_refused(f, plan, show);
  return(WPE_STARTCMD_CANCEL);
 }
 if (verdict == WPE_PLAN_CLEAN)
  return(WPE_STARTCMD_RUN);
 if (verdict == WPE_PLAN_RUN)
 {
  wpe_ui_trace("startcmd run %s", key);
  return(WPE_STARTCMD_RUN);
 }
 if (verdict == WPE_PLAN_USE_OWN)
 {
  wpe_ui_trace("startcmd remembered %s own", key);
  e_startcmd_say_answer(f, &c, WPE_STARTCMD_USE_OWN, 1);
  return(WPE_STARTCMD_USE_OWN);
 }
 e_startcmd_say_store_bad(f);
 verdict = e_startplan_ask(f, plan, show, key, heading);
 wpe_ui_trace("startcmd answer %s %s", key, e_startcmd_answer_word(verdict));
 if (verdict == WPE_STARTCMD_CANCEL)
 {
  plan->verdict = WPE_PLAN_CANCEL;
  e_startcmd_say_answer(f, &c, verdict, 0);
  return(verdict);
 }
 if (verdict == WPE_STARTCMD_RUN)
  plan->verdict = WPE_PLAN_RUN;
 else
  plan->verdict = WPE_PLAN_USE_OWN;
 e_startcmd_say_answer(f, &c, verdict, wpe_startplan_remember(plan, plan->verdict) == 0);
 return(verdict);
}

/**
 * e_startplan_say_not_removed - Say that a build output outside the folder
 * was left in place.
 * @f: the window; @plan: the plan; @path: the output, as the build names it.
 *
 * The original removed a stale object wherever Exedir pointed before it
 * compiled; a removal outside the folder is no longer done, and the status
 * row says so instead of leaving the user to wonder.
 */
void e_startplan_say_not_removed(FENSTER *f, struct WpeStartPlan *plan, const char *path)
{
 char line[E_STARTCMD_STATUS_MAX];
 const char *shown = path;
 int i;

 for (i = 0; plan && i < plan->npath; i++)
  if (plan->path[i].role == WPE_PATH_REMOVE && strcmp(plan->path[i].path, path) == 0)
   shown = plan->path[i].resolved;
 snprintf(line, sizeof line, E_STARTPLAN_DROPPED_FMT, shown);
 e_startcmd_say(f, line);
}

/**
 * e_build_record_say - Say once that the record of what was built could not
 * be read or written.
 * @f: the window; @what: E_BUILT_NOT_READ or E_BUILT_NOT_SAVED.
 *
 * Without the record nothing counts as built here, so every F9 builds; the
 * build itself works, and the status row says why it never says "nothing to
 * build".
 */
void e_build_record_say(FENSTER *f, int what)
{
 /* Each failure is said once a session. */
 static const char not_read[] = "build record not read: the next F9 builds everything again";
 static const char not_saved[] = "build record not saved: the next F9 builds everything again";
 static int said[E_BUILT_NOT_SAVED + 1];

 if (what < E_BUILT_NOT_READ || what > E_BUILT_NOT_SAVED || said[what])
  return;
 said[what] = 1;
 if (what == E_BUILT_NOT_READ)
  e_startcmd_say(f, not_read);
 else
  e_startcmd_say(f, not_saved);
}

/*         First opening of a window                 */
void e_firstl(FENSTER *f, int sw)
{
 /* Pass the window's existing view (f->pic) into e_ed_kst: when it is
    non-NULL (a repaint -- e.g. e_rep_win_tree during a debug step)
    e_change_pic closes it before opening the new one. Discarding it here
    (f->pic = NULL) leaked a full-screen backing buffer (~19 KB) on every
    repaint -- the 24-year "debugging leaks memory" bug, since a debug step
    repaints the whole window tree repeatedly. New windows initialise
    f->pic = NULL at creation, so the close is skipped for them. */
 f->pic = e_ed_kst(f, f->pic, sw);
 if (f->pic == NULL)
  e_error(e_msg[ERR_LOWMEM], 1, f->fb);
}

/*         Writing of the file type    */
int e_pr_filetype(FENSTER *f)
{
 int frb = f->fb->es.fb;

 e_pr_char(f->a.x+2, f->e.y, 'A', frb);
 if (f->ins == 0 || f->ins == 2)
  e_pr_char(f->a.x+16, f->e.y, 'O', frb);
 else if (f->ins == 8)
  e_pr_char(f->a.x+16, f->e.y, 'R', frb);
 else
  e_pr_char(f->a.x+16, f->e.y, 'I', frb);
 if (f->ins > 1)
  e_pr_char(f->a.x+17, f->e.y, 'S', frb);
 else
  e_pr_char(f->a.x+17, f->e.y, 'L', frb);
 return(0);
}

/*   open section of screen and save background  */
PIC *e_open_view(int xa, int ya, int xe, int ye, int col, int sw)
{
 PIC *pic = MALLOC(sizeof(PIC));
 int i, j;
 int w = xe - xa + 1;
 int h = ye - ya + 1;

 if (pic == NULL) return(NULL);
 wpe_layout_frame_outside(xa, ya, xe, ye, sw);
 pic->a.x = xa;
 pic->a.y = ya;
 pic->e.x = xe;
 pic->e.y = ye;
 pic->generation = wpe_layout_generation();

 /* Save schirm content behind this popup using calloc'd SCREENCELL array.
    This is the same approach as the original code but with proper
    initialisation (calloc) to prevent uninitialised data issues. */
 pic->buf = NULL;
 
 if (sw != 0)
 {
  int pw = xe - xa + 1;
  int ph = ye - ya + 1;
  int sy = ye < MAXSLNS ? ye : MAXSLNS - 1;
  int sx = xe < MAXSCOL ? xe : MAXSCOL - 1;
  SCREENCELL *buf = calloc(pw * ph, sizeof(SCREENCELL));
  if (buf == NULL) { FREE(pic); return(NULL); }
  pic->buf = (WINDOW *)buf;
  for (j = ya; j <= sy; ++j)
   for (i = xa; i <= sx; ++i)
    buf[(j - ya) * pw + (i - xa)] = schirm[j * MAXSCOL + i];
 }

 if (sw < 2)
 {
  for (j = ya; j <= ye; ++j)
   for (i = xa; i <= xe; ++i)
    e_pr_char(i, j, ' ', col);
 }

#ifndef NO_XWINDOWS
 if (WpeIsXwin()) (*e_u_setlastpic)(pic);
#endif
 return(pic);
}

/**
 * e_close_view - Close a screen section and put its background back.
 * @pic: the view e_open_view made.
 * @sw: 0 to just free it; non-zero to restore the saved background.
 *
 * A background saved before the screen took its present size describes a
 * screen that is gone: put back, it would paint the old status row or old
 * text over the new desk.  So it is restored only when it was saved at the
 * current size (see wpe_layout_generation); otherwise the view is freed as
 * with @sw == 0 and the trace says so.  This is why a command that holds
 * the screen across a resize leaves the editor whole afterwards.
 */
int e_close_view(PIC *pic, int sw)
{
 int i, j;
 int answer = sw;
#ifndef NO_XWINDOWS
 if (WpeIsXwin()) (*e_u_setlastpic)(NULL);
#endif
 if (pic == NULL) return(-1);
 if (sw != 0 && pic->generation != wpe_layout_generation())
 {
  wpe_ui_trace("layout dropped a stale background %d,%d-%d,%d",
               pic->a.x, pic->a.y, pic->e.x, pic->e.y);
  sw = 0;
 }

 if (sw != 0 && pic->buf)
 {
  int pw = pic->e.x - pic->a.x + 1;
  int ey = pic->e.y < MAXSLNS ? pic->e.y : MAXSLNS - 1;
  int ex = pic->e.x < MAXSCOL ? pic->e.x : MAXSCOL - 1;
  SCREENCELL *buf = (SCREENCELL *)pic->buf;
  for (j = pic->a.y; j <= ey; ++j)
   for (i = pic->a.x; i <= ex; ++i)
    schirm[j * MAXSCOL + i] = buf[(j - pic->a.y) * pw + (i - pic->a.x)];
 }
 else if (sw != 0)
 {
  int ey = pic->e.y < MAXSLNS ? pic->e.y : MAXSLNS - 1;
  int ex = pic->e.x < MAXSCOL ? pic->e.x : MAXSCOL - 1;
  for (j = pic->a.y; j <= ey; ++j)
   for (i = pic->a.x; i <= ex; ++i)
    e_pr_char(i, j, ' ', 0);
 }

 { int ixe = pic->e.x < MAXSCOL - 1 ? pic->e.x : MAXSCOL - 1;
   int iye = pic->e.y < MAXSLNS - 1 ? pic->e.y : MAXSLNS - 1;
   e_invalidate_area(pic->a.x, pic->a.y, ixe, iye);
 }

 /* Always free the view: e_open_view() always allocates the PIC struct (and
    its saved-background buffer when sw != 0). The old `sw < 2` guard left the
    PIC + buffer allocated for sw == 2 callers (e_change_pic's repaint path,
    option-dialog probes), leaking a backing buffer on every window-tree
    repaint -- the bulk of the "debugging leaks memory" bug, since a debug
    step repaints repeatedly. No caller reads the PIC after closing it (they
    set it to NULL or let a local go out of scope). */
 if (pic->buf) free((SCREENCELL *)pic->buf);
 pic->buf = NULL;
 FREE(pic);

 e_refresh();
 return(answer);
}

/** Free a window's off-screen backing view and clear the caller's pointer.
 *
 *  Usability note: the window-arrangement commands (Window > Cascade,
 *  Window > Tile) and Alt-<n> window switching drop every window's
 *  saved-background view and then repaint the whole tree from scratch. The
 *  repaint (e_firstl -> e_change_pic) re-opens a fresh view for each window,
 *  so the old one has to be released first. Always NULLing the pointer here
 *  is what keeps that safe: e_change_pic only closes a non-NULL pic, so a
 *  dangling pointer would be freed a second time and abort the program with
 *  "free(): invalid pointer". Routing every such release through this one
 *  function makes that entire class of double-free impossible by
 *  construction -- callers can no longer forget the "= NULL". */
void e_free_view(PIC **pp)
{
 PIC *pic = *pp;
 if (pic == NULL) return;
 if (pic->buf) free((SCREENCELL *)pic->buf);
 FREE(pic);
 *pp = NULL;
}

/**
 * e_window_text_locked - May the user change this window's text right now?
 * @f: the window about to receive an inserting, deleting or replacing key.
 * Return: 1 when the key must be refused, 0 when the window is editable.
 *
 * The editor has always had windows whose text is closed to editing: the Help
 * viewer, the clipboard viewer, a read-only file and the tool/output panes all
 * carry WPE_INS_READONLY.  A window waiting for an assistant reply is closed
 * for the same reason -- its lines are about to be written back from a copy
 * taken when the request left -- so it answers the same question, and the user
 * is told why on the status row instead of the key disappearing in silence.
 *
 * Every text-changing path asks this one predicate, so the three backends and
 * every route to the buffer (typing, the block operations, paste) agree.
 */
int e_window_text_locked(FENSTER *f)
{
 if (!f)
  return(0);
 if (f->ins == WPE_INS_READONLY)
  return(1);
#ifdef WPE_AI
 if (wpe_ai_window_locked(f))
 {
  wpe_ai_report_window_locked(f);
  return(1);
 }
#endif
 return(0);
}

/* e_win_backs_file - True only when the window's title names a real file on
   disk -- the one thing the read-only padlock should mark.  The synthetic
   tool/output panes (Messages, Watches, Stack, "Metals Doctor", and any other
   e_d_p_named output window) reuse the read-only flag (ins == 8) to mean "not
   editable", but they back no file -- so checking the file exists keeps the
   padlock honest without enumerating every pane name (a new pane would
   otherwise sprout a spurious lock, as "Metals Doctor" did).  A locked library
   source (.metals/readonly, /usr/include header, a 0444 file) DOES exist, so it
   still earns its lock.  (Help is excluded separately by its DTMD_HELP.) */
static int e_win_backs_file(FENSTER *f)
{
 char *full;
 int ok;

 if (!f->datnam || !f->datnam[0])
  return(0);
 full = e_mkfilename(f->dirct, f->datnam);
 ok = (full && access(full, F_OK) == 0);
 if (full)
  FREE(full);
 return(ok);
}

/* Title-bar markers (Unicode; e_t_chrome_ascii has the non-UTF stand-ins).  The
   tool marker is a BMP, width-1, monochrome glyph on purpose: a wrench/tool
   emoji (0x1F527) is a wide colour SMP glyph that renders blank on some
   terminals (kitty) and tiny/misaligned in the Xft cell, whereas U+2699 paints
   crisply from the normal monospace font everywhere. */
#define WIN_GLYPH_LOCK 0x1F512   /* padlock: a locked file you cannot edit; the  */
                                 /* graphical backends draw this as a vector --  */
                                 /* keep in sync with WR_GLYPH_LOCK (we_render.h) */
#define WIN_GLYPH_TOOL 0x2699    /* gear: a tool/output pane, not a document     */

/* The editability nature of a text window, which decides its title-bar marker. */
typedef enum { WIN_DOCUMENT, WIN_LOCKED_FILE, WIN_TOOL_PANE } e_win_kind;

/* e_win_kind_of - Classify a text window for its title-bar marker:
     WIN_LOCKED_FILE  a read-only file on disk (0444, an extracted library
                      source) -- gets the padlock and a dimmed name ("you would
                      edit this file, but it is locked").
     WIN_TOOL_PANE    a synthetic tool/output pane (Messages, Watches, Stack,
                      "Metals Doctor", ...: ins == 8 but backing no file) -- gets
                      the gear ("a tool pane, not your document").  NOT dimmed
                      and NOT "read-only": Messages also carries a running
                      program's stdin.
     WIN_DOCUMENT     a normal editable document (or Help) -- no marker. */
static e_win_kind e_win_kind_of(FENSTER *f)
{
 if (f->ins != 8 || f->dtmd == DTMD_HELP)
  return(WIN_DOCUMENT);
 if (f->ed && f == f->ed->f[0])   /* the clipboard / Show Buffer viewer: */
  return(WIN_LOCKED_FILE);        /* read-only, so it wears the padlock */
 if (e_win_backs_file(f))
  return(WIN_LOCKED_FILE);
 if (f->datnam && f->datnam[0])
  return(WIN_TOOL_PANE);
 return(WIN_DOCUMENT);
}

/*    Frame for edit window   */
void e_ed_rahmen(FENSTER *f, int sw)
{
 extern char *e_hlp;
 extern int nblst;
 extern WOPT *blst;
 char *header = NULL;

 if (!DTMD_ISTEXT(f->dtmd))
 {
  if (f->datnam[0]) header = f->datnam;
  if (f->dtmd == DTMD_FILEDROPDOWN)
   e_std_rahmen(f->a.x, f->a.y, f->e.x, f->e.y, header, sw, f->fb->er.fb,
     f->fb->es.fb);
  else
   e_std_rahmen(f->a.x, f->a.y, f->e.x, f->e.y, header, sw, f->fb->nr.fb,
     f->fb->ne.fb);
  if (f->winnum < 10 && f->winnum >= 0)
   e_pr_char(f->e.x-7, f->a.y, '0' + f->winnum, f->fb->nr.fb);
  else if (f->winnum >= 0)
   e_pr_char(f->e.x-7, f->a.y, 'A' - 10 + f->winnum, f->fb->nr.fb);
  if (sw > 0 && (f->dtmd == DTMD_FILEMANAGER || f->dtmd == DTMD_DATA))
  {
   /* Modal pickers (project list, open-project, add-to-project) have no
      maximize/close button. */
   if (f->dtmd == DTMD_DATA ||
       (f->dtmd == DTMD_FILEMANAGER &&
        (((FLBFFR *)f->b)->prj_sel || ((FLBFFR *)f->b)->sw == 5)))
    e_clear_titlebar_buttons(f);
   else
    /* File-manager dialogs (Open / Save As / search) are fixed-size: a close
       box only, no maximize, like a Borland TDialog. */
    e_draw_dialog_close_button(f);
   blst = f->blst;
   nblst = f->nblst;
   e_hlp = f->hlp_str;
   e_pr_uul(f->fb);
  }
  return;
 }
 if (f->datnam[0])
 {
  if (strcmp(f->dirct, f->ed->dirct) == 0 ||
    f->dtmd == DTMD_HELP || strcmp(f->datnam, BUFFER_NAME) == 0 ||
    NUM_COLS_ON_SCREEN < 40)
  {
   header = (char *)WpeMalloc(strlen(f->datnam) + 1);
   strcpy(header, f->datnam);
  }
  else
  {
   header = (char *)WpeMalloc(strlen(f->dirct) + strlen(f->datnam) + 1);
   strcpy(header, f->dirct);
   strcat(header, f->datnam);
  }
 }
 {
  /* Mark a non-editable window at the left of its title bar (see e_win_kind_of):
     a locked file gets the padlock and a dimmed name; a tool/output pane gets
     the wrench. */
  e_win_kind kind = e_win_kind_of(f);
  if (kind == WIN_LOCKED_FILE)
   g_rahmen_hdr_frb = f->fb->es.fb;       /* dim the locked-file title */
  e_std_rahmen(f->a.x, f->a.y, f->e.x, f->e.y, header, sw, f->fb->er.fb,
    f->fb->es.fb);
  g_rahmen_hdr_frb = -1;
  if (f->e.x - f->a.x > 8)
  {
   /* The padlock / tool glyphs are wide emoji (wcwidth 2).  Draw them as a
      wide cell so the next title-frame character becomes their spacer instead
      of over-painting the glyph's right half (the "half padlock" under the
      Cairo/Pango renderers, which draw the emoji its true two cells wide). */
   int gl = (kind == WIN_LOCKED_FILE) ? WIN_GLYPH_LOCK
          : (kind == WIN_TOOL_PANE)   ? WIN_GLYPH_TOOL : 0;
   if (gl)
   {
    int cw = wcwidth(gl);
    e_put_wide_cell(f->a.x + 2, f->a.y, gl, cw > 0 ? cw : 1, f->fb->er.fb, 1);
   }
  }
 }
 if (header)
  WpeFree(header);
 if (sw > 0)
 {
  e_mouse_bar(f->e.x, f->a.y+1, NUM_LINES_ON_SCREEN - 1, 0, f->fb->em.fb);
  e_mouse_bar(f->a.x+19, f->e.y, NUM_COLS_ON_SCREEN - 20, 1, f->fb->em.fb);
  e_draw_window_buttons(f);
  e_pr_filetype(f);
  e_zlsplt(f);
  e_lsp_bar_label(f);          /* name the LSP bar after the active server */
  blst = f->blst;
  nblst = f->nblst;
  e_hlp = f->hlp_str;
  e_pr_uul(f->fb);
 }
 if (f->winnum < 10 && f->winnum >= 0)
  e_pr_char(f->e.x-7, f->a.y, '0' + f->winnum, f->fb->er.fb);
 else if (f->winnum >= 0)
  e_pr_char(f->e.x-7, f->a.y, 'A' - 10 + f->winnum, f->fb->er.fb);
}

static void e_restore_pic_to_schirm(PIC *pic)
{
 int i, j;
 int pw, ey, ex;
 SCREENCELL *buf;

 if (pic == NULL || pic->buf == NULL)
  return;
 pw = pic->e.x - pic->a.x + 1;
 ey = pic->e.y < MAXSLNS ? pic->e.y : MAXSLNS - 1;
 ex = pic->e.x < MAXSCOL ? pic->e.x : MAXSCOL - 1;
 buf = (SCREENCELL *)pic->buf;
 for (j = pic->a.y; j <= ey; j++)
  for (i = pic->a.x; i <= ex; i++)
   schirm[j * MAXSCOL + i] = buf[(j - pic->a.y) * pw + (i - pic->a.x)];
}

static void e_render_window_content(FENSTER *f)
{
 int j;

 if (f->dtmd == DTMD_FILEMANAGER)
 {
  WpeDrawFileManager(f);
  return;
 }
 if (f->dtmd == DTMD_DATA)
 {
  e_data_schirm(f);
  return;
 }
 if (f->dtmd == DTMD_FILEDROPDOWN)
 {
  e_pr_file_window((FLWND*)f->b, 1, 0, f->fb->er.fb, f->fb->ez.fb,
    f->fb->frft.fb);
  return;
 }
 if (NUM_LINES_OFF_SCREEN_TOP < 0)
  NUM_LINES_OFF_SCREEN_TOP = 0;
#ifdef PROG
 if (f->c_sw)
  for (j = NUM_LINES_OFF_SCREEN_TOP;
       j < f->b->mxlines && j < LINE_NUM_ON_SCREEN_BOTTOM; j++)
   e_pr_c_line(j, f);
 else
#endif
  for (j = NUM_LINES_OFF_SCREEN_TOP;
       j < f->b->mxlines && j < LINE_NUM_ON_SCREEN_BOTTOM; j++)
   e_pr_line(j, f);
 for (; j < LINE_NUM_ON_SCREEN_BOTTOM; j++)
  e_blk(NUM_COLS_ON_SCREEN - 1, f->a.x + 1,
    j - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1, f->fb->et.fb);
}

void e_move_window_recompose(FENSTER *f)
{
 PIC *old_pic = f->pic;
 int oax, oay, oex, oey;

 if (old_pic == NULL)
  return;
 oax = old_pic->a.x;
 oay = old_pic->a.y;
 oex = old_pic->e.x;
 oey = old_pic->e.y;

 e_restore_pic_to_schirm(old_pic);
 e_refresh_area(oax, oay, oex - oax + 1, oey - oay + 1);

 if (old_pic->buf)
  free((SCREENCELL *)old_pic->buf);
 old_pic->buf = NULL;
 FREE(old_pic);

 f->pic = e_open_view(f->a.x, f->a.y, f->e.x, f->e.y, f->fb->er.fb, 2);
 if (f->pic == NULL)
  e_error(e_msg[ERR_LOWMEM], 1, f->fb);

 e_ed_rahmen(f, 0);
 e_render_window_content(f);
 e_refresh_area(f->a.x, f->a.y, f->e.x - f->a.x + 1, f->e.y - f->a.y + 1);
 e_cursor_pos_only(f);
 e_refresh();
}

/*   Output - screen content */
int e_schirm(FENSTER *f, int sw)
{
 int j;

 e_abs_refr();
 if (f->dtmd == DTMD_FILEMANAGER)
  return(WpeDrawFileManager(f));
 else if (f->dtmd == DTMD_DATA)
  return(e_data_schirm(f));
 else if (f->dtmd == DTMD_FILEDROPDOWN)
  return(e_pr_file_window((FLWND*)f->b, 1, sw, f->fb->er.fb, f->fb->ez.fb,
    f->fb->frft.fb));
#ifdef WPE_AI
 wpe_ai_pane_before_paint(f);   /* the AI pane's rows at its present width */
#endif
 if (NUM_LINES_OFF_SCREEN_TOP < 0)
  NUM_LINES_OFF_SCREEN_TOP = 0;

#ifdef PROG
 if (f->c_sw)
  for (j = NUM_LINES_OFF_SCREEN_TOP; j < f->b->mxlines && j < LINE_NUM_ON_SCREEN_BOTTOM ; j++ )
   e_pr_c_line(j, f);
 else
#endif
  for (j = NUM_LINES_OFF_SCREEN_TOP; j < f->b->mxlines && j < LINE_NUM_ON_SCREEN_BOTTOM ; j++ )
   e_pr_line(j, f);
 for (; j < LINE_NUM_ON_SCREEN_BOTTOM ; j++ )
  e_blk((NUM_COLS_ON_SCREEN - 1), f->a.x + 1, j - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1, f->fb->et.fb);
 return(j);
}

/**
 * e_size_move - Move and size the window with the cursor keys until Enter
 * or Esc.
 * @f: the window.
 *
 * While the user moves the frame, a stop or program output that comes is
 * shown once the window has its new place, not drawn under the moving frame.
 * Return: the key that ended it.
 */
int e_size_move(FENSTER *f)
{
 int xa = f->a.x, ya = f->a.y, xe = f->e.x, ye = f->e.y;
 int c = 0, xmin = 26, ymin = 3;

 e_ed_rahmen(f, 0);
 if (f->dtmd == DTMD_FILEDROPDOWN)
  xmin = 15;
 else if (!DTMD_ISTEXT(f->dtmd))
  ymin = 9;
 while ((c = wpe_paint_modal_getch()) != WPE_ESC && c != WPE_CR)
 {
  switch (c)
  {
   case CLE:
    if (xa > 0) {  xa--; xe--;  }
    break;
   case CRI:
    if (xe < MAXSCOL-1) {  xa++; xe++;  }
    break;
   case CUP:
    if (ya > 1) {  ya--; ye--;  }
    break;
   case CDO:
    if (ye < MAXSLNS-2) {  ya++; ye++;  }
    break;
   case SCLE:
   case CCLE:
    if ((xe - xa) > xmin) xe--;
    break;
   case SCRI:
   case CCRI:
    if (xe < MAXSCOL-1) xe++;
    break;
   case SCUP:
   case BUP:
    if ((ye - ya) > ymin) ye--;
    break;
   case SCDO:
   case BDO:
    if (ye < MAXSLNS-2) ye++;
    break;
  }
  if (xa != f->a.x || ya != f->a.y || xe != f->e.x || ye != f->e.y)
  {
   f->a.x = xa;
   f->a.y = ya;
   f->e.x = xe;
   f->e.y = ye;
   f->pic = e_ed_kst(f, f->pic, 0);
   if (f->pic == NULL)  e_error(e_msg[ERR_LOWMEM], 1, f->fb);
   if (f->dtmd == DTMD_FILEDROPDOWN)
   {
    FLWND *fw = (FLWND*) f->b;
    fw->xa = f->a.x+1; fw->xe = f->e.x;
    fw->ya = f->a.y+1; fw->ye = f->e.y;
   }
   e_cursor(f, 0);
   e_schirm(f, 0);
  }
 }
 /* Repaint the whole desktop, not just this window: moving/shrinking a window
    uncovers the area it vacated and any windows beneath it (e.g. the Messages
    and Watches windows during a debug session overlap).  Redrawing only this
    frame left stale borders and a half-erased neighbour. */
 e_repaint_desk_nopic(f);
 return(c);
}

/*       Standard Box                                  */
PIC *e_std_kst(int xa, int ya, int xe, int ye, char *name, int sw, int fr,
  int ft, int fes)
{
 PIC *pic = e_open_view(xa, ya, xe, ye, ft, 1);
 if (pic == NULL) return (NULL);
 e_std_rahmen(xa, ya, xe, ye, name, sw, fr, fes);
 return(pic);
}

PIC *e_change_pic(int xa, int ya, int xe, int ye, PIC *pic, int sw, int frb)
{
   int i, j;
   int box = 2, ax, ay, ex, ey;
   PIC *newpic;
   if(sw<0) {  sw = -sw; box = 1;  }
   if (pic == NULL)
   {  newpic = e_open_view(xa, ya, xe, ye, frb, box);
      if (newpic == NULL) return (NULL);
   }
   else
   {  /* With ncurses panels, always close old and open new.
         The panel library handles background save/restore automatically. */
      e_close_view(pic, box);
      newpic = e_open_view(xa, ya, xe, ye, frb, box);
      if (newpic == NULL) return (NULL);
   }
#ifndef NO_XWINDOWS
   if (WpeIsXwin()) (*e_u_setlastpic)(newpic);
#endif
   return(newpic);
}

PIC *e_ed_kst(FENSTER *f, PIC *pic, int sw)
{
   PIC *newpic = e_change_pic(f->a.x, f->a.y, f->e.x,
				f->e.y, pic, sw, f->fb->er.fb);
   e_ed_rahmen(f, sw);
   return(newpic);
}

/*    delete buffer     */
int e_close_buffer(BUFFER *b)
{
 int i;

 if (b != NULL)
 {
  e_buffer_history_drop(b);
  if (b->bf != NULL)
  {
   for (i = 0; i < b->mxlines; i++)
   {
    if (b->bf[i].s != NULL)
     FREE( b->bf[i].s );
    b->bf[i].s = NULL;
   }
   FREE(b->bf);
  }
  FREE(b);
 }
 return(0);
}

/**
 * e_close_save_failed - Save a window the user agreed to save before closing.
 * @f: the window being closed.
 *
 * When the write fails (a read-only folder, a full disk) the editor has
 * already shown the reason, and when the user answers No to the question
 * about NUL bytes the file was left as it was on purpose.  Either way the
 * text exists only in this window, so the caller must keep the window open
 * and the edits with it.
 *
 * Return: nonzero when the file was not written, 0 when it was.  A window
 * with nothing to write (a file manager) counts as written, so it closes.
 */
static int e_close_save_failed(FENSTER *f)
{
 return(e_save(f) != WPE_WRITE_WRITTEN);
}

/*    close window */
int e_close_window(FENSTER *f)
{
 ECNT *cn = f->ed;
 FENSTER *f0 = f->ed->f[0];
 int c = 0;
 long maxname;
 char text[256];

 f = cn->f[cn->mxedt];
 if (f->dtmd == DTMD_FILEMANAGER)
 {
  FLBFFR *b = (FLBFFR *)f->b;

  FREE(f->dirct);
  FREE(b->rdfile);
  freedf(b->df);  freedf(b->fw->df);
  freedf(b->dd);  freedf(b->cd);  freedf(b->dw->df);
  FREE(b->fw);
  FREE(b->dw);
  FREE(b);
  (cn->mxedt)--;
  cn->curedt = cn->edt[cn->mxedt];
  e_close_view(f->pic, 1);
  if (f != f0 && f != NULL)
  {
   e_free_find(&f->fd);
   FREE(f);
  }
  if (cn->mxedt > 0)
  {
   f = cn->f[cn->mxedt];
   e_ed_rahmen(f, 1);
   e_refresh();
  }
  return(0);
 }
 if (f->dtmd == DTMD_DATA)
 {
  FLWND *fw = (FLWND *)f->b;
  int swt = f->ins;

#ifdef PROG
  if (swt == 4 && f->save)
   e_p_update_prj_fl(f);
#endif
  if (f->dirct)
   FREE(f->dirct);
  if (swt == 7)
   freedf(fw->df);
  FREE(fw);
  (cn->mxedt)--;
  cn->curedt = cn->edt[cn->mxedt];
  e_close_view(f->pic, 1);
  if (f != f0 && f != NULL)
  {
   e_free_find(&f->fd);
   FREE(f);
  }
  if (cn->mxedt > 0 && (swt < 5 || swt == 7))
  {
   f = cn->f[cn->mxedt];
   e_ed_rahmen(f, 1);
   e_refresh();
  }
  return(0);
 }
 if (f == NULL || f->ed->mxedt <= 0)
  return(0);
 if (f != f0)
 {
  if (f->save != 0 && f->ins != 8)
  {
   snprintf(text, sizeof(text), "File %s NOT saved!\nDo you want to save File ?", f->datnam);
   c = e_message(1, text, f);
   if (c == WPE_ESC)
    return(c);
   if (c == 'Y' && e_close_save_failed(f))
    return(WPE_ESC);
  }
#ifdef WPE_AI
  /* The close is certain now and the window is still whole: a background AI
     task started from it must let go of it here, before its name, buffer and
     screen are freed below, or its next heartbeat paints through freed memory. */
  wpe_ai_window_closing(f);
#endif
#ifdef DEBUGGER
  /* The language server's news must not be drawn into the window after it
     is freed below. */
  e_lsp_window_closing(f);
#endif
  /* Check if file system could have an autosave or emergency save file
     >12 check is to eliminate dos file systems */
  if ((maxname = pathconf(f->dirct, _PC_NAME_MAX) >= strlen(f->datnam) + 4) &&
    (maxname > 12))
  {
   remove(e_make_postf(text, f->datnam, ".ASV"));
   remove(e_make_postf(text, f->datnam, ".ESV"));
  }
  if (strcmp(f->datnam, "Messages") && strcmp(f->datnam, "Watches"))
   e_close_buffer(f->b);
  else
   f->b->f = NULL;              /* kept for the next window: no window to keep in step */
  if (f->dtmd == DTMD_HELP && f->ins == 8)
   e_help_free(f);
  if (f->datnam != NULL)
   FREE(f->datnam);
  if (f->dirct != NULL)
   FREE(f->dirct);
  /* c_sw -- the per-line syntax-continuation state array (e_sc_txt), grown by
     the buffer's owner as the file is read.  It is freed and rebuilt on
     re-highlight (we_progn.c, we_fl_unix.c) but was never freed at window
     close, so each editor window leaked it. */
  if (f->c_sw != NULL)
  {
   FREE(f->c_sw);
   f->c_sw = NULL;
  }
  if (f && f->s != NULL)
  {
   /* s->brp -- the per-screen breakpoint-line array, MALLOCed in e_edit and
      grown by e_brk_schirm/e_breakpoint -- is owned by the SCHIRM and must be
      freed with it, else every window close (editor and Messages alike) leaks
      it (valgrind: definitely lost at e_brk_schirm we_debug.c:1748). */
   if (f->s->brp != NULL)
    FREE(f->s->brp);
   FREE(f->s);
  }
 }
 (cn->mxedt)--;
 cn->curedt = cn->edt[cn->mxedt];
 e_close_view(f->pic, 1);
 if (f != f0 && f != NULL)
 {
  e_free_find(&f->fd);
  FREE(f);
 }
 if (cn->mxedt > 0)
 {
  f = cn->f[cn->mxedt];
  e_ed_rahmen(f, 1);
  e_refresh();
 }
 return(c);
}

/*    Toggle among windows  */
int e_rep_win_tree(ECNT *cn)
{
 int i;

 if (cn->mxedt <= 0) return(0);
 ini_repaint(cn);
 for ( i = 1; i < cn->mxedt; i++)
 {
  e_firstl(cn->f[i], 0);
  e_schirm(cn->f[i], 0);
 }
 e_firstl(cn->f[i], 1);
 e_schirm(cn->f[i], 1);
 e_cursor(cn->f[i], 1);
 end_repaint();
 return(0);
}

void e_switch_window(int num, FENSTER *f)
{
 ECNT *cn = f->ed;
 FENSTER *ft;
 int n, i, te;

 for (n = 1; cn->edt[n] != num && n < cn->mxedt; n++)
  ;
 if (n >= cn->mxedt) return;
 for (i = cn->mxedt; i >= 1; i--)
 {
  e_free_view(&cn->f[i]->pic);
 }
 ft = cn->f[n];
 te = cn->edt[n];
 for ( i = n; i < cn->mxedt; i++)
 {
  cn->edt[i] = cn->edt[i+1];
  cn->f[i] = cn->f[i+1];
 }
 cn->f[i] = ft;
 cn->edt[i] = te;
 cn->curedt = num;
 e_rep_win_tree(cn);
}

/*    zoom windows   */
int e_ed_zoom(FENSTER *f)
{
 if (f->ed->mxedt > 0)
 {
  if(f->zoom == 0)
  {
   f->sa = e_set_pnt(f->a.x, f->a.y);
   f->se = e_set_pnt(f->e.x, f->e.y);
   f->a = e_set_pnt(0, 1);
   f->e = e_set_pnt(MAXSCOL-1, MAXSLNS-2);
   f->zoom = 1;
  }
  else
  {
   f->a = e_set_pnt(f->sa.x, f->sa.y);
   f->e = e_set_pnt(f->se.x, f->se.y);
   f->zoom = 0;
  }
  f->pic = e_ed_kst(f, f->pic, 1);
  if(f->pic == NULL)  e_error(e_msg[ERR_LOWMEM], 1, f->fb);
  e_cursor(f, 1);
  e_schirm(f, 1);
 }
 return(WPE_ESC);
}

/* The lowest row a docked pane may start at for Tile and Cascade to still
   arrange the file windows above it: a title row and three text rows. */
#define ARRANGE_MIN_STRIP_TOP 5
/* The last row Cascade may use when nothing is docked: above the status line. */
#define CASCADE_DEFAULT_BOTTOM_ROW (MAXSLNS - 2)
/* The last row Tile may use when nothing is docked: the lower third stays
   free for the Messages window a build will open. */
#define TILE_DEFAULT_BOTTOM_ROW (MAXSLNS - 2 - (MAXSLNS / 3 - 1))
/* Tile's "maxlines" is the screen height whose last window row is the tiles'
   bottom row; the tiles end two rows above it. */
#define TILE_LINES_BELOW_BOTTOM 2

/**
 * e_arrange_leaves_docked - Must Tile and Cascade leave this window where it is?
 * @cn: the desktop.
 * @f: the window.
 *
 * In the programming mode with the modern layout, Messages, Watches and the AI
 * pane keep their place at the bottom while the file windows are arranged, so
 * the compiler's output and the conversation stay readable after the command.
 * The "old style" tile option restores the original arrangement of every
 * window.
 * Return: 1 to leave the window alone, 0 to arrange it.
 */
static int e_arrange_leaves_docked(ECNT *cn, const FENSTER *f)
{
 if (cn->edopt & ED_OLD_TILE_METHOD)
  return 0;
 if (!WpeIsProg())
  return 0;
 return e_win_is_bottom_docked(f);
}

/**
 * e_tile_leaves_window - Must Tile leave this window where it is?
 * @cn: the desktop.
 * @f: the window.
 *
 * Besides the docked panes, the modern layout leaves the non-text windows
 * (the file manager, the data windows) alone, as they are not editors.
 * Return: 1 to leave the window alone, 0 to tile it.
 */
static int e_tile_leaves_window(ECNT *cn, const FENSTER *f)
{
 if (cn->edopt & ED_OLD_TILE_METHOD)
  return 0;
 if (!DTMD_ISTEXT(f->dtmd))
  return 1;
 return e_arrange_leaves_docked(cn, f);
}

/**
 * e_docked_strip_top - The first row a docked pane occupies.
 * @cn: the desktop.
 *
 * Return: the highest top row among the docked panes Tile and Cascade leave
 * alone, or MAXSLNS - 1 (the status line) when none is open.
 */
static int e_docked_strip_top(ECNT *cn)
{
 int i, top = MAXSLNS - 1;

 for (i = 1; i <= cn->mxedt; i++)
 {
  if (!e_arrange_leaves_docked(cn, cn->f[i])) continue;
  if (cn->f[i]->a.y < top)
   top = cn->f[i]->a.y;
 }
 return top;
}

/**
 * e_arranged_bottom_row - The last row Tile or Cascade may fill.
 * @cn: the desktop.
 * @default_bottom: the row used when nothing is docked.
 *
 * With a pane docked, the arranged file windows end on the row above it, so
 * the command never pushes a window under the compiler's output or the chat.
 * A pane that leaves no usable room above it (a tiny terminal) does not
 * shrink the arrangement.
 */
static int e_arranged_bottom_row(ECNT *cn, int default_bottom)
{
 int strip_top = e_docked_strip_top(cn);

 if (strip_top >= MAXSLNS - 1)
  return default_bottom;
 if (strip_top < ARRANGE_MIN_STRIP_TOP)
  return default_bottom;
 return strip_top - 1;
}

/**
 * e_count_cascaded - How many windows Cascade arranges.
 * @cn: the desktop.
 */
static int e_count_cascaded(ECNT *cn)
{
 int i, n = 0;

 for (i = 1; i <= cn->mxedt; i++)
  if (!e_arrange_leaves_docked(cn, cn->f[i]))
   n++;
 return n;
}

/**
 * e_cascade_place - Put one window on its step of the staircase.
 * @f: the window.
 * @step: its step, 1 for the window at the back of the stack.
 * @steps: the number of cascaded windows.
 * @bottom: the last row the staircase may use.
 *
 * Each step is one row down and one column right of the previous, so every
 * title stays visible and the active window (the last step) is on top.
 */
static void e_cascade_place(FENSTER *f, int step, int steps, int bottom)
{
 f->a = e_set_pnt(step - 1, step);
 f->e = e_set_pnt(MAXSCOL - 1 - steps + step, bottom - steps + step);
}

/**
 * e_ed_cascade - Window > Cascade: stack the file windows as a staircase.
 * @f: the window the command came from.
 *
 * A docked pane (Messages, Watches, the AI pane) keeps its place and the
 * staircase ends above it, so a build's output or a conversation stays where
 * the user left it.
 */
int e_ed_cascade(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i, step = 0;
 int steps = e_count_cascaded(cn);
 int bottom = e_arranged_bottom_row(cn, CASCADE_DEFAULT_BOTTOM_ROW);

 if (cn->mxedt < 1)
  return 0; /* no windows open */
 for (i = 1; i <= cn->mxedt; i++)
 {
  e_free_view(&cn->f[i]->pic);
  if (e_arrange_leaves_docked(cn, cn->f[i]))
   continue;
  step++;
  e_cascade_place(cn->f[i], step, steps, bottom);
 }
 ini_repaint(cn);
 for ( i = 1; i < cn->mxedt; i++)
 {
  e_firstl(cn->f[i], 0);
  e_schirm(cn->f[i], 0);
 }
 e_firstl(cn->f[i], 1);
 e_schirm(cn->f[i], 1);
 e_cursor(cn->f[i], 1);
 end_repaint();
 return(0);
}

/*   Tile windows   */
int e_ed_tile(FENSTER *f)
{
 ECNT *cn = f->ed;
 POINT atmp[MAXEDT+1];
 POINT etmp[MAXEDT+1];
 int i, j, ni, nj;
 int editwin = 0; /* number of editor windows */
 int editorwin[MAXEDT + 1];
 int maxlines = MAXSLNS;

 for (i = cn->mxedt; i >= 1; i--)
 {
  if (e_tile_leaves_window(cn, cn->f[i]))
  {
   editorwin[i] = 0;
  }
  else
  {
   editwin++;
   editorwin[i] = 1;
  }
 }
 if (editwin < 1)
  return(0);
 if ((!(cn->edopt & ED_OLD_TILE_METHOD)) && (WpeIsProg()))
 {
  /* The tiles end above the docked strip: on the row over an open pane, else
     over the lower third kept free for Messages as before. */
  maxlines = e_arranged_bottom_row(cn, TILE_DEFAULT_BOTTOM_ROW)
             + TILE_LINES_BELOW_BOTTOM;
 }
 for (i = cn->mxedt; i >= 1; i--)
 {
  e_free_view(&cn->f[i]->pic);
 }
 for (ni = editwin, nj = 1; ni > 1; ni--)
 {
  nj = editwin / ni;
  if (editwin % ni)
   nj++;
  if (nj >= ni)
   break;
 }
 if (nj*ni < editwin)
  nj++;
 for (j = 0; j < nj; j++)
 {
  for (i = 0; i < ni; i++)
  {
   if (j == 0)
   {
    if (i == 0)
    {
     atmp[j*ni+i].x = i * MAXSCOL / ni;
     etmp[j*ni+i].x = (i + 1) * MAXSCOL / ni - 1;
     if (etmp[j*ni+i].x - atmp[j*ni+i].x < 26)
      etmp[j*ni+i].x = atmp[j*ni+i].x + 26;
    }
    else
    {
     etmp[j*ni+i].x = (i + 1) * MAXSCOL / ni - 1;
     atmp[j*ni+i].x = etmp[j*ni+i-1].x + 1;
     if (etmp[j*ni+i].x - atmp[j*ni+i].x < 26)
      etmp[j*ni+i].x = atmp[j*ni+i].x + 26;
     if (etmp[j*ni+i].x >= MAXSCOL)
     {
      etmp[j*ni+i].x = MAXSCOL - 1;
      atmp[j*ni+i].x = etmp[j*ni+i].x - 26;
     }
    }
   }
   else
   {
    atmp[j*ni+i].x = atmp[(j-1)*ni+i].x;
    etmp[j*ni+i].x = etmp[(j-1)*ni+i].x;
    /* make the last window full width */
    if ((j * ni + i) == (editwin - 1))
     etmp[j * ni + i].x = MAXSCOL - 1;
   }
  }
 }
 for (i = 0; i < ni; i++)
 {
  for (j = 0; j < nj; j++)
  {
   if (i == 0)
   {
    if (j == 0)
    {
     atmp[j*ni+i].y = j * (maxlines-2) / nj + 1;
     etmp[j*ni+i].y = (j + 1) * (maxlines-2) / nj;
     if (etmp[j*ni+i].y - atmp[j*ni+i].y < 3)
      etmp[j*ni+i].y = atmp[j*ni+i].y + 3;
    }
    else
    {
     etmp[j*ni+i].y = (j + 1) * (maxlines-2) / nj;
     atmp[j*ni+i].y = etmp[(j-1)*ni+i].y + 1;
     if (etmp[j*ni+i].y - atmp[j*ni+i].y < 3)
      etmp[j*ni+i].y = atmp[j*ni+i].y + 3;
     if (etmp[j*ni+i].y > maxlines - 2)
     {
      etmp[j*ni+i].y = maxlines - 2;
      atmp[j*ni+i].y = etmp[j*ni+i].y - 3;
     }
    }
   }
   else
   {
    atmp[j*ni+i].y = atmp[j*ni+i-1].y;
    etmp[j*ni+i].y = etmp[j*ni+i-1].y;
   }
  }
 }
 for (i = 0, j = 1; i < editwin; i++, j++)
 {
  while (!editorwin[j]) j++;
  cn->f[j]->a = e_set_pnt(atmp[i].x, atmp[i].y);
  cn->f[j]->e = e_set_pnt(etmp[i].x, etmp[i].y);
  cn->f[j]->zoom = 0; /* Make sure zoom is off */
 }
 ini_repaint(cn);
 for ( i = 1; i < cn->mxedt; i++)
 {
  e_firstl(cn->f[i], 0);
  e_schirm(cn->f[i], 0);
 }
 e_firstl(cn->f[i], 1);
 e_schirm(cn->f[i], 1);
 e_cursor(cn->f[i], 1);
 end_repaint();
 return(0);
}

/*   call next window   */
int e_ed_next(FENSTER *f)
{
 if (f->ed->mxedt > 0) e_switch_window(f->ed->edt[1], f);
 return(0);
}

/*   write a line (screen content)     */
void e_pr_line(int y, FENSTER *f)
{
 BUFFER *b = f->b;
 SCHIRM *s = f->s;
 int i, j, k, frb;
 /* The colour this line's plain text paints in: the editor text entry, or
    in the AI pane the entry of the role that wrote the line. */
 int text_frb = s->fb->et.fb;
#ifdef DEBUGGER
 int fsw = 0;
 int hollow = 0;   /* the breakpoint on this line is not confirmed yet */
 /* Inline LSP decorations in the plain (syntax-off) painter, mirroring
    e_pr_c_line: recolor cells inside a problem's range (errors red, warnings
    amber) or a document-highlight span.  e_lsp_decor_active_for gates per line;
    both are no-ops unless a language server is attached to f, so help/non-source
    windows are unaffected. */
 extern int e_lsp_decor_active_for(FENSTER *f);
 extern int e_lsp_decor_attr_at(SCHIRM *s, int y, int x, int base);
 int diag_on = e_lsp_decor_active_for(f);
#endif
#ifdef WPE_AI
 text_frb = wpe_ai_pane_line_attr(f, y, text_frb);
#endif

 for (i = j = 0; j < NUM_COLS_OFF_SCREEN_LEFT && i < b->bf[y].len; j++)
 {
  unsigned char ch = (unsigned char) *(b->bf[y].s + i);
  if (ch == WPE_TAB)
  { j += (f->ed->tabn - j % f->ed->tabn - 1); i++; }
  else if (ch < ' ')
  { j++; i++; }
  else if (ch >= 0xC0 && ch < 0xFE)
  { /* UTF-8 lead byte: one glyph, as the keys step over it */
   int cp, cw, nb = e_utf8_glyph_at(b->bf[y].s + i, b->bf[y].len - i, &cp, &cw);
   j += cw - 1;
   i += nb;
  }
  else
   i++;
 }
 if (j > NUM_COLS_OFF_SCREEN_LEFT) i--;
#ifdef DEBUGGER
 for (j = 1; j <= s->brp[0]; j++)
  if (s->brp[j] == y) {  fsw = 1;  break;  }
 if (fsw)
  hollow = e_d_brk_drawn_hollow(f->datnam, y + 1);
 for (j = NUM_COLS_OFF_SCREEN_LEFT; i < b->bf[y].len && j < COL_NUM_ON_SCREEN_RIGHT; i++, j++)
 {
  if (y == s->da.y && i >= s->da.x && i < s->de.x )
   frb = s->fb->dy.fb;
  else if (fsw && hollow) frb = s->fb->du.fb;
  else if (fsw) frb = s->fb->db.fb;
/*	else if( (i == s->pt[0].x && y == s->pt[0].y) || (i == s->pt[1].x && y == s->pt[1].y)  */
  else if (y == s->fa.y && i >= s->fa.x && i < s->fe.x )
   frb = s->fb->ek.fb;
#else
 for (j = NUM_COLS_OFF_SCREEN_LEFT; i < b->bf[y].len && j < COL_NUM_ON_SCREEN_RIGHT; i++, j++)
 {
  if (y == s->fa.y && i >= s->fa.x && i < s->fe.x )
   frb = s->fb->ek.fb;
#endif
/*	if( (i == s->pt[0].x && y == s->pt[0].y) || (i == s->pt[1].x && y == s->pt[1].y)  
         || (i == s->pt[2].x && y == s->pt[2].y) || (i == s->pt[3].x && y == s->pt[3].y)  
         || (i == s->pt[4].x && y == s->pt[4].y) || (i == s->pt[5].x && y == s->pt[5].y)  
         || (i == s->pt[6].x && y == s->pt[6].y) || (i == s->pt[7].x && y == s->pt[7].y)  
         || (i == s->pt[8].x && y == s->pt[8].y) || (i == s->pt[9].x && y == s->pt[9].y))
            frb = s->fb->ek.fb;
*/
  else if ((y < s->mark_end.y && ( y > s->mark_begin.y ||
    (y == s->mark_begin.y && i >= s->mark_begin.x) ) ) ||
    (y == s->mark_end.y && i < s->mark_end.x && ( y > s->mark_begin.y ||
    (y == s->mark_begin.y && i >= s->mark_begin.x) ) ) )
   frb = s->fb->ez.fb;
  else
   frb = text_frb;
#ifdef DEBUGGER
  if (diag_on)
   frb = e_lsp_decor_attr_at(s, y, i, frb);  /* recolor LSP problem/highlight cells */
#endif

  if (f->dtmd == DTMD_HELP)
  {
   if (*(b->bf[y].s + i) == HBG || *(b->bf[y].s + i) == HFB ||
     *(b->bf[y].s + i) == HHD || *(b->bf[y].s + i) == HBB)
   {
    if (*(b->bf[y].s + i) == HHD) frb = s->fb->hh.fb;
    else if(*(b->bf[y].s + i) == HBB) frb = s->fb->hm.fb;
    else frb = s->fb->hb.fb;
#ifdef NEWSTYLE
    if (*(b->bf[y].s + i) != HBB) k = j;
    else k = -1;
#endif
    for (i++; b->bf[y].s[i] != HED && i < b->bf[y].len &&
      j < COL_NUM_ON_SCREEN_RIGHT; i++, j++)
     e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1,
       y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1, *(b->bf[y].s + i), frb);
    j--;
#ifdef NEWSTYLE
    if (WpeIsXwin() && k >= 0)
     e_make_xrect(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + k + 1, y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1,
       f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1, y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1, 0);
#endif
    continue;
   }
   else if (*(b->bf[y].s + i) == HFE)
   {
    for (; j < COL_NUM_ON_SCREEN_RIGHT; j++)
     e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1, y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1,
       ' ', s->fb->hh.fb);
    return;
   }
   else if (*(b->bf[y].s + i) == HNF)
   {
    for (k = j, i++; b->bf[y].s[i] != ':' && i < b->bf[y].len &&
      j < COL_NUM_ON_SCREEN_RIGHT; i++, j++)
     e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1,
       y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1, *(b->bf[y].s + i), s->fb->hb.fb);
#ifdef NEWSTYLE
    if (WpeIsXwin())
     e_make_xrect(f->a.x-NUM_COLS_OFF_SCREEN_LEFT+k+1, y-NUM_LINES_OFF_SCREEN_TOP+f->a.y+1,
       f->a.x-NUM_COLS_OFF_SCREEN_LEFT+j, y-NUM_LINES_OFF_SCREEN_TOP+f->a.y+1, 0);
#endif
    for (; b->bf[y].s[i] != HED && i < b->bf[y].len &&
      j < COL_NUM_ON_SCREEN_RIGHT; i++, j++)
     e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1,
       y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1, *(b->bf[y].s + i), frb);
    for (i++; b->bf[y].s[i] != HED && i < b->bf[y].len &&
      j < COL_NUM_ON_SCREEN_RIGHT; i++, j++)
     e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1,
       y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1, ' ', frb);
    for (; b->bf[y].s[i] != '.' && i < b->bf[y].len &&
      j < COL_NUM_ON_SCREEN_RIGHT; i++, j++)
     e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1,
       y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1, ' ', frb);
    j--;
    continue;
   }
   else if (*(b->bf[y].s + i) == HED) {  j--;  continue;  }
  }
  if (*(b->bf[y].s + i) == WPE_TAB)
   for (k = f->ed->tabn - j % f->ed->tabn; k > 1 &&
     j < NUM_COLS_ON_SCREEN + NUM_COLS_OFF_SCREEN_LEFT - 2; k--, j++)
    e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1, y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1,
      ' ', frb);
  else if (*(b->bf[y].s + i) < ' ')
  {
   e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j +1, y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1,
     '^', frb);
   if (++j >= COL_NUM_ON_SCREEN_RIGHT) return;
  }
  if (*(b->bf[y].s + i) == WPE_TAB)
   e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1, y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1,                  ' ', frb);
  else if (*(b->bf[y].s + i) < ' ' && j < COL_NUM_ON_SCREEN_RIGHT)
   e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1, y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1,
     *(b->bf[y].s + i) + 'A' - 1, frb);
  else
  {
   unsigned char uc = (unsigned char) *(b->bf[y].s + i);
   if (uc >= 0xC0 && uc < 0xFE)
   {
    /* UTF-8 lead byte: one glyph, the bytes the keys step over */
    int wc, cw;
    int nb = e_utf8_glyph_at(b->bf[y].s + i, b->bf[y].len - i, &wc, &cw);
    if (wc != 0)
    {
     int sx = f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1;
     int sy = y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1;
     j += e_put_wide_cell(sx, sy, wc, cw, frb,
                          j + 1 < COL_NUM_ON_SCREEN_RIGHT);
    }
    else
     e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1,
       y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1, '?', frb);
    i += nb - 1;  /* skip the rest of the glyph (loop does i++) */
   }
   else
    e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1,
      y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1, *(b->bf[y].s + i), frb);
  }
 }

 if ((i == b->bf[y].len) && (f->ed->edopt & ED_SHOW_ENDMARKS) &&
   (DTMD_ISMARKABLE(f->dtmd)) && (j < COL_NUM_ON_SCREEN_RIGHT))
 {
  if ((y < s->mark_end.y && ( y > s->mark_begin.y ||
    (y == s->mark_begin.y && i >= s->mark_begin.x) ) ) ||
    (y == s->mark_end.y && i < s->mark_end.x && ( y > s->mark_begin.y ||
    (y == s->mark_begin.y && i >= s->mark_begin.x) ) ) )
  {
   if (*(b->bf[y].s + i) == WPE_WR)
    e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1, y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1,
      PWR, s->fb->ez.fb);
   else
    e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1, y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1,
      PNL, s->fb->ez.fb);
  }
  else
  {
   if (*(b->bf[y].s + i) == WPE_WR)
    e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1, y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1,
      PWR, s->fb->et.fb);
   else
    e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1, y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1,
      PNL, s->fb->et.fb);
  }
  j++;
 }

#ifdef DEBUGGER
 /* End-of-line LSP inlay hints, when the file has no syntax highlighting (this
    plain painter); the syntax painter e_pr_c_line hooks the same helper. */
 if (i == b->bf[y].len)
 {
  extern void e_pr_inlay_eol(FENSTER *f, int y, int *jp, int frb);
  e_pr_inlay_eol(f, y, &j, s->fb->et.fb);
 }
#endif
 for (; j < COL_NUM_ON_SCREEN_RIGHT; j++)
  e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1, y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1,
    ' ', s->fb->et.fb);
}

/*   draw standard-box frame  */
static void e_draw_titlebar_buttons(int xa, int ya, int xe, int frb, int fes)
{
 /* Borland message/dialog boxes (TDialog) carried ONLY a close box, never a
    zoom/maximize box: a popup is fixed-size, so maximizing it is meaningless.
    Draw just the close glyph at xe-2 (modernized to the top-right corner);
    the cells that used to hold the maximize glyph are left as the title-bar
    frame line already painted by e_std_rahmen.  Clicking this [X] dismisses
    the popup (handled in e_opt_mouse / e_error), the same as pressing Esc.
    Document windows keep their zoom box -- that is drawn by
    e_draw_window_buttons, not here. */
 (void) xa;
 (void) frb;
 e_pr_char(xe-2, ya, 0x2715, fes);
}

/* e_draw_dialog_close_button - paint ONLY the close [X] on a dialog title bar.
   File-manager pickers (Open, Save As, search dialogs) are fixed-size like a
   Borland TDialog: a maximize/zoom box is meaningless there, so we draw the
   close glyph at the top-right (xe-2) and leave the maximize cell (xe-4) as the
   plain frame line already painted by e_std_rahmen.  The click is hit-tested in
   WpeMngMouseInFileManager (e_hit_close_button) and dismisses the dialog. */
static void e_draw_dialog_close_button(FENSTER *f)
{
 if (WpeIsXwin())
  e_pr_char(f->e.x-2, f->a.y, 0x2715, f->fb->er.fb);
 else
  e_pr_char(f->e.x-2, f->a.y, 0x2715, f->fb->es.fb);
}

static void e_clear_titlebar_buttons(FENSTER *f)
{
 /* Overwrite the maximize/close button cells with the horizontal frame
    line (RD5), not the corner glyph (RD2), so the title bar reads as a
    clean border on modal pickers. The corner at f->e.x is drawn by
    e_std_rahmen and must be left untouched. */
 int i, frb = f->fb->nr.fb;
 for (i = f->e.x - 5; i <= f->e.x - 1; i++)
  e_pr_char(i, f->a.y, RD5, frb);
}

static void e_draw_window_buttons(FENSTER *f)
{
 if (WpeIsXwin())
 {
  int maximize_glyph = (f->zoom == 0) ? 0x25FB : 0x25A3;
  e_pr_char(f->e.x-4, f->a.y, maximize_glyph, f->fb->es.fb);
  e_pr_char(f->e.x-3, f->a.y, ' ', f->fb->er.fb);
  e_pr_char(f->e.x-2, f->a.y, 0x2715, f->fb->er.fb);
 }
 else
 {
  int maximize_glyph = (f->zoom == 0) ? 0x25A1 : 0x25A3;
  e_pr_char(f->e.x-5, f->a.y, ' ', f->fb->er.fb);
  e_pr_char(f->e.x-4, f->a.y, maximize_glyph, f->fb->es.fb);
  e_pr_char(f->e.x-3, f->a.y, ' ', f->fb->er.fb);
  e_pr_char(f->e.x-2, f->a.y, 0x2715, f->fb->es.fb);
 }
}

void e_std_rahmen(int xa, int ya, int xe, int ye, char *name, int sw, int frb,
  int fes)
{
 int i, hfrb;
 char rhm[2][6];
 char *short_name;

 hfrb = (g_rahmen_hdr_frb >= 0) ? g_rahmen_hdr_frb : frb;
   
 rhm[0][0] = RE1; rhm[0][1] = RE2; rhm[0][2] = RE3; rhm[0][3] = RE4;
 rhm[0][4] = RE5; rhm[0][5] = RE6; rhm[1][0] = RD1; rhm[1][1] = RD2;
 rhm[1][2] = RD3; rhm[1][3] = RD4; rhm[1][4] = RD5; rhm[1][5] = RD6;
 for (i = xa + 1; i < xe; i++)
 {
  e_pr_char(i, ya, rhm[sw][4], frb);
  e_pr_char(i, ye, rhm[sw][4], frb);
 }
 for (i = ya + 1; i < ye; i++)
 {
  e_pr_char(xa, i, rhm[sw][5], frb);
  e_pr_char(xe, i, rhm[sw][5], frb);
 }
 e_pr_char(xa, ya, rhm[sw][0], frb);
 e_pr_char(xa, ye, rhm[sw][2], frb);
 e_pr_char(xe, ya, rhm[sw][1], frb);
 e_pr_char(xe, ye, rhm[sw][3], frb);

 if (name && xe - xa > 6)
 {
  int width = xe - xa;
  int nlen = strlen(name);
  if (nlen <= width - 14)
   e_pr_str((xa+xe-nlen)/2, ya, name, hfrb, 0, 0, 0, 0);
  else if (width > 20)
  {
   short_name = strdup(name);
   if (short_name)
   {
    short_name[width - 17] = '\0';
    strcat(short_name, "...");
    e_pr_str(xa + 7, ya, short_name, hfrb, 0, 0, 0, 0);
    free(short_name);
   }
  }
 }
 if (sw != 0)
  e_draw_titlebar_buttons(xa, ya, xe, frb, fes);
}

struct dirfile *e_add_df(char *str, struct dirfile *df)
{
   int i, n;
   char *tmp;

   if (df == NULL)
   {  df = MALLOC(sizeof(struct dirfile));
      df->anz = 0;
      df->name = MALLOC(sizeof(char*));
   }
   for(n = 0; n < df->anz && *df->name[n] && strcmp(df->name[n], str); n++);
   if(n == df->anz)
   {  if(df->anz == MAXSVSTR - 1) FREE(df->name[df->anz-1]);
      else
      {  df->anz++;
	 df->name = REALLOC(df->name, df->anz * sizeof(char*));
      }
      for(i = df->anz-1; i > 0; i--) df->name[i] = df->name[i-1];
      df->name[0] = MALLOC((strlen(str)+1) * sizeof(char));
      strcpy(df->name[0], str);
   }
   else
   {  tmp = df->name[n];
      for(i = n; i > 0; i--) df->name[i] = df->name[i-1];
      if(!tmp[0])
      {  FREE(tmp);
	 df->name[0] = MALLOC((strlen(str) + 1) * sizeof(char));
	 strcpy(df->name[0], str);
      }
      else df->name[0] = tmp;
   }
   return(df);
}

int e_sv_window(int xa, int ya, int *n, struct dirfile *df, FENSTER *f)
{
 ECNT *cn = f->ed;
 int ret, ye = ya + 6;
 int xe = xa +21;
 FLWND *fw = MALLOC(sizeof(FLWND));

 if ((f = (FENSTER *) MALLOC(sizeof(FENSTER))) == NULL)
  e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
 if (xe > MAXSCOL-3) {  xe = MAXSCOL - 3;  xa = xe - 21;  }
 if (ye > MAXSLNS-3) {  ye = MAXSLNS - 3;  ya = ye - 6;  }
 f->fb = cn->fb;
 f->a = e_set_pnt(xa, ya);
 f->e = e_set_pnt(xe, ye);
 f->dtmd = DTMD_FILEDROPDOWN;
 f->zoom = 0;
 f->ed = cn;
 f->c_sw = NULL;
 f->c_st = NULL;
 f->fd.dirct = NULL;
 f->winnum = -1;
 f->datnam = "";
 if (!(f->pic = e_ed_kst(f, NULL, 1)))
 {  e_error(e_msg[ERR_LOWMEM], 0, f->fb);  return(0);  }
 f->b = (BUFFER *)fw;
 fw->mxa = xa; fw->mxe = xe; fw->mya = ya; fw->mye = ye;
 fw->xa = xa+1; fw->xe = xe; fw->ya = ya+1; fw->ye = ye;
 fw->df = df; fw->srcha = 0; fw->f = f;
 fw->nf = fw->ia = fw->ja = 0;
 do
 {
  ret = e_file_window(0, fw, f->fb->er.fb, f->fb->ez.fb);
#if MOUSE
  if (ret < 0) ret = e_rahmen_mouse(f);
#endif
  if (wpe_binding_action(ret, wpe_binding_style(f->ed->edopt),
                         WPE_CTX_DIALOG_MOVE) == WPE_ACT_SIZE_MOVE)
   e_size_move(f);
 } while(ret != WPE_CR && ret != WPE_ESC);
 *n = fw->nf;
 e_close_view(f->pic, 1);
 FREE(fw);
 FREE(f);
 return(ret);
}

/**
 * e_schr_lst_pick - Put the history entry the user picked into the field.
 * @str: the field's text, room for @len bytes and a terminator;
 * @len: the field's capacity; @pick: the entry from the list.
 *
 * The lists are shared by several fields (the folder list by the file
 * manager, Find and Grep), and an entry typed into a roomier one can be
 * longer than this field.  Such an entry is left in the list and the field
 * keeps its text, with the status row saying it is full, instead of the
 * entry running past the field's buffer or being cut.
 */
static void e_schr_lst_pick(char *str, int len, const char *pick)
{
   if (strlen(pick) > (size_t)len)
   {
      e_input_limit_say(len);
      return;
   }
   if (wpe_field_take(pick, str, (size_t)len + 1) != 0)
      str[0] = '\0';
}

int e_schr_lst_wsv(char *str, int xa, int ya, int n, int len, int ft,
  int fz, struct dirfile **df, FENSTER *f)
{
#if MOUSE
   extern struct mouse e_mouse;
#endif
   int ret, num;
   do
   {  *df = e_add_df(str, *df);
#ifndef NEWSTYLE
      ret = e_schreib_leiste(str, xa, ya, n-4, len, ft, fz);
#else
      ret = e_schreib_leiste(str, xa, ya, n-3, len, ft, fz);
#endif
#if MOUSE
      if(ret < 0 && e_mouse.y == ya && e_mouse.x >= xa+n-3
				&& e_mouse.x <= xa+n-1) ret = CDO;
#endif
      if (ret == CDO && e_sv_window(xa+n, ya, &num, *df, f) == WPE_CR)
          e_schr_lst_pick(str, len, (*df)->name[num]);
   }  while(ret == CDO);
   return(ret);
}

int e_schr_nchar_wsv(char *str, int x, int y, int n, int max, int col,
  int csw)
#ifdef NEWSTYLE
{
 e_pr_char(x+max-3, y, ' ', csw);
 e_pr_char(x+max-2, y, WSW, csw);
 e_pr_char(x+max-1, y, ' ', csw);
 e_make_xrect(x+max-3, y, x+max-1, y, 0);
 return(e_schr_nchar(str, x, y, n, max-3, col));
}
#else
{
#if !defined(NO_XWINDOWS)
 int swcol = (e_gt_col(x+max, y) / 16) * 16;
 if (WpeIsXwin())
 {
  e_pr_char(x+max, y, SCR, swcol);
  e_pr_char(x+max, y+1, SCD, swcol);
  e_pr_char(x+max-1, y+1, SCD, swcol);
  e_pr_char(x+max-2, y+1, SCD, swcol);
 }
#endif
 e_pr_char(x+max-3, y, ' ', csw);
 e_pr_char(x+max-2, y, WSW, csw);
 e_pr_char(x+max-1, y, ' ', csw);
 return(e_schr_nchar(str, x, y, n, max-4, col));
}
#endif

/* The Ctrl-C box of a long file operation (Remove, Rename, Copy): its frame
   is the text's width plus E_MESS_FRAME_COLS columns and the lines plus
   E_MESS_FRAME_ROWS rows.  It keeps E_MESS_INSET rows free next to the menu
   bar and the status row, and is centred as a box one column narrower, as it
   has been drawn since 1.5.30a.  The text starts E_MESS_TEXT_COL columns in
   and E_MESS_TEXT_TOP rows down; below it are the blank row and the "Ctrl C"
   button row. */
#define E_MESS_FRAME_COLS  7
#define E_MESS_FRAME_ROWS  6
#define E_MESS_INSET       1
#define E_MESS_TEXT_COL    3
#define E_MESS_TEXT_TOP    2
#define E_MESS_TAIL_ROWS   5   /* rows of the frame that are not text */
#define E_MESS_ELLIPSIS    "..."

/* What the Ctrl-C box's draw hook needs. */
typedef struct
{
 char *header;
 char **lines;
 int anz;
 ECNT *cn;
 PIC *carry;      /* the background the previous call left on the screen */
} E_MESS_BOX;

/**
 * e_mess_take_carry - Use the previous call's background, or give it back.
 * @box: the box, placed by the owner; @m: its state.
 *
 * The box lives across calls, one per file of the operation, and the
 * caller keeps its saved background in between.  When the box is at the
 * same place, at the same screen size, it is drawn over itself and the
 * background is kept; otherwise the old background is put back (a stale one
 * from before a resize is only freed) and the box takes a new one.
 */
static void e_mess_take_carry(WpeBox *box, E_MESS_BOX *m)
{
 PIC *c = m->carry;

 m->carry = NULL;
 if (c == NULL)
  return;
 if (c->generation == wpe_layout_generation() && c->a.x == box->at.bx_left &&
     c->a.y == box->at.bx_top && c->e.x == box->at.bx_right &&
     c->e.y == box->at.bx_bottom)
  box->pic = c;
 else
  e_close_view(c, 1);
}

/**
 * e_mess_box_new_frame - Frame a Ctrl-C box that has no background yet and
 * draw its button row.
 * @box: the box; @m: its state.
 */
static void e_mess_box_new_frame(WpeBox *box, E_MESS_BOX *m)
{
 FARBE *fb = m->cn->fb;
 int xa = box->at.bx_left, xe = box->at.bx_right, ye = box->at.bx_bottom;
 int i;

 if (wpe_box_frame(box, m->header, fb->er.fb, fb->et.fb, fb->es.fb) != 0)
  return;
 for (i = xa + 1; i < xe; i++)
 {
  e_pr_char(i, ye-2, ' ', fb->et.fb);
  e_pr_char(i, ye-1, ' ', fb->et.fb);
 }
 e_pr_str((xe + xa - 6)/2, ye-2, "Ctrl C", fb->nz.fb, -1, -1,
   fb->ns.fb, fb->nt.fb);
}

/**
 * e_mess_box_text_rows - How many text lines the box has room for.
 */
static int e_mess_box_text_rows(WpeBox *box, E_MESS_BOX *m)
{
 int room = box->at.bx_bottom - box->at.bx_top - E_MESS_TAIL_ROWS;

 return(m->anz < room ? m->anz : room);
}

/**
 * e_mess_box_draw - Draw the Ctrl-C box: the operation's header, the file
 * it is on, and the "Ctrl C" button.
 * @box: the box, placed by the owner for the screen as it is now.
 * Return: WPE_BOX_DRAWN.  A box that cannot save its background is still
 * drawn: the operation goes on.
 */
static WpeBoxDrawn e_mess_box_draw(WpeBox *box)
{
 E_MESS_BOX *m = box->self;
 FARBE *fb = m->cn->fb;
 int xa = box->at.bx_left, ya = box->at.bx_top;
 int xe = box->at.bx_right, ye = box->at.bx_bottom;
 int num = e_mess_box_text_rows(box, m);
 int i, j;

 e_mess_take_carry(box, m);
 if (box->pic == NULL)
  e_mess_box_new_frame(box, m);
 e_std_rahmen(xa, ya, xe, ye, m->header, 1, fb->er.fb, fb->es.fb);
 for (i = xa + 1; i < xe; i++)
  e_pr_char(i, ya+1, ' ', fb->er.fb);
 for (j = 0; j < num; j++)
 {
  char *line = (j == num - 1 && num < m->anz) ? E_MESS_ELLIPSIS : m->lines[j];

  e_pr_char(xa+1, ya+E_MESS_TEXT_TOP+j, ' ', fb->et.fb);
  e_pr_char(xa+2, ya+E_MESS_TEXT_TOP+j, ' ', fb->et.fb);
  e_pr_str(xa+E_MESS_TEXT_COL, ya+E_MESS_TEXT_TOP+j, line, fb->et.fb, 0, 0, 0, 0);
  for (i = xa+strlen(line)+E_MESS_TEXT_COL; i < xe; i++)
   e_pr_char(i, ya+E_MESS_TEXT_TOP+j, ' ', fb->et.fb);
 }
 for (j = (num > 0 ? num : 0) + ya + E_MESS_TEXT_TOP; j < ye-2; j++)
  for (i = xa + 1; i < xe; i++)
   e_pr_char(i, j, ' ', fb->et.fb);
 return(WPE_BOX_DRAWN);
}

/**
 * e_mess_box_init - Give the Ctrl-C box its rule.
 * @box: receives the box; @m: its state; @mxlen: the width of the text;
 * @anz: the number of lines.
 */
static void e_mess_box_init(WpeBox *box, E_MESS_BOX *m, int mxlen, int anz)
{
 memset(box, 0, sizeof *box);
 box->what = "the Ctrl-C box";
 box->self = m;
 box->draw = e_mess_box_draw;
 box->place.anchor = WPE_BOX_CENTRED_SCREEN;
 box->place.w = mxlen + E_MESS_FRAME_COLS;
 box->place.h = anz + E_MESS_FRAME_ROWS;
 box->place.inset = E_MESS_INSET;
 box->place.centre_w = box->place.w - 1;
 box->place.centre_h = box->place.h;
}

/**
 * e_mess_win - Show which file a long operation is on, and take a Ctrl C.
 * @header: the operation; @str: the file (or files) it is on.
 * @pic: the box's saved background, kept by the caller between calls.
 * @f: the current window.
 * Return: 1 when the user cancelled, else 0.
 *
 * The box is opened here and left again before the call returns, so nothing
 * on the layout owner's stack outlives it; the caller closes the background
 * it holds in @pic with e_close_view when the operation ends.  A resize
 * while the box waits for a key is laid out by the owner, which gives the
 * box the new background.
 */
int e_mess_win(char *header, char *str, PIC **pic, FENSTER *f)
{
 WpeBox box;
 E_MESS_BOX m;
 int mxlen = 0, anz = 0, i, key;
 char **s;

 s = StringToStringArray(str, &mxlen, strlen(header) + 8, &anz);
 m.header = header;
 m.lines = s;
 m.anz = anz;
 m.cn = f->ed;
 m.carry = *pic;
 e_mess_box_init(&box, &m, mxlen, anz);
 wpe_box_open(&box);
 e_refresh();
 e_t_wait_for_enter_on_raw_terminal();
 key = e_t_drain_keys_after_box(box.at.bx_left, box.at.bx_right, box.at.bx_bottom);
 e_mouse_flush();
 *pic = box.pic;
 wpe_box_close(&box);
 for (i = 0; i < anz; i++)
  FREE(s[i]);
 FREE(s);
 return(key == CtrlC ? 1 : 0);
}

int e_opt_sec_box(int xa, int ya, int num, OPTK *opt, FENSTER *f, int sw)
{
   PIC *pic;
   int n, nold, max = 0, i, c = 0, xe, ye = ya + num + 1;
   for(i = 0; i < num; i++)
   if((n = strlen(opt[i].t)) > max) max = n;
   xe = xa + max + 3;
   pic = e_std_kst(xa, ya, xe, ye, NULL, sw, f->fb->nr.fb, f->fb->nt.fb, f->fb->ne.fb);
   if(pic == NULL)  {  e_error(e_msg[ERR_LOWMEM], 0, f->fb); return(-2);  }
   for (i = 0; i < num; i++)
   e_pr_str_wsd(xa+2, ya+i+1, opt[i].t, f->fb->mt.fb, opt[i].x,
		1, f->fb->ms.fb, xa+1, xe-1);
#if  MOUSE
   while (e_mshit() != 0);
#endif
   n = 0; nold = 1;
   while (c != WPE_ESC && c != WPE_CR)
   {  if (nold != n)
      {  e_pr_str_wsd(xa+2, nold+ya+1, opt[nold].t, f->fb->mt.fb,
				opt[nold].x, 1, f->fb->ms.fb, xa+1, xe-1);
	 e_pr_str_wsd(xa+2, n+ya+1, opt[n].t, f->fb->mz.fb,
				opt[n].x, 1, f->fb->mz.fb, xa+1, xe-1);
	 nold = n;
      }
#if  MOUSE
      if( (c = e_toupper(wpe_paint_modal_getch())) == -1)
      c = e_m2_mouse(xa, ya, xe, ye, opt);
#else
      c = e_toupper(wpe_paint_modal_getch());
#endif
      for (i = 0; i < ye - ya - 1; i++)
      if( c == opt[i].o) {  c = WPE_CR;  n = i;  break;  }
      if (i > ye - ya) c = WPE_ESC;
      else if ( c == CUP || c == CtrlP ) n = n > 0 ? n-1 : ye - ya - 2 ;
      else if ( c == CDO || c == CtrlN ) n = n < ye-ya-2 ? n+1 : 0 ;
      else if ( c == POS1 || c == CtrlA ) n = 0;
      else if ( c == ENDE || c == CtrlE ) n = ye-ya-2;
   }
   if(sw == 1) e_close_view(pic, 1);
   return(c == WPE_ESC ? -1 : n);
}

struct dirfile *e_make_win_list(FENSTER *f)
{
 int i;
 struct dirfile *df;

 if (!(df = MALLOC(sizeof(struct dirfile)))) return(NULL);
 df->anz = f->ed->mxedt;
 if (!(df->name = MALLOC(df->anz * sizeof(char *))))
 {
  FREE(df);
  return(NULL);
 }
 for (i = 0; i < df->anz; i++)
 {
  if (f->ed->f[df->anz-i]->datnam)
  {
   if (!(df->name[i] =
     MALLOC((strlen(f->ed->f[df->anz-i]->datnam)+1) * sizeof(char))))
   {
    df->anz = i;
    freedf(df);
    return(NULL);
   }
   else strcpy(df->name[i], f->ed->f[df->anz-i]->datnam);
  }
  else
  {
   if (!(df->name[i] = MALLOC(sizeof(char))))
   {
    df->anz = i;
    freedf(df);
    return(NULL);
   }
   else *df->name[i] = '\0';
  }
 }
 return(df);
}

int e_list_all_win(FENSTER *f)
{
 int i;

 for (i = f->ed->mxedt; i > 0; i--)
  if (f->ed->f[i]->dtmd == DTMD_DATA && f->ed->f[i]->ins == 7)
  {
   e_switch_window(f->ed->edt[i], f);
   return(0);
  }
 return(e_data_first(7, f->ed, NULL));
}

#ifdef NEWSTYLE
int e_get_pic_xrect(int xa, int ya, int xe, int ye, PIC *pic)
{
 int i = xa, j, ebbg;

 ebbg = (xe - xa + 1) * 2 * (ye - ya + 1);
 for (j = ya; j <= ye; ++j)
  for (i = xa; i <= xe; ++i)
   /* TODO: pic->p removed in panel migration; extbyte handling needs rework */
   (void)ebbg; /* suppress unused warning */
 return(i);
}

int e_put_pic_xrect(PIC *pic)
{
 int i = 0, j;
 int ebbg = (pic->e.x - pic->a.x + 1) * 2 * (pic->e.y - pic->a.y + 1);

 for (j = pic->a.y; j <= pic->e.y; ++j)
  for (i = pic->a.x; i <= pic->e.x; ++i)
   extbyte[j*MAXSCOL+i] =
     0; /* TODO: pic->p removed in panel migration */
 return(i);
}

/* The window-frame extension plane (extbyte) is exactly MAXSCOL*MAXSLNS bytes.
   A bottom-bar label or a window frame can legitimately be asked to draw one
   column or row past the grid after a resize (e.g. e_pr_uul's last visible
   button ends at the next button's x-1, which can equal MAXSCOL once the grid
   shrinks); an unchecked extbyte write there runs off the end of the plane and
   corrupts the heap.  These two helpers own the "stay inside the plane"
   invariant so every e_make_xrect caller is safe -- an off-grid cell is simply
   not part of any frame and is skipped. */
static void e_extbyte_or(int x, int y, int bits)
{
 if (extbyte && x >= 0 && x < MAXSCOL && y >= 0 && y < MAXSLNS)
  *(extbyte + y * MAXSCOL + x) |= bits;
}
static void e_extbyte_clear(int x, int y)
{
 if (extbyte && x >= 0 && x < MAXSCOL && y >= 0 && y < MAXSLNS)
  *(extbyte + y * MAXSCOL + x) = 0;
}

int e_make_xrect_abs(int xa, int ya, int xe, int ye, int sw)
{
 int j;

 for (j = xa; j <= xe; j++)
 {
  e_extbyte_clear(j, ya);
  e_extbyte_clear(j, ye);
 }
 for (j = ya; j <= ye; j++)
 {
  e_extbyte_clear(xa, j);
  e_extbyte_clear(xe, j);
 }
 return(e_make_xrect(xa, ya, xe, ye, sw));
}

int e_make_xrect(int xa, int ya, int xe, int ye, int sw)
{
 int j;

 if (sw & 2)
 {
  sw = (sw & 1) ? 16 : 0;
  for (j = xa+1; j < xe; j++)
  {
   e_extbyte_or(j, ya, sw | 4);
   e_extbyte_or(j, ye, sw | 1);
  }
  for (j = ya+1; j < ye; j++)
  {
   e_extbyte_or(xa, j, sw | 2);
   e_extbyte_or(xe, j, sw | 8);
  }
 }
 else
 {
  sw = (sw & 1) ? 16 : 0;
  for (j = xa; j <= xe; j++)
  {
   e_extbyte_or(j, ya, sw | 1);
   e_extbyte_or(j, ye, sw | 4);
  }
  for (j = ya; j <= ye; j++)
  {
   e_extbyte_or(xa, j, sw | 8);
   e_extbyte_or(xe, j, sw | 2);
  }
 }
 return(j);
}

int e_make_xr_rahmen(int xa, int ya, int xe, int ye, int sw)
{
 if (!sw)
 {
  e_make_xrect(xa, ya, xe, ye, 0);
  e_make_xrect(xa, ya, xe, ye, 2);
 }
 else
 {
  e_make_xrect(xa+1, ya, xe-1, ya, 0);
  e_make_xrect(xa+1, ye, xe-1, ye, 0);
  e_make_xrect(xa, ya+1, xa, ye-1, 0);
  e_make_xrect(xe, ya+1, xe, ye-1, 0);
  e_make_xrect(xa, ya, xa, ya, 0);
  e_make_xrect(xe, ya, xe, ya, 0);
  e_make_xrect(xe, ye, xe, ye, 0);
  e_make_xrect(xa, ye, xa, ye, 0);
 }
 return(sw);
}
#endif

