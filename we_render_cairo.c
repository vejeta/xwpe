/* we_render_cairo.c -- Cairo + Pango rendering backend for X11    */
/* Copyright (C) 2026 Juan Manuel Mendez Rey                      */
/* This is free software; see the file COPYING.                    */

#ifdef HAVE_CAIRO
#ifdef HAVE_PANGO

#include "we_render.h"
#include "edit.h"
#include <cairo.h>
#include <cairo-xlib.h>
#include <pango/pangocairo.h>
#include <X11/Xlib.h>
#include <string.h>

#ifndef NO_XWINDOWS
#include "WeXterm.h"
#endif

static cairo_surface_t *cr_surface;
static cairo_t *cr;
static PangoLayout *pg_layout;
static PangoFontDescription *pg_font;

static double cairo_colors[16][3];

static void cairo_init_colors(void)
{
 int i;
#ifndef NO_XWINDOWS
 Colormap cmap = DefaultColormap(WpeXInfo.display, WpeXInfo.screen);
 for (i = 0; i < 16; i++)
 {
  XColor xc;
  xc.pixel = WpeXInfo.colors[i];
  XQueryColor(WpeXInfo.display, cmap, &xc);
  cairo_colors[i][0] = xc.red   / 65535.0;
  cairo_colors[i][1] = xc.green / 65535.0;
  cairo_colors[i][2] = xc.blue  / 65535.0;
 }
#else
 for (i = 0; i < 16; i++)
  cairo_colors[i][0] = cairo_colors[i][1] = cairo_colors[i][2] = 0.0;
#endif
}

static void cr_draw_rect(int x, int y, int w, int h, int color_idx)
{
 cairo_set_source_rgb(cr, cairo_colors[color_idx][0],
   cairo_colors[color_idx][1], cairo_colors[color_idx][2]);
 cairo_rectangle(cr, x, y, w, h);
 cairo_fill(cr);
}

static void cr_draw_text(int x, int y, const char *utf8, int u8len,
                         int cell_width, int fg_idx, int bg_idx)
{
 cr_draw_rect(x, y, WpeRender.font_width * cell_width,
   WpeRender.font_height, bg_idx);
 cairo_set_source_rgb(cr, cairo_colors[fg_idx][0],
   cairo_colors[fg_idx][1], cairo_colors[fg_idx][2]);
 cairo_move_to(cr, x, y);
 pango_layout_set_text(pg_layout, utf8, u8len);
 pango_cairo_show_layout(cr, pg_layout);
}

static void cr_draw_line(int x1, int y1, int x2, int y2,
                         int color_idx, int width)
{
 cairo_set_source_rgb(cr, cairo_colors[color_idx][0],
   cairo_colors[color_idx][1], cairo_colors[color_idx][2]);
 cairo_set_line_width(cr, width);
 cairo_move_to(cr, x1 + 0.5, y1 + 0.5);
 cairo_line_to(cr, x2 + 0.5, y2 + 0.5);
 cairo_stroke(cr);
}

static void cr_clear_rect(int x, int y, int w, int h, int color_idx)
{
 cr_draw_rect(x, y, w, h, color_idx);
}

