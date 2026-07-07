/* we_wayland.c -- native Wayland backend for xwpe (graphical mode).
 *
 * xwpe's GUI runs on one of two graphical backends, chosen once at startup by
 * e_pick_gfx_backend() in we_unix.c: the long-standing Xlib backend
 * (we_xterm.c, which also covers XWayland) or this native Wayland client.
 * Both feed the same SCREENCELL grid through the same Cairo+Pango glyph
 * renderer; only the windowing and input transport differ.
 *
 * Wayland exposes no server-side font protocol, so every glyph is rendered
 * client-side into a wl_shm buffer via Cairo -- the very path the X11 backend
 * already uses, minus the X server.
 *
 * Bring-up is phased: this file currently stands up the window (registry,
 * compositor, wl_shm, xdg-shell toplevel) and paints a frame.  The renderer
 * (we_render_wayland.c) and the keyboard/pointer pumps arrive next; until the
 * native surface is interactive, WpeWaylandInit() returns non-zero so the
 * caller falls back to the X11/XWayland backend and `xwpe` stays fully usable.
 */

/* memfd_create()/mkostemp() live behind _GNU_SOURCE; config.h (force-included)
   carries no system headers, so defining it here is still before the first. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifdef HAVE_WAYLAND

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <stdint.h>

#include <poll.h>
#include <wayland-cursor.h>
#include "edit.h"            /* SCREENCELL, schirm/altschirm, MAXSCOL/MAXSLNS,
                                e_gt_char/e_gt_col, the ATTR_ and CELL_ macros
                                (unixmakr.h), the e_u_ and fk_u_ function-pointer
                                slots, MCI/MCA/RD/RE/WBT/ctree, WpeEditor, cur_x,
                                WpeNullFunction/WpeZeroFunction */
#include "we_render.h"
#include "we_wayland.h"
#include "we_fdloop.h"
#include "we_clip.h"        /* e_clip_os_set / e_clip_os_get function pointers */
#include "we_trace.h"

extern int e_codepoint_to_utf8(int cp, unsigned char *out);  /* we_edit.c */
extern int e_refresh();                                      /* editor refresh wrapper */

/* Toolkit-agnostic backend functions reused as-is (audited: they touch only
   schirm / editor globals, never Xlib).  K&R empty-paren externs slot into the
   editor's function pointers without signature friction (-std=gnu17). */
extern int  fk_x_locate(), fk_x_cursor(), fk_x_putchar(), x_bioskey();
extern int  e_x_sys_ini(), e_x_sys_end(), e_x_system();
extern int  e_t_deb_out();
extern void e_setlastpic();
extern int  old_cursor_x, old_cursor_y;   /* defined in we_xterm.c */
extern struct mouse e_mouse;              /* defined in we_main.c   */

static const char *g_wl_uidump;   /* XWPE_WL_UIDUMP: dump an editor frame to PPM */
static int g_wl_dump_after;       /* XWPE_WL_UIDUMP_AFTER: dump after N keys read */
static int g_wl_keys_seen;        /* keys returned to the editor so far          */

/* Pointer state, maintained from wl_pointer events (Wayland has no synchronous
   pointer query like X11's XQueryPointer).  Button mask uses xwpe's codes:
   left=1, middle=2, right=4 -- the values fk_mouse / e_w_getch hand the editor. */
#ifndef BTN_LEFT
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112
#endif
static int g_ptr_px, g_ptr_py;    /* last pointer position, pixels        */
static int g_ptr_btn;             /* currently-held xwpe button mask      */
static int g_mouse_pending;       /* a click/scroll code awaiting e_w_getch */
static const char *g_wl_mousetest;/* XWPE_WL_MOUSETEST: report first mouse code */
static const char *g_wl_dump_path;/* XWPE_WL_DUMP: mirror each frame to this PPM */

WpeWlInfo WpeWl;

/* The xwpe desktop background (classic Borland blue) used for the W1 flat
   paint; once the renderer lands it is replaced by per-cell drawing. */
#define WPE_WL_BG_ARGB 0xff0000a8u   /* XRGB: opaque, R=00 G=00 B=a8         */

/* Default surface size while the font cell is not yet known (W2 sets the real
   cell metrics): 80x24 cells at a nominal 8x16 px cell. */
#define WPE_WL_DEF_COLS 80
#define WPE_WL_DEF_ROWS 24
#define WPE_WL_DEF_CW    8
#define WPE_WL_DEF_CH   16

