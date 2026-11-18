/* we_xbackbuf.h -- the one owner of the X11 back buffer.
 *
 * Private to the X11 translation units (we_xterm.c, WeXterm.c,
 * we_render_cairo.c).  Nothing else creates, frees or copies the pixmap the
 * editor paints a frame into: this file does, and its handle is private to
 * we_xbackbuf.c, so the compiler refuses every bypass in every build.
 *
 * What the user gets: a window that always shows a finished frame (never a
 * half-drawn one), that is given its picture back when another window
 * uncovers it, that keeps what was painted while it is resized, and an editor
 * that starts and survives a resize also on a machine where Xft finds no font
 * (the editor then draws with the core font, straight into the window).
 *
 * Invariant: the window is only ever given pixels by this file or by a
 * renderer painting into e_x_backbuf_target(); a blit never names a pixmap
 * that does not exist; a blit never runs past the buffer.
 *
 * States: ABSENT (no buffer; the target is the window, the core font path) and
 * PRESENT (a buffer of a known size).  ABSENT can become PRESENT at run time
 * (a font change under libx11-compat finds an Xft font the first start did
 * not), so a caller asks e_x_backbuf_exists() instead of remembering how the
 * editor started.
 *
 * Whoever wraps the buffer (the Xft draw, the cairo surface) registers a
 * release/back pair, and e_x_backbuf_remake runs it in one order: every
 * release, the new buffer, the picture carried over, every back, the old
 * buffer freed.  No caller destroys or rewraps its drawable by hand.
 */
#ifndef WE_XBACKBUF_H
#define WE_XBACKBUF_H

#ifndef NO_XWINDOWS

#include <X11/Xlib.h>

/* The most wrappers (the Xft draw, the cairo surface) that can be registered. */
#define E_X_BACKBUF_HOOKS_MAX 4

/* The buffer is about to be replaced: give back what wraps it. */
typedef void (*EXBackbufRelease)(void);
/* The new buffer exists, at this size: wrap it again. */
typedef void (*EXBackbufBack)(int pixel_w, int pixel_h);

/* Whether the editor has a back buffer now (the one question every dispatch
   between the buffered and the core-font path asks). */
int e_x_backbuf_exists(void);

/* Register the release/back pair of something that wraps the buffer.  A pair
   with the same release function replaces the old one. */
void e_x_backbuf_hooks_add(EXBackbufRelease release, EXBackbufBack back);
/* Forget the pair whose release function this is. */
void e_x_backbuf_hooks_drop(EXBackbufRelease release);

/* Make the buffer pixel_w by pixel_h, starting in fill_pixel, and carry the
   old picture over when keep_picture is set.  At the size the buffer already
   has it does nothing and keeps the picture. */
void e_x_backbuf_remake(int pixel_w, int pixel_h, int keep_picture,
                        unsigned long fill_pixel);

/* Where a renderer paints: the buffer when there is one, else the window. */
Drawable e_x_backbuf_target(void);

/* "I painted this area, show it": copy it to the window.  Without a buffer
   there is nothing to copy (the painting went to the window). */
void e_x_backbuf_present(int x, int y, int w, int h);

/* "The window lost this area" (an Expose): copy it from the buffer, or, with
   no buffer, repaint those cells from the screen cells.  A caller that gave a
   run of rectangles then calls e_x_backbuf_restore_done once. */
void e_x_backbuf_restore(int x, int y, int w, int h);
void e_x_backbuf_restore_done(void);

/* Move painted pixels inside the buffer (a scroll). */
void e_x_backbuf_scroll(int sx, int sy, int dx, int dy, int w, int h);

/* Clear the window outside the grid's pixels (the sub-cell band at the right
   and bottom edge), so only real grid content shows there. */
void e_x_backbuf_clear_outside(int win_w, int win_h, int grid_w, int grid_h);

#endif /* NO_XWINDOWS */

#endif
