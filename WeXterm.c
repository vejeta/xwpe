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

 name_list[1] = XrmStringToQuark("font");
 class_list[1] = XrmStringToQuark("Font");
 if (XrmQGetResource(xresdb, name_list, class_list, &return_value, &xval))
 {
  font_name = (char *)xval.addr;
 }
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
 XrmRepresentation return_value;
 XrmValue xval;
 Colormap cmap;
 XColor exact_def;
 Visual *visual;
 int loop, depth;
 char color_name[8];
 char color_class[8];
 Screen *xscreen;

 memset(color_name, 0, 8);
 memset(color_class, 0, 8);
 strcpy(color_name, "color");
 strcpy(color_class, "Color");
 for (loop = 0; loop < 16; loop++)
 {
  /* color names start at 1 not 0 */
  color_name[5] = color_class[5] = (loop < 9)? loop + '1': '1';
  if (loop > 8)
  {
   color_name[6] = color_class[6] = loop - 9 + '0';
  }
  name_list[1] = XrmStringToQuark(color_name);
  class_list[1] = XrmStringToQuark(color_name);
  if (XrmQGetResource(xresdb, name_list, class_list, &return_value, &xval))
  {
   WpeXColorNames[loop] = (char *)xval.addr;
  }
 }

 cmap = DefaultColormap(WpeXInfo.display, WpeXInfo.screen);
 depth = DisplayPlanes(WpeXInfo.display, WpeXInfo.screen);
 visual = DefaultVisual(WpeXInfo.display, WpeXInfo.screen);

 if (depth == 1)
 {
  /* Old code */
  /* Error message for depth of one removed */
  e_X_sw_color();
  WpeXInfo.colors[0] = BlackPixel(WpeXInfo.display, WpeXInfo.screen);
  WpeXInfo.colors[1] = WhitePixel(WpeXInfo.display, WpeXInfo.screen);
  return ;
 }
 /* Check for private colormap option */
 name_list[1] = XrmStringToQuark("pcmap");
 class_list[1] = XrmStringToQuark("Pcmap");
 if (XrmQGetResource(xresdb, name_list, class_list, &return_value, &xval))
 {
  if (WpeStrccmp((char *)xval.addr, "on") == 0)
  {
   xscreen = DefaultScreenOfDisplay(WpeXInfo.display);
   cmap = XCreateColormap(WpeXInfo.display,
     RootWindowOfScreen(xscreen), visual, AllocNone);
   XSetWindowColormap(WpeXInfo.display, WpeXInfo.window, cmap);
  }
 }
 for(loop = 0; loop < 16; loop++)
 {
  if (!XParseColor(WpeXInfo.display, cmap, WpeXColorNames[loop], &exact_def))
  {
   /* Should try a default color then bail if need be */
   fprintf(stderr, "xwpe: unable to find color \"%s\", exiting ...\n",
           WpeXColorNames[loop]);
   exit(1);
  }
  if (!XAllocColor(WpeXInfo.display, cmap, &exact_def))
  {
   /* Should try to find closest color */
   fprintf(stderr, "Xwpe: all colorcells allocated, exiting ...\n");
   exit(1);
  }
  WpeXInfo.colors[loop] = exact_def.pixel;
 }
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
  Colormap cmap = DefaultColormap(WpeXInfo.display, WpeXInfo.screen);
  int ci;
  for (ci = 0; ci < 16; ci++)
  {
   XColor xc;
   xc.pixel = WpeXInfo.colors[ci];
   XQueryColor(WpeXInfo.display, cmap, &xc);
   {
    XRenderColor rc;
    rc.red = xc.red;
    rc.green = xc.green;
    rc.blue = xc.blue;
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
  WpeXInfo.xftdraw = XftDrawCreate(WpeXInfo.display, WpeXInfo.backbuf,
    DefaultVisual(WpeXInfo.display, WpeXInfo.screen),
    DefaultColormap(WpeXInfo.display, WpeXInfo.screen));
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
    WpeXInfo.xftdraw = XftDrawCreate(WpeXInfo.display, WpeXInfo.backbuf,
      DefaultVisual(WpeXInfo.display, WpeXInfo.screen),
      DefaultColormap(WpeXInfo.display, WpeXInfo.screen));
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