static void cr_draw_acs(int sc, int px, int py, int fg_idx, int bg_idx)
{
 int fw = WpeRender.font_width;
 int fh = WpeRender.font_height;
 int lw = fw > 8 ? 2 : 1;
 int mx = px + fw / 2;
 int my = py + fh / 2;

 cr_draw_rect(px, py, fw, fh, bg_idx);

 cairo_set_source_rgb(cr, cairo_colors[fg_idx][0],
   cairo_colors[fg_idx][1], cairo_colors[fg_idx][2]);

 switch (sc)
 {
 case 1: /* upper-left corner */
  cairo_rectangle(cr, mx, my, fw - fw/2, lw);
  cairo_fill(cr);
  cairo_rectangle(cr, mx, my, lw, fh - fh/2);
  cairo_fill(cr);
  break;
 case 2: /* upper-right corner */
  cairo_rectangle(cr, px, my, fw/2 + lw, lw);
  cairo_fill(cr);
  cairo_rectangle(cr, mx, my, lw, fh - fh/2);
  cairo_fill(cr);
  break;
 case 3: /* lower-left corner */
  cairo_rectangle(cr, mx, my, fw - fw/2, lw);
  cairo_fill(cr);
  cairo_rectangle(cr, mx, py, lw, fh/2 + lw);
  cairo_fill(cr);
  break;
 case 4: /* lower-right corner */
  cairo_rectangle(cr, px, my, fw/2 + lw, lw);
  cairo_fill(cr);
  cairo_rectangle(cr, mx, py, lw, fh/2 + lw);
  cairo_fill(cr);
  break;
 case 5: /* horizontal line */
  cairo_rectangle(cr, px, my, fw, lw);
  cairo_fill(cr);
  break;
 case 6: case 8: case 9: /* vertical line */
  cairo_rectangle(cr, mx, py, lw, fh);
  cairo_fill(cr);
  break;
 case 7: case 10: /* scrollbar track (stipple) */
  { int tx, ty;
    for (ty = py; ty < py + fh; ty += 2)
     for (tx = px; tx < px + fw; tx += 2)
      cairo_rectangle(cr, tx, ty, 1, 1);
    cairo_fill(cr);
  }
  break;
 case 11: /* scrollbar thumb */
  { int m = fw > 8 ? 2 : 1;
    cairo_rectangle(cr, px + m, py + m, fw - 2*m, fh - 2*m);
    cairo_fill(cr);
  }
  break;
 }
}

static void cr_flush(int x, int y, int w, int h)
{
#ifndef NO_XWINDOWS
 cairo_surface_flush(cr_surface);
 XCopyArea(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.window,
   WpeXInfo.gc, x, y, w, h, x, y);
#endif
}

static void cr_flush_all(void)
{
#ifndef NO_XWINDOWS
 extern int MAXSLNS, MAXSCOL;
 cr_flush(0, 0, WpeRender.font_width * MAXSCOL,
   WpeRender.font_height * MAXSLNS);
#endif
}

static void cr_resize(int pixel_w, int pixel_h)
{
#ifndef NO_XWINDOWS
 Pixmap old_buf = WpeXInfo.backbuf;

 WpeXInfo.backbuf = XCreatePixmap(WpeXInfo.display, WpeXInfo.window,
   pixel_w, pixel_h,
   DefaultDepth(WpeXInfo.display, WpeXInfo.screen));

 XSetForeground(WpeXInfo.display, WpeXInfo.gc,
   BlackPixel(WpeXInfo.display, WpeXInfo.screen));
 XFillRectangle(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.gc,
   0, 0, pixel_w, pixel_h);

 if (cr)
  cairo_destroy(cr);
 if (cr_surface)
  cairo_surface_destroy(cr_surface);

 cr_surface = cairo_xlib_surface_create(WpeXInfo.display,
   WpeXInfo.backbuf,
   DefaultVisual(WpeXInfo.display, WpeXInfo.screen),
   pixel_w, pixel_h);
 cr = cairo_create(cr_surface);
 cairo_set_antialias(cr, CAIRO_ANTIALIAS_GRAY);

 if (old_buf)
  XFreePixmap(WpeXInfo.display, old_buf);
#endif
}

static void cr_cleanup(void)
{
 if (pg_font)
 {
  pango_font_description_free(pg_font);
  pg_font = NULL;
 }
 if (pg_layout)
 {
  g_object_unref(pg_layout);
  pg_layout = NULL;
 }
 if (cr)
 {
  cairo_destroy(cr);
  cr = NULL;
 }
 if (cr_surface)
 {
  cairo_surface_destroy(cr_surface);
  cr_surface = NULL;
 }
}

