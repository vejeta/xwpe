/* test_layout.c -- unit test for the box layout owner (we_layout.c).
 * Links we_layout.c, we_paint.c and we_fdloop.c only; no editor, no display,
 * always runs.  The editor's desk is played by the hooks below.
 *
 * What the user would see go wrong if a rule broke: a box on the status row
 * or the bar row (a placement), a dropdown that runs off the right edge, a
 * box under the box that opened it after a resize (the walk's order), a
 * background of the old size put back over the new desk (the generation),
 * or a resize that is lost because a draw ran another one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../we_layout.h"
#include "../we_paint.h"

static int fails;

static void chk(const char *what, int got, int want)
{
	if (got != want) {
		printf("FAIL: %s: got %d want %d\n", what, got, want);
		fails++;
	}
}

static void chk_rect(const char *what, const WpeBoxRect *got, int l, int t,
                     int r, int b)
{
	if (got->bx_left != l || got->bx_top != t || got->bx_right != r ||
	    got->bx_bottom != b) {
		printf("FAIL: %s: got %d,%d-%d,%d want %d,%d-%d,%d\n", what,
		       got->bx_left, got->bx_top, got->bx_right, got->bx_bottom,
		       l, t, r, b);
		fails++;
	}
}

static WpeBoxPlace rule(WpeBoxAnchor a, int w, int h, int xh, int yh)
{
	WpeBoxPlace p;

	memset(&p, 0, sizeof p);
	p.anchor = a;
	p.w = w;
	p.h = h;
	p.x_hint = xh;
	p.y_hint = yh;
	return p;
}

/* ---- the places ---- */

