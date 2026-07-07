/* we_render_wayland.c -- Cairo + Pango rendering backend for native Wayland.
 *
 * The X11 backend renders the SCREENCELL grid with Cairo onto a server-side
 * Pixmap (we_render_cairo.c).  Wayland has no server-side drawables, so this
 * backend points the very same Cairo drawing model at a CPU image surface laid
 * directly over the wl_shm frame buffer (WpeWl.pixels): the glyph/rect/line
 * primitives are identical in spirit; only the surface and the "present" step
 * differ (wl_surface attach/commit instead of XCopyArea).
 *
 * It is intentionally self-contained rather than sharing we_render_cairo.c's
 * statics: that file is X11-coupled (cairo-xlib, XQueryColor, XCopyArea) and is
 * the long-verified desktop path, so the Wayland bring-up keeps it untouched.
 * Once both backends are covered by tests, the common Cairo core (primitives +
 * Pango font + fluid-scrollbar chrome) is a clean follow-up extraction.
 */

#ifdef HAVE_WAYLAND

#include <stdlib.h>
#include <string.h>
#include <cairo.h>
#include <pango/pangocairo.h>

#include "we_render.h"
#include "we_wayland.h"
#include "we_lsp.h"        /* e_lsp_sem_slot_rgb, LSP_SEM_TC_MAX */

static cairo_surface_t      *wcr_surface;   /* image surface over WpeWl.pixels */
static cairo_t              *wcr;
static PangoLayout          *pg_layout;
static PangoFontDescription *pg_font;

/* 16 base colours + up to LSP_SEM_TC_MAX semantic-token truecolor slots at
   indices 16+slot (a foreground index >= 16 paints a slot's exact RGB), in
   Cairo's 0..1 range -- the same layout as we_render_cairo.c's cairo_colors. */
static double wl_colors[16 + LSP_SEM_TC_MAX][3];

/* The 16 xwpe palette colours as RGB.  These are the standard X11 rgb.txt
   values for the names the X11 backend allocates (WpeXColorNames in WeXterm.c);
   listing them directly keeps the native backend free of any X11 dependency
   for colour resolution (Wayland has no colormap or X resource database). */
static const unsigned char wl_palette_rgb[16][3] = {
 {   0,   0,   0 },  /*  0 Black            */
 { 205,   0,   0 },  /*  1 Red3             */
 {  34, 139,  34 },  /*  2 Forest Green     */
 { 165,  42,  42 },  /*  3 Brown            */
 {  72,  61, 139 },  /*  4 Dark Slate Blue  */
 { 208,  32, 144 },  /*  5 Violet Red       */
 {   0, 197, 205 },  /*  6 Turquoise3       */
 { 211, 211, 211 },  /*  7 Light Gray       */
 {  47,  79,  79 },  /*  8 Dark Slate Grey  */
 { 255,   0,   0 },  /*  9 Red1             */
 {   0, 255,   0 },  /* 10 Green            */
 { 255, 255,   0 },  /* 11 Yellow           */
 {   0,   0, 255 },  /* 12 Blue             */
 { 238, 130, 238 },  /* 13 Violet           */
 {   0, 245, 255 },  /* 14 Turquoise1       */
 { 255, 255, 255 }   /* 15 White            */
};

