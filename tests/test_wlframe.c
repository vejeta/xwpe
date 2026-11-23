/* test_wlframe.c -- unit test for the Wayland frame owner (we_wlframe.c).
 * Links we_wlframe.c only; no compositor, no display, always runs.  The
 * compositor is a recorder: what the owner would send (acknowledge, attach
 * and commit) and what it would allocate land in an event list this file
 * reads back.
 *
 * What the user would see go wrong if a rule broke: a window that the
 * compositor closes (a buffer larger than a fullscreen bound, or not equal
 * to a maximized one), a window that stays black after a small resize, a
 * window dragged smaller that keeps showing the old size, or an editor that
 * commits a frame of one size against a configure of another while a drag
 * streams new sizes.
 *
 * The cases:
 *   fit table        the buffer size per window state (wpe_wlframe_fit_size)
 *   W-A sequence     configure, apply, a configure that arrives inside the
 *                    relayout, present: the commit that answers an
 *                    acknowledgement always fits it
 *   queue            a configure is invisible until the owner drains it
 *   state only       a focus change is answered at once, a size change waits
 *   0x0 path         a compositor that leaves the size to the editor
 *   new buffer       a buffer replaced without a grid change is re-drawn
 *                    before it is shown
 *   no buffer        a refused allocation leaves the configure pending
 *   wire size        a window whose pixels no wl_shm buffer can carry is
 *                    refused before any memory is asked for
 *   ceiling          a window wider than the grid's ceiling carries the
 *                    ceiling's grid, and the rest is remainder
 *   lone event       a read that brings only a frame-queue event returns at
 *                    once (a real libwayland connection over a socket pair)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>

#include "../we_wayland.h"
#include "../we_render.h"
#include "../we_wlframe.h"
#include "../we_grid.h"
#include "test_watchdog.h"

#define CELL_W 9
#define CELL_H 18
#define OWN_W (80 * CELL_W)
#define OWN_H (30 * CELL_H)

/* The two globals the owner reads. */
WpeWlInfo WpeWl;
WpeRenderBackend WpeRender;

/* The connection section of the editor is not linked here; the owner's real
   commit path is replaced by the recorder, so nothing flushes. */
void wpe_wl_conn_flush(void)
{
}

static int fails;

static void chk(const char *what, int got, int want)
{
	if (got != want) {
		printf("FAIL: %s: got %d want %d\n", what, got, want);
		fails++;
	}
}

/* ---- the recorder ---- */

enum { EV_ALLOC = 'N', EV_ACK = 'A', EV_COMMIT = 'C', EV_REPAINT = 'R' };

struct ev {
	int kind;
	int a;   /* ack: serial; commit and alloc: buffer width */
	int b;   /* commit and alloc: buffer height */
};

#define MAX_EV 64
static struct ev log_[MAX_EV];
static int nlog;
static int refuse_alloc;
static int promise_pixels;

static void rec(int kind, int a, int b)
{
	if (nlog < MAX_EV) {
		log_[nlog].kind = kind;
		log_[nlog].a = a;
		log_[nlog].b = b;
		nlog++;
	}
}

static int count(int kind)
{
	int i, n = 0;

	for (i = 0; i < nlog; i++)
		if (log_[i].kind == kind)
			n++;
	return n;
}

/* The index of the k-th (from 0) event of a kind, or -1. */
static int nth(int kind, int k)
{
	int i;

	for (i = 0; i < nlog; i++)
		if (log_[i].kind == kind && k-- == 0)
			return i;
	return -1;
}

static int fake_alloc(int w, int h)
{
	if (refuse_alloc)
		return -1;
	if (promise_pixels) {
		/* The sizes the wire-size row asks for are ones no system would
		   map: answer yes without asking, so what the row sees is the
		   owner's own refusal and not the system's. */
		WpeWl.width = w;
		WpeWl.height = h;
		rec(EV_ALLOC, w, h);
		return 0;
	}
	free(WpeWl.pixels);
	WpeWl.pixels = calloc((size_t)w * h, sizeof(uint32_t));
	WpeWl.width = w;
	WpeWl.height = h;
	WpeWl.stride = w * 4;
	rec(EV_ALLOC, w, h);
	return WpeWl.pixels ? 0 : -1;
}