static void test_known_places(void)
{
	WpeBoxRect at;
	int fits;
	WpeBoxPlace p;

	/* About at 80x24 is where 1.5.30a drew it (8,3)-(71,20); at 60x20 it
	   is cut to the width and never reaches the status row (row 19). */
	p = rule(WPE_BOX_CENTRED_SCREEN, 64, 18, 0, 0);
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("About 80x24", &at, 8, 3, 71, 20);
	chk("About 80x24 fits", fits, 1);
	wpe_box_place(&p, 60, 20, &at, &fits);
	chk_rect("About 60x20", &at, 0, 1, 59, 18);
	chk("About 60x20 clipped", fits, 0);

	/* A dialog is centred on the window under it, as the dialog engine
	   always did: frame 40x10 on a window 0,1-79,22. */
	p = rule(WPE_BOX_CENTRED_WINDOW, 40, 10, 0, 0);
	p.has_within = 1;
	p.within.bx_left = 0;
	p.within.bx_top = 1;
	p.within.bx_right = 79;
	p.within.bx_bottom = 22;
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("dialog on a window", &at, 20, 7, 59, 16);
	p.within_is_pane = 1;
	p.within.bx_left = 40;
	p.within.bx_top = 20;
	p.within.bx_right = 79;
	p.within.bx_bottom = 22;
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("dialog over a pane is centred on the screen", &at, 20, 7, 59, 16);

	/* A dropdown keeps its column, is moved left at the right edge, and
	   says it was cut when its rows do not fit. */
	p = rule(WPE_BOX_FROM_BAR, 20, 8, 10, 0);
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("dropdown", &at, 10, 1, 29, 8);
	p = rule(WPE_BOX_FROM_BAR, 20, 8, 70, 0);
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("dropdown at the right edge", &at, 60, 1, 79, 8);
	p = rule(WPE_BOX_FROM_BAR, 20, 30, 10, 0);
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk("dropdown taller than the screen", fits, 0);
	chk("dropdown stops above the status row", at.bx_bottom, 22);

	/* A popup under the cell, above it when there is no room below. */
	p = rule(WPE_BOX_AT_TEXT, 20, 6, 10, 5);
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("popup below", &at, 9, 6, 28, 11);
	p = rule(WPE_BOX_AT_TEXT, 20, 6, 10, 21);
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("popup above", &at, 9, 15, 28, 20);

	/* A popup whose last row is the last row before the status row still
	   goes below the cell; one row more and it goes above. */
	p = rule(WPE_BOX_AT_TEXT, 20, 5, 10, 17);
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("popup ending on the last usable row", &at, 9, 18, 28, 22);
	chk("popup ending on the last usable row fits", fits, 1);
	p = rule(WPE_BOX_AT_TEXT, 20, 6, 10, 17);
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("popup one row too tall below", &at, 9, 11, 28, 16);

	/* A dialog whose corner rule applies: a frame left too narrow or too
	   short by the cut at the edge goes back to the corner. */
	p = rule(WPE_BOX_CENTRED_WINDOW, 4, 10, 0, 0);
	p.has_within = 1;
	p.within.bx_left = 75;
	p.within.bx_top = 1;
	p.within.bx_right = 79;
	p.within.bx_bottom = 22;
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("dialog too narrow goes to column 0", &at, 0, 7, 78, 16);
	p = rule(WPE_BOX_CENTRED_WINDOW, 40, 2, 0, 0);
	p.has_within = 1;
	p.within.bx_left = 0;
	p.within.bx_top = 20;
	p.within.bx_right = 79;
	p.within.bx_bottom = 22;
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("dialog too short goes to row 1", &at, 20, 1, 59, 21);
	/* The same at the edge of the rule: exactly four columns, two rows. */
	p = rule(WPE_BOX_CENTRED_WINDOW, 5, 10, 0, 0);
	p.has_within = 1;
	p.within.bx_left = 75;
	p.within.bx_top = 1;
	p.within.bx_right = 79;
	p.within.bx_bottom = 22;
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("dialog four columns wide goes to column 0", &at, 0, 7, 79, 16);
	p = rule(WPE_BOX_CENTRED_WINDOW, 40, 3, 0, 0);
	p.has_within = 1;
	p.within.bx_left = 0;
	p.within.bx_top = 20;
	p.within.bx_right = 79;
	p.within.bx_bottom = 22;
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("dialog two rows tall goes to row 1", &at, 20, 1, 59, 22);

	/* A picker: across the middle, from its own row down. */
	p = rule(WPE_BOX_TOP_CENTRED, 30, 10, 0, 2);
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("picker", &at, 25, 2, 54, 11);

	/* The screen handed to a command, the bar's highlight. */
	p = rule(WPE_BOX_FULL_SCREEN, 0, 0, 0, 0);
	wpe_box_place(&p, 100, 40, &at, &fits);
	chk_rect("full screen", &at, 0, 0, 99, 39);
	p = rule(WPE_BOX_BAR_ENTRY, 6, 1, 5, 0);
	wpe_box_place(&p, 80, 24, &at, &fits);
	chk_rect("bar entry", &at, 5, 0, 10, 0);
}

static void test_every_rule_stays_on_the_screen(void)
{
	static const int sizes[][2] = { {80, 24}, {60, 20}, {200, 60}, {30, 6} };
	static const WpeBoxAnchor anchors[] = {
		WPE_BOX_CENTRED_SCREEN, WPE_BOX_TOP_CENTRED, WPE_BOX_CENTRED_WINDOW,
		WPE_BOX_FROM_BAR, WPE_BOX_AT_TEXT, WPE_BOX_HINTED
	};
	size_t s, a;
	int hint;

	for (s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
		int scol = sizes[s][0], slns = sizes[s][1];

		for (a = 0; a < sizeof anchors / sizeof anchors[0]; a++) {
			for (hint = 0; hint < scol; hint += 7) {
				WpeBoxPlace p = rule(anchors[a], 64, 18, hint, hint % slns);
				WpeBoxRect at;
				int fits;
				char what[96];

				p.has_within = 1;
				p.within.bx_left = 0;
				p.within.bx_top = 1;
				p.within.bx_right = scol - 1;
				p.within.bx_bottom = slns - 2;
				wpe_box_place(&p, scol, slns, &at, &fits);
				snprintf(what, sizeof what, "anchor %d at %dx%d hint %d",
				         (int)anchors[a], scol, slns, hint);
				chk(what, at.bx_left >= 0 && at.bx_right < scol, 1);
				chk(what, at.bx_left <= at.bx_right, 1);
				chk(what, at.bx_top >= 1 && at.bx_bottom <= slns - 2, 1);
				chk(what, at.bx_top <= at.bx_bottom, 1);
				if (at.bx_right - at.bx_left + 1 < p.w ||
				    at.bx_bottom - at.bx_top + 1 < p.h)
					chk(what, fits, 0);
			}
		}
	}
}

