/* we_paint.c -- the screen has one owner: who may draw, and when.
   See we_paint.h.

   One depth says how many boxes own the screen; nothing else in the editor
   keeps a flag for it, so a box that opened and closed another box inside
   itself is still the screen's owner afterwards.  Paints asked for while
   the editor is not idle wait in a short queue and are run, in the order
   they were asked for, by the key loop before it waits for the next key:
   the one point that is outside every box, however the user got back. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "we_paint.h"
#include "we_fdloop.h"

typedef struct
{
 WpePaintReplay fn;
 void          *data;
 const char    *what;
} WpePaintEntry;

static WpePaintDesk  paint_desk;
static int           paint_desk_known = 0;
static int           paint_depth = 0;
static int           paint_running = 0;
static int           paint_chrome_off = 0;
static int           paint_callbacks = 0;
static WpePaintEntry paint_queue[WPE_PAINT_DEFER_MAX];
static int           paint_queued = 0;
static const char   *paint_lost_what = NULL;

static void wpe_paint_callback_enter(void);
static void wpe_paint_callback_leave(void);

/**
 * wpe_paint_trace - Write one line to the file $XWPE_UI_TRACE names.
 *
 * A test, or a user chasing a screen that did not change, reads there which
 * box held the screen and which paint waited for it.  Nothing is written
 * when the variable is not set.
 */
static void wpe_paint_trace(const char *fmt, ...)
{
 const char *path = getenv("XWPE_UI_TRACE");
 FILE *tf;
 va_list ap;

 if (!path || !(tf = fopen(path, "a")))
  return;
 va_start(ap, fmt);
 vfprintf(tf, fmt, ap);
 va_end(ap);
 fputc('\n', tf);
 fclose(tf);
}

/**
 * wpe_paint_trace_say - Write a line the editor said on the status row to
 * the trace.
 * @line: what was said.
 *
 * A test, or a user chasing a "why did it stop", reads there what the
 * editor told about a command it waited for or ended.
 */
void wpe_paint_trace_say(const char *line)
{
 wpe_paint_trace("say %s", line);
}

/* What a trace line calls a box or a paint whose caller gave no name. */
#define WPE_PAINT_UNNAMED "?"

/**
 * wpe_paint_label - The name a trace line gives a box or a paint.
 * @what: the caller's name for it, or NULL.
 *
 * Every line of the trace names what it is about, so a user reading it
 * can tell which box held the screen even when its caller named none.
 */
static const char *wpe_paint_label(const char *what)
{
 if (what)
  return what;
 return WPE_PAINT_UNNAMED;
}

/**
 * wpe_paint_desk_set - Tell the owner how to see the editor's desk.
 * @desk: the hooks, copied.
 *
 * Given once when the editor starts; until then nothing is idle, so a
 * paint asked for that early waits for the key loop instead of drawing on
 * a screen that is not set up.
 */
void wpe_paint_desk_set(const WpePaintDesk *desk)
{
 paint_desk = *desk;
 paint_desk_known = 1;
 wpe_loop_hooks_set(wpe_paint_callback_enter, wpe_paint_callback_leave);
}

/**
 * wpe_paint_text_on_top - Is a text window on top of the desk?
 */
static int wpe_paint_text_on_top(void)
{
 if (!paint_desk_known || !paint_desk.text_on_top)
  return 0;
 return paint_desk.text_on_top();
}

/**
 * wpe_paint_idle - May content be drawn now?
 *
 * True when no box, menu or prefix owns the screen, no command has the
 * terminal (the editor is not inside its nested wait), and a text window is
 * on top.  Every painter that draws from outside the user's own keys asks
 * this one question, so a reply, a stop or a line of output never lands
 * under a box, whoever opened it.
 * Return: non-zero when idle.
 */
int wpe_paint_idle(void)
{
 if (paint_depth > 0)
  return 0;
 if (wpe_fd_waiting_for() != NULL)
  return 0;
 return wpe_paint_text_on_top();
}

/**
 * wpe_paint_depth_up - One box more owns the screen.
 * @what: names it in the trace.
 */
static void wpe_paint_depth_up(const char *what)
{
 paint_depth++;
 if (paint_depth > WPE_PAINT_DEPTH_MAX)
  wpe_paint_trace("modal depth over %d at %s", WPE_PAINT_DEPTH_MAX,
                  wpe_paint_label(what));
 wpe_paint_trace("modal enter depth=%d %s", paint_depth,
                 wpe_paint_label(what));
}

/**
 * wpe_paint_modal_enter - A box, a menu or a prefix takes the screen.
 * @what: names it in the trace.
 *
 * Until the matching wpe_paint_modal_leave, paints wait.  A status line
 * kept until the next key gives way: the box is what the user reads next.
 */
