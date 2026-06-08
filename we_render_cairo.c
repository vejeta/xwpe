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
static FT_Face cr_ft_ftface;   /* owned by us: cairo references but never frees it */

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

/* Fast single-glyph path used only while a scrollbar drag is in flight, where
   cr_draw_text restricts callers to one printable ASCII byte.  By that
   contract only utf8[0] is meaningful; u8len is accepted to match the
   cr_draw_text_* signature but is deliberately unused. */
static void cr_draw_text_ft(int x, int y, const char *utf8, int u8len,
                            int fg_idx)
{
 cairo_glyph_t glyph;
 (void)u8len;
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

static void cr_blit(int sx, int sy, int dx, int dy, int w, int h)
{
#ifndef NO_XWINDOWS
 cairo_surface_flush(cr_surface);
 XCopyArea(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.backbuf,
   WpeXInfo.gc, sx, sy, w, h, dx, dy);
 cairo_surface_mark_dirty_rectangle(cr_surface, dx, dy, w, h);
#endif
}

static void cr_resize(int pixel_w, int pixel_h)
{
#ifndef NO_XWINDOWS
 static int prev_w = 0, prev_h = 0;
 Pixmap old_buf = WpeXInfo.backbuf;

 WpeXInfo.backbuf = XCreatePixmap(WpeXInfo.display, WpeXInfo.window,
   pixel_w, pixel_h,
   DefaultDepth(WpeXInfo.display, WpeXInfo.screen));

 XSetForeground(WpeXInfo.display, WpeXInfo.gc,
   BlackPixel(WpeXInfo.display, WpeXInfo.screen));
 XFillRectangle(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.gc,
   0, 0, pixel_w, pixel_h);

 if (old_buf && prev_w > 0 && prev_h > 0)
 {
  int cw = pixel_w < prev_w ? pixel_w : prev_w;
  int ch = pixel_h < prev_h ? pixel_h : prev_h;
  XCopyArea(WpeXInfo.display, old_buf, WpeXInfo.backbuf, WpeXInfo.gc,
    0, 0, cw, ch, 0, 0);
 }
 prev_w = pixel_w;
 prev_h = pixel_h;

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
 /* Tear the font stack down in dependency order: the scaled font references
    the cairo FT font face, which references the FreeType FT_Face that cairo
    does not own -- so we FT_Done it ourselves, last. */
 if (cr_scaled)
 {
  cairo_scaled_font_destroy(cr_scaled);
  cr_scaled = NULL;
 }
 if (cr_ft_face)
 {
  cairo_font_face_destroy(cr_ft_face);
  cr_ft_face = NULL;
 }
 if (cr_ft_ftface)
 {
  FT_Done_Face(cr_ft_ftface);
  cr_ft_ftface = NULL;
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

 snprintf(font_with_size, sizeof(font_with_size), "%s 10", sys_font);
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
 cr_ft_ftface = ft;   /* keep the FT_Face so cr_cleanup can FT_Done it */
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

static void cr_set_render_backend(void)
{
 WpeRender.draw_rect   = cr_draw_rect;
 WpeRender.draw_text   = cr_draw_text;
 WpeRender.draw_line   = cr_draw_line;
 WpeRender.clear_rect  = cr_clear_rect;
 WpeRender.draw_acs    = cr_draw_acs;
 WpeRender.flush       = cr_flush;
 WpeRender.flush_all   = cr_flush_all;
 WpeRender.blit        = cr_blit;
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

/* Thumb offset+length within a track, from the scroll position.  Shared by the
   vertical and horizontal paint and hit-test so a click always lands on the
   painted thumb.  When the content fits the view the thumb fills the track. */
static void cr_thumb_geometry(int track_start, int track_len,
                              int total, int visible, int pos,
                              int *thumb_start, int *thumb_len)
{
 int tlen = track_len, tstart = track_start;

 if (total > visible && visible > 0 && track_len > 0)
 {
  tlen = (visible * track_len) / total;
  if (tlen < 10) tlen = 10;
  if (tlen > track_len) tlen = track_len;
  if (pos > total - visible) pos = total - visible;
  if (pos < 0) pos = 0;
  tstart = track_start + ((pos * (track_len - tlen)) / (total - visible));
 }
 *thumb_start = tstart;
 *thumb_len = tlen;
}

/* The horizontal scrollbar thumb track, in pixels.  This MUST equal the track
   e_scroll_drag_h() maps in we_mouse.c (columns ax+20 .. ex-3), because the
   drag handler is the source of truth: in X11 a drag only starts once the
   hit-test confirms the press is on the thumb, and the drag then maps the
   pointer through this same span.  Paint and hit-test both call this so the
   painted thumb, the clickable area and the drag mapping cannot drift apart. */
static void cr_hscroll_track(FENSTER *f, int *track_left_px, int *track_w_px)
{
 int fw = WpeRender.font_width;
 *track_left_px = (f->a.x + 20) * fw;
 *track_w_px = (f->e.x - 3) * fw - *track_left_px;
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
  cr_thumb_geometry(track_top, track_h, f->b->mxlines, ey - ay - 1,
                    f->s->c.y, &thumb_y, &thumb_h);
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

 /* Arrows sit at the two ends of the bar (left..right); the draggable thumb
    lives in the narrower track between them, whose span is owned by
    cr_hscroll_track so paint/hit/drag agree. */
 cr_chrome_arrow_left(left + fw / 2, row_y + fh / 2, arrow_sz, ci_fg);
 cr_chrome_arrow_right(right - fw / 2, row_y + fh / 2, arrow_sz, ci_fg);

 cr_hscroll_track(f, &track_left, &track_w);
 if (track_w <= 0)
  return;

 cr_chrome_track(track_left, bar_y, track_w, bar_h, ci_bg);

 thumb_w = track_w;
 thumb_x = track_left;
 if (f->b && f->b->mx.x > 1)
  cr_thumb_geometry(track_left, track_w, f->b->mx.x, ex - ax - 1,
                    f->s->c.x, &thumb_x, &thumb_w);
 cr_chrome_thumb(thumb_x, bar_y, thumb_w, bar_h, ci_fg);
}

#ifndef NO_XWINDOWS
/* A window covered down to just its border column/row would show a lone
   "floating" scrollbar -- its interior hidden by the windows on top, the bar
   peeking out only because it is one column/row wider or offset.  These return
   true only if some interior cell BESIDE the bar is still visible (not covered
   by a window stacked above f).  When false, skip the fluid scrollbar overlay;
   the plain border line drawn by the cell renderer still shows the exposed
   edge -- the authentic Borland look, no stray scrollbar. */
static int e_chrome_col_content_visible(ECNT *ed, FENSTER *f, int col, int w)
{
 int row, w2;
 for (row = f->a.y + 1; row <= f->e.y - 1; row++)
 {
  int covered = 0;
  for (w2 = w + 1; w2 <= ed->mxedt; w2++)
  {
   FENSTER *g = ed->f[w2];
   if (col >= g->a.x && col <= g->e.x && row >= g->a.y && row <= g->e.y)
   {  covered = 1;  break;  }
  }
  if (!covered)
   return 1;
 }
 return 0;
}

static int e_chrome_row_content_visible(ECNT *ed, FENSTER *f, int row, int w)
{
 int col, w2;
 for (col = f->a.x + 1; col <= f->e.x - 1; col++)
 {
  int covered = 0;
  for (w2 = w + 1; w2 <= ed->mxedt; w2++)
  {
   FENSTER *g = ed->f[w2];
   if (col >= g->a.x && col <= g->e.x && row >= g->a.y && row <= g->e.y)
   {  covered = 1;  break;  }
  }
  if (!covered)
   return 1;
 }
 return 0;
}

/* cr_window_pixel_rect - A window's full extent in device pixels.  Windows are
   stored in character cells (a.x..e.x, a.y..e.y); the Cairo chrome works in
   pixels.  Used wherever a window must be clipped or subtracted as a region. */
static void cr_window_pixel_rect(FENSTER *f, cairo_rectangle_int_t *r)
{
 int fw = WpeRender.font_width, fh = WpeRender.font_height;
 r->x = fw * f->a.x;
 r->y = fh * f->a.y;
 r->width = fw * (f->e.x - f->a.x + 1);
 r->height = fh * (f->e.y - f->a.y + 1);
}

/* e_chrome_visible_region - The on-screen region of the window at z-level w that
   is NOT hidden by any window stacked above it: its own rectangle MINUS the
   union of every higher window (f[w+1..mxedt]).  cairo_region_t does exact
   integer-rectangle set algebra, so this stays correct even when the covering
   windows overlap EACH OTHER (a cascade) -- where an even-odd path clip would
   miscount the triple-overlap band and let a covered scrollbar bleed through.
   Caller owns the returned region (cairo_region_destroy). */
static cairo_region_t *e_chrome_visible_region(ECNT *ed, int w)
{
 cairo_rectangle_int_t r;
 cairo_region_t *vis;
 int w2;

 cr_window_pixel_rect(ed->f[w], &r);
 vis = cairo_region_create_rectangle(&r);
 for (w2 = w + 1; w2 <= ed->mxedt; w2++)
 {
  cairo_rectangle_int_t gr;
  cr_window_pixel_rect(ed->f[w2], &gr);
  cairo_region_subtract_rectangle(vis, &gr);
 }
 return vis;
}

/* cr_clip_to_region - Restrict subsequent Cairo drawing to a region.  A region
   is a set of DISJOINT rectangles, so adding them all to the path and filling
   with the default winding rule is an exact clip; an empty region (window fully
   covered) clips to nothing, which is exactly what such a window wants. */
static void cr_clip_to_region(cairo_region_t *reg)
{
 int n = cairo_region_num_rectangles(reg), i;

 for (i = 0; i < n; i++)
 {
  cairo_rectangle_int_t r;
  cairo_region_get_rectangle(reg, i, &r);
  cairo_rectangle(cr, r.x, r.y, r.width, r.height);
 }
 cairo_clip(cr);
}

/* Scrollbar colour indices into the editor palette: a focused window draws its
   bars in the bright "active" pair, a background window in the dimmed pair, so
   the user can tell at a glance which window has focus (Borland convention). */
#define CHROME_BAR_TRACK_ACTIVE    4   /* track groove, focused window  */
#define CHROME_BAR_TRACK_INACTIVE  0   /* track groove, background window */
#define CHROME_BAR_THUMB_ACTIVE    7   /* thumb + arrow markers, focused */
#define CHROME_BAR_THUMB_INACTIVE  8   /* thumb + arrow markers, background */

/* e_chrome_paint_window_scrollbars - Paint window w's fluid vertical and
   horizontal scrollbars, in focused or background colours.  Each bar is drawn
   only when some interior cell BESIDE it is still exposed, so a window covered
   down to just its border shows the plain border line, not a lone floating bar.
   The caller has already clipped the context to the window's visible region. */
static void e_chrome_paint_window_scrollbars(ECNT *ed, FENSTER *f, int w,
                                             int is_active)
{
 int track = is_active ? CHROME_BAR_TRACK_ACTIVE : CHROME_BAR_TRACK_INACTIVE;
 int thumb = is_active ? CHROME_BAR_THUMB_ACTIVE : CHROME_BAR_THUMB_INACTIVE;

 if (e_chrome_col_content_visible(ed, f, f->e.x - 1, w))
  cr_chrome_vscrollbar(f, track, thumb);
 if (e_chrome_row_content_visible(ed, f, f->e.y - 1, w))
  cr_chrome_hscrollbar(f, track, thumb);
}
#endif

void wpe_render_chrome(void)
{
#ifndef NO_XWINDOWS
 extern ECNT *WpeEditor;
 int w;

 if (!cr || !WpeEditor || wpe_chrome_suppress)
  return;

 for (w = 1; w <= WpeEditor->mxedt; w++)
 {
  /* f[] is indexed by Z-LEVEL (f[1]=bottom .. f[mxedt]=top), the same
     convention the cell compositor uses (e_repaint_desk_nopic).  The active
     window is the topmost, f[mxedt].  (An earlier version indexed f[edt[w]]
     -- by window NUMBER -- which only matches when edt is the identity, i.e.
     windows never reordered; after a click/raise it read the WRONG window's
     geometry and the scrollbars bled.) */
  FENSTER *f = WpeEditor->f[w];
  int is_active = (w == WpeEditor->mxedt);

  if (!DTMD_ISTEXT(f->dtmd))
   continue;

  /* Clip this window's fluid scrollbars to the part of it still visible -- its
     rectangle minus the union of the windows stacked above it.  Drawn here
     after the cells and z-order-blind, an unclipped covered scrollbar paints
     over the window covering it (the zoom/drag/3-window bleed; X11 only). */
  cairo_save(cr);
  {
   cairo_region_t *vis = e_chrome_visible_region(WpeEditor, w);
   cr_clip_to_region(vis);
   cairo_region_destroy(vis);
  }

  e_chrome_paint_window_scrollbars(WpeEditor, f, w, is_active);

  cairo_restore(cr);
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

  /* Same track as cr_chrome_vscrollbar's paint: top (ay+1)*fh + one arrow
     row, height minus the two arrow rows. */
  track_top = (ay + 2) * fh;
  track_h = (ey - ay - 3) * fh;
  if (track_h <= 0)
   return 0;

  /* Only a draggable thumb counts as a hit.  When the content fits the view
     (mxlines <= visible) cr_thumb_geometry reports a full-track thumb, but
     there is nothing to drag -- report no hit so the caller treats the press
     as click-to-position instead. */
  thumb_h = track_h;
  thumb_y = track_top;
  if (f->b && f->b->mxlines > ey - ay - 1)
  {
   int row_px = row * fh;
   cr_thumb_geometry(track_top, track_h, f->b->mxlines, ey - ay - 1,
                     f->s->c.y, &thumb_y, &thumb_h);
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
  int ex = f->e.x, ey = f->e.y, ax = f->a.x;
  int track_left, track_w;

  if (!DTMD_ISTEXT(f->dtmd) || row != ey)
   continue;

  /* Same track and thumb math as the paint (cr_hscroll_track +
     cr_thumb_geometry), so a press only registers on the visible thumb.
     Require the content to overflow the view (mx.x > visible): when it fits,
     there is no draggable thumb and the press must fall through to
     click-to-position, exactly as the drag handler (e_scroll_drag_h) expects. */
  cr_hscroll_track(f, &track_left, &track_w);
  if (f->b && f->b->mx.x > ex - ax - 1 && track_w > 0)
  {
   int thumb_w, thumb_x, col_px = col * fw;
   cr_thumb_geometry(track_left, track_w, f->b->mx.x, ex - ax - 1,
                     f->s->c.x, &thumb_x, &thumb_w);
   if (col_px >= thumb_x && col_px < thumb_x + thumb_w)
    return 1;
  }
 }
#endif
 return 0;
}

#endif /* HAVE_PANGO */
#endif /* HAVE_CAIRO */
