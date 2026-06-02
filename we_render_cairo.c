/* we_render_cairo.c -- Cairo + Pango rendering backend for X11    */
/* Copyright (C) 2026 Juan Manuel Mendez Rey                      */
/* This is free software; see the file COPYING.                    */

#ifdef HAVE_CAIRO
#ifdef HAVE_PANGO

#include "we_render.h"
#include "edit.h"
#include <cairo.h>
#include <cairo-xlib.h>
#include <cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <pango/pangocairo.h>
#include <X11/Xlib.h>
#include <string.h>

#include <fontconfig/fontconfig.h>

#ifndef NO_XWINDOWS
#include "WeXterm.h"
#include <X11/Xft/Xft.h>
#endif

static cairo_surface_t *cr_surface;
static cairo_t *cr;
static PangoLayout *pg_layout;
static PangoFontDescription *pg_font;
static cairo_font_face_t *cr_ft_face;
static cairo_scaled_font_t *cr_scaled;

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

static void cr_draw_text_ft(int x, int y, const char *utf8, int u8len,
                            int fg_idx)
{
 cairo_glyph_t glyph;
 glyph.index = FT_Get_Char_Index(
   cairo_ft_scaled_font_lock_face(cr_scaled),
   (unsigned char)utf8[0]);
 cairo_ft_scaled_font_unlock_face(cr_scaled);
 glyph.x = x;
 glyph.y = y + WpeRender.font_ascent;
 cairo_set_scaled_font(cr, cr_scaled);
 cairo_set_source_rgb(cr, cairo_colors[fg_idx][0],
   cairo_colors[fg_idx][1], cairo_colors[fg_idx][2]);
 cairo_show_glyphs(cr, &glyph, 1);
}

static void cr_draw_text_pango(int x, int y, const char *utf8, int u8len,
                               int fg_idx)
{
 cairo_set_source_rgb(cr, cairo_colors[fg_idx][0],
   cairo_colors[fg_idx][1], cairo_colors[fg_idx][2]);
 cairo_move_to(cr, x, y);
 pango_layout_set_text(pg_layout, utf8, u8len);
 pango_cairo_show_layout(cr, pg_layout);
}

