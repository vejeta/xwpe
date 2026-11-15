/* we_wlframe.c -- the one owner of the size the Wayland surface may commit.
 *
 * See we_wlframe.h for the invariant and the states.  This file is the only
 * one that allocates the wl_shm buffer, answers a configure, attaches and
 * commits the editor's surface, and owns the private event queue the
 * surface's configure events arrive on.  tests/check-wayland-frame.sh fails
 * the build when another file does.
 *
 * Mirror of the X11 backend, and where the platform forces a difference:
 * X11 asks the window manager for whole-cell steps (PResizeInc) and a
 * minimum of 80x24 cells (PMinSize, WeXterm.c) and snaps its back buffer to
 * whole cells (we_xterm.c); the X server paints the rest of a window that is
 * larger than the grid with the window background (BlackPixel).  Wayland has
 * no window apart from the buffer, so this owner sizes the buffer itself,
 * announces the minimum with xdg_toplevel.set_min_size, and paints the
 * remainder strip black, the colour the X11 window shows there.
 */

#ifdef HAVE_WAYLAND

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdint.h>
#include "we_platform.h"     /* memfd_create where the system has it */

#include "we_render.h"       /* WpeRender: the renderer to re-point */
#include "we_wayland.h"      /* WpeWl: the handles this file owns */
#include "we_wlframe.h"

/* The colour of the strip between the last cell and the buffer's edge, and
   of a buffer before the first frame: X11's BlackPixel and Borland blue. */
#define WLF_REMAINDER_XRGB 0xff000000u
#define WLF_SPLASH_XRGB    0xff0000a8u

#define WLF_NAME_SIZE 4096
#define WLF_BYTES_PER_PIXEL 4
#define WLF_COMPOSITOR_DAMAGE_BUFFER 4   /* wl_surface.damage_buffer since v4 */

/* The owner's state. */
static struct {
 WpeWlFrameState state;
 uint32_t serial;            /* the newest configure, acknowledged at the present */
 int have_serial;
 int topl_w, topl_h;         /* the toplevel's size, taken at xdg_surface.configure */
 unsigned topl_states;
 int bound_w, bound_h;       /* the size of the newest configure; 0 = the client decides */
 unsigned states;
 int own_w, own_h;           /* the editor's own size before any configure */
 int cell_w, cell_h;
 int cols, rows;             /* the grid the buffer carries */
 int w, h;                   /* the buffer's size */
 int in_apply;
 int withheld;               /* presents refused while a configure waited (a bug when not 0,
                                outside a wait that left a size change pending) */
 struct wl_event_queue *queue;
 const char *trace_path;
 const WpeWlFrameOps *ops;
} F;

/* ---------------------------------------------------------------------- *\
  Frame trace (XWPE_WL_FRAME_TRACE=/path): one line per event
\* ---------------------------------------------------------------------- */

/**
 * wlf_trace - Append one line to the file $XWPE_WL_FRAME_TRACE names.
 * @fmt: printf format of the line, without the newline.
 *
 * Someone chasing a window that came up at the wrong size, or a compositor
 * that closed the editor, reads there which configure came, which buffer
 * answered it and what was committed.  Nothing is opened when the variable
 * is not set.
 */
static void wlf_trace(const char *fmt, ...)
{
 FILE *tf;
 va_list ap;

 if (!F.trace_path || !*F.trace_path || !(tf = fopen(F.trace_path, "a")))
  return;
 va_start(ap, fmt);
 vfprintf(tf, fmt, ap);
 va_end(ap);
 fputc('\n', tf);
 fclose(tf);
}

/**
 * wlf_state_names - The names of the states of a configure, for the trace.
 * @states: bit set of WPE_WLFRAME_ST_*.
 * @out: receives the comma separated names.
 * @size: size of @out.
 */
static void wlf_state_names(unsigned states, char *out, size_t size)
{
 snprintf(out, size, "%s%s%s%s",
          (states & WPE_WLFRAME_ST_MAXIMIZED) ? "maximized," : "",
          (states & WPE_WLFRAME_ST_FULLSCREEN) ? "fullscreen," : "",
          (states & WPE_WLFRAME_ST_RESIZING) ? "resizing," : "",
          (states & WPE_WLFRAME_ST_ACTIVATED) ? "activated," : "");
}

/* ---------------------------------------------------------------------- *\
  The fit rule
\* ---------------------------------------------------------------------- */

/**
 * wlf_snap - Round a length down to whole cells.
 * @bound: the length the compositor offers, in pixels.
 * @cell: the cell's length.
 */
