#ifndef WPE_MOUSEDRAIN_H
#define WPE_MOUSEDRAIN_H

/* Emptying curses' mouse event queue.  Kept in its own file, with no curses
   or editor headers, so the loop can be unit tested against a fake queue. */

/** Most events ncurses keeps queued: its circular buffer (EV_MAX in
    lib_mouse.c) holds 8.  No queue of any curses holds more than a few, so a
    drain that has read this many has read everything there can be. */
#define WPE_MOUSE_QUEUE_MAX 8

/**
 * wpe_mouse_pull - Take one event off the curses queue.
 * @arg: caller's private pointer.
 *
 * Return: 1 for a real event (some button state), 0 for an empty event, -1
 * when the queue is empty.  Supplied by the caller: the editor wraps
 * getmouse(), a test answers from a fake.
 */
typedef int (*wpe_mouse_pull)(void *arg);

/**
 * wpe_mouse_drain - Throw away mouse events that arrived before a dialog
 * closed.
 * @pull: reads one event.
 * @arg:  passed through to @pull.
 *
 * After an option box or message closes, clicks made while it was open must
 * not act on the editor underneath.  Stops at the first empty queue, at the
 * first empty event, or after WPE_MOUSE_QUEUE_MAX events, so a curses whose
 * getmouse() keeps answering "ok, nothing" (OpenBSD's base curses) cannot
 * leave the editor spinning and deaf to keys.
 *
 * Return: how many real events were thrown away.
 */
int wpe_mouse_drain(wpe_mouse_pull pull, void *arg);

#endif
