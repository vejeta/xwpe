/* test_paint.c -- unit test for the screen's owner (we_paint.c).
 * Links we_paint.c and we_fdloop.c only; no editor, no display -- always
 * runs (no skip).  The editor's desk is played by the hooks below.
 *
 * What the user would see go wrong if a rule broke: output drawn under a
 * dialog (a paint not kept), the same stop shown twice (no coalescing), a
 * reply shown before the build that ended first (order), a box that lost
 * the screen to a box inside it, a paint drawn on a menu between two of its
 * keys (a replay on leave), or a lost update nobody is told about (bound).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../we_paint.h"
#include "../we_fdloop.h"

#define TRACE_FILE    "test_paint.trace"
#define LOG_MAX       256
#define KEY_ON_THE_BOX 'k'

static int fails;

static void chk(const char *what, int got, int want)
{
	if (got != want) {
		printf("FAIL: %s: got %d want %d\n", what, got, want);
		fails++;
	}
}

static void chk_str(const char *what, const char *got, const char *want)
{
	if (strcmp(got, want) != 0) {
		printf("FAIL: %s: got \"%s\" want \"%s\"\n", what, got, want);
		fails++;
	}
}

/* ---- the desk ---- */

static int text_on_top = 1;
static int depth_seen_by_read = -1;
static int boxes_opened = 0;
static char lost_said[LOG_MAX];
static int lost_times = 0;

static int desk_text_on_top(void) { return text_on_top; }

static int desk_read_key(void)
{
	depth_seen_by_read = wpe_paint_modal_depth();
	return KEY_ON_THE_BOX;
}

static void desk_box_opens(void) { boxes_opened++; }

static void desk_say_lost(const char *what)
{
	snprintf(lost_said, sizeof lost_said, "%s", what);
	lost_times++;
}

static const WpePaintDesk desk = {
	desk_text_on_top, desk_read_key, desk_box_opens, desk_say_lost
};

/* ---- the paints: each appends its letter to the log ---- */

static char order[LOG_MAX];

static void log_letter(char c)
{
	size_t n = strlen(order);

	if (n + 1 < sizeof order) {
		order[n] = c;
		order[n + 1] = '\0';
	}
}

static void paint_letter(void *data) { log_letter(*(const char *)data); }

static const char A = 'A', B = 'B', C = 'C';

/* A paint that opens a box and leaves it open (a review box whose reply
   came behind a dialog: it is still up when the paint returns, for this
   test's purpose). */
static void paint_opens_a_box(void *data)
{
	log_letter(*(const char *)data);
	wpe_paint_modal_enter("a box a paint opened");
}

/* A paint that asks for another paint while it runs. */
static void paint_asks_for_b(void *data)
{
	log_letter('[');
	wpe_paint_request(paint_letter, (void *)&B, "B");
	log_letter(*(const char *)data);
	log_letter(']');
}

/* A paint that asks for itself again every time it runs, until told not to. */
static int self_runs = 0;
static int self_keeps_asking = 1;
static void paint_asks_for_itself(void *data)
{
	self_runs++;
	if (self_keeps_asking)
		wpe_paint_request(paint_asks_for_itself, data, "itself");
}

/* ---- helpers ---- */

static void reset(void)
{
	text_on_top = 1;
	while (wpe_paint_modal_depth() > 0)
		wpe_paint_modal_leave();
	wpe_paint_replay_if_idle();
	order[0] = '\0';
	lost_said[0] = '\0';
	lost_times = 0;
}

static int trace_has(const char *needle)
{
	char line[LOG_MAX];
	FILE *f = fopen(TRACE_FILE, "r");
	int found = 0;

	if (!f)
		return 0;
	while (!found && fgets(line, sizeof line, f))
		found = strstr(line, needle) != NULL;
	fclose(f);
	return found;
}

/* ---- the cases ---- */

static void test_idle_paints_at_once(void)
{
	reset();
	chk("idle: the paint runs now", wpe_paint_request(paint_letter, (void *)&A, "A"), WPE_PAINT_NOW);
	chk_str("idle: it ran once", order, "A");
}

static void test_under_a_box_the_paint_waits_for_the_key_loop(void)
{
	reset();
	wpe_paint_modal_enter("a dialog");
	chk("under a box: not idle", wpe_paint_idle(), 0);
	chk("under a box: kept", wpe_paint_request(paint_letter, (void *)&A, "A"), WPE_PAINT_DEFERRED);
	chk_str("under a box: not drawn", order, "");
	wpe_paint_replay_if_idle();
	chk_str("under a box: the key loop does not draw it either", order, "");
	wpe_paint_modal_leave();
	chk_str("leave never draws (a menu between two keys is still up)", order, "");
	wpe_paint_replay_if_idle();
	chk_str("the key loop draws it once the box is gone", order, "A");
	wpe_paint_replay_if_idle();
	chk_str("and only once", order, "A");
}

