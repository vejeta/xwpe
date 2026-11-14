/* we_mouse.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include "we_buffer.h"
#include "we_render.h"
#include "we_mousedrain.h"

#if  MOUSE

#include "makro.h"

/* Highest column the legacy X10 mouse encoding can report (a coordinate is
   sent as one byte with a +32 bias, so 255-32 = 223).  Past that we must turn
   on SGR 1006 extended reporting, which is not byte-limited. */
#define MOUSE_X10_MAX_COL 223

/* xterm private modes for console mouse tracking: 1002 reports button events
   and drags, 1006 switches the reports to SGR encoding (no 223-column cap,
   readable without terminfo).  OSC 22 picks the pointer shape: "default" is
   the arrow, an empty name pops back to the terminal's own pointer. */
#define TERM_MOUSE_BUTTON_TRACKING_ON  "\033[?1002h"
#define TERM_MOUSE_BUTTON_TRACKING_OFF "\033[?1002l"
#define TERM_MOUSE_SGR_REPORTS_ON      "\033[?1006h"
#define TERM_MOUSE_SGR_REPORTS_OFF     "\033[?1006l"
#define TERM_POINTER_SHAPE_ARROW       "\033]22;default\a"
#define TERM_POINTER_SHAPE_RESET       "\033]22;\a"

int e_mouse_cursor();
void e_scroll_drag_v(FENSTER *f);
void e_scroll_drag_h(FENSTER *f);
#ifndef NO_XWINDOWS
static void e_scroll_drag_end_cursor(FENSTER *f);
#endif

void e_mouse_tracking_enable(void)
{
#if MOUSE
#ifdef NCURSES
 mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
 /* Enable button-event tracking, then ALWAYS request SGR (1006) extended
    coordinates -- not just past 223 columns.  SGR is what every modern
    terminal speaks, has no 223 cap, and lets the terminfo-independent SGR
    parser in we_term.c (e_t_sgr_mouse) work on terminals whose curses cannot
    decode mouse input itself (FreeBSD's termcap, OpenBSD's base curses).  On
    ncurses-with-SGR systems (Linux, NetBSD pkgsrc) this is harmless: ncurses
    already enabled SGR and folds the reports into KEY_MOUSE before the parser
    ever sees them. */
 printf(TERM_MOUSE_BUTTON_TRACKING_ON);
 printf(TERM_MOUSE_SGR_REPORTS_ON);
 /* Ask the terminal to show an arrow pointer instead of the text I-beam while
    mouse tracking is active (OSC 22, kitty extension also honoured by iTerm2
    and foot).  The shape names follow CSS, so "default" is the arrow and
    "pointer" is the hyperlink hand -- we want the arrow.  Terminal.app
    ignores unknown OSCs so this is a no-op there. */
 printf(TERM_POINTER_SHAPE_ARROW);
 fflush(stdout);
#endif
#endif
}

void e_mouse_tracking_disable(void)
{
#if MOUSE
#ifdef NCURSES
 /* Empty payload (not "default", which is the CSS arrow) pops back to the
    terminal's own configured cursor on exit. */
 printf(TERM_POINTER_SHAPE_RESET);
 printf(TERM_MOUSE_SGR_REPORTS_OFF);
 printf(TERM_MOUSE_BUTTON_TRACKING_OFF);
 fflush(stdout);
#endif
#endif
}

#if MOUSE
#ifdef NCURSES
/**
 * e_mouse_pull_curses - Read one event from curses' mouse queue.
 * @arg: unused.
 *
 * Lets the bounded drain (wpe_mouse_drain) work on the real queue.  An event
 * with no button state is reported as empty, which is how OpenBSD's curses
 * answers once its queue has run dry.
 *
 * Return: 1 real event, 0 empty event, -1 queue empty.
 */
static int e_mouse_pull_curses(void *arg)
{
 MEVENT mev;
 (void)arg;
 if (getmouse(&mev) != OK)
  return -1;
 if (mev.bstate == 0)
  return 0;
 return 1;
}
#endif
#endif

void e_mouse_flush(void)
{
#if MOUSE
#ifdef NCURSES
 wpe_mouse_drain(e_mouse_pull_curses, NULL);
 e_mouse_tracking_enable();
#endif
 { extern int g_mouse_buttons;
   g_mouse_buttons = 0;
 }
 { extern struct mouse e_mouse;
   e_mouse.k = 0;
 }
#endif
}

int e_mshit()
{
 extern struct mouse e_mouse;
 int g[4]; /* = { 3, 0, 0, 0 };  */
 g[0] = 3;  g[1] = 0;

 fk_mouse(g);
 e_mouse.x = g[2]/8;
 e_mouse.y = g[3]/8;
 return(g[1]);
}

/*     mouse main menu control */
int e_m1_mouse()
{
 extern struct mouse e_mouse;
 extern OPT opt[];
 extern int e_mn_men;
 int c, n;

 if (e_mouse.y == MAXSLNS-1) c = e_m3_mouse();
 else if (e_mouse.y != 0) c = WPE_ESC;
 else
 {
  for (n = 1; n < MENOPT; n++)
   if (e_mouse.x < opt[n].x-e_mn_men) break;
  c = opt[n-1].as;
 }
 return(c);
}

/*   mouse options box control */
int e_m2_mouse(int xa, int ya, int xe, int ye, OPTK *fopt)
{
 extern struct mouse e_mouse;
 int c;

 if (e_mouse.y == MAXSLNS) c = e_m3_mouse();
 else if (e_mouse.y == 0) return(e_m1_mouse());
 else if (e_mouse.x <= xa || e_mouse.x >= xe || e_mouse.y <= ya ||
   e_mouse.y >= ye)
  c = WPE_ESC;
 else  c = fopt[e_mouse.y-ya-1].o;
 while (e_mshit())
  ;
 return(c);
}

/*      Mouse assistant bar control       */
int e_m3_mouse()
{
 extern WOPT *blst;
 extern struct mouse e_mouse;
 extern int nblst;
 int i, last;

 if (e_mouse.y != MAXSLNS-1) return(WPE_ESC);
 while (e_mshit())
  ;
 for(i = 1; i < nblst; i++)
  if(e_mouse.x < blst[i].x) return(blst[i-1].as);
 last = nblst - 1;
 if (e_mouse.x <= blst[last].x + (int)strlen(blst[last].t))
  return(blst[last].as);
 return(WPE_ESC);
}

/*   mouse errors box control */
int e_er_mouse(int x, int y, int xx, int yy)
{
 extern struct mouse e_mouse;

 while (e_mshit() != 0)
  ;
 if (y == e_mouse.y && x == e_mouse.x) return (WPE_CR);
 if (yy == e_mouse.y && xx-1 <= e_mouse.x && xx+4 >= e_mouse.x)
  return (WPE_CR);
 return(0);
}

/*   mouse messages box control */
int e_msg_mouse(int x, int y, int x1, int x2, int yy)
{
 extern struct mouse e_mouse;

 while (e_mshit() != 0)
  ;
 if (y == e_mouse.y && x == e_mouse.x) return (WPE_CR);
 if (yy == e_mouse.y)
 {
  if (x1-1 <= e_mouse.x && x1+5 >= e_mouse.x) return ('Y');
  if (x2-1 <= e_mouse.x && x2+5 >= e_mouse.x) return (WPE_ESC);
  if ((x1=(x1+x2)/2 - 1) <= e_mouse.x && x1+5 >= e_mouse.x) return ('N');
 }
 return(0);
}

static int e_hit_close_button(FENSTER *f)
{
 extern struct mouse e_mouse;
 if (e_mouse.y != f->a.y) return 0;
 return e_mouse.x >= f->e.x - 3 && e_mouse.x <= f->e.x - 1;
}

static int e_hit_maximize_button(FENSTER *f)
{
 extern struct mouse e_mouse;
 if (e_mouse.y != f->a.y) return 0;
 return e_mouse.x == f->e.x - 4;
}

int e_rahmen_mouse(FENSTER *f)
{
 extern struct mouse e_mouse;
 int c = 1;

 if (e_hit_close_button(f))
  c = WPE_ESC;
 else if (e_hit_maximize_button(f))
  e_ed_zoom(f);
 else if (e_mouse.x == f->a.x && e_mouse.y == f->a.y) e_eck_mouse(f, 1);
 else if (e_mouse.x == f->e.x && e_mouse.y == f->a.y) e_eck_mouse(f, 2);
 else if (e_mouse.x == f->e.x && e_mouse.y == f->e.y) e_eck_mouse(f, 3);
 else if (e_mouse.x == f->a.x && e_mouse.y == f->e.y) e_eck_mouse(f, 4);
 else if (e_mouse.y == f->a.y && e_mouse.x > f->a.x && e_mouse.x < f->e.x)
  e_eck_mouse(f, 0);
 else c = 0;
 while (e_mshit() != 0)
  ;
 return(c);
}

