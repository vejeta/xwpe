/* we_xbackbuf.c -- the one owner of the X11 back buffer (see we_xbackbuf.h) */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#ifndef NO_XWINDOWS

#include "edit.h"
#include "WeXterm.h"
#include "we_xbackbuf.h"

/* The buffer, its size in pixels, and the wrappers to rebuild around it.  All
   private: nothing else can name the pixmap. */
static Pixmap bb_pixmap = None;
static int bb_width;
static int bb_height;

static struct {
 EXBackbufRelease release;
 EXBackbufBack back;
} bb_hooks[E_X_BACKBUF_HOOKS_MAX];
static int bb_hook_count;

/**
 * e_x_backbuf_exists - Does the editor have a back buffer now?
 *
 * Where the editor draws a frame decides what it must do after it: with a
 * buffer the frame is copied to the window, without one (the core font) it
 * was drawn on the window already.
 *
 * Return: non-zero when there is a buffer.
 */
int e_x_backbuf_exists(void)
{
 return bb_pixmap != None;
}

/**
 * e_x_backbuf_hooks_add - Register what wraps the buffer.
 * @release: gives back the wrapper before the buffer is replaced
 * @back: wraps the new buffer, which has the size it is given
 *
 * So the Xft draw and the cairo surface follow the buffer to its new size
 * without the code that resizes it knowing they exist.
 */
void e_x_backbuf_hooks_add(EXBackbufRelease release, EXBackbufBack back)
{
 int i;

 for (i = 0; i < bb_hook_count; i++)
  if (bb_hooks[i].release == release)
  {
   bb_hooks[i].back = back;
   return;
  }
 if (bb_hook_count >= E_X_BACKBUF_HOOKS_MAX)
  return;
 bb_hooks[bb_hook_count].release = release;
 bb_hooks[bb_hook_count].back = back;
 bb_hook_count++;
}

/**
 * e_x_backbuf_hooks_drop - Forget a wrapper that is going away.
 * @release: the release function it was registered with
 */
void e_x_backbuf_hooks_drop(EXBackbufRelease release)
{
 int i, j;

 for (i = 0; i < bb_hook_count; i++)
  if (bb_hooks[i].release == release)
  {
   for (j = i; j + 1 < bb_hook_count; j++)
    bb_hooks[j] = bb_hooks[j + 1];
   bb_hook_count--;
   return;
  }
}

/**
 * bb_run_release - Ask every wrapper to give the buffer back.
 */
static void bb_run_release(void)
{
 int i;

 for (i = 0; i < bb_hook_count; i++)
  if (bb_hooks[i].release)
   bb_hooks[i].release();
}

/**
 * bb_run_back - Ask every wrapper to wrap the new buffer.
 * @pixel_w: the new buffer's width, in pixels
 * @pixel_h: the new buffer's height, in pixels
 */
static void bb_run_back(int pixel_w, int pixel_h)
{
 int i;

 for (i = 0; i < bb_hook_count; i++)
  if (bb_hooks[i].back)
   bb_hooks[i].back(pixel_w, pixel_h);
}

/**
 * bb_new_pixmap - Make a pixmap and paint it in one colour.
 * @pixel_w: width, in pixels
 * @pixel_h: height, in pixels
 * @fill_pixel: the pixel it starts in
 *
 * Return: the new pixmap.
 */
static Pixmap bb_new_pixmap(int pixel_w, int pixel_h, unsigned long fill_pixel)
{
 Pixmap pixmap = XCreatePixmap(WpeXInfo.display, WpeXInfo.window,
   pixel_w, pixel_h, XDefaultDepth(WpeXInfo.display, WpeXInfo.screen));

 XSetForeground(WpeXInfo.display, WpeXInfo.gc, fill_pixel);
 XFillRectangle(WpeXInfo.display, pixmap, WpeXInfo.gc,
   0, 0, pixel_w, pixel_h);
 return pixmap;
}

