/* we_render_cairo.c -- Cairo + Pango rendering backend for X11    */
/* Copyright (C) 2026 Juan Manuel Mendez Rey                      */
/* This is free software; see the file COPYING.                    */

#ifdef HAVE_CAIRO
#ifdef HAVE_PANGO

#include "we_render.h"
#include "edit.h"
#include "we_child.h"
#include "we_startplan.h"
#include <cairo.h>
#include <cairo-xlib.h>
#include <cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <pango/pangocairo.h>
#include <X11/Xlib.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include "we_render_glyphs.h"    /* wpe_glyph_acs / wpe_glyph_lock (shared with Wayland) */
#include "we_palette.h"          /* wpe_palette_nearest (shared with the User Screen) */

#include <fontconfig/fontconfig.h>

#ifndef NO_XWINDOWS
#include "WeXterm.h"
#include "we_lsp.h"           /* e_lsp_sem_slot_rgb, LSP_SEM_TC_MAX */
#include <X11/Xft/Xft.h>
#endif

static cairo_surface_t *cr_surface;
static cairo_t *cr;
static PangoLayout *pg_layout;
static PangoFontDescription *pg_font;
static cairo_font_face_t *cr_ft_face;
static cairo_scaled_font_t *cr_scaled;
static FT_Face cr_ft_ftface;   /* owned by us: cairo references but never frees it */

/* 16 base colours + up to LSP_SEM_TC_MAX semantic-token truecolor slots at
   indices 16+slot, so a foreground index >= 16 paints a slot's exact RGB. */
static double cairo_colors[16 + LSP_SEM_TC_MAX][3];

/* Where the painted picture goes and how it reaches the window.  The drawing
   primitives always paint with cairo onto cr_surface; a sink owns that
   surface, records which part of it changed, and puts the changed part on the
   window.  The display's visual decides which sink the editor uses, so a
   display whose colours are not plain red/green/blue masks can get a sink
   that turns every painted colour into one of the editor's own pixels. */
struct cr_sink {
 /* Make cr_surface and cr for a back buffer of this size. */
 void (*init)(int pixel_w, int pixel_h);
 /* Remake them at a new size, keeping what was already painted. */
 void (*resize)(int pixel_w, int pixel_h);
 /* A primitive painted this rectangle of cr_surface. */
 void (*touch)(int x, int y, int w, int h);
 /* Put this rectangle of the painted picture on the window. */
 void (*present)(int x, int y, int w, int h);
 /* Put everything painted since the last time on the window. */
 void (*present_all)(void);
 /* Move a rectangle of the picture inside the back buffer. */
 void (*blit)(int sx, int sy, int dx, int dy, int w, int h);
 /* Give back cr and cr_surface. */
 void (*cleanup)(void);
 /* How cairo smooths the edges of shapes and glyphs on this sink. */
 cairo_antialias_t antialias;
 /* Nonzero when the sink sends only what was touched to the window, so a
    glyph's ink outside its cell must be reported too. */
 int tracks_damage;
};

static const struct cr_sink cr_sink_direct;
static const struct cr_sink *cr_sink = &cr_sink_direct;

/**
 * cr_touch - Tell the sink that a primitive painted a rectangle.
 * @x: left edge, in pixels
 * @y: top edge, in pixels
 * @w: width, in pixels
 * @h: height, in pixels
 *
 * Every drawing primitive ends here, so a sink that only sends the changed
 * part of the picture to the window knows what changed, and a keystroke costs
 * the few cells it repainted rather than the whole window.
 */
static void cr_touch(int x, int y, int w, int h)
{
 cr_sink->touch(x, y, w, h);
}

/**
 * cr_touch_line - Tell the sink that a straight line was painted.
 * @x1: start column, in pixels
 * @y1: start row, in pixels
 * @x2: end column, in pixels
 * @y2: end row, in pixels
 * @width: the line's width, in pixels
 *
 * The rectangle covers both ends and the line's width on every side, so the
 * whole line reaches the window, smoothed edges included.
 */
static void cr_touch_line(int x1, int y1, int x2, int y2, int width)
{
 int left = x1, top = y1, right = x2, bottom = y2;

 if (x2 < x1)
 {
  left = x2;
  right = x1;
 }
 if (y2 < y1)
 {
  top = y2;
  bottom = y1;
 }
 cr_touch(left - width, top - width,
          right - left + 2 * width + 1, bottom - top + 2 * width + 1);
}

/**
 * cr_touch_arrow - Tell the sink that a scrollbar arrow was painted.
 * @cx: the arrow's centre column, in pixels
 * @cy: the arrow's centre row, in pixels
 * @sz: the arrow's half size, in pixels
 *
 * The rectangle is the square every arrow direction fits in, so a scrollbar
 * arrow appears on the window in full the moment its window is repainted.
 */
static void cr_touch_arrow(int cx, int cy, int sz)
{
 cr_touch(cx - sz, cy - sz, 2 * sz + 1, 2 * sz + 1);
}

/**
 * cr_touch_ink - Tell the sink that glyph ink covered a rectangle.
 * @x: left edge, in pixels, possibly fractional
 * @y: top edge, in pixels, possibly fractional
 * @w: width, in pixels
 * @h: height, in pixels
 *
 * The rectangle is widened to whole pixels, so the last column of a glyph
 * that ends half way through a pixel still reaches the window.  A glyph
 * with no ink (a space) reports nothing.
 */
static void cr_touch_ink(double x, double y, double w, double h)
{
 int left, top, right, bottom;

 if (w <= 0 || h <= 0)
  return;
 left = (int)floor(x);
 top = (int)floor(y);
 right = (int)ceil(x + w);
 bottom = (int)ceil(y + h);
 cr_touch(left, top, right - left, bottom - top);
}

static void cr_touch_glyph_ink(cairo_glyph_t *glyph);

/**
 * cr_sink_font_options - Make glyph options sharp when the sink wants it.
 * @opts: font options about to be used for the editor's glyphs
 *
 * Both fonts the editor draws with (the Pango text font and the one used
 * while a scrollbar is dragged) take their options through here, so on a
 * display that can only show the editor's 16 colours every glyph is drawn
 * without in-between colours, and on any other display nothing changes.
 */
static void cr_sink_font_options(cairo_font_options_t *opts)
{
 if (cr_sink->antialias != CAIRO_ANTIALIAS_NONE)
  return;
 cairo_font_options_set_antialias(opts, CAIRO_ANTIALIAS_NONE);
 cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_FULL);
}

