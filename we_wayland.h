/* we_wayland.h -- shared state for the native Wayland backend.
 *
 * Private to the Wayland translation units (we_wayland.c, we_render_wayland.c);
 * the rest of xwpe only ever calls WpeWaylandInit() (declared in we_unix.c).
 * Mirrors the role of WpeXStruct/WpeXInfo for the X11 backend: one struct that
 * bundles the compositor connection, the surface, the shared-memory buffer and
 * the current grid geometry, so both the windowing code and the Cairo/wl_shm
 * renderer work against the same handles.
 */
#ifndef WE_WAYLAND_H
#define WE_WAYLAND_H

#ifdef HAVE_WAYLAND

#include <stddef.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"

typedef struct WpeWlInfo {
 /* Connection + globals (bound from the registry). */
 struct wl_display    *display;
 struct wl_registry   *registry;
 struct wl_compositor *compositor;
 struct wl_shm        *shm;
 struct xdg_wm_base   *wm_base;
 struct wl_seat       *seat;
 uint32_t              compositor_version;

 /* Window objects. */
 struct wl_surface   *surface;
 struct xdg_surface  *xdg_surface;
 struct xdg_toplevel *xdg_toplevel;

 /* Single shared-memory frame buffer (XRGB8888). */
 struct wl_buffer *buffer;
 uint32_t         *pixels;     /* mmap'd, width*height words                 */
 int               shm_fd;
 size_t            shm_size;
 int               width;      /* surface size in pixels                     */
 int               height;
 int               stride;     /* bytes per row (width * 4)                  */

 /* Keyboard (wl_keyboard + xkbcommon keymap/state). */
 struct wl_keyboard *keyboard;
 struct xkb_context *xkb_ctx;
 struct xkb_keymap  *xkb_keymap;
 struct xkb_state   *xkb_state;

 /* Decoded xwpe key codes waiting for e_w_getch.  One wl_keyboard key event
    yields at most one code; a small ring decouples Wayland event dispatch
    from the blocking getch caller (like the X11 path returning one key). */
 int key_q[64];
 int key_head;
 int key_tail;
 int kbd_focus;                /* surface currently has keyboard focus       */

 /* Lifecycle flags. */
 int configured;               /* first xdg_surface.configure acknowledged   */
 int painted;                  /* at least one frame committed               */
 int running;                  /* cleared on xdg_toplevel.close              */
} WpeWlInfo;

extern WpeWlInfo WpeWl;

/* Diagnostic (XWPE_WL_DUMP=path): write the current shm buffer as a binary
   PPM so a headless test can assert on the exact pixels xwpe drew.  Returns 0
   on success.  Always available -- it reads the buffer, no compositor call. */
int wpe_wl_dump_ppm(const char *path);

/* Implemented in we_render_wayland.c: lay a Cairo image surface over the
   wl_shm buffer, load the font, publish the drawing primitives via WpeRender.
   Must be called after the buffer is allocated.  Returns 0 on success. */
int wpe_render_wayland_init(void);

#endif /* HAVE_WAYLAND */
#endif /* WE_WAYLAND_H */
