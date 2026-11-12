/*-------------------------------------------------------------------------*\
  <WeXterm.c> -- Xwpe routines for X window support

  Date      Programmer  Description
  05/04/97  Dennis      Created based on functions from "we_xterm.c".
\*-------------------------------------------------------------------------*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Original header of "we_xterm.c"
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* we_xterm.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#ifndef NO_XWINDOWS

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  Includes
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/Xatom.h>
#include "Xwpe.h"
#include "WeXterm.h"
#include "we_render.h"

/* needed for the time being to call old routines */
#include "model.h"
#include "edit.h"
#include "we_lsp.h"           /* e_lsp_sem_slot_rgb: semantic-token truecolor */

#ifndef DEFAULT_ALTMASK
#if defined(__linux__) || defined(__osf__)			/* a.r. */
#define DEFAULT_ALTMASK Mod1Mask
#else
#define DEFAULT_ALTMASK Mod4Mask
#endif
#endif

/* Information from X that is needed throughout execution */
WpeXStruct WpeXInfo;

/* Standard colors (after initialization pointers may not be valid) */
static char *WpeXColorNames[16] = {
 "Black",
 "Red3",
 "Forest Green",
 "Brown",
 "Dark Slate Blue",
 "Violet Red",
 "Turquoise3",
 "Light Gray",
 "Dark Slate Grey",
 "Red1",
 "Green",
 "Yellow",
 "Blue",
 "Violet",
 "Turquoise1",
 "White"
};

#define OPTION_TABLE_SIZE 7

/* Translation table for command line option to X resources */
static XrmOptionDescRec WpeXOptionTable[OPTION_TABLE_SIZE] = {
 {"-display",  ".display",  XrmoptionSepArg, (XPointer) NULL},
 {"-fn",       ".font",     XrmoptionSepArg, (XPointer) NULL},
 {"-font",     ".font",     XrmoptionSepArg, (XPointer) NULL},
 {"-g",        ".geometry", XrmoptionSepArg, (XPointer) NULL},
 {"-geometry", ".geometry", XrmoptionSepArg, (XPointer) NULL},
 {"-iconic",   ".iconic",   XrmoptionNoArg,  (XPointer) "on"},
 {"-pcmap",    ".pcmap",    XrmoptionNoArg,  (XPointer) "on"}
};

/* Shape for mouse in X */
static int WpeXMouseDefault[WpeLastShape] = {132, 142, 150, 88, 124};
static Cursor WpeXMouseCursor[WpeLastShape];

/* Backing scale the font was last built at, read from the libx11-compat root
   window property. Lets a mixed-DPI monitor move detect that the font must be
   reloaded at the new density. 1.0 (and unchanging) on a real X server. */
static double wpe_font_dpi_scale = 1.0;

/* Resolved core-font name from the last WpeXFontGet, so the DPI refit can
   reload it (the shim re-scales its metrics by the current backing) to reseed
   the Xft point size. */
static char wpe_font_name[128] = "8x13";

/* Read the libx11-compat HiDPI backing scale advertised on the root window
   (_LIBX11_COMPAT_HIDPI_SCALE, a CARDINAL of scale * 1000). Returns 1.0 when
   the property is absent, i.e. on a real X server or a non-HiDPI host, so the
   font handling there is unchanged. */
