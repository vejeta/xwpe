#ifndef __WEXTERM_H
#define __WEXTERM_H
/*-------------------------------------------------------------------------*\
  <WeXterm.h> -- Header file of Xwpe routines for X window support

  Copyright (C) 1997 Dennis Payne
  Copyright (C) 2026 Juan Manuel Mendez Rey
  This is free software; see the file COPYING (GPL-2).

  Date      Programmer  Description
  05/04/97  Dennis      Created for xwpe reorganization.
\*-------------------------------------------------------------------------*/

#ifdef __cplusplus
extern "C" {
#endif

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Includes
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
#include <X11/Xlib.h>
#include <X11/extensions/sync.h>
#ifdef HAVE_XFT
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#endif
#include "Xwpe.h"

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Defines
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  New Types
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
typedef struct wpeXStruct {
 Display *display;
 int screen;
 Window window;
 GC gc;
 XFontStruct *font;
 Atom delete_atom, protocol_atom, selection_atom, text_atom, property_atom,
      sync_request_atom, clipboard_atom, utf8_atom, targets_atom;
 XSyncCounter sync_counter;
 XSyncValue sync_value;
 int font_height, font_width;
 int font_descent;     /* baseline offset; from the core font, or Xft when the
                          legacy "8x13" core font is unavailable (no xfonts-base) */
 int altmask;
 int colors[16];
 WpeMouseShape shape_list[2];
 char *selection;
#ifdef HAVE_XFT
 XftFont *xftfont;
 XftDraw *xftdraw;
 XftColor xftcolors[24];   /* 16 base + up to 8 LSP truecolor slots (we_lsp.h
                              LSP_SEM_TC_MAX): a fg index 16+slot picks a slot */
 Pixmap backbuf;
 FcPattern *xftpattern;
 FcFontSet *xftfont_set;
#endif
} WpeXStruct;

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Global Variables
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
extern WpeXStruct WpeXInfo;


void WpeXInit(int *argc, char **argv);
void WpeXMouseChangeShape(WpeMouseShape new_shape);
void WpeXMouseRestoreShape();

/* Reload the font at the current libx11-compat backing scale after a mixed-DPI
   monitor move. Returns nonzero when the font changed so the caller recomputes
   the grid. No-op (returns 0) on a real X server or without Xft. */
int e_x_refit_font_for_dpi(void);

/* Re-advertise WM_NORMAL_HINTS resize increments from the current font metrics.
   Called after a DPI refit so the shim's cached PResizeInc snap grid tracks the
   new cell size; keeps WM_NORMAL_HINTS accurate on a real X server. */
void e_x_publish_size_hints(void);

/* The 16 palette colours as the X server allocated them, for the renderers
   that paint with the editor's own colours.  e_x_palette_rgb16 gives the
   server's 16-bit channels; e_x_palette_rgb is the display table's slot
   (e_u_palette_rgb): 8-bit channels, 1 when idx is a palette index. */
void e_x_palette_rgb16(int idx, unsigned short rgb[3]);
int e_x_palette_rgb(int idx, unsigned char rgb[3]);
/* The colormap those colours live in: the default one, or the private one
   the pcmap resource asked for. */
Colormap e_x_palette_colormap(void);
#ifdef HAVE_XFT
/* The glyph surface for the back buffer, made in that same colormap. */
XftDraw *e_x_xftdraw_create(void);
#endif

#ifdef __cplusplus
}
#endif

#endif