static int wlf_snap(int bound, int cell)
{
 return (bound / cell) * cell;
}

/**
 * wlf_snap_below_floor - Snap one length to cells, but never above the bound.
 * @bound: the length the compositor offers.
 * @cell: the cell's length.
 * @min_cells: the floor, in cells.
 *
 * A bound smaller than the floor is taken exactly (the grid is then clipped
 * by the buffer); otherwise whole cells, never above the bound.
 */
static int wlf_snap_below_floor(int bound, int cell, int min_cells)
{
 if (bound < min_cells * cell)
  return bound;
 return wlf_snap(bound, cell);
}

/**
 * wlf_snap_at_least_floor - Snap one length to cells, at least the floor.
 * @bound: the length the compositor suggests.
 * @cell: the cell's length.
 * @min_cells: the floor, in cells.
 *
 * For a window the client may size as it likes (a floating window with no
 * state): whole cells, and never below the minimum the editor announced.
 */
static int wlf_snap_at_least_floor(int bound, int cell, int min_cells)
{
 int snapped = wlf_snap(bound, cell);

 if (snapped < min_cells * cell)
  return min_cells * cell;
 return snapped;
}

/**
 * wpe_wlframe_fit_size - The size of the buffer that answers a configure.
 * @bound_w: width the compositor offered, 0 when it leaves the size to us.
 * @bound_h: height the compositor offered, 0 when it leaves the size to us.
 * @states: WPE_WLFRAME_ST_* bits of the configure.
 * @cell_w: width of one cell, pixels.
 * @cell_h: height of one cell, pixels.
 * @own_w: width of the editor's own size (its grid times the cell).
 * @own_h: height of the editor's own size.
 * @out_w: receives the buffer's width.
 * @out_h: receives the buffer's height.
 *
 * What the user gets: a maximized or fullscreen window is filled to its
 * edge (the compositor requires it: weston closes an editor whose maximized
 * buffer is not the configured size, and one whose fullscreen buffer is
 * larger), the strip past the last cell being painted black as X11 shows
 * it; a window being dragged keeps its whole-cell steps and is never larger
 * than the drag offers; a floating window is sized in whole cells, at least
 * the announced minimum; and no suggestion at all means the editor's own
 * size.  The protocol says so: MAXIMIZED "must be obeyed", FULLSCREEN is "a
 * maximum" whose geometry must be obeyed to cover the area, and RESIZING is
 * "a maximum ... clients that have aspect ratio or cell sizing
 * configuration can use a smaller size"; a floating size is a hint the
 * client may decline.
 */
void wpe_wlframe_fit_size(int bound_w, int bound_h, unsigned states,
                          int cell_w, int cell_h, int own_w, int own_h,
                          int *out_w, int *out_h)
{
 if (bound_w <= 0 || bound_h <= 0)
 {
  *out_w = own_w;
  *out_h = own_h;
  return;
 }
 if (cell_w <= 0 || cell_h <= 0 ||
     (states & (WPE_WLFRAME_ST_MAXIMIZED | WPE_WLFRAME_ST_FULLSCREEN)))
 {
  *out_w = bound_w;
  *out_h = bound_h;
  return;
 }
 if (states & WPE_WLFRAME_ST_RESIZING)
 {
  *out_w = wlf_snap_below_floor(bound_w, cell_w, WPE_WLFRAME_MIN_COLS);
  *out_h = wlf_snap_below_floor(bound_h, cell_h, WPE_WLFRAME_MIN_ROWS);
  return;
 }
 *out_w = wlf_snap_at_least_floor(bound_w, cell_w, WPE_WLFRAME_MIN_COLS);
 *out_h = wlf_snap_at_least_floor(bound_h, cell_h, WPE_WLFRAME_MIN_ROWS);
}

/**
 * wlf_grid_of - The grid a buffer of a size carries.
 * @w: buffer width.
 * @h: buffer height.
 * @cols: receives the columns, at least the floor.
 * @rows: receives the rows, at least the floor.
 */
static void wlf_grid_of(int w, int h, int *cols, int *rows)
{
 if (F.cell_w <= 0 || F.cell_h <= 0)
 {
  *cols = F.cols;
  *rows = F.rows;
  return;
 }
 *cols = w / F.cell_w;
 *rows = h / F.cell_h;
 if (*cols < WPE_WLFRAME_MIN_COLS)
  *cols = WPE_WLFRAME_MIN_COLS;
 if (*rows < WPE_WLFRAME_MIN_ROWS)
  *rows = WPE_WLFRAME_MIN_ROWS;
}