int WpeMngMouseInFileManager(FENSTER * f)
{
  extern struct mouse e_mouse;
  ECNT           *cn = f->ed;
  FLBFFR         *b = (FLBFFR *) f->b;
  int             i, c = 0, by = 4;

  if(e_mouse.y == 0)
    return(AltBl);
  else if(e_mouse.y == MAXSLNS - 1)
    return(e_m3_mouse());
  else if(e_mouse.x < f->a.x || e_mouse.x > f->e.x
          || e_mouse.y < f->a.y || e_mouse.y > f->e.y)
  {
    for(i = cn->mxedt; i > 0; i--)
    {
      if(e_mouse.x >= cn->f[i]->a.x && e_mouse.x <= cn->f[i]->e.x
         && e_mouse.y >= cn->f[i]->a.y && e_mouse.y <= cn->f[i]->e.y)
      {
        while(e_mshit() != 0);
        return(cn->edt[i] < 10 ? Alt1 - 1 + cn->edt[i] : 1014 + cn->edt[i]);
      }
    }
  }
  else if(e_hit_close_button(f))
    c = WPE_ESC;            /* close [X] at xe-2: dismiss the dialog */
  else if(e_mouse.x == f->a.x && e_mouse.y == f->a.y)
    e_eck_mouse(f, 1);
  else if(e_mouse.x == f->e.x && e_mouse.y == f->a.y)
    e_eck_mouse(f, 2);
  else if(e_mouse.x == f->e.x && e_mouse.y == f->e.y)
    e_eck_mouse(f, 3);
  else if(e_mouse.x == f->a.x && e_mouse.y == f->e.y)
    e_eck_mouse(f, 4);
  else if(e_mouse.y == f->a.y && e_mouse.x > f->a.x
          && e_mouse.x < f->e.x)
    e_eck_mouse(f, 0);
  else
  {
    if(NUM_LINES_ON_SCREEN <= 17)
      by = -1;
    else if(b->sw != 0 || NUM_LINES_ON_SCREEN <= 19)
      by = 2;

    if(NUM_LINES_ON_SCREEN > 17)
    {
      if(e_mouse.y == f->e.y - by && e_mouse.x >= f->a.x + 3
         && e_mouse.x <= f->a.x + 10)
        c = WPE_ESC;
      else if(e_mouse.y == f->e.y - by && e_mouse.x >= f->a.x + 13
              && e_mouse.x <= f->a.x + 24)
        c = AltC;
      else if(b->sw == 1 && NUM_COLS_ON_SCREEN >= 34 && e_mouse.y == f->e.y - by
              && e_mouse.x >= f->a.x + 27 && e_mouse.x <= f->a.x + 32)
        c = AltR;
      else if(b->sw == 2 && NUM_COLS_ON_SCREEN >= 35 && e_mouse.y == f->e.y - by
              && e_mouse.x >= f->a.x + 27 && e_mouse.x <= f->a.x + 33)
        c = AltW;
      else if(b->sw == 4 && NUM_COLS_ON_SCREEN >= 34 && e_mouse.y == f->e.y - by
              && e_mouse.x >= f->a.x + 27 && e_mouse.x <= f->a.x + 32)
        c = AltS;
      else if(b->sw == 4 && NUM_COLS_ON_SCREEN >= 48 && e_mouse.y == f->e.y - by
              && e_mouse.x >= f->a.x + 35 && e_mouse.x <= f->a.x + 46)
        c = AltY;
      else if(b->sw == 3 && NUM_COLS_ON_SCREEN >= 37 && e_mouse.y == f->e.y - by
              && e_mouse.x >= f->a.x + 27 && e_mouse.x <= f->a.x + 35)
        c = AltE;
      else if(b->sw == 5 && NUM_COLS_ON_SCREEN >= 33 && e_mouse.y == f->e.y - by
              && e_mouse.x >= f->a.x + 27 && e_mouse.x <= f->a.x + 31)
        c = AltA;
      else if(b->sw == 0 && NUM_COLS_ON_SCREEN >= 35 && e_mouse.y == f->e.y - by
              && e_mouse.x >= f->a.x + 27 && e_mouse.x <= f->a.x + 33)
        c = AltK;
      else if(b->sw == 0 && NUM_COLS_ON_SCREEN >= 49 && e_mouse.y == f->e.y - by
              && e_mouse.x >= f->a.x + 36 && e_mouse.x <= f->a.x + 47)
        c = AltA;
    }
    if(b->sw == 0 && NUM_LINES_ON_SCREEN > 19)
    {
      if(e_mouse.y == f->e.y - 2 && e_mouse.x >= f->a.x + 3
         && e_mouse.x <= f->a.x + 8)
        c = AltM;
      else if(e_mouse.y == f->e.y - 2 && NUM_COLS_ON_SCREEN >= 21 &&
              e_mouse.x >= f->a.x + 12 && e_mouse.x <= f->a.x + 19)
        c = AltR;
      else if(e_mouse.y == f->e.y - 2 && NUM_COLS_ON_SCREEN >= 30 &&
              e_mouse.x >= f->a.x + 23 && e_mouse.x <= f->a.x + 28)
        c = AltL;
      else if(e_mouse.y == f->e.y - 2 && NUM_COLS_ON_SCREEN >= 39 &&
              e_mouse.x >= f->a.x + 32 && e_mouse.x <= f->a.x + 37)
        c = AltO;
      else if(e_mouse.y == f->e.y - 2 && NUM_COLS_ON_SCREEN >= 48 &&
              e_mouse.x >= f->a.x + 41 && e_mouse.x <= f->a.x + 46)
        c = AltE;
    }

    if(e_mouse.y == f->a.y + 3 && e_mouse.x >= f->a.x + b->xfa
       && e_mouse.x <= f->a.x + b->xfa + b->xfd)
      c = AltN;
    else if(e_mouse.y == f->a.y + 3 && e_mouse.x >= f->a.x + b->xda
            && e_mouse.x <= f->a.x + b->xda + b->xdd)
      c = AltD;
    else if(e_mouse.y >= b->fw->ya && e_mouse.y <= b->fw->ye
            && e_mouse.x >= b->fw->xa && e_mouse.x <= b->fw->xe)
      c = AltF;
    else if(e_mouse.y >= b->dw->ya && e_mouse.y <= b->dw->ye
            && e_mouse.x >= b->dw->xa && e_mouse.x <= b->dw->xe)
      c = AltT;
  }
  while(e_mshit() != 0);
  return(c);
}

/**
 * WpeDragTarget - The path a dragged file gets in the window it is dropped on.
 * @f: the file manager the drag started in (where the box is shown);
 * @tmp/@n: receive the path; @dirct: the folder of the window dropped on;
 * @name: the dragged file's name.
 *
 * A folder and a name that each fit the system can still be too long
 * together.  The drop then does nothing and the user is told so, instead of
 * the file going to a path that is not the one they dropped it on.
 * Return: 0, or -1 when the path does not fit (the box was shown).
 */
static int WpeDragTarget(FENSTER *f, char *tmp, size_t n, const char *dirct, const char *name)
{
  char            msg[WPE_PATH_MSG_MAX];

  if(wpe_path_join(tmp, n, dirct, name) == 0)
    return(0);
  snprintf(msg, sizeof msg, WPE_PATH_TOO_LONG_FMT, "target file", WPE_PATH_MAX);
  e_error(msg, 0, f->fb);
  return(-1);
}