/**
 * cr_apply_antialias - Set the edge smoothing the current sink wants on cr.
 *
 * Called right after every cairo_create, at start-up and after a resize, so
 * the new context draws exactly like the one it replaces.  On a display that
 * shows real red/green/blue the edges stay smoothed with grey levels, as the
 * editor always drew them.  A sink that can only show the editor's 16 colours
 * turns smoothing off, for shapes and for text, so every glyph is drawn in
 * its own colour and stays sharp instead of collecting stray in-between
 * colours that have no matching cell; the font options go on the context
 * before the font is measured, so the cell size matches the sharp glyphs.
 */
static void cr_apply_antialias(void)
{
 cairo_font_options_t *opts;

 cairo_set_antialias(cr, cr_sink->antialias);
 if (cr_sink->antialias != CAIRO_ANTIALIAS_NONE)
  return;

 opts = cairo_font_options_create();
 cr_sink_font_options(opts);
 cairo_set_font_options(cr, opts);
 cairo_font_options_destroy(opts);
 if (pg_layout)
  pango_cairo_update_layout(cr, pg_layout);
}

static void cairo_init_sem_truecolors(void)
{
 int s, r, g, b;
 for (s = 0; s < LSP_SEM_TC_MAX; s++)
 {
  if (!e_lsp_sem_slot_rgb(s, &r, &g, &b))
   break;
  cairo_colors[16 + s][0] = r / 255.0;
  cairo_colors[16 + s][1] = g / 255.0;
  cairo_colors[16 + s][2] = b / 255.0;
 }
}

static void cairo_init_colors(void)
{
 int i;
#ifndef NO_XWINDOWS
 for (i = 0; i < 16; i++)
 {
  unsigned short rgb[3];
  e_x_palette_rgb16(i, rgb);
  cairo_colors[i][0] = rgb[0] / 65535.0;
  cairo_colors[i][1] = rgb[1] / 65535.0;
  cairo_colors[i][2] = rgb[2] / 65535.0;
 }
#else
 for (i = 0; i < 16; i++)
  cairo_colors[i][0] = cairo_colors[i][1] = cairo_colors[i][2] = 0.0;
#endif
 cairo_init_sem_truecolors();
}

static void cr_draw_rect(int x, int y, int w, int h, int color_idx)
{
 cairo_set_source_rgb(cr, cairo_colors[color_idx][0],
   cairo_colors[color_idx][1], cairo_colors[color_idx][2]);
 cairo_rectangle(cr, x, y, w, h);
 cairo_fill(cr);
 cr_touch(x, y, w, h);
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
 cr_touch_glyph_ink(&glyph);
}

/**
 * cr_touch_glyph_ink - Report the ink of a glyph drawn while dragging.
 * @glyph: the glyph just shown with cr_scaled
 *
 * A glyph may reach past its cell; on a sink that sends only what was
 * touched, its whole ink then still reaches the window.
 */
static void cr_touch_glyph_ink(cairo_glyph_t *glyph)
{
 cairo_text_extents_t ext;

 if (!cr_sink->tracks_damage)
  return;
 cairo_scaled_font_glyph_extents(cr_scaled, glyph, 1, &ext);
 cr_touch_ink(glyph->x + ext.x_bearing, glyph->y + ext.y_bearing,
              ext.width, ext.height);
}

/**
 * cr_touch_layout_ink - Report the ink of the text the layout just drew.
 * @x: where the layout was drawn, left edge in pixels
 * @y: where the layout was drawn, top edge in pixels
 *
 * A fallback glyph wider than its cells, an overhang or a combining mark
 * can paint outside the cell box; on a sink that sends only what was
 * touched, that ink must reach the window too, or it shows up only after
 * the next full repaint.
 */
static void cr_touch_layout_ink(int x, int y)
{
 PangoRectangle ink;

 if (!cr_sink->tracks_damage)
  return;
 pango_layout_get_pixel_extents(pg_layout, &ink, NULL);
 cr_touch_ink(x + ink.x, y + ink.y, ink.width, ink.height);
}

static void cr_draw_text_pango(int x, int y, const char *utf8, int u8len,
                               int fg_idx)
{
 cairo_set_source_rgb(cr, cairo_colors[fg_idx][0],
   cairo_colors[fg_idx][1], cairo_colors[fg_idx][2]);
 cairo_move_to(cr, x, y);
 pango_layout_set_text(pg_layout, utf8, u8len);
 pango_cairo_show_layout(cr, pg_layout);
 cr_touch_layout_ink(x, y);
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
 cr_touch(x, y, WpeRender.font_width * cell_width, WpeRender.font_height);
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
 cr_touch_line(x1, y1, x2, y2, width);
}

static void cr_clear_rect(int x, int y, int w, int h, int color_idx)
{
 cr_draw_rect(x, y, w, h, color_idx);
}

static void cr_draw_acs(int sc, int px, int py, int fg_idx, int bg_idx)
{
 wpe_glyph_acs(cr, sc, px, py, WpeRender.font_width, WpeRender.font_height,
               cairo_colors[fg_idx], cairo_colors[bg_idx]);
 cr_touch(px, py, WpeRender.font_width, WpeRender.font_height);
}