static void test_the_same_paint_is_kept_once(void)
{
	reset();
	wpe_paint_modal_enter("a dialog");
	wpe_paint_request(paint_letter, (void *)&A, "A");
	wpe_paint_request(paint_letter, (void *)&B, "B");
	wpe_paint_request(paint_letter, (void *)&A, "A again");
	wpe_paint_request(paint_letter, (void *)&A, "A again");
	wpe_paint_modal_leave();
	wpe_paint_replay_if_idle();
	chk_str("one A in its first place, then B", order, "AB");
}

static void test_paints_run_in_the_order_asked(void)
{
	reset();
	wpe_paint_modal_enter("a dialog");
	wpe_paint_request(paint_letter, (void *)&C, "C");
	wpe_paint_request(paint_letter, (void *)&A, "A");
	wpe_paint_request(paint_letter, (void *)&B, "B");
	wpe_paint_modal_leave();
	wpe_paint_replay_if_idle();
	chk_str("the order asked", order, "CAB");
}

static char bound_data[WPE_PAINT_DEFER_MAX + 1];

static void test_a_full_queue_refuses_and_says_so_once(void)
{
	int i;

	reset();
	wpe_paint_modal_enter("a dialog");
	for (i = 0; i < WPE_PAINT_DEFER_MAX; i++) {
		bound_data[i] = 'a' + i;
		chk("within the bound: kept", wpe_paint_request(paint_letter, &bound_data[i], "kept"), WPE_PAINT_DEFERRED);
	}
	bound_data[WPE_PAINT_DEFER_MAX] = 'z';
	chk("past the bound: refused", wpe_paint_request(paint_letter, &bound_data[WPE_PAINT_DEFER_MAX], "the seventeenth"), WPE_PAINT_REFUSED);
	chk("nothing said under the box", lost_times, 0);
	wpe_paint_modal_leave();
	wpe_paint_replay_if_idle();
	chk("every kept paint ran", (int)strlen(order), WPE_PAINT_DEFER_MAX);
	chk("the refused one did not", strchr(order, 'z') != NULL, 0);
	chk("the status row names it once", lost_times, 1);
	chk_str("by its name", lost_said, "the seventeenth");
	wpe_paint_replay_if_idle();
	chk("and not again", lost_times, 1);
}

static void test_a_paint_that_opens_a_box_leaves_the_rest_for_later(void)
{
	reset();
	wpe_paint_modal_enter("a dialog");
	wpe_paint_request(paint_opens_a_box, (void *)&A, "A opens a box");
	wpe_paint_request(paint_letter, (void *)&B, "B");
	wpe_paint_request(paint_letter, (void *)&C, "C");
	wpe_paint_modal_leave();
	wpe_paint_replay_if_idle();
	chk_str("A ran and opened its box; B and C wait", order, "A");
	chk("the box A opened owns the screen", wpe_paint_modal_depth(), 1);
	wpe_paint_modal_leave();
	wpe_paint_replay_if_idle();
	chk_str("B and C run once A's box is gone, in order", order, "ABC");
}

static void test_a_box_inside_a_box_keeps_the_screen(void)
{
	reset();
	wpe_paint_modal_enter("the review box");
	wpe_paint_modal_enter("an error box inside it");
	wpe_paint_modal_leave();
	chk("the review box is still the owner", wpe_paint_idle(), 0);
	chk("a paint waits", wpe_paint_request(paint_letter, (void *)&A, "A"), WPE_PAINT_DEFERRED);
	wpe_paint_replay_if_idle();
	chk_str("nothing drawn under the review box", order, "");
	wpe_paint_modal_leave();
	wpe_paint_replay_if_idle();
	chk_str("drawn when the review box closes", order, "A");
}

static void test_a_paint_asked_for_inside_a_paint_runs_after_it(void)
{
	reset();
	wpe_paint_request(paint_asks_for_b, (void *)&A, "A asks for B");
	chk_str("B after A, never inside it", order, "[A]B");
}

static void test_a_paint_that_asks_for_itself_cannot_hold_the_key_loop(void)
{
	reset();
	self_runs = 0;
	wpe_paint_request(paint_asks_for_itself, NULL, "itself");
	chk("bounded runs", self_runs, WPE_PAINT_REPLAY_MAX + 1);
	self_runs = 0;
	wpe_paint_replay_if_idle();
	chk("the next turn of the key loop goes on, still bounded", self_runs, WPE_PAINT_REPLAY_MAX);
	self_keeps_asking = 0;
	self_runs = 0;
	wpe_paint_replay_if_idle();
	chk("once it stops asking, it runs out", self_runs, 1);
}