/**
 * wlf_target_size - The buffer size the newest configure asks for.
 * @cols: the grid to size by when the compositor leaves the size to us.
 * @rows: likewise.
 * @w: receives the width.
 * @h: receives the height.
 */
static void wlf_target_size(int cols, int rows, int *w, int *h)
{
 int own_w = cols * F.cell_w;
 int own_h = rows * F.cell_h;

 if (F.cell_w <= 0 || F.cell_h <= 0)
 {
  own_w = F.own_w;
  own_h = F.own_h;
 }
 wpe_wlframe_fit_size(F.bound_w, F.bound_h, F.states, F.cell_w, F.cell_h,
                      own_w, own_h, w, h);
}

/* ---------------------------------------------------------------------- *\
  The compositor's table: the default operations
\* ---------------------------------------------------------------------- */

/**
 * wlf_anon_file - Create an anonymous, close-on-exec, sized file for a pool.
 * @size: its size in bytes.
 *
 * Prefers memfd (no filesystem name at all) and falls back to an unlinked
 * temporary file in XDG_RUNTIME_DIR where the system has no memfd.
 * Return: the descriptor, or -1.
 */
static int wlf_anon_file(size_t size)
{
 int fd = -1;

#if WPE_HAS_MEMFD_CREATE
 fd = memfd_create("xwpe-wl-shm", MFD_CLOEXEC);
#endif
 if (fd < 0)
 {
  const char *dir = getenv("XDG_RUNTIME_DIR");
  char name[WLF_NAME_SIZE];

  if (!dir || !*dir)
   dir = "/tmp";
  snprintf(name, sizeof name, "%s/xwpe-wl-XXXXXX", dir);
  fd = mkostemp(name, O_CLOEXEC);
  if (fd >= 0)
   unlink(name);
 }
 if (fd < 0)
  return -1;
 if (ftruncate(fd, (off_t)size) < 0)
 {
  close(fd);
  return -1;
 }
 return fd;
}

/**
 * wlf_release_buffer - Give back the shm buffer, its mapping and its file.
 *
 * Called before a new buffer is made and at the end of the editor.
 */
static void wlf_release_buffer(void)
{
 if (WpeWl.buffer)
 {
  wl_buffer_destroy(WpeWl.buffer);
  WpeWl.buffer = NULL;
 }
 if (WpeWl.pixels && WpeWl.pixels != MAP_FAILED)
  munmap(WpeWl.pixels, WpeWl.shm_size);
 WpeWl.pixels = NULL;
 if (WpeWl.shm_fd >= 0)
 {
  close(WpeWl.shm_fd);
  WpeWl.shm_fd = -1;
 }
}

/**
 * wlf_shm_alloc - (Re)allocate the single shm frame buffer at w x h.
 * @w: width in pixels.
 * @h: height in pixels.
 *
 * Maps it and creates the wl_buffer; the pixels start black.
 * Return: 0 on success, -1 when the system refuses.
 */
static int wlf_shm_alloc(int w, int h)
{
 struct wl_shm_pool *pool;
 int stride = w * WLF_BYTES_PER_PIXEL;
 size_t size = (size_t)stride * h;

 wlf_release_buffer();
 WpeWl.shm_fd = wlf_anon_file(size);
 if (WpeWl.shm_fd < 0)
  return -1;
 WpeWl.pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     WpeWl.shm_fd, 0);
 if (WpeWl.pixels == MAP_FAILED)
 {
  WpeWl.pixels = NULL;
  return -1;
 }
 pool = wl_shm_create_pool(WpeWl.shm, WpeWl.shm_fd, (int32_t)size);
 WpeWl.buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                          WL_SHM_FORMAT_XRGB8888);
 wl_shm_pool_destroy(pool);
 WpeWl.width = w;
 WpeWl.height = h;
 WpeWl.stride = stride;
 WpeWl.shm_size = size;
 return 0;
}

/**
 * wlf_wire_ack - Send xdg_surface.ack_configure for a serial.
 * @serial: the configure's serial.
 */
static void wlf_wire_ack(uint32_t serial)
{
 xdg_surface_ack_configure(WpeWl.xdg_surface, serial);
}

/**
 * wlf_wire_frame - Attach the buffer, damage a rectangle and commit.
 * @x: left of the damage, buffer pixels.
 * @y: top of the damage.
 * @w: width of the damage.
 * @h: height of the damage.
 *
 * The one commit of the editor's surface.  Flushes the connection so the
 * compositor sees the frame at once.
 */
