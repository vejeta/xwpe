/* test_pmsg.c -- unit test for the lines kept behind a box until it closes
 * (we_pmsg.c).  The editor's side is played by the recorders below.  Always
 * runs.
 *
 * What the user would see go wrong if a rule broke: the lines a language
 * server or a debugger said while a dialog was open come out in another
 * order or not at all, the assistant's checklist drawn twice behind one
 * dialog comes out twice when the dialog closes (the plan and its tick one
 * under the other), or a line another writer kept is lost with the old
 * checklist.
 */
#include <stdio.h>
#include "linebuf.h"
#include <stdlib.h>
#include <string.h>

#include "../edit.h"
#include "../we_paint.h"
#include "../we_pmsg.h"

#define SAID_MAX 64

static int fails;
static char said[SAID_MAX][128];
static int nsaid;
static int g_requests;

/* ---- the editor's side, as recorders ---- */

WpePaintVerdict wpe_paint_request(WpePaintReplay fn, void *data, const char *what)
{
 (void) fn; (void) data; (void) what;
 g_requests++;
 return(WPE_PAINT_DEFERRED);
}

/* What the editor writes into Messages when the screen is free. */
static int say_into_messages(char *str, FENSTER *f, int sw)
{
 (void) f; (void) sw;
 if (nsaid < SAID_MAX)
  snprintf(said[nsaid++], sizeof said[0], "%s", str);
 return(0);
}

/* The checklist's wording, as the assistant draws it. */
static int is_checklist_line(const char *line)
{
 return(strncmp(line, "=== Agent plan:", 15) == 0 || strncmp(line, "  [", 3) == 0);
}

/* ---- helpers ---- */

static ECNT g_cn;
static FENSTER g_win;

static void reset(void)
{
 wpe_pmsg_replay(NULL);                  /* nothing kept from a row before */
 nsaid = 0;
 g_requests = 0;
 memset(&g_cn, 0, sizeof g_cn);
 memset(&g_win, 0, sizeof g_win);
 g_win.ed = &g_cn;
 g_cn.mxedt = 1;
 g_cn.f[1] = &g_win;
}

static void chk(const char *what, int got, int want)
{
 if (got != want) {
  printf("FAIL: %s: got %d want %d\n", what, got, want);
  fails++;
 }
}

static void chk_said(const char *what, int at, const char *want)
{
 const char *got = (at >= 0 && at < nsaid) ? said[at] : "(nothing)";

 if (strcmp(got, want) != 0) {
  printf("FAIL: %s: said[%d] is \"%s\", want \"%s\"\n", what, at, got, want);
  fails++;
 }
}

static void hold(const char *line)
{
 wpe_pmsg_hold(line, &g_win, 0, say_into_messages);
}

/* ---- rows ---- */

static void test_lines_come_out_in_the_order_they_were_said(void)
{
 reset();
 hold("first");
 hold("second");
 hold("third");
 chk("three lines are kept", wpe_pmsg_pending(), 3);
 chk("the screen's owner is asked to replay", g_requests > 0, 1);
 wpe_pmsg_replay(NULL);
 chk("three lines were written", nsaid, 3);
 chk_said("order", 0, "first");
 chk_said("order", 1, "second");
 chk_said("order", 2, "third");
 chk("nothing is kept after the replay", wpe_pmsg_pending(), 0);
}

static void test_a_full_keeping_says_how_many_were_lost(void)
{
 int i;

 reset();
 for (i = 0; i < 40; i++)
  hold("line");
 wpe_pmsg_replay(NULL);
 chk("the kept lines and the count", nsaid, 33);
 chk_said("the count", 32, "(8 more messages were not shown)");
}

/* The checklist drawn twice behind one dialog: only the latest copy comes out,
   and what another writer kept stays, in its place.  Red when the old copy is
   not taken out of the keeping (wpe_pmsg_drop not called). */
static void test_a_checklist_drawn_twice_behind_a_box_comes_out_once(void)
{
 reset();
 hold("=== Agent plan: 0/2 done ===");
 hold("  [ ] first");
 hold("  [ ] second");
 hold("the language server said this");
 wpe_pmsg_drop(is_checklist_line);       /* the checklist is drawn again */
 hold("=== Agent plan: 1/2 done ===");
 hold("  [x] first");
 hold("  [ ] second");
 wpe_pmsg_replay(NULL);
 chk("one checklist and the other writer's line", nsaid, 4);
 chk_said("the other writer's line stays first", 0, "the language server said this");
 chk_said("the latest header", 1, "=== Agent plan: 1/2 done ===");
 chk_said("the latest tick", 2, "  [x] first");
 chk_said("the pending step", 3, "  [ ] second");
}

static void test_dropping_nothing_keeps_everything(void)
{
 reset();
 hold("one");
 hold("two");
 wpe_pmsg_drop(is_checklist_line);
 chk("both lines are still kept", wpe_pmsg_pending(), 2);
 wpe_pmsg_replay(NULL);
 chk_said("first", 0, "one");
 chk_said("second", 1, "two");
}

int main(void)
{
 test_lines_come_out_in_the_order_they_were_said();
 test_a_full_keeping_says_how_many_were_lost();
 test_a_checklist_drawn_twice_behind_a_box_comes_out_once();
 test_dropping_nothing_keeps_everything();
 if (fails) {
  printf("%d failure(s)\n", fails);
  return(1);
 }
 printf("test_pmsg: ok\n");
 return(0);
}
