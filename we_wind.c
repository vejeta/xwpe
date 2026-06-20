/* we_wind.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include <wchar.h>

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
}

void e_position_messages_window(FENSTER *msg, ECNT *cn)
{
 int j, lowest_editor = -1;
 int split_y, default_split = 2 * MAXSLNS / 3;

 for (j = 1; j <= cn->mxedt; j++)
 {
  if (cn->f[j] == msg) continue;
  if (!strcmp(cn->f[j]->datnam, "Messages")) continue;
  if (!strcmp(cn->f[j]->datnam, "Watches")) continue;
  if (cn->f[j]->e.y > lowest_editor)
   lowest_editor = cn->f[j]->e.y;
 }

 if (lowest_editor > 0 && lowest_editor <= default_split)
  split_y = lowest_editor;
 else
  split_y = default_split;

 if (split_y > MAXSLNS - 5)
  split_y = MAXSLNS - 5;
 if (split_y < 4)
  split_y = 4;

 msg->a = e_set_pnt(0, split_y + 1);
 msg->e = e_set_pnt(MAXSCOL - 1, MAXSLNS - 2);

 for (j = 1; j <= cn->mxedt; j++)
 {
  if (cn->f[j] == msg) continue;
  /* Only clamp EDITOR windows to make room at the bottom.  The Messages and
     Watches windows are themselves bottom-docked (this function positions both
     of them), so they must be excluded exactly as in the split_y computation
     above -- otherwise positioning Watches collapses Messages to e.y = split_y
     while its a.y stays split_y+1, leaving an inverted, one-row-tall window. */
  if (!strcmp(cn->f[j]->datnam, "Messages")) continue;
  if (!strcmp(cn->f[j]->datnam, "Watches")) continue;
  if (cn->f[j]->e.y > split_y)
   cn->f[j]->e.y = split_y;
 }
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
  extern void e_x_clear_area(int, int, int, int);
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

/*
      Print error message        */
static int e_error_run(char *text, int sw, FARBE *f)
{
 PIC *pic = NULL;
 int len, i, xa, xe, ya = 8, ye = 14;
 char *header = NULL;

 fk_cursor(0);
 WpeMouseChangeShape(WpeErrorShape);
 if ((len = strlen((char *)text)) < 20 ) len = 20;
 if (sw == -1) header = "Message";
 else if (sw == 0) header = "Error";
 else if (sw == 1) header = "Serious Error";
 else if (sw == 2) header = "Fatal Error";

e_error_restart:
 xa = (MAXSCOL-len)/2 - 2;
 xe = xa + len + 4;
 ya = (MAXSLNS-6)/2;
 ye = ya + 6;
 if (xa < 0) xa = 0;
 if (xe >= MAXSCOL) xe = MAXSCOL - 1;
 if (ya < 1) ya = 1;
 if (ye >= MAXSLNS - 1) ye = MAXSLNS - 2;

 if (sw < 2) pic = e_std_kst(xa, ya, xe, ye, header, 1, f->nr.fb, f->nt.fb, f->ne.fb);
 if (sw == 2 || pic == NULL)
 {
  pic = e_open_view(xa, ya, xe, ye, 0, 0);
  e_std_rahmen(xa, ya, xe, ye, header, 1, 0, 0);
 }
 if (sw < 2)
 {
  e_pr_str((xe + xa - e_str_len((unsigned char *)text))/2,
    ya + 2, text, f->nt.fb, 0, 0, 0, 0);
  e_pr_str((xe + xa - 4)/2, ya + 4, " OK ", f->nz.fb, 1, -1,
    f->ns.fb, f->nt.fb);
 }
 else
 {
  e_pr_str((xe + xa - e_str_len((unsigned char *)text))/2,
    ya + 2, text, 112, 0, 0, 0, 0);
  e_pr_str((xe + xa - 4)/2, ya + 4, " OK ", 32, 1, -1, 46, 112);
 }
 do
 {
#if  MOUSE
  if ((i = e_toupper(e_getch())) == -1)
  {
   extern struct mouse e_mouse;
   i = e_er_mouse(xa+3, ya,(xe+xa-4)/2, ya+4);
   if (i == 0 && e_mouse.y == ya && e_mouse.x == xe-2)
    i = WPE_ESC;   /* clicked the title-bar [X] close box -> dismiss */
  }
#else
  i = e_toupper(e_getch());
#endif
  if (i == WPE_RESIZE)
  {
   e_close_view(pic, 1);
   goto e_error_restart;
  }
 } while (i != WPE_ESC && i != WPE_CR && i != 'O');
 WpeMouseRestoreShape();
 if (pic != NULL) e_close_view(pic, 1);
 else e_cls(0, ' ');
 fk_cursor(1);
 if (sw == 1) e_quit(WpeEditor->f[WpeEditor->mxedt]);
 if (sw > 0) WpeExit(sw);
 return(sw);
}

