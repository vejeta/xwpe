#ifndef __XWPE_H
#define __XWPE_H
/*-------------------------------------------------------------------------*\
  <Xwpe.h> -- Header file for core Xwpe functions

  Date      Programmer  Description
  04/27/97  Dennis      Created for xwpe reorganization.
\*-------------------------------------------------------------------------*/

typedef enum wpeMouseShape {
 WpeEditingShape, WpeDebuggingShape, WpeWorkingShape, WpeErrorShape,
 WpeSelectionShape, WpeLastShape
} WpeMouseShape;

/* Checks if programming editor is running (old variable currently used) */
#define WpeIsProg() (e_we_sw & 2)

/* Checks if x windows is running (old variable currently used) */
#define WpeIsXwin() (e_we_sw & 1)

/* Graphical backend selected at startup when running as xwpe (WpeIsXwin()).
   Console mode (wpe) never consults this.  The choice is made once by
   e_pick_gfx_backend() in we_unix.c from the session environment; both
   backends drive the same SCREENCELL grid and Cairo+Pango glyph renderer. */
typedef enum WpeGfxBackend {
 WPE_GFX_X11 = 0,    /* Xlib backend (we_xterm.c); also covers XWayland */
 WPE_GFX_WAYLAND = 1 /* native Wayland client (we_wayland.c) */
} WpeGfxBackend;

extern WpeGfxBackend e_gfx_backend;

/* True only when xwpe is driving a native Wayland surface (not XWayland). */
#define WpeIsWayland() (WpeIsXwin() && e_gfx_backend == WPE_GFX_WAYLAND)

#define WpeMalloc(x) malloc(x)
#define WpeRealloc(x, y) realloc(x, y)
#define WpeFree(x) free(x)

#endif

