/* test_xbackbuf.c -- unit test for the X11 back buffer owner (we_xbackbuf.c).
 * Links we_xbackbuf.c only; no X server, no display, always runs.  Xlib is a
 * recorder: each call the owner makes lands in a log this file reads back.
 *
 * What the user would see go wrong if a rule broke: an editor that ends with
 * BadDrawable (a copy from a pixmap that does not exist), a cairo or Xft
 * surface still wrapping a pixmap that was freed, a picture that is carried
 * over before the surface that painted it was flushed, or a window that is
 * not repainted after another window uncovered it.
 *
 * The log letters: R a release hook ran, P XCreatePixmap, C XCopyArea,
 * B a back hook ran, F XFreePixmap, A e_refresh_area, W e_refresh,
 * L XClearArea.
 *
 * The cases:
 *   absent         no X call from present, scroll or restore; restore asks
 *                  for a repaint of the cells and restore_done refreshes
 *   create         a buffer made from absent: release, create, back
 *   order          a remake at a new size: every release, create, the carry,
 *                  every back, free (and no carry when it is not asked for)
 *   same size      a remake at the size it has makes no X call
 *   present        a buffer copies; restore_done does not refresh
 *   clip           a copy never runs past the buffer
 *   drop           a wrapper that was dropped is not called
 *   lost           a server that is gone gets no request from present,
 *                  remake, scroll, clear_outside or restore
 *   lost mid-remake a loss found by the carry ends the remake there: no
 *                  wrapper is asked to wrap the new buffer and the old
 *                  pixmap is not freed
 */
#include <stdio.h>
#include "linebuf.h"
#include <string.h>
#include <X11/Xlib.h>

#include "../edit.h"
#include "../WeXterm.h"
#include "../we_xbackbuf.h"

WpeXStruct WpeXInfo;

static int fails;

static void chk_str(const char *what, const char *got, const char *want)
{
	if (strcmp(got, want) != 0) {
		printf("FAIL: %s: got \"%s\" want \"%s\"\n", what, got, want);
		fails++;
	}
}

static void chk(const char *what, int got, int want)
{
	if (got != want) {
		printf("FAIL: %s: got %d want %d\n", what, got, want);
		fails++;
	}
}

/* ---- the recorder ---- */

#define LOG_MAX 64
static char xlog[LOG_MAX];
static int nlog;
static int last_copy[8];          /* src x y w h, of the last XCopyArea */
static int last_back_w, last_back_h;
/* Armed by a case: the next XCopyArea is the call inside which Xlib finds the
   connection broken, so it sets the flag the owner of the connection reads. */
static int copy_ends_the_server;

/* The owner of the X11 connection lives in WeXterm.c, which this test does
   not link: it is the flag the owner reads, and the test is the server. */
int e_x_conn_usable(const char *what)
{
	(void)what;
	return !WpeXInfo.lost;
}

int e_x_conn_step_usable(void)
{
	return !WpeXInfo.lost;
}

static void rec(char c)
{
	if (nlog < LOG_MAX - 1)
		xlog[nlog++] = c;
	xlog[nlog] = 0;
}

static void reset_log(void)
{
	nlog = 0;
	xlog[0] = 0;
}

static unsigned long next_pixmap = 100;

Pixmap XCreatePixmap(Display *d, Drawable w, unsigned int width,
		     unsigned int height, unsigned int depth)
{
	(void)d; (void)w; (void)width; (void)height; (void)depth;
	rec('P');
	return next_pixmap++;
}

int XFreePixmap(Display *d, Pixmap p)
{
	(void)d; (void)p;
	rec('F');
	return 0;
}

int XCopyArea(Display *d, Drawable src, Drawable dst, GC gc, int sx, int sy,
	      unsigned int w, unsigned int h, int dx, int dy)
{
	(void)d; (void)src; (void)dst; (void)gc; (void)dx; (void)dy;
	rec('C');
	last_copy[0] = sx;
	last_copy[1] = sy;
	last_copy[2] = (int)w;
	last_copy[3] = (int)h;
	if (copy_ends_the_server) {
		copy_ends_the_server = 0;
		WpeXInfo.lost = 1;
	}
	return 0;
}

int XSetForeground(Display *d, GC gc, unsigned long pixel)
{
	(void)d; (void)gc; (void)pixel;
	return 0;
}

int XFillRectangle(Display *d, Drawable w, GC gc, int x, int y,
		   unsigned int width, unsigned int height)
{
	(void)d; (void)w; (void)gc; (void)x; (void)y; (void)width; (void)height;
	return 0;
}

int XClearArea(Display *d, Window w, int x, int y, unsigned int width,
	       unsigned int height, Bool exposures)
{
	(void)d; (void)w; (void)x; (void)y; (void)width; (void)height;
	(void)exposures;
	rec('L');
	return 0;
}

int XDefaultDepth(Display *d, int screen)
{
	(void)d; (void)screen;
	return 24;
}

