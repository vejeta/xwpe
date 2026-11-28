/* test_waitline.c -- unit test for the owner of the waiting lines at the foot
 * of Messages (we_waitline.c) on the buffer's owner (we_buffer.c).  The
 * editor's side is played by the recorders below.  Always runs.
 *
 * What the user would see go wrong if a rule broke: two waiting lines in one
 * row so that only one of them is ever read, a line that is rewritten into
 * another activity's row, output that lands below a waiting line, a line
 * deleted with the output above it, the lines gone after Messages was
 * emptied for a new build, a row the user typed over taken away, a line shown
 * twice after Messages was closed and opened, or no repaint when the seconds
 * on a line turn.
 */
#include <stdio.h>
#include "linebuf.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "../we_buffer.h"
#include "../we_lineend.h"
#include "../we_cancel.h"
#include "../we_waitline.h"

#define WIDTH 80

static int fails;

/* ---- the editor's side, as recorders ---- */

BUFFER *e_p_m_buffer;
struct CNT *WpeEditor;
static int g_paints;

void e_messages_show_tail(ECNT *cn)
{
 (void) cn;
 g_paints++;
}

/* The order Esc takes the claims in the editor's table. */
int e_cancel_rank(const char *what)
{
 if (strcmp(what, "the build") == 0)
  return(0);
 if (strcmp(what, "the debugger start") == 0)
  return(1);
 if (strcmp(what, "scala-cli setup") == 0)
  return(2);
 if (strcmp(what, "the compile for debug") == 0)
  return(5);
 return(WPE_CANCEL_NO_RANK);
}

void wpe_ui_trace(const char *fmt, ...) { (void) fmt; }

void wpe_buffer_syntax_changed(BUFFER *b, int y) { (void) b; (void) y; }

void wpe_buffer_lines_moved(BUFFER *b, int y, int n_del, int n_ins,
                            const int *rows_removed, int n_removed,
                            const int *rows_added, int n_added)
{
 (void) b; (void) y; (void) n_del; (void) n_ins; (void) rows_removed;
 (void) n_removed; (void) rows_added; (void) n_added;
}

void wpe_buffer_say_nomem(BUFFER *b, const char *what)
{
 (void) b;
 printf("FAIL: refused for memory: %s\n", what);
 fails++;
}

void wpe_buffer_say_shape(BUFFER *b, const char *what) { (void) b; (void) what; }
void wpe_buffer_say_partial(BUFFER *b) { (void) b; }
void wpe_buffer_say_read_only(BUFFER *b) { (void) b; }
int wpe_buffer_replay_kind(BUFFER *b, SCHIRM *s, Undo *rec)
{
 (void) b; (void) s; (void) rec;
 return(E_BUFFER_OK);
}
void wpe_buffer_replay_on_shell(BUFFER *shell, SCHIRM *s, const Undo *rec, int y0)
{
 (void) shell; (void) s; (void) rec; (void) y0;
}
void wpe_buffer_trace(const char *fmt, ...) { (void) fmt; }

/* ---- helpers ---- */

static void chk(const char *what, int got, int want)
{
 if (got != want) {
  printf("FAIL: %s: got %d want %d\n", what, got, want);
  fails++;
 }
}

static void chk_row(const char *what, BUFFER *b, int y, const char *want)
{
 const char *got = (y >= 0 && y < b->mxlines) ? (const char *) b->bf[y].s : "(no row)";

 if (strncmp(got, want, strlen(want)) != 0 || (int) strlen(want) != b->bf[y].len) {
  printf("FAIL: %s: row %d reads \"%s\" want \"%s\"\n", what, y, got, want);
  fails++;
 }
}

/* A Messages text as a compile leaves it: one empty row. */
static BUFFER *fresh_messages(void)
{
 BUFFER *b = calloc(1, sizeof(BUFFER));

 e_buffer_init(b, NULL, NULL, WIDTH, E_BUFFER_NO_HISTORY);
 e_buffer_clear(b);
 e_p_m_buffer = b;
 return(b);
}

static void free_buffer(BUFFER *b)
{
 e_buffer_lines_free(b->bf, b->mxlines);
 free(b);
 e_p_m_buffer = NULL;
}

/* A writer of output, as the output primitives are: the foot is lifted, the
   text appended, the foot laid. */