void wpe_paint_modal_enter(const char *what)
{
 if (paint_desk_known && paint_desk.box_opens)
  paint_desk.box_opens();
 wpe_paint_depth_up(what);
}

/**
 * wpe_paint_modal_leave - One box less.
 *
 * Nothing is drawn here: a box that reads one key at a time comes back
 * here between two keys while it is still on the screen, and a paint now
 * would land on it.  What waited is drawn by the key loop.
 */
void wpe_paint_modal_leave(void)
{
 if (paint_depth == 0)
 {
  wpe_paint_trace("modal leave unbalanced");
  return;
 }
 paint_depth--;
 wpe_paint_trace("modal leave depth=%d", paint_depth);
}

/**
 * wpe_paint_modal_depth - How many boxes own the screen.
 *
 * For what is drawn over every window rather than in one (the scrollbar
 * overlay, the assistant's pane): it stays off while a box is up.
 */
int wpe_paint_modal_depth(void)
{
 return paint_depth;
}

/**
 * wpe_paint_modal_getch - Read one key for a box that is on the screen.
 *
 * The one way a box reads a key: while the key is awaited, output, replies
 * and answers wait instead of drawing under the box.  Reading the next key
 * is not a new box opening, so a status line kept for the user to read
 * stays until that key is pressed, however many keys the menu bar or the
 * Size/Move loop reads.
 * Return: the key.
 */
int wpe_paint_modal_getch(void)
{
 int c = 0;

 wpe_paint_depth_up("a key read by a box");
 if (paint_desk_known && paint_desk.read_key)
  c = paint_desk.read_key();
 wpe_paint_modal_leave();
 return c;
}

/**
 * wpe_paint_take_key - Take a key the user has typed, without waiting.
 * @want: WPE_PAINT_ANY_KEY for the first key, whatever it is; or one key
 * (WPE_ESC), taken only if it was typed, every other key left where it is.
 *
 * The one way a loop that must not wait looks at the keyboard: the Run
 * loop passes the user's typing to the program, and a short command the
 * editor waits for is ended by Esc while the keys typed before it are
 * still the editor's afterwards.  The same on the terminal, X11 and
 * Wayland, because each display answers it through its own table entry.
 * Nothing is waited for, so no output or reply can arrive meanwhile.
 * Return: the key taken, 0 for none.
 */
int wpe_paint_take_key(int want)
{
 if (!paint_desk_known || !paint_desk.take_key)
  return 0;
 return paint_desk.take_key(want);
}

/**
 * wpe_paint_find - The queue slot of the paint (@fn, @data), or -1.
 */
static int wpe_paint_find(WpePaintReplay fn, void *data)
{
 int i;

 for (i = 0; i < paint_queued; i++)
 {
  if (paint_queue[i].fn == fn && paint_queue[i].data == data)
   return i;
 }
 return -1;
}

/**
 * wpe_paint_keep - Keep a paint for the key loop.
 *
 * The same paint asked for twice is kept once, in its first place: the
 * user sees the latest state once, not every step.  A full queue refuses
 * it; the first refused one is named on the status row later.
 * Return: WPE_PAINT_DEFERRED, or WPE_PAINT_REFUSED.
 */
static WpePaintVerdict wpe_paint_keep(WpePaintReplay fn, void *data,
                                      const char *what)
{
 if (wpe_paint_find(fn, data) >= 0)
 {
  wpe_paint_trace("paint deferred %s (already kept)", wpe_paint_label(what));
  return WPE_PAINT_DEFERRED;
 }
 if (paint_queued >= WPE_PAINT_DEFER_MAX)
 {
  if (!paint_lost_what)
   paint_lost_what = what ? what : "an update";
  wpe_paint_trace("paint refused %s", wpe_paint_label(what));
  return WPE_PAINT_REFUSED;
 }
 paint_queue[paint_queued].fn = fn;
 paint_queue[paint_queued].data = data;
 paint_queue[paint_queued].what = what;
 paint_queued++;
 wpe_paint_trace("paint deferred %s", wpe_paint_label(what));
 return WPE_PAINT_DEFERRED;
}

/**
 * wpe_paint_take_first - Take the oldest kept paint off the queue.
 * @out: receives it.
 */
static void wpe_paint_take_first(WpePaintEntry *out)
{
 int i;

 *out = paint_queue[0];
 for (i = 1; i < paint_queued; i++)
  paint_queue[i - 1] = paint_queue[i];
 paint_queued--;
}

/**
 * wpe_paint_say_lost - Name, once, the paint the full queue refused.
 *
 * Said only once the queue is empty and the editor idle, so the line is
 * not drawn under a box and is not overwritten by a paint that follows.
 */