static void cr_draw_lock(int px, int py, int cw, int fg_idx, int bg_idx)
{
 wpe_glyph_lock(cr, px, py, WpeRender.font_width * cw, WpeRender.font_height,
                cairo_colors[fg_idx], cairo_colors[bg_idx]);
 cr_touch(px, py, WpeRender.font_width * cw, WpeRender.font_height);
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  The direct sink: cairo paints straight into the back buffer pixmap through
  the X server, and presenting copies the back buffer to the window.  Right
  for a display whose pixels are red, green and blue masks.
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/**
 * cr_direct_touch - Nothing to record on the direct sink.
 * @x: left edge, in pixels
 * @y: top edge, in pixels
 * @w: width, in pixels
 * @h: height, in pixels
 *
 * Cairo already painted into the back buffer and the whole of it is copied
 * at the end of each refresh, so a changed rectangle needs no bookkeeping.
 */
static void cr_direct_touch(int x, int y, int w, int h)
{
 (void)x;
 (void)y;
 (void)w;
 (void)h;
}

/**
 * cr_direct_present - Copy a rectangle of the back buffer to the window.
 * @x: left edge, in pixels
 * @y: top edge, in pixels
 * @w: width, in pixels
 * @h: height, in pixels
 *
 * What the user sees after a refresh: the frame the editor finished
 * painting, put on the window in one copy so it never shows half drawn.
 */
static void cr_direct_present(int x, int y, int w, int h)
{
#ifndef NO_XWINDOWS
 cairo_surface_flush(cr_surface);
 XCopyArea(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.window,
   WpeXInfo.gc, x, y, w, h, x, y);
#endif
}

/**
 * cr_direct_present_all - Copy the whole editor area to the window.
 *
 * The end of every refresh, so all the cells repainted in it appear at once.
 */
static void cr_direct_present_all(void)
{
#ifndef NO_XWINDOWS
 extern int MAXSLNS, MAXSCOL;
 cr_direct_present(0, 0, WpeRender.font_width * MAXSCOL,
   WpeRender.font_height * MAXSLNS);
#endif
}

/**
 * cr_direct_blit - Move a rectangle inside the back buffer.
 * @sx: source left edge, in pixels
 * @sy: source top edge, in pixels
 * @dx: destination left edge, in pixels
 * @dy: destination top edge, in pixels
 * @w: width, in pixels
 * @h: height, in pixels
 *
 * Lets a scroll move the text already painted instead of painting it again.
 */
static void cr_direct_blit(int sx, int sy, int dx, int dy, int w, int h)
{
#ifndef NO_XWINDOWS
 cairo_surface_flush(cr_surface);
 XCopyArea(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.backbuf,
   WpeXInfo.gc, sx, sy, w, h, dx, dy);
 cairo_surface_mark_dirty_rectangle(cr_surface, dx, dy, w, h);
#endif
}

/**
 * cr_direct_resize - Remake the back buffer and cairo context at a new size.
 * @pixel_w: new width, in pixels
 * @pixel_h: new height, in pixels
 *
 * After the user resizes the window, what was painted stays in place while
 * the editor lays the screen out again, and the new area starts black.
 */
static void cr_direct_resize(int pixel_w, int pixel_h)
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
 cr_apply_antialias();

 if (old_buf)
  XFreePixmap(WpeXInfo.display, old_buf);
#endif
}

/**
 * cr_direct_cleanup - Give back the cairo context and surface.
 *
 * When the editor closes its window, so nothing still points at the back
 * buffer.
 */