static void output(BUFFER *b, const char *text)
{
 wpe_wait_lift(b);
 e_eol_append_rows(b, text, E_EOL_NORMALIZE);
 wpe_wait_lay(b);
}

/* ---- the rows ---- */

static void test_two_lines_are_two_rows_in_the_esc_order(void)
{
 BUFFER *b = fresh_messages();
 WpeWait dbg = WPE_WAIT("the debugger start");
 WpeWait build = WPE_WAIT("the build");

 wpe_wait_begin(&dbg);
 wpe_wait_say(&dbg, "starting dlv ... %lds", 3L);
 wpe_wait_begin(&build);
 wpe_wait_say(&build, "building ... %lds", 1L);
 chk("rows", b->mxlines, 3);
 chk_row("the one Esc ends later is above", b, 1, "starting dlv ... 3s");
 chk_row("the one Esc ends first is the bottom", b, 2, "building ... 1s");
 chk("the debugger's line is shown", wpe_wait_shown(&dbg), 1);
 chk("the build's line is shown", wpe_wait_shown(&build), 1);
 chk("the foot has two rows", wpe_wait_foot_rows(b), 2);
 wpe_wait_end(&build);
 wpe_wait_end(&dbg);
 free_buffer(b);
}

static void test_the_order_does_not_depend_on_who_began(void)
{
 BUFFER *b = fresh_messages();
 WpeWait dbg = WPE_WAIT("the debugger start");
 WpeWait build = WPE_WAIT("the build");

 wpe_wait_begin(&build);
 wpe_wait_say(&build, "building ... 0s");
 wpe_wait_begin(&dbg);
 wpe_wait_say(&dbg, "starting dlv ... 0s");
 chk_row("the debugger's line is above the build's", b, 1, "starting dlv ... 0s");
 chk_row("the build's line is the bottom", b, 2, "building ... 0s");
 wpe_wait_end(&dbg);
 wpe_wait_end(&build);
 free_buffer(b);
}

static void test_a_line_turns_in_its_own_row_and_the_screen_is_asked_once(void)
{
 BUFFER *b = fresh_messages();
 WpeWait dbg = WPE_WAIT("the debugger start");
 WpeWait build = WPE_WAIT("the build");
 ECNT desk;
 int paints;

 memset(&desk, 0, sizeof desk);
 desk.mxedt = 1;
 WpeEditor = &desk;                 /* a desk with one window: a repaint is asked */
 wpe_wait_begin(&dbg);
 wpe_wait_say(&dbg, "starting dlv ... 1s");
 wpe_wait_begin(&build);
 wpe_wait_say(&build, "building ... 1s");
 paints = g_paints;
 wpe_wait_say(&dbg, "starting dlv ... 2s");
 chk("a turn asks for one repaint", g_paints - paints, 1);
 chk("a turn keeps the row count", b->mxlines, 3);
 chk_row("the debugger's row turned", b, 1, "starting dlv ... 2s");
 chk_row("the build's row stayed", b, 2, "building ... 1s");
 paints = g_paints;
 wpe_wait_say(&dbg, "starting dlv ... 2s");
 chk("the same text asks for none", g_paints - paints, 0);
 wpe_wait_end(&build);
 wpe_wait_end(&dbg);
 WpeEditor = NULL;
 free_buffer(b);
}

static void test_output_lands_above_the_foot(void)
{
 BUFFER *b = fresh_messages();
 WpeWait dbg = WPE_WAIT("the debugger start");
 WpeWait build = WPE_WAIT("the build");

 wpe_wait_begin(&dbg);
 wpe_wait_say(&dbg, "starting dlv ... 1s");
 wpe_wait_begin(&build);
 wpe_wait_say(&build, "building ... 1s");
 output(b, "cc -c a.c");
 output(b, "a.c:3: error");
 chk("rows", b->mxlines, 5);
 chk_row("first output", b, 1, "cc -c a.c");
 chk_row("second output", b, 2, "a.c:3: error");
 chk_row("the debugger's line", b, 3, "starting dlv ... 1s");
 chk_row("the build's line", b, 4, "building ... 1s");
 chk("the foot is still in the buffer", wpe_wait_foot_rows(b), 2);
 wpe_wait_end(&build);
 wpe_wait_end(&dbg);
 free_buffer(b);
}