int WpeMouseInFileDirList(int k, int sw, FENSTER * f)
{
  extern struct mouse e_mouse;
  ECNT           *cn = f->ed;
  FLBFFR         *b = (FLBFFR *) f->b;
  int             i;
  char            tmp[WPE_PATH_SIZE];

  if(e_mouse.x >= f->a.x && e_mouse.x <= f->e.x
     && e_mouse.y >= f->a.y && e_mouse.y <= f->e.y)
    return(0);
  for(i = cn->mxedt - 1; i > 0; i--)
  {
    if(e_mouse.x >= cn->f[i]->a.x && e_mouse.x <= cn->f[i]->e.x
       && e_mouse.y >= cn->f[i]->a.y && e_mouse.y <= cn->f[i]->e.y)
      break;
  }
  if(i <= 0)
    return(0);
  if(sw)
  {
    if(WpeDragTarget(f, tmp, sizeof tmp, cn->f[i]->dirct, b->dd->name[b->dw->nf - b->cd->anz]) != 0)
      return(0);
#ifndef DJGPP
    if(k == -2)
      e_copy(b->dd->name[b->dw->nf - b->cd->anz], tmp, f);
    else if(k == -4)
      e_link(b->dd->name[b->dw->nf - b->cd->anz], tmp, f);
#else
    if(k == -4)
      e_copy(b->dd->name[b->dw->nf - b->cd->anz], tmp, f);
#endif
    else
      e_rename(b->dd->name[b->dw->nf - b->cd->anz], tmp, f);
    freedf(b->cd);
    freedf(b->dw->df);
    freedf(b->dd);
    b->dd = e_find_dir(SUDIR, f->ed->flopt & FM_SHOW_HIDDEN_DIRS ? 1 : 0);
    b->cd = WpeCreateWorkingDirTree(f->save, cn);  /* ??? cn */
    b->dw->df = WpeGraphicalDirTree(b->cd, b->dd, cn);
    b->dw->nf = b->cd->anz - 1;
    b->dw->ia = b->dw->ja = 0;
    e_pr_file_window(b->dw, 0, 1, f->fb->ft.fb, f->fb->fz.fb, f->fb->frft.fb);
  }
  else
  {
    if(WpeDragTarget(f, tmp, sizeof tmp, cn->f[i]->dirct, b->df->name[b->fw->nf]) != 0)
      return(0);
#ifndef DJGPP
    if(k == -2)
      e_copy(b->df->name[b->fw->nf], tmp, f);
    else if(k == -4)
      e_link(b->df->name[b->fw->nf], tmp, f);
#else
    if(k == -4)
      e_copy(b->df->name[b->fw->nf], tmp, f);
#endif
    else
      e_rename(b->df->name[b->fw->nf], tmp, f);
    freedf(b->df);
    freedf(b->fw->df);
    b->df = e_find_files(b->rdfile, f->ed->flopt & FM_SHOW_HIDDEN_DIRS ? 1 : 0);
    b->fw->df = WpeGraphicalFileList(b->df, f->ed->flopt >> 9, cn);
    b->fw->ia = b->fw->nf = 0;
    b->fw->ja = b->fw->srcha;
    e_pr_file_window(b->fw, 0, 1, f->fb->ft.fb, f->fb->fz.fb, f->fb->frft.fb);
  }
  return(cn->edt[i] < 10 ? Alt1 - 1 + cn->edt[i] : 1014 + cn->edt[i]);
}


/*  File Window  */
char *e_gt_btstr(int x, int y, int n, char *buffer)
{
 if (y < 0 || y >= MAXSLNS || x < 0) return(buffer);
 if (x + n > MAXSCOL) n = MAXSCOL - x;
 if (n > 0)
  memcpy(buffer, &schirm[y * MAXSCOL + x], n * sizeof(SCREENCELL));
 return(buffer);
}

char *e_pt_btstr(int x, int y, int n, char *buffer)
{
 if (y < 0 || y >= MAXSLNS || x < 0) return(buffer);
 if (x + n > MAXSCOL) n = MAXSCOL - x;
 if (n > 0)
  memcpy(&schirm[y * MAXSCOL + x], buffer, n * sizeof(SCREENCELL));
 return(buffer);
}

int fl_wnd_mouse(sw, k, fw)
     int sw;
     int k;
     FLWND *fw;
{
   extern struct mouse e_mouse;
   int MLEN, i, c, xa, ya, xn, yn, xdif;
   char *file, *bgrd;
   if(e_mouse.x == fw->xe && e_mouse.y >= fw->ya && e_mouse.y < fw->ye)
   {  fw->nf = e_lst_mouse(fw->xe, fw->ya, fw->ye-fw->ya, 0,
						fw->df->anz, fw->nf);
      return(0);
   }
   else if(e_mouse.y == fw->ye && e_mouse.x >= fw->xa && e_mouse.x < fw->xe)
   {  fw->ja = e_lst_mouse(fw->xa, fw->ye, fw->xe-fw->xa, 1,
				e_fl_sel_len(fw), fw->ja);
      return(0);
   }
   else if(e_mouse.y >= fw->ya && e_mouse.y < fw->ye &&
			 e_mouse.x >= fw->xa && e_mouse.x < fw->xe)
   {  if(fw->nf == e_mouse.y - fw->ya + fw->ia
		 && e_mouse.y - fw->ya + fw->ia < fw->df->anz)
      {  /*  if(k == -2) {  while (e_mshit() != 0);  return(AltU);  }
	 else if(k == -4) {  while (e_mshit() != 0);  return(AltM);  }
	 else   */
	 {  xa = e_mouse.x; ya = e_mouse.y; xdif = e_mouse.x - fw->xa;
	    if(fw->srcha >= 0) c = fw->srcha;
	    else
	    {  for(c = 0; *(fw->df->name[fw->nf]+c)
			&& ( *(fw->df->name[fw->nf]+c) <= 32
			  || *(fw->df->name[fw->nf]+c) >= 127); c++);
	       if(!WpeIsXwin()) c += 3;
	    }
	    for(MLEN = 1; *(fw->df->name[fw->nf]+c+MLEN)
			&& *(fw->df->name[fw->nf]+c+MLEN) != ' '; MLEN++);
	    /* MLEN is the number of cells (not bytes) */
	    file = MALLOC(MLEN * sizeof(SCREENCELL));
	    bgrd = MALLOC(MLEN * sizeof(SCREENCELL));
	    while (e_mshit() != 0)
	    if(sw && (e_mouse.x != xa || e_mouse.y != ya))
	    {  xn = e_mouse.x;  yn = e_mouse.y;
	       if(fw->srcha < 0)
	       {  FLBFFR *b = (FLBFFR *)fw->f->b;
		  if(b->cd->anz > fw->nf)
		  {  while (e_mshit() != 0);
		     FREE (file);  FREE (bgrd);  return(WPE_CR);
		  }
		  for(c = 0; *(fw->df->name[fw->nf]+c)
			&& ( *(fw->df->name[fw->nf]+c) <= 32
			  || *(fw->df->name[fw->nf]+c) >= 127); c++);
		  if(!WpeIsXwin()) c += 3;
		  xdif -= (c + 1);
	       }
	       { SCREENCELL *fcells = (SCREENCELL *)file;
	         for (i = 0; i < MLEN; i++)
	         {  fcells[i].ch = *(fw->df->name[fw->nf]+c+i);
		    fcells[i].attr = fw->f->fb->fz.fb;
		    fcells[i].flags = 0;
	         }
	       }
	       e_gt_btstr(e_mouse.x-xdif, e_mouse.y, MLEN, bgrd);
	       while (e_mshit() != 0)
	       {  e_pt_btstr(xn-xdif, yn, MLEN, bgrd);
		  xn = e_mouse.x;  yn = e_mouse.y;
		  e_gt_btstr(e_mouse.x-xdif, e_mouse.y, MLEN, bgrd);
		  e_pt_btstr(e_mouse.x-xdif, e_mouse.y, MLEN, file);
		  e_refresh();
	       }
	       e_pt_btstr(xn-xdif, yn, MLEN, bgrd);
	       FREE (file);  FREE (bgrd);
	       return(k);
	    }
	    FREE (file);  FREE (bgrd);
	    if(sw && k == -2) return(AltU);
	    else if(sw && k == -4) return(AltM);
	    else return(WPE_CR);
	 }
      }
      else {  while (e_mshit() != 0);
	      fw->nf = e_mouse.y - fw->ya + fw->ia;  return(0);  }
   }
   else return(MBKEY);
}

static void e_scroll_invalidate_content(FENSTER *f)
{
 extern SCREENCELL *altschirm;
 extern int MAXSCOL;
 int row, col;

 for (row = f->a.y + 1; row < f->e.y; row++)
  for (col = f->a.x + 1; col <= f->e.x; col++)
   memset(&altschirm[row * MAXSCOL + col], 0, sizeof(SCREENCELL));
}

static void e_scroll_render_lines(FENSTER *f)
{
 int j;
 int first = f->b->b.y;
 int visible = f->e.y - f->a.y - 1;
 int last = first + visible;

 f->s->c.y = first;

 for (j = first; j < f->b->mxlines && j < last; j++)
  e_pr_line(j, f);
 for (; j < last; j++)
  e_blk(f->e.x - f->a.x - 2, f->a.x + 1,
    j - first + f->a.y + 1, f->fb->et.fb);
}

static void e_scroll_content(FENSTER *f)
{
 e_scroll_invalidate_content(f);
 e_scroll_render_lines(f);
}