static void cr_direct_cleanup(void)
{
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

/**
 * cr_direct_init - Wrap the back buffer in a cairo surface and context.
 * @pixel_w: width of the back buffer, in pixels
 * @pixel_h: height of the back buffer, in pixels
 *
 * At start-up, so the first frame the user sees is painted straight into
 * the back buffer.
 */
static void cr_direct_init(int pixel_w, int pixel_h)
{
 cr_surface = cairo_xlib_surface_create(WpeXInfo.display,
   WpeXInfo.backbuf,
   DefaultVisual(WpeXInfo.display, WpeXInfo.screen),
   pixel_w, pixel_h);
 cr = cairo_create(cr_surface);
 cr_apply_antialias();
}

static const struct cr_sink cr_sink_direct = {
 cr_direct_init,
 cr_direct_resize,
 cr_direct_touch,
 cr_direct_present,
 cr_direct_present_all,
 cr_direct_blit,
 cr_direct_cleanup,
 CAIRO_ANTIALIAS_GRAY,
 0
};

#ifndef NO_XWINDOWS
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  The indexed sink: cairo paints into an image in memory, and presenting a
  rectangle turns each of its pixels into one of the 16 pixels the editor
  allocated, puts the result into the back buffer and copies it to the
  window.  Right for a display whose pixels are indexes into a colormap (a
  palette display), where cairo would otherwise pick pixels from cells the
  editor never allocated.
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* Above this many rectangles a refresh presents the box around them all,
   which costs fewer requests than many small ones. */
#define CR_DIRTY_MAX_RECTS 16
/* The remembered nearest entries: one per 15-bit colour (5 bits a
   channel); CR_IX_UNFILLED marks a colour not looked up yet. */
#define CR_IX_MEMO_SIZE (1 << 15)
#define CR_IX_UNFILLED 0xFF
#define CR_RGB24_MASK 0xFFFFFFu
/* Bits of a channel the memo keeps, and of an 8-bit channel. */
#define CR_IX_MEMO_BITS 5
#define CR_CHANNEL_BITS 8
/* A display whose pixels are one byte each: the mapping writes bytes. */
#define CR_IX_BYTE_PIXEL_BITS 8
/* The scanline unit the XImage is made with, in bits. */
#define CR_IX_PAD_BITS 32

/* The image XPutImage sends, as wide as the surface. */
static XImage *cr_ix_image;
/* What was painted since the last present, in surface pixels. */
static cairo_region_t *cr_ix_dirty;
/* Each palette entry as cairo writes it into the image (0x00RRGGBB), and
   as 8-bit channels for the nearest-colour search. */
static unsigned long cr_ix_rgb24[WPE_PALETTE_SIZE];
static unsigned char cr_ix_pal[WPE_PALETTE_SIZE][3];
/* The pixel the server gave each entry. */
static unsigned long cr_ix_pixel[WPE_PALETTE_SIZE];
/* The nearest entry of every 15-bit colour looked up so far. */
static unsigned char cr_ix_memo[CR_IX_MEMO_SIZE];
/* The last colour mapped and its entry: runs of one colour are the rule. */
static unsigned long cr_ix_last_rgb24;
static int cr_ix_last_idx = -1;
/* Bytes handed to XPutImage since the last present_all, for the trace. */
static unsigned long cr_ix_bytes;

/**
 * cr_ix_load_palette - Learn the editor's 16 colours and their pixels.
 *
 * At start-up, before the first frame, so every colour cairo paints in can
 * be turned into the pixel of the same palette entry.
 */
static void cr_ix_load_palette(void)
{
 int i;

 for (i = 0; i < WPE_PALETTE_SIZE; i++)
 {
  unsigned short rgb[3];

  e_x_palette_rgb16(i, rgb);
  cr_ix_pal[i][0] = rgb[0] >> CR_CHANNEL_BITS;
  cr_ix_pal[i][1] = rgb[1] >> CR_CHANNEL_BITS;
  cr_ix_pal[i][2] = rgb[2] >> CR_CHANNEL_BITS;
  cr_ix_rgb24[i] = ((unsigned long)cr_ix_pal[i][0] << 16)
                   | ((unsigned long)cr_ix_pal[i][1] << 8)
                   | (unsigned long)cr_ix_pal[i][2];
  cr_ix_pixel[i] = e_x_palette_pixel(i);
 }
 memset(cr_ix_memo, CR_IX_UNFILLED, sizeof(cr_ix_memo));
 cr_ix_last_idx = -1;
}

/**
 * cr_ix_exact_index - The palette entry painted in exactly this colour.
 * @rgb24: a pixel of the image, 0x00RRGGBB
 *
 * Text, boxes and backgrounds are painted in palette colours, so nearly
 * every pixel is found here and keeps its own entry, also when two entries
 * are close to each other (light grey and white).
 *
 * Return: the entry, or -1 when no entry has this colour.
 */
static int cr_ix_exact_index(unsigned long rgb24)
{
 int i;

 for (i = 0; i < WPE_PALETTE_SIZE; i++)
  if (cr_ix_rgb24[i] == rgb24)
   return i;
 return -1;
}

/**
 * cr_ix_channel_widen - A memo channel back to 8 bits.
 * @c5: a channel cut to CR_IX_MEMO_BITS bits
 *
 * Return: the 8-bit channel at the same place in the range.
 */
static int cr_ix_channel_widen(int c5)
{
 return (c5 << (CR_CHANNEL_BITS - CR_IX_MEMO_BITS))
        | (c5 >> (2 * CR_IX_MEMO_BITS - CR_CHANNEL_BITS));
}

/**
 * cr_ix_nearest_index - The palette entry nearest to a colour no entry has.
 * @rgb24: a pixel of the image, 0x00RRGGBB
 *
 * For the colours that are not in the palette (a semantic-token colour, the
 * dimmed scrollbar track): they show as the nearest of the editor's 16, by
 * the rule the User Screen uses.  The answer is remembered per 15-bit
 * colour, so a frame full of them costs one search per colour.
 *
 * Return: the entry.
 */
static int cr_ix_nearest_index(unsigned long rgb24)
{
 int r5 = (rgb24 >> (16 + CR_CHANNEL_BITS - CR_IX_MEMO_BITS)) & 0x1F;
 int g5 = (rgb24 >> (8 + CR_CHANNEL_BITS - CR_IX_MEMO_BITS)) & 0x1F;
 int b5 = (rgb24 >> (CR_CHANNEL_BITS - CR_IX_MEMO_BITS)) & 0x1F;
 int key = (r5 << (2 * CR_IX_MEMO_BITS)) | (g5 << CR_IX_MEMO_BITS) | b5;

 if (cr_ix_memo[key] == CR_IX_UNFILLED)
  cr_ix_memo[key] = (unsigned char)wpe_palette_nearest(cr_ix_pal,
                      cr_ix_channel_widen(r5), cr_ix_channel_widen(g5),
                      cr_ix_channel_widen(b5));
 return cr_ix_memo[key];
}

/**
 * cr_pixel_index - The palette entry a pixel of the image is shown as.
 * @rgb24: a pixel of the image, 0x00RRGGBB
 *
 * Return: the entry, 0..WPE_PALETTE_SIZE-1.
 */
static int cr_pixel_index(unsigned long rgb24)
{
 int idx;

 if (cr_ix_last_idx >= 0 && rgb24 == cr_ix_last_rgb24)
  return cr_ix_last_idx;
 idx = cr_ix_exact_index(rgb24);
 if (idx < 0)
  idx = cr_ix_nearest_index(rgb24);
 cr_ix_last_rgb24 = rgb24;
 cr_ix_last_idx = idx;
 return idx;
}

/**
 * cr_ix_surface_rect - The surface's own rectangle.
 * @r: receives it
 */
static void cr_ix_surface_rect(cairo_rectangle_int_t *r)
{
 r->x = 0;
 r->y = 0;
 r->width = cairo_image_surface_get_width(cr_surface);
 r->height = cairo_image_surface_get_height(cr_surface);
}

/**
 * cr_ix_clip_rect - Cut a rectangle to the part inside the surface.
 * @r: the rectangle, cut in place
 *
 * A shape may be drawn partly outside the window (an arrow at the edge,
 * a glyph's overhang); only its visible part is ever mapped and sent.
 *
 * Return: nonzero when something of it is left.
 */
static int cr_ix_clip_rect(cairo_rectangle_int_t *r)
{
 cairo_rectangle_int_t all;
 int right, bottom;

 cr_ix_surface_rect(&all);
 right = r->x + r->width;
 bottom = r->y + r->height;
 if (r->x < 0)
  r->x = 0;
 if (r->y < 0)
  r->y = 0;
 if (right > all.width)
  right = all.width;
 if (bottom > all.height)
  bottom = all.height;
 r->width = right - r->x;
 r->height = bottom - r->y;
 return r->width > 0 && r->height > 0;
}

/**
 * cr_ix_mapped_pixel - The editor's pixel for a pixel of the image.
 * @rgb: a pixel of the image surface
 *
 * Return: the pixel the server gave the palette entry it is shown as.
 */
static unsigned long cr_ix_mapped_pixel(uint32_t rgb)
{
 return cr_ix_pixel[cr_pixel_index(rgb & CR_RGB24_MASK)];
}

/**
 * cr_ix_span_widen - Take one more changed column into a span.
 * @col: the column that changed
 * @lo: first changed column so far
 * @hi: one past the last changed column so far
 */
static void cr_ix_span_widen(int col, int *lo, int *hi)
{
 if (col < *lo)
  *lo = col;
 if (col + 1 > *hi)
  *hi = col + 1;
}

/**
 * cr_ix_map_row - Turn one row of a rectangle into the editor's pixels.
 * @row: the row, in surface pixels
 * @x: left edge, in pixels
 * @w: width, in pixels
 * @lo: widened to the first column whose pixel changed
 * @hi: widened to one past the last column whose pixel changed
 *
 * Writes into cr_ix_image, which always holds what the back buffer holds:
 * a byte per pixel on an 8-bit display, which is nearly every palette
 * display, and XPutPixel on any other depth.  A cell the editor repainted
 * in the colours it already had changes no pixel, and is not sent.
 */
static void cr_ix_map_row(int row, int x, int w, int *lo, int *hi)
{
 const unsigned char *data = cairo_image_surface_get_data(cr_surface);
 int stride = cairo_image_surface_get_stride(cr_surface);
 const uint32_t *src = (const uint32_t *)(data + (size_t)row * stride);
 unsigned long pixel;
 int col;

 if (cr_ix_image->bits_per_pixel == CR_IX_BYTE_PIXEL_BITS)
 {
  unsigned char *dst = (unsigned char *)cr_ix_image->data
                       + (size_t)row * cr_ix_image->bytes_per_line;
  for (col = x; col < x + w; col++)
  {
   pixel = cr_ix_mapped_pixel(src[col]);
   if (dst[col] != (unsigned char)pixel)
   {
    dst[col] = (unsigned char)pixel;
    cr_ix_span_widen(col, lo, hi);
   }
  }
  return;
 }
 for (col = x; col < x + w; col++)
 {
  pixel = cr_ix_mapped_pixel(src[col]);
  if (XGetPixel(cr_ix_image, col, row) != pixel)
  {
   XPutPixel(cr_ix_image, col, row, pixel);
   cr_ix_span_widen(col, lo, hi);
  }
 }
}

/**
 * cr_ix_band_end - Where the text row holding a pixel row ends.
 * @top: a pixel row
 * @limit: the row the caller stops at
 *
 * Changes are collected per text row, so the cells one refresh changed
 * come out as a few rectangles even when they lie far apart.
 *
 * Return: the first pixel row past that text row, at most @limit.
 */
static int cr_ix_band_end(int top, int limit)
{
 int band_h = WpeRender.font_height;
 int end;

 if (band_h <= 0)
  return limit;
 end = (top / band_h + 1) * band_h;
 if (end > limit)
  end = limit;
 return end;
}

/**
 * cr_ix_map_rect - Map a painted rectangle and collect what changed.
 * @r: the rectangle, inside the surface
 * @changed: receives, per text row, the box of the pixels that changed
 */
static void cr_ix_map_rect(const cairo_rectangle_int_t *r,
                           cairo_region_t *changed)
{
 cairo_rectangle_int_t band;
 int top, bottom, row, lo, hi;

 for (top = r->y; top < r->y + r->height; top = bottom)
 {
  bottom = cr_ix_band_end(top, r->y + r->height);
  lo = r->x + r->width;
  hi = r->x;
  for (row = top; row < bottom; row++)
   cr_ix_map_row(row, r->x, r->width, &lo, &hi);
  if (hi <= lo)
   continue;
  band.x = lo;
  band.y = top;
  band.width = hi - lo;
  band.height = bottom - top;
  cairo_region_union_rectangle(changed, &band);
 }
}

/**
 * cr_ix_put_bytes - The pixel bytes XPutImage sends for a rectangle.
 * @r: the rectangle
 *
 * Each row goes out padded to the 32-bit unit the image was made with;
 * the trace adds these up so a slow refresh can be told by its size.
 *
 * Return: the byte count.
 */
static unsigned long cr_ix_put_bytes(const cairo_rectangle_int_t *r)
{
 unsigned long row_bits = (unsigned long)r->width * cr_ix_image->bits_per_pixel;
 unsigned long row_bytes = (row_bits + CR_IX_PAD_BITS - 1) / CR_IX_PAD_BITS
                           * (CR_IX_PAD_BITS / CR_CHANNEL_BITS);

 return row_bytes * (unsigned long)r->height;
}

/**
 * cr_ix_put_rect - Send one rectangle of the mapped picture to the window.
 * @r: the rectangle, inside the surface
 *
 * Puts it into the back buffer (which Expose copies from) and copies it to
 * the window.
 */
static void cr_ix_put_rect(const cairo_rectangle_int_t *r)
{
 XPutImage(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.gc, cr_ix_image,
   r->x, r->y, r->x, r->y, r->width, r->height);
 XCopyArea(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.window,
   WpeXInfo.gc, r->x, r->y, r->width, r->height, r->x, r->y);
 cr_ix_bytes += cr_ix_put_bytes(r);
}

/**
 * cr_ix_send - Send the changed part of the picture to the window.
 * @changed: what changed since it was last sent
 *
 * Each changed rectangle goes on its own; when a refresh changed many
 * scattered pieces, the box around them goes in one request instead.
 *
 * Return: how many rectangles changed.
 */
static int cr_ix_send(cairo_region_t *changed)
{
 cairo_rectangle_int_t r;
 int i, n;

 n = cairo_region_num_rectangles(changed);
 if (n > CR_DIRTY_MAX_RECTS)
 {
  cairo_region_get_extents(changed, &r);
  cr_ix_put_rect(&r);
  return n;
 }
 for (i = 0; i < n; i++)
 {
  cairo_region_get_rectangle(changed, i, &r);
  cr_ix_put_rect(&r);
 }
 return n;
}

/**
 * cr_ix_surface_ok - Whether cairo's picture of the window is there.
 *
 * Return: nonzero when the image surface was made and is usable.
 */
static int cr_ix_surface_ok(void)
{
 if (cr_surface == NULL)
  return 0;
 return cairo_surface_status(cr_surface) == CAIRO_STATUS_SUCCESS;
}

/**
 * cr_ix_ready - Whether the indexed route has what a present needs.
 *
 * Both the picture cairo paints on and the image it is sent with; either
 * may be missing after memory ran out, and a present then draws nothing
 * instead of crashing the editor.
 *
 * Return: nonzero when a present can go ahead.
 */
static int cr_ix_ready(void)
{
 return cr_ix_image != NULL && cr_ix_surface_ok();
}

/**
 * cr_ix_drop_dirty - Forget what was painted since the last present.
 *
 * When a refresh cannot be sent: the next resize repaints and sends the
 * whole window anyway.
 */
static void cr_ix_drop_dirty(void)
{
 cairo_region_destroy(cr_ix_dirty);
 cr_ix_dirty = cairo_region_create();
}

/**
 * cr_ix_touch - Remember that a primitive painted a rectangle.
 * @x: left edge, in pixels
 * @y: top edge, in pixels
 * @w: width, in pixels
 * @h: height, in pixels
 *
 * Only the rectangles painted are looked at at the end of a refresh, and
 * of those only the pixels that changed are sent, so typing one character
 * over a slow link to an X terminal sends a few cells, not the whole
 * window.
 */
static void cr_ix_touch(int x, int y, int w, int h)
{
 cairo_rectangle_int_t r;

 r.x = x;
 r.y = y;
 r.width = w;
 r.height = h;
 if (cr_ix_clip_rect(&r))
  cairo_region_union_rectangle(cr_ix_dirty, &r);
}

/**
 * cr_ix_present - Send a rectangle of the picture to the window now.
 * @x: left edge, in pixels
 * @y: top edge, in pixels
 * @w: width, in pixels
 * @h: height, in pixels
 */
static void cr_ix_present(int x, int y, int w, int h)
{
 cairo_rectangle_int_t r;
 cairo_region_t *changed;

 r.x = x;
 r.y = y;
 r.width = w;
 r.height = h;
 if (!cr_ix_ready() || !cr_ix_clip_rect(&r))
  return;
 cairo_surface_flush(cr_surface);
 changed = cairo_region_create();
 cr_ix_map_rect(&r, changed);
 cr_ix_send(changed);
 cairo_region_destroy(changed);
 cairo_region_subtract_rectangle(cr_ix_dirty, &r);
}

/**
 * cr_ix_present_trace - Say in $XWPE_UI_TRACE what a refresh sent.
 * @rects: how many rectangles it sent
 *
 * Someone wondering why typing is slow on a remote palette display reads
 * here how many bytes each refresh put on the wire.  Nothing is written
 * when the variable is not set.
 */
static void cr_ix_present_trace(int rects)
{
 wpe_ui_trace("palette present rects=%d bytes=%lu", rects, cr_ix_bytes);
}

/**
 * cr_ix_present_all - Send what changed since the last refresh.
 *
 * The end of every refresh: everything painted is mapped to the editor's
 * pixels, and what came out different from the window is sent.
 */
static void cr_ix_present_all(void)
{
 cairo_rectangle_int_t r;
 cairo_region_t *changed;
 int i, n;

 n = cairo_region_num_rectangles(cr_ix_dirty);
 if (n == 0)
  return;
 if (!cr_ix_ready())
 {
  cr_ix_drop_dirty();
  return;
 }
 cr_ix_bytes = 0;
 cairo_surface_flush(cr_surface);
 changed = cairo_region_create();
 for (i = 0; i < n; i++)
 {
  cairo_region_get_rectangle(cr_ix_dirty, i, &r);
  cr_ix_map_rect(&r, changed);
 }
 n = cr_ix_send(changed);
 if (n > 0)
  cr_ix_present_trace(n);
 cairo_region_destroy(changed);
 cairo_region_destroy(cr_ix_dirty);
 cr_ix_dirty = cairo_region_create();
}

/**
 * cr_ix_blit - Move a rectangle inside the painted picture.
 * @sx: source left edge, in pixels
 * @sy: source top edge, in pixels
 * @dx: destination left edge, in pixels
 * @dy: destination top edge, in pixels
 * @w: width, in pixels
 * @h: height, in pixels
 *
 * Lets a scroll move the text already painted instead of painting it
 * again; the moved rectangle reaches the window with the next refresh.
 */
static void cr_ix_blit(int sx, int sy, int dx, int dy, int w, int h)
{
 unsigned char *data;
 int stride, row, first, last, step;

 if (!cr_ix_surface_ok())
  return;
 cairo_surface_flush(cr_surface);
 data = cairo_image_surface_get_data(cr_surface);
 stride = cairo_image_surface_get_stride(cr_surface);
 first = 0;
 last = h;
 step = 1;
 if (dy > sy)
 {
  first = h - 1;
  last = -1;
  step = -1;
 }
 for (row = first; row != last; row += step)
  memmove(data + (size_t)(dy + row) * stride + (size_t)dx * sizeof(uint32_t),
          data + (size_t)(sy + row) * stride + (size_t)sx * sizeof(uint32_t),
          (size_t)w * sizeof(uint32_t));
 cairo_surface_mark_dirty_rectangle(cr_surface, dx, dy, w, h);
 cr_ix_touch(dx, dy, w, h);
}

/**
 * cr_ix_fill_backbuf - Paint the back buffer in palette entry 0.
 * @pixel_w: width, in pixels
 * @pixel_h: height, in pixels
 *
 * A new back buffer starts in a colour the editor allocated, so whatever
 * the window shows before the first frame is the editor's own black, also
 * in a private colormap where the screen's black pixel may be no colour.
 */
static void cr_ix_fill_backbuf(int pixel_w, int pixel_h)
{
 XSetForeground(WpeXInfo.display, WpeXInfo.gc, e_x_palette_pixel(0));
 XFillRectangle(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.gc,
   0, 0, pixel_w, pixel_h);
}

/* The test fault $XWPE_TEST_FAULT names for the indexed route,
   "palette-image:<n>": the n-th picture memory the route asks for is
   refused, as on a machine out of memory. */
#define CR_IX_FAULT_NAME "palette-image"
#define CR_IX_FAULT_UNREAD (-1)
#define CR_IX_FAULT_NONE 0
static int cr_ix_fault_n = CR_IX_FAULT_UNREAD;
/* How many times the route has asked for picture memory. */
static int cr_ix_image_asks;

/**
 * cr_ix_fault_from_env - Read which picture memory a test refuses.
 *
 * Read once, at the first request.  A test of the running editor arms it to
 * show that a window whose picture memory is refused stays alive, draws
 * nothing, says so once, and draws again after the next resize.  Unset, or
 * naming another fault point, arms nothing.
 *
 * Return: the request to refuse, counting from 1, or CR_IX_FAULT_NONE.
 */
static int cr_ix_fault_from_env(void)
{
 const char *spec;
 size_t len = strlen(CR_IX_FAULT_NAME);

 if (cr_ix_fault_n != CR_IX_FAULT_UNREAD)
  return cr_ix_fault_n;
 cr_ix_fault_n = CR_IX_FAULT_NONE;
 spec = getenv("XWPE_TEST_FAULT");
 if (spec && strncmp(spec, CR_IX_FAULT_NAME, len) == 0 && spec[len] == ':')
  cr_ix_fault_n = atoi(spec + len + 1);
 return cr_ix_fault_n;
}

/**
 * cr_ix_image_memory - The memory for the picture the presents send.
 * @bytes_per_line: one row, as the server pads it
 * @pixel_h: rows
 *
 * Return: zeroed memory, or NULL when there is none (or a test refused it).
 */
static char *cr_ix_image_memory(int bytes_per_line, int pixel_h)
{
 cr_ix_image_asks++;
 if (cr_ix_image_asks == cr_ix_fault_from_env())
  return NULL;
 return calloc((size_t)bytes_per_line, (size_t)pixel_h);
}

/**
 * cr_ix_image_refused - Say once that a window size got no picture memory.
 * @pixel_w: width, in pixels
 * @pixel_h: height, in pixels
 *
 * The user sees a window that stays as it was; this line on stderr tells
 * why, and that a smaller window (or another resize) draws again.
 */
static void cr_ix_image_refused(int pixel_w, int pixel_h)
{
 fprintf(stderr, "xwpe: out of memory for a %dx%d window picture, "
         "not drawing until the window is resized\n", pixel_w, pixel_h);
 wpe_ui_trace("palette image refused w=%d h=%d", pixel_w, pixel_h);
}

/**
 * cr_ix_image_made_trace - Say in $XWPE_UI_TRACE that a window size got its
 * picture memory.
 * @pixel_w: width, in pixels
 * @pixel_h: height, in pixels
 *
 * Read with the refusal line: someone whose window stayed empty sees from
 * which resize on it is drawn again.  Nothing is written when the variable
 * is not set.
 */
static void cr_ix_image_made_trace(int pixel_w, int pixel_h)
{
 wpe_ui_trace("palette image made w=%d h=%d", pixel_w, pixel_h);
}

/**
 * cr_ix_make_image - Make the XImage the presents are sent with.
 * @pixel_w: width, in pixels
 * @pixel_h: height, in pixels
 *
 * The server's own pixel size and byte order, so the mapped pixels go out
 * as they are on any depth and on any machine.  It starts in palette
 * entry 0, like the back buffer, because it always holds what the back
 * buffer holds: that is how a refresh knows which pixels changed.  When
 * there is no memory for it the editor keeps running and draws nothing
 * until the next resize makes the image again.
 *
 * Return: nonzero when the image is there.
 */
static int cr_ix_make_image(int pixel_w, int pixel_h)
{
 Display *dpy = WpeXInfo.display;

 cr_ix_image = XCreateImage(dpy, DefaultVisual(dpy, WpeXInfo.screen),
   DefaultDepth(dpy, WpeXInfo.screen), ZPixmap, 0, NULL,
   pixel_w, pixel_h, CR_IX_PAD_BITS, 0);
 if (cr_ix_image == NULL)
 {
  cr_ix_image_refused(pixel_w, pixel_h);
  return 0;
 }
 cr_ix_image->data = cr_ix_image_memory(cr_ix_image->bytes_per_line, pixel_h);
 if (cr_ix_image->data == NULL)
 {
  XDestroyImage(cr_ix_image);
  cr_ix_image = NULL;
  cr_ix_image_refused(pixel_w, pixel_h);
  return 0;
 }
 XAddPixel(cr_ix_image, (long)e_x_palette_pixel(0));
 cr_ix_image_made_trace(pixel_w, pixel_h);
 return 1;
}

/**
 * cr_ix_make_surface - Make the image surface and context cairo paints on.
 * @pixel_w: width, in pixels
 * @pixel_h: height, in pixels
 *
 * The surface starts in palette entry 0, the colour the back buffer starts
 * in, so the two agree before anything is painted.
 */
static void cr_ix_make_surface(int pixel_w, int pixel_h)
{
 cr_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
   pixel_w, pixel_h);
 cr = cairo_create(cr_surface);
 cairo_set_source_rgb(cr, cairo_colors[0][0], cairo_colors[0][1],
   cairo_colors[0][2]);
 cairo_paint(cr);
 cr_apply_antialias();
}

