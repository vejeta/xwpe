/* we_grid_desk.c -- the editor's side of the screen grid owner.
   See we_grid.h.

   The owner (we_grid.c) knows no display and no editor structure.  This file
   gives its answers their meaning for the editor: what is written to the
   trace, what the user is told when a size was cut down or refused, and the
   fault a test arms through the environment to show a refusal on every
   display without depending on how much memory the machine has. */

#include <stdlib.h>
#include <string.h>

#include "messages.h"
#include "edit.h"
#include "we_fmt.h"
#include "we_paint.h"

/* XWPE_TEST_FAULT=grid:N refuses the first allocator request of the Nth
   take that allocates.  Unset, or another fault name, arms nothing.  The
   grammar is the one the picture-memory fault reads (name:N). */
#define GRID_FAULT_NAME "grid"
#define GRID_NOTICE_SIZE 160

static int grid_fault_take;       /* the take to refuse, 0 for none */
static int grid_takes_counted;    /* takes that allocated so far */
static int grid_fault_refuse_next;
static int grid_ready;

static void *e_screen_fault_alloc(size_t bytes)
{
 if (grid_fault_refuse_next)
 {
  grid_fault_refuse_next = 0;
  return NULL;
 }
 return malloc(bytes);
}

static void e_screen_fault_release(void *block)
{
 free(block);
}

static void e_screen_fault_begin_take(void)
{
 grid_takes_counted++;
 if (grid_takes_counted == grid_fault_take)
  grid_fault_refuse_next = 1;
}

/**
 * e_screen_arm_fault - Read the fault a test asks for, once.
 *
 * Called before the first take, which is made while the display starts.
 * Only a take that allocates counts: a take of the size the grid already has,
 * or of the size just refused, does not, so N does not depend on how many
 * configure events a display happens to send.
 */
static void e_screen_arm_fault(void)
{
 static const WpeGridAlloc faulty = {
  e_screen_fault_alloc, e_screen_fault_release, e_screen_fault_begin_take
 };
 const char *spec = getenv("XWPE_TEST_FAULT");
 size_t len = strlen(GRID_FAULT_NAME);

 grid_ready = 1;
 if (spec == NULL || strncmp(spec, GRID_FAULT_NAME, len) != 0 ||
     spec[len] != ':')
  return;
 grid_fault_take = atoi(spec + len + 1);
 if (grid_fault_take > 0)
  wpe_grid_use_alloc(&faulty);
}

/**
 * e_screen_take - Take a new grid size for the editor, and write it to the
 * trace.
 * @cols: @rows: the size asked for.
 * @old_cols: @old_rows: receive the size the grid had, for the layout owner.
 *
 * The one call every display makes when its window or terminal changes size.
 * The trace has "grid COLSxROWS taken" for a new size and "grid kept
 * COLSxROWS, refused CxR" when memory was refused.
 *
 * Return: what wpe_grid_take answered.
 */
WpeGridTake e_screen_take(int cols, int rows, int *old_cols, int *old_rows)
{
 WpeGridTake took;

 if (!grid_ready)
  e_screen_arm_fault();
 *old_cols = MAXSCOL;
 *old_rows = MAXSLNS;
 took = wpe_grid_take(cols, rows);
 if (took == WPE_GRID_CHANGED)
  wpe_ui_trace("grid %dx%d taken", MAXSCOL, MAXSLNS);
 else if (took == WPE_GRID_KEPT)
  wpe_ui_trace("grid kept %dx%d, refused %dx%d", MAXSCOL, MAXSLNS,
               cols, rows);
 return took;
}

/**
 * e_screen_start - Take the first grid of the run.
 * @cols: @rows: the size the display starts at.
 *
 * What the user gets: an editor that starts on a smaller grid, and says so,
 * when the memory for the size it was asked for is not there.
 *
 * Return: 0 when a grid exists, -1 when there is no memory even for the
 * usual 80 x 24; the caller releases its display and ends with a sentence.
 */
int e_screen_start(int cols, int rows)
{
 int old_cols, old_rows;

 e_screen_take(cols, rows, &old_cols, &old_rows);
 if (!wpe_grid_ready())
  return -1;
 return 0;
}

/**
 * e_screen_say_clamp - Build the sentence for a size that was cut down.
 */
static void e_screen_say_clamp(const WpeGridNotice *notice, char *text,
                               size_t size)
{
 (void) wpe_fmt_shown(text, size, "Screen cut to %d x %d (asked %d x %d)",
                      notice->used_cols, notice->used_rows,
                      notice->asked_cols, notice->asked_rows);
}

/**
 * e_screen_say_refusal - Build the sentence for a size the memory refused.
 */
static void e_screen_say_refusal(const WpeGridNotice *notice, char *text,
                                 size_t size)
{
 (void) wpe_fmt_shown(text, size, "%s for %d x %d, keeping %d x %d",
                      e_msg[ERR_LOWMEM], notice->asked_cols,
                      notice->asked_rows, notice->used_cols,
                      notice->used_rows);
}

/**
 * e_screen_can_say - Whether a box can be shown on the desk now.
 *
 * Return: 1 when the top window exists and can carry a box, else 0.  Until
 * then a notice waits in the owner and is said at the first key read after
 * the desk is up.
 */
static int e_screen_can_say(void)
{
 return WpeEditor != NULL && WpeEditor->mxedt >= 1 &&
        WpeEditor->f[WpeEditor->mxedt] != NULL;
}

/**
 * e_screen_say_pending - Tell the user once about a screen size that was cut
 * down or refused.
 *
 * Called by the three key readers: at the top of a read, and again right
 * after the size a display just handed over was taken, so a size the grid
 * kept is said at once and not at the user's next key.  Both are points where
 * nothing is being drawn; this is never called from a paint.  What the user
 * sees: a box that names the size asked for and the size the editor uses; the
 * editor keeps working on it.  Nothing is said until the desk exists; the
 * notice waits.
 */
void e_screen_say_pending(void)
{
 WpeGridNotice notice;
 char text[GRID_NOTICE_SIZE];

 if (!e_screen_can_say())
  return;
 if (wpe_grid_take_notice(&notice) == WPE_GRID_NOTICE_NONE)
  return;
 if (notice.kind == WPE_GRID_NOTICE_REFUSED)
  e_screen_say_refusal(&notice, text, sizeof text);
 else
  e_screen_say_clamp(&notice, text, sizeof text);
 wpe_ui_trace("grid notice %s", text);
 e_error(text, 0, WpeEditor->f[WpeEditor->mxedt]->fb);
}