#ifndef NO_XWINDOWS  /* scrollbar fluid-drag helpers: X11-only (Xlib events) */
static void e_scroll_render_h(FENSTER *f, int new_bx)
{
 f->s->c.x = new_bx;
 e_schirm(f, 0);
 e_cursor_pos_only(f);
 e_refresh();
}

static int e_scroll_pixel_to_pos(int px, int track_start, int track_end,
                                 int total, int visible)
{
 int pos;
 if (px < track_start) px = track_start;
 if (px > track_end) px = track_end;
 pos = (int)((double)(px - track_start) / (track_end - track_start)
       * (total - visible));
 if (pos < 0) pos = 0;
 if (pos > total - visible) pos = total - visible;
 return pos;
}

/**
 * e_scroll_drag_begin - A scrollbar thumb is grabbed: the drag owns the
 * screen until the button is let go.
 *
 * A stop, program output or a reply that comes while the user drags is
 * shown once the drag is over, alike on X11 and Wayland (whose drag waits
 * on the editor's loop and so runs its callbacks meanwhile).
 */
static void e_scroll_drag_begin(void)
{
 wpe_paint_modal_enter("a scrollbar drag");
 wpe_scroll_dragging = 1;
 fk_u_grab_pointer(1);
}

/**
 * e_scroll_drag_end - The button is let go: the pointer, the text cursor
 * and the screen are the editor's again.
 * @f: the window dragged; @saved_cur_on: the cursor's state before the drag.
 */
static void e_scroll_drag_end(FENSTER *f, int saved_cur_on)
{
 fk_u_grab_pointer(0);
 wpe_scroll_dragging = 0;
 cur_on = saved_cur_on;
 e_scroll_drag_end_cursor(f);
 wpe_paint_modal_leave();
}
#endif  /* NO_XWINDOWS */

void e_scroll_drag_h(FENSTER *f)
{
#ifndef NO_XWINDOWS
 int fw = WpeRender.font_width;
 int track_l = (f->a.x + 20) * fw;
 int track_r = (f->e.x - 3) * fw;
 int max_x = f->b->mx.x;
 int visible = f->e.x - f->a.x - 1;
 int old_bx = f->b->b.x;

 if (track_r - track_l < 1 || max_x <= visible)
  return;

 e_scroll_drag_begin();
 for (;;)
 {
  int px, py, new_bx, rc;
  rc = fk_u_drag_next(&px, &py);
  if (rc == 0) break;
  if (rc < 0) continue;

  new_bx = e_scroll_pixel_to_pos(px, track_l, track_r, max_x, visible);
  if (new_bx == old_bx) continue;

  e_scroll_render_h(f, new_bx);
  old_bx = new_bx;
 }
 e_scroll_drag_end(f, cur_on);
#endif
}

#ifndef NO_XWINDOWS
static void e_scroll_drag_end_cursor(FENSTER *f)
{
 int vp_left = f->s->c.x;
 int vp_right = vp_left + (f->e.x - f->a.x - 2);
 int vp_top = f->s->c.y;
 int vp_bottom = vp_top + (f->e.y - f->a.y - 2);
 int col = e_cursor_display_col(f);
 int row = f->b->b.y;

 /* The viewport is counted in screen columns, so the cursor is compared by
    the column it is shown in, not by its byte. */
 if (col < vp_left || col > vp_right)
  col = vp_left;
 if (row < vp_top || row > vp_bottom)
  row = (vp_top + vp_bottom) / 2;
 e_buffer_cursor_set_col(f->b, row, col);

 e_cursor_pos_only(f);
 e_schirm(f, 1);
 e_refresh();
}
#endif  /* NO_XWINDOWS */

void e_scroll_drag_v(FENSTER *f)
{
#ifndef NO_XWINDOWS
 int fh = WpeRender.font_height;
 int track_t = (f->a.y + 2) * fh;
 int track_b = (f->e.y - 1) * fh;
 int total = f->b->mxlines;
 int visible = f->e.y - f->a.y - 1;
 int old_by = f->b->b.y;

 if (track_b - track_t < 1 || total <= visible)
  return;

 e_scroll_drag_begin();
 for (;;)
 {
  int px, py, new_by, rc;
  rc = fk_u_drag_next(&px, &py);
  if (rc == 0) break;
  if (rc < 0) continue;

  new_by = e_scroll_pixel_to_pos(py, track_t, track_b, total, visible);
  if (new_by == old_by) continue;

  f->s->c.y = new_by;
  e_schirm(f, 0);
  e_cursor_pos_only(f);
  e_refresh();
  old_by = new_by;
 }
 e_scroll_drag_end(f, cur_on);
#endif
}

/*   mouse slider bar control */
int e_lst_mouse(x, y, n, sw, max, nf)
     int x;
     int y;
     int n;
     int sw;
     int max;
     int nf;
{
   extern struct mouse e_mouse;
   int g[4];  /*  = { 1, 0, 0, 0 };  */
   int inew, iold, nret, frb = e_gt_col(x, y);
   double d;
   if(n < 2) return(nf);
   d = ((double)max)/((double)(n-2));
   g[0] = 1;
   if(sw == 0)
   {  if(e_mouse.x != x) return(nf);
      if(e_mouse.y < y || e_mouse.y >= y+n) return(nf);
      if(e_mouse.y == y) nret= (nf < 1) ? nf : nf-1;
      else if(e_mouse.y == y+n-1) nret= (nf >= max-1) ? nf : nf+1;
      else
      {  nret = (int) ((e_mouse.y-y-1)*d);
	 if(e_gt_char(e_mouse.x, e_mouse.y) == MCA
	    || wpe_chrome_hit_vthumb(e_mouse.x, e_mouse.y))
	 {  iold = e_mouse.y;
#ifdef NEWSTYLE
	    e_make_xrect_abs(x, iold, x, iold, 1);
	    e_refresh();
#endif
	    fk_mouse(g);
	    for(g[1]=1; g[1] != 0; fk_mouse(g), g[0]=3)
	    {  if((inew = g[3]/8) < y+1) inew = y+1;
	       if(inew > y+n-2) inew = y+n-2;
	       if(iold != inew)
	       {  g[0] = 2; fk_mouse(g);
		  e_pr_char(x, iold, MCI, frb);
		  e_pr_char(x, inew, MCA, frb);
#ifdef NEWSTYLE
		  e_make_xrect(x, y+1, x, y+n-2, 0);
		  e_make_xrect_abs(x, inew, x, inew, 1);
#endif
		  e_refresh();
		  iold = inew;
		  g[0] = 1; fk_mouse(g);
	       };
	    }
	    g[0] = 2;
	    fk_mouse(g);
	    e_pr_char(x, inew, MCI, frb);
	    if(inew-y < 2) nret = 0;
	    else if(inew-y > n - 3) nret = max - 1;
	    else nret = (int) ((inew-y-0.5)*d);
	 }
	 else if(nf < nret)
	 {  nret = (nf+n > max) ? max : nf+n-1;
	    while (e_mshit() != 0);
	 }
	 else
	 {  nret = (nf-n < 0) ? 0 : nf-n+1;
	    while (e_mshit() != 0);
	 }
      }
   }
   else
   {  if(e_mouse.y != y) return(nf);
      if(e_mouse.x < x || e_mouse.x >= x+n) return(nf);
      if(e_mouse.x == x) nret= (nf < 1) ? nf : nf-1;
      else if(e_mouse.x == x+n-1) nret= (nf >= max-1) ? nf : nf+1;
      else
      {  nret = (int) ((e_mouse.x-x-1)*d);
	 if( e_gt_char(e_mouse.x, e_mouse.y) == MCA )
	 {  iold = e_mouse.x;
#ifdef NEWSTYLE
	    e_make_xrect_abs(iold, y, iold, y, 1);
	    e_refresh();
#endif
	    fk_mouse(g);
	    for(g[1]=1; g[1] != 0; fk_mouse(g), g[0]=3)
	    {  if((inew = g[2]/8) < x+1) inew = x+1;
	       if(inew > x+n-2) inew = x+n-2;
	       if(iold != inew)
	       {  g[0] = 2; fk_mouse(g);
		  e_pr_char(iold, y, MCI, frb);
		  e_pr_char(inew, y, MCA, frb);
#ifdef NEWSTYLE
		  e_make_xrect(x+1, y, x+n-2, y, 0);
		  e_make_xrect_abs(inew, y, inew, y, 1);
#endif
		  e_refresh();
		  iold = inew;
		  g[0] = 1; fk_mouse(g);
	       };
	    }
	    g[0] = 2;
	    fk_mouse(g);
	    e_pr_char(inew, y, MCI, frb);
	    if(inew-y < 2) nret = 0;
	    else if(inew-y > n - 3) nret = max - 1;
	    else nret = (int) ((inew-y-0.5)*d);
	 }
	 else if(nf < nret)
	 {  nret = (nf+n > max) ? max : nf+n-1;
	    while (e_mshit() != 0);
	 }
	 else
	 {  nret = (nf-n < 0) ? 0 : nf-n+1;
	    while (e_mshit() != 0);
	 }
      }
   }
   return(nret);
}

