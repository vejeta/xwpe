/* unixkeys.h						  */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#ifndef LIBRARY_DIR
#define LIBRARY_DIR "/usr/local/lib/xwpe"
#endif

#ifndef INFO_DIR
#define INFO_DIR "/usr/share/info:/usr/local/info:/usr/info"
#endif

#define XWPE_HOME   ".xwpe"
#define WASTEBASKET ".wastebasket"

#define SYNTAX_FILE "syntax_def"
#define HELP_FILE   "help.xwpe"
#define OPTION_FILE "xwperc"

#define BUFFER_NAME "Buffer"   /*  Clipboard in Buffer changed  */
#define SUDIR "*"
#define MAXREC 15
#ifndef DEF_SHELL
#define DEF_SHELL "sh"
#endif

/*   Characters  */

extern char MCI, MCA, RD1, RD2, RD3, RD4, RD5, RD6;
extern char RE1, RE2, RE3, RE4, RE5, RE6, WBT;

#define MCU '^'
#define MCD 'v'
#define MCL '<'
#define MCR '>'
#define WZY 'z'
#define WZN 'Z'
#define PWR '$'
#define PNL '\\'
#define SCR 20
#define SCD 16
#define WSW 'v'
#define DIRC '/'
#define DIRS "/"
#define SWSYM '+'
#define PTHD ':'
/*  Special colors  */
#define SHDCOL 8

#define STOP  421
#define AGAIN 422
#define PROPS 423
#define UNDO  424
#define FRONT 425
#define COPY  426
#define OPEN  427
#define PASTE 428
#define FID   429
#define CUT   430
#define HELP  431

#define DWR 20       /*  ctrl t  */
#define DWL 18       /*  ctrl r  */
#define DNDL 17      /*  ctrl q  */
#define DGZ 25       /*  ctrl y  */

#define WPE_CR 13
#define WPE_WR 10
#define WPE_ESC 27
#define WPE_DC 8
#define WPE_TAB 9
/* WPE_BTAB (Shift-Tab) is in keys.h, above the control bytes. */

/* The terminal's private modes the editor sets while it owns the keyboard,
   written to the terminal and never read.  xterm's modifyOtherKeys level 2
   and kitty's "disambiguate escape codes" plus "report alternate keys"
   (flags 1 and 4) make Ctrl, Alt and Shift chords arrive as unambiguous
   reports; the pop and reset give the shell its own mode back.  The off
   string is one constant so a signal handler can write it. */
#define E_T_KEYS_KITTY_PUSH   "\033[>5u"
#define E_T_KEYS_KITTY_POP    "\033[<u"
#define E_T_KEYS_KITTY_QUERY  "\033[?u"
#define E_T_KEYS_MOK_ON       "\033[>4;2m"
#define E_T_KEYS_MOK_OFF      "\033[>4m"
#define E_T_KEYS_MOK_QUERY    "\033[?4m"
#define E_T_KEYS_ON_STR   E_T_KEYS_MOK_ON E_T_KEYS_KITTY_PUSH
#define E_T_KEYS_OFF_STR  E_T_KEYS_KITTY_POP E_T_KEYS_MOK_OFF
#define E_T_MOUSE_PASTE_ON_STR  "\033[?1000h\033[?1002h\033[?1006h\033[?2004h"
#define E_T_MOUSE_PASTE_OFF_STR "\033[?2004l\033[?1002l\033[?1006l\033[?1000l"
#define E_T_PRIVATE_MODES_OFF_STR E_T_KEYS_OFF_STR E_T_MOUSE_PASTE_OFF_STR
/* OSC 22 with an empty name gives the terminal's own mouse pointer back.  It
   is written when the mouse tracking ends, and by the release string a
   signal handler writes. */
#define E_T_POINTER_SHAPE_RESET_STR "\033]22;\a"

#define SCLE CLE+512
#define SCRI CRI+512
#define SCUP CUP+512
#define SCDO CDO+512