static void fake_ack(uint32_t serial)
{
	rec(EV_ACK, (int)serial, 0);
}

static void fake_frame(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
	rec(EV_COMMIT, WpeWl.width, WpeWl.height);
}

static void fake_retarget(int w, int h)
{
	(void)w; (void)h;
}

static void fake_repaint(void)
{
	rec(EV_REPAINT, 0, 0);
}

/* The frame queue: configures wait here until the owner drains it. */
struct cfg {
	int w, h;
	unsigned states;   /* WPE_WLFRAME_ST_* bits */
	uint32_t serial;
};
static struct cfg queue_[16];
static int nqueue;

#define FULLSCREEN_ACTIVE (WPE_WLFRAME_ST_FULLSCREEN | WPE_WLFRAME_ST_ACTIVATED)
#define ACTIVE_ONLY WPE_WLFRAME_ST_ACTIVATED
#define DRAGGING WPE_WLFRAME_ST_RESIZING

static void queue_configure(int w, int h, unsigned states, uint32_t serial)
{
	queue_[nqueue].w = w;
	queue_[nqueue].h = h;
	queue_[nqueue].states = states;
	queue_[nqueue].serial = serial;
	nqueue++;
}

/* Add one state to a configure's states array. */
static void add_state(struct wl_array *states, unsigned wanted, unsigned bit,
                      uint32_t value)
{
	uint32_t *p;

	if (!(wanted & bit))
		return;
	p = wl_array_add(states, sizeof *p);
	*p = value;
}

/* Deliver one configure the way the compositor's listeners do. */
static void deliver(const struct cfg *c)
{
	struct wl_array states;

	wl_array_init(&states);
	add_state(&states, c->states, WPE_WLFRAME_ST_FULLSCREEN, XDG_TOPLEVEL_STATE_FULLSCREEN);
	add_state(&states, c->states, WPE_WLFRAME_ST_MAXIMIZED, XDG_TOPLEVEL_STATE_MAXIMIZED);
	add_state(&states, c->states, WPE_WLFRAME_ST_RESIZING, XDG_TOPLEVEL_STATE_RESIZING);
	add_state(&states, c->states, WPE_WLFRAME_ST_ACTIVATED, XDG_TOPLEVEL_STATE_ACTIVATED);
	wpe_wlframe_note_toplevel(c->w, c->h, &states);
	wpe_wlframe_note_configure(c->serial);
	wl_array_release(&states);
}

static void fake_drain(void)
{
	int i;

	for (i = 0; i < nqueue; i++)
		deliver(&queue_[i]);
	nqueue = 0;
}

static const WpeWlFrameOps recorder = {
	fake_alloc, fake_ack, fake_frame, fake_retarget, fake_repaint, fake_drain
};

static void fresh(void)
{
	free(WpeWl.pixels);
	memset(&WpeWl, 0, sizeof WpeWl);
	WpeWl.shm_fd = -1;
	memset(&WpeRender, 0, sizeof WpeRender);
	nlog = 0;
	nqueue = 0;
	refuse_alloc = 0;
	promise_pixels = 0;
	wpe_wlframe_init(OWN_W, OWN_H, CELL_W, CELL_H);
	wpe_wlframe_use_ops(&recorder);
}

static void dummy_text(int x, int y, const char *utf8, int u8len,
                       int cell_width, int fg_idx, int bg_idx)
{
	(void)x; (void)y; (void)utf8; (void)u8len;
	(void)cell_width; (void)fg_idx; (void)bg_idx;
}

/* Make the editor's renderer look installed, so a new buffer is re-drawn. */
static void renderer_up(void)
{
	WpeRender.draw_text = dummy_text;
	WpeRender.font_width = CELL_W;
	WpeRender.font_height = CELL_H;
}

/* ---- the fit table ---- */

static void fit(const char *what, int bw, int bh, unsigned st, int ww, int wh)
{
	int w = -1, h = -1;
	char name[96];

	wpe_wlframe_fit_size(bw, bh, st, CELL_W, CELL_H, OWN_W, OWN_H, &w, &h);
	snprintf(name, sizeof name, "%s width", what);
	chk(name, w, ww);
	snprintf(name, sizeof name, "%s height", what);
	chk(name, h, wh);
}