/**
 * cr_ix_init - Make the image, surface and context of the indexed sink.
 * @pixel_w: width of the back buffer, in pixels
 * @pixel_h: height of the back buffer, in pixels
 *
 * At start-up on a palette display, before the font is measured, so the
 * glyphs are measured as the sharp ones the editor will draw.
 */
static void cr_ix_init(int pixel_w, int pixel_h)
{
 cr_ix_load_palette();
 cr_ix_fill_backbuf(pixel_w, pixel_h);
 cr_ix_make_image(pixel_w, pixel_h);
 cr_ix_make_surface(pixel_w, pixel_h);
 cr_ix_dirty = cairo_region_create();
}

/**
 * cr_ix_resize - Remake everything at a new size, keeping the picture.
 * @pixel_w: new width, in pixels
 * @pixel_h: new height, in pixels
 *
 * After the user resizes the window, what was painted stays in place, the
 * new area starts in the editor's black, and the whole new picture is put
 * in the back buffer and on the window at once, before the callers copy
 * the back buffer to the window and lay the screen out again.
 */
static void cr_ix_resize(int pixel_w, int pixel_h)
{
 cairo_surface_t *old_surface = cr_surface;
 Pixmap old_buf = WpeXInfo.backbuf;
 cairo_rectangle_int_t all;

 WpeXInfo.backbuf = XCreatePixmap(WpeXInfo.display, WpeXInfo.window,
   pixel_w, pixel_h,
   DefaultDepth(WpeXInfo.display, WpeXInfo.screen));
 cr_ix_fill_backbuf(pixel_w, pixel_h);
 if (old_buf)
  XFreePixmap(WpeXInfo.display, old_buf);

 if (cr_ix_image)
  XDestroyImage(cr_ix_image);
 cr_ix_make_image(pixel_w, pixel_h);

 if (cr)
  cairo_destroy(cr);
 cr_ix_make_surface(pixel_w, pixel_h);
 if (old_surface)
 {
  cairo_set_source_surface(cr, old_surface, 0, 0);
  cairo_paint(cr);
  cairo_surface_destroy(old_surface);
 }

 cr_ix_surface_rect(&all);
 cairo_region_destroy(cr_ix_dirty);
 cr_ix_dirty = cairo_region_create_rectangle(&all);
 cr_ix_present_all();
}