static uint32_t wl_umin(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* Forward declarations (used before their definitions further down). */
static void e_w_render_dirty_cells(int force);
static void wl_selftest_fill_grid(void);
static void wl_pump_once(int block);

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Shared-memory backing file
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* wl_anon_file - create an anonymous, CLOEXEC, `size`-byte file for a wl_shm
   pool.  Prefers memfd (no filesystem name at all); falls back to an unlinked
   temp file in XDG_RUNTIME_DIR for kernels/libcs without memfd_create. */
static int wl_anon_file(size_t size)
{
 int fd = -1;

#ifdef __linux__
 fd = memfd_create("xwpe-wl-shm", MFD_CLOEXEC);
#endif
 if (fd < 0)
 {
  const char *dir = getenv("XDG_RUNTIME_DIR");
  char name[PATH_MAX];

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

/* wl_alloc_buffer - (re)allocate the single shm frame buffer at w x h and map
   it.  Sets WpeWl.pixels/stride/size and creates the wl_buffer.  Returns 0 on
   success. */
static int wl_alloc_buffer(int w, int h)
{
 struct wl_shm_pool *pool;
 int stride = w * 4;
 size_t size = (size_t)stride * h;

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

 WpeWl.shm_fd = wl_anon_file(size);
 if (WpeWl.shm_fd < 0)
 {
  WPE_TRACE("wayland", "shm: cannot create %zu-byte pool: %s\n", size, strerror(errno));
  return -1;
 }
 WpeWl.pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, WpeWl.shm_fd, 0);
 if (WpeWl.pixels == MAP_FAILED)
 {
  WPE_TRACE("wayland", "shm: mmap failed: %s\n", strerror(errno));
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

/* wl_paint_flat - fill the whole buffer with one XRGB pixel value. */
static void wl_paint_flat(uint32_t argb)
{
 size_t n = (size_t)WpeWl.width * WpeWl.height;
 size_t i;

 if (!WpeWl.pixels)
  return;
 for (i = 0; i < n; i++)
  WpeWl.pixels[i] = argb;
}

/* wpe_wl_dump_ppm - write the current buffer as a binary PPM (P6).  Diagnostic
   hook for headless tests; reads only local memory, issues no compositor call. */
int wpe_wl_dump_ppm(const char *path)
{
 FILE *f;
 int x, y;

 if (!WpeWl.pixels || WpeWl.width <= 0 || WpeWl.height <= 0)
  return -1;
 f = fopen(path, "wb");
 if (!f)
  return -1;

 fprintf(f, "P6\n%d %d\n255\n", WpeWl.width, WpeWl.height);
 for (y = 0; y < WpeWl.height; y++)
 {
  for (x = 0; x < WpeWl.width; x++)
  {
   uint32_t p = WpeWl.pixels[(size_t)y * WpeWl.width + x];
   unsigned char rgb[3];
   rgb[0] = (unsigned char)((p >> 16) & 0xff);  /* R */
   rgb[1] = (unsigned char)((p >> 8) & 0xff);   /* G */
   rgb[2] = (unsigned char)(p & 0xff);          /* B */
   fwrite(rgb, 1, 3, f);
  }
 }
 fclose(f);
 return 0;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Protocol listeners
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
 (void)data;
 xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { wm_base_ping };

static void e_w_render_dirty_cells(int force);   /* full repaint after a realloc */

/* --- Interactive resize (compositor-driven) ---------------------------- *\
   During an interactive resize the compositor streams a configure event per
   pointer motion.  Re-laying-out the editor from inside each callback backs up
   a queue that then plays out in slow motion.  So the configure handlers only
   RECORD the latest requested size (cheap); the grid re-fit + repaint happen
   ONCE per event-dispatch cycle, in wl_apply_pending_resize, coalescing a whole
   burst into a single relayout.  This is the Wayland equivalent of what the X11
   ConfigureNotify handler does by draining the queue to the final size with
   XCheckTypedWindowEvent (we_xterm.c), and of the ncurses KEY_RESIZE drain
   (we_term.c).
\* ----------------------------------------------------------------------- */

static int g_pending_w, g_pending_h;    /* latest size the compositor asked for */
static int g_resize_pending;            /* a size change is waiting to be applied */
static int g_last_conf_w, g_last_conf_h;/* last size actually configured (dedup) */

/* Record the latest requested size (called from xdg_toplevel.configure).  Flags
   a resize only when the size differs from the LAST configured one, so a
   compositor that re-sends the same size (focus/re-map, or its sub-cell output
   size every frame) is not mistaken for a new resize -- otherwise every repeat
   would re-arm the pending flag and stall presentation. */
static void wl_note_configure_size(int w, int h)
{
 if (w <= 0 || h <= 0)
  return;
 if (w == g_last_conf_w && h == g_last_conf_h)
  return;
 g_last_conf_w = w;
 g_last_conf_h = h;
 g_pending_w = w;
 g_pending_h = h;
 g_resize_pending = 1;
}

/* Apply the latest pending size exactly once, after the event-dispatch batch.
   Derives the cell grid from the pixel size (clamped to a usable minimum) and,
   only when the grid actually changes, re-lays-out the windows and repaints --
   e_repaint_desk (WpeIsXwin) reallocates the grid and the wl_shm buffer through
   e_w_ini_size, re-points Cairo, and presents.  When only the sub-cell remainder
   changed, the existing buffer is already correct and we just re-present. */
static void wl_apply_pending_resize(void)
{
 int old_scol, old_slns, cols, rows;

 if (!g_resize_pending)
  return;
 g_resize_pending = 0;
 if (WpeRender.font_width <= 0 || WpeRender.font_height <= 0 || !WpeRender.draw_text)
  return;

 cols = g_pending_w / WpeRender.font_width;
 rows = g_pending_h / WpeRender.font_height;
 if (cols < 30) cols = 30;
 if (rows < 6)  rows = 6;
 old_scol = MAXSCOL;
 old_slns = MAXSLNS;

 /* Only a real change of cell dimensions needs a relayout + repaint (which also
    reallocates the buffer and presents).  A sub-cell size change leaves the grid
    and its buffer correct -- xdg_surface_configure has already re-presented the
    current frame -- so there is nothing to do. */
 if (cols != old_scol || rows != old_slns)
 {
  MAXSCOL = cols;
  MAXSLNS = rows;
  e_relayout_windows(WpeEditor, old_scol, old_slns);
  e_free_all_pics(WpeEditor);
  e_repaint_desk(WpeEditor->f[WpeEditor->mxedt]);
 }
}

/* xdg_surface.configure: the compositor is ready for content.  Acknowledge,
   make sure a buffer exists, and present the current frame.  The expensive part
   of a resize (re-fitting the grid and repainting) is NOT done here: a pending
   size change is applied once, after the whole event batch, by
   wl_apply_pending_resize -- so a burst of configures does not play back in slow
   motion.  When a resize is pending we skip presenting the current (old-size)
   buffer here and let the apply step present the correctly-sized frame. */
static void xdg_surface_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
 (void)data;
 xdg_surface_ack_configure(xs, serial);

 if (!WpeWl.pixels)
 {
  /* First map (or a re-map after the buffer was released): allocate at the
     current size and re-point Cairo, or the next paint fills through a dangling
     pointer into freed memory. */
  if (wl_alloc_buffer(WpeWl.width, WpeWl.height) < 0)
   return;
  if (WpeRender.resize)
   WpeRender.resize(WpeWl.width, WpeWl.height);
 }

 /* Editor up + a real resize queued: do NOT commit the current (old-size) buffer
    against the just-acknowledged new size -- committing a mismatched buffer stalls
    the compositor's configure sequence.  wl_apply_pending_resize (after this
    dispatch batch) reallocates to the new size and presents the correct frame.
    (The very first configure -- configured==0 -- must still paint the bring-up.) */
 if (WpeRender.draw_text && WpeWl.configured && g_resize_pending)
  return;

 /* Only flat-fill as a bring-up splash BEFORE the cell renderer is installed.
    Once it is, the buffer holds the editor's rendered cells and a configure
    (focus/raise/re-map) must re-present them, never repaint over them. */
 if (!WpeRender.draw_text)
  wl_paint_flat(WPE_WL_BG_ARGB);
 wl_surface_attach(WpeWl.surface, WpeWl.buffer, 0, 0);
 if (WpeWl.compositor_version >= 4)
  wl_surface_damage_buffer(WpeWl.surface, 0, 0, WpeWl.width, WpeWl.height);
 else
  wl_surface_damage(WpeWl.surface, 0, 0, WpeWl.width, WpeWl.height);
 wl_surface_commit(WpeWl.surface);

 WpeWl.configured = 1;
 WpeWl.painted = 1;
}
static const struct xdg_surface_listener xdg_surface_listener = { xdg_surface_configure };

/* xdg_toplevel.configure: a size suggestion (and window state).  Record-only --
   just note the requested size (wl_apply_pending_resize acts on it once the
   dispatch batch is done).  Headless and first-map often give 0x0; ignored. */
static void xdg_toplevel_configure(void *data, struct xdg_toplevel *tl,
                                   int32_t w, int32_t h, struct wl_array *states)
{
 (void)data; (void)tl; (void)states;
 wl_note_configure_size(w, h);
}
static void xdg_toplevel_close(void *data, struct xdg_toplevel *tl)
{
 (void)data; (void)tl;
 WpeWl.running = 0;
}
/* configure_bounds (v4) and wm_capabilities (v3) are no-ops here; provided so
   the listener vtable is complete regardless of the bound xdg-shell version. */
static void xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *tl,
                                          int32_t w, int32_t h)
{ (void)data; (void)tl; (void)w; (void)h; }
static void xdg_toplevel_wm_capabilities(void *data, struct xdg_toplevel *tl,
                                         struct wl_array *caps)
{ (void)data; (void)tl; (void)caps; }
static const struct xdg_toplevel_listener xdg_toplevel_listener = {
 xdg_toplevel_configure,
 xdg_toplevel_close,
 xdg_toplevel_configure_bounds,
 xdg_toplevel_wm_capabilities
};

/* Registry: bind the globals xwpe needs.  Versions are capped to what this
   client actually uses so we never request more than we handle. */
static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version)
{
 (void)data;
 if (!strcmp(iface, wl_compositor_interface.name))
 {
  WpeWl.compositor_version = wl_umin(version, 4);
  WpeWl.compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
                                      WpeWl.compositor_version);
 }
 else if (!strcmp(iface, wl_shm_interface.name))
 {
  WpeWl.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
 }
 else if (!strcmp(iface, xdg_wm_base_interface.name))
 {
  WpeWl.wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface,
                                   wl_umin(version, 1));
  xdg_wm_base_add_listener(WpeWl.wm_base, &wm_base_listener, NULL);
 }
 else if (!strcmp(iface, wl_seat_interface.name))
 {
  /* Bound now, driven in the keyboard/pointer phases. */
  WpeWl.seat = wl_registry_bind(reg, name, &wl_seat_interface,
                                wl_umin(version, 5));
 }
 else if (!strcmp(iface, wl_data_device_manager_interface.name))
 {
  WpeWl.ddm = wl_registry_bind(reg, name, &wl_data_device_manager_interface,
                               wl_umin(version, 3));
 }
 else if (!strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name))
 {
  WpeWl.psm = wl_registry_bind(reg, name,
                 &zwp_primary_selection_device_manager_v1_interface, 1);
 }
 else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name))
 {
  WpeWl.deco_manager = wl_registry_bind(reg, name,
                 &zxdg_decoration_manager_v1_interface, 1);
 }
}
static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{ (void)data; (void)reg; (void)name; }
static const struct wl_registry_listener registry_listener = {
 registry_global, registry_global_remove
};

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Keyboard (wl_keyboard + xkbcommon)
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define WL_KEYQ_LEN ((int)(sizeof WpeWl.key_q / sizeof WpeWl.key_q[0]))

static void wl_key_push(int code)
{
 int next = (WpeWl.key_tail + 1) % WL_KEYQ_LEN;
 if (code == 0 || next == WpeWl.key_head)   /* drop unmapped keys / on overflow */
  return;
 WpeWl.key_q[WpeWl.key_tail] = code;
 WpeWl.key_tail = next;
}

static int wl_key_pop(void)
{
 int code;
 if (WpeWl.key_head == WpeWl.key_tail)
  return 0;
 code = WpeWl.key_q[WpeWl.key_head];
 WpeWl.key_head = (WpeWl.key_head + 1) % WL_KEYQ_LEN;
 return code;
}

/* keysym_to_xwpe - map an xkbcommon keysym (plus the UTF-8 text it produced and
   the active modifiers) to an internal xwpe key code.  This mirrors the X11
   translation in e_x_getch one-for-one: xkbcommon's XKB_KEY_* values are the
   same numbers as Xlib's XK_*, so the two backends agree key-for-key.  `u8len`
   plays the role of XLookupString's charcount: 1 means a character was
   produced; 0 or >1 means a special key or a multibyte codepoint.  Returns 0
   when nothing maps (the caller drops it). */
int keysym_to_xwpe(xkb_keysym_t sym, const char *utf8, int u8len,
                   int ctrl, int shift, int alt)
{
 int c = 0;

 if (sym == XKB_KEY_ISO_Left_Tab)
  return WPE_BTAB;

 if (u8len == 1)
 {
  unsigned char ch = (unsigned char)utf8[0];

  if (ch == 127)
  {
   if (ctrl)  return CENTF;
   if (shift) return ShiftDel;
   if (alt)   return AltDel;
   return ENTF;
  }
  if (shift && ch == '\t')
   return WPE_BTAB;
  if (alt)
   c = e_tast_sim(shift ? toupper(ch) : ch);   /* Alt+<letter> menu accelerator */
  else
   return ch;
 }
 else if (ctrl)
 {
  if      (sym == XKB_KEY_Left)   c = CCLE;
  else if (sym == XKB_KEY_Right)  c = CCRI;
  else if (sym == XKB_KEY_Home)   c = CPS1;
  else if (sym == XKB_KEY_End)    c = CEND;
  else if (sym == XKB_KEY_Insert) c = CEINFG;
  else if (sym == XKB_KEY_Delete) c = CENTF;
  else if (sym == XKB_KEY_Prior)  c = CBUP;
  else if (sym == XKB_KEY_Next)   c = CBDO;
  else if (sym == XKB_KEY_F1)  c = CF1;
  else if (sym == XKB_KEY_F2)  c = CF2;
  else if (sym == XKB_KEY_F3)  c = CF3;
  else if (sym == XKB_KEY_F4)  c = CF4;
  else if (sym == XKB_KEY_F5)  c = CF5;
  else if (sym == XKB_KEY_F6)  c = CF6;
  else if (sym == XKB_KEY_F7)  c = CF7;
  else if (sym == XKB_KEY_F8)  c = CF8;
  else if (sym == XKB_KEY_F9)  c = CF9;
  else if (sym == XKB_KEY_F10) c = CF10;
 }
 else if (alt)
 {
  if      (sym == XKB_KEY_F1)  c = AF1;
  else if (sym == XKB_KEY_F2)  c = AF2;
  else if (sym == XKB_KEY_F3)  c = AF3;
  else if (sym == XKB_KEY_F4)  c = AF4;
  else if (sym == XKB_KEY_F5)  c = AF5;
  else if (sym == XKB_KEY_F6)  c = AF6;
  else if (sym == XKB_KEY_F7)  c = AF7;
  else if (sym == XKB_KEY_F8)  c = AF8;
  else if (sym == XKB_KEY_F9)  c = AF9;
  else if (sym == XKB_KEY_F10) c = AF10;
  else if (sym == XKB_KEY_Insert) c = AltEin;
  else if (sym == XKB_KEY_Delete) c = AltDel;
 }
 else
 {
  if      (sym == XKB_KEY_Left)      c = CLE;
  else if (sym == XKB_KEY_Right)     c = CRI;
  else if (sym == XKB_KEY_Up)        c = CUP;
  else if (sym == XKB_KEY_Down)      c = CDO;
  else if (sym == XKB_KEY_Home)      c = POS1;
  else if (sym == XKB_KEY_End)       c = ENDE;
  else if (sym == XKB_KEY_Insert)    c = EINFG;
  else if (sym == XKB_KEY_Delete)    c = ENTF;
  else if (sym == XKB_KEY_BackSpace) c = CtrlH;
  else if (sym == XKB_KEY_Prior)     c = BUP;
  else if (sym == XKB_KEY_Next)      c = BDO;
  else if (sym == XKB_KEY_F1)  c = F1;
  else if (sym == XKB_KEY_F2)  c = F2;
  else if (sym == XKB_KEY_F3)  c = F3;
  else if (sym == XKB_KEY_F4)  c = F4;
  else if (sym == XKB_KEY_F5)  c = F5;
  else if (sym == XKB_KEY_F6)  c = F6;
  else if (sym == XKB_KEY_F7)  c = F7;
  else if (sym == XKB_KEY_F8)  c = F8;
  else if (sym == XKB_KEY_F9)  c = F9;
  else if (sym == XKB_KEY_F10) c = F10;
  else if (sym == XKB_KEY_Help) c = HELP;
 }

 if (c != 0)
 {
  if (shift)
   c += 512;
  return c;
 }
 if (u8len >= 2 && ((unsigned char)utf8[0] & 0xC0) == 0xC0)
 {
  int cp = e_utf8_to_codepoint((unsigned char *)utf8, u8len);
  if (cp > 0)
   return cp;
 }
 return 0;
}