int wpe_render_cairo_init(void)
{
#ifndef NO_XWINDOWS
 extern int MAXSLNS, MAXSCOL;
 PangoFontMetrics *metrics;
 PangoContext *pg_ctx;
 int pixel_w, pixel_h;
 int pt_size;
 char font_spec[64];

 cairo_init_colors();

 pixel_w = WpeXInfo.font_width * MAXSCOL;
 pixel_h = WpeXInfo.font_height * MAXSLNS;

 cr_surface = cairo_xlib_surface_create(WpeXInfo.display,
   WpeXInfo.backbuf,
   DefaultVisual(WpeXInfo.display, WpeXInfo.screen),
   pixel_w, pixel_h);
 cr = cairo_create(cr_surface);
 cairo_set_antialias(cr, CAIRO_ANTIALIAS_GRAY);

 pg_layout = pango_cairo_create_layout(cr);
 pg_font = pango_font_description_from_string("monospace");

 /* Shrink font until layout fits within Xft cell grid. */
 { int abs_px = WpeXInfo.font_height;
   PangoRectangle logical;
   for (;;)
   {
    pango_font_description_set_absolute_size(pg_font,
      abs_px * PANGO_SCALE);
    pango_layout_set_font_description(pg_layout, pg_font);
    pango_layout_set_text(pg_layout, "M", 1);
    pango_layout_get_pixel_extents(pg_layout, NULL, &logical);
    if (logical.height <= WpeXInfo.font_height
        && logical.width <= WpeXInfo.font_width)
     break;
    abs_px--;
    if (abs_px < 4) break;
   }
 }

 pg_ctx = pango_layout_get_context(pg_layout);
 metrics = pango_context_get_metrics(pg_ctx, pg_font, NULL);

 { PangoRectangle logical;
   int pg_w, pg_h;
   FILE *dbg;

   pango_layout_set_text(pg_layout, "M", 1);
   pango_layout_get_pixel_extents(pg_layout, NULL, &logical);
   pg_w = logical.width;
   pg_h = logical.height;

   dbg = fopen("/home/mendezr/development/debian/xwpe-dev/tmp/font-metrics-debug.txt", "w");
   if (dbg)
   {
    fprintf(dbg, "=== Font Metrics (fitted) ===\n");
    fprintf(dbg, "Xft cell:  w=%d h=%d asc=%d desc=%d\n",
      WpeXInfo.font_width, WpeXInfo.font_height,
      WpeXInfo.xftfont->ascent, WpeXInfo.xftfont->descent);
    fprintf(dbg, "Pango 'M': w=%d h=%d\n", pg_w, pg_h);
    fprintf(dbg, "Delta:     w=%+d h=%+d\n",
      pg_w - WpeXInfo.font_width, pg_h - WpeXInfo.font_height);
    fclose(dbg);
   }
 }

 WpeRender.font_width  = WpeXInfo.font_width;
 WpeRender.font_height = WpeXInfo.font_height;
 WpeRender.font_ascent = pango_font_metrics_get_ascent(metrics)
                         / PANGO_SCALE;

 pango_font_metrics_unref(metrics);

 WpeRender.draw_rect   = cr_draw_rect;
 WpeRender.draw_text   = cr_draw_text;
 WpeRender.draw_line   = cr_draw_line;
 WpeRender.clear_rect  = cr_clear_rect;
 WpeRender.draw_acs    = cr_draw_acs;
 WpeRender.flush       = cr_flush;
 WpeRender.flush_all   = cr_flush_all;
 WpeRender.resize      = cr_resize;
 WpeRender.cleanup     = cr_cleanup;

 return 0;
#else
 return -1;
#endif
}

static void cr_chrome_arrow_up(int cx, int cy, int sz, int ci)
{
 cairo_set_source_rgb(cr, cairo_colors[ci][0],
   cairo_colors[ci][1], cairo_colors[ci][2]);
 cairo_move_to(cr, cx, cy - sz);
 cairo_line_to(cr, cx - sz, cy + sz / 2);
 cairo_line_to(cr, cx + sz, cy + sz / 2);
 cairo_close_path(cr);
 cairo_fill(cr);
}

static void cr_chrome_arrow_down(int cx, int cy, int sz, int ci)
{
 cairo_set_source_rgb(cr, cairo_colors[ci][0],
   cairo_colors[ci][1], cairo_colors[ci][2]);
 cairo_move_to(cr, cx, cy + sz);
 cairo_line_to(cr, cx - sz, cy - sz / 2);
 cairo_line_to(cr, cx + sz, cy - sz / 2);
 cairo_close_path(cr);
 cairo_fill(cr);
}

static void cr_chrome_track(int x, int y, int w, int h, int ci)
{
 cairo_set_source_rgb(cr, cairo_colors[ci][0] * 0.5,
   cairo_colors[ci][1] * 0.5, cairo_colors[ci][2] * 0.5);
 cairo_rectangle(cr, x, y, w, h);
 cairo_fill(cr);
}

static void cr_chrome_thumb(int x, int y, int w, int h, int ci)
{
 cairo_set_source_rgb(cr, cairo_colors[ci][0],
   cairo_colors[ci][1], cairo_colors[ci][2]);
 cairo_rectangle(cr, x, y, w, h);
 cairo_fill(cr);
}