static void wlf_wire_frame(int x, int y, int w, int h)
{
 if (!WpeWl.surface || !WpeWl.buffer)
  return;
 wl_surface_attach(WpeWl.surface, WpeWl.buffer, 0, 0);
 if (WpeWl.compositor_version >= WLF_COMPOSITOR_DAMAGE_BUFFER)
  wl_surface_damage_buffer(WpeWl.surface, x, y, w, h);
 else
  wl_surface_damage(WpeWl.surface, x, y, w, h);
 wl_surface_commit(WpeWl.surface);
 if (WpeWl.display)
  wl_display_flush(WpeWl.display);
}

/**
 * wlf_wire_retarget - Point the renderer at the new buffer, if there is one.
 * @w: width in pixels.
 * @h: height in pixels.
 */
static void wlf_wire_retarget(int w, int h)
{
 if (WpeRender.resize)
  WpeRender.resize(w, h);
}

/**
 * wlf_wire_drain - Dispatch the events waiting on the frame queue.
 *
 * Reads nothing from the socket: it only delivers what a read by the key
 * loop's own pump has already queued for the surface.
 */
static void wlf_wire_drain(void)
{
 if (F.queue && WpeWl.display)
  wl_display_dispatch_queue_pending(WpeWl.display, F.queue);
}

static void (*g_repaint_hook)(void);

/**
 * wlf_hook_repaint - Re-render every cell through the editor's hook.
 */
static void wlf_hook_repaint(void)
{
 if (g_repaint_hook && WpeRender.draw_text)
  g_repaint_hook();
}

static const WpeWlFrameOps wlf_compositor_ops = {
 wlf_shm_alloc,
 wlf_wire_ack,
 wlf_wire_frame,
 wlf_wire_retarget,
 wlf_hook_repaint,
 wlf_wire_drain
};

/* ---------------------------------------------------------------------- *\
  Bring-up
\* ---------------------------------------------------------------------- */

/**
 * wpe_wlframe_use_ops - Replace the owner's operations (the unit test's seam).
 * @ops: the table, or NULL for the compositor's own.
 */
void wpe_wlframe_use_ops(const WpeWlFrameOps *ops)
{
 F.ops = ops ? ops : &wlf_compositor_ops;
}

/**
 * wpe_wlframe_init - Start the owner: UNBOUND, with the editor's own size.
 * @own_w: width the window has until the compositor says otherwise.
 * @own_h: height likewise.
 * @cell_w: width of one cell.
 * @cell_h: height of one cell.
 *
 * Called once, before the window is created.
 */
void wpe_wlframe_init(int own_w, int own_h, int cell_w, int cell_h)
{
 memset(&F, 0, sizeof F);
 F.ops = &wlf_compositor_ops;
 F.own_w = own_w;
 F.own_h = own_h;
 F.cell_w = cell_w;
 F.cell_h = cell_h;
 wlf_grid_of(own_w, own_h, &F.cols, &F.rows);
 F.trace_path = getenv("XWPE_WL_FRAME_TRACE");
 g_repaint_hook = NULL;
}

/**
 * wpe_wlframe_set_cell - Tell the owner the real cell once the font is loaded.
 * @cell_w: width of one cell.
 * @cell_h: height of one cell.
 */
void wpe_wlframe_set_cell(int cell_w, int cell_h)
{
 F.cell_w = cell_w;
 F.cell_h = cell_h;
 if (F.w > 0 && F.h > 0)
  wlf_grid_of(F.w, F.h, &F.cols, &F.rows);
}

/**
 * wpe_wlframe_set_repaint - Give the owner the way to re-render every cell.
 * @repaint: the editor's full repaint of its screen cells.
 *
 * Used when the buffer was replaced without the grid changing: a user who
 * nudges a maximized window by a few pixels still sees the whole editor,
 * not a black frame.
 */
void wpe_wlframe_set_repaint(void (*repaint)(void))
{
 g_repaint_hook = repaint;
}

/**
 * wpe_wlframe_claim_proxy - Move a surface object's events to the frame queue.
 * @proxy: the xdg_surface, or the xdg_toplevel.
 *
 * Their configure events then wait there until wpe_wlframe_apply or
 * wpe_wlframe_serve dispatch them, whoever reads the connection.  The
 * queue is made on the first call.
 */