/*   mouse window resizer control */

void e_eck_mouse(FENSTER *f, int sw)
{
 int g[4];  /*  = { 3, 1, 0, 0 };  */
 int xold, yold, x, y, xa, xmin = 26, ymin = 3;
 POINT fa, fe;

 e_ed_rahmen(f, 0);
 memcpy(&fa, &f->a, sizeof(POINT));
 memcpy(&fe, &f->e, sizeof(POINT));
 g[0] = 3;
 g[1] = 1;
 fk_mouse(g);
 xold = g[2]/8;
 yold = g[3]/8;
 xa = xold - f->a.x;
 if (f->dtmd == DTMD_FILEDROPDOWN)
  xmin = 15;
 else if (!DTMD_ISTEXT(f->dtmd))
  ymin = 9;
 while(g[1] != 0)
 {
  x = g[2]/8;
  y = g[3]/8;
  if (y < 1)
   y = 1;
  else if (y > MAXSLNS-2)
   y = MAXSLNS-2;
  if (x < 0)
   x = 0;
  else if (x > MAXSCOL-1)
   x = MAXSCOL-1;
  if (xold != x || yold != y)
  {
   xold = x;
   yold = y;
   if (sw == 0)
   {
    x -= xa;
    if (x < 0)
     x = 0;
    else if (x + NUM_COLS_ON_SCREEN > MAXSCOL-1)
     x = MAXSCOL - f->e.x + f->a.x - 1;
    if (f->e.y + y - f->a.y > MAXSLNS-2)
     y = MAXSLNS - f->e.y + f->a.y - 2;
    f->e.x = NUM_COLS_ON_SCREEN + x;
    f->a.x = x;
    f->e.y = f->e.y + y - f->a.y;
    f->a.y = y;
   }
   else if (sw == 1)
   {
    if (x > f->e.x - xmin)
     x = f->e.x - xmin;
    if (y > f->e.y - ymin)
     y = f->e.y - ymin;
    f->a.x = x;
    f->a.y = y;
   }
   else if (sw == 2)
   {
    if (x < f->a.x + xmin)
     x = f->a.x + xmin;
    if (y > f->e.y - ymin)
     y = f->e.y - ymin;
    f->e.x = x;
    f->a.y = y;
   }
   else if (sw == 3)
   {
    if (x < f->a.x + xmin)
     x = f->a.x + xmin;
    if (y < f->a.y + ymin)
     y = f->a.y + ymin;
    f->e.x = x;
    f->e.y = y;
   }
   else if (sw == 4)
   {
    if (x > f->e.x - xmin)
     x = f->e.x - xmin;
    if (y < f->a.y + ymin)
     y = f->a.y + ymin;
    f->a.x = x;
    f->e.y = y;
   }
   g[0] = 2;
   fk_mouse(g);
   if (f->dtmd == DTMD_FILEDROPDOWN)
   {
    FLWND *fw = (FLWND*) f->b;
    fw->xa = f->a.x+1;
    fw->xe = f->e.x;
    fw->ya = f->a.y+1;
    fw->ye = f->e.y;
   }
   e_move_window_recompose(f);
   g[0] = 1;
   fk_mouse(g);
  }
  g[0] = 3;
  fk_mouse(g);
 }
 if ((memcmp(&fa, &f->a, sizeof(POINT))) ||
   (memcmp(&fe, &f->e, sizeof(POINT))))
  f->zoom = 0;
 e_ed_rahmen(f, 1);
}

/*       Mouse edit window control   */
int e_edt_mouse(int c, FENSTER *f)
{
 int i, ret = 0;
 extern struct mouse e_mouse;
 ECNT *cn = f->ed;

 if (e_mouse.y == 0) return(e_m1_mouse());
 else if (e_mouse.y == MAXSLNS-1) return(e_m3_mouse());
 else if (e_mouse.x >= f->a.x && e_mouse.x <= f->e.x &&
   e_mouse.y >= f->a.y && e_mouse.y <= f->e.y)
 {
  if (e_hit_close_button(f))
   ret = CtrlW;     /* close window: CtrlW closes in both CUA and default mode */
  else if (e_hit_maximize_button(f))
   e_ed_zoom(f);
  else if (e_mouse.x == f->a.x && e_mouse.y == f->a.y) e_eck_mouse(f, 1);
  else if (e_mouse.x == f->e.x && e_mouse.y == f->a.y) e_eck_mouse(f, 2);
  else if (e_mouse.x == f->e.x && e_mouse.y == f->e.y) e_eck_mouse(f, 3);
  else if (e_mouse.x == f->a.x && e_mouse.y == f->e.y) e_eck_mouse(f, 4);
  else if (e_mouse.y == f->a.y) e_eck_mouse(f, 0);
  else if (e_mouse.y == f->e.y && e_mouse.x > f->a.x + 4 &&
    e_mouse.x < f->a.x + 14) ret = AltG;
  else if (e_mouse.y == f->e.y && e_mouse.x == f->a.x + 15 && f->ins != 8)
  {
   if (f->ins & 1) f->ins &= ~1;
   else f->ins |= 1;
   e_pr_filetype(f);
  }
  else if (e_mouse.y == f->e.y && e_mouse.x == f->a.x + 16 && f->ins != 8)
  {
   if (f->ins & 2) f->ins &= ~2;
   else f->ins |= 2;
   e_pr_filetype(f);
  }
  else if (e_mouse.x > f->a.x && e_mouse.x < f->e.x &&
    e_mouse.y > f->a.y && e_mouse.y < f->e.y)
  {
   if (c < -1) ret = e_ccp_mouse(c, f);
   else if (f->dtmd == DTMD_HELP && f->ins == 8 &&
     f->b->b.y == e_mouse.y-f->a.y+NUM_LINES_OFF_SCREEN_TOP-1 &&
     ((i = f->b->b.x) == e_mouse_cursor(f->b, f->s, f)))
    ret = WPE_CR;
   else
#ifdef PROG
#ifdef DEBUGGER
   {
    if (WpeIsProg() && (!strcmp(f->datnam, "Watches") ||
      !strcmp(f->datnam, "Messages") ||
      !strcmp(f->datnam, "Stack")))
     ret = e_d_car_mouse(f);
    else
     e_cur_mouse(f);
   }
#else
   {
    if (WpeIsProg() && !strcmp(f->datnam, "Messages"))
     ret = e_d_car_mouse(f);
    else
     e_cur_mouse(f);
   }
#endif
#else
    e_cur_mouse(f);
#endif
  }
  else if (e_mouse.x == f->e.x && e_mouse.y == f->a.y+1)
  {
   /*changed the while()... to a do...while();  :Mark L*/
   do
   {
    e_buffer_cursor_set_col(f->b, f->b->b.y - 1, f->b->clsv);
    e_cursor(f, 1);
    e_refresh();
   } while(e_mshit());
  }
  else if (e_mouse.x == f->e.x && e_mouse.y == f->e.y-1)
  {
   /*changed the while()... to a do...while();  :Mark L*/
   do
   {
    e_buffer_cursor_set_col(f->b, f->b->b.y + 1, f->b->clsv);
    e_cursor(f, 1);
    e_refresh();
   } while(e_mshit());
  }
  else if (e_mouse.x == f->e.x &&
    e_mouse.y > f->a.y+1 && e_mouse.y < f->e.y-1)
  {
   if (wpe_chrome_hit_vthumb(e_mouse.x, e_mouse.y)
       || e_gt_char(e_mouse.x, e_mouse.y) == MCA)
    e_scroll_drag_v(f);
   else
   {
    e_buffer_cursor_set_col(f->b, e_lst_mouse(f->e.x, f->a.y+1,
      f->e.y-f->a.y-1, 0, f->b->mxlines, f->b->b.y), f->b->clsv);
    e_cursor(f, 1);
    e_refresh();
   }
  }
  else if (e_mouse.y == f->e.y && e_mouse.x == f->a.x+19)
  {
   while (e_mshit())
   {
    e_buffer_cursor_step(f->b, -1);
    e_cursor(f, 1);
    e_refresh();
   }
  }
  else if (e_mouse.y == f->e.y && e_mouse.x == f->e.x-2)
  {
   while (e_mshit())
   {
    e_buffer_cursor_step(f->b, 1);
    e_cursor(f, 1);
    e_refresh();
   }
  }
  else if (e_mouse.y == f->e.y &&
    e_mouse.x > f->a.x+19 && e_mouse.x < f->e.x-2)
  {
   if (wpe_chrome_hit_hthumb(e_mouse.x, e_mouse.y)
       || e_gt_char(e_mouse.x, e_mouse.y) == MCA)
    e_scroll_drag_h(f);
   else
   {
    e_buffer_cursor_set_col(f->b, f->b->b.y, e_lst_mouse(f->a.x+19, f->e.y,
      f->e.x-f->a.x-20, 1, f->b->mx.x, NUM_COLS_OFF_SCREEN_LEFT));
    e_cursor(f, 1);
    e_refresh();
   }
  }
 }
 else
 {
  for (i = cn->mxedt; i > 0; i--)
  {
   if (e_mouse.x >= cn->f[i]->a.x && e_mouse.x <= cn->f[i]->e.x &&
     e_mouse.y >= cn->f[i]->a.y && e_mouse.y <= cn->f[i]->e.y)
   {
#ifdef PROG
    if (WpeIsProg())
     e_d_note_messages_focus_click(cn->f[i]);
#endif
    ret = cn->edt[i] < 10 ? Alt1-1+cn->edt[i] : 1014+cn->edt[i];
    break;
   }
  }
 }
 while (e_mshit()!= 0)
  ;
 return(ret);
}