/**
 * cr_ix_cleanup - Give back the context, surface, image and region.
 *
 * When the editor closes its window.
 */
static void cr_ix_cleanup(void)
{
 cr_direct_cleanup();
 if (cr_ix_image)
 {
  XDestroyImage(cr_ix_image);
  cr_ix_image = NULL;
 }
 if (cr_ix_dirty)
 {
  cairo_region_destroy(cr_ix_dirty);
  cr_ix_dirty = NULL;
 }
}

static const struct cr_sink cr_sink_indexed = {
 cr_ix_init,
 cr_ix_resize,
 cr_ix_touch,
 cr_ix_present,
 cr_ix_present_all,
 cr_ix_blit,
 cr_ix_cleanup,
 CAIRO_ANTIALIAS_NONE,
 1
};

/**
 * cr_sink_for_route - The sink for the way the X11 backend presents.
 * @route: what e_x_render_route() decided for the display
 *
 * A true-colour display gets the direct sink, which paints through cairo
 * into the back buffer as the editor always did; every other display gets
 * the indexed sink, so the window shows the 16 colours the editor
 * allocated and nothing else.
 *
 * Return: the sink the renderer uses from start-up on.
 */
static const struct cr_sink *cr_sink_for_route(EXRenderRoute route)
{
 if (route == E_X_RENDER_INDEXED)
  return &cr_sink_indexed;
 return &cr_sink_direct;
}
#endif