void wpe_wlframe_claim_proxy(void *proxy)
{
 if (!F.queue)
  F.queue = wl_display_create_queue(WpeWl.display);
 wl_proxy_set_queue((struct wl_proxy *)proxy, F.queue);
}

/**
 * wpe_wlframe_read_events - Read the connection once, never waiting for a
 * particular event.
 *
 * What the user gets: the editor answers the compositor's resize even when
 * nothing else arrives with it, and still serves its end request.  Called
 * when the connection is known to be readable.  libwayland's own dispatch
 * call is not used on purpose: when a read brings only events for the frame
 * queue (a configure that comes alone) it polls again until an event for the
 * default queue arrives, and the editor would sit in it with its window
 * unanswered and its end request never served.  If the default queue already
 * holds events nothing is read: the caller dispatches them first and the data
 * waits in the socket for the next pump.  The events read are queued; the
 * caller dispatches its own queue.
 */
void wpe_wlframe_read_events(void)
{
 if (wl_display_prepare_read(WpeWl.display) == 0)
  wl_display_read_events(WpeWl.display);
}

/**
 * wpe_wlframe_dispatch_frame_events - Deliver what waits on the frame queue.
 *
 * Only the top-level points (apply, serve) do this in the editor; exposed so
 * a test can show that a read left the events queued and delivers them when
 * it chooses.
 */
void wpe_wlframe_dispatch_frame_events(void)
{
 wlf_wire_drain();
}

/**
 * wpe_wlframe_announce_floor - Tell the compositor the smallest window.
 *
 * The compositor then stops offering sizes below 30x6 cells, as X11's
 * PMinSize does for its window manager.
 */
void wpe_wlframe_announce_floor(void)
{
 if (F.cell_w <= 0 || F.cell_h <= 0 || !WpeWl.xdg_toplevel)
  return;
 xdg_toplevel_set_min_size(WpeWl.xdg_toplevel,
                           WPE_WLFRAME_MIN_COLS * F.cell_w,
                           WPE_WLFRAME_MIN_ROWS * F.cell_h);
}

/**
 * wpe_wlframe_request_configure - The role-giving first commit.
 *
 * A surface with no buffer is committed once so the compositor sends its
 * first configure.
 */
void wpe_wlframe_request_configure(void)
{
 wl_surface_commit(WpeWl.surface);
}

/**
 * wpe_wlframe_wait_configure - Wait for the compositor's first configure.
 *
 * Reads the connection until the surface's first configure has arrived on
 * the frame queue.  Used only during bring-up, before the editor's loop.
 * Return: 0 when a configure is pending, -1 when the window was closed or
 * the connection failed (the caller then falls back to X11).
 */
int wpe_wlframe_wait_configure(void)
{
 while (WpeWl.running && F.state != WPE_WLFRAME_PENDING)
 {
  if (wl_display_dispatch_queue(WpeWl.display, F.queue) == -1)
   return -1;
 }
 return F.state == WPE_WLFRAME_PENDING ? 0 : -1;
}

/**
 * wpe_wlframe_forget_buffer - Mark the shm handles as holding nothing.
 *
 * Called right after the shared state is zeroed, so the first buffer is made
 * (and an end of the editor releases) a file that is not descriptor 0.
 */
void wpe_wlframe_forget_buffer(void)
{
 WpeWl.shm_fd = -1;
}

/**
 * wpe_wlframe_teardown_buffer - Give the shm buffer back at the editor's end.
 */
void wpe_wlframe_teardown_buffer(void)
{
 wlf_release_buffer();
}

/**
 * wpe_wlframe_teardown_queue - Drop the frame queue and the state.
 *
 * Called after the xdg objects that used the queue are destroyed.
 */
void wpe_wlframe_teardown_queue(void)
{
 if (F.queue)
  wl_event_queue_destroy(F.queue);
 F.queue = NULL;
 F.state = WPE_WLFRAME_UNBOUND;
}

/* ---------------------------------------------------------------------- *\
  The compositor's events: record only
\* ---------------------------------------------------------------------- */

/**
 * wlf_states_of - Fold a configure's states array into WPE_WLFRAME_ST_* bits.
 * @states: the xdg_toplevel.configure states array.
 */
static unsigned wlf_states_of(const struct wl_array *states)
{
 unsigned bits = 0;
 const uint32_t *s;

 if (!states)
  return 0;
 wl_array_for_each(s, states)
 {
  if (*s == XDG_TOPLEVEL_STATE_MAXIMIZED)
   bits |= WPE_WLFRAME_ST_MAXIMIZED;
  else if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN)
   bits |= WPE_WLFRAME_ST_FULLSCREEN;
  else if (*s == XDG_TOPLEVEL_STATE_RESIZING)
   bits |= WPE_WLFRAME_ST_RESIZING;
  else if (*s == XDG_TOPLEVEL_STATE_ACTIVATED)
   bits |= WPE_WLFRAME_ST_ACTIVATED;
 }
 return bits;
}