int e_mouse_cursor(BUFFER *b, SCHIRM *s, FENSTER *f)
{
 extern struct mouse e_mouse;
 int mx = e_mouse.x;

 /* If click lands on a wide-char spacer cell, back up to the wide char */
 if (mx >= 0 && mx < MAXSCOL && e_mouse.y >= 0 && e_mouse.y < MAXSLNS &&
     (e_gt_flags(mx, e_mouse.y) & CELL_WIDE_SPACER) && mx > 0)
  mx--;

 e_buffer_cursor_set_col(b, e_mouse.y-f->a.y+s->c.y-1, mx-f->a.x+s->c.x-1);
 return(b->b.x);
}

/*  Copy, Cut and Paste functions    */
int e_ccp_mouse(int c, FENSTER *f)
{
 BUFFER *b = f->ed->f[f->ed->mxedt]->b;
 SCHIRM *s = f->ed->f[f->ed->mxedt]->s;

 while (e_mshit() != 0)
  ;
 if (c == -2)
 {
  e_mouse_cursor(b, s, f);
  return((bioskey() & 8) ? AltEin : ShiftEin);
 }
 else if (c == -4)
 {
  return((bioskey() & 3) ? ShiftDel : ((bioskey() & 8) ? AltDel : CEINFG));
 }
 else return(0);
}

/* Is the place (@ay, @ax) at or before the place (@zy, @zx) in the text? */
static int e_mouse_not_after(int ay, int ax, int zy, int zx)
{
 return(ay < zy || (ay == zy && ax <= zx));
}

/**
 * e_mouse_select_line - A second click on a selected word selects the whole
 * row the cursor is on.
 */
static void e_mouse_select_line(BUFFER *b)
{
 POINT first, last;

 first.y = last.y = b->b.y;
 first.x = 0;
 last.x = b->bf[b->b.y].len;
 e_buffer_block_set(b, &first, &last);
}

/**
 * e_mouse_select_word - A click that does not move the cursor selects the
 * word under it: the letters, digits and underscores on both sides.
 */
static void e_mouse_select_word(BUFFER *b)
{
 const STRING *row = &b->bf[b->b.y];
 POINT first, last;

 first.y = last.y = b->b.y;
 for (first.x = b->b.x; first.x > 0 && isalnum1(row->s[first.x-1]); first.x--)
  ;
 for (last.x = b->b.x; last.x < row->len && isalnum1(row->s[last.x]); last.x++)
  ;
 e_buffer_block_set(b, &first, &last);
}

/* The block runs from its end @kept (E_BUFFER_MARK_BEGIN or _END) to the
   cursor, and that end is the anchor a drag goes on from. */
static void e_mouse_block_keeps(BUFFER *b, int kept)
{
 e_buffer_block_between(b, kept, E_BUFFER_POS_CURSOR);
 e_buffer_mark_from(b, E_BUFFER_MARK_ANCHOR, kept);
}

/**
 * e_mouse_extend_block - A Shift click moves the end of the block that the
 * click is on the side of, and keeps the other end as the anchor the drag
 * goes on from.
 * @bs: where the cursor stood before the click.
 *
 * A click after the block moves its end; before the block, its begin; inside
 * it, the end the cursor was nearer to before the click.
 */
static void e_mouse_extend_block(BUFFER *b, SCHIRM *s, const POINT *bs)
{
 if (e_mouse_not_after(s->mark_end.y, s->mark_end.x, b->b.y, b->b.x))
  e_mouse_block_keeps(b, E_BUFFER_MARK_BEGIN);
 else if (e_mouse_not_after(b->b.y, b->b.x, s->mark_begin.y, s->mark_begin.x) ||
          e_mouse_not_after(s->mark_end.y, s->mark_end.x, bs->y, bs->x))
  e_mouse_block_keeps(b, E_BUFFER_MARK_END);
 else
  e_mouse_block_keeps(b, E_BUFFER_MARK_BEGIN);
}

/*       Mouse cursor in edit window control   */
void e_cur_mouse(f)
     FENSTER *f;
{
   BUFFER *b = f->ed->f[f->ed->mxedt]->b;
   SCHIRM *s = f->ed->f[f->ed->mxedt]->s;
   POINT bs;
   bs.x = b->b.x;  bs.y = b->b.y;
   e_mouse_cursor(b, s, f);
   if((bioskey() & 3) == 0)
   {  if(b->b.x == bs.x && b->b.y == bs.y && f->dtmd != DTMD_HELP)
      {  if(s->mark_begin.y == b->b.y && s->mark_end.y == b->b.y
		&& s->mark_begin.x <= b->b.x && s->mark_end.x > b->b.x)
	    e_mouse_select_line(b);
	 else
	    e_mouse_select_word(b);
	 e_schirm(f, 1);
      }
      else
	 e_selection_end(f);
      e_buffer_mark_from(b, E_BUFFER_MARK_ANCHOR, E_BUFFER_POS_CURSOR);
   }
   else
      e_mouse_extend_block(b, s, &bs);
   WpeMouseChangeShape(WpeSelectionShape);
   while(e_mshit() != 0)
   {  bs.x = b->b.x;  bs.y = b->b.y;
      e_mouse_cursor(b, s, f);
      if(b->b.x != bs.x || b->b.y != bs.y)
	 e_buffer_block_between(b, E_BUFFER_MARK_ANCHOR, E_BUFFER_POS_CURSOR);
      e_cursor(f, 1);
      e_schirm(f, 1);
      e_refresh();
   }
   e_buffer_mark_from(b, E_BUFFER_MARK_ANCHOR, E_BUFFER_POS_CURSOR);
   WpeMouseRestoreShape();
   e_cursor(f, 1);
}

int e_opt_ck_mouse(xa, ya, md)
     int xa;
     int ya;
     int md;
{
   extern struct mouse e_mouse;
   if(e_mouse.x < xa-2 || e_mouse.x > xa+25
			|| e_mouse.y < ya-1 || e_mouse.y > ya+18) return(WPE_CR);
   if(e_mouse.x >= xa && e_mouse.x < xa+24
			&& e_mouse.y > ya && e_mouse.y < ya+17)
   return(1000+(e_mouse.y-ya-1)*16 + (e_mouse.x - xa)/3);
   else
   return(0);
}

int e_opt_cw_mouse(xa, ya, md)
     int xa;
     int ya;
     int md;
{
   extern struct mouse e_mouse;
   if(e_mouse.y == 0 || e_mouse.y == MAXSLNS-1) return(WPE_ESC);
   if(e_mouse.y == 1 && e_mouse.x == 3) return(WPE_ESC);
   if(e_mouse.x >= xa-30 && e_mouse.x <= xa-3
			&& e_mouse.y >= ya && e_mouse.y <= ya+19) return(WPE_CR);
   if(e_mouse.x >= 1 && e_mouse.x <= 33
			&& e_mouse.y >= 2 && e_mouse.y <= 21)
   {  if(md == 1) return(e_opt_bs_mouse_1());
      else if(md == 2) return(e_opt_bs_mouse_2());
      else if(md == 3) return(e_opt_bs_mouse_3());
      else if(md == COLOR_MODE_AI) return(e_opt_bs_mouse_ai());
      else return(e_opt_bs_mouse());
   }
   if(e_mouse.x > xa && e_mouse.x < xa+12
			&& e_mouse.y > ya && e_mouse.y < ya+20)
   return(374+e_mouse.y-ya);
   else
   return(0);
}