static void wl_active_mods(int *ctrl, int *shift, int *alt)
{
 *ctrl  = WpeWl.xkb_state && xkb_state_mod_name_is_active(
            WpeWl.xkb_state, XKB_MOD_NAME_CTRL,  XKB_STATE_MODS_EFFECTIVE) > 0;
 *shift = WpeWl.xkb_state && xkb_state_mod_name_is_active(
            WpeWl.xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
 *alt   = WpeWl.xkb_state && xkb_state_mod_name_is_active(
            WpeWl.xkb_state, XKB_MOD_NAME_ALT,   XKB_STATE_MODS_EFFECTIVE) > 0;
}

/* Key auto-repeat.  Wayland sends ONE press and ONE release per key and leaves
   repeating a held key to the client, using the compositor's repeat_info
   (rate = keys/second, delay = ms before the first repeat).  We drive it with a
   timerfd in the shared fd-loop: on the press of a repeatable key we arm the
   timer (delay, then 1000/rate interval); each expiry re-injects the key; the
   release (or a different key, or losing focus) disarms it. */
static int g_repeat_fd    = -1;
static int g_repeat_code  = 0;      /* xwpe key code currently repeating         */
static int g_repeat_rate  = 25;     /* keys/second (0 = repeat disabled)         */
static int g_repeat_delay = 600;    /* ms before the first repeat                */

static void wl_repeat_disarm(void)
{
 struct itimerspec its;
 if (g_repeat_fd < 0)
  return;
 memset(&its, 0, sizeof its);       /* all-zero = disarm the timer */
 timerfd_settime(g_repeat_fd, 0, &its, NULL);
}

static void wl_repeat_arm(int xwc)
{
 struct itimerspec its;
 if (g_repeat_fd < 0 || g_repeat_rate <= 0)
  return;
 g_repeat_code = xwc;
 its.it_value.tv_sec     = g_repeat_delay / 1000;
 its.it_value.tv_nsec    = (long)(g_repeat_delay % 1000) * 1000000L;
 its.it_interval.tv_sec  = 0;
 its.it_interval.tv_nsec = 1000000000L / g_repeat_rate;
 timerfd_settime(g_repeat_fd, 0, &its, NULL);
}

/* fd-loop callback: the repeat timer fired -- re-inject the held key. */
static void wl_repeat_fire(int fd, void *data)
{
 uint64_t expirations;
 (void)data;
 if (read(fd, &expirations, sizeof expirations) != (ssize_t)sizeof expirations)
  return;
 wl_key_push(g_repeat_code);
}

static void kbd_keymap(void *data, struct wl_keyboard *kbd, uint32_t format,
                       int fd, uint32_t size)
{
 char *map;

 (void)data; (void)kbd;
 if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
 map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
 if (map == MAP_FAILED) { close(fd); return; }

 if (WpeWl.xkb_state)  { xkb_state_unref(WpeWl.xkb_state);   WpeWl.xkb_state = NULL; }
 if (WpeWl.xkb_keymap) { xkb_keymap_unref(WpeWl.xkb_keymap); WpeWl.xkb_keymap = NULL; }
 WpeWl.xkb_keymap = xkb_keymap_new_from_string(WpeWl.xkb_ctx, map,
   XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
 munmap(map, size);
 close(fd);
 if (WpeWl.xkb_keymap)
  WpeWl.xkb_state = xkb_state_new(WpeWl.xkb_keymap);
}

static void kbd_enter(void *data, struct wl_keyboard *kbd, uint32_t serial,
                      struct wl_surface *surface, struct wl_array *keys)
{ (void)data; (void)kbd; (void)serial; (void)surface; (void)keys; WpeWl.kbd_focus = 1; }

static void kbd_leave(void *data, struct wl_keyboard *kbd, uint32_t serial,
                      struct wl_surface *surface)
{ (void)data; (void)kbd; (void)serial; (void)surface; WpeWl.kbd_focus = 0;
  wl_repeat_disarm(); }   /* focus lost: no key is held here any more */

static void kbd_key(void *data, struct wl_keyboard *kbd, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state)
{
 xkb_keycode_t code = key + 8;   /* evdev -> xkb keycode offset */
 xkb_keysym_t sym;
 char u8[16];
 int u8len, ctrl, shift, alt;

 (void)data; (void)kbd; (void)time;
 WpeWl.last_serial = serial;     /* freshest serial for set_selection */
 if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
 {
  wl_repeat_disarm();            /* key released: stop auto-repeat */
  return;
 }
 if (!WpeWl.xkb_state)
  return;
 sym = xkb_state_key_get_one_sym(WpeWl.xkb_state, code);
 u8len = xkb_state_key_get_utf8(WpeWl.xkb_state, code, u8, sizeof u8);
 wl_active_mods(&ctrl, &shift, &alt);
 {
  int xwc = keysym_to_xwpe(sym, u8, u8len, ctrl, shift, alt);
  WPE_TRACE("wayland", "kbd_key sym=0x%x u8len=%d ctrl=%d shift=%d alt=%d -> %d\n",
            (unsigned)sym, u8len, ctrl, shift, alt, xwc);
  wl_key_push(xwc);
  /* Hold-to-repeat: arm for keys xkb marks repeatable (arrows, letters, ...),
     never for modifiers/locks.  A new repeatable key replaces the previous. */
  if (xwc && WpeWl.xkb_keymap && xkb_keymap_key_repeats(WpeWl.xkb_keymap, code))
   wl_repeat_arm(xwc);
  else
   wl_repeat_disarm();
 }
}

static void kbd_modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial,
                          uint32_t dep, uint32_t lat, uint32_t lck, uint32_t group)
{
 (void)data; (void)kbd; (void)serial;
 if (WpeWl.xkb_state)
  xkb_state_update_mask(WpeWl.xkb_state, dep, lat, lck, 0, 0, group);
}

static void kbd_repeat_info(void *data, struct wl_keyboard *kbd, int32_t rate, int32_t delay)
{
 (void)data; (void)kbd;
 g_repeat_rate  = rate;    /* keys/second; 0 = the compositor asks for no repeat */
 g_repeat_delay = delay;   /* ms before the first repeat */
}

static const struct wl_keyboard_listener kbd_listener = {
 kbd_keymap, kbd_enter, kbd_leave, kbd_key, kbd_modifiers, kbd_repeat_info
};

/* Translate a Wayland button code (BTN_*) to xwpe's button bit. */
static int wl_btn_bit(uint32_t button)
{
 if (button == BTN_LEFT)   return 1;
 if (button == BTN_MIDDLE) return 2;
 if (button == BTN_RIGHT)  return 4;
 return 0;
}

/* Pointer cursor.  A client must set its own cursor whenever the pointer enters
   its surface; if it does not, the compositor keeps whatever it last showed --
   so the resize cursor from the decoration border leaks into the edit area.
   xwpe uses a plain arrow over the whole window, matching the X11 backend
   (WpeEditingShape = XC_top_left_arrow). */
static struct wl_cursor_theme *g_cursor_theme;
static struct wl_cursor       *g_cursor;
static struct wl_surface      *g_cursor_surface;

static void wl_cursor_init(void)
{
 const char *env;
 int size = 24;

 if (g_cursor_theme || !WpeWl.shm)
  return;
 env = getenv("XCURSOR_SIZE");
 if (env && atoi(env) > 0)
  size = atoi(env);
 g_cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), size, WpeWl.shm);
 if (!g_cursor_theme)
  return;
 g_cursor = wl_cursor_theme_get_cursor(g_cursor_theme, "left_ptr");
 if (!g_cursor)
  g_cursor = wl_cursor_theme_get_cursor(g_cursor_theme, "default");
 if (WpeWl.compositor)
  g_cursor_surface = wl_compositor_create_surface(WpeWl.compositor);
}