static void wpe_paint_say_lost(void)
{
 const char *what = paint_lost_what;

 if (!what || paint_queued > 0)
  return;
 paint_lost_what = NULL;
 if (paint_desk_known && paint_desk.say_lost)
  paint_desk.say_lost(what);
}

/**
 * wpe_paint_drain - Run what was kept, oldest first, while the editor
 * stays idle.
 *
 * Each paint is taken off before it runs, so one that asks for itself
 * again is kept again; one that opens a box leaves the rest for the next
 * time the key loop comes round.
 */
static void wpe_paint_drain(void)
{
 WpePaintEntry e;
 int runs = 0;

 while (paint_queued > 0 && wpe_paint_idle())
 {
  if (runs >= WPE_PAINT_REPLAY_MAX)
  {
   wpe_paint_trace("paint replay bound reached");
   return;
  }
  wpe_paint_take_first(&e);
  wpe_paint_trace("paint replay %s", wpe_paint_label(e.what));
  e.fn(e.data);
  runs++;
 }
 if (wpe_paint_idle())
  wpe_paint_say_lost();
}

/**
 * wpe_paint_run_now - Run a paint while the editor is idle, then what it
 * asked for meanwhile.
 */
static void wpe_paint_run_now(WpePaintReplay fn, void *data)
{
 paint_running = 1;
 fn(data);
 wpe_paint_drain();
 paint_running = 0;
}

/**
 * wpe_paint_request - Draw now if the editor is idle, else when it is.
 * @fn: the paint; @data: its argument, also what tells two paints of the
 * same function apart; @what: names it in the trace and on the status row,
 * a string that lives as long as the editor.
 *
 * Program output, a stop, a reply or an answer that arrives while a box is
 * up is kept and shown when the box closes, on the window the user then
 * sees.  A paint asked for while another runs is run right after it, never
 * inside it.
 * Return: WPE_PAINT_NOW, WPE_PAINT_DEFERRED or WPE_PAINT_REFUSED.
 */
WpePaintVerdict wpe_paint_request(WpePaintReplay fn, void *data,
                                  const char *what)
{
 if (!fn)
  return WPE_PAINT_REFUSED;
 if (paint_running || !wpe_paint_idle())
  return wpe_paint_keep(fn, data, what);
 wpe_paint_run_now(fn, data);
 return WPE_PAINT_NOW;
}

/**
 * wpe_paint_replay_if_idle - Show what waited behind a box.
 *
 * The key loop calls this before it waits for a key.  The user then sees
 * the build's end, the stop, the reply or the output that came while the
 * box was up, before the next key can be taken for something else.
 */
void wpe_paint_replay_if_idle(void)
{
 if (paint_running || !wpe_paint_idle())
  return;
 paint_running = 1;
 wpe_paint_drain();
 paint_running = 0;
}

/**
 * wpe_paint_callback_enter - The loop starts a callback (a descriptor's or a
 * timer's).
 *
 * Until the matching leave, wpe_paint_in_callback says so, and the paints the
 * callback asks for while a box, a run or a command has the screen are kept.
 */
static void wpe_paint_callback_enter(void)
{
 paint_callbacks++;
}

/**
 * wpe_paint_callback_leave - A callback of the loop has returned.
 *
 * When it was the outermost one and the editor is idle, what it (or an
 * earlier one) asked to draw while the screen was not the editor's is shown
 * now: the user sees the build's end or the reply the moment it can be
 * drawn, not at the next key.  A callback that ran inside a box's own key
 * wait leaves the paints kept: the key loop shows them when the box closes.
 */
static void wpe_paint_callback_leave(void)
{
 if (paint_callbacks > 0)
  paint_callbacks--;
 if (paint_callbacks > 0)
  return;
 wpe_paint_replay_if_idle();
}

/**
 * wpe_paint_in_callback - Is a callback of the loop running?
 *
 * A message said from a callback while a box owns the screen waits for the
 * box; the same message said from the user's own key is drawn at once.
 * Return: non-zero inside a callback.
 */
int wpe_paint_in_callback(void)
{
 return paint_callbacks > 0;
}

/**
 * wpe_paint_chrome_suppress - Keep the scrollbar overlay off (@on non-zero)
 * or let it draw again.
 *
 * The User Screen and a scrollbar drag own the whole window: the overlay
 * drawn over it would bleed onto the program's screen.
 */
void wpe_paint_chrome_suppress(int on)
{
 paint_chrome_off = on;
}

/** wpe_paint_chrome_suppressed - Is the scrollbar overlay kept off? */
int wpe_paint_chrome_suppressed(void)
{
 return paint_chrome_off;
}