int e_opt_bs_mouse_1()
{
   extern struct mouse e_mouse;  /*  return = sw + 375;  */
   int sw = 0;
   if(e_mouse.y < 2 || e_mouse.y > 21 ||
	   e_mouse.x < 2 || e_mouse.x > 33) return(0);
   else if(e_mouse.y == 2 && e_mouse.x == 6) sw = 1;
   else if(e_mouse.y == 2 && e_mouse.x >= 17 && e_mouse.x <= 28) sw = 3;
   else if(e_mouse.y == 21 && e_mouse.x >= 5 && e_mouse.x <= 10) sw = 1;
   else if(e_mouse.y == 2 || e_mouse.y == 21) sw = 2;
   else if((e_mouse.y == 3 || e_mouse.y == 5)
		&& e_mouse.x >= 18 && e_mouse.x <= 27) sw = 0;
   else if(e_mouse.y == 4 && (e_mouse.x == 18 || e_mouse.x == 27)) sw = 0;
   else if(e_mouse.y == 4 && e_mouse.x == 20) sw = 1;
   else if(e_mouse.y == 4 && e_mouse.x > 18 && e_mouse.x < 27) sw = 2;
   else sw = 4;
   return(sw + 375);
}

int e_opt_bs_mouse_2()
{
   extern struct mouse e_mouse;  /*  return = sw + 375;  */
   int sw = 0;
   if(e_mouse.y < 2 || e_mouse.y > 21 ||
	   e_mouse.x < 1 || e_mouse.x > 32) return(0);
   else if(e_mouse.y == 2 && e_mouse.x == 4) sw = 1;
   else if(e_mouse.y == 2 || e_mouse.y == 21 ||
	   	e_mouse.x == 1 || e_mouse.x == 32) sw = 0;
   else if(e_mouse.y == 4 && e_mouse.x == 5) sw = 3;
   else if(e_mouse.y == 5 && e_mouse.x >= 5 && e_mouse.x <= 24 ) sw = 5;
   else if(e_mouse.y == 7 && e_mouse.x == 5) sw = 3;
   else if(e_mouse.y == 8 && e_mouse.x >= 5 && e_mouse.x <= 24 ) sw = 4;
   else if(e_mouse.y == 10 && e_mouse.x == 5) sw = 3;
   else if(e_mouse.y == 11 && e_mouse.x >= 5 && e_mouse.x <= 20 ) sw = 7;
   else if(e_mouse.y == 12 && e_mouse.x >= 5 && e_mouse.x <= 20 ) sw = 8;
   else if(e_mouse.y == 13 && e_mouse.x >= 5 && e_mouse.x <= 20 ) sw = 6;
   else if(e_mouse.y == 15 && e_mouse.x == 5) sw = 3;
   else if(e_mouse.y == 16 && e_mouse.x >= 5 && e_mouse.x <= 24 ) sw = 11;
   else if(e_mouse.y == 17 && e_mouse.x == 10) sw = 10;
   else if(e_mouse.y == 17 && e_mouse.x >= 5 && e_mouse.x <= 24 ) sw = 9;
   else if(e_mouse.y == 19 && e_mouse.x == 7) sw = 13;
   else if(e_mouse.y == 19 && e_mouse.x >= 6 && e_mouse.x <= 13 ) sw = 12;
   else if(e_mouse.y == 19 && e_mouse.x >= 19 && e_mouse.x <= 26 ) sw = 14;
   else sw = 2;
   return(sw + 375);
}

int e_opt_bs_mouse_3()
{
   extern struct mouse e_mouse;  /*  return = sw + 375;  */
   int sw = 0;
   if(e_mouse.y < 2 || e_mouse.y > 21 ||
	   e_mouse.x < 1 || e_mouse.x > 32) return(0);
   else if(e_mouse.y == 5) sw = 3;
   else if(e_mouse.y == 9) sw = 1;
   else if(e_mouse.y == 11) sw = 2;
   else if(e_mouse.y == 13) sw = 4;
   return(sw+375);
}

/**
 * e_opt_bs_mouse_ai - Map a click on the AI pane colour example to its entry.
 *
 * The example shows one line per role at the rows e_pr_ed_beispiel draws
 * them on; clicking the user's, the machinery, the attention or the error
 * line selects that entry, so the mouse works in this section as in the
 * others.  A click elsewhere in the example keeps the selection.
 */
int e_opt_bs_mouse_ai()
{
   extern struct mouse e_mouse;  /*  return = sw + 375;  */
   int sw = -1;
   if(e_mouse.y < 2 || e_mouse.y > 21 ||
	   e_mouse.x < 1 || e_mouse.x > 32) return(0);
   else if(e_mouse.y == OPT_AI_EXAMPLE_USER_ROW) sw = 0;
   else if(e_mouse.y == OPT_AI_EXAMPLE_MACHINERY_ROW) sw = 1;
   else if(e_mouse.y == OPT_AI_EXAMPLE_ATTENTION_ROW) sw = 2;
   else if(e_mouse.y == OPT_AI_EXAMPLE_ERROR_ROW) sw = 3;
   if (sw < 0) return(0);
   return(sw+375);
}

int e_opt_bs_mouse()
{
   extern struct mouse e_mouse;
   int sw = 0;
   if(e_mouse.y < 2 || e_mouse.y > 21 ||
	   e_mouse.x < 1 || e_mouse.x > 32) return(0);
   else if(e_mouse.y == 2 && (e_mouse.x == 4 || e_mouse.x == 29 )) sw = 1;
   else if(e_mouse.x == 32 && e_mouse.y > 2 && e_mouse.y < 21 ) sw = 5;
   else if(e_mouse.y == 21 && e_mouse.x > 20 && e_mouse.y < 32 ) sw = 5;
   else if(e_mouse.y == 2 || e_mouse.y == 21 ||
	   e_mouse.x == 1 || e_mouse.x == 32) sw = 0;
   else if(e_mouse.y == 6 && e_mouse.x >= 4 && e_mouse.x <= 30 ) sw = 3;
   else if(e_mouse.y == 7 && e_mouse.x >= 14 && e_mouse.x <= 21 ) sw = 4;
   else if(e_mouse.y == 10 && e_mouse.x >= 4 && e_mouse.x <= 16 ) sw = 6;
   else if(e_mouse.y == 11 && e_mouse.x >= 15 && e_mouse.x <= 20 ) sw = 8;
   else if(e_mouse.y == 13 && e_mouse.x >= 4 && e_mouse.x <= 17 ) sw = 7;
   else if(e_mouse.y == 16 && e_mouse.x >= 4 && e_mouse.x <= 25 ) sw = 9;
   else if(e_mouse.y == 17 && e_mouse.x >= 4 && e_mouse.x <= 23 ) sw = 10;
   else sw = 2;
   return(sw+375);
}