static void cr_draw_text(int x, int y, const char *utf8, int u8len,
                         int cell_width, int fg_idx, int bg_idx)
{
 cr_draw_rect(x, y, WpeRender.font_width * cell_width,
   WpeRender.font_height, bg_idx);

 if (wpe_scroll_dragging && cr_scaled
     && u8len == 1 && (unsigned char)utf8[0] >= 32
     && (unsigned char)utf8[0] < 128)
  cr_draw_text_ft(x, y, utf8, u8len, fg_idx);
 else
  cr_draw_text_pango(x, y, utf8, u8len, fg_idx);
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

static void cr_init_surface(int pixel_w, int pixel_h)
{
 cr_surface = cairo_xlib_surface_create(WpeXInfo.display,
   WpeXInfo.backbuf,
   DefaultVisual(WpeXInfo.display, WpeXInfo.screen),
   pixel_w, pixel_h);
 cr = cairo_create(cr_surface);
 cairo_set_antialias(cr, CAIRO_ANTIALIAS_GRAY);
}

static const char *cr_get_system_monospace_font(void)
{
 static char fontname[128];
 FILE *p = popen(
   "gsettings get org.gnome.desktop.interface monospace-font-name "
   "2>/dev/null", "r");
 if (p)
 {
  if (fgets(fontname, sizeof(fontname), p))
  {
   char *s = fontname;
   char *e;
   while (*s == '\'' || *s == ' ') s++;
   e = s + strlen(s) - 1;
   while (e > s && (*e == '\'' || *e == '\n' || *e == ' ')) *e-- = 0;
   /* Strip trailing size number */
   while (e > s && (*e >= '0' && *e <= '9')) *e-- = 0;
   while (e > s && *e == ' ') *e-- = 0;
   pclose(p);
   if (*s) return s;
  }
  else
   pclose(p);
 }
 return "monospace";
}

static void cr_init_pango_font(void)
{
 PangoRectangle logical;
 PangoContext *pg_ctx;
 PangoFontMetrics *metrics;
 const char *sys_font = cr_get_system_monospace_font();
 char font_with_size[140];

 pg_layout = pango_cairo_create_layout(cr);

 sprintf(font_with_size, "%s 10", sys_font);
 pg_font = pango_font_description_from_string(font_with_size);
 pango_layout_set_font_description(pg_layout, pg_font);

 pango_layout_set_text(pg_layout, "M", 1);
 pango_layout_get_pixel_extents(pg_layout, NULL, &logical);

 pg_ctx = pango_layout_get_context(pg_layout);
 metrics = pango_context_get_metrics(pg_ctx, pg_font, NULL);

 WpeRender.font_width  = logical.width;
 WpeRender.font_height = logical.height;
 WpeRender.font_ascent = pango_font_metrics_get_ascent(metrics)
                         / PANGO_SCALE;
 WpeXInfo.font_width   = logical.width;
 WpeXInfo.font_height  = logical.height;

 pango_font_metrics_unref(metrics);
}

static void cr_init_ft_font(void)
{
 FT_Face ft;
 cairo_matrix_t font_mat, ctm;
 cairo_font_options_t *opts;
 cairo_text_extents_t ext;
 double sz;
 const char *sys_font = cr_get_system_monospace_font();
 FcPattern *pat, *match;
 FcResult res;
 FcChar8 *path;

 pat = FcNameParse((const FcChar8 *)sys_font);
 FcPatternAddBool(pat, FC_SCALABLE, 1);
 FcConfigSubstitute(NULL, pat, FcMatchPattern);
 FcDefaultSubstitute(pat);
 match = FcFontMatch(NULL, pat, &res);
 FcPatternDestroy(pat);
 if (!match) return;
 if (FcPatternGetString(match, FC_FILE, 0, &path) != FcResultMatch)
 { FcPatternDestroy(match); return; }

 { static FT_Library ftlib;
   if (!ftlib) FT_Init_FreeType(&ftlib);
   if (FT_New_Face(ftlib, (const char *)path, 0, &ft))
   { FcPatternDestroy(match); return; }
 }
 FcPatternDestroy(match);

 cr_ft_face = cairo_ft_font_face_create_for_ft_face(ft, 0);
 cairo_matrix_init_identity(&ctm);
 opts = cairo_font_options_create();
 cairo_surface_get_font_options(cr_surface, opts);

 for (sz = WpeXInfo.font_height; sz > 4; sz -= 0.5)
 {
  cairo_matrix_init_scale(&font_mat, sz, sz);
  if (cr_scaled)
   cairo_scaled_font_destroy(cr_scaled);
  cr_scaled = cairo_scaled_font_create(cr_ft_face,
    &font_mat, &ctm, opts);
  cairo_scaled_font_text_extents(cr_scaled, "M", &ext);
  if (ext.x_advance <= WpeXInfo.font_width
      && ext.height <= WpeXInfo.font_height)
   break;
 }

 cairo_font_options_destroy(opts);
}

static void cr_dump_font_metrics(void)
{
 FILE *dbg = fopen(
   "/home/mendezr/development/debian/xwpe-dev/tmp/font-metrics-debug.txt",
   "w");
 if (!dbg) return;

 { PangoRectangle logical;
   pango_layout_set_text(pg_layout, "M", 1);
   pango_layout_get_pixel_extents(pg_layout, NULL, &logical);
   fprintf(dbg, "=== Font Metrics (fitted) ===\n");
   fprintf(dbg, "Xft cell:  w=%d h=%d asc=%d desc=%d\n",
     WpeXInfo.font_width, WpeXInfo.font_height,
     WpeXInfo.xftfont->ascent, WpeXInfo.xftfont->descent);
   fprintf(dbg, "Pango 'M': w=%d h=%d\n",
     logical.width, logical.height);
   fprintf(dbg, "Delta:     w=%+d h=%+d\n",
     logical.width - WpeXInfo.font_width,
     logical.height - WpeXInfo.font_height);
 }
 if (cr_scaled)
 {
  cairo_text_extents_t ext;
  cairo_scaled_font_text_extents(cr_scaled, "M", &ext);
  fprintf(dbg, "cairo_ft:  adv=%.1f h=%.1f\n",
    ext.x_advance, ext.height);
 }
 fclose(dbg);
}

static void cr_set_render_backend(void)
{
 WpeRender.draw_rect   = cr_draw_rect;
 WpeRender.draw_text   = cr_draw_text;
 WpeRender.draw_line   = cr_draw_line;
 WpeRender.clear_rect  = cr_clear_rect;
 WpeRender.draw_acs    = cr_draw_acs;
 WpeRender.flush       = cr_flush;
 WpeRender.flush_all   = cr_flush_all;
 WpeRender.resize      = cr_resize;
 WpeRender.cleanup     = cr_cleanup;
}

int wpe_render_cairo_init(void)
{
#ifndef NO_XWINDOWS
 extern int MAXSLNS, MAXSCOL;
 PangoContext *pg_ctx;
 PangoFontMetrics *metrics;

 cairo_init_colors();
 cr_init_surface(WpeXInfo.font_width * MAXSCOL,
   WpeXInfo.font_height * MAXSLNS);
 cr_init_pango_font();
 cr_init_ft_font();
 cr_dump_font_metrics();
 cr_set_render_backend();
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
  int pos = f->s->c.y;
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

static void cr_chrome_arrow_left(int cx, int cy, int sz, int ci)
{
 cairo_set_source_rgb(cr, cairo_colors[ci][0],
   cairo_colors[ci][1], cairo_colors[ci][2]);
 cairo_move_to(cr, cx - sz, cy);
 cairo_line_to(cr, cx + sz / 2, cy - sz);
 cairo_line_to(cr, cx + sz / 2, cy + sz);
 cairo_close_path(cr);
 cairo_fill(cr);
}

static void cr_chrome_arrow_right(int cx, int cy, int sz, int ci)
{
 cairo_set_source_rgb(cr, cairo_colors[ci][0],
   cairo_colors[ci][1], cairo_colors[ci][2]);
 cairo_move_to(cr, cx + sz, cy);
 cairo_line_to(cr, cx - sz / 2, cy - sz);
 cairo_line_to(cr, cx - sz / 2, cy + sz);
 cairo_close_path(cr);
 cairo_fill(cr);
}

static void cr_chrome_hscrollbar(FENSTER *f, int ci_bg, int ci_fg)
{
 int fw = WpeRender.font_width;
 int fh = WpeRender.font_height;
 int ax = f->a.x, ex = f->e.x, ey = f->e.y;
 int hsb_start = ax + 19;
 int hsb_end = ex - 2;
 int row_y = ey * fh;
 int left = hsb_start * fw;
 int right = (hsb_end + 1) * fw;
 int w = right - left;
 int bar_h = fh > 12 ? 5 : 4;
 int bar_y = row_y + (fh - bar_h) / 2;
 int arrow_sz = fh > 12 ? 5 : 3;
 int track_left, track_w, thumb_w, thumb_x;

 if (w < fw * 5)
  return;

 cr_draw_rect(left, row_y, w, fh, ci_bg);

 cr_chrome_arrow_left(left + fw / 2, row_y + fh / 2, arrow_sz, ci_fg);
 cr_chrome_arrow_right(right - fw / 2, row_y + fh / 2, arrow_sz, ci_fg);

 track_left = left + fw;
 track_w = w - fw * 2;
 if (track_w <= 0)
  return;

 cr_chrome_track(track_left, bar_y, track_w, bar_h, ci_bg);

 thumb_w = track_w;
 thumb_x = track_left;
 if (f->b && f->b->mx.x > 1)
 {
  int visible = ex - ax - 1;
  int total = f->b->mx.x;
  int pos = f->s->c.x;
  if (total > visible && visible > 0)
  {
   thumb_w = (visible * track_w) / total;
   if (thumb_w < 10) thumb_w = 10;
   if (thumb_w > track_w) thumb_w = track_w;
   if (pos > total - visible) pos = total - visible;
   thumb_x = track_left + ((pos * (track_w - thumb_w))
             / (total - visible));
  }
 }
 cr_chrome_thumb(thumb_x, bar_y, thumb_w, bar_h, ci_fg);
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
  cr_chrome_hscrollbar(f, is_active ? 4 : 0, is_active ? 7 : 8);
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
   int pos = f->s->c.y;
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

int wpe_chrome_hit_hthumb(int col, int row)
{
#ifndef NO_XWINDOWS
 extern ECNT *WpeEditor;
 int w;
 int fw = WpeRender.font_width;

 if (!WpeEditor)
  return 0;

 for (w = 1; w <= WpeEditor->mxedt; w++)
 {
  FENSTER *f = WpeEditor->f[WpeEditor->edt[w]];
  int ax = f->a.x, ex = f->e.x, ey = f->e.y;
  int hsb_start = ax + 20;
  int hsb_end = ex - 3;

  if (!DTMD_ISTEXT(f->dtmd) || row != ey)
   continue;
  if (col <= hsb_start || col >= hsb_end)
   return 0;

  if (f->b && f->b->mx.x > 1)
  {
   int visible = ex - ax - 1;
   int total = f->b->mx.x;
   int pos = f->s->c.x;
   int track_w = (hsb_end - hsb_start) * fw;
   int thumb_w, thumb_x;

   if (total > visible && visible > 0)
   {
    thumb_w = (visible * track_w) / total;
    if (thumb_w < 10) thumb_w = 10;
    if (pos > total - visible) pos = total - visible;
    thumb_x = hsb_start * fw + ((pos * (track_w - thumb_w))
              / (total - visible));
    { int col_px = col * fw;
      if (col_px >= thumb_x && col_px < thumb_x + thumb_w)
       return 1;
    }
   }
  }
 }
#endif
 return 0;
}

#endif /* HAVE_PANGO */
#endif /* HAVE_CAIRO */