/* Point the seat at our arrow cursor for this enter serial. */
static void wl_cursor_apply(uint32_t serial)
{
 struct wl_cursor_image *img;
 struct wl_buffer *buf;

 if (!WpeWl.pointer || !g_cursor || !g_cursor_surface)
  return;
 img = g_cursor->images[0];
 buf = wl_cursor_image_get_buffer(img);
 if (!buf)
  return;
 wl_pointer_set_cursor(WpeWl.pointer, serial, g_cursor_surface,
                       img->hotspot_x, img->hotspot_y);
 wl_surface_attach(g_cursor_surface, buf, 0, 0);
 wl_surface_damage(g_cursor_surface, 0, 0, img->width, img->height);
 wl_surface_commit(g_cursor_surface);
}

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
 (void)data; (void)p; (void)surface;
 g_ptr_px = wl_fixed_to_int(sx);
 g_ptr_py = wl_fixed_to_int(sy);
 wl_cursor_apply(serial);
}
static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *surface)
{ (void)data; (void)p; (void)serial; (void)surface; }

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
                       wl_fixed_t sx, wl_fixed_t sy)
{
 (void)data; (void)p; (void)time;
 g_ptr_px = wl_fixed_to_int(sx);
 g_ptr_py = wl_fixed_to_int(sy);
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state)
{
 int bit = wl_btn_bit(button);

 (void)data; (void)p; (void)time;
 WpeWl.last_serial = serial;     /* freshest serial for set_selection */
 WPE_TRACE("wayland", "ptr_button button=%u state=%u bit=%d px=%d py=%d\n",
           button, state, bit, g_ptr_px, g_ptr_py);
 if (!bit)
  return;
 if (state == WL_POINTER_BUTTON_STATE_PRESSED)
 {
  int ctrl, shift, alt;
  g_ptr_btn |= bit;
  /* A press the editor can act on in e_w_getch (the X11 ButtonPress path). */
  if (WpeRender.font_width > 0)
  {
   e_mouse.x = g_ptr_px / WpeRender.font_width;
   e_mouse.y = g_ptr_py / WpeRender.font_height;
  }
  wl_active_mods(&ctrl, &shift, &alt);
  e_mouse.k = (shift ? 3 : 0) | (ctrl ? 4 : 0) | (alt ? 8 : 0);
  g_mouse_pending = -bit;
 }
 else
 {
  g_ptr_btn &= ~bit;
 }
}

/* Vertical wheel -> the editor's scroll keys (one step per axis event). */
static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
                     uint32_t axis, wl_fixed_t value)
{
 (void)data; (void)p; (void)time;
 if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
  return;
 if (value > 0)
  g_mouse_pending = WPE_SCROLL_DOWN;
 else if (value < 0)
  g_mouse_pending = WPE_SCROLL_UP;
}