static void cr_chrome_vscrollbar(FENSTER *f, int ci_bg, int ci_fg)
{
 int fw = WpeRender.font_width;
 int fh = WpeRender.font_height;
 int ay = f->a.y, ey = f->e.y, ex = f->e.x;
 int col_x = ex * fw;
 int top = (ay + 1) * fh;
 int bot = ey * fh;
 int h = bot - top;
 int bar_w = fw > 8 ? 5 : 4;
 int bar_x = col_x + (fw - bar_w) / 2;
 int arrow_sz = fw > 8 ? 5 : 3;
 int track_top, track_h, thumb_h, thumb_y;

 if (h < fh * 3)
  return;

 cr_draw_rect(col_x, top, fw, h, ci_bg);

 cr_chrome_arrow_up(col_x + fw / 2, top + fh / 2, arrow_sz, ci_fg);
 cr_chrome_arrow_down(col_x + fw / 2, bot - fh / 2, arrow_sz, ci_fg);

 track_top = top + fh;
 track_h = h - fh * 2;
 if (track_h <= 0)
  return;

 cr_chrome_track(bar_x, track_top, bar_w, track_h, ci_bg);

 thumb_h = track_h;
 thumb_y = track_top;
 if (f->b && f->b->mxlines > 1)
 {
  int visible = ey - ay - 1;
  int total = f->b->mxlines;
  int pos = f->b->b.y;
  if (total > visible && visible > 0)
  {
   thumb_h = (visible * track_h) / total;
   if (thumb_h < 10) thumb_h = 10;
   if (thumb_h > track_h) thumb_h = track_h;
   if (pos > total - visible) pos = total - visible;
   thumb_y = track_top + ((pos * (track_h - thumb_h))
             / (total - visible));
  }
 }
 cr_chrome_thumb(bar_x, thumb_y, bar_w, thumb_h, ci_fg);
}

void wpe_render_chrome(void)
{
#ifndef NO_XWINDOWS
 extern ECNT *WpeEditor;
 int w;

 if (!cr || !WpeEditor || wpe_chrome_suppress)
  return;

 for (w = 1; w <= WpeEditor->mxedt; w++)
 {
  FENSTER *f = WpeEditor->f[WpeEditor->edt[w]];
  int is_active = (WpeEditor->edt[w] ==
                   WpeEditor->edt[WpeEditor->curedt]);

  if (!DTMD_ISTEXT(f->dtmd))
   continue;

  cr_chrome_vscrollbar(f, is_active ? 4 : 0, is_active ? 7 : 8);
 }
#endif
}

int wpe_chrome_hit_vthumb(int col, int row)
{
#ifndef NO_XWINDOWS
 extern ECNT *WpeEditor;
 int w;
 int fh = WpeRender.font_height;

 if (!WpeEditor)
  return 0;

 for (w = 1; w <= WpeEditor->mxedt; w++)
 {
  FENSTER *f = WpeEditor->f[WpeEditor->edt[w]];
  int ay = f->a.y, ey = f->e.y, ex = f->e.x;
  int track_top, track_h, thumb_h, thumb_y;

  if (!DTMD_ISTEXT(f->dtmd) || col != ex)
   continue;
  if (row <= ay + 1 || row >= ey - 1)
   return 0;

  track_top = (ay + 2) * fh;
  track_h = (ey - ay - 3) * fh;
  if (track_h <= 0)
   return 0;

  thumb_h = track_h;
  thumb_y = track_top;
  if (f->b && f->b->mxlines > 1)
  {
   int visible = ey - ay - 1;
   int total = f->b->mxlines;
   int pos = f->b->b.y;
   if (total > visible && visible > 0)
   {
    thumb_h = (visible * track_h) / total;
    if (thumb_h < 10) thumb_h = 10;
    if (thumb_h > track_h) thumb_h = track_h;
    if (pos > total - visible) pos = total - visible;
    thumb_y = track_top + ((pos * (track_h - thumb_h))
              / (total - visible));
   }
  }

  { int row_px = row * fh;
    if (row_px >= thumb_y && row_px < thumb_y + thumb_h)
     return 1;
  }
 }
#endif
 return 0;
}

#endif /* HAVE_PANGO */
#endif /* HAVE_CAIRO */