/**
 * bb_carry_picture - Copy what an old buffer held into the new one.
 * @old: the old pixmap
 * @old_w: its width, in pixels
 * @old_h: its height, in pixels
 *
 * What was painted stays in place while the editor lays the screen out again:
 * the part both sizes have is carried, the rest of the new buffer stays in
 * its fill colour.
 */
static void bb_carry_picture(Pixmap old, int old_w, int old_h)
{
 int w = bb_width < old_w ? bb_width : old_w;
 int h = bb_height < old_h ? bb_height : old_h;

 XCopyArea(WpeXInfo.display, old, bb_pixmap, WpeXInfo.gc, 0, 0, w, h, 0, 0);
}

/**
 * e_x_backbuf_remake - Give the editor a back buffer of a new size.
 * @pixel_w: new width, in pixels
 * @pixel_h: new height, in pixels
 * @keep_picture: non-zero to carry what was painted into the new buffer
 * @fill_pixel: the pixel the new buffer (or its new part) starts in
 *
 * After the user resizes the window, or at start-up, the editor paints into a
 * buffer that fits the grid.  Every wrapper is released first, so what a
 * cairo surface still holds is flushed before the picture is carried, and
 * wrapped again after; the old buffer is freed last.  Starting from no buffer
 * makes one.  At the size the buffer already has this does nothing and the
 * picture stays: the editor asks twice per resize (the size, then the grid).
 */
void e_x_backbuf_remake(int pixel_w, int pixel_h, int keep_picture,
                        unsigned long fill_pixel)
{
 Pixmap old = bb_pixmap;
 int old_w = bb_width;
 int old_h = bb_height;

 if (pixel_w < 1)
  pixel_w = 1;
 if (pixel_h < 1)
  pixel_h = 1;
 if (old != None && pixel_w == old_w && pixel_h == old_h)
  return;

 bb_run_release();
 bb_pixmap = bb_new_pixmap(pixel_w, pixel_h, fill_pixel);
 bb_width = pixel_w;
 bb_height = pixel_h;
 if (keep_picture && old != None)
  bb_carry_picture(old, old_w, old_h);
 bb_run_back(pixel_w, pixel_h);
 if (old != None)
  XFreePixmap(WpeXInfo.display, old);
}

/**
 * e_x_backbuf_target - Where a renderer paints the frame.
 *
 * Return: the back buffer when there is one, else the window.
 */
Drawable e_x_backbuf_target(void)
{
 if (bb_pixmap != None)
  return bb_pixmap;
 return WpeXInfo.window;
}

/**
 * bb_clip - Cut a rectangle to the buffer.
 * @x: left edge, in pixels, moved in when it is outside
 * @y: top edge, in pixels, moved in when it is outside
 * @w: width, in pixels, shortened to fit
 * @h: height, in pixels, shortened to fit
 *
 * Under libx11-compat an Expose carries the full physical window size, whose
 * sub-cell band runs past the buffer; copying that area would drop the last
 * row (the bottom function-key bar).
 *
 * Return: non-zero when something of the rectangle is left.
 */
static int bb_clip(int *x, int *y, int *w, int *h)
{
 if (*x < 0)
 {
  *w += *x;
  *x = 0;
 }
 if (*y < 0)
 {
  *h += *y;
  *y = 0;
 }
 if (*x + *w > bb_width)
  *w = bb_width - *x;
 if (*y + *h > bb_height)
  *h = bb_height - *y;
 return *w > 0 && *h > 0;
}

/**
 * bb_copy_to_window - Put a rectangle of the buffer on the window.
 */
static void bb_copy_to_window(int x, int y, int w, int h)
{
 if (!bb_clip(&x, &y, &w, &h))
  return;
 XCopyArea(WpeXInfo.display, bb_pixmap, WpeXInfo.window, WpeXInfo.gc,
   x, y, w, h, x, y);
}