static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s)
{ (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{ (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }

static const struct wl_pointer_listener ptr_listener = {
 ptr_enter, ptr_leave, ptr_motion, ptr_button, ptr_axis,
 ptr_frame, ptr_axis_source, ptr_axis_stop, ptr_axis_discrete
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
 (void)data;
 if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !WpeWl.keyboard)
 {
  WpeWl.keyboard = wl_seat_get_keyboard(seat);
  wl_keyboard_add_listener(WpeWl.keyboard, &kbd_listener, NULL);
  if (g_repeat_fd < 0)   /* one key-repeat timer, driven by the shared fd-loop */
  {
   g_repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
   if (g_repeat_fd >= 0)
    wpe_fd_add(g_repeat_fd, POLLIN, wl_repeat_fire, NULL);
  }
 }
 else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && WpeWl.keyboard)
 {
  wl_keyboard_release(WpeWl.keyboard);
  WpeWl.keyboard = NULL;
 }
 if ((caps & WL_SEAT_CAPABILITY_POINTER) && !WpeWl.pointer)
 {
  WpeWl.pointer = wl_seat_get_pointer(seat);
  wl_pointer_add_listener(WpeWl.pointer, &ptr_listener, NULL);
  wl_cursor_init();               /* so ptr_enter can set our own arrow cursor */
 }
 else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && WpeWl.pointer)
 {
  wl_pointer_release(WpeWl.pointer);
  WpeWl.pointer = NULL;
 }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name)
{ (void)data; (void)seat; (void)name; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Clipboard (wl_data_device: the system selection)

  Mirrors the X11 e_clip_x_set/get contract: a Copy makes xwpe own the selection
  and caches the text; e_clip_os_get returns NULL while WE own it (the editor's
  internal clipboard is then authoritative) and the external text only when
  another client owns the selection.
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static const char *const WL_CLIP_MIME = "text/plain;charset=utf-8";

static char *g_clip_text;            /* our copied text (we own the selection) */
static size_t g_clip_len;
static int    g_clip_we_own;         /* xwpe currently owns the OS selection    */
static struct wl_data_source *g_clip_source;
static struct wl_data_offer  *g_clip_offer;   /* current selection offer (any owner) */

/* Write our cached copied text to a pasting client's fd and close it.  Shared by
   the CLIPBOARD (wl_data_source) and PRIMARY (zwp_primary_selection_source)
   send handlers -- both serve the same bytes.  Async-signal-unsafe work is fine
   here: this runs from the normal event dispatch, not a signal handler. */
static void clip_send_bytes(int fd)
{
 if (g_clip_text)
 {
  size_t off = 0;
  while (off < g_clip_len)
  {
   ssize_t n = write(fd, g_clip_text + off, g_clip_len - off);
   if (n <= 0)
    break;
   off += (size_t)n;
  }
 }
 close(fd);
}
/* The compositor asks us to hand our copied text to a pasting client. */
static void clip_source_send(void *data, struct wl_data_source *src,
                             const char *mime, int fd)
{
 (void)data; (void)src; (void)mime;
 clip_send_bytes(fd);
}
/* A source was cancelled (another client took the selection, or we replaced it
   on the next Copy).  Destroy whatever source was cancelled so it never leaks;
   only drop ownership when the CURRENT source is the one being cancelled, so a
   stale source's late cancellation cannot clear the ownership we just took. */
static void clip_source_cancelled(void *data, struct wl_data_source *src)
{
 (void)data;
 if (src == g_clip_source)
 {
  g_clip_we_own = 0;
  g_clip_source = NULL;
 }
 wl_data_source_destroy(src);
}
static void clip_source_target(void *d, struct wl_data_source *s, const char *m)
{ (void)d; (void)s; (void)m; }
static void clip_source_dnd_drop(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void clip_source_dnd_finished(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void clip_source_action(void *d, struct wl_data_source *s, uint32_t a)
{ (void)d; (void)s; (void)a; }
static const struct wl_data_source_listener clip_source_listener = {
 clip_source_target, clip_source_send, clip_source_cancelled,
 clip_source_dnd_drop, clip_source_dnd_finished, clip_source_action
};

static void clip_offer_offer(void *d, struct wl_data_offer *o, const char *mime)
{ (void)d; (void)o; (void)mime; }
static void clip_offer_source_actions(void *d, struct wl_data_offer *o, uint32_t a)
{ (void)d; (void)o; (void)a; }
static void clip_offer_action(void *d, struct wl_data_offer *o, uint32_t a)
{ (void)d; (void)o; (void)a; }
static const struct wl_data_offer_listener clip_offer_listener = {
 clip_offer_offer, clip_offer_source_actions, clip_offer_action
};

/* A new offer is being introduced; attach our listener before its mime events. */
static void ddev_data_offer(void *data, struct wl_data_device *dd,
                            struct wl_data_offer *offer)
{
 (void)data; (void)dd;
 wl_data_offer_add_listener(offer, &clip_offer_listener, NULL);
}
/* The selection changed: remember the current offer (ours or external). */
static void ddev_selection(void *data, struct wl_data_device *dd,
                           struct wl_data_offer *offer)
{
 (void)data; (void)dd;
 if (g_clip_offer && g_clip_offer != offer)
  wl_data_offer_destroy(g_clip_offer);
 g_clip_offer = offer;
}
static void ddev_enter(void *d, struct wl_data_device *dd, uint32_t s,
                       struct wl_surface *su, wl_fixed_t x, wl_fixed_t y,
                       struct wl_data_offer *o)
{ (void)d; (void)dd; (void)s; (void)su; (void)x; (void)y; (void)o; }
static void ddev_leave(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void ddev_motion(void *d, struct wl_data_device *dd, uint32_t t,
                        wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)dd; (void)t; (void)x; (void)y; }
static void ddev_drop(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static const struct wl_data_device_listener ddev_listener = {
 ddev_data_offer, ddev_enter, ddev_leave, ddev_motion, ddev_drop, ddev_selection
};

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Primary selection (zwp_primary_selection_v1: the middle-click selection)

  The X11 backend owns BOTH the PRIMARY (middle-click paste) and CLIPBOARD
  selections on a Copy (we_xterm.c).  We match that here: alongside the
  wl_data_device CLIPBOARD source, a Copy also publishes a primary-selection
  source serving the same bytes, so a middle-click in a Wayland-native terminal
  or editor pastes the last Copy.  The text is shared (g_clip_text); only the
  protocol objects differ.
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static struct zwp_primary_selection_source_v1 *g_pclip_source;
static struct zwp_primary_selection_offer_v1  *g_pclip_offer;
static int    g_pclip_we_own;        /* xwpe currently owns the PRIMARY selection */

static void pclip_source_send(void *data,
        struct zwp_primary_selection_source_v1 *src, const char *mime, int fd)
{ (void)data; (void)src; (void)mime; clip_send_bytes(fd); }

static void pclip_source_cancelled(void *data,
        struct zwp_primary_selection_source_v1 *src)
{
 (void)data;
 if (src == g_pclip_source)
 {
  g_pclip_we_own = 0;
  g_pclip_source = NULL;
 }
 zwp_primary_selection_source_v1_destroy(src);
}
static const struct zwp_primary_selection_source_v1_listener pclip_source_listener = {
 pclip_source_send, pclip_source_cancelled
};

static void pclip_offer_offer(void *d,
        struct zwp_primary_selection_offer_v1 *o, const char *mime)
{ (void)d; (void)o; (void)mime; }
static const struct zwp_primary_selection_offer_v1_listener pclip_offer_listener = {
 pclip_offer_offer
};

/* A new primary offer is being introduced; attach our listener before its mime
   events, exactly as ddev_data_offer does for the clipboard. */
static void pdev_data_offer(void *data,
        struct zwp_primary_selection_device_v1 *dev,
        struct zwp_primary_selection_offer_v1 *offer)
{
 (void)data; (void)dev;
 zwp_primary_selection_offer_v1_add_listener(offer, &pclip_offer_listener, NULL);
}
/* The primary selection changed: remember the current offer (ours or external). */
static void pdev_selection(void *data,
        struct zwp_primary_selection_device_v1 *dev,
        struct zwp_primary_selection_offer_v1 *offer)
{
 (void)data; (void)dev;
 if (g_pclip_offer && g_pclip_offer != offer)
  zwp_primary_selection_offer_v1_destroy(g_pclip_offer);
 g_pclip_offer = offer;
}
static const struct zwp_primary_selection_device_v1_listener pdev_listener = {
 pdev_data_offer, pdev_selection
};

/* e_clip_w_set - Copy: own the OS selection and cache the text (e_clip_os_set). */
static void e_clip_w_set(const char *utf8, int len)
{
 if (!WpeWl.ddm || !WpeWl.ddev)
  return;
 free(g_clip_text);
 g_clip_text = malloc((size_t)len + 1);
 if (!g_clip_text)
 {
  g_clip_len = 0;
  return;
 }
 memcpy(g_clip_text, utf8, len);
 g_clip_text[len] = 0;
 g_clip_len = (size_t)len;

 g_clip_source = wl_data_device_manager_create_data_source(WpeWl.ddm);
 wl_data_source_add_listener(g_clip_source, &clip_source_listener, NULL);
 wl_data_source_offer(g_clip_source, WL_CLIP_MIME);
 wl_data_source_offer(g_clip_source, "text/plain");
 wl_data_source_offer(g_clip_source, "UTF8_STRING");
 wl_data_source_offer(g_clip_source, "TEXT");
 wl_data_source_offer(g_clip_source, "STRING");
 wl_data_device_set_selection(WpeWl.ddev, g_clip_source, WpeWl.last_serial);
 g_clip_we_own = 1;

 /* Own PRIMARY too, so a middle-click paste in a Wayland app gets the same
    text (the X11 backend owns both PRIMARY and CLIPBOARD on a Copy). */
 if (WpeWl.psm && WpeWl.pdev)
 {
  g_pclip_source =
    zwp_primary_selection_device_manager_v1_create_source(WpeWl.psm);
  zwp_primary_selection_source_v1_add_listener(g_pclip_source,
    &pclip_source_listener, NULL);
  zwp_primary_selection_source_v1_offer(g_pclip_source, WL_CLIP_MIME);
  zwp_primary_selection_source_v1_offer(g_pclip_source, "text/plain");
  zwp_primary_selection_source_v1_offer(g_pclip_source, "UTF8_STRING");
  zwp_primary_selection_source_v1_offer(g_pclip_source, "TEXT");
  zwp_primary_selection_source_v1_offer(g_pclip_source, "STRING");
  zwp_primary_selection_device_v1_set_selection(WpeWl.pdev, g_pclip_source,
    WpeWl.last_serial);
  g_pclip_we_own = 1;
 }
 wl_display_flush(WpeWl.display);
}

/* clip_pipe_drain - read a selection's bytes from read_fd until EOF/timeout.
   The owning client writes asynchronously, so we keep the wl event loop turning
   while draining -- no busy-wait, no fixed sleep.  The caller has already issued
   the matching *_offer_receive() and closed the write end; the caller also
   closes read_fd.  Returns malloc'd, NUL-terminated bytes (caller frees), or
   NULL if nothing was read.  Shared by the CLIPBOARD and PRIMARY readers. */
static char *clip_pipe_drain(int read_fd, int *len)
{
 char  *buf = NULL;
 size_t cap = 0, used = 0;
 int    wl_fd = wl_display_get_fd(WpeWl.display);

 for (;;)
 {
  struct pollfd pf[2];
  char chunk[4096];
  ssize_t n;

  wl_display_flush(WpeWl.display);
  pf[0].fd = read_fd; pf[0].events = POLLIN; pf[0].revents = 0;
  pf[1].fd = wl_fd;   pf[1].events = POLLIN; pf[1].revents = 0;
  if (poll(pf, 2, 2000) <= 0)
   break;                        /* timeout / error: stop with what we have */
  if (pf[1].revents & POLLIN)
   wl_display_dispatch(WpeWl.display);
  if (pf[0].revents & (POLLIN | POLLHUP))
  {
   n = read(read_fd, chunk, sizeof chunk);
   if (n < 0)
    break;
   if (n == 0)
    break;                       /* EOF: owner finished writing */
   if (used + (size_t)n + 1 > cap)
   {
    size_t ncap = (cap ? cap * 2 : 8192);
    char *nb;
    while (ncap < used + (size_t)n + 1)
     ncap *= 2;
    nb = realloc(buf, ncap);
    if (!nb)
     break;
    buf = nb; cap = ncap;
   }
   memcpy(buf + used, chunk, (size_t)n);
   used += (size_t)n;
  }
 }
 if (!buf)
  return NULL;
 buf[used] = 0;
 if (len)
  *len = (int)used;
 return buf;
}

/* e_clip_w_get - Paste (e_clip_os_get): read the external owner's text.  Tries
   the CLIPBOARD selection first (the ^C/^V selection of GUI apps), then the
   PRIMARY selection (middle-click / select-to-copy), matching the X11 reader.
   Returns NULL while WE own a selection (the editor's internal clipboard is
   then authoritative) or when nothing is offered.  Caller frees the result. */
static char *e_clip_w_get(int *len)
{
 int   fds[2];
 char *res;

 if (len)
  *len = 0;
 if (!WpeWl.display)
  return NULL;

 if (!g_clip_we_own && g_clip_offer)
 {
  if (pipe(fds) != 0)
   return NULL;
  wl_data_offer_receive(g_clip_offer, WL_CLIP_MIME, fds[1]);
  close(fds[1]);
  wl_display_flush(WpeWl.display);   /* let the owner start writing */
  res = clip_pipe_drain(fds[0], len);
  close(fds[0]);
  if (res)
   return res;
 }
 if (!g_pclip_we_own && g_pclip_offer)
 {
  if (pipe(fds) != 0)
   return NULL;
  zwp_primary_selection_offer_v1_receive(g_pclip_offer, WL_CLIP_MIME, fds[1]);
  close(fds[1]);
  wl_display_flush(WpeWl.display);
  res = clip_pipe_drain(fds[0], len);
  close(fds[0]);
  if (res)
   return res;
 }
 return NULL;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Bring-up and teardown
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* wl_connect_and_bind - open the display, gather globals, fail if the
   compositor lacks anything mandatory.  Returns 0 on success. */
static int wl_connect_and_bind(void)
{
 memset(&WpeWl, 0, sizeof WpeWl);
 WpeWl.shm_fd = -1;
 WpeWl.width = WPE_WL_DEF_COLS * WPE_WL_DEF_CW;
 WpeWl.height = WPE_WL_DEF_ROWS * WPE_WL_DEF_CH;
 WpeWl.running = 1;

 WpeWl.display = wl_display_connect(NULL);
 if (!WpeWl.display)
  return -1;

 WpeWl.registry = wl_display_get_registry(WpeWl.display);
 wl_registry_add_listener(WpeWl.registry, &registry_listener, NULL);
 wl_display_roundtrip(WpeWl.display);   /* run registry_global for each global */

 if (!WpeWl.compositor || !WpeWl.shm || !WpeWl.wm_base)
 {
  WPE_TRACE("wayland", "compositor missing required globals "
            "(compositor=%p shm=%p xdg_wm_base=%p)\n",
            (void *)WpeWl.compositor, (void *)WpeWl.shm, (void *)WpeWl.wm_base);
  return -1;
 }

 /* Keyboard: build an xkb context, then listen to the seat.  Two extra
    roundtrips let the seat report its capabilities (-> we grab the keyboard)
    and then deliver the keymap, so WpeWl.xkb_state is ready before first use. */
 WpeWl.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
 if (WpeWl.seat)
 {
  wl_seat_add_listener(WpeWl.seat, &seat_listener, NULL);
  /* OS clipboard: a data device on this seat (the wl_data_device_manager was
     bound in the registry pass).  Selection events flow through ddev_listener. */
  if (WpeWl.ddm)
  {
   WpeWl.ddev = wl_data_device_manager_get_data_device(WpeWl.ddm, WpeWl.seat);
   wl_data_device_add_listener(WpeWl.ddev, &ddev_listener, NULL);
  }
  /* Primary selection device on the same seat (manager bound in the registry
     pass), so middle-click paste reaches the editor's copied text. */
  if (WpeWl.psm)
  {
   WpeWl.pdev = zwp_primary_selection_device_manager_v1_get_device(WpeWl.psm,
                                                                   WpeWl.seat);
   zwp_primary_selection_device_v1_add_listener(WpeWl.pdev, &pdev_listener,
                                                NULL);
  }
  wl_display_roundtrip(WpeWl.display);   /* seat capabilities -> get_keyboard */
  wl_display_roundtrip(WpeWl.display);   /* wl_keyboard.keymap -> xkb_state    */
 }
 return 0;
}

/* wl_create_window - create the surface and its xdg-shell toplevel, then commit
   so the compositor sends the first configure. */
static void wl_create_window(void)
{
 WpeWl.surface = wl_compositor_create_surface(WpeWl.compositor);
 WpeWl.xdg_surface = xdg_wm_base_get_xdg_surface(WpeWl.wm_base, WpeWl.surface);
 xdg_surface_add_listener(WpeWl.xdg_surface, &xdg_surface_listener, NULL);
 WpeWl.xdg_toplevel = xdg_surface_get_toplevel(WpeWl.xdg_surface);
 xdg_toplevel_add_listener(WpeWl.xdg_toplevel, &xdg_toplevel_listener, NULL);
 xdg_toplevel_set_title(WpeWl.xdg_toplevel, "xwpe");
 xdg_toplevel_set_app_id(WpeWl.xdg_toplevel, "io.codeberg.mendezr.xwpe");

 /* Ask the compositor to draw the window frame (title bar + resize borders).
    xwpe paints no client-side decoration, so without this the toplevel is
    borderless and there is nothing for the user to grab to move or resize it. */
 if (WpeWl.deco_manager)
 {
  WpeWl.toplevel_deco = zxdg_decoration_manager_v1_get_toplevel_decoration(
    WpeWl.deco_manager, WpeWl.xdg_toplevel);
  zxdg_toplevel_decoration_v1_set_mode(WpeWl.toplevel_deco,
    ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
 }

 wl_surface_commit(WpeWl.surface);
}

static void wl_teardown(void)
{
 if (WpeWl.xkb_state)  xkb_state_unref(WpeWl.xkb_state);
 if (WpeWl.xkb_keymap) xkb_keymap_unref(WpeWl.xkb_keymap);
 if (WpeWl.xkb_ctx)    xkb_context_unref(WpeWl.xkb_ctx);
 if (g_clip_offer)     { wl_data_offer_destroy(g_clip_offer); g_clip_offer = NULL; }
 if (g_clip_source)    { wl_data_source_destroy(g_clip_source); g_clip_source = NULL; }
 if (g_pclip_offer)    { zwp_primary_selection_offer_v1_destroy(g_pclip_offer); g_pclip_offer = NULL; }
 if (g_pclip_source)   { zwp_primary_selection_source_v1_destroy(g_pclip_source); g_pclip_source = NULL; }
 if (WpeWl.pdev)       zwp_primary_selection_device_v1_destroy(WpeWl.pdev);
 if (WpeWl.psm)        zwp_primary_selection_device_manager_v1_destroy(WpeWl.psm);
 if (WpeWl.ddev)       wl_data_device_destroy(WpeWl.ddev);
 if (WpeWl.ddm)        wl_data_device_manager_destroy(WpeWl.ddm);
 free(g_clip_text); g_clip_text = NULL; g_clip_len = 0; g_clip_we_own = 0;
 g_pclip_we_own = 0;
 if (g_cursor_surface) { wl_surface_destroy(g_cursor_surface); g_cursor_surface = NULL; }
 if (g_cursor_theme)   { wl_cursor_theme_destroy(g_cursor_theme); g_cursor_theme = NULL; g_cursor = NULL; }
 if (WpeWl.keyboard)   wl_keyboard_release(WpeWl.keyboard);
 if (WpeWl.pointer)    wl_pointer_release(WpeWl.pointer);
 if (WpeWl.buffer)        wl_buffer_destroy(WpeWl.buffer);
 if (WpeWl.pixels && WpeWl.pixels != MAP_FAILED) munmap(WpeWl.pixels, WpeWl.shm_size);
 if (WpeWl.shm_fd >= 0)   close(WpeWl.shm_fd);
 if (WpeWl.toplevel_deco) zxdg_toplevel_decoration_v1_destroy(WpeWl.toplevel_deco);
 if (WpeWl.deco_manager)  zxdg_decoration_manager_v1_destroy(WpeWl.deco_manager);
 if (WpeWl.xdg_toplevel)  xdg_toplevel_destroy(WpeWl.xdg_toplevel);
 if (WpeWl.xdg_surface)   xdg_surface_destroy(WpeWl.xdg_surface);
 if (WpeWl.surface)       wl_surface_destroy(WpeWl.surface);
 if (WpeWl.wm_base)       xdg_wm_base_destroy(WpeWl.wm_base);
 if (WpeWl.display)       wl_display_disconnect(WpeWl.display);
 memset(&WpeWl, 0, sizeof WpeWl);
 WpeWl.shm_fd = -1;
}

/* wl_selftest - connect, map a window, paint, dump the buffer, tear down and
   exit.  Headless bring-up verification only (XWPE_WL_SELFTEST=path); it never
   hands control to the editor, so it is independent of the not-yet-wired
   input/render path. */
static void wl_selftest(const char *dump_path)
{
 int rc = 1;

 if (wl_connect_and_bind() != 0)
 {
  WPE_TRACE("wayland", "selftest: no usable compositor\n");
  fprintf(stderr, "xwpe wayland selftest: no usable compositor\n");
  _exit(2);
 }
 wl_create_window();

 /* Keyboard came up during connect/bind (seat + keymap roundtrips). */
 fprintf(stderr, "xwpe wayland selftest: keyboard=%s xkb_keymap=%s pointer=%s\n",
         WpeWl.keyboard ? "yes" : "no",
         WpeWl.xkb_state ? "received" : "none",
         WpeWl.pointer ? "yes" : "no");

 /* Drive the protocol until the first frame is painted. */
 while (WpeWl.running && !WpeWl.painted && wl_display_dispatch(WpeWl.display) != -1)
  ;

 if (WpeWl.painted)
 {
  /* Bring up the Cairo/wl_shm renderer and paint a real cell grid, so the dump
     verifies glyph rendering (W2), not just the flat fill (W1). */
  if (wpe_render_wayland_init() == 0)
  {
   wl_selftest_fill_grid();
   e_w_render_dirty_cells(1);
   WpeRender.flush_all();
  }
  wl_display_roundtrip(WpeWl.display);   /* let the commit reach the server */
  rc = wpe_wl_dump_ppm(dump_path);
  WPE_TRACE("wayland", "selftest: %dx%d painted, dumped '%s' rc=%d\n",
            WpeWl.width, WpeWl.height, dump_path, rc);
  fprintf(stderr, "xwpe wayland selftest: %dx%d painted, cell %dx%d, dump rc=%d -> %s\n",
          WpeWl.width, WpeWl.height, WpeRender.font_width, WpeRender.font_height,
          rc, dump_path);
 }
 wl_teardown();
 _exit(rc == 0 ? 0 : 3);
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  SCREENCELL -> pixels (the same decode the X11 backend does in
  e_x_render_cell / e_x_render_dirty_cells, routed through WpeRender)
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* e_w_render_cell - paint one cell.  sc 1..12 are box-drawing/scrollbar glyphs;
   sc>12 (and not a plain space) is text encoded to UTF-8; everything else is a
   blank filled with the background colour. */
static void e_w_render_cell(int sc, int px, int py, int cw, int fg_idx, int bg_idx)
{
 if (sc >= 1 && sc <= 12)
 {
  WpeRender.draw_acs(sc, px, py, fg_idx, bg_idx);
 }
 else if (sc == WR_GLYPH_LOCK && WpeRender.draw_lock)
 {
  WpeRender.draw_lock(px, py, cw, fg_idx, bg_idx);
 }
 else if (sc > 12 && sc != ' ')
 {
  unsigned char u8[8];
  int u8len;

  if (sc < 32)        { u8[0] = ' '; u8len = 1; }
  else if (sc < 128)  { u8[0] = (unsigned char)sc; u8len = 1; }
  else                  u8len = e_codepoint_to_utf8(sc, u8);

  WpeRender.draw_text(px, py, (const char *)u8, u8len, cw, fg_idx, bg_idx);
 }
 else
 {
  WpeRender.draw_rect(px, py, WpeRender.font_width * cw, WpeRender.font_height, bg_idx);
 }
}

/* e_w_render_dirty_cells - repaint cells that changed since the last frame
   (force=1 repaints all).  Byte-for-byte the X11 walk: skip wide-char spacers,
   decode the attribute to fg/bg (honouring the semantic-token truecolor flag),
   and shadow into altschirm. */
static void e_w_render_dirty_cells(int force)
{
 int i, j;

 for (i = 0; i < MAXSLNS; i++)
  for (j = 0; j < MAXSCOL; j++)
  {
   int _n = i * MAXSCOL + j;
   int sc, sa;

   if (schirm[_n].flags & CELL_WIDE_SPACER)
   {
    if (!force)
     altschirm[_n] = schirm[_n];
    continue;
   }
   sc = e_gt_char(j, i);
   sa = e_gt_col(j, i);
   if (force || sc != altschirm[_n].ch || sa != altschirm[_n].attr
       || schirm[_n].flags != altschirm[_n].flags)
   {
    e_w_render_cell(sc, WpeRender.font_width * j, WpeRender.font_height * i,
                    (schirm[_n].flags & CELL_WIDE) ? 2 : 1,
                    ATTR_IS_TC(sa) ? 16 + ATTR_TC_SLOT(sa) : ATTR_BASE(sa) % 16,
                    ATTR_BASE(sa) / 16);
    altschirm[_n] = schirm[_n];
   }
  }
}

/* wl_selftest_fill_grid - populate the screen-cell grid for the headless render
   test: a defined black background plus a white-on-blue banner, so the dumped
   buffer carries both a known background colour and real rendered glyphs. */
static void wl_selftest_fill_grid(void)
{
 const char *msg = "XWPE WAYLAND OK";
 int cols = WpeWl.width / WpeRender.font_width;
 int rows = WpeWl.height / WpeRender.font_height;
 int total, n, i;

 if (cols < 1) cols = 1;
 if (rows < 1) rows = 1;
 MAXSCOL = cols;
 MAXSLNS = rows;
 total = cols * rows;

 schirm = calloc((size_t)total, sizeof(SCREENCELL));
 altschirm = calloc((size_t)total, sizeof(SCREENCELL));
 for (n = 0; n < total; n++)
 {
  schirm[n].ch = ' ';
  schirm[n].attr = 0 * 16 + 7;     /* fg light-gray on bg black */
  schirm[n].flags = 0;
 }
 for (i = 0; msg[i] && (2 + i) < cols; i++)
 {
  int idx = 1 * cols + (2 + i);    /* banner on row 1, from column 2 */
  schirm[idx].ch = (unsigned char)msg[i];
  schirm[idx].attr = 4 * 16 + 15;  /* fg white (15) on bg dark-slate-blue (4) */
 }
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Interactive backend (the e_w_* function-pointer targets)
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* Paint one cell, optionally inverting fg/bg for the text cursor (mirrors the
   X11 e_xft_paint_cursor_cell decode). */
static void e_w_render_one(int x, int y, int invert)
{
 int n, sc, sa, base, fg, bg, cw;

 /* The text cursor can transiently sit off the visible grid -- e.g. while a
    buffer edit such as Undo rescrolls the viewport, cur_x/cur_y (or the saved
    old_cursor_x/y being restored) can be negative or past the last row/column.
    The X11 backend paints through an X Drawable, which clips such coordinates;
    this backend writes straight into the wl_shm buffer, so an off-grid cell
    would index schirm[] and fill pixels before the buffer start and crash.
    Skip it -- an off-screen cursor has nothing to paint. */
 if (x < 0 || x >= MAXSCOL || y < 0 || y >= MAXSLNS)
  return;

 n  = y * MAXSCOL + x;
 sc = e_gt_char(x, y);
 sa = e_gt_col(x, y);
 base = ATTR_BASE(sa);
 fg = invert ? base / 16
             : (ATTR_IS_TC(sa) ? 16 + ATTR_TC_SLOT(sa) : base % 16);
 bg = invert ? base % 16 : base / 16;
 cw = (schirm[n].flags & CELL_WIDE) ? 2 : 1;

 e_w_render_cell(sc, WpeRender.font_width * x, WpeRender.font_height * y, cw, fg, bg);
}

/* Draw the block cursor as a reverse-video cell, restoring the one it left. */
static void e_w_show_cursor(void)
{
 if (!cur_on)
  return;
 if (old_cursor_x != cur_x || old_cursor_y != cur_y)
  e_w_render_one(old_cursor_x, old_cursor_y, 0);
 e_w_render_one(cur_x, cur_y, 1);
 old_cursor_x = cur_x;
 old_cursor_y = cur_y;
}

static int e_w_refresh(void)
{
 if (!schirm || !WpeWl.pixels)
  return 0;
 e_w_render_dirty_cells(0);
 e_w_show_cursor();
 if (WpeRender.flush_all)
  WpeRender.flush_all();
 /* Headless screenshot: when XWPE_WL_DUMP is set, mirror every painted frame to
    that PPM (atomically) so a test can read the current screen at any time --
    no signal/thread coordination needed. */
 if (g_wl_dump_path && *g_wl_dump_path)
 {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof tmp, "%s.tmp", g_wl_dump_path);
  if (wpe_wl_dump_ppm(tmp) == 0)
   rename(tmp, g_wl_dump_path);
 }
 return 0;
}

/* Non-blocking peek: drain pending Wayland events, report a queued key or a
   pending mouse click/scroll. */
static int e_w_kbhit(void)
{
 if (!WpeWl.display)
  return 0;
 wl_pump_once(0);
 return (WpeWl.key_head != WpeWl.key_tail) || g_mouse_pending != 0;
}

/* Blocking key read: pump the Wayland fd through xwpe's shared poll loop (so
   LSP/DAP fds keep being serviced) until a translated key is queued.  A 0-ms
   poll on just the wl fd after wpe_fd_poll() tells us whether to read+dispatch
   or merely dispatch what is already buffered -- never blocking on the wl fd
   when the poll actually woke for some other descriptor. */
static int e_w_getch(void)
{
 int code;

 e_refresh();

 /* Headless UI verification: once the editor has consumed the requested number
    of keys (0 = the initial frame), dump the rendered buffer and exit.  Proves
    the whole backend -- and, with XWPE_WL_UIDUMP_AFTER>0 + injected keystrokes,
    the live input path -- without needing an interactive terminal. */
 if (g_wl_uidump && g_wl_keys_seen >= g_wl_dump_after)
 {
  const char *path = g_wl_uidump;
  g_wl_uidump = NULL;
  wl_display_roundtrip(WpeWl.display);
  wpe_wl_dump_ppm(path);
  fprintf(stderr, "xwpe wayland UI dump -> %s (%dx%d, cell %dx%d, after %d keys)\n",
          path, WpeWl.width, WpeWl.height,
          WpeRender.font_width, WpeRender.font_height, g_wl_keys_seen);
  _exit(0);
 }

 wpe_fd_add(wl_display_get_fd(WpeWl.display), POLLIN, NULL, NULL);

 for (;;)
 {
  wl_display_dispatch_pending(WpeWl.display);
  wl_apply_pending_resize();     /* coalesced re-fit for any configure just seen */

  code = wl_key_pop();
  if (code != 0)
  {
   g_wl_keys_seen++;
   return code;
  }
  if (g_mouse_pending != 0)        /* a button press (-bit) or wheel step */
  {
   code = g_mouse_pending;
   g_mouse_pending = 0;
   if (g_wl_mousetest)             /* headless mouse-path verification */
   {
    fprintf(stderr, "WL_MOUSE code=%d cell=%d,%d mods=%d\n",
            code, e_mouse.x, e_mouse.y, e_mouse.k);
    _exit(0);
   }
   return code;
  }
  if (!WpeWl.running)              /* xdg_toplevel.close */
  {
   extern int e_quit(FENSTER *);
   WpeWl.running = 1;
   e_quit(WpeEditor->f[WpeEditor->mxedt]);
   e_refresh();
   continue;
  }
  wl_pump_once(1);                 /* block in the shared poll, then dispatch */
 }
}

/* Allocate the screen-cell grid for MAXSCOL x MAXSLNS and match the wl_shm
   buffer to it.  Called once at init and again on resize (the e_u_ini_size
   slot), mirroring e_ini_size minus the X pixmap. */
static int e_w_ini_size(void)
{
 old_cursor_x = cur_x;
 old_cursor_y = cur_y;
 if (schirm)    free(schirm);
 if (altschirm) free(altschirm);
 schirm    = malloc(sizeof(SCREENCELL) * MAXSCOL * MAXSLNS);
 altschirm = malloc(sizeof(SCREENCELL) * MAXSCOL * MAXSLNS);
 if (!schirm || !altschirm)
  return -1;
#ifdef NEWSTYLE
 /* The border/extension byte-plane (window frames, e_make_xrect): the graphical
    builds use it unconditionally, so it must exist exactly like e_ini_size. */
 if (extbyte)    free(extbyte);
 if (altextbyte) free(altextbyte);
 extbyte    = calloc((size_t)MAXSCOL * MAXSLNS, 1);
 altextbyte = calloc((size_t)MAXSCOL * MAXSLNS, 1);
 if (!extbyte || !altextbyte)
  return -1;
#endif

 if (WpeWl.shm && WpeRender.font_width > 0)
 {
  int pw = WpeRender.font_width  * MAXSCOL;
  int ph = WpeRender.font_height * MAXSLNS;
  if (!WpeWl.pixels || pw != WpeWl.width || ph != WpeWl.height)
  {
   if (wl_alloc_buffer(pw, ph) == 0 && WpeRender.resize)
    WpeRender.resize(pw, ph);
  }
 }
 return 0;
}

/* Window-picture save/restore is unnecessary in the recompositing SCREENCELL
   model (the X11 backend uses a pixmap; we repaint from the window stack). */
static int e_w_change(PIC *pic) { (void)pic; return 0; }

/* Pump Wayland events once: block in the shared poll loop first when `block`,
   then read-and-dispatch if the wl fd has data, else dispatch what is buffered
   (never blocking on the wl fd when the poll woke for another descriptor). */
static void wl_pump_once(int block)
{
 struct pollfd p;

 wl_display_flush(WpeWl.display);
 if (block)
  wpe_fd_poll(-1);
 p.fd = wl_display_get_fd(WpeWl.display);
 p.events = POLLIN;
 p.revents = 0;
 if (poll(&p, 1, 0) > 0 && (p.revents & POLLIN))
  wl_display_dispatch(WpeWl.display);
 else
  wl_display_dispatch_pending(WpeWl.display);

 /* One relayout per dispatch batch, collapsing a resize-drag's burst of
    configure events into a single re-fit (see wl_apply_pending_resize). */
 wl_apply_pending_resize();
}

/* fk_w_mouse - report the current pointer state, exactly like the X11
   fk_x_mouse: a single NON-BLOCKING poll.  The incoming g[0] (the editor's
   "wait for press/release/poll" hint) is IGNORED -- fk_x_mouse ignores it too
   and never blocks; the drag loop in we_mouse.c does its own looping on g[1].
   On return g[0]=g[1]=the held-button mask, g[2]/g[3]=position in 1/8-cell units.

   Blocking on g[0] was wrong and caused a 100%-CPU hang: at startup we_main.c
   calls fk_mouse with g[0]=1 BEFORE e_w_getch has registered the wl fd, so a
   wait-loop's wpe_fd_poll() (no fds yet) returned instantly and spun forever --
   the editor never reached its input loop. */
static int fk_w_mouse(int *g)
{
 if (!g)
  return 0;

 if (WpeWl.display)
  wl_pump_once(0);     /* absorb pending pointer events, never block */
 g_mouse_pending = 0;  /* a click handled here is not also an e_w_getch event */

 g[0] = g_ptr_btn;
 g[1] = g_ptr_btn;
 g[2] = WpeRender.font_width  ? (g_ptr_px * 8) / WpeRender.font_width  : 0;
 g[3] = WpeRender.font_height ? (g_ptr_py * 8) / WpeRender.font_height : 0;
 return g[1];
}

/* Wayland has no explicit pointer grab: between a button press and its release
   the compositor delivers every pointer event to the focused surface (an
   implicit grab), so a scrollbar drag needs no XGrabPointer equivalent. */
static int fk_w_grab_pointer(int on)
{
 (void)on;
 return 0;
}

/* Wayland scrollbar-drag event step, mirroring fk_x_drag_next's contract:
   block for the next pointer event, then return 0 once the left button is
   released (drag done), or 1 with the current pointer position (pixels). */
static int fk_w_drag_next(int *px, int *py)
{
 if (!WpeWl.display)
  return 0;
 wl_pump_once(1);            /* block in the shared poll until a wl event arrives */
 if (!(g_ptr_btn & 1))       /* left button no longer held -> drag finished */
  return 0;
 *px = g_ptr_px;
 *py = g_ptr_py;
 return 1;
}

static void e_w_display_end(void) { wl_teardown(); }

/* wl_keytest - deterministic, compositor-free check of keysym_to_xwpe (the
   bug-prone translation that has to agree with the X11 path).  Drives a table
   of (keysym, produced UTF-8, modifiers) -> expected xwpe code and reports.
   Gated by XWPE_WL_KEYTEST; exits non-zero on any mismatch. */
static void wl_keytest(void)
{
 static const struct {
  xkb_keysym_t sym; const char *u8; int len, ctrl, shift, alt, want;
  const char *name;
 } t[] = {
  { XKB_KEY_a,           "a",    1, 0,0,0, 'a',      "a"          },
  { XKB_KEY_Z,           "Z",    1, 0,0,0, 'Z',      "Z"          },
  { XKB_KEY_Up,          "",     0, 0,0,0, CUP,      "Up"         },
  { XKB_KEY_Down,        "",     0, 0,0,0, CDO,      "Down"       },
  { XKB_KEY_Left,        "",     0, 0,0,0, CLE,      "Left"       },
  { XKB_KEY_Right,       "",     0, 0,0,0, CRI,      "Right"      },
  { XKB_KEY_Home,        "",     0, 0,0,0, POS1,     "Home"       },
  { XKB_KEY_End,         "",     0, 0,0,0, ENDE,     "End"        },
  { XKB_KEY_Prior,       "",     0, 0,0,0, BUP,      "PgUp"       },
  { XKB_KEY_Next,        "",     0, 0,0,0, BDO,      "PgDn"       },
  { XKB_KEY_F1,          "",     0, 0,0,0, F1,       "F1"         },
  { XKB_KEY_F10,         "",     0, 0,0,0, F10,      "F10"        },
  { XKB_KEY_F1,          "",     0, 0,1,0, F1 + 512, "Shift+F1"   },
  { XKB_KEY_F2,          "",     0, 0,0,1, AF2,      "Alt+F2"     },
  { XKB_KEY_F3,          "",     0, 1,0,0, CF3,      "Ctrl+F3"    },
  { XKB_KEY_Left,        "",     0, 1,0,0, CCLE,     "Ctrl+Left"  },
  { XKB_KEY_Delete,      "\177", 1, 0,0,0, ENTF,     "Delete"     },
  { XKB_KEY_Delete,      "\177", 1, 0,0,1, AltDel,   "Alt+Delete" },
  { XKB_KEY_BackSpace,   "\010", 1, 0,0,0, CtrlH,    "BackSpace"  },
  { XKB_KEY_ISO_Left_Tab,"",     0, 0,1,0, WPE_BTAB, "Shift+Tab"  },
  { XKB_KEY_Help,        "",     0, 0,0,0, HELP,     "Help"       }
 };
 int n = (int)(sizeof t / sizeof t[0]), i, fails = 0;

 for (i = 0; i < n; i++)
 {
  int got = keysym_to_xwpe(t[i].sym, t[i].u8, t[i].len,
                           t[i].ctrl, t[i].shift, t[i].alt);
  int ok = (got == t[i].want);
  if (!ok) fails++;
  fprintf(stderr, "  %-12s want=%d got=%d %s\n",
          t[i].name, t[i].want, got, ok ? "OK" : "FAIL");
 }
 fprintf(stderr, "keysym_to_xwpe selftest: %d/%d passed\n", n - fails, n);
 _exit(fails ? 4 : 0);
}

/* WpeWaylandInit - entry point from we_unix.c.  Returns 0 once the native
   surface drives the editor, non-zero to fall back to X11/XWayland.
   During bring-up the native editor path is gated behind XWPE_WL_NATIVE and
   the rest falls back, so `xwpe` always opens a window. */
int WpeWaylandInit(int *argc, char **argv)
{
 const char *selftest;
 int cw, ch, cols, rows;

 (void)argc;
 (void)argv;

 if (getenv("XWPE_WL_KEYTEST"))
  wl_keytest();                   /* does not return; no compositor needed */

 selftest = getenv("XWPE_WL_SELFTEST");
 if (selftest && *selftest)
  wl_selftest(selftest);          /* does not return */

 g_wl_uidump = getenv("XWPE_WL_UIDUMP");
 g_wl_mousetest = getenv("XWPE_WL_MOUSETEST");
 g_wl_dump_path = getenv("XWPE_WL_DUMP");   /* e_w_refresh mirrors each frame here */
 {
  const char *after = getenv("XWPE_WL_UIDUMP_AFTER");
  g_wl_dump_after = (after && *after) ? atoi(after) : 0;
 }

 if (wl_connect_and_bind() != 0)
 {
  WPE_TRACE("wayland", "no usable compositor; falling back to X11\n");
  return 1;
 }

 /* Size the window to a cell grid (probe the font before the buffer exists). */
 wpe_render_wayland_probe_cell(&cw, &ch);
 /* Initial window size: an 80x30 cell grid by default, or an explicit pixel
    size via XWPE_WL_WIDTH/XWPE_WL_HEIGHT (lets a test match another backend's
    geometry so dialog-position assertions line up). */
 {
  const char *ew = getenv("XWPE_WL_WIDTH");
  const char *eh = getenv("XWPE_WL_HEIGHT");
  WpeWl.width  = (ew && *ew) ? atoi(ew) : 80 * cw;
  WpeWl.height = (eh && *eh) ? atoi(eh) : 30 * ch;
  if (WpeWl.width  < cw * 8) WpeWl.width  = cw * 8;
  if (WpeWl.height < ch * 4) WpeWl.height = ch * 4;
 }

 wl_create_window();
 while (WpeWl.running && !WpeWl.configured
        && wl_display_dispatch(WpeWl.display) != -1)
  ;
 if (!WpeWl.configured || wpe_render_wayland_init() != 0)
 {
  WPE_TRACE("wayland", "surface/renderer bring-up failed; falling back to X11\n");
  wl_teardown();
  return 1;
 }

 /* Grid from the actual surface size and the now-known cell metrics. */
 cols = WpeWl.width  / (WpeRender.font_width  > 0 ? WpeRender.font_width  : cw);
 rows = WpeWl.height / (WpeRender.font_height > 0 ? WpeRender.font_height : ch);
 MAXSCOL = cols;
 MAXSLNS = rows;
 if (e_w_ini_size() != 0)
 {
  wl_teardown();
  return 1;
 }

 /* Publish the backend.  Reuse the audited schirm-only X functions; override
    the I/O (refresh/getch/kbhit/mouse/display/resize) with the Wayland ones.
    Mirrors WpeXtermInit's table -- the values it shares with us are unchanged. */
 e_s_u_clr         = e_s_x_clr;
 e_n_u_clr         = e_n_x_clr;
 e_frb_u_menue     = e_frb_x_menue;
 e_pr_u_col_kasten = e_pr_x_col_kasten;
 fk_u_cursor       = fk_x_cursor;
 fk_u_locate       = fk_x_locate;
 e_u_refresh       = e_w_refresh;
 e_u_getch         = e_w_getch;
 u_bioskey         = x_bioskey;
 e_u_sys_ini       = e_x_sys_ini;
 e_u_sys_end       = e_x_sys_end;
 e_u_system        = e_x_system;
 fk_u_putchar      = fk_x_putchar;
 fk_mouse          = fk_w_mouse;
 fk_u_grab_pointer = fk_w_grab_pointer;
 fk_u_drag_next    = fk_w_drag_next;
 e_u_kbhit         = e_w_kbhit;
 e_u_change        = e_w_change;
 e_u_ini_size      = e_w_ini_size;
 e_u_setlastpic    = e_setlastpic;
 WpeMouseChangeShape  = (void (*)(WpeMouseShape))WpeNullFunction;
 WpeMouseRestoreShape = (void (*)(WpeMouseShape))WpeNullFunction;
 WpeDisplayEnd     = e_w_display_end;
 e_u_switch_screen = WpeZeroFunction;
 e_u_d_switch_out  = (int (*)(int))WpeZeroFunction;
 e_u_deb_out       = e_t_deb_out;
 if (WpeWl.ddm && WpeWl.ddev)     /* OS clipboard via wl_data_device */
 {
  e_clip_os_set = e_clip_w_set;   /* ^C / ^Ins -> the Wayland selection */
  e_clip_os_get = e_clip_w_get;   /* ^V <- another app's selection      */
 }

 MCI = 7;   /* scrollbar track  (ACS via draw_acs, as X11) */
 MCA = 11;  /* scrollbar thumb */
 RD1 = 1; RD2 = 2; RD3 = 3; RD4 = 4; RD5 = 5; RD6 = 6;
 RE1 = 1; RE2 = 2; RE3 = 3; RE4 = 4; RE5 = 5; RE6 = 6;
 WBT = 1;
 ctree[0] = "\016\022\030";
 ctree[1] = "\016\022\022";
 ctree[2] = "\016\030\022";
 ctree[3] = "\025\022\022";
 ctree[4] = "\016\022\022";

 WPE_TRACE("wayland", "native backend up: %dx%d, %dx%d cells\n",
           WpeWl.width, WpeWl.height, MAXSCOL, MAXSLNS);
 return 0;
}

#endif /* HAVE_WAYLAND */
