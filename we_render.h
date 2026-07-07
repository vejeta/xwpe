#ifndef __WE_RENDER_H
#define __WE_RENDER_H
/*-------------------------------------------------------------------------*\
  <we_render.h> -- Rendering backend abstraction for xwpe

  Copyright (C) 2026 Juan Manuel Mendez Rey
  This is free software; see the file COPYING (GPL-2).

  Follows Kruse's function pointer pattern (fk_u_locate, e_u_refresh, etc.)
  applied to the drawing layer.  Each backend (Cairo+Pango on X11, ncurses
  on terminal, Cairo+Pango on Wayland in the future) provides a set of
  drawing primitives through the WpeRenderBackend struct.
\*-------------------------------------------------------------------------*/

/* Codepoint xwpe stores in a title-bar cell to mark a read-only window.  In
   ncurses it is drawn as the U+1F512 padlock emoji; the graphical backends
   recognise it here and paint a vector lock via draw_lock instead. */
#define WR_GLYPH_LOCK 0x1F512

typedef struct WpeRenderBackend {
 void (*draw_rect)(int x, int y, int w, int h, int color_idx);
 void (*draw_text)(int x, int y, const char *utf8, int u8len,
                   int cell_width, int fg_idx, int bg_idx);
 void (*draw_line)(int x1, int y1, int x2, int y2,
                   int color_idx, int width);
 void (*clear_rect)(int x, int y, int w, int h, int color_idx);
 void (*draw_acs)(int sc, int px, int py, int fg_idx, int bg_idx);
 /* Paint the read-only padlock as a vector icon scaled to cw cells.  The
    colour-emoji glyph (WR_GLYPH_LOCK) renders at its own large bitmap size and
    cannot be fit to a text cell, so the graphical backends draw it themselves
    -- the same reason box/scrollbar glyphs go through draw_acs.  A backend that
    leaves this NULL (ncurses) just paints the emoji character via the terminal. */
 void (*draw_lock)(int px, int py, int cw, int fg_idx, int bg_idx);
 void (*flush)(int x, int y, int w, int h);
 void (*flush_all)(void);
 void (*blit)(int sx, int sy, int dx, int dy, int w, int h);
 void (*resize)(int pixel_w, int pixel_h);
 void (*cleanup)(void);

 int font_width;
 int font_height;
 int font_ascent;
} WpeRenderBackend;

extern WpeRenderBackend WpeRender;
extern int wpe_chrome_suppress;
extern int wpe_modal_active;
extern int wpe_scroll_dragging;

int wpe_render_cairo_init(void);
void wpe_render_chrome(void);
#ifdef NO_XWINDOWS
/* The Cairo chrome (the fluid scrollbar thumb) is X11-only; with no X11 there
   is no chrome, so these hit-tests can never match.  Inline no-op stubs let the
   shared mouse code (we_mouse.c) link in a --without-x build, where
   we_render_cairo.c is not compiled. */
static inline int wpe_chrome_hit_vthumb(int col, int row) { (void)col; (void)row; return 0; }
static inline int wpe_chrome_hit_hthumb(int col, int row) { (void)col; (void)row; return 0; }
#else
int wpe_chrome_hit_vthumb(int col, int row);
int wpe_chrome_hit_hthumb(int col, int row);
#endif

#endif