static void test_fit_table(void)
{
	const unsigned none = 0;
	const unsigned full = WPE_WLFRAME_ST_FULLSCREEN | WPE_WLFRAME_ST_ACTIVATED;
	const unsigned maxi = WPE_WLFRAME_ST_MAXIMIZED;
	const unsigned drag = WPE_WLFRAME_ST_RESIZING;

	fit("fullscreen is the bound", 1000, 750, full, 1000, 750);
	fit("maximized is the bound", 1003, 757, maxi, 1003, 757);
	fit("maximized and resizing is the bound", 1003, 757, maxi | drag, 1003, 757);
	fit("drag snaps to whole cells", 1000, 750, drag, 999, 738);
	fit("drag above the floor", 300, 200, drag, 297, 198);
	fit("drag below the floor is the bound", 200, 100, drag, 200, 100);
	fit("drag below the floor, one axis", 200, 750, drag, 200, 738);
	fit("floating snaps", 1000, 750, none, 999, 738);
	fit("floating below the floor gets the floor", 100, 50, none, 270, 108);
	fit("no suggestion is the editor's own", 0, 0, none, OWN_W, OWN_H);
	fit("width only is no suggestion", 800, 0, none, OWN_W, OWN_H);
	fit("height only is no suggestion", 0, 600, full, OWN_W, OWN_H);

	{
		int w, h;

		wpe_wlframe_fit_size(500, 400, none, 0, 0, OWN_W, OWN_H, &w, &h);
		chk("no cell yet: the bound, width", w, 500);
		chk("no cell yet: the bound, height", h, 400);
	}
}

/* ---- the sequences ---- */

/* Every commit after an acknowledgement has a buffer of the size that
   acknowledged configure offered: the exact rule the wire log is checked
   against in the Wayland suite. */
static void check_commits_fit(const char *what, const int *serials,
                              const int *w, const int *h, int n)
{
	int i, k, last = -1;
	char name[96];

	for (i = 0; i < nlog; i++) {
		if (log_[i].kind == EV_ACK) {
			last = -1;
			for (k = 0; k < n; k++)
				if (serials[k] == log_[i].a)
					last = k;
			snprintf(name, sizeof name, "%s: ack %d is a known configure", what, log_[i].a);
			chk(name, last >= 0, 1);
		} else if (log_[i].kind == EV_COMMIT && last >= 0) {
			snprintf(name, sizeof name, "%s: commit width after ack of %d", what, serials[last]);
			chk(name, log_[i].a, w[last]);
			snprintf(name, sizeof name, "%s: commit height after ack of %d", what, serials[last]);
			chk(name, log_[i].b, h[last]);
		}
	}
}

static void test_wa_sequence(void)
{
	int cols, rows;
	int serials[] = { 1, 2, 3 };
	int ws[] = { 1000, 968, 984 };
	int hs[] = { 750, 726, 731 };
	struct cfg nested = { 984, 731, FULLSCREEN_ACTIVE, 3 };

	fresh();                        /* bring-up: no renderer yet */
	queue_configure(1000, 750, FULLSCREEN_ACTIVE, 1);
	chk("first apply sees the queued configure", wpe_wlframe_apply(&cols, &rows), 1);
	chk("first grid columns", cols, 111);
	chk("first grid rows", rows, 41);
	chk("fitted, nothing committed yet", count(EV_COMMIT), 0);
	chk("fitted, nothing acknowledged yet", count(EV_ACK), 0);
	wpe_wlframe_finish();
	chk("one ack", count(EV_ACK), 1);
	chk("one commit", count(EV_COMMIT), 1);
	chk("the ack comes before the commit", nth(EV_ACK, 0) < nth(EV_COMMIT, 0), 1);
	chk("shown", wpe_wlframe_shown(), 1);

	/* A drag: the next configure, applied, then a nested one inside the relayout. */
	renderer_up();
	queue_configure(968, 726, FULLSCREEN_ACTIVE, 2);
	chk("second apply changes the grid", wpe_wlframe_apply(&cols, &rows), 1);
	deliver(&nested);               /* a configure read inside the relayout's repaint */
	wpe_wlframe_present(0, 0, 968, 726);
	chk("a present with a newer configure waiting is withheld", wpe_wlframe_withheld(), 1);
	chk("nothing was committed at the old size", count(EV_COMMIT), 1);
	chk("the configure is still pending", wpe_wlframe_pending(), 1);
	wpe_wlframe_apply(&cols, &rows);
	wpe_wlframe_finish();
	chk("the newest configure was acknowledged", log_[nth(EV_ACK, 1)].a, 3);
	chk("two acks, serial 2 was skipped", count(EV_ACK), 2);
	chk("two commits", count(EV_COMMIT), 2);
	check_commits_fit("W-A", serials, ws, hs, 3);
}

