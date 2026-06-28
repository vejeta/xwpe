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

#include <poll.h>
#include "edit.h"            /* SCREENCELL, schirm/altschirm, MAXSCOL/MAXSLNS,
                                e_gt_char/e_gt_col, the ATTR_ and CELL_ macros
                                (unixmakr.h), the e_u_ and fk_u_ function-pointer
                                slots, MCI/MCA/RD/RE/WBT/ctree, WpeEditor, cur_x,
                                WpeNullFunction/WpeZeroFunction */
#include "we_render.h"
#include "we_wayland.h"
#include "we_fdloop.h"
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
{ (void)data; (void)kbd; (void)serial; (void)surface; WpeWl.kbd_focus = 0; }

static void kbd_key(void *data, struct wl_keyboard *kbd, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state)
{
 xkb_keycode_t code = key + 8;   /* evdev -> xkb keycode offset */
 xkb_keysym_t sym;
 char u8[16];
 int u8len, ctrl, shift, alt;

 (void)data; (void)kbd; (void)serial; (void)time;
 if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !WpeWl.xkb_state)
  return;
 sym = xkb_state_key_get_one_sym(WpeWl.xkb_state, code);
 u8len = xkb_state_key_get_utf8(WpeWl.xkb_state, code, u8, sizeof u8);
 wl_active_mods(&ctrl, &shift, &alt);
 {
  int xwc = keysym_to_xwpe(sym, u8, u8len, ctrl, shift, alt);
  WPE_TRACE("wayland", "kbd_key sym=0x%x u8len=%d ctrl=%d shift=%d alt=%d -> %d\n",
            (unsigned)sym, u8len, ctrl, shift, alt, xwc);
  wl_key_push(xwc);
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
{ (void)data; (void)kbd; (void)rate; (void)delay; }

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

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
 (void)data; (void)p; (void)serial; (void)surface;
 g_ptr_px = wl_fixed_to_int(sx);
 g_ptr_py = wl_fixed_to_int(sy);
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

 (void)data; (void)p; (void)serial; (void)time;
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
 wl_surface_commit(WpeWl.surface);
}

static void wl_teardown(void)
{
 if (WpeWl.xkb_state)  xkb_state_unref(WpeWl.xkb_state);
 if (WpeWl.xkb_keymap) xkb_keymap_unref(WpeWl.xkb_keymap);
 if (WpeWl.xkb_ctx)    xkb_context_unref(WpeWl.xkb_ctx);
 if (WpeWl.keyboard)   wl_keyboard_release(WpeWl.keyboard);
 if (WpeWl.pointer)    wl_pointer_release(WpeWl.pointer);
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
 int n  = y * MAXSCOL + x;
 int sc = e_gt_char(x, y);
 int sa = e_gt_col(x, y);
 int base = ATTR_BASE(sa);
 int fg = invert ? base / 16
                 : (ATTR_IS_TC(sa) ? 16 + ATTR_TC_SLOT(sa) : base % 16);
 int bg = invert ? base % 16 : base / 16;
 int cw = (schirm[n].flags & CELL_WIDE) ? 2 : 1;

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
 cols = 80;
 rows = 30;
 WpeWl.width  = cols * cw;
 WpeWl.height = rows * ch;

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