/**
 * wpe_wlframe_note_toplevel - Record the size and states of a configure.
 * @w: width the compositor offers, 0 for no suggestion.
 * @h: height likewise.
 * @states: the xdg_toplevel states.
 *
 * Record only: the size takes effect when the surface's configure
 * completes the sequence.
 */
void wpe_wlframe_note_toplevel(int32_t w, int32_t h, const struct wl_array *states)
{
 F.topl_w = w;
 F.topl_h = h;
 F.topl_states = wlf_states_of(states);
}

/**
 * wpe_wlframe_note_configure - A configure sequence is complete.
 * @serial: its serial.
 *
 * Record only: the owner is now PENDING, and a later configure that arrives
 * before this one is applied replaces it (only the newest is answered).
 */
void wpe_wlframe_note_configure(uint32_t serial)
{
 char names[64];

 F.serial = serial;
 F.have_serial = 1;
 F.bound_w = F.topl_w;
 F.bound_h = F.topl_h;
 F.states = F.topl_states;
 F.state = WPE_WLFRAME_PENDING;
 wlf_state_names(F.states, names, sizeof names);
 wlf_trace("configure serial=%u size=%dx%d states=%s", serial, F.bound_w,
           F.bound_h, names);
}

/* ---------------------------------------------------------------------- *\
  The one place a configure is taken
\* ---------------------------------------------------------------------- */

/**
 * wpe_wlframe_pending - Does a configure wait for a top-level point?
 */
int wpe_wlframe_pending(void)
{
 return F.state == WPE_WLFRAME_PENDING;
}

/**
 * wpe_wlframe_shown - Is the window showing a frame that answered the newest configure?
 *
 * True from the first acknowledged commit until the next configure arrives
 * (state APPLIED); false before the first frame and while a newer configure
 * waits.  Bring-up asks it to know the first frame is on screen.
 */
int wpe_wlframe_shown(void)
{
 return F.state == WPE_WLFRAME_APPLIED;
}

/**
 * wpe_wlframe_state - The owner's state, for the trace and the tests.
 */
WpeWlFrameState wpe_wlframe_state(void)
{
 return F.state;
}

/**
 * wpe_wlframe_withheld - How many presents were refused while a configure waited.
 *
 * Not 0 is a bug: the frame queue is dispatched only at top-level points, so
 * nothing should present between a configure and its application.  The one
 * exception is a wait (File > Shell) that left a size change pending: the
 * repaint that closes it is withheld, and the next apply shows the frame again.
 */
int wpe_wlframe_withheld(void)
{
 return F.withheld;
}

/**
 * wlf_fill - Fill the whole buffer with one XRGB value.
 * @xrgb: the value.
 */
static void wlf_fill(uint32_t xrgb)
{
 size_t n = (size_t)WpeWl.width * WpeWl.height;
 size_t i;

 if (!WpeWl.pixels)
  return;
 for (i = 0; i < n; i++)
  WpeWl.pixels[i] = xrgb;
}

/**
 * wpe_wlframe_paint_flat - Fill the buffer with one colour (the bring-up splash).
 * @xrgb: the colour.
 */
void wpe_wlframe_paint_flat(uint32_t xrgb)
{
 wlf_fill(xrgb);
}

/**
 * wlf_remainder_start - The first pixel of a row that lies outside the grid.
 * @y: the pixel row.
 * @grid_w: width of the grid in pixels.
 * @grid_h: height of the grid in pixels.
 *
 * Rows below the grid are remainder from their first pixel; rows beside it
 * from the grid's right edge.
 */
static int wlf_remainder_start(int y, int grid_w, int grid_h)
{
 if (y < grid_h)
  return grid_w;
 return 0;
}

/**
 * wpe_wlframe_paint_remainder - Paint black the strip past the last cell.
 *
 * A window filled to its edge (maximized, fullscreen) is rarely a whole
 * number of cells; the strip to the right of and below the grid is black,
 * as the X server's window background shows it under X11.
 */