static void test_a_non_text_window_on_top_is_not_idle(void)
{
	reset();
	text_on_top = 0;
	chk("the File Manager on top: not idle", wpe_paint_idle(), 0);
	chk("a paint waits", wpe_paint_request(paint_letter, (void *)&A, "A"), WPE_PAINT_DEFERRED);
	text_on_top = 1;
	wpe_paint_replay_if_idle();
	chk_str("drawn once a text window is back on top", order, "A");
}

static int idle_inside_the_wait = -1;

static int wait_pred(void)
{
	idle_inside_the_wait = wpe_paint_idle();
	return 1;
}

static void test_a_nested_wait_is_not_idle(void)
{
	reset();
	wpe_fd_wait_for(wait_pred, 0, "a command that has the terminal");
	chk("inside the nested wait: not idle", idle_inside_the_wait, 0);
	chk("after it: idle", wpe_paint_idle(), 1);
}

static void test_a_box_reads_its_key_owning_the_screen(void)
{
	int c;

	reset();
	boxes_opened = 0;
	c = wpe_paint_modal_getch();
	chk("the key", c, KEY_ON_THE_BOX);
	chk("read with the box owning the screen", depth_seen_by_read, 1);
	chk("the owner gives the screen back", wpe_paint_modal_depth(), 0);
	chk("the desk was told a box opened", boxes_opened, 1);
}

static void test_depth_past_the_bound_is_traced_and_stays_balanced(void)
{
	int i;

	reset();
	unlink(TRACE_FILE);
	setenv("XWPE_UI_TRACE", TRACE_FILE, 1);
	for (i = 0; i < WPE_PAINT_DEPTH_MAX + 1; i++)
		wpe_paint_modal_enter("deep");
	chk("past the bound: traced", trace_has("modal depth over"), 1);
	chk("every level counted", wpe_paint_modal_depth(), WPE_PAINT_DEPTH_MAX + 1);
	for (i = 0; i < WPE_PAINT_DEPTH_MAX + 1; i++)
		wpe_paint_modal_leave();
	chk("balanced back to none", wpe_paint_modal_depth(), 0);
	wpe_paint_modal_leave();
	chk("a leave too many: traced", trace_has("modal leave unbalanced"), 1);
	chk("and the depth stays none", wpe_paint_modal_depth(), 0);
	unsetenv("XWPE_UI_TRACE");
	unlink(TRACE_FILE);
}

static void test_the_trace_shows_deferred_then_replay(void)
{
	reset();
	unlink(TRACE_FILE);
	setenv("XWPE_UI_TRACE", TRACE_FILE, 1);
	wpe_paint_modal_enter("a dialog");
	wpe_paint_request(paint_letter, (void *)&A, "program output");
	wpe_paint_modal_leave();
	chk("deferred traced", trace_has("paint deferred program output"), 1);
	chk("no replay yet", trace_has("paint replay program output"), 0);
	wpe_paint_replay_if_idle();
	chk("replay traced", trace_has("paint replay program output"), 1);
	unsetenv("XWPE_UI_TRACE");
	unlink(TRACE_FILE);
}

static void test_the_overlay_switch(void)
{
	wpe_paint_chrome_suppress(1);
	chk("the overlay kept off", wpe_paint_chrome_suppressed(), 1);
	wpe_paint_chrome_suppress(0);
	chk("the overlay back", wpe_paint_chrome_suppressed(), 0);
}

static void test_nothing_is_idle_before_the_desk_is_known(void)
{
	chk("before the editor said how to see its desk", wpe_paint_idle(), 0);
}

int main(void)
{
	test_nothing_is_idle_before_the_desk_is_known();
	wpe_paint_desk_set(&desk);
	test_idle_paints_at_once();
	test_under_a_box_the_paint_waits_for_the_key_loop();
	test_the_same_paint_is_kept_once();
	test_paints_run_in_the_order_asked();
	test_a_full_queue_refuses_and_says_so_once();
	test_a_paint_that_opens_a_box_leaves_the_rest_for_later();
	test_a_box_inside_a_box_keeps_the_screen();
	test_a_paint_asked_for_inside_a_paint_runs_after_it();
	test_a_non_text_window_on_top_is_not_idle();
	test_a_nested_wait_is_not_idle();
	test_a_box_reads_its_key_owning_the_screen();
	test_depth_past_the_bound_is_traced_and_stays_balanced();
	test_the_trace_shows_deferred_then_replay();
	test_the_overlay_switch();
	test_a_paint_that_asks_for_itself_cannot_hold_the_key_loop();
	if (fails) {
		printf("test_paint: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_paint: all passed\n");
	return 0;
}
