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
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/mman.h>

#include "edit.h"            /* SCREENCELL, schirm/altschirm, MAXSCOL/MAXSLNS,
                                e_gt_char/e_gt_col + ATTR_*/CELL_* (unixmakr.h) */
#include "we_render.h"
#include "we_wayland.h"
#include "we_trace.h"

extern int e_codepoint_to_utf8(int cp, unsigned char *out);  /* we_edit.c */

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

/* Forward declarations (used by the selftest, defined further down). */
static void e_w_render_dirty_cells(int force);
static void wl_selftest_fill_grid(void);

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

/* xdg_surface.configure: the compositor is ready for content.  Acknowledge,
   (re)allocate the buffer if needed, paint, attach and commit.  This is also
   the standard place to perform the very first paint. */
static void xdg_surface_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
 (void)data;
 xdg_surface_ack_configure(xs, serial);

 if (!WpeWl.pixels)
 {
  if (wl_alloc_buffer(WpeWl.width, WpeWl.height) < 0)
   return;
 }
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

/* xdg_toplevel.configure: a size suggestion (and window state).  Headless and
   first-map often give 0x0 -- keep our default in that case. */
static void xdg_toplevel_configure(void *data, struct xdg_toplevel *tl,
                                   int32_t w, int32_t h, struct wl_array *states)
{
 (void)data; (void)tl; (void)states;
 if (w > 0 && h > 0 && (w != WpeWl.width || h != WpeWl.height))
 {
  /* Drop the stale buffer; xdg_surface_configure reallocates at the new size. */
  WpeWl.width = w;
  WpeWl.height = h;
  if (WpeWl.pixels)
  {
   munmap(WpeWl.pixels, WpeWl.shm_size);
   WpeWl.pixels = NULL;
  }
 }
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
}
static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{ (void)data; (void)reg; (void)name; }
static const struct wl_registry_listener registry_listener = {
 registry_global, registry_global_remove
};

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
 wl_surface_commit(WpeWl.surface);
}

static void wl_teardown(void)
{
 if (WpeWl.buffer)        wl_buffer_destroy(WpeWl.buffer);
 if (WpeWl.pixels && WpeWl.pixels != MAP_FAILED) munmap(WpeWl.pixels, WpeWl.shm_size);
 if (WpeWl.shm_fd >= 0)   close(WpeWl.shm_fd);
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

/* WpeWaylandInit - entry point from we_unix.c.  Returns 0 once the native
   surface drives the editor, non-zero to fall back to X11/XWayland.
   During bring-up the native editor path is gated behind XWPE_WL_NATIVE and
   the rest falls back, so `xwpe` always opens a window. */
int WpeWaylandInit(int *argc, char **argv)
{
 const char *selftest;

 (void)argc;
 (void)argv;

 selftest = getenv("XWPE_WL_SELFTEST");
 if (selftest && *selftest)
  wl_selftest(selftest);          /* does not return */

 /* The renderer and input pumps are not wired yet, so even a reachable
    compositor defers to X11/XWayland for now (W2/W3 flip this). */
 {
  struct wl_display *probe = wl_display_connect(NULL);
  if (!probe)
  {
   WPE_TRACE("wayland", "no compositor reachable; using X11\n");
   return 1;
  }
  WPE_TRACE("wayland", "compositor reachable; native backend not yet interactive, using XWayland\n");
  wl_display_disconnect(probe);
  return 1;
 }
}

#endif /* HAVE_WAYLAND */