void wpe_wlframe_paint_remainder(void)
{
 int x, y, from, grid_w = F.cols * F.cell_w, grid_h = F.rows * F.cell_h;

 if (!WpeWl.pixels || F.cell_w <= 0 || F.cell_h <= 0)
  return;
 for (y = 0; y < WpeWl.height; y++)
 {
  from = wlf_remainder_start(y, grid_w, grid_h);
  for (x = from; x < WpeWl.width; x++)
   WpeWl.pixels[(size_t)y * WpeWl.width + x] = WLF_REMAINDER_XRGB;
 }
}

/**
 * wlf_resize_buffer - Replace the buffer by one of a size and re-point the renderer.
 * @w: width.
 * @h: height.
 *
 * Return: 0 on success; on failure the old buffer is gone and the owner
 * stays PENDING so the next top-level point tries again.
 */
static int wlf_resize_buffer(int w, int h)
{
 if (F.ops->alloc_buffer(w, h) != 0)
  return -1;
 F.w = w;
 F.h = h;
 F.ops->retarget(w, h);
 return 0;
}

/**
 * wlf_refill_after_new_buffer - Put content in a buffer that has none yet.
 * @repaint_cells: 1 when no relayout follows, so the cells are drawn here.
 *
 * The black remainder first, then, when the editor's renderer exists and
 * nobody else will draw, every cell again; before the renderer exists, the
 * bring-up splash.
 */
static void wlf_refill_after_new_buffer(int repaint_cells)
{
 if (!WpeRender.draw_text)
 {
  wlf_fill(WLF_SPLASH_XRGB);
  return;
 }
 wlf_fill(WLF_REMAINDER_XRGB);
 if (repaint_cells)
  F.ops->repaint();
}

/**
 * wlf_take_configure - Fit the buffer to the newest configure.
 * @cols: receives the grid the buffer carries.
 * @rows: likewise.
 * @reallocated: receives 1 when a new buffer was made.
 *
 * Return: 0 on success, -1 when no buffer could be made.
 */
static int wlf_take_configure(int *cols, int *rows, int *reallocated)
{
 int w, h;

 wlf_target_size(F.cols, F.rows, &w, &h);
 *reallocated = (!WpeWl.pixels || w != F.w || h != F.h);
 if (*reallocated && wlf_resize_buffer(w, h) != 0)
  return -1;
 wlf_grid_of(F.w, F.h, cols, rows);
 return 0;
}

/**
 * wpe_wlframe_apply - Take the newest configure, at a top-level point.
 * @cols: receives the new grid when it changed.
 * @rows: likewise.
 *
 * What the user gets: after a resize the editor shows a frame of the size
 * the window has, and the compositor never sees a buffer that does not
 * answer its configure.  Called only from the top of the key loop and the
 * bring-up, so no repaint is running when the buffer is replaced.
 * Not re-entrant: a call from inside it returns 0.
 *
 * Return: 1 when the grid changed (the caller lays the windows out again
 * and then calls wpe_wlframe_finish), 0 otherwise (a configure that left the
 * grid alone has already been answered here).
 */
int wpe_wlframe_apply(int *cols, int *rows)
{
 int new_cols, new_rows, reallocated, changed;

 if (F.in_apply)
  return 0;
 F.in_apply = 1;
 F.ops->drain();
 if (F.state != WPE_WLFRAME_PENDING || wlf_take_configure(&new_cols, &new_rows, &reallocated) != 0)
 {
  F.in_apply = 0;
  return 0;
 }
 changed = (new_cols != F.cols || new_rows != F.rows);
 F.cols = new_cols;
 F.rows = new_rows;
 F.state = WPE_WLFRAME_FITTED;
 wlf_trace("apply serial=%u buffer=%dx%d grid=%dx%d", F.serial, F.w, F.h, F.cols, F.rows);
 *cols = new_cols;
 *rows = new_rows;
 if (reallocated)
  wlf_refill_after_new_buffer(!changed);
 if (!changed)
  wpe_wlframe_finish();
 F.in_apply = 0;
 return changed;
}

/**
 * wpe_wlframe_serve - Answer a configure while the editor waits for something else.
 *
 * Used while a command runs in a window of its own: a focus or state change
 * that leaves the buffer's size alone is answered at once, so the compositor
 * does not find the window unresponsive.  A change of size is left pending:
 * the key loop applies it, as X11 notes a configure and lays out at its loop
 * top.
 */
void wpe_wlframe_serve(void)
{
 int w, h;

 if (F.in_apply)
  return;
 F.ops->drain();
 if (F.state != WPE_WLFRAME_PENDING)
  return;
 wlf_target_size(F.cols, F.rows, &w, &h);
 if (!WpeWl.pixels || w != F.w || h != F.h)
  return;
 F.state = WPE_WLFRAME_FITTED;
 wpe_wlframe_finish();
}