static void wl_render_init_colors(void)
{
 int i, s, r, g, b;

 for (i = 0; i < 16; i++)
 {
  wl_colors[i][0] = wl_palette_rgb[i][0] / 255.0;
  wl_colors[i][1] = wl_palette_rgb[i][1] / 255.0;
  wl_colors[i][2] = wl_palette_rgb[i][2] / 255.0;
 }
 for (s = 0; s < LSP_SEM_TC_MAX; s++)
 {
  if (!e_lsp_sem_slot_rgb(s, &r, &g, &b))
   break;
  wl_colors[16 + s][0] = r / 255.0;
  wl_colors[16 + s][1] = g / 255.0;
  wl_colors[16 + s][2] = b / 255.0;
 }
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Drawing primitives (operate on the image surface / wl_shm memory)
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void wr_set_color(int idx)
{
 cairo_set_source_rgb(wcr, wl_colors[idx][0], wl_colors[idx][1], wl_colors[idx][2]);
}

static void wr_draw_rect(int x, int y, int w, int h, int color_idx)
{
 wr_set_color(color_idx);
 cairo_rectangle(wcr, x, y, w, h);
 cairo_fill(wcr);
}

static void wr_clear_rect(int x, int y, int w, int h, int color_idx)
{
 wr_draw_rect(x, y, w, h, color_idx);
}

static void wr_draw_text(int x, int y, const char *utf8, int u8len,
                         int cell_width, int fg_idx, int bg_idx)
{
 wr_draw_rect(x, y, WpeRender.font_width * cell_width, WpeRender.font_height, bg_idx);
 wr_set_color(fg_idx);
 cairo_move_to(wcr, x, y);
 pango_layout_set_text(pg_layout, utf8, u8len);
 pango_cairo_show_layout(wcr, pg_layout);
}

static void wr_draw_line(int x1, int y1, int x2, int y2, int color_idx, int width)
{
 wr_set_color(color_idx);
 cairo_set_line_width(wcr, width);
 cairo_move_to(wcr, x1 + 0.5, y1 + 0.5);
 cairo_line_to(wcr, x2 + 0.5, y2 + 0.5);
 cairo_stroke(wcr);
}

/* Box-drawing / scrollbar glyphs (sc 1..12), painted as filled rectangles --
   the same geometry as we_render_cairo.c's cr_draw_acs so the two backends
   draw an identical frame. */
static void wr_draw_acs(int sc, int px, int py, int fg_idx, int bg_idx)
{
 int fw = WpeRender.font_width;
 int fh = WpeRender.font_height;
 int lw = fw > 8 ? 2 : 1;
 int mx = px + fw / 2;
 int my = py + fh / 2;

 wr_draw_rect(px, py, fw, fh, bg_idx);
 wr_set_color(fg_idx);

 switch (sc)
 {
 case 1: /* upper-left corner */
  cairo_rectangle(wcr, mx, my, fw - fw/2, lw); cairo_fill(wcr);
  cairo_rectangle(wcr, mx, my, lw, fh - fh/2); cairo_fill(wcr);
  break;
 case 2: /* upper-right corner */
  cairo_rectangle(wcr, px, my, fw/2 + lw, lw); cairo_fill(wcr);
  cairo_rectangle(wcr, mx, my, lw, fh - fh/2); cairo_fill(wcr);
  break;
 case 3: /* lower-left corner */
  cairo_rectangle(wcr, mx, my, fw - fw/2, lw); cairo_fill(wcr);
  cairo_rectangle(wcr, mx, py, lw, fh/2 + lw); cairo_fill(wcr);
  break;
 case 4: /* lower-right corner */
  cairo_rectangle(wcr, px, my, fw/2 + lw, lw); cairo_fill(wcr);
  cairo_rectangle(wcr, mx, py, lw, fh/2 + lw); cairo_fill(wcr);
  break;
 case 5: /* horizontal line */
  cairo_rectangle(wcr, px, my, fw, lw); cairo_fill(wcr);
  break;
 case 6: case 8: case 9: /* vertical line */
  cairo_rectangle(wcr, mx, py, lw, fh); cairo_fill(wcr);
  break;
 case 7: case 10: /* scrollbar track (stipple) */
  { int tx, ty;
    for (ty = py; ty < py + fh; ty += 2)
     for (tx = px; tx < px + fw; tx += 2)
      cairo_rectangle(wcr, tx, ty, 1, 1);
    cairo_fill(wcr);
  }
  break;
 case 11: /* scrollbar thumb */
  { int m = fw > 8 ? 2 : 1;
    cairo_rectangle(wcr, px + m, py + m, fw - 2*m, fh - 2*m);
    cairo_fill(wcr);
  }
  break;
 }
}

/* Read-only padlock, drawn as a vector icon that fills cw cells -- a filled
   body rectangle with an inverted-U shackle of three thin bars above it, plus a
   background keyhole notch.  Rectangles only, like wr_draw_acs, so it scales to
   any cell size and never depends on a colour-emoji font (which renders at its
   own bitmap size and would show clipped). */
static void wr_draw_lock(int px, int py, int cw, int fg_idx, int bg_idx)
{
 int cell_w = WpeRender.font_width * cw;
 int cell_h = WpeRender.font_height;
 int body_w = cell_w * 6 / 10;
 int body_h = cell_h * 42 / 100;
 int body_x = px + (cell_w - body_w) / 2;
 int body_y = py + cell_h - body_h - cell_h / 10;
 int shk_w  = body_w * 6 / 10;
 int shk_x  = px + (cell_w - shk_w) / 2;
 int shk_y  = py + cell_h * 12 / 100;
 int shk_h  = body_y - shk_y;
 int t      = cell_w > 12 ? 2 : 1;

 wr_draw_rect(px, py, cell_w, cell_h, bg_idx);
 wr_set_color(fg_idx);
 cairo_rectangle(wcr, body_x, body_y, body_w, body_h);            /* body        */
 cairo_rectangle(wcr, shk_x, shk_y, shk_w, t);                    /* shackle top */
 cairo_rectangle(wcr, shk_x, shk_y, t, shk_h);                    /* shackle left*/
 cairo_rectangle(wcr, shk_x + shk_w - t, shk_y, t, shk_h);        /* shackle rght*/
 cairo_fill(wcr);
 wr_set_color(bg_idx);
 cairo_rectangle(wcr, px + cell_w / 2 - t, body_y + body_h / 4,   /* keyhole     */
                 2 * t, body_h / 2);
 cairo_fill(wcr);
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Present / blit / resize
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* The image surface writes straight into the displayed wl_shm buffer, so a
   "present" is: make sure Cairo has flushed its writes, then tell the
   compositor which rectangle changed and commit.  (Proper buffer-release /
   double-buffering against tearing is added with the frame-callback work.) */
static void wr_present(int x, int y, int w, int h)
{
 if (!WpeWl.surface || !WpeWl.buffer)
  return;
 cairo_surface_flush(wcr_surface);
 wl_surface_attach(WpeWl.surface, WpeWl.buffer, 0, 0);
 if (WpeWl.compositor_version >= 4)
  wl_surface_damage_buffer(WpeWl.surface, x, y, w, h);
 else
  wl_surface_damage(WpeWl.surface, x, y, w, h);
 wl_surface_commit(WpeWl.surface);
 if (WpeWl.display)
  wl_display_flush(WpeWl.display);
}

static void wr_flush(int x, int y, int w, int h) { wr_present(x, y, w, h); }
static void wr_flush_all(void) { wr_present(0, 0, WpeWl.width, WpeWl.height); }

/* Copy a pixel rectangle within the single buffer (used by smooth scroll).
   Row-wise memmove handles vertical overlap in both directions. */
static void wr_blit(int sx, int sy, int dx, int dy, int w, int h)
{
 int row, bytes = w * 4;

 if (!WpeWl.pixels || w <= 0 || h <= 0)
  return;
 cairo_surface_flush(wcr_surface);
 if (dy <= sy)
  for (row = 0; row < h; row++)
   memmove(WpeWl.pixels + (size_t)(dy + row) * WpeWl.width + dx,
           WpeWl.pixels + (size_t)(sy + row) * WpeWl.width + sx, bytes);
 else
  for (row = h - 1; row >= 0; row--)
   memmove(WpeWl.pixels + (size_t)(dy + row) * WpeWl.width + dx,
           WpeWl.pixels + (size_t)(sy + row) * WpeWl.width + sx, bytes);
 cairo_surface_mark_dirty_rectangle(wcr_surface, dx, dy, w, h);
}

/* Re-point the Cairo image surface at the (already reallocated) wl_shm buffer
   after a resize.  we_wayland.c reallocates WpeWl.pixels/stride first, then
   calls this. */
static void wr_resize(int pixel_w, int pixel_h)
{
 if (wcr)         cairo_destroy(wcr);
 if (wcr_surface) cairo_surface_destroy(wcr_surface);
 wcr_surface = cairo_image_surface_create_for_data(
   (unsigned char *)WpeWl.pixels, CAIRO_FORMAT_RGB24,
   pixel_w, pixel_h, WpeWl.stride);
 wcr = cairo_create(wcr_surface);
 cairo_set_antialias(wcr, CAIRO_ANTIALIAS_GRAY);
}

static void wr_cleanup(void)
{
 if (pg_font)    { pango_font_description_free(pg_font); pg_font = NULL; }
 if (pg_layout)  { g_object_unref(pg_layout); pg_layout = NULL; }
 if (wcr)        { cairo_destroy(wcr); wcr = NULL; }
 if (wcr_surface){ cairo_surface_destroy(wcr_surface); wcr_surface = NULL; }
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Font + init
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* Editor font point size: an explicit XWPE_FONT_SIZE wins (works headless and
   over SSH); otherwise a sane default.  Clamped so a typo cannot make a 1px or
   4000px grid. */
static int wl_font_point_size(void)
{
 const char *env = getenv("XWPE_FONT_SIZE");
 int pt = (env && *env) ? atoi(env) : 0;
 if (pt < 4 || pt > 96)
  pt = 11;
 return pt;
}

static void wl_render_init_font(void)
{
 PangoRectangle logical;
 PangoContext *ctx;
 PangoFontMetrics *metrics;
 char font_with_size[64];

 pg_layout = pango_cairo_create_layout(wcr);
 snprintf(font_with_size, sizeof font_with_size, "monospace %d", wl_font_point_size());
 pg_font = pango_font_description_from_string(font_with_size);
 pango_layout_set_font_description(pg_layout, pg_font);

 pango_layout_set_text(pg_layout, "M", 1);
 pango_layout_get_pixel_extents(pg_layout, NULL, &logical);

 ctx = pango_layout_get_context(pg_layout);
 metrics = pango_context_get_metrics(ctx, pg_font, NULL);

 WpeRender.font_width  = logical.width;
 WpeRender.font_height = logical.height;
 WpeRender.font_ascent = pango_font_metrics_get_ascent(metrics) / PANGO_SCALE;

 pango_font_metrics_unref(metrics);
}

static void wr_set_backend(void)
{
 WpeRender.draw_rect  = wr_draw_rect;
 WpeRender.draw_text  = wr_draw_text;
 WpeRender.draw_line  = wr_draw_line;
 WpeRender.clear_rect = wr_clear_rect;
 WpeRender.draw_acs   = wr_draw_acs;
 WpeRender.draw_lock  = wr_draw_lock;
 WpeRender.flush      = wr_flush;
 WpeRender.flush_all  = wr_flush_all;
 WpeRender.blit       = wr_blit;
 WpeRender.resize     = wr_resize;
 WpeRender.cleanup    = wr_cleanup;
}

/* wpe_render_wayland_probe_cell - measure the monospace cell (width x height in
   px) on a throwaway image surface, WITHOUT touching the real wl_shm buffer.
   Lets WpeWaylandInit size the window to a cell grid before the buffer exists
   (the persistent layout is built later by wpe_render_wayland_init). */
int wpe_render_wayland_probe_cell(int *cw, int *ch)
{
 cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 8, 8);
 cairo_t *c = cairo_create(s);
 PangoLayout *l = pango_cairo_create_layout(c);
 PangoFontDescription *f;
 PangoRectangle lg;
 char fws[64];

 snprintf(fws, sizeof fws, "monospace %d", wl_font_point_size());
 f = pango_font_description_from_string(fws);
 pango_layout_set_font_description(l, f);
 pango_layout_set_text(l, "M", 1);
 pango_layout_get_pixel_extents(l, NULL, &lg);
 *cw = lg.width  > 0 ? lg.width  : 8;
 *ch = lg.height > 0 ? lg.height : 16;
 pango_font_description_free(f);
 g_object_unref(l);
 cairo_destroy(c);
 cairo_surface_destroy(s);
 return 0;
}

/* wpe_render_wayland_init - lay a Cairo image surface over the (already
   allocated) wl_shm buffer, load the font, build the colour table and publish
   the primitives through WpeRender.  Returns 0 on success. */
int wpe_render_wayland_init(void)
{
 if (!WpeWl.pixels || WpeWl.width <= 0 || WpeWl.height <= 0)
  return -1;

 wl_render_init_colors();
 wcr_surface = cairo_image_surface_create_for_data(
   (unsigned char *)WpeWl.pixels, CAIRO_FORMAT_RGB24,
   WpeWl.width, WpeWl.height, WpeWl.stride);
 wcr = cairo_create(wcr_surface);
 cairo_set_antialias(wcr, CAIRO_ANTIALIAS_GRAY);
 wl_render_init_font();
 wr_set_backend();
 return 0;
}

#endif /* HAVE_WAYLAND */