/* ---- the walk ---- */

#define LOG_MAX 128
static char log_line[LOG_MAX];
static int  screen_cols = 80, screen_rows = 24;
static int  no_memory_said;
static int  dropped;
static int  reenter_in_draw;

static void log_add(const char *s)
{
	size_t n = strlen(log_line);

	snprintf(log_line + n, sizeof log_line - n, "%s", s);
}

static void desk_screen(int *c, int *r) { *c = screen_cols; *r = screen_rows; }
static void desk_relayout(int oc, int orows)
{
	char s[32];
	snprintf(s, sizeof s, "R%dx%d ", oc, orows);
	log_add(s);
}
static void desk_forget(void) { log_add("F "); }
static void desk_repaint(void) { log_add("P "); }
static void desk_drop(void *pic) { (void)pic; dropped++; }
static void desk_no_memory(const char *what) { (void)what; no_memory_said++; }

static const WpeLayoutDesk desk = {
	desk_screen, desk_relayout, desk_forget, desk_repaint, desk_drop,
	desk_no_memory
};

static WpeBoxDrawn draw_log(WpeBox *b)
{
	char s[32];

	snprintf(s, sizeof s, "D%s@%d ", (const char *)b->self, b->at.bx_left);
	log_add(s);
	if (reenter_in_draw && b->self && strcmp(b->self, "b") == 0)
		wpe_layout_screen_resized(1, 1);
	return WPE_BOX_DRAWN;
}

static WpeBoxDrawn draw_gone(WpeBox *b)
{
	(void)b;
	return WPE_BOX_GONE;
}

static WpeBoxDrawn draw_no_memory(WpeBox *b)
{
	(void)b;
	return WPE_BOX_NO_MEMORY;
}

static void make_box(WpeBox *b, const char *name, WpeBoxDraw draw)
{
	memset(b, 0, sizeof *b);
	b->what = name;
	b->self = (void *)name;
	b->draw = draw;
	b->place = rule(WPE_BOX_CENTRED_SCREEN, 20, 6, 0, 0);
	b->pic = (void *)b;       /* a saved background the walk must drop */
}

static void test_the_walk(void)
{
	WpeBox a, b, c;
	unsigned g0;

	wpe_layout_desk_set(&desk);
	make_box(&a, "a", draw_log);
	make_box(&b, "b", draw_log);
	make_box(&c, "c", draw_log);
	log_line[0] = 0;
	chk("open a", wpe_box_open(&a), WPE_BOX_DRAWN);
	chk("open b", wpe_box_open(&b), WPE_BOX_DRAWN);
	chk("open c", wpe_box_open(&c), WPE_BOX_DRAWN);
	chk("three boxes on the stack", wpe_box_depth(), 3);
	chk("the screen owner counts them too", wpe_paint_modal_depth(), 3);
	a.pic = b.pic = c.pic = (void *)&a;

	/* The grid changed: the generation moves at once, the walk is pending. */
	g0 = wpe_layout_generation();
	screen_cols = 100;
	screen_rows = 40;
	wpe_layout_note_resize(80, 24);
	chk("generation moved", wpe_layout_generation() != g0, 1);
	chk("walk pending", wpe_layout_resize_pending(), 1);
	log_line[0] = 0;
	dropped = 0;
	chk("the reader runs the walk", wpe_layout_run_pending(), 1);
	chk("no walk pending after", wpe_layout_resize_pending(), 0);
	if (strcmp(log_line, "F R80x24 P Da@40 Db@40 Dc@40 ") != 0) {
		printf("FAIL: walk order: got \"%s\"\n", log_line);
		fails++;
	}
	chk("every old background dropped, none put back", dropped, 3);
	chk("a box is told it moved", a.moved && b.moved && c.moved, 1);
	chk("a second run finds nothing", wpe_layout_run_pending(), 0);

	/* Several resizes before the walk: the windows scale from the oldest. */
	wpe_layout_note_resize(100, 40);
	wpe_layout_note_resize(120, 50);
	log_line[0] = 0;
	wpe_layout_run_pending();
	chk("the oldest size is kept", strstr(log_line, "R100x40") != NULL, 1);

	/* A draw that starts a resize does not re-enter the walk. */
	reenter_in_draw = 1;
	wpe_layout_note_resize(100, 40);
	log_line[0] = 0;
	wpe_layout_run_pending();
	reenter_in_draw = 0;
	chk("the nested resize is left pending", wpe_layout_resize_pending(), 1);
	log_line[0] = 0;
	wpe_layout_run_pending();
	chk("and runs at the next reader pass", strstr(log_line, "Dc@") != NULL, 1);

	wpe_box_close(&c);
	wpe_box_close(&b);
	wpe_box_close(&a);
	chk("all closed", wpe_box_depth(), 0);
	chk("the screen owner is back", wpe_paint_modal_depth(), 0);
}