/**
 * wpe_wlframe_finish - Acknowledge and show a frame that fits but is not shown yet.
 *
 * Called after a relayout, in case it drew nothing that presented.
 */
void wpe_wlframe_finish(void)
{
 if (F.state == WPE_WLFRAME_FITTED)
  wpe_wlframe_present(0, 0, F.w, F.h);
}

/**
 * wpe_wlframe_fit_buffer - Make the buffer the size the current configure asks for.
 * @cols: the grid the layout owner is about to draw.
 * @rows: likewise.
 *
 * The buffer step of the display's resize slot: a relayout that changed the
 * grid finds a buffer that fits the window.  The pixels of a replaced buffer
 * start black; the repaint that follows draws every cell.
 */
void wpe_wlframe_fit_buffer(int cols, int rows)
{
 int w, h;

 wlf_target_size(cols, rows, &w, &h);
 if (WpeWl.pixels && w == F.w && h == F.h)
  return;
 wlf_resize_buffer(w, h);
}

/* ---------------------------------------------------------------------- *\
  The one commit
\* ---------------------------------------------------------------------- */

/**
 * wlf_clip_damage - Keep a damage rectangle inside the buffer.
 * @x: left, changed in place.
 * @y: top.
 * @w: width.
 * @h: height.
 */
static void wlf_clip_damage(int *x, int *y, int *w, int *h)
{
 if (*x < 0) { *w += *x; *x = 0; }
 if (*y < 0) { *h += *y; *y = 0; }
 if (*x + *w > F.w) *w = F.w - *x;
 if (*y + *h > F.h) *h = F.h - *y;
 if (*w < 0) *w = 0;
 if (*h < 0) *h = 0;
}

/**
 * wlf_present_fitted - Acknowledge the configure and commit the whole frame.
 *
 * The acknowledgement and the first conforming commit are one step.
 */
static void wlf_present_fitted(void)
{
 F.ops->send_ack(F.serial);
 wlf_trace("ack serial=%u newest=%u", F.serial, F.serial);
 F.ops->send_frame(0, 0, F.w, F.h);
 wlf_trace("commit serial=%u buffer=%dx%d bound=%dx%d damage=0,0,%d,%d", F.serial,
           F.w, F.h, F.bound_w, F.bound_h, F.w, F.h);
 F.state = WPE_WLFRAME_APPLIED;
}

/**
 * wpe_wlframe_present - Show the buffer: the only commit of the editor's surface.
 * @x: left of the changed area, buffer pixels.
 * @y: top.
 * @w: width.
 * @h: height.
 *
 * What the user gets: every frame the compositor receives fits the window
 * it was asked for.  A fitted frame is acknowledged and committed whole; a
 * shown one commits just the changed area; while a configure waits for the
 * key loop nothing is committed (the application re-presents the whole
 * frame), and the refusal is counted.
 */
void wpe_wlframe_present(int x, int y, int w, int h)
{
 if (!WpeWl.pixels)
  return;
 if (F.state == WPE_WLFRAME_FITTED)
 {
  wlf_present_fitted();
  return;
 }
 if (F.state != WPE_WLFRAME_APPLIED)
 {
  F.withheld++;
  wlf_trace("withheld buffer=%dx%d pending=%dx%d", F.w, F.h, F.bound_w, F.bound_h);
  return;
 }
 wlf_clip_damage(&x, &y, &w, &h);
 F.ops->send_frame(x, y, w, h);
 wlf_trace("commit serial=- buffer=%dx%d bound=%dx%d damage=%d,%d,%d,%d", F.w, F.h,
           F.bound_w, F.bound_h, x, y, w, h);
}

/**
 * wpe_wlframe_grid - The grid the buffer carries.
 * @cols: receives the columns.
 * @rows: receives the rows.
 */
void wpe_wlframe_grid(int *cols, int *rows)
{
 wlf_grid_of(F.w > 0 ? F.w : F.own_w, F.h > 0 ? F.h : F.own_h, cols, rows);
}

/**
 * wpe_wlframe_width - The buffer's width in pixels.
 */
int wpe_wlframe_width(void)
{
 return F.w;
}

/**
 * wpe_wlframe_height - The buffer's height in pixels.
 */
int wpe_wlframe_height(void)
{
 return F.h;
}

#endif /* HAVE_WAYLAND */