void e_refresh_area(int x, int y, int width, int height)
{
	(void)x; (void)y; (void)width; (void)height;
	rec('A');
}

static int fake_refresh(void)
{
	rec('W');
	return 0;
}

int (*e_u_refresh)(void) = fake_refresh;

/* ---- the two wrappers ---- */

static void release_one(void) { rec('R'); }
static void back_one(int w, int h) { rec('B'); last_back_w = w; last_back_h = h; }
static void release_two(void) { rec('R'); }
static void back_two(int w, int h) { (void)w; (void)h; rec('B'); }

static void case_absent(void)
{
	chk("starts with no buffer", e_x_backbuf_exists(), 0);
	reset_log();
	e_x_backbuf_present(0, 0, 10, 10);
	e_x_backbuf_scroll(0, 0, 5, 5, 10, 10);
	e_x_backbuf_restore(0, 0, 90, 36);
	chk_str("absent: no copy, restore asks for cells", xlog, "A");
	reset_log();
	e_x_backbuf_restore_done();
	chk_str("absent: restore_done refreshes", xlog, "W");
}

static void case_create(void)
{
	reset_log();
	e_x_backbuf_remake(800, 600, 1, 0);
	chk_str("create from absent: release, create, back (no carry, no free)",
		xlog, "RRPBB");
	chk("created", e_x_backbuf_exists(), 1);
	chk("back hook got the width", last_back_w, 800);
	chk("back hook got the height", last_back_h, 600);
}

static void case_order(void)
{
	reset_log();
	e_x_backbuf_remake(640, 480, 1, 0);
	chk_str("order with carry", xlog, "RRPCBBF");
	reset_log();
	e_x_backbuf_remake(700, 500, 0, 0);
	chk_str("order without carry", xlog, "RRPBBF");
}

static void case_same_size(void)
{
	reset_log();
	e_x_backbuf_remake(700, 500, 1, 0);
	chk_str("same size makes no X call", xlog, "");
}

static void case_present(void)
{
	reset_log();
	e_x_backbuf_present(0, 0, 10, 10);
	e_x_backbuf_scroll(0, 0, 5, 5, 10, 10);
	e_x_backbuf_restore(0, 0, 10, 10);
	chk_str("present, scroll and restore copy", xlog, "CCC");
	reset_log();
	e_x_backbuf_restore_done();
	chk_str("restore_done does not refresh with a buffer", xlog, "");
}

static void case_clip(void)
{
	reset_log();
	e_x_backbuf_present(600, 400, 500, 500);
	chk_str("clipped copy still made", xlog, "C");
	chk("clipped to the width", last_copy[2], 100);
	chk("clipped to the height", last_copy[3], 100);
	reset_log();
	e_x_backbuf_present(700, 0, 10, 10);
	chk_str("a copy outside the buffer is not made", xlog, "");
}

static void case_drop(void)
{
	e_x_backbuf_hooks_drop(release_two);
	reset_log();
	e_x_backbuf_remake(720, 520, 0, 0);
	chk_str("a dropped wrapper is not called", xlog, "RPBF");
}

/* The buffer is 720x520 here and one wrapper is left (release_two was
   dropped), so a remake that carries the picture logs R P C B F. */
static void case_lost(void)
{
	WpeXInfo.lost = 1;
	reset_log();
	e_x_backbuf_present(0, 0, 10, 10);
	e_x_backbuf_remake(600, 400, 1, 0);
	e_x_backbuf_scroll(0, 0, 5, 5, 10, 10);
	e_x_backbuf_clear_outside(700, 500, 640, 480);
	e_x_backbuf_restore(0, 0, 10, 10);
	chk_str("lost: present, remake, scroll, clear and restore send nothing",
		xlog, "");
	WpeXInfo.lost = 0;
	reset_log();
	e_x_backbuf_remake(720, 520, 1, 0);
	chk_str("lost: the refused remake left the buffer's size alone", xlog, "");
}

static void case_lost_mid_remake(void)
{
	reset_log();
	e_x_backbuf_remake(620, 420, 1, 0);
	chk_str("a remake that carries the picture", xlog, "RPCBF");
	copy_ends_the_server = 1;
	reset_log();
	e_x_backbuf_remake(640, 440, 1, 0);
	chk_str("a loss the carry found ends the remake there: no wrapper is"
		" asked back and the old pixmap is not freed", xlog, "RPC");
	chk("the carry was the call that found the loss", WpeXInfo.lost, 1);
	WpeXInfo.lost = 0;
}

int main(void)
{
	wpe_grid_take(80, 30);
	WpeXInfo.font_width = 9;
	WpeXInfo.font_height = 18;
	e_x_backbuf_hooks_add(release_one, back_one);
	e_x_backbuf_hooks_add(release_two, back_two);
	case_absent();
	case_create();
	case_order();
	case_same_size();
	case_present();
	case_clip();
	case_drop();
	case_lost();
	case_lost_mid_remake();
	if (fails) {
		printf("test_xbackbuf: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_xbackbuf: ok\n");
	return 0;
}