static void cr_flush(int x, int y, int w, int h)
{
 cr_sink->present(x, y, w, h);
}

static void cr_flush_all(void)
{
 cr_sink->present_all();
}

static void cr_blit(int sx, int sy, int dx, int dy, int w, int h)
{
 cr_sink->blit(sx, sy, dx, dy, w, h);
}

static void cr_resize(int pixel_w, int pixel_h)
{
 cr_sink->resize(pixel_w, pixel_h);
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
 cr_sink->cleanup();
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

/**
 * cr_init_surface - Choose the sink for this display and make its surface.
 * @pixel_w: width of the back buffer, in pixels
 * @pixel_h: height of the back buffer, in pixels
 *
 * At start-up, before the font is measured, so the cell size comes from the
 * surface the editor will really paint on.
 */
static void cr_init_surface(int pixel_w, int pixel_h)
{
#ifndef NO_XWINDOWS
 cr_sink = cr_sink_for_route(e_x_render_route());
#endif
 cr_sink->init(pixel_w, pixel_h);
}

/* Point size baked into the desktop monospace-font-name (e.g. the 13 in
   "JetBrains Mono 13"); 0 when the setting carries no size or is unset.
   Filled in by cr_get_system_monospace_font(), read by cr_font_point_size(). */
static int cr_monospace_pt;

/* The longest the desktop's font setting is waited for at start-up: a
   settings service that does not answer must not keep the window from
   opening, and the editor then uses "monospace". */
#define CR_FONT_QUERY_MS  1500L
#define CR_FONT_QUERY_MAX 128
#define CR_FONT_QUERY_WHAT "the desktop's font setting"

/**
 * cr_query_desktop_font - Ask the desktop for its monospace font.
 * @fontname: receives the raw setting, e.g. 'JetBrains Mono 13'.
 * @size: its size.
 * Return: 1 when the desktop answered, 0 when it has no such setting, the
 * program is missing, or it did not answer in time.
 */
static int cr_query_desktop_font(char *fontname, size_t size)
{
 char *argv[] = { "gsettings", "get", "org.gnome.desktop.interface",
                  "monospace-font-name", NULL };
 WpeStartPlan *plan = wpe_startplan_fixed(WPE_START_PROBE, CR_FONT_QUERY_WHAT, argv);
 int status = 0, ran;

 ran = wpe_startplan_probe(plan, fontname, size, CR_FONT_QUERY_MS, &status,
                           CR_FONT_QUERY_WHAT);
 wpe_startplan_free(plan);
 return(wpe_capture_succeeded(ran, status) && fontname[0] != '\0');
}

static const char *cr_get_system_monospace_font(void)
{
 static char fontname[CR_FONT_QUERY_MAX];
 cr_monospace_pt = 0;
 if (cr_query_desktop_font(fontname, sizeof(fontname)))
 {
  char *s = fontname;
  char *e;
  while (*s == '\'' || *s == ' ') s++;
  e = s + strlen(s) - 1;
  while (e > s && (*e == '\'' || *e == '\n' || *e == ' ')) *e-- = 0;
  /* The family name ends in a trailing point size ("JetBrains Mono 13").
     Remember it -- so the editor honours the size the user configured --
     then strip it off, leaving just the family for fontconfig. */
  if (e > s && *e >= '0' && *e <= '9')
  {
   char *d = e;
   while (d > s && *d >= '0' && *d <= '9') d--;
   cr_monospace_pt = atoi(d + 1);
   while (e > s && *e >= '0' && *e <= '9') *e-- = 0;
   while (e > s && *e == ' ') *e-- = 0;
  }
  if (*s) return s;
 }
 return "monospace";
}

/* The point size to render the editor font at, in precedence order:
   1. an explicit XWPE_FONT_SIZE override (works headless / over SSH / on macOS,
      where there may be no desktop setting);
   2. the size baked into the desktop monospace-font-name (cr_monospace_pt);
   3. a sensible built-in default.
   Call after cr_get_system_monospace_font() so cr_monospace_pt is current.
   Clamped to a sane range so a typo cannot produce a 1px or 4000px grid. */
static int cr_font_point_size(void)
{
 const char *env = getenv("XWPE_FONT_SIZE");
 int pt = (env && *env) ? atoi(env) : 0;
 if (pt < 4 || pt > 96)
  pt = cr_monospace_pt;
 if (pt < 4 || pt > 96)
  pt = 10;
 return pt;
}

static void cr_init_pango_font(void)
{
 PangoRectangle logical;
 PangoContext *pg_ctx;
 PangoFontMetrics *metrics;
 const char *sys_font = cr_get_system_monospace_font();
 char font_with_size[140];

 pg_layout = pango_cairo_create_layout(cr);

 snprintf(font_with_size, sizeof(font_with_size), "%s %d",
          sys_font, cr_font_point_size());
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
 cr_sink_font_options(opts);

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
 WpeRender.draw_lock   = cr_draw_lock;
 WpeRender.flush       = cr_flush;
 WpeRender.flush_all   = cr_flush_all;
 WpeRender.blit        = cr_blit;
 WpeRender.resize      = cr_resize;
 WpeRender.cleanup     = cr_cleanup;
}

/**
 * wpe_render_cairo_start_threads - Create Pango's font map now, which
 * starts the thread Pango loads fonts on.
 *
 * Called by start-up with SIGCHLD blocked, so that thread keeps it blocked
 * and the end of a program the editor runs is only ever seen on the
 * editor's own thread.  Both the X11 and the Wayland window later use this
 * same font map.
 */
void wpe_render_cairo_start_threads(void)
{
 (void)pango_cairo_font_map_get_default();
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
 cr_touch_arrow(cx, cy, sz);
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
 cr_touch_arrow(cx, cy, sz);
}

static void cr_chrome_track(int x, int y, int w, int h, int ci)
{
 cairo_set_source_rgb(cr, cairo_colors[ci][0] * 0.5,
   cairo_colors[ci][1] * 0.5, cairo_colors[ci][2] * 0.5);
 cairo_rectangle(cr, x, y, w, h);
 cairo_fill(cr);
 cr_touch(x, y, w, h);
}

static void cr_chrome_thumb(int x, int y, int w, int h, int ci)
{
 cairo_set_source_rgb(cr, cairo_colors[ci][0],
   cairo_colors[ci][1], cairo_colors[ci][2]);
 cairo_rectangle(cr, x, y, w, h);
 cairo_fill(cr);
 cr_touch(x, y, w, h);
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
 cr_touch_arrow(cx, cy, sz);
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
 cr_touch_arrow(cx, cy, sz);
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

 if (!cr || !WpeEditor || wpe_paint_chrome_suppressed() ||
     wpe_paint_modal_depth() > 0)
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