static void test_queue_is_invisible_until_drained(void)
{
	int cols, rows;

	fresh();
	chk("no configure yet: apply does nothing", wpe_wlframe_apply(&cols, &rows), 0);
	chk("unbound", wpe_wlframe_state(), WPE_WLFRAME_UNBOUND);
	queue_configure(1000, 750, FULLSCREEN_ACTIVE, 1);
	chk("queued but not drained: still unbound", wpe_wlframe_pending(), 0);
	wpe_wlframe_serve();
	chk("serve drains and takes a size change as pending", wpe_wlframe_pending(), 1);
	chk("serve committed nothing for a size change", count(EV_COMMIT), 0);
	chk("serve acknowledged nothing for a size change", count(EV_ACK), 0);
	chk("the key loop applies it", wpe_wlframe_apply(&cols, &rows), 1);
}

static void test_state_only_configure(void)
{
	int cols, rows;
	int before_alloc;

	fresh();
	queue_configure(1000, 750, FULLSCREEN_ACTIVE, 1);
	wpe_wlframe_apply(&cols, &rows);
	wpe_wlframe_finish();
	before_alloc = count(EV_ALLOC);
	queue_configure(1000, 750, FULLSCREEN_ACTIVE, 2);   /* a focus change: same size */
	wpe_wlframe_serve();
	chk("a state-only configure is answered at once", count(EV_ACK), 2);
	chk("and the frame is shown again", count(EV_COMMIT), 2);
	chk("without a new buffer", count(EV_ALLOC), before_alloc);
	chk("not pending any more", wpe_wlframe_pending(), 0);
	queue_configure(900, 700, FULLSCREEN_ACTIVE, 3);    /* a size change during a wait */
	wpe_wlframe_serve();
	chk("a size change is left for the key loop", wpe_wlframe_pending(), 1);
	chk("and not acknowledged", count(EV_ACK), 2);
	chk("the key loop takes it", wpe_wlframe_apply(&cols, &rows), 1);
	wpe_wlframe_finish();
	chk("then it is acknowledged", count(EV_ACK), 3);
}

static void test_zero_by_zero(void)
{
	int cols, rows;

	fresh();
	queue_configure(0, 0, 0, 1);
	chk("a 0x0 configure keeps the editor's grid", wpe_wlframe_apply(&cols, &rows), 0);
	chk("the buffer is the editor's own width", WpeWl.width, OWN_W);
	chk("the buffer is the editor's own height", WpeWl.height, OWN_H);
	chk("it was answered in the same step: ack", count(EV_ACK), 1);
	chk("it was answered in the same step: commit", count(EV_COMMIT), 1);
	chk("the ack carries the serial", log_[nth(EV_ACK, 0)].a, 1);
}

static void test_new_buffer_is_redrawn(void)
{
	int cols, rows;

	fresh();
	queue_configure(1000, 750, FULLSCREEN_ACTIVE, 1);
	wpe_wlframe_apply(&cols, &rows);
	wpe_wlframe_finish();
	renderer_up();
	nlog = 0;
	queue_configure(1003, 752, FULLSCREEN_ACTIVE, 2);   /* a few pixels more: same grid */
	chk("same grid", wpe_wlframe_apply(&cols, &rows), 0);
	chk("a new buffer was made", count(EV_ALLOC), 1);
	chk("every cell was drawn again", count(EV_REPAINT), 1);
	chk("drawn before it was shown", nth(EV_REPAINT, 0) < nth(EV_COMMIT, 0), 1);
	chk("then acknowledged and committed", count(EV_ACK) + count(EV_COMMIT), 2);
}