static void test_gone_and_no_memory(void)
{
	WpeBox a, d, m;

	wpe_layout_desk_set(&desk);
	make_box(&a, "a", draw_log);
	make_box(&d, "d", draw_gone);
	chk("open a", wpe_box_open(&a), WPE_BOX_DRAWN);
	chk("a box that is gone on its first draw", wpe_box_open(&d), WPE_BOX_GONE);
	chk("it is not left open", wpe_box_depth(), 1);
	chk("nor on the screen owner", wpe_paint_modal_depth(), 1);

	/* A box whose draw is gone only at the walk leaves the stack and its
	   loop closes it afterwards. */
	d.draw = draw_log;
	chk("open d", wpe_box_open(&d), WPE_BOX_DRAWN);
	d.draw = draw_gone;
	wpe_layout_note_resize(80, 24);
	wpe_layout_run_pending();
	chk("gone at the walk: off the stack", wpe_box_depth(), 1);
	chk("but the screen owner still counts it", wpe_paint_modal_depth(), 2);
	wpe_box_close(&d);
	chk("closing it by its loop leaves the owner", wpe_paint_modal_depth(), 1);
	wpe_box_close(&d);
	chk("a second close is ignored", wpe_paint_modal_depth(), 1);

	/* Out of memory in a draw is reported once, after the walk. */
	make_box(&m, "m", draw_log);
	chk("open m", wpe_box_open(&m), WPE_BOX_DRAWN);
	m.draw = draw_no_memory;
	no_memory_said = 0;
	wpe_layout_note_resize(80, 24);
	wpe_layout_run_pending();
	chk("no memory said once", no_memory_said, 1);
	wpe_box_close(&m);
	wpe_box_close(&a);
	chk("all closed", wpe_paint_modal_depth(), 0);
}

static void test_the_walk_without_a_graphical_display(void)
{
	static const WpeLayoutDesk bare = {
		desk_screen, NULL, NULL, NULL, NULL, NULL
	};
	WpeBox a;

	/* The terminal fills no keep-view hook and a build without the walk's
	   hooks must not call a slot that is NULL. */
	wpe_layout_desk_set(&bare);
	make_box(&a, "a", draw_log);
	a.pic = NULL;
	chk("open", wpe_box_open(&a), WPE_BOX_DRAWN);
	wpe_layout_note_resize(80, 24);
	chk("walks with NULL slots", wpe_layout_run_pending(), 1);
	wpe_box_close(&a);
}

/* @scenario SC-box-04 */
int main(void)
{
	test_known_places();
	test_every_rule_stays_on_the_screen();
	test_the_walk();
	test_gone_and_no_memory();
	test_the_walk_without_a_graphical_display();
	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("test_layout: ok\n");
	return 0;
}