static double e_x_read_hidpi_scale(void)
{
 Atom scale_atom, actual_type;
 int actual_format;
 unsigned long nitems, bytes_after;
 unsigned char *prop = NULL;
 double scale = 1.0;

 if (!WpeXInfo.display)
  return 1.0;
 scale_atom = XInternAtom(WpeXInfo.display, "_LIBX11_COMPAT_HIDPI_SCALE", True);
 if (scale_atom == None)
  return 1.0;
 if (XGetWindowProperty(WpeXInfo.display,
       RootWindow(WpeXInfo.display, WpeXInfo.screen), scale_atom, 0, 1, False,
       XA_CARDINAL, &actual_type, &actual_format, &nitems, &bytes_after,
       &prop) == Success
     && prop)
 {
  if (actual_type == XA_CARDINAL && actual_format == 32 && nitems >= 1)
  {
   unsigned long fixed = *(unsigned long *)prop;
   if (fixed > 0)
    scale = (double)fixed / 1000.0;
  }
  XFree(prop);
 }
 if (scale < 0.5 || scale > 8.0)
  scale = 1.0;
 return scale;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  WpeXFontGet - Get font settings from the X resources.

    Parameters:
      xresdb       (In)  X resource database
      name_list    (In)  Name quark list (second element unset)
      class_list   (In)  Class quark list (second element unset)
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
void WpeXFontGet(XrmDatabase xresdb, XrmQuark *name_list,
                 XrmQuark *class_list)
{
 char *font_name = "8x13";
 XrmRepresentation return_value;
 XrmValue xval;

 wpe_font_dpi_scale = e_x_read_hidpi_scale();
 name_list[1] = XrmStringToQuark("font");
 class_list[1] = XrmStringToQuark("Font");
 if (XrmQGetResource(xresdb, name_list, class_list, &return_value, &xval))
 {
  font_name = (char *)xval.addr;
 }
 snprintf(wpe_font_name, sizeof(wpe_font_name), "%s", font_name);
 WpeXInfo.font = XLoadQueryFont(WpeXInfo.display, font_name);
#ifdef HAVE_XFT
 /* With Xft built in, the legacy "8x13" core bitmap font (Debian: xfonts-base)
    is OPTIONAL: it only seeds the initial point size; Xft overrides the metrics
    below and does all the drawing.  Do NOT exit when it -- or any core font --
    is missing, so xwpe runs on a minimal X server (no core fonts installed). */
 if (WpeXInfo.font &&
     WpeXInfo.font->max_bounds.width == WpeXInfo.font->min_bounds.width)
 {
  WpeXInfo.font_height  = WpeXInfo.font->max_bounds.ascent +
                          WpeXInfo.font->max_bounds.descent;
  WpeXInfo.font_width   = WpeXInfo.font->max_bounds.width;
  WpeXInfo.font_descent = WpeXInfo.font->max_bounds.descent;
 }
 else
 {
  if (WpeXInfo.font)
   { XFreeFont(WpeXInfo.display, WpeXInfo.font); WpeXInfo.font = NULL; }
  WpeXInfo.font_height  = 13;   /* 8x13 seed values; Xft overrides them next */
  WpeXInfo.font_width   = 8;
  WpeXInfo.font_descent = 2;
 }
#else
 /* No Xft: the core font IS the renderer, so a fixed-width one is required. */
 if (WpeXInfo.font == NULL)
 {
  fprintf(stderr, "Xwpe: unable to open font \"%s\", exiting ...\n", font_name);
  exit(-1);
 }
 if (WpeXInfo.font->max_bounds.width != WpeXInfo.font->min_bounds.width)
 {
  fprintf(stderr, "Xwpe: Font \"%s\" not fixed width, using \"8x13\"\n",
    font_name);
  XFreeFont(WpeXInfo.display, WpeXInfo.font);
  WpeXInfo.font = XLoadQueryFont(WpeXInfo.display, "8x13");
  if (WpeXInfo.font == NULL)
  {
   fprintf(stderr, "Xwpe: unable to open font \"8x13\", exiting ...\n");
   exit(-1);
  }
 }
 WpeXInfo.font_height  = WpeXInfo.font->max_bounds.ascent +
                         WpeXInfo.font->max_bounds.descent;
 WpeXInfo.font_width   = WpeXInfo.font->max_bounds.width;
 WpeXInfo.font_descent = WpeXInfo.font->max_bounds.descent;
#endif

#ifdef HAVE_XFT
 /* Initialize Xft font using fontconfig pattern matching (st pattern).
  * Derive point size from the core X font metrics so the Xft font is
  * the same visual size. */
 {
  char xft_spec[64];
  FcPattern *configured, *match;
  FcResult fcres;
  int pt_size = WpeXInfo.font_height <= 13 ? 10 : WpeXInfo.font_height - 3;

  snprintf(xft_spec, sizeof(xft_spec), "monospace:size=%d", pt_size);
  WpeXInfo.xftpattern = FcNameParse((const FcChar8 *)xft_spec);
  if (!WpeXInfo.xftpattern)
  {
   fprintf(stderr, "Xwpe: FcNameParse failed for \"%s\"\n", xft_spec);
   goto xft_skip;
  }

  configured = FcPatternDuplicate(WpeXInfo.xftpattern);
  if (!configured)
   goto xft_skip;

  FcConfigSubstitute(NULL, configured, FcMatchPattern);
  XftDefaultSubstitute(WpeXInfo.display, WpeXInfo.screen, configured);

  match = FcFontMatch(NULL, configured, &fcres);
  if (!match)
  {
   FcPatternDestroy(configured);
   goto xft_skip;
  }

  WpeXInfo.xftfont = XftFontOpenPattern(WpeXInfo.display, match);
  if (!WpeXInfo.xftfont)
  {
   FcPatternDestroy(configured);
   FcPatternDestroy(match);
   goto xft_skip;
  }

  /* Override font metrics from Xft */
  WpeXInfo.font_height = WpeXInfo.xftfont->ascent + WpeXInfo.xftfont->descent;
  WpeXInfo.font_width = WpeXInfo.xftfont->max_advance_width;
  WpeXInfo.font_descent = WpeXInfo.xftfont->descent;

  /* Save configured pattern for fallback lookups */
  FcPatternDestroy(WpeXInfo.xftpattern);
  WpeXInfo.xftpattern = configured;
  WpeXInfo.xftfont_set = NULL;

  goto xft_done;
xft_skip:
  fprintf(stderr, "Xwpe: Xft font init failed, falling back to core X font\n");
  WpeXInfo.xftfont = NULL;
xft_done:
  ;
 }
#endif

 return ;
}

/* e_x_publish_size_hints - Re-send WM_NORMAL_HINTS with the current font
   metrics as the ICCCM resize increment. WpeXGeometryGet publishes these once at
   startup (width_inc=font_width, height_inc=font_height, min = 80x24 cells), but
   after e_x_refit_font_for_dpi reloads the font at a different backing scale the
   cell size changes, so the increment a resize snaps to must be re-advertised.
   Under libx11-compat there is no window manager; the shim caches PResizeInc and
   snaps drag-end resizes to it, so a stale increment leaves a sub-cell remainder
   band. On a real X server this just keeps WM_NORMAL_HINTS accurate. */
void e_x_publish_size_hints(void)
{
 XSizeHints size_hints;

 if (!WpeXInfo.display || !WpeXInfo.window)
  return;
 if (WpeXInfo.font_width <= 0 || WpeXInfo.font_height <= 0)
  return;
 memset(&size_hints, 0, sizeof(size_hints));
 size_hints.flags = PResizeInc | PMinSize | PBaseSize;
 size_hints.width_inc  = WpeXInfo.font_width;
 size_hints.height_inc = WpeXInfo.font_height;
 size_hints.min_width  = size_hints.width_inc * 80;
 size_hints.min_height = size_hints.height_inc * 24;
 size_hints.base_width = size_hints.base_height = 0;
 XSetWMNormalHints(WpeXInfo.display, WpeXInfo.window, &size_hints);
}

/* e_x_refit_font_for_dpi - Reload the font at the current backing scale when a
   mixed-DPI monitor move changed it (libx11-compat advertises the scale on the
   root window; see e_x_read_hidpi_scale). The libx11-compat core-font path
   scales its metrics by the backing, so the Xft point size -- seeded from those
   metrics -- and thus the glyph size track the destination monitor. Returns 1
   when the font was reloaded (caller recomputes MAXSCOL/MAXSLNS and the back
   buffer), 0 when the scale was unchanged. No-op on a real X server, where the
   scale is always 1.0. */
int e_x_refit_font_for_dpi(void)
{
#ifdef HAVE_XFT
 double scale = e_x_read_hidpi_scale();
 int seed_h;
 int pt_size;
 char xft_spec[64];
 FcPattern *parsed, *configured, *match;
 FcResult fcres;
 XftFont *newfont;

 if (scale - wpe_font_dpi_scale < 0.001 &&
     wpe_font_dpi_scale - scale < 0.001)
  return 0;

 /* Reseed the point size from the core font reloaded at the new backing (the
    shim rescales its metrics); fall back to the 8x13 seed when no core font is
    available. */
 if (WpeXInfo.font)
  { XFreeFont(WpeXInfo.display, WpeXInfo.font); WpeXInfo.font = NULL; }
 WpeXInfo.font = XLoadQueryFont(WpeXInfo.display, wpe_font_name);
 if (WpeXInfo.font &&
     WpeXInfo.font->max_bounds.width == WpeXInfo.font->min_bounds.width)
  seed_h = WpeXInfo.font->max_bounds.ascent +
           WpeXInfo.font->max_bounds.descent;
 else
  seed_h = 13;
 pt_size = seed_h <= 13 ? 10 : seed_h - 3;

 snprintf(xft_spec, sizeof(xft_spec), "monospace:size=%d", pt_size);
 parsed = FcNameParse((const FcChar8 *)xft_spec);
 if (!parsed)
  return 0;
 configured = FcPatternDuplicate(parsed);
 FcPatternDestroy(parsed);
 if (!configured)
  return 0;
 FcConfigSubstitute(NULL, configured, FcMatchPattern);
 XftDefaultSubstitute(WpeXInfo.display, WpeXInfo.screen, configured);
 match = FcFontMatch(NULL, configured, &fcres);
 if (!match)
  { FcPatternDestroy(configured); return 0; }
 newfont = XftFontOpenPattern(WpeXInfo.display, match);
 if (!newfont)
  { FcPatternDestroy(configured); FcPatternDestroy(match); return 0; }

 if (WpeXInfo.xftfont)
  XftFontClose(WpeXInfo.display, WpeXInfo.xftfont);
 WpeXInfo.xftfont = newfont;
 WpeXInfo.font_height  = WpeXInfo.xftfont->ascent + WpeXInfo.xftfont->descent;
 WpeXInfo.font_width   = WpeXInfo.xftfont->max_advance_width;
 WpeXInfo.font_descent = WpeXInfo.xftfont->descent;

 if (WpeXInfo.xftpattern)
  FcPatternDestroy(WpeXInfo.xftpattern);
 WpeXInfo.xftpattern = configured;

 wpe_font_dpi_scale = scale;
 /* The cell size just changed, so re-advertise the resize increment. Under
    libx11-compat the shim caches PResizeInc and snaps drag-end resizes to it;
    without this it would keep snapping to the previous monitor's cell, leaving
    a sub-cell remainder band on the new display. */
 e_x_publish_size_hints();
 return 1;
#else
 return 0;
#endif
}

/* The 16 palette colours the way the X server allocated them: the 16-bit
   channels XAllocColor returned for each (the colour actually used, which on a
   TrueColor visual is the requested one rounded to the visual's depth), and the
   colormap the pixels live in.  Filled once, by WpeXColorGet.  Everything that
   needs the editor's colours as RGB (the Cairo renderer, the Xft glyph colours,
   the User Screen) reads these, so none of them asks the server again and none
   can ask the wrong colormap when the pcmap resource gives the editor a private
   one. */
#define X_PALETTE_SIZE 16
static unsigned short x_palette_rgb16[X_PALETTE_SIZE][3];
static Colormap x_palette_cmap = None;

/**
 * x_palette_record - Remember the RGB the server gave palette entry @idx.
 * @idx: palette index, 0..15
 * @color: the XColor XAllocColor filled in
 */
static void x_palette_record(int idx, const XColor *color)
{
 x_palette_rgb16[idx][0] = color->red;
 x_palette_rgb16[idx][1] = color->green;
 x_palette_rgb16[idx][2] = color->blue;
}

/**
 * x_palette_record_mono - Remember the colour of a pixel on a one-bit display.
 * @idx: palette index, 0..15
 * @pixel: the pixel the editor uses for that entry
 *
 * A one-bit screen has no colour cells to allocate: a pixel is black or white,
 * so the entry is the white of the screen when its pixel is the screen's white
 * and black otherwise.
 */
static void x_palette_record_mono(int idx, unsigned long pixel)
{
 XColor color;
 unsigned short level = 0;

 if (pixel == WhitePixel(WpeXInfo.display, WpeXInfo.screen))
  level = 0xffff;
 color.red = color.green = color.blue = level;
 x_palette_record(idx, &color);
}

/**
 * e_x_palette_colormap - The colormap the editor's 16 colours live in.
 *
 * The screen's default colormap, or the private one the pcmap resource gave
 * the editor.  A colour built for a glyph must be made in the same colormap as
 * the pixels it is drawn next to, or on a palette-indexed display it comes out
 * as the wrong colour; and it must be given back to that same colormap when
 * the editor ends.
 */
Colormap e_x_palette_colormap(void)
{
 if (x_palette_cmap != None)
  return x_palette_cmap;
 return DefaultColormap(WpeXInfo.display, WpeXInfo.screen);
}

#ifdef HAVE_XFT
/**
 * e_x_xftdraw_trace - Say in $XWPE_UI_TRACE which colormap a glyph surface got.
 * @cmap: the colormap the new XftDraw was made with
 *
 * A test (or someone chasing wrong glyph colours under the pcmap resource)
 * reads here whether the text is painted in the colormap the palette lives in.
 * On a true-colour display the two colormaps paint the same pixels, so this
 * line is the only place the choice can be seen.  Nothing is written when the
 * variable is not set.
 */
static void e_x_xftdraw_trace(Colormap cmap)
{
 wpe_ui_trace("xftdraw colormap=%lu private=%d", (unsigned long)cmap,
              cmap != DefaultColormap(WpeXInfo.display, WpeXInfo.screen));
}

/**
 * e_x_xftdraw_create - Make the glyph surface for the back buffer.
 *
 * Every place that rebuilds the text surface (start, resize, font change)
 * calls this, so the editor's text is always painted in the colormap the 16
 * colours were allocated in.  With the pcmap resource on, that is the private
 * colormap, and text made in the screen's default one would come out in the
 * wrong colours on a palette display.
 *
 * Return: the new XftDraw, to be given back with XftDrawDestroy.
 */
XftDraw *e_x_xftdraw_create(void)
{
 Colormap cmap = e_x_palette_colormap();

 e_x_xftdraw_trace(cmap);
 return XftDrawCreate(WpeXInfo.display, WpeXInfo.backbuf,
                      DefaultVisual(WpeXInfo.display, WpeXInfo.screen), cmap);
}
#endif

/**
 * e_x_palette_rgb16 - The colour the X server gave palette entry @idx.
 * @idx: palette index, 0..15
 * @rgb: receives red, green and blue as the server's 16-bit channels
 *
 * For the renderers of the X11 backend (Cairo, Xft): they paint the editor's
 * colours exactly as the server allocated them.  An index out of range gives
 * black.
 */
void e_x_palette_rgb16(int idx, unsigned short rgb[3])
{
 int ch;

 for (ch = 0; ch < 3; ch++)
  rgb[ch] = (idx >= 0 && idx < X_PALETTE_SIZE) ? x_palette_rgb16[idx][ch] : 0;
}

/**
 * e_x_palette_rgb - The X11 display's answer to "what colour is entry @idx".
 * @idx: palette index, 0..15
 * @rgb: receives red, green and blue, 8 bits each
 *
 * Fills the display table's palette slot (e_u_palette_rgb), so the User Screen
 * matches a program's colours against the ones the editor really shows.
 *
 * Return: 1 for a palette index, 0 for any other number.
 */
int e_x_palette_rgb(int idx, unsigned char rgb[3])
{
 unsigned short wide[3];
 int ch;

 if (idx < 0 || idx >= X_PALETTE_SIZE)
  return 0;
 e_x_palette_rgb16(idx, wide);
 for (ch = 0; ch < 3; ch++)
  rgb[ch] = wide[ch] >> 8;
 return 1;
}

/**
 * e_x_palette_pixel - The pixel the X server gave palette entry @idx.
 * @idx: palette index, 0..15
 *
 * For a renderer that turns its own picture into the editor's pixels: every
 * colour on the window is then one the editor allocated, in the colormap the
 * window uses, so a palette display shows the editor's colours and not
 * whatever happens to sit in other cells.  Any number outside the palette
 * gives entry 0's pixel.
 *
 * Return: the pixel value.
 */
unsigned long e_x_palette_pixel(int idx)
{
 if (idx < 0 || idx >= X_PALETTE_SIZE)
  return (unsigned long)WpeXInfo.colors[0];
 return (unsigned long)WpeXInfo.colors[idx];
}

/* How the painted picture reaches the window.  Decided by WpeXColorGet once
   the colours are allocated, read by the renderer when it makes its surface. */
static EXRenderRoute x_render_route = E_X_RENDER_DIRECT;

/**
 * e_x_render_route - How the painted picture reaches the window.
 *
 * The renderer asks this when it starts, so it paints in the way the
 * display's visual can show the editor's colours.
 *
 * Return: the route WpeXColorGet chose.
 */
EXRenderRoute e_x_render_route(void)
{
 return x_render_route;
}

/**
 * x_render_route_name - The word the trace uses for a route.
 * @route: a route
 *
 * Return: "direct" or "indexed".
 */
static const char *x_render_route_name(EXRenderRoute route)
{
 if (route == E_X_RENDER_INDEXED)
  return "indexed";
 return "direct";
}

/**
 * x_render_route_trace - Say in $XWPE_UI_TRACE which route the display got.
 * @visual: the window's visual
 * @depth: the screen's depth
 *
 * Someone chasing wrong colours on an unusual display, or a test, reads here
 * the route, the visual class and the colormap the editor paints with; the
 * picture alone cannot tell them apart on a true-colour display.  Nothing is
 * written when the variable is not set.
 */
static void x_render_route_trace(Visual *visual, int depth)
{
 Colormap cmap = e_x_palette_colormap();

 wpe_ui_trace("palette route=%s class=%d depth=%d cmap=%lu private=%d",
              x_render_route_name(x_render_route), visual->class, depth,
              (unsigned long)cmap, x_palette_cmap != None);
}

/* The shallowest true-colour display whose red, green and blue masks give
   the editor's colours closely enough to paint them straight through the
   server (15-bit and 16-bit displays and up). */
#define X_RENDER_DIRECT_MIN_DEPTH 15

/**
 * x_render_route_choose - Decide how the picture reaches the window.
 * @visual: the window's visual
 * @depth: the screen's depth
 *
 * Called once the colours are allocated, before the renderer starts.  On a
 * true-colour display of 15 bits or more cairo paints into the back buffer
 * and the back buffer is copied to the window, as the editor always did.
 * Every other display (a palette, a grey or fixed-colour display, a
 * DirectColor one, a shallow true-colour one) gets the indexed route: cairo
 * paints in memory and each pixel becomes one of the 16 the editor
 * allocated, so the user sees the editor's own colours there too.
 */
static void x_render_route_choose(Visual *visual, int depth)
{
 if (visual->class == TrueColor && depth >= X_RENDER_DIRECT_MIN_DEPTH)
  x_render_route = E_X_RENDER_DIRECT;
 else
  x_render_route = E_X_RENDER_INDEXED;
 x_render_route_trace(visual, depth);
}

/* How a palette entry got its colour cell, as the trace names it.  Exact:
   the colour itself (a free cell, or a read-only cell that already holds
   it).  Nearest: an existing cell of a full colormap close to it.  Private:
   the default colormap was full and held nothing close, so the editor moved
   to a colormap of its own and allocated the colour exactly there.  Black:
   not even that worked, and the entry shows as the screen's black. */
#define X_PALETTE_LEVEL_EXACT   1
#define X_PALETTE_LEVEL_NEAREST 2
#define X_PALETTE_LEVEL_PRIVATE 3
#define X_PALETTE_LEVEL_BLACK   4

/* How far, per channel of 0..255, an existing cell may be from the colour an
   entry asks for and still be taken for it: close enough to read as the same
   colour, far enough that the cell does not belong to some other colour. */
#define X_PALETTE_NEAREST_MAX 32
/* The largest colormap whose cells are read to find the nearest one; a
   colormap with more cells than this is not a palette worth searching. */
#define X_PALETTE_QUERY_MAX 4096
/* The 16-bit channels XQueryColors gives, cut to the 8 bits compared. */
#define X_PALETTE_CHANNEL_SHIFT 8

/* The cells of the colormap the nearest colour is searched in, read once
   with XQueryColors, and which of them were tried and refused (a writable
   cell of another client cannot be shared). */
static XColor *x_palette_cells;
static char *x_palette_tried;
static int x_palette_ncells;
/* The colour each entry asked for (before the server rounded it) and how
   many entries have a cell so far, so two different colours never end up
   in one cell. */
static XColor x_palette_requested[X_PALETTE_SIZE];
static int x_palette_assigned;

/**
 * x_palette_resource_names - Read the colour names the X resources set.
 * @xresdb: X resource database
 * @name_list: name quark list (second element set here)
 * @class_list: class quark list (second element set here)
 * @names: receives, per entry, the name to allocate: the resource
 *         (Xwpe.color1 .. Xwpe.color16) when set, else the built-in one
 *
 * A user who changed a colour in ~/.Xdefaults sees it; the built-in names
 * stay at hand for when the user's name is not a colour.
 */
static void x_palette_resource_names(XrmDatabase xresdb, XrmQuark *name_list,
                                     XrmQuark *class_list,
                                     char *names[X_PALETTE_SIZE])
{
 XrmRepresentation return_value;
 XrmValue xval;
 char color_name[8];
 int loop;

 memset(color_name, 0, sizeof color_name);
 strcpy(color_name, "color");
 for (loop = 0; loop < X_PALETTE_SIZE; loop++)
 {
  names[loop] = WpeXColorNames[loop];
  /* color names start at 1 not 0 */
  if (loop < 9)
   color_name[5] = loop + '1';
  else
  {
   color_name[5] = '1';
   color_name[6] = loop - 9 + '0';
  }
  name_list[1] = XrmStringToQuark(color_name);
  class_list[1] = XrmStringToQuark(color_name);
  if (XrmQGetResource(xresdb, name_list, class_list, &return_value, &xval))
   names[loop] = (char *)xval.addr;
 }
}

/**
 * x_palette_choose_colormap - The colormap the editor allocates in first.
 * @xresdb: X resource database
 * @name_list: name quark list (second element set here)
 * @class_list: class quark list (second element set here)
 * @visual: the window's visual
 *
 * The screen's default colormap, so the editor's colours show without a
 * colormap flash; or, when the user asked for one with -pcmap (the pcmap
 * resource "on"), a private colormap set on the window.
 *
 * Return: the colormap.
 */
static Colormap x_palette_choose_colormap(XrmDatabase xresdb,
                                          XrmQuark *name_list,
                                          XrmQuark *class_list, Visual *visual)
{
 XrmRepresentation return_value;
 XrmValue xval;
 Screen *xscreen;
 Colormap cmap;

 name_list[1] = XrmStringToQuark("pcmap");
 class_list[1] = XrmStringToQuark("Pcmap");
 if (!XrmQGetResource(xresdb, name_list, class_list, &return_value, &xval))
  return DefaultColormap(WpeXInfo.display, WpeXInfo.screen);
 if (WpeStrccmp((char *)xval.addr, "on") != 0)
  return DefaultColormap(WpeXInfo.display, WpeXInfo.screen);
 xscreen = DefaultScreenOfDisplay(WpeXInfo.display);
 cmap = XCreateColormap(WpeXInfo.display, RootWindowOfScreen(xscreen),
   visual, AllocNone);
 XSetWindowColormap(WpeXInfo.display, WpeXInfo.window, cmap);
 x_palette_cmap = cmap;
 return cmap;
}

/**
 * x_palette_parse - Turn an entry's colour name into the colour it asks for.
 * @idx: palette index, 0..15
 * @name: the name the resources gave it
 * @cmap: the colormap the name is looked up for
 * @color: receives the colour
 *
 * A mistyped colour in the user's resources costs that one colour, not the
 * editor: it is said once on stderr and the built-in colour is used.  Only
 * when the built-in name is unknown too (a server without a colour
 * database) does the editor give up.
 */
static void x_palette_parse(int idx, const char *name, Colormap cmap,
                            XColor *color)
{
 if (XParseColor(WpeXInfo.display, cmap, name, color))
  return;
 fprintf(stderr, "xwpe: unknown colour \"%s\" for color%d, using \"%s\"\n",
         name, idx + 1, WpeXColorNames[idx]);
 if (XParseColor(WpeXInfo.display, cmap, WpeXColorNames[idx], color))
  return;
 fprintf(stderr, "xwpe: unable to find color \"%s\", exiting ...\n",
         WpeXColorNames[idx]);
 exit(1);
}

/**
 * x_palette_alloc_exact - Allocate the colour an entry asks for.
 * @cmap: the colormap
 * @color: the colour; on success it holds the pixel and the colour the
 *         server really gave
 *
 * Level 1: a free cell, or a read-only cell that already holds this colour
 * (black and white, and on many servers a small colour cube, are always
 * there), so it works on a full colormap for those colours too.
 *
 * Return: nonzero when the colour was allocated.
 */
static int x_palette_alloc_exact(Colormap cmap, XColor *color)
{
 return XAllocColor(WpeXInfo.display, cmap, color) != 0;
}

/**
 * x_palette_forget_cells - Drop the cells read from a colormap.
 *
 * When the editor moves to another colormap, whose cells are other ones.
 */
static void x_palette_forget_cells(void)
{
 free(x_palette_cells);
 free(x_palette_tried);
 x_palette_cells = NULL;
 x_palette_tried = NULL;
 x_palette_ncells = 0;
}

/**
 * x_palette_searchable - Whether a visual's cells can be searched by pixel.
 * @visual: the window's visual
 *
 * On a palette or grey display with writable cells a pixel is an index into
 * the colormap, so its cells can be read one by one and shared.  Fixed
 * displays never run out of cells, and on a DirectColor display a pixel is
 * three indexes, so neither is searched.
 */
static int x_palette_searchable(Visual *visual)
{
 if (visual->class != PseudoColor && visual->class != GrayScale)
  return 0;
 return visual->map_entries > 0 && visual->map_entries <= X_PALETTE_QUERY_MAX;
}

/**
 * x_palette_read_cells - Read every cell of a colormap, once.
 * @cmap: the colormap
 * @visual: the window's visual
 *
 * The first entry that finds the colormap full pays for one XQueryColors;
 * the entries after it search the same answer.
 *
 * Return: nonzero when the cells are at hand.
 */
static int x_palette_read_cells(Colormap cmap, Visual *visual)
{
 int i, n;

 if (x_palette_cells)
  return 1;
 if (!x_palette_searchable(visual))
  return 0;
 n = visual->map_entries;
 x_palette_cells = calloc((size_t)n, sizeof *x_palette_cells);
 x_palette_tried = calloc((size_t)n, 1);
 if (!x_palette_cells || !x_palette_tried)
 {
  x_palette_forget_cells();
  return 0;
 }
 for (i = 0; i < n; i++)
 {
  x_palette_cells[i].pixel = (unsigned long)i;
  x_palette_cells[i].flags = DoRed | DoGreen | DoBlue;
 }
 XQueryColors(WpeXInfo.display, cmap, x_palette_cells, n);
 x_palette_ncells = n;
 return 1;
}

/**
 * x_palette_channel - A 16-bit channel cut to 8 bits.
 * @c: the channel as the server gives it
 *
 * Return: 0..255.
 */
static int x_palette_channel(unsigned short c)
{
 return c >> X_PALETTE_CHANNEL_SHIFT;
}

/**
 * x_palette_same_rgb - Whether two colours are the same colour.
 * @a: one colour
 * @b: the other
 *
 * Return: nonzero when red, green and blue all match.
 */
static int x_palette_same_rgb(const XColor *a, const XColor *b)
{
 return a->red == b->red && a->green == b->green && a->blue == b->blue;
}

/**
 * x_palette_cell_taken_by_other - Whether a cell already shows another entry.
 * @pixel: the cell
 * @want: the colour the entry being allocated asks for
 *
 * Two entries that ask for different colours must never share one cell:
 * the text in one would vanish into the background of the other.  Two that
 * ask for the same colour may.
 *
 * Return: nonzero when an entry with a different colour has this cell.
 */
static int x_palette_cell_taken_by_other(unsigned long pixel, const XColor *want)
{
 int j;

 for (j = 0; j < x_palette_assigned; j++)
  if ((unsigned long)WpeXInfo.colors[j] == pixel
      && !x_palette_same_rgb(&x_palette_requested[j], want))
   return 1;
 return 0;
}

/**
 * x_palette_near_enough - Whether a cell is close enough to stand for a colour.
 * @cell: the cell's colour
 * @want: the colour asked for
 *
 * Return: nonzero when every channel is within X_PALETTE_NEAREST_MAX.
 */
static int x_palette_near_enough(const XColor *cell, const XColor *want)
{
 return abs(x_palette_channel(cell->red) - x_palette_channel(want->red))
          <= X_PALETTE_NEAREST_MAX
        && abs(x_palette_channel(cell->green) - x_palette_channel(want->green))
          <= X_PALETTE_NEAREST_MAX
        && abs(x_palette_channel(cell->blue) - x_palette_channel(want->blue))
          <= X_PALETTE_NEAREST_MAX;
}

/**
 * x_palette_distance - How far a cell's colour is from the one asked for.
 * @cell: the cell's colour
 * @want: the colour asked for
 *
 * Return: the squared distance over the three 8-bit channels.
 */
static long x_palette_distance(const XColor *cell, const XColor *want)
{
 long dr = x_palette_channel(cell->red) - x_palette_channel(want->red);
 long dg = x_palette_channel(cell->green) - x_palette_channel(want->green);
 long db = x_palette_channel(cell->blue) - x_palette_channel(want->blue);

 return dr * dr + dg * dg + db * db;
}

/**
 * x_palette_nearest_cell - The closest cell not tried yet.
 * @want: the colour asked for
 *
 * Return: the index into the cells read, or -1 when none is near enough.
 */
static int x_palette_nearest_cell(const XColor *want)
{
 long best_distance = -1, d;
 int best = -1, i;

 for (i = 0; i < x_palette_ncells; i++)
 {
  if (x_palette_tried[i] || !x_palette_near_enough(&x_palette_cells[i], want))
   continue;
  if (x_palette_cell_taken_by_other(x_palette_cells[i].pixel, want))
   continue;
  d = x_palette_distance(&x_palette_cells[i], want);
  if (best < 0 || d < best_distance)
  {
   best = i;
   best_distance = d;
  }
 }
 return best;
}

/**
 * x_palette_share_cell - Take a read-only share of an existing cell.
 * @cmap: the colormap
 * @cell: the cell's colour
 * @want: the colour the entry asked for
 * @color: receives the pixel and colour on success
 *
 * Asking the server for exactly the colour a cell holds gives a share of a
 * read-only cell with that colour.  A writable cell of another client
 * cannot be shared, and the server then has no cell to give.
 *
 * Return: nonzero when the entry got a cell.
 */
static int x_palette_share_cell(Colormap cmap, const XColor *cell,
                                const XColor *want, XColor *color)
{
 XColor got = *cell;
 unsigned long pixel;

 got.flags = DoRed | DoGreen | DoBlue;
 if (!XAllocColor(WpeXInfo.display, cmap, &got))
  return 0;
 if (x_palette_cell_taken_by_other(got.pixel, want))
 {
  pixel = got.pixel;
  XFreeColors(WpeXInfo.display, cmap, &pixel, 1, 0);
  return 0;
 }
 *color = got;
 return 1;
}

/**
 * x_palette_alloc_nearest - Take the nearest existing cell of a full colormap.
 * @cmap: the colormap
 * @visual: the window's visual
 * @want: the colour the entry asked for
 * @color: receives the pixel and colour on success
 *
 * Level 2, as xterm and rxvt do: when another program filled the colormap,
 * the editor shows a colour very close to its own instead of ending, and
 * stays in the screen's colormap, so no colormap flash.
 *
 * Return: nonzero when the entry got a cell.
 */
static int x_palette_alloc_nearest(Colormap cmap, Visual *visual,
                                   const XColor *want, XColor *color)
{
 int i;

 if (!x_palette_read_cells(cmap, visual))
  return 0;
 while ((i = x_palette_nearest_cell(want)) >= 0)
 {
  if (x_palette_share_cell(cmap, &x_palette_cells[i], want, color))
   return 1;
  x_palette_tried[i] = 1;
 }
 return 0;
}

/**
 * x_palette_go_private - Move the editor to a colormap of its own.
 * @cmap: the full colormap the editor was allocating in
 *
 * Level 3: the default colormap holds nothing close to a colour the editor
 * needs.  The cells the editor already has move with it, at the same
 * pixels, and the rest of the new colormap is free for the colours still to
 * come.  The user is told once, because the editor's colours then show only
 * while its window has the colormap installed, as with -pcmap.
 *
 * Return: the new colormap.
 */
static Colormap x_palette_go_private(Colormap cmap)
{
 Colormap private_cmap;

 private_cmap = XCopyColormapAndFree(WpeXInfo.display, cmap);
 XSetWindowColormap(WpeXInfo.display, WpeXInfo.window, private_cmap);
 x_palette_cmap = private_cmap;
 x_palette_forget_cells();
 fprintf(stderr,
         "xwpe: the default colormap is full, using a private one (as -pcmap)\n");
 return private_cmap;
}

/**
 * x_palette_alloc_black - The last way an entry gets a pixel.
 * @idx: palette index, 0..15
 * @color: receives the screen's black
 *
 * Reached only when a private colormap is full too.  The editor keeps
 * running with that entry shown as black and says which colour it lost.
 */
static void x_palette_alloc_black(int idx, XColor *color)
{
 fprintf(stderr, "xwpe: no colour cell for \"%s\", showing it as black\n",
         WpeXColorNames[idx]);
 color->pixel = BlackPixel(WpeXInfo.display, WpeXInfo.screen);
 color->red = color->green = color->blue = 0;
}

/**
 * x_palette_alloc - Give a palette entry a colour cell, whatever is free.
 * @idx: palette index, 0..15
 * @cmap: the colormap; changed when the editor moves to a private one
 * @visual: the window's visual
 * @color: the colour asked for; receives the pixel and the colour given
 *
 * Tries the levels in order: the exact colour, the nearest existing cell,
 * a private colormap and the exact colour there.  A full colormap never
 * ends the editor.
 *
 * Return: the X_PALETTE_LEVEL_* that gave the cell.
 */
static int x_palette_alloc(int idx, Colormap *cmap, Visual *visual,
                           XColor *color)
{
 XColor want = *color;

 if (x_palette_alloc_exact(*cmap, color))
  return X_PALETTE_LEVEL_EXACT;
 if (x_palette_alloc_nearest(*cmap, visual, &want, color))
  return X_PALETTE_LEVEL_NEAREST;
 if (x_palette_cmap == None)
 {
  *cmap = x_palette_go_private(*cmap);
  *color = want;
  if (x_palette_alloc_exact(*cmap, color))
   return X_PALETTE_LEVEL_PRIVATE;
 }
 *color = want;
 if (x_palette_alloc_nearest(*cmap, visual, &want, color))
  return X_PALETTE_LEVEL_NEAREST;
 x_palette_alloc_black(idx, color);
 return X_PALETTE_LEVEL_BLACK;
}

/**
 * x_palette_alloc_trace - Say in $XWPE_UI_TRACE how an entry got its cell.
 * @idx: palette index, 0..15
 * @level: the X_PALETTE_LEVEL_* that gave it
 *
 * Someone whose colours look slightly off reads here which entries took a
 * nearby colour and whether the editor moved to a private colormap.
 * Nothing is written when the variable is not set.
 */
static void x_palette_alloc_trace(int idx, int level)
{
 wpe_ui_trace("palette alloc idx=%d level=%d", idx, level);
}

/**
 * x_palette_alloc_all - Give all 16 entries their cells.
 * @names: the colour name of each entry
 * @cmap: the colormap to start in
 * @visual: the window's visual
 */
static void x_palette_alloc_all(char *names[X_PALETTE_SIZE], Colormap cmap,
                                Visual *visual)
{
 XColor color;
 int loop, level;

 x_palette_assigned = 0;
 for (loop = 0; loop < X_PALETTE_SIZE; loop++)
 {
  x_palette_parse(loop, names[loop], cmap, &color);
  x_palette_requested[loop] = color;
  level = x_palette_alloc(loop, &cmap, visual, &color);
  WpeXInfo.colors[loop] = color.pixel;
  x_palette_record(loop, &color);
  x_palette_assigned = loop + 1;
  x_palette_alloc_trace(loop, level);
 }
 x_palette_forget_cells();
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  WpeXColorGet - Get color settings from the X resources.

    Parameters:
      xresdb       (In)  X resource database
      name_list    (In)  Name quark list (second element unset)
      class_list   (In)  Class quark list (second element unset)
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
void WpeXColorGet(XrmDatabase xresdb, XrmQuark *name_list,
                  XrmQuark *class_list)
{
 char *names[X_PALETTE_SIZE];
 Colormap cmap;
 Visual *visual;
 int loop, depth;

 x_palette_resource_names(xresdb, name_list, class_list, names);
 depth = DisplayPlanes(WpeXInfo.display, WpeXInfo.screen);
 visual = DefaultVisual(WpeXInfo.display, WpeXInfo.screen);

 if (depth == 1)
 {
  /* Old code */
  /* Error message for depth of one removed */
  e_X_sw_color();
  WpeXInfo.colors[0] = BlackPixel(WpeXInfo.display, WpeXInfo.screen);
  WpeXInfo.colors[1] = WhitePixel(WpeXInfo.display, WpeXInfo.screen);
  for (loop = 0; loop < X_PALETTE_SIZE; loop++)
   x_palette_record_mono(loop, WpeXInfo.colors[loop]);
  x_render_route_choose(visual, depth);
  return ;
 }
 cmap = x_palette_choose_colormap(xresdb, name_list, class_list, visual);
 x_palette_alloc_all(names, cmap, visual);
 x_render_route_choose(visual, depth);
 return ;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  WpeXGeometryGet - Get geometry setting from the X resources.

    Parameters:
      xresdb       (In)  X resource database
      name_list    (In)  Name quark list (second element unset)
      class_list   (In)  Class quark list (second element unset)
      size_hints   (Out) Initial geometry of window
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
void WpeXGeometryGet(XrmDatabase xresdb, XrmQuark *name_list,
                     XrmQuark *class_list, XSizeHints *size_hints)
{
 XrmRepresentation return_value;
 XrmValue xval;
 char geom_str[20];
 int grav;

 size_hints->flags = PResizeInc | PMinSize | PBaseSize;
 size_hints->height_inc = WpeXInfo.font_height;
 size_hints->width_inc = WpeXInfo.font_width;
 size_hints->min_width = size_hints->width_inc * 80;
 size_hints->min_height = size_hints->height_inc * 24;
 size_hints->base_width = size_hints->base_height = 0;
 name_list[1] = XrmStringToQuark("geometry");
 class_list[1] = XrmStringToQuark("Geometry");
 if (!XrmQGetResource(xresdb, name_list, class_list, &return_value, &xval))
 {
  xval.addr = NULL;
 }
 size_hints->x = size_hints->y = 0;
 /* Default to 80 columns and a number of lines to fill 3/4th of the screen */
 snprintf(geom_str, sizeof(geom_str), "80x%d", (3 *
         DisplayHeight(WpeXInfo.display, WpeXInfo.screen) / 4) /
         WpeXInfo.font_height);
 /* This doesn't correctly account for the title bar.  If someone could
   point me in the right direction for correcting this I'd be grateful. */
 if (XWMGeometry(WpeXInfo.display, WpeXInfo.screen, (char *)xval.addr,
                 geom_str, 4, size_hints, &size_hints->x, &size_hints->y,
                 &size_hints->width, &size_hints->height, &grav) &
     (XValue | YValue))
 {
  size_hints->flags |= PPosition;
 }
 /* Old variables used since not converted yet */
 MAXSCOL = size_hints->width / WpeXInfo.font_width;
 MAXSLNS = size_hints->height / WpeXInfo.font_height;
 return ;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  WpeXOptionsGet - Get miscellaneous options from the X resources.

    Parameters:
      xresdb       (In)  X resource database
      name_list    (In)  Name quark list (second element unset)
      class_list   (In)  Class quark list (second element unset)
      iconic       (Out) Initial mode to start program in
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
void WpeXOptionsGet(XrmDatabase xresdb, XrmQuark *name_list,
                    XrmQuark *class_list, int *iconic)
{
 XrmRepresentation return_value;
 XrmValue xval;

 WpeXInfo.altmask = DEFAULT_ALTMASK;
 name_list[1] = XrmStringToQuark("altMask");
 class_list[1] = XrmStringToQuark("AltMask");
 if (XrmQGetResource(xresdb, name_list, class_list, &return_value, &xval))
 {
  if (WpeStrnccmp((char *)xval.addr, "mod", 3) == 0)
  {
   /* Accept both "modN" (X11 convention) and "mod N" (a legacy spelling the
      historical parser at xval.addr[4] silently required): scan past the
      "mod" prefix and any whitespace to find the digit. */
   const char *p = (const char *)xval.addr + 3;
   while (*p == ' ' || *p == '\t') p++;
   switch (*p - '0')
   {
    case 1:
     WpeXInfo.altmask = Mod1Mask;
     break;
    case 2:
     WpeXInfo.altmask = Mod2Mask;
     break;
    case 3:
     WpeXInfo.altmask = Mod3Mask;
     break;
    case 4:
     WpeXInfo.altmask = Mod4Mask;
     break;
    case 5:
     WpeXInfo.altmask = Mod5Mask;
     break;
    default:
     break;
   }
  }
 }
 *iconic = NormalState;
 name_list[1] = XrmStringToQuark("iconic");
 class_list[1] = XrmStringToQuark("Iconic");
 if (XrmQGetResource(xresdb, name_list, class_list, &return_value, &xval))
 {
  if (WpeStrccmp((char *)xval.addr, "on") == 0)
  {
   *iconic = IconicState;
  }
 }
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  WpeXDefaults - Gets the X default resources.

    Returns: X resource database
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
XrmDatabase WpeXDefaults()
{
 char *s, *home_env;

 s = XResourceManagerString(WpeXInfo.display);
 if (s)
 {
  return XrmGetStringDatabase(s);
 }
 /* The password file should be checked if HOME isn't set but not done yet. */
 home_env = getenv("HOME");
 if (home_env)
 {
  XrmDatabase tmpdb;

  s = malloc(strlen(home_env) + 12);
  snprintf(s, strlen(home_env) + 12, "%s/.Xdefaults", home_env);
  tmpdb = XrmGetFileDatabase(s);
  free(s);
  return (tmpdb);
 }
 return NULL;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  WpeXGCSetup - Sets up the graphic context with appropriate font, line, etc.
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
void WpeXGCSetup()
{
 XGCValues values;
 static char dash_list[2] = {  12,  24  };
   
 WpeXInfo.gc = XCreateGC(WpeXInfo.display, WpeXInfo.window, 0, &values);
 if (WpeXInfo.font)
  XSetFont(WpeXInfo.display, WpeXInfo.gc, WpeXInfo.font->fid);
 XSetForeground(WpeXInfo.display, WpeXInfo.gc,
   BlackPixel(WpeXInfo.display, WpeXInfo.screen));
 XSetLineAttributes(WpeXInfo.display, WpeXInfo.gc, /*line width*/ 1,
   /*line style*/ LineSolid, /*cap style*/ CapRound,
   /*join style*/ JoinRound);
 XSetDashes(WpeXInfo.display, WpeXInfo.gc, /*dash offset*/ 0, dash_list,
   /*dash list length*/ 2);
 return ;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  WpeXInit - Initializes the X display.

    Parameters:
      argc         (In & Out) Argument count
      argv         (In & Out) Argument values
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
void WpeXInit(int *argc, char **argv)
{
 XrmDatabase xres = 0;
 XrmQuark class_list[3];
 XrmQuark name_list[3];
 XrmRepresentation return_value;
 XrmValue xval;
 XWMHints wm_hints;
 XClassHint class_hint;
 XSizeHints size_hints;
 Atom *atom_list, *new_atom_list;
 int atom_num, cursor_num;
 char *window_name;
 char *display_name;

 XrmInitialize();
 if (WpeIsProg())
 {
  window_name = "Window Programming Environment";
  class_hint.res_name = "xwpe";
 }
 else
 {
  window_name = "Window Editor";
  class_hint.res_name = "xwe";
 }
 class_hint.res_class = "Xwpe";
 XrmParseCommand(&xres, WpeXOptionTable, OPTION_TABLE_SIZE,
   class_hint.res_name, argc, argv);
 class_list[0] = XrmStringToQuark(class_hint.res_class);
 name_list[0] = XrmStringToQuark(class_hint.res_name);
 /* The second class quark is set to "display" since it doesn't matter */
 name_list[1] = class_list[1] = XrmStringToQuark("display");
 name_list[2] = class_list[2] = NULLQUARK;
 if (XrmQGetResource(xres, name_list, class_list, &return_value, &xval))
 {
  display_name = (char *)xval.addr;
 }
 else
 {
  display_name = NULL;
 }
 /* Cairo+Pango (and fontconfig) spawn worker threads.  Xlib is not
    thread-safe unless XInitThreads() is called BEFORE any other Xlib
    function: without it the X request/event buffers corrupt intermittently,
    which shows up as either a crash deep in libX11 (_XEventsQueued during a
    cairo_fill) or a hung, all-black window.  Must precede XOpenDisplay. */
 XInitThreads();
 if ((WpeXInfo.display = XOpenDisplay(display_name)) == NULL)
 {
  fprintf(stderr, "Xwpe: unable to open display \"%s\", exiting ...\n",
    XDisplayName(display_name));
  exit(-1);
 }
 WpeXInfo.screen = DefaultScreen(WpeXInfo.display);
 XrmCombineDatabase(WpeXDefaults(), &xres, False);
 WpeXFontGet(xres, name_list, class_list);
 WpeXGeometryGet(xres, name_list, class_list, &size_hints);
 WpeXOptionsGet(xres, name_list, class_list, &wm_hints.initial_state);

 WpeXInfo.window = XCreateSimpleWindow(WpeXInfo.display,
   RootWindow(WpeXInfo.display, WpeXInfo.screen), size_hints.x, size_hints.y,
   size_hints.width, size_hints.height, 4,
   BlackPixel(WpeXInfo.display, WpeXInfo.screen),
   BlackPixel(WpeXInfo.display, WpeXInfo.screen));
 { XSetWindowAttributes wa;
   wa.bit_gravity = StaticGravity;
   XChangeWindowAttributes(WpeXInfo.display, WpeXInfo.window,
     CWBitGravity, &wa);
 }

 WpeXColorGet(xres, name_list, class_list);
 XrmDestroyDatabase(xres);

 wm_hints.flags = InputHint | StateHint | WindowGroupHint; /*  make it serve the window */
 wm_hints.input = True;                  /*  right from start */
 wm_hints.window_group = WpeXInfo.window;
 
 XmbSetWMProperties(WpeXInfo.display, WpeXInfo.window, window_name,
   class_hint.res_name, argv, *argc, &size_hints, &wm_hints, &class_hint);
 /* XmbSetWMProperties silently drops WM_NAME/WM_ICON_NAME when the libX11
    locale module is incomplete (observed with the Homebrew libX11 on macOS,
    where xprop shows everything except WM_NAME).  Always set the legacy
    STRING properties and the EWMH UTF8_STRING properties directly so the
    title is discoverable by xdotool/wmctrl and modern window managers. */
 XStoreName(WpeXInfo.display, WpeXInfo.window, window_name);
 XSetIconName(WpeXInfo.display, WpeXInfo.window, window_name);
 {
  Atom net_wm_name      = XInternAtom(WpeXInfo.display, "_NET_WM_NAME", False);
  Atom net_wm_icon_name = XInternAtom(WpeXInfo.display, "_NET_WM_ICON_NAME", False);
  Atom utf8             = XInternAtom(WpeXInfo.display, "UTF8_STRING", False);
  XChangeProperty(WpeXInfo.display, WpeXInfo.window, net_wm_name, utf8, 8,
    PropModeReplace, (unsigned char *)window_name, strlen(window_name));
  XChangeProperty(WpeXInfo.display, WpeXInfo.window, net_wm_icon_name, utf8, 8,
    PropModeReplace, (unsigned char *)window_name, strlen(window_name));
 }
 XSelectInput(WpeXInfo.display, WpeXInfo.window, ExposureMask |
   KeyPressMask | ButtonPressMask | ButtonReleaseMask |
   Button1MotionMask | StructureNotifyMask);

 if(!XGetWMProtocols(WpeXInfo.display, WpeXInfo.window, &atom_list, &atom_num))
 {
  atom_num = 0;
 }
 new_atom_list = WpeMalloc((atom_num + 1) * sizeof(Atom));
 if (atom_list != NULL)
 {
  memcpy(new_atom_list, atom_list, atom_num * sizeof(Atom));
 }
 if (atom_num) XFree(atom_list);
 new_atom_list[atom_num] = WpeXInfo.delete_atom =
   XInternAtom(WpeXInfo.display, "WM_DELETE_WINDOW", False);
 WpeXInfo.protocol_atom = XInternAtom(WpeXInfo.display, "WM_PROTOCOLS", False);
 XSetWMProtocols(WpeXInfo.display, WpeXInfo.window, new_atom_list,
   atom_num + 1);
 WpeFree(new_atom_list);

 WpeXInfo.selection_atom = XInternAtom(WpeXInfo.display, "PRIMARY", False);
 WpeXInfo.clipboard_atom = XInternAtom(WpeXInfo.display, "CLIPBOARD", False);
 WpeXInfo.text_atom = XInternAtom(WpeXInfo.display, "STRING", False);
 WpeXInfo.utf8_atom = XInternAtom(WpeXInfo.display, "UTF8_STRING", False);
 WpeXInfo.targets_atom = XInternAtom(WpeXInfo.display, "TARGETS", False);
 WpeXInfo.property_atom = XInternAtom(WpeXInfo.display, "XWPE_SELECTION", False);
 WpeXInfo.selection = NULL;
 WpeXGCSetup();

#ifdef HAVE_XFT
 /* Initialize XftColor array from the allocated X11 colors */
 if (WpeXInfo.xftfont)
 {
  Colormap cmap = e_x_palette_colormap();
  int ci;
  for (ci = 0; ci < X_PALETTE_SIZE; ci++)
  {
   unsigned short wide[3];
   e_x_palette_rgb16(ci, wide);
   {
    XRenderColor rc;
    rc.red = wide[0];
    rc.green = wide[1];
    rc.blue = wide[2];
    rc.alpha = 0xffff;
    XftColorAllocValue(WpeXInfo.display,
      DefaultVisual(WpeXInfo.display, WpeXInfo.screen),
      cmap, &rc, &WpeXInfo.xftcolors[ci]);
   }
  }
  /* LSP semantic-token truecolor slots occupy xftcolors[16 + slot]: a
     foreground index >= 16 from the renderer selects the exact 24-bit colour
     (e.g. method -> orange) that the 16-colour palette cannot express. */
  for (ci = 0; ci < LSP_SEM_TC_MAX; ci++)
  {
   int r, g, b;
   XRenderColor rc;
   if (!e_lsp_sem_slot_rgb(ci, &r, &g, &b))
    break;
   rc.red   = r * 257;     /* 0..255 -> 0..65535 */
   rc.green = g * 257;
   rc.blue  = b * 257;
   rc.alpha = 0xffff;
   XftColorAllocValue(WpeXInfo.display,
     DefaultVisual(WpeXInfo.display, WpeXInfo.screen),
     cmap, &rc, &WpeXInfo.xftcolors[16 + ci]);
  }
 }
#endif

 {
#include "icons/xwpe_icon_data.h"
  Atom net_wm_icon = XInternAtom(WpeXInfo.display, "_NET_WM_ICON", False);
  XChangeProperty(WpeXInfo.display, WpeXInfo.window, net_wm_icon,
    XA_CARDINAL, 32, PropModeReplace,
    (unsigned char *)xwpe_icon_data,
    sizeof(xwpe_icon_data) / sizeof(xwpe_icon_data[0]));
 }

 XMapWindow(WpeXInfo.display, WpeXInfo.window);

#ifdef HAVE_XFT
 /* Create back buffer Pixmap and XftDraw (after XMapWindow so window exists) */
 if (WpeXInfo.xftfont)
 {
  /* Size the initial cell grid from the MAPPED window rather than the requested
     size_hints.  Under libx11-compat on a HiDPI backing the top-level window is
     promoted to physical pixels at map time, so the real window is larger than
     what was requested (e.g. 3040x1728 vs 1520x864).  xwpe normally corrects
     MAXSCOL/MAXSLNS from that size only when it processes the post-map
     ConfigureNotify in its event loop -- but main() draws the initial File
     Manager before the event loop runs, so the first dialog would be laid out
     with the stale requested grid (MAXSCOL=80) and be the wrong size.  Reading
     the actual geometry here yields the same grid the ConfigureNotify handler
     would compute (MAXSCOL=160), so the first dialog matches later ones and the
     back buffer below is allocated at the correct size.  On a non-HiDPI host
     XGetWindowAttributes returns the requested size, leaving this a no-op. */
  {
   XWindowAttributes wattr;
   if (XGetWindowAttributes(WpeXInfo.display, WpeXInfo.window, &wattr) &&
       wattr.width > 0 && wattr.height > 0)
   {
    MAXSCOL = wattr.width  / WpeXInfo.font_width;
    MAXSLNS = wattr.height / WpeXInfo.font_height;
    size_hints.width  = MAXSCOL * WpeXInfo.font_width;
    size_hints.height = MAXSLNS * WpeXInfo.font_height;
   }
  }
  WpeXInfo.backbuf = XCreatePixmap(WpeXInfo.display, WpeXInfo.window,
    size_hints.width, size_hints.height,
    DefaultDepth(WpeXInfo.display, WpeXInfo.screen));
  WpeXInfo.xftdraw = e_x_xftdraw_create();
  /* Clear the back buffer to black */
  XSetForeground(WpeXInfo.display, WpeXInfo.gc,
    BlackPixel(WpeXInfo.display, WpeXInfo.screen));
  XFillRectangle(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.gc,
    0, 0, size_hints.width, size_hints.height);
#ifdef HAVE_CAIRO
  wpe_render_cairo_init();
  /* cr_init_pango_font() may have overwritten WpeXInfo.font_width/height
     with Pango's actual rendered metrics, which can differ from the seed
     core/Xft values used by WpeXGeometryGet to compute MAXSCOL/MAXSLNS.
     Refit the cell grid to the existing window so the first frame -- and
     in particular any dialog laid out before the first ConfigureNotify --
     uses cell counts that match the chrome we are about to draw. */
  {
   XWindowAttributes wattr;
   int fit_w = size_hints.width, fit_h = size_hints.height;
   int old_w = size_hints.width, old_h = size_hints.height;
   if (XGetWindowAttributes(WpeXInfo.display, WpeXInfo.window, &wattr) &&
       wattr.width > 0 && wattr.height > 0)
   {
    fit_w = wattr.width;
    fit_h = wattr.height;
   }
   MAXSCOL = fit_w / WpeXInfo.font_width;
   MAXSLNS = fit_h / WpeXInfo.font_height;
   size_hints.width  = MAXSCOL * WpeXInfo.font_width;
   size_hints.height = MAXSLNS * WpeXInfo.font_height;
   /* wpe_render_cairo_init() sized the back buffer and cairo surface to the
      pre-refit grid.  When the refit enlarged it (HiDPI physical promotion),
      grow the render backing to match now, mirroring the ConfigureNotify
      handler, so the first dialog is not clipped to the stale surface. */
   if ((size_hints.width != old_w || size_hints.height != old_h) &&
       WpeRender.resize)
   {
    if (WpeXInfo.xftdraw)
     XftDrawDestroy(WpeXInfo.xftdraw);
    WpeRender.resize(size_hints.width, size_hints.height);
    WpeXInfo.xftdraw = e_x_xftdraw_create();
   }
  }
#endif
 }
#endif

 for (cursor_num = WpeEditingShape; cursor_num < WpeLastShape; cursor_num++)
 {
  WpeXMouseCursor[cursor_num] =
    XCreateFontCursor(WpeXInfo.display, WpeXMouseDefault[cursor_num]);
 }
 XDefineCursor(WpeXInfo.display, WpeXInfo.window,
   WpeXMouseCursor[WpeEditingShape]);
 WpeXInfo.shape_list[0] = WpeXInfo.shape_list[1] = WpeEditingShape;

 /* Copied with little change since I haven't had the time to look into
   what it does */
 if((*e_u_ini_size)())
 {
  *argc = -1;
  return ;
 }

 e_abs_refr();
 /* end of untouched section */

 /* Opt in to libx11-compat's live-resize reflow: during a macOS modal resize
    drag it invokes our hook on each tick so the editor reflows and paints real
    content into the grown area instead of a guessed fill.  Weak-linked, so this
    is a no-op under a real libX11.  Registered here, once, after the window,
    font metrics, back buffer and initial grid are all set up. */
 { extern void e_x_register_live_resize(void); e_x_register_live_resize(); }

 return;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  WpeXMouseChangeShape - Changes the mouse pointer to a different shape.

    Parameters:
      new_shape    (In) new shape of the mouse pointer
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
void WpeXMouseChangeShape(WpeMouseShape new_shape)
{
 WpeXInfo.shape_list[1] = WpeXInfo.shape_list[0];
 WpeXInfo.shape_list[0] = new_shape;
 XDefineCursor(WpeXInfo.display, WpeXInfo.window, WpeXMouseCursor[new_shape]);
 XFlush(WpeXInfo.display);
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  WpeXMouseRestoreShape - Restores the mouse pointer to the previous shape.
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
void WpeXMouseRestoreShape()
{
 WpeXInfo.shape_list[0] = WpeXInfo.shape_list[1];
 WpeXInfo.shape_list[1] = WpeEditingShape;
 XDefineCursor(WpeXInfo.display, WpeXInfo.window,
   WpeXMouseCursor[WpeXInfo.shape_list[0]]);
}

#endif

