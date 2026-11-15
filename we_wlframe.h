/* we_wlframe.h -- the one owner of the size the Wayland surface may commit.
 *
 * Private to the Wayland translation units (we_wayland.c, we_render_wayland.c).
 * Nothing else sizes the wl_shm buffer, acknowledges a configure, attaches,
 * damages or commits the editor's surface: this file does.
 *
 * What the user gets: the window opens at the size the compositor gives it
 * (also on a small or fullscreen output), a maximized or fast-shrunk window
 * never makes the compositor close the editor, and a window dragged to a new
 * size always shows a frame that fits it.
 *
 * Invariant: a buffer is committed only if it fits the configure it answers
 * (see wpe_wlframe_fit_size for what "fits" means per window state), and the
 * acknowledgement of a configure and the first conforming commit are one
 * step, taken at a top-level point (the key loop or the bring-up), never
 * inside an event listener.  The configure events of the xdg_surface and the
 * xdg_toplevel come through a private event queue that only
 * wpe_wlframe_apply and wpe_wlframe_serve dispatch, so a nested read of the
 * connection (the pointer poll of a repaint) can never deliver a configure
 * in the middle of a relayout.
 *
 * States: UNBOUND (no configure yet), PENDING (a configure waits for a
 * top-level point), FITTED (the buffer fits, not yet acknowledged and
 * shown), APPLIED (shown; later commits need no acknowledgement).
 */
#ifndef WE_WLFRAME_H
#define WE_WLFRAME_H

#ifdef HAVE_WAYLAND

#include <stdint.h>
#include <wayland-client.h>

/* The smallest grid the editor lays out: below it the windows no longer fit
   their own frames.  Announced to the compositor as the window's minimum. */
#define WPE_WLFRAME_MIN_COLS 30
#define WPE_WLFRAME_MIN_ROWS 6

/* The xdg_toplevel states the owner looks at (bit set built from the
   configure's states array). */
#define WPE_WLFRAME_ST_MAXIMIZED  0x01u
#define WPE_WLFRAME_ST_FULLSCREEN 0x02u
#define WPE_WLFRAME_ST_RESIZING   0x04u
#define WPE_WLFRAME_ST_ACTIVATED  0x08u

typedef enum WpeWlFrameState {
 WPE_WLFRAME_UNBOUND = 0,
 WPE_WLFRAME_PENDING,
 WPE_WLFRAME_FITTED,
 WPE_WLFRAME_APPLIED
} WpeWlFrameState;

/* What the owner asks of the compositor connection and of the renderer.  The
   editor uses the table of the compositor (the default); the unit test
   replaces it with a recorder, so the state machine runs with no compositor. */
typedef struct WpeWlFrameOps {
 int  (*alloc_buffer)(int w, int h);              /* new shm buffer; 0 on success */
 void (*send_ack)(uint32_t serial);               /* xdg_surface.ack_configure    */
 void (*send_frame)(int x, int y, int w, int h);  /* attach + damage + commit     */
 void (*retarget)(int w, int h);                  /* re-point the renderer        */
 void (*repaint)(void);                           /* re-render every cell         */
 void (*drain)(void);                             /* dispatch the frame queue     */
} WpeWlFrameOps;

/* ---- bring-up and teardown ------------------------------------------ */
void wpe_wlframe_init(int own_w, int own_h, int cell_w, int cell_h);
void wpe_wlframe_set_cell(int cell_w, int cell_h);
void wpe_wlframe_set_repaint(void (*repaint)(void));
void wpe_wlframe_use_ops(const WpeWlFrameOps *ops);
void wpe_wlframe_claim_proxy(void *proxy);
void wpe_wlframe_read_events(void);
void wpe_wlframe_dispatch_frame_events(void);
void wpe_wlframe_announce_floor(void);
void wpe_wlframe_request_configure(void);
int  wpe_wlframe_wait_configure(void);
void wpe_wlframe_forget_buffer(void);
void wpe_wlframe_teardown_buffer(void);
void wpe_wlframe_teardown_queue(void);

/* ---- the compositor's events (record only) -------------------------- */
void wpe_wlframe_note_toplevel(int32_t w, int32_t h, const struct wl_array *states);
void wpe_wlframe_note_configure(uint32_t serial);

/* ---- the one place a configure is taken ----------------------------- */
int  wpe_wlframe_pending(void);
int  wpe_wlframe_shown(void);
int  wpe_wlframe_apply(int *cols, int *rows);
void wpe_wlframe_serve(void);
void wpe_wlframe_finish(void);
void wpe_wlframe_fit_buffer(int cols, int rows);

/* ---- the one commit ------------------------------------------------- */
void wpe_wlframe_present(int x, int y, int w, int h);
void wpe_wlframe_paint_flat(uint32_t xrgb);
void wpe_wlframe_paint_remainder(void);

/* ---- queries -------------------------------------------------------- */
void wpe_wlframe_grid(int *cols, int *rows);
int  wpe_wlframe_width(void);
int  wpe_wlframe_height(void);
int  wpe_wlframe_withheld(void);
WpeWlFrameState wpe_wlframe_state(void);

/* The pure fit rule: the buffer size for a configure.  Exposed so the unit
   test can walk its table. */
void wpe_wlframe_fit_size(int bound_w, int bound_h, unsigned states,
                          int cell_w, int cell_h, int own_w, int own_h,
                          int *out_w, int *out_h);

#endif /* HAVE_WAYLAND */
#endif /* WE_WLFRAME_H */