/* e_error - public entry to the message box (errors and plain "Message" notes,
   e.g. an LSP action's "No references found", shown right after the action while
   the server may still be streaming).  It runs its own getch loop over a box that
   pushes no window, so -- like e_opt_kst and WpeHandleSubmenu -- it brackets the
   async LSP fd-loop's painting with the modal-depth guard, so a server update
   arriving behind the box cannot draw under it and corrupt it.  A fatal error
   (sw >= 1) exits from e_error_run and never returns -- fine, the process is going
   away, so the unbalanced guard never matters. */
int e_error(char *text, int sw, FARBE *f)
{
 int ret;
#ifdef DEBUGGER
 e_lsp_modal_enter();
#endif
 ret = e_error_run(text, sw, f);
#ifdef DEBUGGER
 e_lsp_modal_leave();
#endif
 return(ret);
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
 ret = e_opt_kst(o);
 freeostr(o);
 return(ret == WPE_ESC ? WPE_ESC : (ret == AltN ? 'N' : 'Y'));
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
 pic->a.x = xa;
 pic->a.y = ya;
 pic->e.x = xe;
 pic->e.y = ye;

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

/*   close screen section - refresh background  */
int e_close_view(PIC *pic, int sw)
{
 int i, j;
#ifndef NO_XWINDOWS
 if (WpeIsXwin()) (*e_u_setlastpic)(NULL);
#endif
 if (pic == NULL) return(-1);

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
 return(sw);
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

/* e_win_is_tool_pane - True for the synthetic output panes (Messages, Watches,
   Stack).  They reuse the read-only flag (ins == 8) to mean "not editable", but
   they are tool output, NOT files on disk -- so they must not wear the read-only
   padlock, which says "this is a locked file".  (Help is excluded separately by
   its DTMD_HELP.)  Used by the title-bar draw to keep the padlock honest. */
static int e_win_is_tool_pane(FENSTER *f)
{
 return (f->datnam &&
         (!strcmp(f->datnam, "Messages") ||
          !strcmp(f->datnam, "Watches")  ||
          !strcmp(f->datnam, "Stack")));
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
  /* A read-only window (f->ins == 8: a 0444 file, an extracted library source):
     draw its name DIMMED and a padlock at the left of the title bar so it is
     unmistakably non-editable.  0x1F512 is LOCK; on a non-UTF console the chrome
     fallback (e_t_chrome_ascii) substitutes a stand-in. */
  int ro = (f->ins == 8 && f->dtmd != DTMD_HELP    /* a locked FILE...              */
            && !e_win_is_tool_pane(f));            /* ...not a Messages/Watches pane */
  if (ro)
   g_rahmen_hdr_frb = f->fb->es.fb;       /* title text in the dimmer frame colour */
  e_std_rahmen(f->a.x, f->a.y, f->e.x, f->e.y, header, sw, f->fb->er.fb,
    f->fb->es.fb);
  g_rahmen_hdr_frb = -1;
  if (ro && f->e.x - f->a.x > 8)
   e_pr_char(f->a.x + 2, f->a.y, 0x1F512, f->fb->er.fb);
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

/*   Move and modify window */
int e_size_move(FENSTER *f)
{
 int xa = f->a.x, ya = f->a.y, xe = f->e.x, ye = f->e.y;
 int c = 0, xmin = 26, ymin = 3;

 e_ed_rahmen(f, 0);
 if (f->dtmd == DTMD_FILEDROPDOWN)
  xmin = 15;
 else if (!DTMD_ISTEXT(f->dtmd))
  ymin = 9;
 while ((c = e_getch()) != WPE_ESC && c != WPE_CR)
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
  e_remove_undo(b->ud, b->cn->numundo + 1);
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
   else if (c == 'Y')
    e_save(f);
  }
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
  if (f->dtmd == DTMD_HELP && f->ins == 8)
   e_help_free(f);
  if (f->datnam != NULL)
   FREE(f->datnam);
  if (f->dirct != NULL)
   FREE(f->dirct);
  /* c_sw -- the per-line syntax-continuation state array (e_sc_txt), grown by
     e_new_line as the file is read.  It is freed and rebuilt on re-highlight
     (we_progn.c, we_fl_unix.c) but was never freed at window close, so each
     editor window leaked it (valgrind: definitely lost at e_new_line
     we_edit.c:2038). */
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

/*   cascade windows   */
int e_ed_cascade(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i;

 if (cn->mxedt < 1)
  return 0; /* no windows open */
 for (i = cn->mxedt; i >= 1; i--)
 {
  e_free_view(&cn->f[i]->pic);
  cn->f[i]->a = e_set_pnt(i-1, i);
  cn->f[i]->e = e_set_pnt(MAXSCOL-1-cn->mxedt+i, MAXSLNS-2-cn->mxedt+i);
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
  if ((!(cn->edopt & ED_OLD_TILE_METHOD)) && (!DTMD_ISTEXT(cn->f[i]->dtmd) ||
    ((WpeIsProg()) && ((strcmp(cn->f[i]->datnam, "Messages") == 0) ||
    (strcmp(cn->f[i]->datnam, "Watches") == 0)))))
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
  maxlines -= MAXSLNS / 3 - 1;
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
#ifdef DEBUGGER
 int fsw = 0;
 /* Inline LSP decorations in the plain (syntax-off) painter, mirroring
    e_pr_c_line: recolor cells inside a problem's range (errors red, warnings
    amber) or a document-highlight span.  e_lsp_decor_active_for gates per line;
    both are no-ops unless a language server is attached to f, so help/non-source
    windows are unaffected. */
 extern int e_lsp_decor_active_for(FENSTER *f);
 extern int e_lsp_decor_attr_at(SCHIRM *s, int y, int x, int base);
 int diag_on = e_lsp_decor_active_for(f);
#endif

 for (i = j = 0; j < NUM_COLS_OFF_SCREEN_LEFT && i < b->bf[y].len; j++)
 {
  unsigned char ch = (unsigned char) *(b->bf[y].s + i);
  if (ch == WPE_TAB)
  { j += (f->ed->tabn - j % f->ed->tabn - 1); i++; }
  else if (ch < ' ')
  { j++; i++; }
  else if (ch >= 0xC0 && ch < 0xFE)
  { /* UTF-8 lead byte: decode and account for wide chars */
   wchar_t wc = 0;
   mbstate_t mbs = {0};
   int nb = mbrtowc(&wc, b->bf[y].s + i, b->bf[y].len - i, &mbs);
   if (nb > 1) { int cw = wcwidth(wc); if (cw > 1) j += cw - 1; i += nb; }
   else i++;
  }
  else
   i++;
 }
 if (j > NUM_COLS_OFF_SCREEN_LEFT) i--;
#ifdef DEBUGGER
 for (j = 1; j <= s->brp[0]; j++)
  if (s->brp[j] == y) {  fsw = 1;  break;  }
 for (j = NUM_COLS_OFF_SCREEN_LEFT; i < b->bf[y].len && j < COL_NUM_ON_SCREEN_RIGHT; i++, j++)
 {
  if (y == s->da.y && i >= s->da.x && i < s->de.x )
   frb = s->fb->dy.fb;
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
   frb = s->fb->et.fb;
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
    /* UTF-8 lead byte: decode full character */
    wchar_t wc = 0;
    mbstate_t mbs = {0};
    int nb = mbrtowc(&wc, b->bf[y].s + i, b->bf[y].len - i, &mbs);
    if (nb > 1)
    {
     int cw = wcwidth(wc);
     int sx = f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1;
     int sy = y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1;
     if (cw < 1) cw = 1;
     j += e_put_wide_cell(sx, sy, (int)wc, cw, frb,
                          j + 1 < COL_NUM_ON_SCREEN_RIGHT);
     i += nb - 1;  /* skip continuation bytes (loop does i++) */
    }
    else
     e_pr_char(f->a.x - NUM_COLS_OFF_SCREEN_LEFT + j + 1,
       y - NUM_LINES_OFF_SCREEN_TOP + f->a.y + 1, '?', frb);
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
  if ((ret == AF2 && !(f->ed->edopt & ED_CUA_STYLE)) ||
    (f->ed->edopt & ED_CUA_STYLE && ret == CtrlL))
   e_size_move(f);
 } while(ret != WPE_CR && ret != WPE_ESC);
 *n = fw->nf;
 e_close_view(f->pic, 1);
 FREE(fw);
 FREE(f);
 return(ret);
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
          (strcpy)(str, (*df)->name[num]);
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

int e_mess_win(char *header, char *str, PIC **pic, FENSTER *f)
{
 ECNT *cn = f->ed;
 extern int (*e_u_kbhit)(void);
#if MOUSE
 extern struct mouse e_mouse;
#endif
 int xa, ya, xe, ye, num, anz = 0, mxlen = 0, i, j;
 char **s;

 s = StringToStringArray(str, &mxlen, strlen(header) + 8, &anz);

 ya = (MAXSLNS - anz - 6)/2;
 ye = ya + anz + 5;
 xa = (MAXSCOL - mxlen - 6)/2;
 xe = xa + mxlen + 6;
 if (ya < 2) ya = 2;
 if (ye > MAXSLNS-3) ye = MAXSLNS - 3;
 num = anz;
 if (num > ye - ya - 5)
 {
  num = ye - ya - 5;
  strcpy(s[num-1], "...");
 }

 if (!(*pic) || (*pic)->e.x != xe || (*pic)->a.x != xa || (*pic)->e.x < xe)
 {
  *pic = e_change_pic(xa, ya, xe, ye, *pic, 1, cn->fb->er.fb);
  for (i = xa + 1; i < xe; i++)
  {
   e_pr_char(i, ye-2, ' ', cn->fb->et.fb);
   e_pr_char(i, ye-1, ' ', cn->fb->et.fb);
  }
  e_pr_str((xe + xa - 6)/2, ye-2, "Ctrl C", cn->fb->nz.fb, -1, -1,
    cn->fb->ns.fb, cn->fb->nt.fb);
 }
 e_std_rahmen(xa, ya, xe, ye, header, 1, cn->fb->er.fb, cn->fb->es.fb);
 for (i = xa + 1; i < xe; i++)
  e_pr_char(i, ya+1, ' ', cn->fb->er.fb);
 for (j = 0; j < num; j++)
 {
  e_pr_char(xa+1, ya+2+j, ' ', cn->fb->et.fb);
  e_pr_char(xa+2, ya+2+j, ' ', cn->fb->et.fb);
  e_pr_str(xa+3, ya+2+j, s[j], cn->fb->et.fb, 0, 0, 0, 0);
  for (i = xa+strlen(s[j])+3; i < xe; i++)
   e_pr_char(i, ya+2+j, ' ', cn->fb->et.fb);
 }
 for (j += ya+2; j < ye-2; j++)
  for (i = xa + 1; i < xe; i++)
   e_pr_char(i, j, ' ', cn->fb->et.fb);
 for (i = 0; i < anz; i++)
  FREE(s[i]);
 FREE(s);
 e_refresh();
 fk_getch();
#ifndef NO_XWINDOWS
 if (WpeIsXwin())
 {
  while ((i = (*e_u_kbhit)()))
  {
   if (i == -1 && e_mouse.y == ye-2 && e_mouse.x > (xe + xa - 10)/2 &&
     e_mouse.x < (xe + xa + 6)/2 )
    i = CtrlC;
   if (i == CtrlC) break;
  }
 }
 else
#endif
  while ((i = (*e_u_kbhit)()) && i != CtrlC)
   ;
 e_mouse_flush();
 return(i == CtrlC ? 1 : 0);
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
      if( (c = e_toupper(e_getch())) == -1)
      c = e_m2_mouse(xa, ya, xe, ye, opt);
#else
      c = e_toupper(e_getch());
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

int e_make_xrect_abs(int xa, int ya, int xe, int ye, int sw)
{
 int j;

 for (j = xa; j <= xe; j++)
  *(extbyte+ya*MAXSCOL+j) = *(extbyte+ye*MAXSCOL+j) = 0;
 for (j = ya; j <= ye; j++)
  *(extbyte+j*MAXSCOL+xa) = *(extbyte+j*MAXSCOL+xe) = 0;
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
   *(extbyte+ya*MAXSCOL+j) |= (sw | 4);
   *(extbyte+ye*MAXSCOL+j) |= (sw | 1);
  }
  for (j = ya+1; j < ye; j++)
  {
   *(extbyte+j*MAXSCOL+xa) |= (sw | 2);
   *(extbyte+j*MAXSCOL+xe) |= (sw | 8);
  }
 }
 else
 {
  sw = (sw & 1) ? 16 : 0;
  for (j = xa; j <= xe; j++)
  {
   *(extbyte+ya*MAXSCOL+j) |= (sw | 1);
   *(extbyte+ye*MAXSCOL+j) |= (sw | 4);
  }
  for (j = ya; j <= ye; j++)
  {
   *(extbyte+j*MAXSCOL+xa) |= (sw | 8);
   *(extbyte+j*MAXSCOL+xe) |= (sw | 2);
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

