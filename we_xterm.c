/* we_xterm.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#ifndef NO_XWINDOWS

#include "edit.h"
#include "we_render.h"
#include "we_fdloop.h"
#include "we_clip.h"
#include <X11/Xatom.h>
#include <poll.h>

/* partial conversion in place */
#include "WeXterm.h"

#ifndef XWPE_DLL
#define WpeDllInit WpeXtermInit
#endif

int e_X_sw_color(void);
int fk_show_cursor(void);
int e_ini_size(void);
int e_x_getch(void);
int fk_x_mouse(int *g);
int fk_x_grab_pointer(int on);
int fk_x_drag_next(int *px, int *py);
int e_x_refresh(void);
int fk_x_locate(int x, int y);
int fk_x_cursor(int x);
int e_x_sys_ini(void);
int e_x_sys_end(void);
int fk_x_putchar(int c);
int x_bioskey(void);
int e_x_system(const char *exe);
int e_x_cp_X_to_buffer(FENSTER *f);
int e_x_copy_X_buffer(FENSTER *f);
int e_x_paste_X_buffer(FENSTER *f);
static void e_clip_x_set(const char *utf8, int len);
static char *e_clip_x_get(int *len);
int e_x_change(PIC *pic);
int e_x_repaint_desk(FENSTER *f);
void e_setlastpic(PIC *pic);
int e_make_xr_rahmen(int xa, int ya, int xe, int ye, int sw);
int e_x_kbhit(void);

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xos.h>

#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/XKBlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_XFT
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <wchar.h>

/* Font cache for fallback fonts (same pattern as st) */
typedef struct {
 XftFont *font;
 int flags;
 long unicodep;
} Fontcache;
static Fontcache *frc = NULL;
static int frclen = 0, frccap = 0;

/* Forward declarations for Xft helper functions */
static int e_x_wchar_to_utf8(int wc, char *buf);
static XftFont *e_xft_fallback_font(int rune);
#endif


#ifndef NEWSTYLE
#define NOXCACHE
#endif

#define BUFSIZE 80

extern char *e_tmp_dir;

/*  global constants  */

/*  for TextSchirm (text screen)   */

extern SCREENCELL *altschirm;
#ifdef NEWSTYLE
extern char *extbyte, *altextbyte;
#endif
int old_cursor_x = 0, old_cursor_y = 0, cur_on = 1;
extern PIC *e_X_l_pic;

extern struct mouse e_mouse;

/* Real X11 teardown at exit, installed as WpeDisplayEnd (X11 used to leave it
   as WpeNullFunction, so nothing was released).  XCloseDisplay reclaims the
   server-side resources (window, GC, Pixmap, core font) and Xlib's own
   client-side memory, but it does NOT know about the foreign client-side
   allocations layered on top: the render backend's cairo/Pango/FreeType stack
   (via WpeRender.cleanup) and libXft's font glyph cache and colour cells.
   Free those explicitly -- the one-time init leaks the valgrind sweep (#166)
   flagged -- then close the display.  cr_cleanup runs first because its cairo
   surface wraps WpeXInfo.backbuf. */
static void e_x_display_end(void)
{
 if (!WpeXInfo.display)
  return;

 if (WpeRender.cleanup)
  WpeRender.cleanup();
#ifdef HAVE_XFT
 if (WpeXInfo.xftfont)
 {
  Visual *vis = DefaultVisual(WpeXInfo.display, WpeXInfo.screen);
  Colormap cmap = DefaultColormap(WpeXInfo.display, WpeXInfo.screen);
  int i;
  for (i = 0; i < 16; i++)
   XftColorFree(WpeXInfo.display, vis, cmap, &WpeXInfo.xftcolors[i]);
  if (WpeXInfo.xftdraw)
   XftDrawDestroy(WpeXInfo.xftdraw);
  XftFontClose(WpeXInfo.display, WpeXInfo.xftfont);
 }
#endif
 XCloseDisplay(WpeXInfo.display);
 WpeXInfo.display = NULL;
}

int WpeDllInit(int *argc, char **argv)
{
 e_s_u_clr = e_s_x_clr;
 e_n_u_clr = e_n_x_clr;
 e_frb_u_menue = e_frb_x_menue;
 e_pr_u_col_kasten = e_pr_x_col_kasten;
 fk_u_cursor = fk_x_cursor;
 fk_u_locate = fk_x_locate;
 e_u_refresh = e_x_refresh;
 e_u_getch = e_x_getch;
 u_bioskey = x_bioskey;
 e_u_sys_ini = e_x_sys_ini;
 e_u_sys_end = e_x_sys_end;
 e_u_system = e_x_system;
 fk_u_putchar = fk_x_putchar;
 fk_mouse = fk_x_mouse;
 fk_u_grab_pointer = fk_x_grab_pointer;
 fk_u_drag_next = fk_x_drag_next;
 e_u_cp_X_to_buffer = e_x_cp_X_to_buffer;
 e_u_copy_X_buffer = e_x_copy_X_buffer;
 e_u_paste_X_buffer = e_x_paste_X_buffer;
 e_clip_os_set = e_clip_x_set;   /* ^C / ^Ins -> PRIMARY + CLIPBOARD (UTF-8) */
 e_clip_os_get = e_clip_x_get;   /* ^V <- the OS selection when another app owns it */
 e_u_kbhit = e_x_kbhit;
 e_u_change = e_x_change;
 e_u_ini_size = e_ini_size;
 e_u_setlastpic = e_setlastpic;
 WpeMouseChangeShape = (void (*)(WpeMouseShape))WpeNullFunction;
 WpeMouseRestoreShape = (void (*)(WpeMouseShape))WpeNullFunction;
/* WpeMouseChangeShape = WpeXMouseChangeShape;
 WpeMouseRestoreShape = WpeXMouseRestoreShape;*/
 WpeDisplayEnd = e_x_display_end;
 e_u_switch_screen = WpeZeroFunction;
 e_u_d_switch_out = (int (*)(int sw))WpeZeroFunction;
 { extern int e_t_deb_out(FENSTER *);
   e_u_deb_out = e_t_deb_out;
 }
 MCI = 7;   /* scrollbar track: ACS_S9 (same as terminal mode) */
 MCA = 11;  /* scrollbar thumb: ACS_DIAMOND (same as terminal mode) */
 RD1 = 1;  /* ACS_ULCORNER */
 RD2 = 2;  /* ACS_URCORNER */
 RD3 = 3;  /* ACS_LLCORNER */
 RD4 = 4;  /* ACS_LRCORNER */
 RD5 = 5;  /* ACS_HLINE */
 RD6 = 6;  /* ACS_VLINE */
 RE1 = 1;
 RE2 = 2;
 RE3 = 3;
 RE4 = 4;
 RE5 = 5;
 RE6 = 6;
 WBT = 1;
 ctree[0] = "\016\022\030";
 ctree[1] = "\016\022\022";
 ctree[2] = "\016\030\022";
 ctree[3] = "\025\022\022";
 ctree[4] = "\016\022\022";
 WpeXInit(argc, argv);
 return 0;
}

#ifdef NEWSTYLE
#ifdef NOXCACHE

#define e_print_xrect(x, y, n)                                               \
  if(extbyte[n])							     \
  {									     \
   XSetForeground(WpeXInfo.display, WpeXInfo.gc, 						     \
			!(extbyte[n] & 16) ? WpeXInfo.colors[0] : WpeXInfo.colors[15]);\
   if(extbyte[n] & 2)							     \
	XDrawLine(WpeXInfo.display, WpeXInfo.window, WpeXInfo.gc, WpeXInfo.font_width*((x)+1)-1, 		     \
		WpeXinfo.font_height*(y), WpeXInfo.font_width*((x)+1)-1, 			     \
					   WpeXInfo.font_height*((y)+1)-1);	     \
   if(extbyte[n] & 4)							     \
   	XDrawLine(WpeXInfo.display, WpeXInfo.window, WpeXInfo.gc, WpeXInfo.font_width*(x), 			     \
		WpeXInfo.font_height*((y)+1)-1, 					     \
		WpeXInfo.font_width*((x)+1)-1, WpeXInfo.font_height*((y)+1)-1);		     \
									     \
   XSetForeground(WpeXInfo.display, WpeXInfo.gc, 						     \
		!(extbyte[n] & 16) ? WpeXInfo.colors[15] : WpeXInfo.colors[0]);	     \
   if(extbyte[n] & 8)							     \
	XDrawLine(WpeXInfo.display, WpeXInfo.window, WpeXInfo.gc, WpeXInfo.font_width*(x), 			     \
		WpeXInfo.font_height*(y), WpeXInfo.font_width*(x), 				     \
		WpeXInfo.font_height*((y)+1)-1);					     \
   if(extbyte[n] & 1)							     \
	XDrawLine(WpeXInfo.display, WpeXInfo.window, WpeXInfo.gc, WpeXInfo.font_width*(x), 			     \
		WpeXInfo.font_height*(y), WpeXInfo.font_width*((x)+1)-1, 			     \
		WpeXInfo.font_height*(y));					     \
  }


#else				/* cached  a.r. */