static void test_a_nested_bracket_lays_once(void)
{
 BUFFER *b = fresh_messages();
 WpeWait build = WPE_WAIT("the build");

 wpe_wait_begin(&build);
 wpe_wait_say(&build, "building ... 1s");
 chk("the outer lift takes one row", wpe_wait_lift(b), 1);
 chk("the inner lift takes none", wpe_wait_lift(b), 0);
 e_eol_append_rows(b, "row", E_EOL_NORMALIZE);
 wpe_wait_lay(b);
 chk("the inner lay waits for the outer", b->mxlines, 2);
 wpe_wait_lay(b);
 chk("the outer lay puts the line back", b->mxlines, 3);
 chk_row("the output", b, 1, "row");
 chk_row("the line", b, 2, "building ... 1s");
 wpe_wait_end(&build);
 free_buffer(b);
}

static void test_a_say_between_a_lift_and_its_lay_waits_for_the_lay(void)
{
 BUFFER *b = fresh_messages();
 WpeWait build = WPE_WAIT("the build");

 wpe_wait_begin(&build);
 wpe_wait_say(&build, "building ... 1s");
 wpe_wait_lift(b);
 wpe_wait_say(&build, "building ... 2s");
 chk("no row is written while the foot is out", b->mxlines, 1);
 wpe_wait_lay(b);
 chk_row("the lay puts the newest text", b, 1, "building ... 2s");
 wpe_wait_end(&build);
 free_buffer(b);
}

static void test_the_end_of_one_leaves_the_other(void)
{
 BUFFER *b = fresh_messages();
 WpeWait dbg = WPE_WAIT("the debugger start");
 WpeWait build = WPE_WAIT("the build");

 wpe_wait_begin(&dbg);
 wpe_wait_say(&dbg, "starting dlv ... 1s");
 wpe_wait_begin(&build);
 wpe_wait_say(&build, "building ... 1s");
 output(b, "output");
 wpe_wait_end(&build);
 chk("one row went", b->mxlines, 3);
 chk_row("the output is untouched", b, 1, "output");
 chk_row("the debugger's line stayed", b, 2, "starting dlv ... 1s");
 chk("the ended waiter shows nothing", wpe_wait_shown(&build), 0);
 output(b, "build cancelled");
 chk_row("the verdict is above the debugger's line", b, 2, "build cancelled");
 chk_row("the debugger's line is the bottom", b, 3, "starting dlv ... 1s");
 wpe_wait_end(&dbg);
 chk("the last end empties the foot", b->mxlines, 3);
 free_buffer(b);
}

static void test_a_clear_keeps_the_foot(void)
{
 BUFFER *b = fresh_messages();
 WpeWait dbg = WPE_WAIT("the debugger start");
 WpeWait build = WPE_WAIT("the build");

 wpe_wait_begin(&dbg);
 wpe_wait_say(&dbg, "starting dlv ... 1s");
 output(b, "old output");
 chk("the clear is whole", wpe_wait_messages_clear(b), E_BUFFER_OK);
 chk("the empty row and the line", b->mxlines, 2);
 chk_row("the line is back at once", b, 1, "starting dlv ... 1s");
 chk("and is laid", wpe_wait_shown(&dbg), 1);
 wpe_wait_begin(&build);
 wpe_wait_say(&build, "building ... 0s");
 wpe_wait_messages_clear(b);
 chk("both lines are back", b->mxlines, 3);
 chk_row("the debugger's", b, 1, "starting dlv ... 1s");
 chk_row("the build's", b, 2, "building ... 0s");
 wpe_wait_end(&build);
 wpe_wait_end(&dbg);
 free_buffer(b);
}

static void test_a_row_the_user_rewrote_is_left_alone(void)
{
 BUFFER *b = fresh_messages();
 WpeWait build = WPE_WAIT("the build");

 wpe_wait_begin(&build);
 wpe_wait_say(&build, "building ... 1s");
 e_buffer_set_line(b, 1, "typed by the user", 17);
 wpe_wait_say(&build, "building ... 2s");
 chk("the user's row stayed and the line was laid again", b->mxlines, 3);
 chk_row("the user's row", b, 1, "typed by the user");
 chk_row("the new line", b, 2, "building ... 2s");
 output(b, "output");
 chk_row("the user's row is still there", b, 1, "typed by the user");
 chk_row("the output", b, 2, "output");
 chk_row("the line", b, 3, "building ... 2s");
 wpe_wait_end(&build);
 chk("only the line went", b->mxlines, 3);
 chk_row("the user's row after the end", b, 1, "typed by the user");
 free_buffer(b);
}