int e_data_ein_mouse(f)
     FENSTER *f;
{
   extern struct mouse e_mouse;
   FLWND *fw = (FLWND *)f->b;
   ECNT *cn = f->ed;
   int i, c = 0;
   if(e_mouse.y == 0) return(AltBl);
   else if(e_mouse.y == MAXSLNS-1) return(e_m3_mouse());
   else if(e_mouse.x < f->a.x || e_mouse.x > f->e.x
	   || e_mouse.y < f->a.y || e_mouse.y > f->e.y)
   {  for(i = cn->mxedt; i > 0; i--)
      {  if(e_mouse.x >= cn->f[i]->a.x && e_mouse.x <= cn->f[i]->e.x
	  && e_mouse.y >= cn->f[i]->a.y && e_mouse.y <= cn->f[i]->e.y)
	 return(cn->edt[i] < 10 ? Alt1-1+cn->edt[i] : 1014+cn->edt[i]);
      }
   }
   else if(e_mouse.x == f->a.x+3 && e_mouse.y == f->a.y) c = WPE_ESC;
   else if(e_mouse.x == f->e.x-3 && e_mouse.y == f->a.y) e_ed_zoom(f);
   else if(e_mouse.x == f->a.x && e_mouse.y == f->a.y) e_eck_mouse(f, 1);
   else if(e_mouse.x == f->e.x && e_mouse.y == f->a.y) e_eck_mouse(f, 2);
   else if(e_mouse.x == f->e.x && e_mouse.y == f->e.y) e_eck_mouse(f, 3);
   else if(e_mouse.x == f->a.x && e_mouse.y == f->e.y) e_eck_mouse(f, 4);
   else if(e_mouse.y == f->a.y && e_mouse.x > f->a.x
				&& e_mouse.x < f->e.x) e_eck_mouse(f, 0);
   else if(e_mouse.y >= fw->ya && e_mouse.y <= fw->ye &&
	e_mouse.x >= fw->xa && e_mouse.x <= fw->xe)  c = AltF;
   else if(e_mouse.y == f->e.y-2 && e_mouse.x >= f->e.x-9
			&& e_mouse.x <= f->e.x-3) c = WPE_ESC;
   else if((f->ins < 4 || f->ins == 7) && e_mouse.y == f->e.y-4
	    && e_mouse.x >= f->e.x-9 && e_mouse.x <= f->e.x-3) c = AltS;
   else if(f->ins > 3 && e_mouse.y == f->e.y-8 && e_mouse.x >= f->e.x-9
			&& e_mouse.x <= f->e.x-3) c = AltA;
   else if(f->ins > 3 && e_mouse.y == f->e.y-6 && e_mouse.x >= f->e.x-9
			&& e_mouse.x <= f->e.x-3) c = AltE;
   else if(f->ins > 3 && e_mouse.y == f->e.y-4 && e_mouse.x >= f->e.x-9
			&& e_mouse.x <= f->e.x-3) c = AltD;
   else if(f->ins == 4 && e_mouse.y == f->e.y-10 && e_mouse.x >= f->e.x-9
			&& e_mouse.x <= f->e.x-3) c = AltO;
   else c = AltF;
   while (e_mshit() != 0);
   return(c);
}

/* Save the dialog's currently drawn image so the drag loop can stamp it at the
   next position -- the visual "the box follows the pointer" feedback.  The
   geometry matches the classic per-backend framing (the X11 frame reserves the
   last column/row).  Each drag step opens a FRESH snapshot and frees it within
   the same step: e_close_view() frees the view, so keeping one snapshot and
   re-stamping it after it was freed is a use-after-free -- that crashed xwpe on
   the second motion of a title-bar drag (and again on release).  Shared with
   the keyboard mover (e_opt_move), which had the identical fault. */
PIC *e_opt_drag_snapshot(W_OPTSTR *o)
{
#ifndef NEWSTYLE
#if !defined(DJGPP)
 if (!WpeIsXwin())
  return e_open_view(o->xa, o->ya, o->xe, o->ye, 0, 2);
#endif
 return e_open_view(o->xa, o->ya, o->xe - 2, o->ye - 1, 0, 2);
#else
 return e_open_view(o->xa, o->ya, o->xe, o->ye, 0, 2);
#endif
}

void e_opt_eck_mouse(o)
     W_OPTSTR *o;
{
   int g[4];
   int xold, yold, x, y, xa;
   e_std_rahmen(o->xa, o->ya, o->xe, o->ye, o->name, 0, o->frt, o->frs);
   g[0] = 3;  g[1] = 1;
   fk_mouse(g);
   xold = g[2]/8;
   yold = g[3]/8;
   xa = xold - o->xa;
   while(g[1] != 0)
   {  x = g[2]/8;
      y = g[3]/8;
      if(y < 1) y = 1;
      else if(y > MAXSLNS-2) y = MAXSLNS-2;
      if(x < 0) x = 0;
      else if(x > MAXSCOL-1) x = MAXSCOL-1;
      if(xold != x || yold != y)
      {  PIC *snap = e_opt_drag_snapshot(o);   /* the box as drawn at the OLD spot */
	 xold = x;  yold = y;
	 x -= xa;
	 if(x < 0) x = 0;
	 else if(x + o->xe - o->xa > MAXSCOL-1)
	 x = MAXSCOL - o->xe + o->xa - 1;
	 if(o->ye + y - o->ya > MAXSLNS-2)
	 y = MAXSLNS - o->ye + o->ya - 2;
	 o->xe = o->xe - o->xa + x; o->xa = x;
	 o->ye = o->ye + y - o->ya;  o->ya = y;
	 g[0] = 2;  fk_mouse(g);
	 o->pic = e_change_pic(o->xa, o->ya, o->xe, o->ye, o->pic, 1, o->frt);
	 if(o->pic == NULL)  e_error(e_msg[ERR_LOWMEM], 1, o->f->fb);
	 g[0] = 1;  fk_mouse(g);
	 if(snap != NULL)
	 {  snap->a.x = o->xa;  snap->a.y = o->ya;   /* stamp it at the NEW spot */
	    snap->e.x = o->xe;  snap->e.y = o->ye;
	    e_close_view(snap, 2);                    /* frees snap; never reused */
	 }
      }
      g[0] = 3;  fk_mouse(g);
   }
   e_std_rahmen(o->xa, o->ya, o->xe, o->ye, o->name, 1, o->frt, o->frs);
}

/* Button bits of a mouse report, in the one order the dialog engine relies on:
   left, middle, right.  The X11 and Wayland readers return these bits negated
   as the key code (-1, -2, -4); the terminal readers (ncurses, GPM) return -1
   and keep the bits in e_mouse.k. */
#define MOUSE_BTN_LEFT   1
#define MOUSE_BTN_MIDDLE 2
#define MOUSE_BTN_RIGHT  4

/* The close glyph of a dialog sits this many cells left of its right border
   (e_draw_titlebar_buttons in we_wind.c paints it at xe-2).  It is the ONLY
   cell of the title row that closes the dialog: the rest of the row drags it. */
#define OPT_CLOSE_BOX_FROM_RIGHT 2

/**
 * e_mouse_report_buttons - Which buttons a mouse report from e_getch names.
 * @c: the negative code e_getch returned for the report
 *
 * Lets the dialog engine tell a left click from a wheel step or a middle or
 * right click without knowing which backend produced the report.  A wheel
 * step on the terminal arrives with no button bit set.
 */
static int e_mouse_report_buttons(int c)
{
 extern struct mouse e_mouse;

 if (WpeIsXwin())
  return(-c);
 return(e_mouse.k);
}

/**
 * e_opt_pointer_inside - Is the pointer on the dialog, borders included?
 *
 * The left and bottom borders count as outside (a click there does nothing),
 * the title row and the right border as inside, as the frame has always been
 * hit-tested.
 */
static int e_opt_pointer_inside(W_OPTSTR *o)
{
 extern struct mouse e_mouse;

 if (e_mouse.y < o->ya || e_mouse.y >= o->ye)
  return(0);
 if (e_mouse.x <= o->xa || e_mouse.x >= o->xe)
  return(0);
 return(1);
}

static int e_opt_pointer_on_title_row(W_OPTSTR *o)
{
 extern struct mouse e_mouse;

 return(e_mouse.y == o->ya);
}

static int e_opt_pointer_on_close_box(W_OPTSTR *o)
{
 extern struct mouse e_mouse;

 if (!e_opt_pointer_on_title_row(o))
  return(0);
 return(e_mouse.x == o->xe - OPT_CLOSE_BOX_FROM_RIGHT);
}

/**
 * e_mouse_wait_release - Swallow the rest of a press so the next key is clean.
 */
static void e_mouse_wait_release(void)
{
 while (e_mshit())
  ;
}

/**
 * e_opt_mouse - Resolve a mouse report inside a dialog into what it asks for.
 * @o: the dialog
 * @c: the negative code e_getch returned for the report
 *
 * Only button 1 does anything in a dialog: a press on the drawn close box
 * closes it like Esc (WPE_ESC), a press elsewhere on the title row drags it,
 * and a press on a widget returns that widget's code so the engine activates
 * it as a click should.  A scroll-wheel step and the middle and right buttons
 * are ignored, so rolling the wheel over "[ ] Enable" no longer toggles it,
 * and a click that lands on nothing returns -1 for "nothing to do".
 */
int e_opt_mouse(W_OPTSTR *o, int c)
{
 extern struct mouse e_mouse;
 int buttons = e_mouse_report_buttons(c);

 if (!e_opt_pointer_inside(o))
  return(-1);
 if (!(buttons & MOUSE_BTN_LEFT))
 {
  e_mouse_wait_release();
  return(-1);
 }
 if (e_opt_pointer_on_close_box(o))
 {
  e_mouse_wait_release();
  return(WPE_ESC);
 }
 if (e_opt_pointer_on_title_row(o))
 {
  e_opt_eck_mouse(o);
  return(-1);
 }
 e_mouse_wait_release();
 c = e_get_opt_sw(0, e_mouse.x, e_mouse.y, o);
 if (c)
  return(c);
 return(-1);
}

#endif