#define WPE_MAXSEG 1000
int nseg[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
XSegment seg[8][WPE_MAXSEG];
int scol[8] = { 0, 0, 15, 15, 15, 15, 0, 0 };

int e_XLookupString(XKeyEvent *event, char *buffer_return, int buffer_size,
		    KeySym *keysym_return, XComposeStatus *status)
{
    static int first = 1;
    static XIC xic;
    static XIM xim;
    Status xim_status;

    if (first) {
	first = 0;
	if (!XSetLocaleModifiers(""))
	    XSetLocaleModifiers("@im=none");
	xim = XOpenIM(event->display, NULL, NULL, NULL);
	if (xim)
	    xic = XCreateIC(xim,
			    XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
			    XNClientWindow, WpeXInfo.window,
			    XNFocusWindow, WpeXInfo.window, NULL);
	else
	    xic = NULL;
    }
    if (xic) {
	if (XFilterEvent((XEvent*)event, WpeXInfo.window))
	    return (0);

	*keysym_return = NoSymbol;
	return (XmbLookupString(xic, event, buffer_return, buffer_size,
				keysym_return, &xim_status));
    }

    return (XLookupString(event, buffer_return, buffer_size,
			  keysym_return, status));
}

void e_flush_xrect()
{
 int i;
 Drawable target;

#ifdef HAVE_XFT
 target = WpeXInfo.xftfont ? WpeXInfo.backbuf : WpeXInfo.window;
#else
 target = WpeXInfo.window;
#endif

 for (i = 0; i < 8; i++)
  if (nseg[i])
  {
   XSetForeground(WpeXInfo.display, WpeXInfo.gc, WpeXInfo.colors[scol[i]]);
   XDrawSegments(WpeXInfo.display, target, WpeXInfo.gc, seg[i], nseg[i]);
   nseg[i] = 0;
  }
}

void e_print_xrect(int x, int y, int n)
{
 int c = extbyte[n] & 16 ? 4 : 0;

 if (extbyte[n])
 {
  if ((nseg[0] > WPE_MAXSEG) || (nseg[1] > WPE_MAXSEG) || (nseg[2] > WPE_MAXSEG) ||
    (nseg[3] > WPE_MAXSEG) || (nseg[4] > WPE_MAXSEG) || (nseg[5] > WPE_MAXSEG) ||
    (nseg[6] > WPE_MAXSEG) || (nseg[7] > WPE_MAXSEG))
   e_flush_xrect();
  if (extbyte[n] & 2)
  {
   seg[c][nseg[c]].x1 = WpeXInfo.font_width*((x)+1)-1;
   seg[c][nseg[c]].y1 = WpeXInfo.font_height*(y);
   seg[c][nseg[c]].x2 = WpeXInfo.font_width*((x)+1)-1;
   seg[c][nseg[c]].y2 = WpeXInfo.font_height*((y)+1)-1;
   nseg[c]++;
  }
  if (extbyte[n] & 4)
  {
   seg[c+1][nseg[c+1]].x1 = WpeXInfo.font_width*(x);
   seg[c+1][nseg[c+1]].y1 = WpeXInfo.font_height*((y)+1)-1;
   seg[c+1][nseg[c+1]].x2 = WpeXInfo.font_width*((x)+1)-1;
   seg[c+1][nseg[c+1]].y2 = WpeXInfo.font_height*((y)+1)-1;
   nseg[c+1]++;
  }
  if (extbyte[n] & 8)
  {
   seg[c+2][nseg[c+2]].x1 = WpeXInfo.font_width*(x);
   seg[c+2][nseg[c+2]].y1 = WpeXInfo.font_height*(y);
   seg[c+2][nseg[c+2]].x2 = WpeXInfo.font_width*(x);
   seg[c+2][nseg[c+2]].y2 = WpeXInfo.font_height*((y)+1)-1;
   nseg[c+2]++;
  }
  if (extbyte[n] & 1)
  {
   seg[c+3][nseg[c+3]].x1 = WpeXInfo.font_width*(x);
   seg[c+3][nseg[c+3]].y1 = WpeXInfo.font_height*(y);
   seg[c+3][nseg[c+3]].x2 = WpeXInfo.font_width*((x)+1)-1;
   seg[c+3][nseg[c+3]].y2 = WpeXInfo.font_height*(y);
   nseg[c+3]++;
  }
 }
}
#endif

#endif

static char e_x_map_char(int sc);
#ifdef HAVE_XFT
static int e_x_map_char_utf8(int sc, char *buf);
static int e_x_wchar_to_utf8(int wc, char *buf);
static XftFont *e_xft_fallback_font(int codepoint);
static void e_xft_draw_acs(int sc, int px, int py, int fg_idx);

static void e_x_render_cell(int sc, int px, int py, int cw,
                            int fg_idx, int bg_idx)
{
 if (sc >= 1 && sc <= 12)
 {
  if (WpeRender.draw_acs)
   WpeRender.draw_acs(sc, px, py, fg_idx, bg_idx);
  else
  {
   XftDrawRect(WpeXInfo.xftdraw, &WpeXInfo.xftcolors[bg_idx],
     px, py, WpeXInfo.font_width * cw, WpeXInfo.font_height);
   e_xft_draw_acs(sc, px, py, fg_idx);
  }
 }
 else if (sc == WR_GLYPH_LOCK && WpeRender.draw_lock)
 {
  WpeRender.draw_lock(px, py, cw, fg_idx, bg_idx);
 }
 else if (sc > 12 && sc != ' ')
 {
  char u8[6];
  int u8len;

  if (sc < 32)
  { u8[0] = ' '; u8len = 1; }
  else if (sc < 128)
  { u8[0] = (char)sc; u8len = 1; }
  else
   u8len = e_x_wchar_to_utf8(sc, u8);

  if (WpeRender.draw_text)
   WpeRender.draw_text(px, py, u8, u8len, cw, fg_idx, bg_idx);
  else
  {
   XftFont *font = WpeXInfo.xftfont;
   XftDrawRect(WpeXInfo.xftdraw, &WpeXInfo.xftcolors[bg_idx],
     px, py, WpeXInfo.font_width * cw, WpeXInfo.font_height);
   if (sc >= 128
       && !XftCharExists(WpeXInfo.display, WpeXInfo.xftfont, sc))
    font = e_xft_fallback_font(sc);
   XftDrawStringUtf8(WpeXInfo.xftdraw, &WpeXInfo.xftcolors[fg_idx],
     font, px, py + WpeXInfo.xftfont->ascent,
     (FcChar8 *)u8, u8len);
  }
 }
 else
 {
  if (WpeRender.draw_rect)
   WpeRender.draw_rect(px, py, WpeXInfo.font_width * cw,
     WpeXInfo.font_height, bg_idx);
  else
   XftDrawRect(WpeXInfo.xftdraw, &WpeXInfo.xftcolors[bg_idx],
     px, py, WpeXInfo.font_width * cw, WpeXInfo.font_height);
 }
}
#endif

#ifdef HAVE_XFT
/* Repaint one cell at the cursor position with the Xft renderer.  inverted
   swaps fg/bg: the cell the cursor now occupies is drawn reverse-video, the
   cell it just left is restored to normal video.  Honours the CELL_WIDE flag. */
static void e_xft_paint_cursor_cell(int cx, int cy, int inverted)
{
 int ch = e_gt_char(cx, cy);
 int raw = e_gt_col(cx, cy);
 int attr = ATTR_BASE(raw);
 int fg_idx = inverted ? attr / 16 : attr % 16;
 int bg_idx = inverted ? attr % 16 : attr / 16;
 int n = cy * MAXSCOL + cx;
 int cw = (schirm[n].flags & CELL_WIDE) ? 2 : 1;

 /* Restoring (not inverted) a semantic-token cell keeps its 24-bit colour, so
    the cursor leaving an orange method does not smear it back to fallback red.
    Under the cursor (inverted) the glyph takes the cell bg, as for any cell. */
 if (!inverted && ATTR_IS_TC(raw))
  fg_idx = 16 + ATTR_TC_SLOT(raw);

 e_x_render_cell(ch, WpeXInfo.font_width * cx, WpeXInfo.font_height * cy,
                 cw, fg_idx, bg_idx);
}
#endif /* HAVE_XFT */

int fk_show_cursor()
{
 if (!cur_on)
  return(0);

#ifdef HAVE_XFT
 if (WpeXInfo.xftfont)
 {
  if (old_cursor_x > 0 || old_cursor_y > 0)
   e_xft_paint_cursor_cell(old_cursor_x, old_cursor_y, 0);  /* restore */
  e_xft_paint_cursor_cell(cur_x, cur_y, 1);                 /* draw inverted */
 }
 else
#endif /* HAVE_XFT */
 {
  /* Original core X font cursor rendering */
  if (old_cursor_x > 0 || old_cursor_y > 0)
  {
   int oc = e_gt_char(old_cursor_x, old_cursor_y);
   int oa = ATTR_BASE(e_gt_col(old_cursor_x, old_cursor_y));
   char obuf[2] = { e_x_map_char(oc), 0 };
   XSetForeground(WpeXInfo.display, WpeXInfo.gc,
     WpeXInfo.colors[oa % 16]);
   XSetBackground(WpeXInfo.display, WpeXInfo.gc,
     WpeXInfo.colors[oa / 16]);
   XDrawImageString(WpeXInfo.display, WpeXInfo.window, WpeXInfo.gc,
     WpeXInfo.font_width*old_cursor_x,
     WpeXInfo.font_height*(old_cursor_y+1) - WpeXInfo.font_descent,
     obuf, 1);
#ifdef NEWSTYLE
   { int _n = old_cursor_y * MAXSCOL + old_cursor_x;
     e_print_xrect(old_cursor_x, old_cursor_y, _n);
#ifndef NOXCACHE
     e_flush_xrect();
#endif
   }
#endif
  }
  {
   int cc = e_gt_char(cur_x, cur_y);
   int ca = ATTR_BASE(e_gt_col(cur_x, cur_y));
   char cbuf[2] = { e_x_map_char(cc), 0 };
   XSetForeground(WpeXInfo.display, WpeXInfo.gc,
     WpeXInfo.colors[ca / 16]);
   XSetBackground(WpeXInfo.display, WpeXInfo.gc,
     WpeXInfo.colors[ca % 16]);
   XDrawImageString(WpeXInfo.display, WpeXInfo.window, WpeXInfo.gc,
     WpeXInfo.font_width * cur_x,
     WpeXInfo.font_height * (cur_y + 1) - WpeXInfo.font_descent,
     cbuf, 1);
  }
 }

 old_cursor_x = cur_x;
 old_cursor_y = cur_y;
 return(cur_on);
}

int e_ini_size()
{
 old_cursor_x = cur_x;
 old_cursor_y = cur_y;

 if (schirm)
  FREE(schirm);
 if(altschirm)
  FREE(altschirm);
 schirm = MALLOC(sizeof(SCREENCELL) * MAXSCOL * MAXSLNS);
 altschirm = MALLOC(sizeof(SCREENCELL) * MAXSCOL * MAXSLNS);
#ifdef NEWSTYLE
 if (extbyte)
  FREE(extbyte);
 if (altextbyte)
  FREE(altextbyte);
 extbyte = calloc(MAXSCOL * MAXSLNS, 1);
 altextbyte = calloc(MAXSCOL * MAXSLNS, 1);
 if (!schirm || !altschirm || !extbyte || !altextbyte)
  return(-1);
#else
 if(!schirm || !altschirm)
  return(-1);
#endif

#ifdef HAVE_XFT
 if (WpeXInfo.xftfont)
 {
  int new_w = WpeXInfo.font_width * MAXSCOL;
  int new_h = WpeXInfo.font_height * MAXSLNS;

  if (WpeRender.resize)
  {
   if (WpeXInfo.xftdraw)
    XftDrawDestroy(WpeXInfo.xftdraw);
   WpeRender.resize(new_w, new_h);
   WpeXInfo.xftdraw = XftDrawCreate(WpeXInfo.display, WpeXInfo.backbuf,
     DefaultVisual(WpeXInfo.display, WpeXInfo.screen),
     DefaultColormap(WpeXInfo.display, WpeXInfo.screen));
  }
  else
  {
   if (WpeXInfo.xftdraw)
    XftDrawDestroy(WpeXInfo.xftdraw);
   if (WpeXInfo.backbuf)
    XFreePixmap(WpeXInfo.display, WpeXInfo.backbuf);
   WpeXInfo.backbuf = XCreatePixmap(WpeXInfo.display, WpeXInfo.window,
     new_w, new_h,
     DefaultDepth(WpeXInfo.display, WpeXInfo.screen));
   WpeXInfo.xftdraw = XftDrawCreate(WpeXInfo.display, WpeXInfo.backbuf,
     DefaultVisual(WpeXInfo.display, WpeXInfo.screen),
     DefaultColormap(WpeXInfo.display, WpeXInfo.screen));
   XSetForeground(WpeXInfo.display, WpeXInfo.gc,
     BlackPixel(WpeXInfo.display, WpeXInfo.screen));
   XFillRectangle(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.gc,
     0, 0, new_w, new_h);
  }
 }
#endif

 return(0);
}

#define A_Normal 	16
#define A_Reverse 	1
#define A_Standout	1
#define A_Underline	1
#define A_Bold		16

int e_X_sw_color()
{
 FARBE *fb = WpeEditor->fb;
 fb->er = e_n_clr(A_Normal);
 fb->et = e_n_clr(A_Normal);
 fb->ez = e_n_clr(A_Reverse);
 fb->es = e_n_clr(A_Normal);
 fb->em = e_n_clr(A_Standout);
 fb->ek = e_n_clr(A_Underline);
 fb->nr = e_n_clr(A_Standout);
 fb->nt = e_n_clr(A_Reverse);
 fb->nz = e_n_clr(A_Normal);
 fb->ns = e_n_clr(A_Bold);
 fb->mr = e_n_clr(A_Standout);
 fb->mt = e_n_clr(A_Standout);
 fb->mz = e_n_clr(A_Normal);
 fb->ms = e_n_clr(A_Normal);
 fb->fr = e_n_clr(A_Normal);
 fb->ft = e_n_clr(A_Normal);
 fb->fz = e_n_clr(A_Standout);
 fb->fs = e_n_clr(A_Standout);
 fb->of = e_n_clr(A_Standout);
 fb->df = e_s_x_clr(7, 4);
 fb->dc = 0xB1;
#ifdef DEBUGGER
 fb->db = e_n_clr(A_Standout);
 fb->dy = e_n_clr(A_Standout);
#endif
 return(0);
}

/**
 * e_x_map_char - Map a SCREENCELL character to a printable X11 glyph.
 * @sc: The SCREENCELL.ch value (may be ASCII, ACS index 0-12, or wide).
 *
 * SCREENCELL stores border/scrollbar characters as small integers (0-12)
 * that index into sp_chr[] for ncurses ACS rendering.  X11 mode can't
 * use ACS, so we map them to ASCII box-drawing approximations.
 *
 * Return: A printable char suitable for XDrawImageString.
 */
static char e_x_map_char(int sc)
{
 /* ACS index to ASCII approximation (matches sp_chr[] indices) */
 static const char acs_to_ascii[] = {
  ' ',  /* 0: space */
  '+',  /* 1: ACS_ULCORNER -> + */
  '+',  /* 2: ACS_URCORNER -> + */
  '+',  /* 3: ACS_LLCORNER -> + */
  '+',  /* 4: ACS_LRCORNER -> + */
  '-',  /* 5: ACS_HLINE -> - */
  '|',  /* 6: ACS_VLINE -> | */
  '~',  /* 7: ACS_S9 (scrollbar track) -> ~ */
  '|',  /* 8: ACS_VLINE -> | */
  '|',  /* 9: ACS_VLINE -> | */
  '~',  /* 10: ACS_S9 -> ~ */
  '#',  /* 11: ACS_DIAMOND (scrollbar thumb) -> # */
  ' ',  /* 12: space */
 };
 if (sc >= 0 && sc <= 12)
  return acs_to_ascii[sc];
 if (sc < 32 || sc > 126)
  return ' ';
 return (char)sc;
}

#ifdef HAVE_XFT
static int e_x_map_char_utf8(int sc, char *buf)
{
 /* ACS index to Unicode box-drawing / block element (UTF-8 encoded) */
 static const int acs_to_unicode[] = {
  ' ',     /* 0: space */
  0x250C,  /* 1: ACS_ULCORNER -> U+250C BOX DRAWINGS LIGHT DOWN AND RIGHT */
  0x2510,  /* 2: ACS_URCORNER -> U+2510 BOX DRAWINGS LIGHT DOWN AND LEFT */
  0x2514,  /* 3: ACS_LLCORNER -> U+2514 BOX DRAWINGS LIGHT UP AND RIGHT */
  0x2518,  /* 4: ACS_LRCORNER -> U+2518 BOX DRAWINGS LIGHT UP AND LEFT */
  0x2500,  /* 5: ACS_HLINE -> U+2500 BOX DRAWINGS LIGHT HORIZONTAL */
  0x2502,  /* 6: ACS_VLINE -> U+2502 BOX DRAWINGS LIGHT VERTICAL */
  0x2591,  /* 7: scrollbar track -> U+2591 LIGHT SHADE */
  0x2502,  /* 8: ACS_VLINE -> U+2502 BOX DRAWINGS LIGHT VERTICAL */
  0x2502,  /* 9: ACS_VLINE -> U+2502 BOX DRAWINGS LIGHT VERTICAL */
  0x2591,  /* 10: scrollbar track -> U+2591 LIGHT SHADE */
  0x2588,  /* 11: scrollbar thumb -> U+2588 FULL BLOCK */
  ' ',     /* 12: space */
 };
 if (sc >= 0 && sc <= 12)
  return e_x_wchar_to_utf8(acs_to_unicode[sc], buf);
 if (sc >= 128)
  return e_x_wchar_to_utf8(sc, buf);
 buf[0] = (sc >= 32 && sc <= 126) ? (char)sc : ' ';
 return 1;
}
#endif

#ifdef HAVE_XFT
static int e_x_wchar_to_utf8(int wc, char *buf)
{
 return e_codepoint_to_utf8(wc, (unsigned char *)buf);
}

static void e_xft_draw_hline(int px, int py, int fg_idx)
{
 int fw = WpeXInfo.font_width, fh = WpeXInfo.font_height;
 int lw = fw > 8 ? 2 : 1;
 XftDrawRect(WpeXInfo.xftdraw, &WpeXInfo.xftcolors[fg_idx],
   px, py + fh/2, fw, lw);
}

static void e_xft_draw_vline(int px, int py, int fg_idx)
{
 int fw = WpeXInfo.font_width, fh = WpeXInfo.font_height;
 int lw = fw > 8 ? 2 : 1;
 XftDrawRect(WpeXInfo.xftdraw, &WpeXInfo.xftcolors[fg_idx],
   px + fw/2, py, lw, fh);
}

static void e_xft_draw_corner(int px, int py, int fg_idx, int right, int bottom)
{
 int fw = WpeXInfo.font_width, fh = WpeXInfo.font_height;
 int lw = fw > 8 ? 2 : 1;
 int mx = px + fw/2, my = py + fh/2;
 int hx = right ? px : mx, hw = right ? fw/2 + lw : fw - fw/2;
 int vy = bottom ? py : my, vh = bottom ? fh/2 + lw : fh - fh/2;
 XftDrawRect(WpeXInfo.xftdraw, &WpeXInfo.xftcolors[fg_idx],
   hx, my, hw, lw);
 XftDrawRect(WpeXInfo.xftdraw, &WpeXInfo.xftcolors[fg_idx],
   mx, vy, lw, vh);
}

static void e_xft_draw_scrollbar_track(int px, int py, int fg_idx)
{
 int fw = WpeXInfo.font_width, fh = WpeXInfo.font_height;
 int tx, ty;
 for (ty = py; ty < py + fh; ty += 2)
  for (tx = px; tx < px + fw; tx += 2)
   XftDrawRect(WpeXInfo.xftdraw, &WpeXInfo.xftcolors[fg_idx],
     tx, ty, 1, 1);
}

static void e_xft_draw_scrollbar_thumb(int px, int py, int fg_idx)
{
 int fw = WpeXInfo.font_width, fh = WpeXInfo.font_height;
 int m = fw > 8 ? 2 : 1;
 XftDrawRect(WpeXInfo.xftdraw, &WpeXInfo.xftcolors[fg_idx],
   px + m, py + m, fw - 2*m, fh - 2*m);
}

/**
 * e_xft_draw_acs - Draw an ACS (box-drawing/scrollbar) character as pixels.
 * @sc:     ACS index (1-12).
 * @px, py: Pixel position of the cell.
 * @fg_idx: Foreground color index.
 */
static void e_xft_draw_acs(int sc, int px, int py, int fg_idx)
{
 switch (sc)
 {
  case 1:  e_xft_draw_corner(px, py, fg_idx, 0, 0); break;
  case 2:  e_xft_draw_corner(px, py, fg_idx, 1, 0); break;
  case 3:  e_xft_draw_corner(px, py, fg_idx, 0, 1); break;
  case 4:  e_xft_draw_corner(px, py, fg_idx, 1, 1); break;
  case 5:  e_xft_draw_hline(px, py, fg_idx); break;
  case 6: case 8: case 9:
           e_xft_draw_vline(px, py, fg_idx); break;
  case 7: case 10:
           e_xft_draw_scrollbar_track(px, py, fg_idx); break;
  case 11: e_xft_draw_scrollbar_thumb(px, py, fg_idx); break;
 }
}

/**
 * e_xft_fallback_font - Find a font for a character not in the main font.
 * @rune: The Unicode code point to find a font for.
 *
 * Uses fontconfig to search for a fallback font, following st's pattern
 * of caching results in frc[] to avoid repeated lookups.
 *
 * Return: An XftFont that can render the character (may be the main font
 *         if no better match is found).
 */
/* Ensure the fallback-font cache (frc) has room for one more entry.  Returns 0
   on success, -1 if it could not grow (out of memory) -- in which case the
   existing cache is left intact so the caller can degrade to the main font
   instead of dereferencing a freed/NULL pointer. */
static int e_xft_frc_reserve(void)
{
 Fontcache *grown;

 if (frclen < frccap)
  return 0;
 grown = realloc(frc, (frccap + 16) * sizeof(Fontcache));
 if (!grown)
  return -1;
 frc = grown;
 frccap += 16;
 return 0;
}

static XftFont *e_xft_fallback_font(int rune)
{
 int f;
 FT_UInt glyphidx;

 /* Search font cache */
 for (f = 0; f < frclen; f++)
 {
  glyphidx = XftCharIndex(WpeXInfo.display, frc[f].font, rune);
  if (glyphidx)
   return frc[f].font;
  /* Default glyph for this rune (already looked up, no better match) */
  if (!glyphidx && frc[f].unicodep == rune)
   return frc[f].font;
 }

 /* Not in cache - use fontconfig to find a font */
 {
  FcPattern *fcpattern, *fontpattern;
  FcCharSet *fccharset;
  FcResult fcres;
  FcFontSet *fcsets[1];

  if (!WpeXInfo.xftfont_set)
   WpeXInfo.xftfont_set = FcFontSort(0, WpeXInfo.xftpattern, 1, 0, &fcres);
  fcsets[0] = WpeXInfo.xftfont_set;

  fcpattern = FcPatternCreate();
  fccharset = FcCharSetCreate();

  FcCharSetAddChar(fccharset, rune);
  FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
  FcPatternAddBool(fcpattern, FC_SCALABLE, 1);
  if (rune > 0x2600)
   FcPatternAddBool(fcpattern, FC_COLOR, FcTrue);
  FcPatternAddInteger(fcpattern, FC_SIZE,
    WpeXInfo.font_height <= 13 ? 10 : WpeXInfo.font_height - 3);

  FcConfigSubstitute(0, fcpattern, FcMatchPattern);
  FcDefaultSubstitute(fcpattern);

  fontpattern = FcFontMatch(0, fcpattern, &fcres);

  /* Grow cache; on OOM degrade to the main font rather than crash */
  if (e_xft_frc_reserve() < 0)
  {
   FcPatternDestroy(fontpattern);
   FcPatternDestroy(fcpattern);
   FcCharSetDestroy(fccharset);
   return WpeXInfo.xftfont;
  }

  frc[frclen].font = XftFontOpenPattern(WpeXInfo.display, fontpattern);
  frc[frclen].unicodep = rune;
  frc[frclen].flags = 0;

  if (!frc[frclen].font)
  {
   /* Use main font as fallback */
   frc[frclen].font = WpeXInfo.xftfont;
   FcPatternDestroy(fontpattern);
  }

  f = frclen++;

  FcPatternDestroy(fcpattern);
  FcCharSetDestroy(fccharset);
 }
 return frc[f].font;
}
#endif

#ifdef HAVE_XFT
/* Walk the screen-cell grid and (re)paint cells via e_x_render_cell.  force=0
   redraws only cells that differ from altschirm[] (the incremental refresh);
   force=1 redraws every cell (a full repaint).  CELL_WIDE_SPACER cells are
   never drawn -- the wide glyph to their left already covered them. */
static void e_x_render_dirty_cells(int force)
{
 int i, j;
 for (i = 0; i < MAXSLNS; i++)
 for (j = 0; j < MAXSCOL; j++)
 {
  int _n = i * MAXSCOL + j;
  int sc, sa;

  if (schirm[_n].flags & CELL_WIDE_SPACER)
  {
   if (!force)
    altschirm[_n] = schirm[_n];
   continue;
  }
  sc = e_gt_char(j, i);
  sa = e_gt_col(j, i);
  if (force || sc != altschirm[_n].ch || sa != altschirm[_n].attr
      || schirm[_n].flags != altschirm[_n].flags)
  {
   e_x_render_cell(sc, WpeXInfo.font_width * j, WpeXInfo.font_height * i,
     (schirm[_n].flags & CELL_WIDE) ? 2 : 1,
     ATTR_IS_TC(sa) ? 16 + ATTR_TC_SLOT(sa) : ATTR_BASE(sa) % 16,
     ATTR_BASE(sa) / 16);
   altschirm[_n] = schirm[_n];
  }
 }
}

static void e_x_refresh_xft(void)
{
 e_x_render_dirty_cells(0);
 XCopyArea(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.window,
   WpeXInfo.gc, 0, 0,
   WpeXInfo.font_width * MAXSCOL, WpeXInfo.font_height * MAXSLNS, 0, 0);
}

/* The Cairo refresh path calls wpe_render_chrome(), which lives in
   we_render_cairo.c and only exists when Cairo+Pango are present. Guard
   it on HAVE_CAIRO (not just HAVE_XFT) so an Xft-only build still links. */
#ifdef HAVE_CAIRO
static void e_x_refresh_cairo(void)
{
 e_x_render_dirty_cells(0);
 wpe_render_chrome();
}

static void e_x_refresh_cairo_full(void)
{
 e_x_render_dirty_cells(1);
 wpe_render_chrome();
}
#endif /* HAVE_CAIRO */
#endif

#ifndef NO_XWINDOWS
/* Extract one 8-bit colour channel from an XImage pixel using its bit mask,
   scaling sub-8-bit masks (e.g. 16bpp 5-6-5) up to full range. */
static unsigned char wpe_x_chan(unsigned long px, unsigned long mask)
{
 if (!mask)
  return 0;
 while (!(mask & 1UL)) { px >>= 1; mask >>= 1; }
 px &= mask;
 while (mask < 0xffUL) { px = (px << 1) | (px >> 4); mask = (mask << 1) | 1UL; }
 while (mask > 0xffUL) { px >>= 1; mask >>= 1; }
 return (unsigned char)(px & 0xffUL);
}

/* Headless screenshot: dump the on-screen window as a binary PPM, the X11
   analogue of the Wayland XWPE_WL_DUMP path, so an X11 build can be verified
   under Xvfb with no real screen and no external screenshot tool.  We grab the
   window (not the back-buffer Pixmap): a Pixmap carries no visual, so XGetImage
   leaves its RGB masks zero and every pixel decodes to black.  Written to a temp
   file and renamed atomically by the caller. */
static int wpe_x_dump_ppm(const char *path)
{
 int w = WpeXInfo.font_width * MAXSCOL;
 int h = WpeXInfo.font_height * MAXSLNS;
 XImage *img;
 FILE *fp;
 int x, y;

 if (w <= 0 || h <= 0 || !WpeXInfo.window)
  return -1;
 img = XGetImage(WpeXInfo.display, WpeXInfo.window, 0, 0, w, h,
                 AllPlanes, ZPixmap);
 if (!img)
  return -1;
 fp = fopen(path, "wb");
 if (!fp) { XDestroyImage(img); return -1; }
 fprintf(fp, "P6\n%d %d\n255\n", w, h);
 for (y = 0; y < h; y++)
  for (x = 0; x < w; x++)
  {
   unsigned long px = XGetPixel(img, x, y);
   unsigned char rgb[3];
   rgb[0] = wpe_x_chan(px, img->red_mask);
   rgb[1] = wpe_x_chan(px, img->green_mask);
   rgb[2] = wpe_x_chan(px, img->blue_mask);
   fwrite(rgb, 1, 3, fp);
  }
 fclose(fp);
 XDestroyImage(img);
 return 0;
}
#endif /* NO_XWINDOWS */

int e_x_refresh()
{
   int i, j, cur_tmp = cur_on;
#ifdef NEWSTYLE
   /* Clear extbyte for window interiors (shadow fix) */
   { int _w;
     for (_w = 1; _w <= WpeEditor->mxedt; _w++)
     { FENSTER *_fw = WpeEditor->f[_w];
       for (i = _fw->a.y + 1; i < _fw->e.y; i++)
        for (j = _fw->a.x + 1; j < _fw->e.x; j++)
         extbyte[i * MAXSCOL + j] = 0;
     }
   }
#endif
   fk_cursor(0);

#ifdef HAVE_XFT
   if (WpeXInfo.xftfont)
   {
#ifdef HAVE_CAIRO
    if (WpeRender.draw_rect)
     e_x_refresh_cairo();
    else
#endif /* HAVE_CAIRO */
     e_x_refresh_xft();
   }
   else
#endif /* HAVE_XFT */
   {
    /* Original XDrawImageString path (fallback when no Xft) */
#ifndef NOXCACHE
#define STRBUFSIZE 1024
    unsigned long oldback = 0, oldfore = 0;
    static char stringbuf[STRBUFSIZE];
    int stringcount = 0, oldI = 0, oldX = 0, oldY = 0, oldJ = 0;
#endif
    for (i = 0; i < MAXSLNS; i++)
    for (j = 0; j < MAXSCOL; j++)
    {
     int sc = e_gt_char(j, i);
     int sa = e_gt_col(j, i);
     int sab = ATTR_BASE(sa);   /* 16-colour indices; sa keeps any TC flag for
                                   the dirty-check below */
     { int _n = i * MAXSCOL + j;
     if (sc != altschirm[_n].ch || sa != altschirm[_n].attr
#ifdef NEWSTYLE
         || extbyte[_n] != altextbyte[_n]
#endif
        )
     {
      char xc = e_x_map_char(sc);
#ifdef NOXCACHE
      XSetForeground(WpeXInfo.display, WpeXInfo.gc, WpeXInfo.colors[sab % 16]);
      XSetBackground(WpeXInfo.display, WpeXInfo.gc, WpeXInfo.colors[sab / 16]);
      XDrawImageString(WpeXInfo.display, WpeXInfo.window, WpeXInfo.gc, WpeXInfo.font_width*j,
          WpeXInfo.font_height*(i+1) - WpeXInfo.font_descent, &xc, 1);
#else
      if (   oldback != WpeXInfo.colors[sab / 16]
          || oldfore != WpeXInfo.colors[sab % 16]
          || i != oldI || j > oldJ+1 || stringcount >= STRBUFSIZE)
      {
       XDrawImageString(WpeXInfo.display, WpeXInfo.window, WpeXInfo.gc,
           oldX, oldY, stringbuf, stringcount);
       oldback = WpeXInfo.colors[sab / 16];
       oldfore = WpeXInfo.colors[sab % 16];
       XSetForeground(WpeXInfo.display, WpeXInfo.gc, oldfore);
       XSetBackground(WpeXInfo.display, WpeXInfo.gc, oldback);
       oldX = WpeXInfo.font_width*j;
       oldY = WpeXInfo.font_height*(i+1) - WpeXInfo.font_descent;
       oldI = i;
       stringcount = 0;
       stringbuf[stringcount++] = xc;
      }
      else
       stringbuf[stringcount++] = xc;
#endif
      altschirm[_n] = schirm[_n];
#ifdef NEWSTYLE
      e_print_xrect(j, i, _n);
      altextbyte[_n] = extbyte[_n];
#endif
#ifndef NOXCACHE
      oldJ = j;
#endif
     }
    } } /* _n block */
#ifndef NOXCACHE
    XDrawImageString(WpeXInfo.display, WpeXInfo.window, WpeXInfo.gc,
        oldX, oldY, stringbuf, stringcount);
#ifdef NEWSTYLE
    e_flush_xrect();
#endif
#endif
   } /* end of non-Xft path */

   fk_cursor(cur_tmp);
   fk_show_cursor();
   if (WpeRender.flush_all)
    WpeRender.flush_all();
   XFlush(WpeXInfo.display);
#ifndef NO_XWINDOWS
   {
    /* Mirror each painted frame to XWPE_X_DUMP (a PPM), like the Wayland dump,
       for headless X11 verification under Xvfb. */
    static const char *dump_path;
    static int dump_read;
    if (!dump_read) { dump_path = getenv("XWPE_X_DUMP"); dump_read = 1; }
    if (dump_path && *dump_path && WpeXInfo.window)
    {
     char tmp[1024];
     snprintf(tmp, sizeof tmp, "%s.tmp", dump_path);
     if (wpe_x_dump_ppm(tmp) == 0)
      rename(tmp, dump_path);
    }
   }
#endif
   return(0);
}

/* Common prefix of the two ConfigureNotify handlers (e_x_change and e_x_getch):
   drain the queued configures down to the final size, and -- only if the cell
   grid actually changed -- update MAXSCOL/MAXSLNS and reallocate the Xft draw /
   back buffer.  Returns 1 with the new pixel size (*pw,*ph) and the old grid
   (*old_scol,*old_slns) when it resized, 0 otherwise.  The caller does the
   repaint tail, which differs between the two sites (e_x_getch also invalidates
   the last-view pic and returns WPE_RESIZE), so only this identical prefix is
   shared.  Snaps to whole cells, like the code it replaces. */
static int e_x_apply_configure(XEvent *report, int *pw, int *ph,
                               int *old_scol, int *old_slns)
{
 int w, h;

 while (XCheckTypedWindowEvent(WpeXInfo.display, WpeXInfo.window,
        ConfigureNotify, report))
  ;
 w = (report->xconfigure.width  / WpeXInfo.font_width)  * WpeXInfo.font_width;
 h = (report->xconfigure.height / WpeXInfo.font_height) * WpeXInfo.font_height;
 if (w == MAXSCOL * WpeXInfo.font_width && h == MAXSLNS * WpeXInfo.font_height)
  return 0;

 *old_scol = MAXSCOL;
 *old_slns = MAXSLNS;
 MAXSCOL = w / WpeXInfo.font_width;
 MAXSLNS = h / WpeXInfo.font_height;
#ifdef HAVE_XFT
 if (WpeXInfo.xftfont)
 {
  if (WpeXInfo.xftdraw)
   XftDrawDestroy(WpeXInfo.xftdraw);
  if (WpeRender.resize)
  {
   WpeRender.resize(w, h);
  }
  else
  {
   if (WpeXInfo.backbuf)
    XFreePixmap(WpeXInfo.display, WpeXInfo.backbuf);
   WpeXInfo.backbuf = XCreatePixmap(WpeXInfo.display, WpeXInfo.window,
     w, h, DefaultDepth(WpeXInfo.display, WpeXInfo.screen));
   XSetForeground(WpeXInfo.display, WpeXInfo.gc,
     BlackPixel(WpeXInfo.display, WpeXInfo.screen));
   XFillRectangle(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.gc,
     0, 0, w, h);
  }
  WpeXInfo.xftdraw = XftDrawCreate(WpeXInfo.display, WpeXInfo.backbuf,
    DefaultVisual(WpeXInfo.display, WpeXInfo.screen),
    DefaultColormap(WpeXInfo.display, WpeXInfo.screen));
 }
#endif
 *pw = w;
 *ph = h;
 return 1;
}

int e_x_change(PIC *pic)
{
 XEvent report;
 XExposeEvent *expose_report;
 KeySym keysym;
 unsigned char buffer[BUFSIZE];
 int charcount;
 unsigned int key_b;
 XSizeHints size_hints;

 expose_report = (XExposeEvent *)&report;
 while (XCheckMaskEvent(WpeXInfo.display, KeyPressMask | ButtonPressMask |
		ExposureMask | StructureNotifyMask, &report) == True)
 {
  switch(report.type)
  {
   case Expose:
#ifdef HAVE_XFT
    if (WpeXInfo.xftfont)
    {
     if (XCheckTypedWindowEvent(WpeXInfo.display, WpeXInfo.window,
         ConfigureNotify, &report))
     {
      XPutBackEvent(WpeXInfo.display, &report);
      break;
     }
     do {
      XCopyArea(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.window,
        WpeXInfo.gc,
        expose_report->x, expose_report->y,
        expose_report->width, expose_report->height,
        expose_report->x, expose_report->y);
     } while (XCheckTypedWindowEvent(WpeXInfo.display, WpeXInfo.window,
              Expose, &report));
    }
    else
#endif
    {
     e_refresh_area(expose_report->x/WpeXInfo.font_width,
                            expose_report->y/WpeXInfo.font_height,
                            expose_report->width/WpeXInfo.font_width+2,
                            expose_report->height/WpeXInfo.font_height+2);
     e_refresh();
    }
    break;
   case ConfigureNotify:
    { int _pw, _ph, _old_scol, _old_slns;
      if (e_x_apply_configure(&report, &_pw, &_ph, &_old_scol, &_old_slns))
      {
       XCopyArea(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.window,
         WpeXInfo.gc, 0, 0, _pw, _ph, 0, 0);
       e_relayout_windows(WpeEditor, _old_scol, _old_slns);
       e_x_repaint_desk(WpeEditor->f[WpeEditor->mxedt]);
      }
    }
    break;
   case KeyPress:
    charcount = e_XLookupString(&report.xkey, buffer, BUFSIZE, &keysym, NULL);
    if (charcount == 1 && *buffer == CtrlC) return(CtrlC);
    break;
   case ButtonPress:
    if (!pic)
     break;
    key_b = report.xbutton.state;
    if (report.xbutton.button == 1)
    {
     e_mouse.k = (key_b & ShiftMask) ? 3 : 0 +
		         (key_b & ControlMask) ? 4 : 0 +
		         (key_b & WpeXInfo.altmask) ? 8 : 0;
     e_mouse.x = report.xbutton.x/WpeXInfo.font_width;
     e_mouse.y = report.xbutton.y/WpeXInfo.font_height;
     if (e_mouse.x > (pic->e.x + pic->a.x - 10)/2 &&
       e_mouse.x < (pic->e.x + pic->a.x + 6)/2 )
      return(CtrlC);
    }
    break;
  }
 }
 return(0);
}

typedef struct {
 KeySym dead;
 int base;
 int composed;
} e_compose_entry;

static const e_compose_entry compose_table[] = {
 { XK_dead_acute, 'a', 0xE1 }, { XK_dead_acute, 'e', 0xE9 },
 { XK_dead_acute, 'i', 0xED }, { XK_dead_acute, 'o', 0xF3 },
 { XK_dead_acute, 'u', 0xFA }, { XK_dead_acute, 'y', 0xFD },
 { XK_dead_acute, 'A', 0xC1 }, { XK_dead_acute, 'E', 0xC9 },
 { XK_dead_acute, 'I', 0xCD }, { XK_dead_acute, 'O', 0xD3 },
 { XK_dead_acute, 'U', 0xDA }, { XK_dead_acute, 'Y', 0xDD },
 { XK_dead_grave, 'a', 0xE0 }, { XK_dead_grave, 'e', 0xE8 },
 { XK_dead_grave, 'i', 0xEC }, { XK_dead_grave, 'o', 0xF2 },
 { XK_dead_grave, 'u', 0xF9 },
 { XK_dead_grave, 'A', 0xC0 }, { XK_dead_grave, 'E', 0xC8 },
 { XK_dead_grave, 'I', 0xCC }, { XK_dead_grave, 'O', 0xD2 },
 { XK_dead_grave, 'U', 0xD9 },
 { XK_dead_diaeresis, 'a', 0xE4 }, { XK_dead_diaeresis, 'e', 0xEB },
 { XK_dead_diaeresis, 'i', 0xEF }, { XK_dead_diaeresis, 'o', 0xF6 },
 { XK_dead_diaeresis, 'u', 0xFC },
 { XK_dead_diaeresis, 'A', 0xC4 }, { XK_dead_diaeresis, 'E', 0xCB },
 { XK_dead_diaeresis, 'I', 0xCF }, { XK_dead_diaeresis, 'O', 0xD6 },
 { XK_dead_diaeresis, 'U', 0xDC },
 { XK_dead_tilde, 'n', 0xF1 }, { XK_dead_tilde, 'N', 0xD1 },
 { XK_dead_tilde, 'a', 0xE3 }, { XK_dead_tilde, 'A', 0xC3 },
 { XK_dead_tilde, 'o', 0xF5 }, { XK_dead_tilde, 'O', 0xD5 },
 { XK_dead_circumflex, 'a', 0xE2 }, { XK_dead_circumflex, 'e', 0xEA },
 { XK_dead_circumflex, 'i', 0xEE }, { XK_dead_circumflex, 'o', 0xF4 },
 { XK_dead_circumflex, 'u', 0xFB },
 { XK_dead_circumflex, 'A', 0xC2 }, { XK_dead_circumflex, 'E', 0xCA },
 { XK_dead_circumflex, 'I', 0xCE }, { XK_dead_circumflex, 'O', 0xD4 },
 { XK_dead_circumflex, 'U', 0xDB },
 { XK_dead_cedilla, 'c', 0xE7 }, { XK_dead_cedilla, 'C', 0xC7 },
 { 0, 0, 0 }
};

static KeySym e_compose_pending = 0;

static int e_compose_dead(KeySym dead, int base)
{
 const e_compose_entry *e;

 for (e = compose_table; e->dead; e++)
  if (e->dead == dead && e->base == base)
   return e->composed;
 return -1;
}

extern int e_utf8_to_codepoint(unsigned char *buf, int len);

/* Discard any typed-ahead keys / clicks (X11): the analogue of e_t_flush_input,
   used after a long synchronous operation so events queued during the freeze are
   not replayed afterwards as unintended actions. */
void e_x_flush_input(void)
{
 XEvent report;
 while (XCheckMaskEvent(WpeXInfo.display,
                        KeyPressMask | ButtonPressMask | ButtonReleaseMask,
                        &report))
  ;
}

int e_x_getch()
{
 Window tmp_win, tmp_root;
 XEvent report;
 XSelectionEvent se;
 KeySym keysym;
 int charcount;
 unsigned char buffer[BUFSIZE];
 int c, root_x, root_y, x, y;
 unsigned int key_b;
 XSizeHints size_hints;

 e_refresh();

 XQueryPointer(WpeXInfo.display, WpeXInfo.window, &tmp_root, &tmp_win,
   &root_x, &root_y, &x, &y, &key_b);
 if (key_b & (Button1Mask | Button2Mask | Button3Mask))
 {
  e_mouse.x = x / WpeXInfo.font_width;
  e_mouse.y = y / WpeXInfo.font_height;
  c = 0;
  if (key_b & Button1Mask) c |= 1;
  if (key_b & Button2Mask) c |= 4;
  if (key_b & Button3Mask) c |= 2;
  return(-c);
 }

 wpe_fd_add(ConnectionNumber(WpeXInfo.display), POLLIN, NULL, NULL);

 while (1)
 {
  if (!XPending(WpeXInfo.display))
  {
   extern int e_d_async_pending;
   extern void e_d_place_cursor_in_messages(void);
   if (e_d_async_pending)
    e_d_place_cursor_in_messages();
   XFlush(WpeXInfo.display);
   wpe_fd_poll(-1);
  }
  if (!XPending(WpeXInfo.display))
   continue;
  XNextEvent(WpeXInfo.display, &report);

  switch (report.type)
  {
   case Expose:
#ifdef HAVE_XFT
    if (WpeXInfo.xftfont)
    {
     if (XCheckTypedWindowEvent(WpeXInfo.display, WpeXInfo.window,
         ConfigureNotify, &report))
     {
      XPutBackEvent(WpeXInfo.display, &report);
      break;
     }
     do {
      XCopyArea(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.window,
        WpeXInfo.gc,
        report.xexpose.x, report.xexpose.y,
        report.xexpose.width, report.xexpose.height,
        report.xexpose.x, report.xexpose.y);
     } while (XCheckTypedWindowEvent(WpeXInfo.display, WpeXInfo.window,
              Expose, &report));
    }
    else
#endif
    {
     do
     {
      /* Reason for +2 : Assumes extra character on either side. */
      e_refresh_area(report.xexpose.x / WpeXInfo.font_width,
        report.xexpose.y / WpeXInfo.font_height,
        report.xexpose.width / WpeXInfo.font_width + 2,
        report.xexpose.height / WpeXInfo.font_height + 2);
     } while (XCheckMaskEvent(WpeXInfo.display, ExposureMask, &report) == True);
     e_refresh();
    }
    break;
   case ConfigureNotify:
    { int _i, _pw, _ph, _old_scol, _old_slns;
      if (e_x_apply_configure(&report, &_pw, &_ph, &_old_scol, &_old_slns))
      {
       XCopyArea(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.window,
         WpeXInfo.gc, 0, 0, _pw, _ph, 0, 0);
       XFlush(WpeXInfo.display);

       /* If the last-view pic is not a live window's, drop it: the grid (and so
          every pic) was just resized, and e_x_repaint_desk would dereference a
          stale one. */
       { extern PIC *e_X_l_pic;
         int _is_win_pic = 0;
         for (_i = 0; _i <= WpeEditor->mxedt; _i++)
          if (e_X_l_pic == WpeEditor->f[_i]->pic) { _is_win_pic = 1; break; }
         if (!_is_win_pic)
          (*e_u_setlastpic)(NULL);
       }
       XCopyArea(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.window,
         WpeXInfo.gc, 0, 0, _pw, _ph, 0, 0);
       e_relayout_windows(WpeEditor, _old_scol, _old_slns);
       e_x_repaint_desk(WpeEditor->f[WpeEditor->mxedt]);
       return WPE_RESIZE;
      }
    }
    break;
   case ClientMessage:
    if (report.xclient.message_type == WpeXInfo.protocol_atom)
    {
     if ((report.xclient.format == 8 &&
        report.xclient.data.b[0] == WpeXInfo.delete_atom) ||
      (report.xclient.format == 16 &&
        report.xclient.data.s[0] == WpeXInfo.delete_atom) ||
      (report.xclient.format == 32 &&
        report.xclient.data.l[0] == WpeXInfo.delete_atom))
     {
      e_quit(WpeEditor->f[WpeEditor->mxedt]);
     }
    }
    break;
   case KeyPress:
    charcount = e_XLookupString(&report.xkey, buffer, BUFSIZE, &keysym,
      NULL);
    key_b = report.xkey.state;
    /* XQuartz's default "Option-as-Compose" mode delivers Alt+<letter> through
       the dead-key compose pipeline (e.g. Alt+E -> the dead_acute keysym;
       Alt+B -> the integral glyph U+222B as UTF-8) instead of as plain letter
       + altmask, which silently breaks every Alt+<letter> menu accelerator
       (and the Alt-Q LSP prefix, Alt-M Make, ...).  When Alt is held, ignore
       the composed/dead keysym and recover the unmodified letter from the
       physical keycode via XKB.  Letters, digits, '#' and Space map directly
       to the AltX codes the menu code expects.  Outside Alt this branch is
       skipped, so normal dead-key compose still works for accented characters
       typed without Alt. */
    if ((key_b & WpeXInfo.altmask) &&
        (charcount == 0 ||
         (charcount == 1 && (unsigned char)buffer[0] >= 0x80) ||
         (charcount >= 2 && ((unsigned char)buffer[0] & 0xC0) == 0xC0)))
    {
     KeySym base = XkbKeycodeToKeysym(WpeXInfo.display,
                                      report.xkey.keycode, 0, 0);
     int alt = 0;
     if (base >= XK_a && base <= XK_z)
      alt = e_tast_sim((int)(base - XK_a + 'a'));
     else if (base >= XK_A && base <= XK_Z)
      alt = e_tast_sim((int)(base - XK_A + 'a'));
     else if (base >= XK_0 && base <= XK_9)
      alt = e_tast_sim((int)(base - XK_0 + '0'));
     else if (base == XK_numbersign)
      alt = AltSYS;
     else if (base == XK_space)
      alt = AltBl;
     if (alt)
     {
      /* Eat any compose state the dead keysym would otherwise leave armed,
         so the next plain letter is not turned into an accented char. */
      e_compose_pending = 0;
      return alt;
     }
     /* Fall through if XKB returned a key we do not handle (e.g. a function
        or punctuation key); the existing decoder below covers those. */
    }
    if (charcount == 0 && keysym >= XK_dead_grave && keysym <= XK_dead_horn)
    {
     e_compose_pending = keysym;
     break;
    }
    if (e_compose_pending && charcount == 1)
    {
     int composed = e_compose_dead(e_compose_pending, buffer[0]);
     e_compose_pending = 0;
     if (composed > 0)
      return composed;
    }
    e_compose_pending = 0;
    /* Shift+Tab is delivered as the ISO_Left_Tab keysym (usually with
       charcount 0), not as Shift+Tab, so the old `ShiftMask && '\t'` test
       below never matched and dialogs got no back-tab.  Map it to WPE_BTAB
       for backward field navigation in option dialogs. */
    if (keysym == XK_ISO_Left_Tab)
     return(WPE_BTAB);
    if (charcount == 1)
    {
     if (*buffer == 127)
     {
      if (key_b & ControlMask)
       return(CENTF);
      else if (key_b & ShiftMask)
       return(ShiftDel);
      else if (key_b & WpeXInfo.altmask)
       return(AltDel);
      else
       return(ENTF);
     }
     if ((key_b & ShiftMask) && (*buffer == '\t'))
      return(WPE_BTAB);

     if (key_b & WpeXInfo.altmask)
      c = e_tast_sim(key_b & ShiftMask ? toupper(*buffer) : *buffer);
     else
      return(*buffer);
    }
    else
    {
     c = 0;
     if (key_b & ControlMask)
     {
      if (keysym == XK_Left) c = CCLE;
      else if (keysym == XK_Right) c = CCRI;
      else if (keysym == XK_Home) c = CPS1;
      else if (keysym == XK_End) c = CEND;
      else if (keysym == XK_Insert) c = CEINFG;
      else if (keysym == XK_Delete) c = CENTF;
      else if (keysym == XK_Prior) c = CBUP;
      else if (keysym == XK_Next) c = CBDO;
      else if (keysym == XK_F1) c = CF1;
      else if (keysym == XK_F2) c = CF2;
      else if (keysym == XK_F3) c = CF3;
      else if (keysym == XK_F4) c = CF4;
      else if (keysym == XK_F5) c = CF5;
      else if (keysym == XK_F6) c = CF6;
      else if (keysym == XK_F7) c = CF7;
      else if (keysym == XK_F8) c = CF8;
      else if (keysym == XK_F9) c = CF9;
      else if (keysym == XK_F10) c = CF10;
     }
     else if (key_b & WpeXInfo.altmask)
     {
      if (keysym == XK_F1) c = AF1;
      else if (keysym == XK_F2) c = AF2;
      else if (keysym == XK_F3) c = AF3;
      else if (keysym == XK_F4) c = AF4;
      else if (keysym == XK_F5) c = AF5;
      else if (keysym == XK_F6) c = AF6;
      else if (keysym == XK_F7) c = AF7;
      else if (keysym == XK_F8) c = AF8;
      else if (keysym == XK_F9) c = AF9;
      else if (keysym == XK_F10) c = AF10;
      else if (keysym == XK_Insert) c = AltEin;
      else if (keysym == XK_Delete) c = AltDel;
     }
     else
     {
      if (keysym == XK_Left) c = CLE;
      else if (keysym == XK_Right) c = CRI;
      else if (keysym == XK_Up) c = CUP;
      else if (keysym == XK_Down) c = CDO;
      else if (keysym == XK_Home) c = POS1;
      else if (keysym == XK_End) c = ENDE;
      else if (keysym == XK_Insert) c = EINFG;
      else if (keysym == XK_Delete) c = ENTF;
      else if (keysym == XK_BackSpace) c = CtrlH;
      else if (keysym == XK_Prior) c = BUP;
      else if (keysym == XK_Next) c = BDO;
      else if (keysym == XK_F1) c = F1;
      else if (keysym == XK_F2) c = F2;
      else if (keysym == XK_F3) c = F3;
      else if (keysym == XK_F4) c = F4;
      else if (keysym == XK_F5) c = F5;
      else if (keysym == XK_F6) c = F6;
      else if (keysym == XK_F7) c = F7;
      else if (keysym == XK_F8) c = F8;
      else if (keysym == XK_F9) c = F9;
      else if (keysym == XK_F10) c = F10;
      else if (keysym == XK_L1) c = STOP;
      else if (keysym == XK_L2) c = AGAIN;
      else if (keysym == XK_L3) c = PROPS;
      else if (keysym == XK_L4) c = UNDO;
      else if (keysym == XK_L5) c = FRONT;
      else if (keysym == XK_L6) c = COPY;
      else if (keysym == XK_L7) c = OPEN;
      else if (keysym == XK_L8) c = PASTE;
      else if (keysym == XK_L9) c = FID;
      else if (keysym == XK_L10) c = CUT;
      else if (keysym == XK_Help) c = HELP;
     }
    }
    if (c != 0)
    {
     if (key_b & ShiftMask)
      c = c + 512;
     return(c);
    }
    if (charcount >= 2 && (buffer[0] & 0xC0) == 0xC0)
    {
     c = e_utf8_to_codepoint(buffer, charcount);
     if (c > 0)
      return(c);
    }
    break;
   case ButtonPress:
    if (report.xbutton.button == 4)
     return(WPE_SCROLL_UP);
    if (report.xbutton.button == 5)
     return(WPE_SCROLL_DOWN);
    key_b = report.xbutton.state;
    e_mouse.k = (key_b & ShiftMask) ? 3 : 0 +
      (key_b & ControlMask) ? 4 : 0 +
      (key_b & WpeXInfo.altmask) ? 8 : 0;
    e_mouse.x = report.xbutton.x/WpeXInfo.font_width;
    e_mouse.y = report.xbutton.y/WpeXInfo.font_height;
    c = 0;
    if (report.xbutton.button == 1) c |= 1;
    if (report.xbutton.button == 2) c |= 2;
    if (report.xbutton.button == 3) c |= 4;
    return(-c);
   case SelectionRequest:
    /* Another app asks for the text we copied.  We own both PRIMARY and
       CLIPBOARD; the requestor's `selection` field says which, and we serve
       the same cached bytes for either.  Answer TARGETS (the list a GTK/Qt app
       probes first), and serve UTF8_STRING or STRING with the UTF-8 bytes. */
    if (WpeXInfo.selection)
    {
     XSelectionRequestEvent *req = &report.xselectionrequest;

     se.type = SelectionNotify;
     se.display = req->display;
     se.requestor = req->requestor;
     se.selection = req->selection;
     se.time = req->time;
     se.target = req->target;
     se.property = (req->property == None) ? req->target : req->property;
     if (req->target == WpeXInfo.targets_atom)
     {
      Atom targets[3];
      targets[0] = WpeXInfo.targets_atom;
      targets[1] = WpeXInfo.utf8_atom;
      targets[2] = WpeXInfo.text_atom;
      XChangeProperty(se.display, se.requestor, se.property, XA_ATOM, 32,
        PropModeReplace, (unsigned char *)targets, 3);
     }
     else if (req->target == WpeXInfo.utf8_atom ||
              req->target == WpeXInfo.text_atom)
      XChangeProperty(se.display, se.requestor, se.property, req->target, 8,
        PropModeReplace, WpeXInfo.selection, strlen(WpeXInfo.selection));
     else
      se.property = None;
     XSendEvent(WpeXInfo.display, se.requestor, False, 0, (XEvent *)&se);
    }
    break;
   case SelectionClear:
    /* We lost ONE selection (PRIMARY or CLIPBOARD) to another app.  Only drop
       the cached bytes once we own NEITHER, so losing CLIPBOARD does not stop
       us serving a still-owned PRIMARY (and vice versa). */
    if (WpeXInfo.selection &&
        XGetSelectionOwner(WpeXInfo.display, WpeXInfo.selection_atom)
          != WpeXInfo.window &&
        XGetSelectionOwner(WpeXInfo.display, WpeXInfo.clipboard_atom)
          != WpeXInfo.window)
    {
     WpeFree(WpeXInfo.selection);
     WpeXInfo.selection = NULL;
    }
    break;
   default:
    break;
  }
 }
 return(0);
}

int e_x_kbhit()
{
 XEvent report;
 KeySym keysym;
 int charcount;
 unsigned char buffer[BUFSIZE];
 int c;
 unsigned int key_b;

 e_refresh();

 if (XCheckMaskEvent(WpeXInfo.display, ButtonPressMask | KeyPressMask, &report) == False)
  return(0);

 if (report.type == ButtonPress)
 {
  key_b = report.xbutton.state;
  e_mouse.k = (key_b & ShiftMask) ? 3 : 0;
  e_mouse.x = report.xbutton.x/WpeXInfo.font_width;
  e_mouse.y = report.xbutton.y/WpeXInfo.font_height;
  c = 0;
  if(report.xbutton.button == 1) c |= 1;
  if(report.xbutton.button == 2) c |= 2;
  if(report.xbutton.button == 3) c |= 4;
  return(-c);
 }
 else
 {
  charcount = e_XLookupString(&report.xkey, buffer, BUFSIZE,
						&keysym, NULL);
  if (charcount == 0 && keysym >= XK_dead_grave && keysym <= XK_dead_horn)
  {
   e_compose_pending = keysym;
   return 0;
  }
  if (e_compose_pending && charcount == 1)
  {
   int composed = e_compose_dead(e_compose_pending, buffer[0]);
   e_compose_pending = 0;
   if (composed > 0) return composed;
  }
  e_compose_pending = 0;
  if (charcount >= 2 && (buffer[0] & 0xC0) == 0xC0)
  {
   int cp = e_utf8_to_codepoint(buffer, charcount);
   if (cp > 0) return cp;
  }
  if(charcount == 1) return(*buffer);
  return(0);
 }
}

void e_setlastpic(PIC *pic)
{
 extern PIC *e_X_l_pic;

 e_X_l_pic = pic;
}

int fk_x_locate(int x, int y)
{
 cur_x = x;
 return(cur_y = y);
}

int fk_x_cursor(int x)
{
 return(cur_on = x);
}

int e_x_sys_ini()
{
 return(0);
}

int e_x_sys_end()
{
 return(0);
}

static Time e_x_get_server_time(void)
{
 XEvent ev;
 Time ts = CurrentTime;

 XChangeProperty(WpeXInfo.display, WpeXInfo.window,
   XA_WM_NAME, XA_STRING, 8, PropModeAppend, (unsigned char *)"", 0);
 XFlush(WpeXInfo.display);
 while (XCheckTypedWindowEvent(WpeXInfo.display, WpeXInfo.window,
        PropertyNotify, &ev))
  ts = ev.xproperty.time;
 return ts;
}

static void e_x_send_active_window(Time ts)
{
 XEvent ev;

 memset(&ev, 0, sizeof(ev));
 ev.type = ClientMessage;
 ev.xclient.window = WpeXInfo.window;
 ev.xclient.message_type = XInternAtom(WpeXInfo.display,
   "_NET_ACTIVE_WINDOW", False);
 ev.xclient.format = 32;
 ev.xclient.data.l[0] = 1;
 ev.xclient.data.l[1] = ts;
 XSendEvent(WpeXInfo.display, DefaultRootWindow(WpeXInfo.display),
   False, SubstructureNotifyMask | SubstructureRedirectMask, &ev);
}

void e_x_reclaim_focus(void)
{
 Time ts = e_x_get_server_time();

 XRaiseWindow(WpeXInfo.display, WpeXInfo.window);
 e_x_send_active_window(ts);
 XSetInputFocus(WpeXInfo.display, WpeXInfo.window,
   RevertToParent, ts);
 XFlush(WpeXInfo.display);
}

/**
 * Clears the pixel area to remove stale XDrawLine segments from
 * NEWSTYLE borders after popup/menu close.
 */
void e_x_clear_area(int xa, int ya, int w, int h)
{
#ifdef HAVE_XFT
 if (WpeXInfo.xftfont)
 {
  int px = WpeXInfo.font_width * xa;
  int py = WpeXInfo.font_height * ya;
  int pw = WpeXInfo.font_width * w;
  int ph = WpeXInfo.font_height * h;
  /* Disabled: Pixmap clearing causes visible flash during window move.
     The altschirm invalidation in e_invalidate_area is sufficient. */
 }
 else
#endif
 {
  XClearArea(WpeXInfo.display, WpeXInfo.window,
    WpeXInfo.font_width * xa, WpeXInfo.font_height * ya,
    WpeXInfo.font_width * w, WpeXInfo.font_height * h, False);
 }
}

int fk_x_putchar(int c)
{
 return(fputc(c, stdout));
}

int x_bioskey()
{
 return(e_mouse.k);
}

int e_x_system(const char *exe)
{
 FILE *fp;
 int ret;
 char file[80];
 char *string;

 sprintf(file, "%s/we_sys_tmp", e_tmp_dir);
 string = MALLOC(strlen(XTERM_CMD) + strlen(exe) + strlen(file) +
   strlen(user_shell) + 40);
 if (!(fp = fopen(file, "w+")))
 {
  FREE(string);
  return(-1);
 }
 fputs("$*\necho type \\<Return\\> to continue\nread i\n", fp);
 fclose(fp);
 chmod(file, 0700);
 if (exe[0] == '/')
  sprintf(string, "%s -geometry 80x25-0-0 +sb -e %s %s %s", XTERM_CMD,
    user_shell, file, exe);
 else
  sprintf(string, "%s -geometry 80x25-0-0 +sb -e %s %s ./%s", XTERM_CMD,
    user_shell, file, exe);
 ret = system(string);
 remove(file);
 FREE(string);
 return(ret);
}

int e_x_repaint_desk(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i, g[4];
 extern PIC *e_X_l_pic;
 PIC *sv_pic = NULL, *nw_pic = NULL;

 if (e_X_l_pic && e_X_l_pic != cn->f[cn->mxedt]->pic)
 {
  sv_pic = e_X_l_pic;
  nw_pic = e_open_view(e_X_l_pic->a.x, e_X_l_pic->a.y, e_X_l_pic->e.x,
    e_X_l_pic->e.y, 0, 2);
 }
 e_ini_size();
 if (cn->mxedt < 1)
 {
  e_cls(f->fb->df.fb, f->fb->dc);
  e_ini_desk(f->ed);
  if (nw_pic)
  {
   e_close_view(nw_pic, 1);
   e_X_l_pic = sv_pic;
  }
  return(0);
 }
 ini_repaint(cn);
 e_abs_refr();
 memset(altextbyte, 0, MAXSCOL * MAXSLNS);
 for (i = cn->mxedt; i >= 1; i--)
  e_free_view(&cn->f[i]->pic);
 /* relayout already done by ConfigureNotify handler -- just clamp */
 for (i = 0; i <= cn->mxedt; i++)
 {
  if (cn->f[i]->e.x >= MAXSCOL) cn->f[i]->e.x = MAXSCOL - 1;
  if (cn->f[i]->e.y >= MAXSLNS - 1) cn->f[i]->e.y = MAXSLNS - 2;
  if (cn->f[i]->a.y >= cn->f[i]->e.y) cn->f[i]->a.y = cn->f[i]->e.y - 3;
  if (cn->f[i]->a.x >= cn->f[i]->e.x) cn->f[i]->a.x = cn->f[i]->e.x - 26;
  if (cn->f[i]->a.y < 1) cn->f[i]->a.y = 1;
  if (cn->f[i]->a.x < 0) cn->f[i]->a.x = 0;
 }
 for (i = 1; i < cn->mxedt; i++)
 {
  e_firstl(cn->f[i], 0);
  e_schirm(cn->f[i], 0);
 }
 e_firstl(cn->f[i], 1);
 e_schirm(cn->f[i], 1);
 if (nw_pic)
 {
  e_close_view(nw_pic, 1);
  e_X_l_pic = sv_pic;
 }
 g[0] = 2; fk_mouse(g);
 end_repaint();
 e_cursor(cn->f[i], 1);
 g[0] = 0; fk_mouse(g);
 g[0] = 1; fk_mouse(g);
 return(0);
}


int fk_x_mouse(int *g)
{
 Window tmp_win, tmp_root;
 int root_x, root_y, x, y;
 unsigned int key_b;

 if (!XQueryPointer(WpeXInfo.display, WpeXInfo.window, &tmp_root, &tmp_win,
   &root_x, &root_y, &x, &y, &key_b))
 {
  g[2] = e_mouse.x * 8;
  g[3] = e_mouse.y * 8;
  g[0] = g[1] = 0;
  return(0);
 }
 g[0] = 0;
 if (key_b & Button1Mask)
  g[0] |= 1;
 if(key_b & Button2Mask)
  g[0] |= 4;
 if(key_b & Button3Mask)
  g[0] |= 2;
 g[1] = g[0];
 g[2] = x/WpeXInfo.font_width * 8;
 g[3] = y/WpeXInfo.font_height * 8;
 return(g[1]);
}

/* X11 scrollbar-drag pointer grab: capture button/motion for the whole drag. */
int fk_x_grab_pointer(int on)
{
 if (on)
  XGrabPointer(WpeXInfo.display, WpeXInfo.window, False,
    Button1MotionMask | ButtonReleaseMask,
    GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
 else
  XUngrabPointer(WpeXInfo.display, CurrentTime);
 return(0);
}

/* X11 scrollbar-drag event step: 0 on release (done), -1 on an unrelated event,
   1 on motion with the pointer position (pixels) in *px/*py.  Queued motions are
   coalesced so the drag tracks the latest position, not every intermediate one. */
int fk_x_drag_next(int *px, int *py)
{
 XEvent ev;

 XNextEvent(WpeXInfo.display, &ev);
 if (ev.type == ButtonRelease)
  return(0);
 if (ev.type != MotionNotify)
  return(-1);

 while (XCheckTypedWindowEvent(WpeXInfo.display, WpeXInfo.window,
        MotionNotify, &ev))
  ;
 *px = ev.xmotion.x;
 *py = ev.xmotion.y;
 return(1);
}

/* e_clip_x_wait_notify - wait, event-driven, up to ~1s for the SelectionNotify
   that answers an XConvertSelection.  Returns 1 with the event in *ev, or 0 on
   timeout.  poll()s the X connection rather than busy-looping on sleep(0)
   (no busy-wait, no timing hacks); non-matching events stay queued
   (XCheckTypedEvent). */
static int e_clip_x_wait_notify(XEvent *ev)
{
 int fd = ConnectionNumber(WpeXInfo.display);
 int waited;
 struct pollfd pfd;

 for (waited = 0; waited < 1000; waited += 100)
 {
  if (XCheckTypedEvent(WpeXInfo.display, SelectionNotify, ev))
   return 1;
  pfd.fd = fd;
  pfd.events = POLLIN;
  if (poll(&pfd, 1, 100) > 0)
   XEventsQueued(WpeXInfo.display, QueuedAfterReading);
 }
 return XCheckTypedEvent(WpeXInfo.display, SelectionNotify, ev);
}

int e_x_cp_X_to_buffer(FENSTER *f)
{
 BUFFER *b0 = f->ed->f[0]->b;
 SCHIRM *s0 = f->ed->f[0]->s;
 int i, j, k, n;
 unsigned char *str;
 XEvent report;
 Atom type;
 int format;
 long nitems, bytes_left;

 for (i = 1; i < b0->mxlines; i++)
  FREE(b0->bf[i].s);
 b0->mxlines = 1;
 *(b0->bf[0].s) = WPE_WR;
 *(b0->bf[0].s+1) = '\0';
 b0->bf[0].len = 0;
#if SELECTION
 if (WpeXInfo.selection)
 {
  str = WpeStrdup(WpeXInfo.selection);
  n = strlen(str);
 }
 else
 {
  /* Pull the OS clipboard: try CLIPBOARD first (the ^C/^V selection of GTK/Qt
     apps), then PRIMARY (middle-click), each as UTF8_STRING.  Event-driven --
     no busy-loop -- and AnyPropertyType so we accept whatever the owner sends. */
  Atom sels[2];
  int si, done = 0;

  sels[0] = WpeXInfo.clipboard_atom;
  sels[1] = WpeXInfo.selection_atom;
  str = NULL;
  for (si = 0; si < 2 && !done; si++)
  {
   if (XGetSelectionOwner(WpeXInfo.display, sels[si]) == None)
    continue;
   XConvertSelection(WpeXInfo.display, sels[si], WpeXInfo.utf8_atom,
     WpeXInfo.property_atom, WpeXInfo.window, CurrentTime);
   XFlush(WpeXInfo.display);
   if (!e_clip_x_wait_notify(&report) || report.xselection.property == None)
    continue;
   if (XGetWindowProperty(WpeXInfo.display, WpeXInfo.window,
       WpeXInfo.property_atom, 0, 1000000, True, AnyPropertyType,
       &type, &format, &nitems, &bytes_left, &str) == Success
       && type != None && nitems > 0 && str)
    done = 1;
   else if (str)
   {
    XFree(str);
    str = NULL;
   }
  }
  if (!done || !str)
   return 0;
  n = (int)nitems;
 }
#else
 str = XFetchBytes(WpeXInfo.display, &n);
#endif
 for (i = k = 0; i < n; i++, k++)
 {
  for (j = 0; i < n && str[i] != '\n' && j < b0->mx.x-1; j++, i++)
   b0->bf[k].s[j] = str[i];
  if (i < n)
  {
   e_new_line(k+1, b0);
   if (str[i] == '\n')
   {
    b0->bf[k].s[j] = WPE_WR;
    b0->bf[k].nrc = j+1;
   }
   else
    b0->bf[k].nrc = j;
   b0->bf[k].s[j+1] = '\0';
   b0->bf[k].len = j;
  }
  else
  {
   b0->bf[k].s[j] = '\0';
   b0->bf[k].nrc = b0->bf[k].len = j;
  }
 }
 s0->mark_begin.x = s0->mark_begin.y = 0;
 s0->mark_end.y = b0->mxlines-1;
 s0->mark_end.x = b0->bf[b0->mxlines-1].len;
#if SELECTION
 if (WpeXInfo.selection)
  WpeFree(str);
 else
#endif
  XFree(str);
 return 0;
}

/* e_clip_x_set - X11 OS-clipboard writer (the e_clip_os_set backend).
   Caches the UTF-8 bytes and takes ownership of BOTH the PRIMARY (middle-click
   paste) and CLIPBOARD (^C/^V in GTK/Qt apps) selections, so a plain Copy in
   xwpe is pasteable everywhere.  The SelectionRequest handler then serves these
   bytes as UTF8_STRING/STRING and answers TARGETS.  Installed at X11 start-up,
   so e_edt_copy's transparent export reaches the X selections. */
static void e_clip_x_set(const char *utf8, int len)
{
 if (len < 0)
  return;
 if (WpeXInfo.selection)
 {
  WpeFree(WpeXInfo.selection);
  WpeXInfo.selection = NULL;
 }
 WpeXInfo.selection = WpeMalloc(len + 1);
 memcpy(WpeXInfo.selection, utf8, (size_t)len);
 WpeXInfo.selection[len] = '\0';
 XSetSelectionOwner(WpeXInfo.display, WpeXInfo.selection_atom,
                    WpeXInfo.window, CurrentTime);
 XSetSelectionOwner(WpeXInfo.display, WpeXInfo.clipboard_atom,
                    WpeXInfo.window, CurrentTime);
 XFlush(WpeXInfo.display);
}

/* e_clip_x_get - X11 OS-clipboard reader (the e_clip_os_get backend).
   Returns NULL when we ourselves own the selection (so Paste uses xwpe's own
   internal clipboard, no round-trip) or when nobody owns it.  Otherwise fetches
   the external owner's text -- CLIPBOARD first, then PRIMARY -- as UTF8_STRING,
   event-driven, and returns it malloc'd + NUL-terminated (caller frees). */
static char *e_clip_x_get(int *len)
{
 Atom sels[2];
 int si;

 if (XGetSelectionOwner(WpeXInfo.display, WpeXInfo.clipboard_atom)
       == WpeXInfo.window ||
     XGetSelectionOwner(WpeXInfo.display, WpeXInfo.selection_atom)
       == WpeXInfo.window)
  return NULL;

 sels[0] = WpeXInfo.clipboard_atom;
 sels[1] = WpeXInfo.selection_atom;
 for (si = 0; si < 2; si++)
 {
  XEvent ev;
  Atom type;
  int format;
  unsigned long nitems, bytes_left;
  unsigned char *prop = NULL;

  if (XGetSelectionOwner(WpeXInfo.display, sels[si]) == None)
   continue;
  XConvertSelection(WpeXInfo.display, sels[si], WpeXInfo.utf8_atom,
                    WpeXInfo.property_atom, WpeXInfo.window, CurrentTime);
  XFlush(WpeXInfo.display);
  if (!e_clip_x_wait_notify(&ev) || ev.xselection.property == None)
   continue;
  if (XGetWindowProperty(WpeXInfo.display, WpeXInfo.window,
        WpeXInfo.property_atom, 0, 1000000, True, AnyPropertyType,
        &type, &format, &nitems, &bytes_left, &prop) == Success
      && type != None && nitems > 0 && prop)
  {
   char *out = malloc((size_t)nitems + 1);
   if (out)
   {
    memcpy(out, prop, (size_t)nitems);
    out[nitems] = '\0';
    *len = (int)nitems;
   }
   XFree(prop);
   return out;
  }
  if (prop)
   XFree(prop);
 }
 return NULL;
}

int e_x_copy_X_buffer(FENSTER *f)
{
 e_cp_X_to_buffer(f);
 e_edt_einf(f);
 return(0);
}

int e_x_paste_X_buffer(FENSTER *f)
{
 BUFFER *b0 = f->ed->f[0]->b;
 SCHIRM *s0 = f->ed->f[0]->s;
 int i, j, n;

 e_edt_copy(f);
#if SELECTION
 if (WpeXInfo.selection)
 {
  WpeFree(WpeXInfo.selection);
  WpeXInfo.selection = NULL;
 }
#endif
 if ((s0->mark_end.y == 0 && s0->mark_end.x == 0) ||
   s0->mark_end.y < s0->mark_begin.y)
  return(0);
 if (s0->mark_end.y == s0->mark_begin.y)
 {
  if (s0->mark_end.x < s0->mark_begin.x)
   return(0);
  n = s0->mark_end.x - s0->mark_begin.x;
#if SELECTION
  WpeXInfo.selection = WpeMalloc(n + 1);
  strncpy(WpeXInfo.selection, b0->bf[s0->mark_begin.y].s+s0->mark_begin.x,
    n);
  WpeXInfo.selection[n] = 0;
  XSetSelectionOwner(WpeXInfo.display, WpeXInfo.selection_atom,
    WpeXInfo.window, CurrentTime);
#else
  XStoreBytes(WpeXInfo.display, b0->bf[s0->mark_begin.y].s+s0->mark_begin.x,
    n);
#endif
  return(0);
 }
 WpeXInfo.selection = WpeMalloc(b0->bf[s0->mark_begin.y].nrc * sizeof(char));
 for (n = 0, j = s0->mark_begin.x; j < b0->bf[s0->mark_begin.y].nrc; j++, n++)
  WpeXInfo.selection[n] = b0->bf[s0->mark_begin.y].s[j];
 for (i = s0->mark_begin.y+1; i < s0->mark_end.y; i++)
 {
  WpeXInfo.selection = WpeRealloc(WpeXInfo.selection, (n + b0->bf[i].nrc)*sizeof(char));
  for (j = 0; j < b0->bf[i].nrc; j++, n++)
   WpeXInfo.selection[n] = b0->bf[i].s[j];
 }
 WpeXInfo.selection = WpeRealloc(WpeXInfo.selection, (n + s0->mark_end.x + 1)*sizeof(char));
 for (j = 0; j < s0->mark_end.x; j++, n++)
  WpeXInfo.selection[n] = b0->bf[i].s[j];
 WpeXInfo.selection[n] = 0;
#if SELECTION
 XSetSelectionOwner(WpeXInfo.display, WpeXInfo.selection_atom,
   WpeXInfo.window, CurrentTime);
#else
 XStoreBytes(WpeXInfo.display, WpeXInfo.selection, n);
 WpeFree(WpeXInfo.selection);
 WpeXInfo.selection = NULL;
#endif
 return(0);
}

#endif