static void test_messages_closed_then_opened_shows_one_line(void)
{
 BUFFER *b;
 WpeWait dbg = WPE_WAIT("the debugger start");

 e_p_m_buffer = NULL;
 wpe_wait_begin(&dbg);
 wpe_wait_say(&dbg, "starting dlv ... 1s");
 chk("nothing is shown with no Messages", wpe_wait_shown(&dbg), 0);
 b = fresh_messages();
 wpe_wait_messages_opened(b);
 chk("the line is laid when Messages opens", b->mxlines, 2);
 chk_row("the line", b, 1, "starting dlv ... 1s");
 wpe_wait_messages_opened(b);
 chk("opened twice shows it once", b->mxlines, 2);
 wpe_wait_say(&dbg, "starting dlv ... 2s");
 chk("a turn after it rewrites the same row", b->mxlines, 2);
 chk_row("the turned line", b, 1, "starting dlv ... 2s");
 wpe_wait_end(&dbg);
 chk("the end takes the row", b->mxlines, 1);
 free_buffer(b);
}

static void test_a_buffer_that_is_not_messages_is_left_alone(void)
{
 BUFFER *msgs = fresh_messages();
 BUFFER *other = calloc(1, sizeof(BUFFER));
 WpeWait build = WPE_WAIT("the build");

 e_buffer_init(other, NULL, NULL, WIDTH, E_BUFFER_NO_HISTORY);
 e_buffer_clear(other);
 wpe_wait_begin(&build);
 wpe_wait_say(&build, "building ... 1s");
 chk("lift of another buffer takes nothing", wpe_wait_lift(other), 0);
 e_eol_append_rows(other, "listing", E_EOL_NORMALIZE);
 wpe_wait_lay(other);
 chk("the other buffer has no foot", other->mxlines, 2);
 chk("the foot rows of another buffer", wpe_wait_foot_rows(other), 0);
 chk("Messages still has its line", msgs->mxlines, 2);
 wpe_wait_end(&build);
 e_buffer_lines_free(other->bf, other->mxlines);
 free(other);
 free_buffer(msgs);
}

static void test_begin_and_end_are_safe_in_every_order(void)
{
 BUFFER *b = fresh_messages();
 WpeWait w = WPE_WAIT("the build");
 WpeWait many[WPE_WAIT_MAX + 1];
 int i;

 wpe_wait_end(&w);                  /* a dead waiter: nothing happens */
 wpe_wait_say(&w, "dead");
 chk("a dead waiter writes nothing", b->mxlines, 1);
 wpe_wait_begin(&w);
 wpe_wait_begin(&w);                /* a live one again: nothing happens */
 wpe_wait_say(&w, "building ... 1s");
 chk("one row", b->mxlines, 2);
 wpe_wait_end(&w);
 wpe_wait_end(&w);
 chk("one end took it", b->mxlines, 1);
 for (i = 0; i <= WPE_WAIT_MAX; i++) {
  WpeWait fresh = WPE_WAIT("the compile for debug");
  many[i] = fresh;
  wpe_wait_begin(&many[i]);
  wpe_wait_say(&many[i], "waiting %d", i);
 }
 chk("the owner's room is not exceeded", b->mxlines, 1 + WPE_WAIT_MAX);
 for (i = 0; i <= WPE_WAIT_MAX; i++)
  wpe_wait_end(&many[i]);
 chk("every line ended", b->mxlines, 1);
 free_buffer(b);
}

int main(void)
{
 test_two_lines_are_two_rows_in_the_esc_order();
 test_the_order_does_not_depend_on_who_began();
 test_a_line_turns_in_its_own_row_and_the_screen_is_asked_once();
 test_output_lands_above_the_foot();
 test_a_nested_bracket_lays_once();
 test_a_say_between_a_lift_and_its_lay_waits_for_the_lay();
 test_the_end_of_one_leaves_the_other();
 test_a_clear_keeps_the_foot();
 test_a_row_the_user_rewrote_is_left_alone();
 test_messages_closed_then_opened_shows_one_line();
 test_a_buffer_that_is_not_messages_is_left_alone();
 test_begin_and_end_are_safe_in_every_order();
 if (fails) {
  printf("%d failure(s)\n", fails);
  return(1);
 }
 printf("test_waitline: ok\n");
 return(0);
}