static void test_refused_buffer_stays_pending(void)
{
	int cols, rows;

	fresh();
	refuse_alloc = 1;
	queue_configure(1000, 750, FULLSCREEN_ACTIVE, 1);
	chk("no buffer: nothing applied", wpe_wlframe_apply(&cols, &rows), 0);
	chk("still pending", wpe_wlframe_pending(), 1);
	chk("nothing committed", count(EV_COMMIT), 0);
	refuse_alloc = 0;
	chk("the next pass takes it", wpe_wlframe_apply(&cols, &rows), 1);
}

static void test_below_the_floor(void)
{
	int cols, rows;

	fresh();
	queue_configure(200, 100, DRAGGING, 1);
	wpe_wlframe_apply(&cols, &rows);
	wpe_wlframe_finish();
	chk("a drag below the floor: the buffer is the window, width", WpeWl.width, 200);
	chk("a drag below the floor: the buffer is the window, height", WpeWl.height, 100);
	chk("the grid keeps its floor, columns", cols, WPE_WLFRAME_MIN_COLS);
	chk("the grid keeps its floor, rows", rows, WPE_WLFRAME_MIN_ROWS);
}

/* ---- a buffer the wire protocol cannot carry ---- */

/* The pool's size and the buffer's stride are an int32 on the wire, so a
   window of 46341 x 46341 pixels (8590320684 bytes) has no buffer, and one
   600000000 pixels wide has no stride either (2400000000).  What the user
   sees when such a size is asked for: the window keeps the frame it has, the
   compositor is never answered with a buffer that does not fit its
   configure, and the next top-level point asks again -- the way of a buffer
   the system refused.  Before the size was counted in size_t the owner asked
   for the buffer and put a truncated count on the wire. */
#define WIRE_BYTES_W 46341
#define WIRE_BYTES_H 46341
#define WIRE_STRIDE_W 600000000
#define WIRE_STRIDE_H 100
#define WIRE_FITTING_W 20000
#define WIRE_FITTING_H 20000

static void check_oversize_is_refused(const char *what, int w, int h)
{
	int cols, rows;
	char name[96];

	fresh();
	promise_pixels = 1;
	queue_configure(w, h, FULLSCREEN_ACTIVE, 1);
	snprintf(name, sizeof name, "%s: the size is refused", what);
	chk(name, wpe_wlframe_apply(&cols, &rows), 0);
	snprintf(name, sizeof name, "%s: no memory was asked for", what);
	chk(name, count(EV_ALLOC), 0);
	snprintf(name, sizeof name, "%s: the configure stays pending", what);
	chk(name, wpe_wlframe_pending(), 1);
	snprintf(name, sizeof name, "%s: nothing was committed", what);
	chk(name, count(EV_COMMIT), 0);
	snprintf(name, sizeof name, "%s: nothing was acknowledged", what);
	chk(name, count(EV_ACK), 0);
}

static void test_a_buffer_the_protocol_cannot_carry_is_refused(void)
{
	int cols, rows;

	check_oversize_is_refused("the byte count passes int32",
	                          WIRE_BYTES_W, WIRE_BYTES_H);
	check_oversize_is_refused("the stride alone passes int32",
	                          WIRE_STRIDE_W, WIRE_STRIDE_H);

	/* A large buffer the wire does carry is still made: what is refused is
	   the protocol's limit, not a large window. */
	fresh();
	promise_pixels = 1;
	queue_configure(WIRE_FITTING_W, WIRE_FITTING_H, FULLSCREEN_ACTIVE, 1);
	chk("a buffer inside int32 is taken", wpe_wlframe_apply(&cols, &rows), 1);
	chk("a buffer inside int32 was asked for", count(EV_ALLOC), 1);
	chk("a buffer inside int32: the width asked for",
	    log_[nth(EV_ALLOC, 0)].a, WIRE_FITTING_W);
	chk("a buffer inside int32: the height asked for",
	    log_[nth(EV_ALLOC, 0)].b, WIRE_FITTING_H);
}

/* ---- the ceiling a buffer carries ---- */

/* A window wide enough for 4200 cells: the grid the editor lays out stops at
   the grid owner's ceiling and the strip past it is the frame's remainder.
   No live row can show this in the Wayland suite, whose compositor runs a
   kiosk shell on the X11 backend: the output is then an X window on a
   1024 x 768 screen, and a fullscreen surface larger than its output is a
   protocol error the compositor answers by disconnecting the editor. */