/**
 * e_x_backbuf_present - Show an area the editor just painted.
 * @x: left edge, in pixels
 * @y: top edge, in pixels
 * @w: width, in pixels
 * @h: height, in pixels
 *
 * What the user sees after a refresh: the finished frame goes onto the window
 * in one copy, so it never shows half drawn.  With no buffer the painting
 * went to the window and nothing is copied; after a resize the layout walk
 * repaints, and painting the old screen cells before it would be wrong.
 */
void e_x_backbuf_present(int x, int y, int w, int h)
{
 if (bb_pixmap == None)
  return;
 bb_copy_to_window(x, y, w, h);
}

/* The cells repainted for an Expose are the rectangle's own cells (its origin
   rounded down) and this many more in width and in height, so a rectangle
   that starts or ends inside a cell is covered. */
#define BB_EXPOSE_SLACK_CELLS 2

/**
 * e_x_backbuf_restore - Give a window area its picture back.
 * @x: left edge of the lost area, in pixels
 * @y: top edge, in pixels
 * @w: width, in pixels
 * @h: height, in pixels
 *
 * Whoever covered the window and moved away gets the editor's picture back
 * from the buffer, or, with no buffer, from the screen cells (marked for
 * repaint here, painted by e_x_backbuf_restore_done).
 */
void e_x_backbuf_restore(int x, int y, int w, int h)
{
 if (bb_pixmap != None)
 {
  bb_copy_to_window(x, y, w, h);
  return;
 }
 e_refresh_area(x / WpeXInfo.font_width, y / WpeXInfo.font_height,
                w / WpeXInfo.font_width + BB_EXPOSE_SLACK_CELLS,
                h / WpeXInfo.font_height + BB_EXPOSE_SLACK_CELLS);
}

/**
 * e_x_backbuf_restore_done - The run of lost areas is complete.
 *
 * With no buffer the marked cells are painted now, once for the whole run
 * (the buffer's copies are already on the window).
 */
void e_x_backbuf_restore_done(void)
{
 if (bb_pixmap == None)
  e_refresh();
}

/**
 * e_x_backbuf_scroll - Move painted pixels inside the buffer.
 * @sx: source left edge, in pixels
 * @sy: source top edge, in pixels
 * @dx: destination left edge, in pixels
 * @dy: destination top edge, in pixels
 * @w: width, in pixels
 * @h: height, in pixels
 *
 * Lets a scroll move the text already painted instead of painting it again.
 */
void e_x_backbuf_scroll(int sx, int sy, int dx, int dy, int w, int h)
{
 int far_x = sx > dx ? sx : dx;
 int far_y = sy > dy ? sy : dy;

 if (bb_pixmap == None || sx < 0 || sy < 0 || dx < 0 || dy < 0)
  return;
 if (far_x + w > bb_width)
  w = bb_width - far_x;
 if (far_y + h > bb_height)
  h = bb_height - far_y;
 if (w <= 0 || h <= 0)
  return;
 XCopyArea(WpeXInfo.display, bb_pixmap, bb_pixmap, WpeXInfo.gc,
   sx, sy, w, h, dx, dy);
}

/**
 * e_x_backbuf_clear_outside - Clear the window beyond the grid.
 * @win_w: the window's width, in pixels
 * @win_h: the window's height, in pixels
 * @grid_w: the width the whole cells cover, in pixels
 * @grid_h: the height the whole cells cover, in pixels
 *
 * The grid snaps down to whole cells, but under libx11-compat the window is
 * the full physical size.  That leaves a band under one cell wide at the right
 * and bottom edge that nothing draws into, and stale pixels (the previous
 * function-key bar) would show there as a second half-drawn bar.  Clearing it
 * to the window background means only real grid content appears.
 */
void e_x_backbuf_clear_outside(int win_w, int win_h, int grid_w, int grid_h)
{
 if (win_w > grid_w)
  XClearArea(WpeXInfo.display, WpeXInfo.window, grid_w, 0,
    win_w - grid_w, win_h, False);
 if (win_h > grid_h)
  XClearArea(WpeXInfo.display, WpeXInfo.window, 0, grid_h,
    win_w, win_h - grid_h, False);
}

#endif /* NO_XWINDOWS */
