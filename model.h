#ifndef __MODEL_H
#define __MODEL_H
/* model.h						  */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

/*
   General Model Definitions      */

/*  System Definition   */

#ifndef UNIX
#define UNIX
#endif
#undef DJGPP

/*  Effects of #Defines (do not change)  */

#define CHECKHEADER

#ifdef DJGPP
#define NODEBUGGER
#undef NOPROG
#define NO_XWINDOWS
#define NOPRINTER
#define NONEWSTYLE
#define MOUSE 1
#ifndef UNIX
#define UNIX
#endif
#endif

/*  XWindow Definitions  */

/* Mouse is available through any of: X11 (Xlib events), GPM (Linux console),
   or ncurses' own xterm mouse reporting.  The last is what gives a terminal-only
   build (--without-x --without-gpm, e.g. macOS in iTerm2/Terminal.app) a working
   mouse -- without it MOUSE was 0 there and `struct mouse` went undefined. */
#if defined(DJGPP) || !defined(NO_XWINDOWS) || defined(HAVE_LIBGPM) || defined(NCURSES)
#define MOUSE   1        /*  activate mouse  */
#else
#define MOUSE   0        /*  deactivate mouse  */
#endif

/*  Programming Environment  */

#ifndef NOPROG
#define PROG
#ifndef NODEBUGGER
#define DEBUGGER
#endif
#endif

/*  Newstyle only for XWindow   */
#if !defined(NO_XWINDOWS) && !defined(NONEWSTYLE)
#define NEWSTYLE
#endif

#endif