#define CEILING_ROWS 6
#define CEILING_BUFFER_W ((WPE_GRID_MAX_COLS + 104) * CELL_W)
#define CEILING_BUFFER_H (CEILING_ROWS * CELL_H)

static void test_a_buffer_wider_than_the_ceiling_carries_the_ceiling(void)
{
	int cols = -1, rows = -1;

	fresh();
	queue_configure(CEILING_BUFFER_W, CEILING_BUFFER_H, FULLSCREEN_ACTIVE, 1);
	chk("a window wider than the ceiling is applied",
	    wpe_wlframe_apply(&cols, &rows), 1);
	chk("the grid stops at the ceiling, columns", cols, WPE_GRID_MAX_COLS);
	chk("the grid keeps the rows that fit", rows, CEILING_ROWS);
	chk("the buffer is the window's own width", WpeWl.width,
	    CEILING_BUFFER_W);
	chk("the buffer is the window's own height", WpeWl.height,
	    CEILING_BUFFER_H);
}

/* ---- the pump: a configure that comes alone ---- */

#define PUMP_DEADLINE_S 5
#define REGISTRY_ID 2          /* the first object a client makes */
#define GLOBAL_EVENT_BYTES 24  /* header, name, "x" padded to 8, version */

static int registry_globals;

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                      const char *iface, uint32_t version)
{
	(void)data; (void)reg; (void)name; (void)iface; (void)version;
	registry_globals++;
}

static void on_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	on_global, on_global_remove
};


/* The compositor sends one event for an object the owner's queue holds and
   nothing else: the shape of a configure that comes alone. */
static void send_lone_event(int fd)
{
	uint32_t msg[GLOBAL_EVENT_BYTES / 4];

	msg[0] = REGISTRY_ID;
	msg[1] = ((uint32_t)GLOBAL_EVENT_BYTES << 16) | 0u;   /* opcode 0: global */
	msg[2] = 1;                                            /* name */
	msg[3] = 2;                                            /* "x" and its NUL */
	memcpy(&msg[4], "x\0\0\0", 4);
	msg[5] = 1;                                            /* version */
	if (write(fd, msg, sizeof msg) != (ssize_t)sizeof msg)
		fails++;
}

/* A read that brings only an event of the frame owner's queue returns at
   once and leaves the event queued; the owner delivers it when it chooses.
   With libwayland's dispatch loop in its place the read polls for an event
   of the default queue that never comes: the editor then never answers the
   configure and never serves its end request.  The alarm turns that wait
   into a failure. */
static void test_lone_frame_event_is_read_without_blocking(void)
{
	int sv[2];
	struct wl_display *d;
	struct wl_registry *reg;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
		printf("FAIL: socketpair\n");
		fails++;
		return;
	}
	fresh();
	d = wl_display_connect_to_fd(sv[0]);
	WpeWl.display = d;
	reg = wl_display_get_registry(d);
	wpe_wlframe_claim_proxy(reg);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	send_lone_event(sv[1]);
	registry_globals = 0;
	test_watchdog_arm("the pump (it blocked inside libwayland waiting for "
	                  "an event of the default queue)", PUMP_DEADLINE_S);
	wpe_wlframe_read_events();
	wl_display_dispatch_pending(d);
	test_watchdog_disarm();
	chk("the read left the frame event queued", registry_globals, 0);
	wpe_wlframe_dispatch_frame_events();
	chk("the frame event is delivered when the owner chooses", registry_globals, 1);
	wl_registry_destroy(reg);
	wpe_wlframe_teardown_queue();
	wl_display_disconnect(d);
	close(sv[1]);
	WpeWl.display = NULL;
}

int main(void)
{
	test_fit_table();
	test_wa_sequence();
	test_queue_is_invisible_until_drained();
	test_state_only_configure();
	test_zero_by_zero();
	test_new_buffer_is_redrawn();
	test_refused_buffer_stays_pending();
	test_below_the_floor();
	test_a_buffer_the_protocol_cannot_carry_is_refused();
	test_a_buffer_wider_than_the_ceiling_carries_the_ceiling();
	test_lone_frame_event_is_read_without_blocking();
	if (fails)
		printf("%d failure(s)\n", fails);
	else
		printf("test_wlframe: all passed\n");
	return fails ? 1 : 0;
}
