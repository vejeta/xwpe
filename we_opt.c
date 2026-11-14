/* we_opt.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include "WeExpArr.h"
#include "we_ai.h"        /* AI backend config keys (guarded by WPE_AI) */
#include "we_lineend.h"
#include "we_testdump.h"
#include "we_startcmd.h"
#include "we_startplan.h"
#include "we_paint.h"
#include "we_layout.h"
#include <ctype.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

int WpeReadGeneral(ECNT *cn, char *section, char *option, char *value);
int WpeWriteGeneral(ECNT *cn, char *section, FILE *opt_file);
int WpeReadColor(ECNT *cn, char *section, char *option, char *value);
int WpeWriteColor(ECNT *cn, char *section, FILE *opt_file);
int WpeReadProgramming(ECNT *cn, char *section, char *option, char *value);
int WpeWriteProgramming(ECNT *cn, char *section, FILE *opt_file);
int WpeReadLanguage(ECNT *cn, char *section, char *option, char *value);
int WpeWriteLanguage(ECNT *cn, char *section, FILE *opt_file);

#define E_HLP_NUM 26
char *e_hlp_str[E_HLP_NUM];

extern char *info_file;
#ifdef DEBUGGER
extern int e_deb_type;
extern int e_dap_adapter;
#endif
extern FARBE *u_fb, *x_fb;

#define OPTION_SECTIONS 4
#define OPT_SECTION_GENERAL     "General"
#define OPT_SECTION_COLOR       "Color"
#define OPT_SECTION_PROGRAMMING "Programming"
#define OPT_SECTION_LANGUAGE    "Language"

WpeOptionSection WpeSectionRead[] = {
 {OPT_SECTION_GENERAL, WpeReadGeneral},
 {OPT_SECTION_COLOR, WpeReadColor},
 {OPT_SECTION_PROGRAMMING, WpeReadProgramming},
 {OPT_SECTION_LANGUAGE, WpeReadLanguage}
};

/* How e_opt_read_file takes the keys of one file. */
#define E_OPT_READ_ALL       0   /* every key: the user's own file, or one named by -sf */
#define E_OPT_READ_SHARED    1   /* a start-folder xwperc: every key but the user-only ones */
#define E_OPT_READ_USER_ONLY 2   /* the user's own file, for the user-only keys only        */

/* One key a start-folder xwperc may not set. */
typedef struct {
 const char *section;
 const char *key;
} e_opt_user_only;

/* The keys that start a program, choose a command line, name a file or a
   folder the assistant reads, choose where the assistant's requests go and
   which certificates it trusts, or set the permission dial and the web
   switch.  A project folder anyone can clone (or the assistant itself can
   write) must not choose them, so they are honoured only from the user's
   own config file and from the environment.  Keys added later with the same
   reach belong here too. */
static const e_opt_user_only e_opt_user_only_keys[] = {
 {OPT_SECTION_PROGRAMMING, "AIBackend"},
 {OPT_SECTION_PROGRAMMING, "AIEndpoint"},
 {OPT_SECTION_PROGRAMMING, "AIProvider"},
 {OPT_SECTION_PROGRAMMING, "AIProviderName"},
 {OPT_SECTION_PROGRAMMING, "AICAFile"},
 {OPT_SECTION_PROGRAMMING, "AISearchURL"},
 {OPT_SECTION_PROGRAMMING, "AIEditHook"},
 {OPT_SECTION_PROGRAMMING, "AIHostCommand"},
 {OPT_SECTION_PROGRAMMING, "AIHostAdapter"},
 {OPT_SECTION_PROGRAMMING, "AIAgentEngine"},
 {OPT_SECTION_PROGRAMMING, "AIReadRoots"},
 {OPT_SECTION_PROGRAMMING, "AIPolicy"},
 {OPT_SECTION_PROGRAMMING, "AIWeb"},
 {NULL, NULL}
};

/* Room for the names of the keys a start-folder xwperc tried to set. */
#define E_OPT_IGNORED_MAX 400

/* The user-only keys the start-folder xwperc named, for the status line. */
static char e_opt_ignored_keys[E_OPT_IGNORED_MAX];

/* The user's own config file, when the options come from a start-folder
   xwperc; NULL when they come from the user's own file already. */
static char *e_opt_user_file;

/* What the status line says when a start-folder xwperc named user-only keys. */
#define E_OPT_IGNORED_FMT "./xwperc may not set (only your own config may): %s"
#define E_OPT_IGNORED_LINE_MAX (E_OPT_IGNORED_MAX + 80)

/* The room of the print command's field in Options > Editor. */
#define E_OPT_PRINT_FIELD_ROOM 128

/* The start-folder xwperc the options come from, and the folder that holds
   it; NULL when the options come from the user's own file. */
static char *e_opt_folder_file;
static char *e_opt_folder_dir;

/* The program keys as the user's own config, or the built-in default, sets
   them, kept while a start-folder xwperc sets the ones in force: "Use mine"
   runs these. */
static char *e_opt_own_print;
static int e_opt_own_deb = E_OPT_DEBUGGER_UNSET;
static struct e_s_prog **e_opt_own_lang;
static int e_opt_own_nlang;

/* The debugger the start-folder xwperc names, or E_OPT_DEBUGGER_UNSET. */
static int e_opt_named_deb = E_OPT_DEBUGGER_UNSET;

/* The [Programming] keys a start plan compares with the user's own: where
   the build writes, what Run and Make are handed, which project is built,
   where headers are looked for.  His own value, or the built-in one, is
   kept for each while a start-folder xwperc sets the one in force. */
static const char *const e_opt_own_prog_keys[] = {
 "Exedir", "Arguments", "Project", "IncludePath", NULL
};
#define E_OPT_OWN_PROG_KEYS 4
static char *e_opt_own_prog[E_OPT_OWN_PROG_KEYS];

/* The answers' key of one value the user counts as his: "own/<scope>/<key>",
   the scope being a language or "Programming". */
#define E_OPT_OWN_FIELD_FMT "own/%s/%s"

/* What the status line says when the user's own config could not take the
   keys a save moved out of the start-folder xwperc. */
#define E_OPT_MOVE_FAILED_FMT "could not write your own config %s: its assistant settings were not saved"

/* The About box: its frame, and the column its text is centred on (the
   frame's width less one, as it was drawn since 1.5.30a). */
#define E_ABOUT_W        55
#define E_ABOUT_H        18
#define E_ABOUT_CENTRE_W (E_ABOUT_W - 1)

/* The System Info box: it keeps this many columns to either side, is never
   narrower than E_SYS_INFO_MIN_W, and its values start E_SYS_INFO_VALUE_COL
   columns in with E_SYS_INFO_VALUE_ROOM columns of the frame left of them. */
#define E_SYS_INFO_SIDE        4
#define E_SYS_INFO_MIN_W       41
#define E_SYS_INFO_VALUE_COL   23
#define E_SYS_INFO_VALUE_ROOM  24
/* Rows of frame and blanks around the System Info lines. */
#define E_SYS_INFO_FIXED_ROWS  7
/* The longest line an information box cuts to fit. */
#define E_INFO_LINE_MAX        512

/**
 * e_info_line - Print one line of an information box, cut at its frame.
 * @box: the open box; @dx: @dy: the line's place from the box's corner;
 * @text: what to print; @col: its colour.
 *
 * A screen too small for the box clips it; a line that would run through
 * the frame, or below it, is cut or left out, so the frame stays clean and
 * nothing is written past the box.
 */
static void e_info_line(WpeBox *box, int dx, int dy, const char *text, int col)
{
 char cut[E_INFO_LINE_MAX];
 int x = box->at.bx_left + dx;
 int y = box->at.bx_top + dy;
 int room = box->at.bx_right - x;

 if (y >= box->at.bx_bottom || room <= 0)
  return;
 if ((int)strlen(text) > room)
 {
  snprintf(cut, sizeof cut, "%.*s", room, text);
  text = cut;
 }
 e_pr_str(x, y, (char *)text, col, 0, 0, 0, 0);
}

/**
 * e_opt_box_wait_key - Hold an information box until the user presses a key.
 * @box: the open box.
 *
 * The About box and the System Info box stay until a key.  A resize is not
 * that key: the owner has already laid the desk and the box out again at the
 * new size.  While the box waits, what arrives from the loop waits for it.
 * Return: the key, or WPE_ESC when the owner could not draw the box again.
 */
static int e_opt_box_wait_key(WpeBox *box)
{
 int c;

#if  MOUSE
 while (e_mshit() != 0)
  ;
#endif
 while ((c = wpe_box_getch(box)) == WPE_RESIZE)
 {
  if (box->pic == NULL)
   return(WPE_ESC);
 }
#if  MOUSE
 while (e_mshit() != 0)
  ;
#endif
 return(c);
}

/**
 * e_opt_box_show - Show an information box until a key, then take it away.
 * @box: the box, with its rule and its draw hook; it is opened here.
 * @f: the current window.
 * Return: 0, or WPE_ESC when there was no memory for the box.
 */
static int e_opt_box_show(WpeBox *box, FENSTER *f)
{
 fk_cursor(0);
 if (wpe_box_open(box) != WPE_BOX_DRAWN)
 {
  e_error(e_msg[ERR_LOWMEM], 1, f->fb);
  return(WPE_ESC);
 }
 wpe_test_ctx_enter(WPE_TEST_CTX_MESSAGE, NULL);
 e_opt_box_wait_key(box);
 wpe_test_ctx_leave();
 if (box->pic != NULL)
  e_close_view(box->pic, 1);
 box->pic = NULL;
 wpe_box_close(box);
 return(0);
}

/**
 * e_opt_info_box_init - Set up an information box centred on the screen.
 * @box: receives the box; @what: its name for the trace; @draw: its draw
 * hook; @w: @h: its size, frame included; @f: the current window.
 */
static void e_opt_info_box_init(WpeBox *box, const char *what, WpeBoxDraw draw,
                                int w, int h, FENSTER *f)
{
 memset(box, 0, sizeof *box);
 box->what = what;
 box->draw = draw;
 box->self = f;
 box->place.anchor = WPE_BOX_CENTRED_SCREEN;
 box->place.w = w;
 box->place.h = h;
}

/**
 * e_about_box_draw - Draw the About box: the program, its version, who
 * wrote and keeps it, and its licence.
 * @box: the box, placed by the owner.
 * Return: WPE_BOX_DRAWN, or WPE_BOX_NO_MEMORY.
 */
static WpeBoxDrawn e_about_box_draw(WpeBox *box)
{
 FENSTER *f = box->self;
 char tmp[60];

 if (wpe_box_frame(box, NULL, f->fb->nr.fb, f->fb->nt.fb, f->fb->ne.fb) != 0)
  return(WPE_BOX_NO_MEMORY);

 sprintf(tmp, "Version %s", VERSION);
#ifdef UNIX
 if (WpeIsXwin() && WpeIsProg())
  e_info_line(box, 9, 2, "XWindow Programming Environment", f->fb->et.fb);
 else if (WpeIsProg())
  e_info_line(box, 10, 2, "Window Programming Environment", f->fb->et.fb);
 else if (WpeIsXwin())
  e_info_line(box, 15, 2, "XWindow Editor", f->fb->et.fb);
 else
#endif
  e_info_line(box, 17, 2, "Window Editor", f->fb->et.fb);
 e_info_line(box, (E_ABOUT_CENTRE_W - (int)strlen(tmp)) / 2, 3, tmp, f->fb->et.fb);
 e_info_line(box, 2, 5, "Created by Fred Kruse, 1993", f->fb->nt.fb);
 e_info_line(box, 2, 6, "Continued by Dennis Payne, 2000-2006", f->fb->nt.fb);
 e_info_line(box, 2, 7, "Maintained by Juan Manuel Mendez Rey, 2026", f->fb->nt.fb);
 e_info_line(box, 2, 8, "https://codeberg.org/mendezr/xwpe", f->fb->nt.fb);
 e_info_line(box, 2, 10, "Copyright (C) 1993-2026", f->fb->nt.fb);
 e_info_line(box, 2, 11, "This software comes with ABSOLUTELY NO WARRANTY;", f->fb->nt.fb);
 e_info_line(box, 2, 12, "This is free software, and you are welcome to", f->fb->nt.fb);
 e_info_line(box, 2, 13, "redistribute it under certain conditions;", f->fb->nt.fb);
 e_info_line(box, 2, 14, "See 'Help\\Editor\\GNU Pub...' for details.", f->fb->nt.fb);
 return(WPE_BOX_DRAWN);
}

/*    About WE      */
int e_about_WE(FENSTER *f)
{
 WpeBox box;

 e_opt_info_box_init(&box, "the About box", e_about_box_draw,
                     E_ABOUT_W, E_ABOUT_H, f);
 return(e_opt_box_show(&box, f));
}

/*    delete everything    */
int e_clear_desk(FENSTER *f)
{
 int i;
 ECNT *cn = f->ed;
#if  MOUSE
 int g[4]; /*  = { 2, 0, 0, 0, };  */
   
 g[0] = 2;
#endif
 fk_cursor(0);
 for (i = cn->mxedt; i > 0; i--)
 {
  f = cn->f[cn->mxedt];
  if( e_close_window(f) == WPE_ESC )
   return(WPE_ESC);
 }
 cn->mxedt = 0;
#if  MOUSE
 fk_mouse(g);
#endif
 e_ini_desk(cn);
#if  MOUSE
 g[0] = 1;
 fk_mouse(g);
#endif
 return(0);
}

/*    redraw everything */
int e_repaint_desk(FENSTER *f)
{
 /* int j; */
 ECNT *cn = f->ed;
 int i, g[4];
#ifndef NO_XWINDOWS
 extern PIC *e_X_l_pic;
 PIC *sv_pic = NULL, *nw_pic = NULL;

 if (WpeIsXwin())
 {
  if (e_X_l_pic && e_X_l_pic != cn->f[cn->mxedt]->pic)
  {
   sv_pic = e_X_l_pic;
   nw_pic = e_open_view(e_X_l_pic->a.x, e_X_l_pic->a.y,
     e_X_l_pic->e.x, e_X_l_pic->e.y, 0, 2);
  }
  (*e_u_ini_size)();
 }
#endif
 if (cn->mxedt < 1)
 {
  e_cls(f->fb->df.fb, f->fb->dc);
  e_ini_desk(f->ed);
#ifndef NO_XWINDOWS
  if ((WpeIsXwin()) && nw_pic)
  {
   e_close_view(nw_pic, 1);
   e_X_l_pic = sv_pic;
  }
#endif
  return(0);
 }
 cn->curedt = cn->mxedt;
 ini_repaint(cn);
 e_abs_refr();
 for (i = 1; i < cn->mxedt; i++)
 {
  e_firstl(cn->f[i], 0);
  e_schirm(cn->f[i], 0);
 }
 e_firstl(cn->f[i], 1);
 e_schirm(cn->f[i], 1);
#ifndef NO_XWINDOWS
 if (WpeIsXwin() && nw_pic)
 {
  e_close_view(nw_pic, 1);
  e_X_l_pic = sv_pic;
 }
#endif
#if  MOUSE
 g[0] = 2; fk_mouse(g);
#endif
 end_repaint();
 e_cursor(cn->f[i], 1);
#if  MOUSE
 g[0] = 0; fk_mouse(g);
 g[0] = 1; fk_mouse(g);
#endif
 return(0);
}

/*    write system information   */
/**
 * e_info_wrap - Print a string over as many lines as it needs, in a box.
 * @box: the open box; @dx: @dy: the first line's place from its corner;
 * @str: the string; @width: the most characters of a line; @col: colour.
 *
 * A path longer than the box is wide goes on the next row, as the System
 * Info box has always shown it; the rows the box has no room for are left
 * out (e_info_line cuts them at the frame).
 * Return: the rows the string takes (at least one).
 */
static int e_info_wrap(WpeBox *box, int dx, int dy, const char *str, int width,
                       int col)
{
 char buf[E_INFO_LINE_MAX];
 int len = strlen(str);
 int rows = 0;
 int pos = 0;

 if (width <= 0)
  return(0);
 while (pos < len)
 {
  int chunk = len - pos;

  if (chunk > width)
   chunk = width;
  if (chunk > E_INFO_LINE_MAX - 1)
   chunk = E_INFO_LINE_MAX - 1;
  snprintf(buf, sizeof buf, "%.*s", chunk, str + pos);
  e_info_line(box, dx, dy + rows, buf, col);
  pos += chunk;
  rows++;
 }
 return(rows > 0 ? rows : 1);
}

/**
 * e_sys_info_rows - How many rows the file and the directory take.
 * @f: the current window; @avail: the characters a value line holds.
 * @file_rows: @dir_rows: receive the counts.
 * @shown: receives the path the file line shows (@buf holds it when it is
 * the full path); @buf: @size: room for it.
 */
static void e_sys_info_rows(FENSTER *f, int avail, int *file_rows, int *dir_rows,
                            const char **shown, char *buf, size_t size)
{
 if (strcmp(f->datnam, "Clipboard") != 0 && strcmp(f->dirct, f->ed->dirct) != 0)
 {
  snprintf(buf, size, "%s%s%s", f->dirct, DIRS, f->datnam);
  *shown = buf;
 }
 else
  *shown = f->datnam;
 if (avail < 1)
  avail = 1;
 *file_rows = ((int)strlen(*shown) + avail - 1) / avail;
 if (*file_rows < 1)
  *file_rows = 1;
 *dir_rows = ((int)strlen(f->ed->dirct) + avail - 1) / avail;
 if (*dir_rows < 1)
  *dir_rows = 1;
}

/**
 * e_sys_info_width - The width of the System Info box on this screen.
 *
 * Most of the screen, with E_SYS_INFO_SIDE columns left to either side.
 */
static int e_sys_info_width(void)
{
 int w = MAXSCOL - 2 * E_SYS_INFO_SIDE;

 return(w < E_SYS_INFO_MIN_W ? E_SYS_INFO_MIN_W : w);
}

/**
 * e_sys_info_refresh - Fit the System Info box to the screen and its text.
 * @box: the box; its size follows the width of the screen and the length of
 * the paths, so a resize lays out the lines again.
 */
static void e_sys_info_refresh(WpeBox *box)
{
 FENSTER *f = box->self;
 char tmp[E_INFO_LINE_MAX];
 const char *shown;
 int w = e_sys_info_width();
 int file_rows, dir_rows;

 e_sys_info_rows(f, w - 1 - E_SYS_INFO_VALUE_ROOM, &file_rows, &dir_rows,
                 &shown, tmp, sizeof tmp);
 box->place.w = w;
 box->place.h = E_SYS_INFO_FIXED_ROWS + file_rows + dir_rows;
}

static WpeBoxDrawn e_sys_info_box_draw(WpeBox *box);   /* (below) */

/**
 * e_sys_info - Display the System Information dialog.
 * @f: Current window (FENSTER).  Used to read the filename, directory,
 *     and editor state for display.
 *
 * Shows a modal dialog with:
 *   - Current File:      filename (or full path if in a different directory)
 *   - Current Directory: the editor's working directory
 *   - Number of Files:   count of open editor buffers
 *
 * The dialog adapts to the terminal: width uses most of the screen,
 * and height grows when long paths wrap to multiple lines.
 *
 * Return: 0 on success, WPE_ESC on memory error.
 */
int e_sys_info(FENSTER *f)
{
 WpeBox box;

 e_opt_info_box_init(&box, "the System Info box", e_sys_info_box_draw,
                     E_SYS_INFO_MIN_W, E_SYS_INFO_FIXED_ROWS + 2, f);
 box.refresh = e_sys_info_refresh;
 return(e_opt_box_show(&box, f));
}

/**
 * e_sys_info_box_draw - Draw the System Information box where the owner
 * placed it, for the screen as it is now.
 * @box: the box.
 * Return: WPE_BOX_DRAWN, or WPE_BOX_NO_MEMORY.
 */
static WpeBoxDrawn e_sys_info_box_draw(WpeBox *box)
{
 FENSTER *f = box->self;
 char tmp[E_INFO_LINE_MAX];
 const char *filepath;
 int avail = box->at.bx_right - box->at.bx_left - E_SYS_INFO_VALUE_ROOM;
 int file_rows, dir_rows, row = 2;
 int col = f->fb->nt.fb;

 if (wpe_box_frame(box, " Information ", f->fb->nr.fb, f->fb->nt.fb,
                   f->fb->ne.fb) != 0)
  return(WPE_BOX_NO_MEMORY);
 e_sys_info_rows(f, avail, &file_rows, &dir_rows, &filepath, tmp, sizeof tmp);

 e_info_line(box, 3, row, " Current File: ", col);
 if (strcmp(f->datnam, "Clipboard") != 0)
  row += e_info_wrap(box, E_SYS_INFO_VALUE_COL, row, filepath, avail, col);
 else
  row++;
 row++;

 e_info_line(box, 3, row, " Current Directory: ", col);
 row += e_info_wrap(box, E_SYS_INFO_VALUE_COL, row, f->ed->dirct, avail, col);
 row++;

 e_info_line(box, 3, row, " Number of Files: ", col);
 e_info_line(box, E_SYS_INFO_VALUE_COL, row,
   WpeNumberToString(f->ed->mxedt, WpeNumberOfPlaces(f->ed->mxedt), tmp), col);
 return(WPE_BOX_DRAWN);
}

/*   color adjustments  */
int e_ad_colors(FENSTER *f)
{
 int n, xa = 48, ya = 2, num = 5;
 OPTK *opt = MALLOC(num * sizeof(OPTK));

 opt[0].t = "Editor Colors";     opt[0].x = 0;  opt[0].o = 'E';
 opt[1].t = "Desk Colors";       opt[1].x = 0;  opt[1].o = 'D';
 opt[2].t = "Option Colors";     opt[2].x = 0;  opt[2].o = 'O';
 opt[3].t = "Progr. Colors";     opt[3].x = 0;  opt[3].o = 'P';
 opt[4].t = "AI pane Colors";    opt[4].x = 0;  opt[4].o = 'A';

 n = e_opt_sec_box(xa, ya, num, opt, f, 1);

 FREE(opt);
 if (n < 0)
  return(WPE_ESC);

 return(e_ad_colors_md(f, n));
}

int e_ad_colors_md(FENSTER *f, int md)
{
 int sw = 0, xa = 0, ya = 1, xe = xa + 79, ye = ya + 22;
 PIC *pic;

 pic = e_std_kst(xa, ya, xe, ye, "Adjust Colors", 1, f->fb->er.fb,
   f->fb->et.fb, f->fb->es.fb);
 if (pic == NULL)
 {
  e_error(e_msg[ERR_LOWMEM], 1, f->fb);
  return(WPE_ESC);
 }
 sw = e_dif_colors(sw, xe-13, ya+1, f, md);
 e_close_view(pic, 1);
 e_repaint_desk(f);
 return(sw);
}

/*   install/execute color adjustments */
int e_dif_colors(int sw, int xa, int ya, FENSTER *f, int md)
{
 COLOR *frb = &(f->fb->er);
 int c = 0, bg, num;

 if (md == 1)
 {
  bg = 11;
  num = 5;
 }
 else if (md == 2)
 {
  bg = 16;
  num = 15;
 }
 else if (md == 3)
 {
  bg = 32;
  num = 5;
 }
 else if (md == COLOR_MODE_AI)
 {
  bg = COLOR_AI_FIRST_ENTRY;
  num = COLOR_AI_ENTRIES;
 }
 else
 {
  bg = 0;
  num = 11;
 }
 while (c != WPE_ESC && c > -2)
 {
  e_pr_dif_colors(sw, xa, ya, f, 1, md);
  e_pr_col_kasten(xa-28, ya+1, frb[sw+bg].f, frb[sw+bg].b , f, 0);
  e_pr_ed_beispiel(1, 2, f, sw, md);
#if  MOUSE
  if ((c = wpe_paint_modal_getch()) == -1)
   c = e_opt_cw_mouse(xa, ya, md);
#else
  c = wpe_paint_modal_getch();
#endif
  if (c >= 375 && c <= 393)
  {
   sw = c-375;
   if (sw >= num) sw = num - 1;
  }
  else if (c == 326)
   sw = 0;
  else if (c == 334)
   sw = num-1;
  else if (c == 327)
   sw = (sw == 0) ? num-1 : sw-1;
  else if (c == 335)
   sw = (sw == num-1) ? 0 : sw+1;
  else if (c == WPE_CR || c == 285 || c == 330)
  {
   e_pr_dif_colors(sw, xa, ya, f, 0, md);
   *(frb+sw+bg) = e_n_clr(e_frb_menue(sw, xa-28, ya+1, f, md));
  }
 }
 return(c);
}

char *text[] = {  "Border", "Bord. Bt.", "Text", "Txt Mrk.1", "Txt Mrk.2",
		  "Scrollbar", "Help Hdr.", "Help Btt.", "Help Mrk.",
		  "Breakpnt.", "Stop Brk.",
		  "Border", "Bord. Bt.", "Text", "Txt Mrk.", "Backgrnd.",
		  "Border", "Bord. Bt.", "Text", "Text Sw.",
		  "Write",  "Wrt. Mrk.", "Data", "Data M.A", "Data M.P",
		  "Switch", "Swtch. S.", "Swtch. A.", "Button",
		  "Bttn. Sw.", "Bttn. Ak.",
		  "Text", "Res. Wrd.", "Constants", "Pre-Proc.", "Comments",
		  "User", "Machinery", "Attention", "Error"
	       };

/*   draw color box */
void e_pr_dif_colors(int sw, int xa, int ya, FENSTER *f, int sw2, int md)
{
 int i, rfrb, cfrb, xe = xa + 12, ye, bg;
 char *header;

 if (md == 1)
 {
  ye = ya + 6;
  bg = 11;
  header = "Desk";
 }
 else if (md == 2)
 {
  ye = ya + 16;
  bg = 16;
  header = "Options";
 }
 else if (md == 3)
 {
  ye = ya + 6;
  bg = 31;
  header = "C-Prog.";
 }
 else if (md == COLOR_MODE_AI)
 {
  ye = ya + COLOR_AI_ENTRIES + 1;
  bg = COLOR_AI_FIRST_NAME;
  header = "AI pane";
 }
 else
 {
  ye = ya + 12;
  bg = 0;
  header = "Editor";
 }
 rfrb = sw2 == 0 ? f->fb->nt.fb : f->fb->fs.fb;
 e_std_rahmen(xa, ya, xe, ye, header, 0, rfrb, 0);
 for (i = 0; i < ye-ya-1; i++)
 {
  cfrb = i == sw ? f->fb->fz.fb : f->fb->ft.fb;
  e_pr_str_wsd(xa+2, ya+1+i,  text[i+bg], cfrb, 0, 0, 0, xa+1, xe-1);
 }
}

/*    color menu  */
int e_frb_x_menue(int sw, int xa, int ya, FENSTER *f, int md)
{
 COLOR *frb = &(f->fb->er);
 int c = 1, fsv = frb[sw].fb, x, y;

 if (md == 1) sw += 11;
 else if (md == 2) sw += 16;
 else if (md == 3) sw += 32;
 else if (md == COLOR_MODE_AI) sw += COLOR_AI_FIRST_ENTRY;
 y = frb[sw].b;
 x = frb[sw].f;
 do
 {
  if (c == CRI && y < 7) y++;
  else if (c == CLE && y > 0) y--;
  else if (c == CUP && x > 0) x--;
  else if (c == CDO && x < 15) x++;
  else if (c >= 1000 && c < 1256)
  {
   x = (c-1000)/16;
   y = (c-1000)%16;
  }
  e_pr_x_col_kasten(xa, ya, x, y, f, 1);
  frb[sw] = e_s_clr(x, y);
  e_pr_ed_beispiel(1, 2, f, sw, md);
#if  MOUSE
  if ((c = wpe_paint_modal_getch()) == -1) c = e_opt_ck_mouse(xa, ya, md);
#else
  c = wpe_paint_modal_getch();
#endif
 } while (c != WPE_ESC && c != WPE_CR && c > -2);
 if (c == WPE_ESC || c < -1)
  frb[sw] = e_n_clr(fsv);
 return(frb[sw].fb);
}

/*   draw color box  */
void e_pr_x_col_kasten(int xa, int ya, int x, int y, FENSTER *f, int sw)
{
 int i, j, rfrb, ffrb, xe = xa + 25, ye = ya + 18;

 rfrb = sw == 0 ? f->fb->nt.fb : f->fb->fs.fb;
 ffrb = rfrb % 16;
 e_std_rahmen(xa-2, ya-1, xe, ye, "Colors", 0, rfrb, 0);
/*     e_pr_str((xa+xe-8)/2, ya-1, "Colors", rfrb, 0, 1, 
                                        f->fb->ms.f+16*(rfrb/16), 0);
*/
 for (j = 0; j < 8; j++)
  for (i = 0; i < 16; i++)
  {
   e_pr_char(3*j+xa, i+ya+1, ' ', 16*j+i);
   e_pr_char(3*j+xa+1, i+ya+1, 'x', 16*j+i);
   e_pr_char(3*j+xa+2, i+ya+1, ' ', 16*j+i);
  }
 for (i = 0; i < 18; i++)
 {
  e_pr_char(xa-1, i+ya, ' ', rfrb);
  e_pr_char(xe-1, i+ya, ' ', rfrb);
 }
 for (j = 0; j < 25; j++)
 {
  e_pr_char(j+xa-1, ya, ' ', rfrb);
  e_pr_char(j+xa-1, ye-1, ' ', rfrb);
 }
#ifdef NEWSTYLE
 if (!WpeIsXwin())
 {
#endif
  for (i = 0; i < 3; i++)
  {
   e_pr_char(3*y+xa+i, x+ya, RE5, x>0 ? 16*y+ffrb : rfrb );
   e_pr_char(3*y+xa+i, x+ya+2, RE5, x<15 ? 16*y+ffrb : rfrb );
  }
  e_pr_char(3*y+xa-1, x+ya+1, RE6, y<1 ? rfrb  : 16*(y-1)+ffrb);
  e_pr_char(3*y+xa+3, x+ya+1, RE6, y>6 ? rfrb  : 16*(y+1)+ffrb);
  e_pr_char(3*y+xa-1, x+ya, RE1, (y<1 || x<1) ? rfrb  : 16*(y-1)+ffrb);
  e_pr_char(3*y+xa+3, x+ya, RE2, (y>6 || x<1) ? rfrb  : 16*(y+1)+ffrb);
  e_pr_char(3*y+xa-1, x+ya+2, RE3, (y<1 || x>14) ? rfrb  : 16*(y-1)+ffrb);
  e_pr_char(3*y+xa+3, x+ya+2, RE4, (y>6 || x>14) ? rfrb  : 16*(y+1)+ffrb);
#ifdef NEWSTYLE
 }
 else e_make_xrect(3*y+xa, x+ya+1, 3*y+xa+2, x+ya+1, 1);
#endif
}

/*    draw color example   */
void e_pr_ed_beispiel(int xa, int ya, FENSTER *f, int sw, int md)
{
 COLOR *frb = &(f->fb->er);
 FARBE *fb = f->fb;
 int i, j, xe = xa+31, ye = ya+19;

 frb[sw] = e_s_clr(frb[sw].f, frb[sw].b);
 if (md == 1)
 {
  e_blk(xe-xa+1, xa+1, ya, fb->mt.fb);
  e_pr_str_wsd(xa+5, ya,  "Edit", fb->mt.fb, 0, 1, f->fb->ms.fb,
    xa+3, xa+11);
  e_pr_str_wsd(xa+18, ya,  "Options", fb->mz.fb, 0, 0, f->fb->ms.fb,
    xa+16, xa+27);
  for (i = ya+1; i < ye; i++)
   for (j = xa+1; j <= xe+1; j++)
   {
    e_pr_char(j, i, fb->dc, fb->df.fb);
    e_pr_char(j, i, fb->dc, fb->df.fb);
   }
  e_std_rahmen(xa+17, ya+1, xa+26, ya+3, NULL, 0, fb->mr.fb, 0);
  e_pr_str(xa+19, ya+2, "Colors", fb->mt.fb, 0, 1, fb->ms.fb, 0);
  e_blk(xe-xa+1, xa+1, ye, fb->mt.fb);
  e_pr_str_wsd(xa+4, ye, "Alt-F3 Close Window",
    fb->mt.fb, 0, 6, fb->ms.fb, xa+2, xa+25);
 }
 else if (md == 2)
 {
  e_std_rahmen(xa, ya, xe, ye, "Message", 1, fb->nr.fb, f->fb->ne.fb);
  for(i = ya+1; i < ye; i++) e_blk(xe-xa-1, xa+1, i, fb->nt.fb)
   ;
  e_pr_str(xa+4, ya+2, "Name:", f->fb->nt.fb, 0, 1,
    f->fb->nsnt.fb, f->fb->nt.fb);
  e_pr_str(xa+5, ya+3, "Active Write-Line ", fb->fa.fb, 0, 0, 0, 0);
  e_pr_str(xa+4, ya+5, "Name:", f->fb->nt.fb, 0, 1,
    f->fb->nsnt.fb, f->fb->nt.fb);
  e_pr_str(xa+5, ya+6, "Passive Write-Line", fb->fr.fb, 0, 0, 0, 0);
  e_pr_str(xa+4, ya+8, "Data:", f->fb->nt.fb, 0, 1,
    f->fb->nsnt.fb, f->fb->nt.fb);
  e_pr_str(xa+5, ya+9, "Active Marked ", f->fb->fz.fb, 0, 0, 0, 0);
  e_pr_str(xa+5, ya+10, "Passive Marked", f->fb->frft.fb, 0, 0, 0, 0);
  e_pr_str(xa+5, ya+11, "Data Text     ", f->fb->ft.fb, 0, 0, 0, 0);
  e_pr_str(xa+4, ya+13, "Switches:", f->fb->nt.fb, 0, 1,
    f->fb->nsnt.fb, f->fb->nt.fb);
  e_pr_str(xa+5, ya+14, "[X] Active Switch ", f->fb->fsm.fb, 0, 0, 0, 0);
  e_pr_str(xa+5, ya+15, "[ ] Passive Switch", f->fb->fs.fb, 4, 1,
    f->fb->nsft.fb, f->fb->fs.fb);
  e_pr_str(xa+6 , ye-2, "Button", f->fb->nz.fb, 0, -1,
    f->fb->ns.fb, f->fb->nt.fb);
  e_pr_str(xe-12 , ye-2, "Active", f->fb->nm.fb, 0, -1,
    f->fb->nm.fb, f->fb->nt.fb);
#ifdef NEWSTYLE
  if (WpeIsXwin())
  {
   e_make_xrect(xa+4, ya+3, xa+23, ya+3, 1);
   e_make_xrect(xa+4, ya+6, xa+23, ya+6, 1);
   e_make_xrect(xa+4, ya+9, xa+19, ya+11, 1);
   e_make_xrect_abs(xa+4, ya+9, xa+19, ya+9, 0);
   e_make_xrect(xa+4, ya+14, xa+23, ya+15, 1);
   e_make_xrect_abs(xa+4, ya+14, xa+23, ya+14, 0);
  }
#endif
 }
 else
 {
  e_std_rahmen(xa, ya, xe, ye, "Filename", 1, fb->er.fb, fb->es.fb);
  e_mouse_bar(xe, ya+1, ye-ya-1, 0, fb->em.fb);
  e_mouse_bar(xa+20, ye, 11, 1, fb->em.fb);
  e_pr_char(xe-3, ya, WZN, fb->es.fb);
#ifdef NEWSTYLE
  if (!WpeIsXwin())
  {
#endif
   e_pr_char(xe-4, ya, '[', fb->er.fb);
   e_pr_char(xe-2, ya, ']', fb->er.fb);
#ifdef NEWSTYLE
  }
  else e_make_xrect(xe-4, ya, xe-2, ya, 0);
#endif
  for (i = ya+1; i < ye; i++) e_blk(xe-xa-1, xa+1, i, fb->et.fb);
  if (md == 3)
  {
   e_pr_str(xa+4, ya+3, "#Preprozessor Comands", fb->cp.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+5, "This are C-Text Colors", fb->ct.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+7, "int char {} [] ; ,", fb->cr.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+9, "\"Constants\" 12 0x13", fb->ck.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+11, "/*   Comments    */", fb->cc.fb, 0, 0, 0, 0);
  }
  else if (md == COLOR_MODE_AI)
  {
   /* One line per role, at the rows e_opt_bs_mouse_ai maps clicks on. */
   e_pr_str(xa+4, ya+OPT_AI_EXAMPLE_USER_LINE, "You: what does this do?",
     fb->aiu.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+OPT_AI_EXAMPLE_REPLY_LINE, "AI: it returns the sum",
     fb->et.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+OPT_AI_EXAMPLE_MACHINERY_LINE, "[agent] running the tests",
     fb->aim.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+OPT_AI_EXAMPLE_ATTENTION_LINE, ">> y = allow   n / Esc = deny",
     fb->aia.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+OPT_AI_EXAMPLE_ERROR_LINE, "[AI edit] backend error",
     fb->aie.fb, 0, 0, 0, 0);
  }
  else
  {
   e_pr_str(xa+4, ya+3, "This are the Editor Colors", fb->et.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+4, "And this is a marked Line", fb->ez.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+5, "This is a found word", fb->et.fb, 0, 0, 0, 0);
   e_pr_str(xa+14, ya+5, "found", fb->ek.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+8, "Help Header", fb->hh.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+9, "This is a marked Word", fb->et.fb, 0, 0, 0, 0);
   e_pr_str(xa+14, ya+9, "marked", fb->hm.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+10, "in the Help File", fb->et.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+11, "Help Button", fb->hb.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+14, "This is a Breakpoint", fb->db.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+15, "Stop at Breakpoint", fb->dy.fb, 0, 0, 0, 0);
   e_pr_str(xa+4, ya+OPT_UNCONFIRMED_BRK_LINE,
     "An unconfirmed Breakpoint", fb->du.fb, 0, 0, 0, 0);
  }
 }
 frb[sw] = e_s_clr(frb[sw].f, frb[sw].b);
}

/*   Save - Options - Menu   */
/* What Save Options asks when the option file's path is longer than its field. */
#define E_OPT_SAVE_LONG_FMT "Save the options to\n%s\n?"

/**
 * e_opt_save_to_long_path - Save Options when the field cannot hold the path.
 * @f: the window.
 *
 * The name field holds E_ARG_FIELD_MAX bytes; a deeper option file (a
 * project folder far down the tree) would be cut there, and Enter would
 * then save to a file the user never named.  Instead the whole path is
 * shown, read-only, and Yes saves there.
 * Return: as e_opt_save.
 */
static int e_opt_save_to_long_path(FENSTER *f)
{
 size_t size = strlen(E_OPT_SAVE_LONG_FMT) + strlen(f->ed->optfile) + 1;
 char *ask = MALLOC(size);
 int answer;

 if (!ask)
  return(0);
 snprintf(ask, size, E_OPT_SAVE_LONG_FMT, f->ed->optfile);
 answer = e_message(1, ask, f);
 FREE(ask);
 if (answer != 'Y')
  return(0);
 e_save_opt(f);
 return(1);
}

int e_opt_save(FENSTER *f)
{
 int ret;
 char tmp[E_ARG_FIELD_MAX + 1];

 if (e_opt_say_file_missing(f->ed, E_OPT_NOT_SAVED))
  return(0);

 if (strlen(f->ed->optfile) > E_ARG_FIELD_MAX)
  return(e_opt_save_to_long_path(f));
 snprintf(tmp, sizeof tmp, "%s", f->ed->optfile);
 ret = e_add_arguments(tmp, sizeof tmp, "Save Option File", f, 0, AltS, NULL);
 if (ret)
 {
  f->ed->optfile = REALLOC(f->ed->optfile, (strlen(tmp)+1)*sizeof(char));
  strcpy(f->ed->optfile, tmp);
  e_save_opt(f);
 }
 return(ret);
}

char *WpeStringToValue(const char *str)
{
 char *answer, *cur_ans;
 const char *cur_str;
 int i, len;

 len = strlen(str);
 answer = WpeMalloc((len * 2) * sizeof(char));
 for (i = strlen(str), cur_ans = answer, cur_str = str; i; i--, cur_str++)
 {
  if ((*cur_str == '\n') || (*cur_str == '\\'))
  {
   len++;
   cur_ans[0] = '\\';
   cur_ans[1] = ((*cur_str == '\n') ? 'n' : '\\');
   cur_ans++;
  }
  else
   *cur_ans = *cur_str;
  cur_ans++;
 }
 *cur_ans = 0;
 return answer;
}

char *WpeValueToString(const char *value)
{
 char *answer, *cur_ans;
 const char *cur_val;
 int i;

 answer = WpeMalloc((strlen(value) + 1) * sizeof(char));
 for (i = strlen(value), cur_ans = answer, cur_val = value; i; i--, cur_ans++)
 {
  if (*cur_val == '\\')
  {
   cur_val++;
   *cur_ans = ((*cur_val == 'n') ? '\n' : '\\');
  }
  else
   *cur_ans = *cur_val;
  cur_val++;
 }
 *cur_ans = 0;
 return answer;
}

/* ---- The key style: one place decides what each style brings ---- */

/** The option bits the Modern keys bring with them: the style bit, and the
    three that make a block a selection that typing replaces, that take back
    an auto-indent nobody used and that keep the file's last line break. */
#define E_KEYS_MODERN_BITS \
 (ED_CUA_STYLE | ED_BLOCK_SELECTION | ED_TRIM_AUTO_INDENT | ED_KEEP_FILE_ENDING)

#define E_KEYS_NOTICE_FMT \
 "Keys are now Modern: %s debugs, %s runs. Change: Options > Editor > Keys"
#define E_KEYS_NOTICE_LEN 160
#define E_KEYS_NAME_LEN 32

static int e_keys_session_style = E_KEYS_STYLE_NONE;  /* --keys / XWPE_KEYS for this run */
static int e_keys_file_edopt = 0;      /* the options as the file gave them, while a session style overrides */
static int e_keys_notice_seen = 0;     /* the file says KeysNotice : 1 */
static int e_keys_notice_pending = 0;  /* the file was migrated at this start */

/**
 * e_keys_style_from_text - The key style a word names.
 * @text: "borland" or "modern", in any case.
 * Return: WPE_STYLE_BORLAND or WPE_STYLE_MODERN, E_KEYS_STYLE_NONE for any
 * other word.  The one parser of --keys and XWPE_KEYS.
 */
int e_keys_style_from_text(const char *text)
{
 if (text && WpeStrccmp(E_KEYS_TEXT_BORLAND, (char *)text) == 0)
  return(WPE_STYLE_BORLAND);
 if (text && WpeStrccmp(E_KEYS_TEXT_MODERN, (char *)text) == 0)
  return(WPE_STYLE_MODERN);
 return(E_KEYS_STYLE_NONE);
}

/**
 * e_keys_arg_takes_value - Is @arg the form of --keys whose style is the
 * next argument?
 *
 * The argument loops of main skip the word that follows it, so a style name
 * is never opened as a file.
 */
int e_keys_arg_takes_value(const char *arg)
{
 return(arg && strcmp(arg, E_KEYS_OPTION) == 0);
}

/**
 * e_keys_session_read - Take the key style a run is asked for.
 * @argc, @argv: the command line; @bad: out, the word that is no style.
 *
 * "--keys borland|modern" (or "--keys=...") wins over $XWPE_KEYS, which wins
 * over the option file.  The style is for this run only: it is never written
 * to xwperc (e_keys_edopt_for_file), so a README line like "xwpe --keys
 * modern file.c" cannot change a user's file.  Setting either also means the
 * first-run question is not asked.
 * Return: 0, or -1 with *@bad set when the word names no style.
 */
int e_keys_session_read(int argc, char **argv, const char **bad)
{
 const char *env = getenv(E_KEYS_ENV);
 const char *word = NULL;
 size_t len = strlen(E_KEYS_OPTION);
 int i, style;

 e_keys_session_style = E_KEYS_STYLE_NONE;
 if (env && env[0])
  word = env;
 for (i = 1; i < argc; i++)
 {
  if (e_keys_arg_takes_value(argv[i]) && i + 1 < argc)
   word = argv[i + 1];
  else if (strncmp(argv[i], E_KEYS_OPTION "=", len + 1) == 0)
   word = argv[i] + len + 1;
  else if (e_keys_arg_takes_value(argv[i]))
   word = "";
 }
 if (!word)
  return(0);
 style = e_keys_style_from_text(word);
 if (style == E_KEYS_STYLE_NONE)
 {
  *bad = word;
  return(-1);
 }
 e_keys_session_style = style;
 return(0);
}

/** e_keys_session_wanted - Was this run given a key style (so no question is asked)? */
int e_keys_session_wanted(void)
{
 return(e_keys_session_style != E_KEYS_STYLE_NONE);
}

/**
 * e_keys_session_apply - Put the style this run was asked for over the file's.
 * @cn: the editor, its options read.
 *
 * Modern brings its option bits with it (the selection that typing replaces,
 * the trimmed auto-indent, the kept last line break); Borland takes only the
 * style bit away, so a profile that also chose a block mode keeps it.  The
 * file's own bits are kept for e_keys_edopt_for_file.
 */
void e_keys_session_apply(ECNT *cn)
{
 if (e_keys_session_style == E_KEYS_STYLE_NONE)
  return;
 e_keys_file_edopt = cn->edopt;
 if (e_keys_session_style == WPE_STYLE_MODERN)
  cn->edopt |= E_KEYS_MODERN_BITS;
 else
  cn->edopt &= ~ED_CUA_STYLE;
}

/**
 * e_keys_edopt_for_file - The editor options to write to xwperc.
 * @cn: the editor.
 *
 * While a session style overrides the file, the file keeps the style bits it
 * had: only what the user sets in Options > Editor > Keys is written.
 */
int e_keys_edopt_for_file(const ECNT *cn)
{
 if (e_keys_session_style == E_KEYS_STYLE_NONE)
  return(cn->edopt);
 return((cn->edopt & ~E_KEYS_MODERN_BITS) | (e_keys_file_edopt & E_KEYS_MODERN_BITS));
}

/** e_keys_session_release - The user chose keys in the dialog: the session style ends. */
static void e_keys_session_release(void)
{
 e_keys_session_style = E_KEYS_STYLE_NONE;
}

/**
 * e_keys_preset - The option bits a key style sets together.
 * @edopt: the options now; @modern: 1 for Modern, 0 for Borland.
 *
 * What the first-run question, the one-time upgrade of an old CUA profile and
 * the Keys radio of the dialog all apply: Modern is the style bit plus the
 * selection, trim and kept-ending bits; Borland clears those four, and keeps
 * a WordStar block mode a user chose.
 * Return: the new options.
 */
int e_keys_preset(int edopt, int modern)
{
 if (modern)
  return(edopt | E_KEYS_MODERN_BITS);
 return(edopt & ~E_KEYS_MODERN_BITS);
}

/**
 * e_keys_migrate - Take an options file from the CUA key style to Modern, once.
 * @cn: the editor, its own options file just read.
 *
 * The style the 1.6 versions called CUA is now Modern.  A file that has its
 * bit and none of the bits Modern added, and does not yet say it was
 * upgraded, gets those bits set: without them the user would have Modern keys
 * with the old block marks, no replace-on-type and no trim.  Only the user's
 * own file is upgraded (a project's xwperc is not rewritten), and not on a
 * run that was given a style: the upgrade waits for a normal start.
 */
void e_keys_migrate(ECNT *cn)
{
 int added = ED_BLOCK_SELECTION | ED_TRIM_AUTO_INDENT | ED_KEEP_FILE_ENDING;

 if (e_keys_session_style != E_KEYS_STYLE_NONE || e_keys_notice_seen)
  return;
 if (e_opt_user_file || e_opt_file_missing(cn))
  return;
 if (!(cn->edopt & ED_CUA_STYLE) || (cn->edopt & added))
  return;
 cn->edopt = e_keys_preset(cn->edopt, 1);
 e_keys_notice_pending = 1;
}

/** e_keys_notice_wanted - Was the file upgraded at this start, and not yet said so? */
int e_keys_notice_wanted(void)
{
 return(e_keys_notice_pending);
}

/**
 * e_keys_notice_shown - Say once that the keys changed, and record that it was said.
 * @cn: the editor, its first window open.
 *
 * The status row names the two keys whose meaning moved furthest (F5 and
 * Ctrl-F5 both start the program); the file gets KeysNotice : 1 by the same
 * save, so the notice and the upgrade never come back.
 */
void e_keys_notice_shown(ECNT *cn)
{
 char line[E_KEYS_NOTICE_LEN];
 char debug_key[E_KEYS_NAME_LEN];
 const char *name;

 if (!e_keys_notice_pending)
  return;
 e_keys_notice_pending = 0;
 e_keys_notice_seen = 1;
 if (cn->mxedt >= 0 && cn->f[0])
  e_save_opt(cn->f[0]);
 if (!cn->fb)
  return;
 name = wpe_binding_key_name(WPE_ACT_DEBUG_RUN, WPE_STYLE_MODERN);
 snprintf(debug_key, sizeof debug_key, "%s", name ? name : "Debug");
 name = wpe_binding_key_name(WPE_ACT_RUN, WPE_STYLE_MODERN);
 snprintf(line, sizeof line, E_KEYS_NOTICE_FMT, debug_key, name ? name : "Run");
 e_status_hold(line);
 e_status_paint(cn->fb, line);
 e_refresh();
}

/* ---- The first-run question: which keys does a new profile want? ---- */

#define E_KEYS_ASK_NAME "Keys"
#define E_KEYS_ASK_LINES 5
#define E_KEYS_ASK_MODERN_FMT "Modern: %s saves, %s undoes, %s debugs, as in VS Code"
#define E_KEYS_ASK_BORLAND_FMT "Borland: %s saves, %s builds, as in Turbo C"
#define E_KEYS_ASK_LINE_MAX 96
#define E_KEYS_ASK_MARGIN 6          /* the frame and the room around the text */
#define E_KEYS_ASK_BUTTON_ROW_UP 2   /* rows from the box's bottom to its buttons */
#define E_KEYS_ASK_BOX_BOTTOM 6      /* rows the box keeps clear below it */
#define E_KEYS_ASK_BOX_PAD 5         /* rows the frame and blanks take besides the text */
#define E_KEYS_ASK_MODERN_X 4
#define E_KEYS_ASK_BORLAND_X 24
#define E_KEYS_ASK_CANCEL_BACK 9     /* columns from the right edge to Cancel */
#define E_KEYS_CHOSEN_FMT "Keys: %s. Options > Editor > Keys changes it"

/**
 * e_keys_ask_allowed - May the first-run question be put now?
 *
 * Not when the run was given a style (--keys, XWPE_KEYS), and on the terminal
 * path not when the input or the screen is not a terminal: a pipe or a
 * script, and "git commit" with this editor as $EDITOR on a terminal that has
 * been redirected, never meet a box with nobody to answer it.  X11 and
 * Wayland show the box as a dialog.
 */
int e_keys_ask_allowed(void)
{
 if (e_keys_session_wanted())
  return(0);
 if (WpeIsXwin())
  return(1);
 return(isatty(0) && isatty(1));
}

/**
 * e_keys_ask_box - Put the question and read the answer.
 * @f: a window of the editor.
 * Return: WPE_STYLE_MODERN or WPE_STYLE_BORLAND; Esc is Borland, the style the
 * editor started in, and the question is not asked again (the configuration
 * folder exists from now on).
 *
 * Enter takes the focused button, which is Modern: the choice a user who
 * presses Enter without reading gets is the style most people already know.
 * The keys the two lines name come from the key table, so the box cannot
 * promise a key the style does not have.
 */
static int e_keys_ask_box(FENSTER *f)
{
 W_OPTSTR *o = e_init_opt_kst(f);
 char text[E_KEYS_ASK_LINES][E_KEYS_ASK_LINE_MAX];
 char a[E_KEYS_NAME_LEN], b[E_KEYS_NAME_LEN], c[E_KEYS_NAME_LEN];
 int i, ret, mxlen = 0, len;

 if (!o)
  return(E_KEYS_STYLE_NONE);
 snprintf(a, sizeof a, "%s", wpe_binding_key_name(WPE_ACT_SAVE, WPE_STYLE_MODERN));
 snprintf(b, sizeof b, "%s", wpe_binding_key_name(WPE_ACT_EDIT_UNDO, WPE_STYLE_MODERN));
 snprintf(c, sizeof c, "%s", wpe_binding_key_name(WPE_ACT_DEBUG_RUN, WPE_STYLE_MODERN));
 snprintf(text[0], sizeof text[0], "%s", "Which keys do you want?");
 text[1][0] = '\0';
 snprintf(text[2], sizeof text[2], E_KEYS_ASK_MODERN_FMT, a, b, c);
 snprintf(a, sizeof a, "%s", wpe_binding_key_name(WPE_ACT_SAVE, WPE_STYLE_BORLAND));
 snprintf(b, sizeof b, "%s", wpe_binding_key_name(WPE_ACT_MAKE, WPE_STYLE_BORLAND));
 snprintf(text[3], sizeof text[3], E_KEYS_ASK_BORLAND_FMT, a, b);
 snprintf(text[4], sizeof text[4], "%s", "Options > Editor > Keys changes it later");
 for (i = 0; i < E_KEYS_ASK_LINES; i++)
 {
  len = (int)strlen(text[i]);
  if (len > mxlen)
   mxlen = len;
 }
 o->ye = MAXSLNS - E_KEYS_ASK_BOX_BOTTOM;
 o->ya = o->ye - E_KEYS_ASK_LINES - E_KEYS_ASK_BOX_PAD;
 o->xa = (MAXSCOL - mxlen - E_KEYS_ASK_MARGIN) / 2;
 o->xe = o->xa + mxlen + E_KEYS_ASK_MARGIN;
 o->bgsw = 0;
 o->name = E_KEYS_ASK_NAME;
 for (i = 0; i < E_KEYS_ASK_LINES; i++)
  e_add_txtstr((o->xe - o->xa - (int)strlen(text[i])) / 2, 2 + i, text[i], o);
 o->crsw = AltM;
 e_add_bttstr(E_KEYS_ASK_MODERN_X, o->ye - o->ya - E_KEYS_ASK_BUTTON_ROW_UP, 0, AltM, "Modern", NULL, o);
 e_add_bttstr(E_KEYS_ASK_BORLAND_X, o->ye - o->ya - E_KEYS_ASK_BUTTON_ROW_UP, 0, AltB, "Borland", NULL, o);
 e_add_bttstr(o->xe - o->xa - E_KEYS_ASK_CANCEL_BACK, o->ye - o->ya - E_KEYS_ASK_BUTTON_ROW_UP, -1,
              WPE_ESC, "Cancel", NULL, o);
 ret = e_opt_kst_message(o);
 freeostr(o);
 if (ret == AltM)
  return(WPE_STYLE_MODERN);
 return(WPE_STYLE_BORLAND);   /* Borland, or Esc: the editor makes its folder at every start, so it cannot ask twice */
}

/**
 * e_keys_first_run - Ask a new profile which keys it wants, and keep the answer.
 * @cn: the editor, its desk drawn.
 *
 * The answer is written to the user's option file at once, so the question is
 * asked once; the status row says which keys are in force and where to change
 * them.  Esc keeps the Borland keys.
 */
void e_keys_first_run(ECNT *cn)
{
 char line[E_KEYS_NOTICE_LEN];
 int style = e_keys_ask_box(cn->f[0]);

 cn->edopt = e_keys_preset(cn->edopt, style == WPE_STYLE_MODERN);
 e_switch_blst(cn);
 e_save_opt(cn->f[0]);
 snprintf(line, sizeof line, E_KEYS_CHOSEN_FMT,
          style == WPE_STYLE_MODERN ? "Modern" : "Borland");
 e_status_hold(line);
 e_status_paint(cn->fb, line);
 e_refresh();
}

int WpeReadGeneral(ECNT *cn, char *section, char *option, char *value)
{
 if (WpeStrccmp("Data", option) == 0)
  e_eol_set_default(cn, atoi(value));
 else if (WpeStrccmp("Autosave", option) == 0)
  cn->autosv = atoi(value);
 else if (WpeStrccmp("MaxColumn", option) == 0)
  cn->maxcol = atoi(value);
 else if (WpeStrccmp("Tab", option) == 0)
  cn->tabn = atoi(value);
 else if (WpeStrccmp("MaxChanges", option) == 0)
  cn->maxchg = atoi(value);
 else if (WpeStrccmp("NumUndo", option) == 0)
  cn->numundo = atoi(value);
 else if (WpeStrccmp("Options1", option) == 0)
  cn->flopt = atoi(value);
 else if (WpeStrccmp("Options2", option) == 0)
  cn->edopt = atoi(value);
 else if (WpeStrccmp("KeysNotice", option) == 0)
  e_keys_notice_seen = atoi(value) != 0;
 else if (WpeStrccmp("InfoDir", option) == 0)
  info_file = WpeStrdup(value);
 else if (WpeStrccmp("AutoIndent", option) == 0)
  cn->autoindent = atoi(value);
 else if (WpeStrccmp("PrintCmd", option) == 0)
  cn->print_cmd = WpeStrdup(value);
 else if (WpeStrccmp("Version", option) == 0)
 {
  sscanf(value, "%d.%d.%d", &cn->major, &cn->minor, &cn->patch);
 }
 return 0;
}

int WpeWriteGeneral(ECNT *cn, char *section, FILE *opt_file)
{
 fprintf(opt_file, "Version : %s\n", VERSION);
 fprintf(opt_file, "Data : %d\n", e_eol_data_key(cn));
 fprintf(opt_file, "Autosave : %d\n", cn->autosv);
 fprintf(opt_file, "MaxColumn : %d\n", cn->maxcol);
 fprintf(opt_file, "Tab : %d\n", cn->tabn);
 fprintf(opt_file, "MaxChanges : %d\n", cn->maxchg);
 fprintf(opt_file, "NumUndo : %d\n", cn->numundo);
 fprintf(opt_file, "Options1 : %d\n", cn->flopt);
 fprintf(opt_file, "Options2 : %d\n", e_keys_edopt_for_file(cn));
 if (e_keys_notice_seen)
  fprintf(opt_file, "KeysNotice : 1\n");
 fprintf(opt_file, "InfoDir : %s\n", info_file);
 fprintf(opt_file, "AutoIndent : %d\n", cn->autoindent);
 fprintf(opt_file, "PrintCmd : %s\n", cn->print_cmd);
 return 0;
}

int WpeReadColor(ECNT *cn, char *section, char *option, char *value)
{
 FARBE *fb = NULL;
 COLOR *c = NULL;
 int convert = 0; /* Convert old X11 colors to new colors */

 if (WpeStrccmp("Term", section + strlen(OPT_SECTION_COLOR) + 1) == 0)
 {
  if (!u_fb)
  {
   u_fb = WpeMalloc(sizeof(FARBE));
   FARBE_Init(u_fb);
  }
  fb = u_fb;
 }
 if (WpeStrccmp("X11", section + strlen(OPT_SECTION_COLOR) + 1) == 0)
 {
  if (!x_fb)
  {
   x_fb = WpeMalloc(sizeof(FARBE));
   FARBE_Init(x_fb);
  }
  fb = x_fb;
  if ((cn->major <= 1) && (cn->minor <= 5) && (cn->patch <= 27))
   convert = 1;
 }
 if (WpeStrccmp("er", option) == 0)
  c = &fb->er;
 else if (WpeStrccmp("es", option) == 0)
  c = &fb->es;
 else if (WpeStrccmp("et", option) == 0)
  c = &fb->et;
 else if (WpeStrccmp("ez", option) == 0)
  c = &fb->ez;
 else if (WpeStrccmp("ek", option) == 0)
  c = &fb->ek;
 else if (WpeStrccmp("em", option) == 0)
  c = &fb->em;
 else if (WpeStrccmp("hh", option) == 0)
  c = &fb->hh;
 else if (WpeStrccmp("hb", option) == 0)
  c = &fb->hb;
 else if (WpeStrccmp("hm", option) == 0)
  c = &fb->hm;
 else if (WpeStrccmp("db", option) == 0)
  c = &fb->db;
 else if (WpeStrccmp("dy", option) == 0)
  c = &fb->dy;
 else if (WpeStrccmp("mr", option) == 0)
  c = &fb->mr;
 else if (WpeStrccmp("ms", option) == 0)
  c = &fb->ms;
 else if (WpeStrccmp("mt", option) == 0)
  c = &fb->mt;
 else if (WpeStrccmp("mz", option) == 0)
  c = &fb->mz;
 else if (WpeStrccmp("df", option) == 0)
  c = &fb->df;
 else if (WpeStrccmp("nr", option) == 0)
  c = &fb->nr;
 else if (WpeStrccmp("ne", option) == 0)
  c = &fb->ne;
 else if (WpeStrccmp("nt", option) == 0)
  c = &fb->nt;
 else if (WpeStrccmp("nsnt", option) == 0)
  c = &fb->nsnt;
 else if (WpeStrccmp("fr", option) == 0)
  c = &fb->fr;
 else if (WpeStrccmp("fa", option) == 0)
  c = &fb->fa;
 else if (WpeStrccmp("ft", option) == 0)
  c = &fb->ft;
 else if (WpeStrccmp("fz", option) == 0)
  c = &fb->fz;
 else if (WpeStrccmp("frft", option) == 0)
  c = &fb->frft;
 else if (WpeStrccmp("fs", option) == 0)
  c = &fb->fs;
 else if (WpeStrccmp("nsft", option) == 0)
  c = &fb->nsft;
 else if (WpeStrccmp("fsm", option) == 0)
  c = &fb->fsm;
 else if (WpeStrccmp("nz", option) == 0)
  c = &fb->nz;
 else if (WpeStrccmp("ns", option) == 0)
  c = &fb->ns;
 else if (WpeStrccmp("nm", option) == 0)
  c = &fb->nm;
 else if (WpeStrccmp("of", option) == 0)
  c = &fb->of;
 else if (WpeStrccmp("ct", option) == 0)
  c = &fb->ct;
 else if (WpeStrccmp("cr", option) == 0)
  c = &fb->cr;
 else if (WpeStrccmp("ck", option) == 0)
  c = &fb->ck;
 else if (WpeStrccmp("cp", option) == 0)
  c = &fb->cp;
 else if (WpeStrccmp("cc", option) == 0)
  c = &fb->cc;
 else if (WpeStrccmp("aiu", option) == 0)
  c = &fb->aiu;
 else if (WpeStrccmp("aim", option) == 0)
  c = &fb->aim;
 else if (WpeStrccmp("aia", option) == 0)
  c = &fb->aia;
 else if (WpeStrccmp("aie", option) == 0)
  c = &fb->aie;
 else if (WpeStrccmp("du", option) == 0)
  c = &fb->du;
 else if (WpeStrccmp("dc", option) == 0)
  fb->dc = atoi(value);
 else if (WpeStrccmp("ws", option) == 0)
  fb->ws = atoi(value);
 if (c != NULL)
 {
  sscanf(value, "%d%d", &c->f, &c->b);
  if (convert)
  {
   switch (c->f)
   {
    case 1: c->f = 4; break;
    case 3: c->f = 6; break;
    case 4: c->f = 1; break;
    case 6: c->f = 3; break;
    case 9: c->f = 12; break;
    case 11: c->f = 14; break;
    case 12: c->f = 9; break;
    case 14: c->f = 11; break;
    default: break;
   }
   switch (c->b)
   {
    case 1: c->b = 4; break;
    case 3: c->b = 6; break;
    case 4: c->b = 1; break;
    case 6: c->b = 3; break;
    case 9: c->b = 12; break;
    case 11: c->b = 14; break;
    case 12: c->b = 9; break;
    case 14: c->b = 11; break;
    default: break;
   }
  }
  *c = e_s_clr(c->f, c->b);
 }
 return 0;
}

int WpeWriteColor(ECNT *cn, char *section, FILE *opt_file)
{
 FARBE *fb;

 if (WpeStrccmp("Term", section + strlen(OPT_SECTION_COLOR) + 1) == 0)
  fb = u_fb;
 if (WpeStrccmp("X11", section + strlen(OPT_SECTION_COLOR) + 1) == 0)
  fb = x_fb;
 fprintf(opt_file, "er : %d %d\n", fb->er.f, fb->er.b);
 fprintf(opt_file, "es : %d %d\n", fb->es.f, fb->es.b);
 fprintf(opt_file, "et : %d %d\n", fb->et.f, fb->et.b);
 fprintf(opt_file, "ez : %d %d\n", fb->ez.f, fb->ez.b);
 fprintf(opt_file, "ek : %d %d\n", fb->ek.f, fb->ek.b);
 fprintf(opt_file, "em : %d %d\n", fb->em.f, fb->em.b);
 fprintf(opt_file, "hh : %d %d\n", fb->hh.f, fb->hh.b);
 fprintf(opt_file, "hb : %d %d\n", fb->hb.f, fb->hb.b);
 fprintf(opt_file, "hm : %d %d\n", fb->hm.f, fb->hm.b);
 fprintf(opt_file, "db : %d %d\n", fb->db.f, fb->db.b);
 fprintf(opt_file, "dy : %d %d\n", fb->dy.f, fb->dy.b);
 fprintf(opt_file, "mr : %d %d\n", fb->mr.f, fb->mr.b);
 fprintf(opt_file, "ms : %d %d\n", fb->ms.f, fb->ms.b);
 fprintf(opt_file, "mt : %d %d\n", fb->mt.f, fb->mt.b);
 fprintf(opt_file, "mz : %d %d\n", fb->mz.f, fb->mz.b);
 fprintf(opt_file, "df : %d %d\n", fb->df.f, fb->df.b);
 fprintf(opt_file, "nr : %d %d\n", fb->nr.f, fb->nr.b);
 fprintf(opt_file, "ne : %d %d\n", fb->ne.f, fb->ne.b);
 fprintf(opt_file, "nt : %d %d\n", fb->nt.f, fb->nt.b);
 fprintf(opt_file, "nsnt : %d %d\n", fb->nsnt.f, fb->nsnt.b);
 fprintf(opt_file, "fr : %d %d\n", fb->fr.f, fb->fr.b);
 fprintf(opt_file, "fa : %d %d\n", fb->fa.f, fb->fa.b);
 fprintf(opt_file, "ft : %d %d\n", fb->ft.f, fb->ft.b);
 fprintf(opt_file, "fz : %d %d\n", fb->fz.f, fb->fz.b);
 fprintf(opt_file, "frft : %d %d\n", fb->frft.f, fb->frft.b);
 fprintf(opt_file, "fs : %d %d\n", fb->fs.f, fb->fs.b);
 fprintf(opt_file, "nsft : %d %d\n", fb->nsft.f, fb->nsft.b);
 fprintf(opt_file, "fsm : %d %d\n", fb->fsm.f, fb->fsm.b);
 fprintf(opt_file, "nz : %d %d\n", fb->nz.f, fb->nz.b);
 fprintf(opt_file, "ns : %d %d\n", fb->ns.f, fb->ns.b);
 fprintf(opt_file, "nm : %d %d\n", fb->nm.f, fb->nm.b);
 fprintf(opt_file, "of : %d %d\n", fb->of.f, fb->of.b);
 fprintf(opt_file, "ct : %d %d\n", fb->ct.f, fb->ct.b);
 fprintf(opt_file, "cr : %d %d\n", fb->cr.f, fb->cr.b);
 fprintf(opt_file, "ck : %d %d\n", fb->ck.f, fb->ck.b);
 fprintf(opt_file, "cp : %d %d\n", fb->cp.f, fb->cp.b);
 fprintf(opt_file, "cc : %d %d\n", fb->cc.f, fb->cc.b);
 fprintf(opt_file, "aiu : %d %d\n", fb->aiu.f, fb->aiu.b);
 fprintf(opt_file, "aim : %d %d\n", fb->aim.f, fb->aim.b);
 fprintf(opt_file, "aia : %d %d\n", fb->aia.f, fb->aia.b);
 fprintf(opt_file, "aie : %d %d\n", fb->aie.f, fb->aie.b);
 fprintf(opt_file, "du : %d %d\n", fb->du.f, fb->du.b);
 fprintf(opt_file, "dc : %d\n", fb->dc);
 fprintf(opt_file, "ws : %d\n", fb->ws);
 return 0;
}

#ifdef WPE_AI
/* Parse an AIProvider config value "name|endpoint|model|cafile" (model/cafile
 * optional) and register the profile.  Splits a private copy on '|'. */
static void e_ai_read_provider(const char *value)
{
 char buf[1024], *f[4], *p;
 int nf = 0;
 snprintf(buf, sizeof buf, "%s", value ? value : "");
 for (p = buf; nf < 4; ) {
  f[nf++] = p;
  p = strchr(p, '|');
  if (!p) break;
  *p++ = '\0';
 }
 if (nf >= 2 && f[0][0])
  wpe_ai_provider_set(f[0], f[1], nf >= 3 ? f[2] : "", nf >= 4 ? f[3] : "");
}
#endif

int WpeReadProgramming(ECNT *cn, char *section, char *option, char *value)
{
 if (WpeStrccmp("Arguments", option) == 0)
  e_prog.arguments = WpeStrdup(value);
 else if (WpeStrccmp("Project", option) == 0)
  e_prog.project = WpeStrdup(value);
 else if (WpeStrccmp("Exedir", option) == 0)
  e_prog.exedir = WpeStrdup(value);
 else if (WpeStrccmp("IncludePath", option) == 0)
  e_prog.sys_include = WpeStrdup(value);
 else if (WpeStrccmp("Debugger", option) == 0)
  e_deb_type = atoi(value);
 else if (WpeStrccmp("DAPAdapter", option) == 0)
  e_dap_adapter = atoi(value);
#ifdef WPE_AI
 else if (WpeStrccmp("AIBackend", option) == 0)
  e_ai_backend = atoi(value);
 else if (WpeStrccmp("AIEndpoint", option) == 0) {
  free(e_ai_endpoint);
  e_ai_endpoint = WpeStrdup(value);
 } else if (WpeStrccmp("AIModel", option) == 0) {
  free(e_ai_model);
  e_ai_model = WpeStrdup(value);
 } else if (WpeStrccmp("AIModelFallback", option) == 0) {
  free(e_ai_model_fallback);
  e_ai_model_fallback = (value && *value) ? WpeStrdup(value) : NULL;
 } else if (WpeStrccmp("AIEditHook", option) == 0) {
  free(e_ai_edit_hook);
  e_ai_edit_hook = (value && *value) ? WpeStrdup(value) : NULL;
 } else if (WpeStrccmp("AIHostCommand", option) == 0) {
  free(e_ai_host_command);
  e_ai_host_command = (value && *value) ? WpeStrdup(value) : NULL;
 } else if (WpeStrccmp("AIReadRoots", option) == 0) {
  free(e_ai_read_roots);
  e_ai_read_roots = (value && *value) ? WpeStrdup(value) : NULL;
 } else if (WpeStrccmp("AIHostAdapter", option) == 0) {
  e_ai_host_adapter = (WpeStrccmp(value, "text") == 0) ? WPE_AI_HOST_ADAPTER_TEXT
                                                       : WPE_AI_HOST_ADAPTER_CLAUDE;
 } else if (WpeStrccmp("AICAFile", option) == 0) {
  free(e_ai_cafile);
  e_ai_cafile = (value && *value) ? WpeStrdup(value) : NULL;
 } else if (WpeStrccmp("AISearchURL", option) == 0) {
  free(e_ai_search_url);
  e_ai_search_url = (value && *value) ? WpeStrdup(value) : NULL;
 } else if (WpeStrccmp("AIToolTimeout", option) == 0) {
  e_ai_tool_timeout = wpe_ai_tool_timeout_from_text(value);
 } else if (WpeStrccmp("AIAnswerTimeout", option) == 0) {
  e_ai_answer_timeout = wpe_ai_answer_timeout_from_text(value);
 } else if (WpeStrccmp("AIProjectInstructions", option) == 0) {
  e_ai_project_scope = wpe_ai_project_scope_from_name(value);
 } else if (WpeStrccmp("AIProjectInstructionsMax", option) == 0) {
  e_ai_project_max = wpe_ai_project_max_from_text(value);
 } else if (WpeStrccmp("AIProviderName", option) == 0) {
  free(e_ai_provider);
  e_ai_provider = (value && *value) ? WpeStrdup(value) : NULL;
 } else if (WpeStrccmp("AIProvider", option) == 0) {
  e_ai_read_provider(value);
 } else if (WpeStrccmp("AIPolicy", option) == 0)
  e_ai_policy = wpe_ai_policy_from_name(value);
 else if (WpeStrccmp("AIWeb", option) == 0)
  e_ai_web = wpe_ai_web_from_name(value);
#ifdef WPE_AI_AGENT_HOST
 else if (WpeStrccmp("AIAgentEngine", option) == 0)
  e_ai_agent_engine = (WpeStrccmp(value, "claude-code") == 0)
                        ? WPE_AI_ENGINE_CLAUDE_HOST : WPE_AI_ENGINE_BUILTIN;
#endif
#endif
 return 0;
}

int WpeWriteProgramming(ECNT *cn, char *section, FILE *opt_file)
{
 fprintf(opt_file, "Arguments : %s\n", e_prog.arguments);
 fprintf(opt_file, "Project : %s\n", e_prog.project);
 fprintf(opt_file, "Exedir : %s\n", e_prog.exedir);
 fprintf(opt_file, "IncludePath : %s\n", e_prog.sys_include);
 fprintf(opt_file, "Debugger : %d\n", e_deb_type);
 fprintf(opt_file, "DAPAdapter : %d\n", e_dap_adapter);
#ifdef WPE_AI
 /* The keys an XWPE_AI_* variable can shadow are written through the config
    layer, which answers with the file's own value while the environment still
    overrides it: a one-session override never lands in xwperc. */
 fprintf(opt_file, "AIBackend : %d\n", wpe_ai_cfg_int_to_save(WPE_AI_CFG_BACKEND));
 fprintf(opt_file, "AIEndpoint : %s\n", wpe_ai_cfg_str_to_save(WPE_AI_CFG_ENDPOINT));
 fprintf(opt_file, "AIModel : %s\n", wpe_ai_cfg_str_to_save(WPE_AI_CFG_MODEL));
 if (e_ai_model_fallback && *e_ai_model_fallback)
  fprintf(opt_file, "AIModelFallback : %s\n", e_ai_model_fallback);
 if (e_ai_edit_hook && *e_ai_edit_hook)
  fprintf(opt_file, "AIEditHook : %s\n", e_ai_edit_hook);
 if (e_ai_host_command && *e_ai_host_command)
  fprintf(opt_file, "AIHostCommand : %s\n", e_ai_host_command);
 /* Roots the user wrote by hand are kept when the file is rewritten; the
    editor never adds one itself. */
 if (e_ai_read_roots && *e_ai_read_roots)
  fprintf(opt_file, "AIReadRoots : %s\n", e_ai_read_roots);
 if (wpe_ai_cfg_int_to_save(WPE_AI_CFG_HOST_ADAPTER) == WPE_AI_HOST_ADAPTER_TEXT)
  fprintf(opt_file, "AIHostAdapter : text\n");
 fprintf(opt_file, "AICAFile : %s\n", wpe_ai_cfg_str_to_save(WPE_AI_CFG_CAFILE));
 if (*wpe_ai_cfg_str_to_save(WPE_AI_CFG_SEARCH_URL))
  fprintf(opt_file, "AISearchURL : %s\n", wpe_ai_cfg_str_to_save(WPE_AI_CFG_SEARCH_URL));
 /* The tool time limit is written only when the user set one, so an xwperc
    that never named it stays as it was. */
 if (wpe_ai_cfg_int_to_save(WPE_AI_CFG_TOOL_TIMEOUT) != WPE_AI_TOOL_TIMEOUT_DEFAULT)
  fprintf(opt_file, "AIToolTimeout : %d\n", wpe_ai_cfg_int_to_save(WPE_AI_CFG_TOOL_TIMEOUT));
 /* The wait for a model's first byte follows the same rule: written only when
    the user moved it off the default, so an xwperc that never named it stays
    as it was. */
 if (wpe_ai_cfg_int_to_save(WPE_AI_CFG_ANSWER_TIMEOUT) != WPE_AI_ANSWER_TIMEOUT_DEFAULT)
  fprintf(opt_file, "AIAnswerTimeout : %d\n",
          wpe_ai_cfg_int_to_save(WPE_AI_CFG_ANSWER_TIMEOUT));
 /* The same for where the project instructions go and how much of the file the
    prompt carries: written only when the user moved them off the defaults. */
 if (wpe_ai_cfg_int_to_save(WPE_AI_CFG_PROJECT_SCOPE) != WPE_AI_PROJECT_AGENT)
  fprintf(opt_file, "AIProjectInstructions : %s\n",
          wpe_ai_project_scope_name(wpe_ai_cfg_int_to_save(WPE_AI_CFG_PROJECT_SCOPE)));
 if (wpe_ai_cfg_int_to_save(WPE_AI_CFG_PROJECT_MAX) != WPE_AI_PROJECT_MAX_DEFAULT)
  fprintf(opt_file, "AIProjectInstructionsMax : %d\n", wpe_ai_cfg_int_to_save(WPE_AI_CFG_PROJECT_MAX));
 fprintf(opt_file, "AIProviderName : %s\n", e_ai_provider ? e_ai_provider : "");
 {
  int i, np = wpe_ai_provider_count();
  for (i = 0; i < np; i++) {
   const struct wpe_ai_provider *p = wpe_ai_provider_get(i);
   fprintf(opt_file, "AIProvider : %s|%s|%s|%s\n", p->name, p->endpoint,
           p->model ? p->model : "", p->cafile ? p->cafile : "");
  }
 }
 fprintf(opt_file, "AIPolicy : %s\n",
         wpe_ai_policy_name(wpe_ai_cfg_int_to_save(WPE_AI_CFG_POLICY)));
 /* The web switch is written only once it has been answered (the notice's y,
    or Options > AI), so an xwperc that never named it keeps asking; and it
    goes through the config layer like every other overridable key, so a
    one-session XWPE_AI_WEB never becomes configuration on its own. */
 if (wpe_ai_cfg_int_to_save(WPE_AI_CFG_WEB) != WPE_AI_WEB_UNSET)
  fprintf(opt_file, "AIWeb : %s\n",
          wpe_ai_web_name(wpe_ai_cfg_int_to_save(WPE_AI_CFG_WEB)));
#ifdef WPE_AI_AGENT_HOST
 fprintf(opt_file, "AIAgentEngine : %s\n",
         wpe_ai_cfg_int_to_save(WPE_AI_CFG_AGENT_ENGINE) == WPE_AI_ENGINE_CLAUDE_HOST
           ? "claude-code" : "builtin");
#endif
#endif
 return 0;
}

/** The key a language's entry names what Ctrl-F9 starts with. */
#define E_OPT_KEY_RUN "Run"
/** The room of a CompilerSwitch value written as a number. */
#define E_OPT_SWITCH_ROOM 16

/**
 * e_opt_take_run_kind - Take the "Run" value of a language's entry.
 * @kind: the entry's kind; @value: the file's word.
 *
 * A word that names no kind leaves the entry as it was, so a misspelt key
 * never turns a language into something else.
 */
static void e_opt_take_run_kind(int *kind, const char *value)
{
 int named = e_run_kind_parse(value);

 if (named != E_RUN_KIND_UNSET)
  *kind = named;
}

/**
 * e_opt_decide_kind - Give an entry that names no Run the kind its style
 * says.
 * @p: the entry.
 */
static void e_opt_decide_kind(struct e_s_prog *p)
{
 if (p->run_kind == E_RUN_KIND_UNSET)
  p->run_kind = e_run_kind_default(p);
}

/**
 * e_opt_lang_finish - Decide the Run kind of every entry a file created.
 *
 * Called once an option file has been read to its end, when every key of
 * each language section is known: a language the editor does not build in
 * (a user's own Lua) that names no Run runs as it did before the key
 * existed, an interpreter when its CompilerSwitch is 1 and its program
 * otherwise.  An entry the editor made keeps its own kind unless the file
 * says Run: Options > Save writes CompilerSwitch for every language and no
 * Run, and must not turn Rust or Java into interpreters.
 */
static void e_opt_lang_finish(void)
{
 int i;

 for (i = 0; i < e_prog.num; i++)
  e_opt_decide_kind(e_prog.comp[i]);
 for (i = 0; i < e_opt_own_nlang; i++)
  e_opt_decide_kind(e_opt_own_lang[i]);
}

int WpeReadLanguage(ECNT *cn, char *section, char *option, char *value)
{
 int i, j;
 char *strtmp;

 for (i = 0;
   (i < e_prog.num) && (WpeStrccmp(e_prog.comp[i]->language, section + strlen(OPT_SECTION_LANGUAGE) + 1) != 0);
   i++)
  ;
 if (i == e_prog.num)
 {
  e_prog.num++;
  e_prog.comp = REALLOC(e_prog.comp, e_prog.num * sizeof(struct e_s_prog *));
  e_prog.comp[i] = calloc(1, sizeof(struct e_s_prog));
  e_prog.comp[i]->language = WpeStrdup(section + strlen(OPT_SECTION_LANGUAGE) + 1);
  e_prog.comp[i]->compiler = WpeStrdup("");
  e_prog.comp[i]->comp_str = WpeStrdup("");
  e_prog.comp[i]->libraries = WpeStrdup("");
  e_prog.comp[i]->exe_name = WpeStrdup("");
  e_prog.comp[i]->filepostfix = (char **)WpeExpArrayCreate(0, sizeof(char *), 1);
  e_prog.comp[i]->intstr = WpeStrdup("");
  e_prog.comp[i]->start_symbol = NULL;
  e_prog.comp[i]->key = '\0';
  e_prog.comp[i]->comp_sw = 0;
  e_prog.comp[i]->run_kind = E_RUN_KIND_UNSET;   /* decided when the file is read to its end */
  e_prog.comp[i]->x = 0;
 }
 if (WpeStrccmp("Compiler", option) == 0)
 {
  if (e_prog.comp[i]->compiler)
   FREE(e_prog.comp[i]->compiler);
  e_prog.comp[i]->compiler = WpeStrdup(value);
 }
 else if (WpeStrccmp("CompilerOptions", option) == 0)
 {
  if (e_prog.comp[i]->comp_str)
   FREE(e_prog.comp[i]->comp_str);
  e_prog.comp[i]->comp_str = WpeStrdup(value);
 }
 else if (WpeStrccmp("Libraries", option) == 0)
 {
  if (e_prog.comp[i]->libraries)
   FREE(e_prog.comp[i]->libraries);
  e_prog.comp[i]->libraries = WpeStrdup(value);
 }
 else if (WpeStrccmp("Executable", option) == 0)
 {
  if (e_prog.comp[i]->exe_name)
   FREE(e_prog.comp[i]->exe_name);
  e_prog.comp[i]->exe_name = WpeStrdup(value);
 }
 else if (WpeStrccmp("FileExtension", option) == 0)
 {
  for (j = WpeExpArrayGetSize(e_prog.comp[i]->filepostfix); j; j--)
   if (strcmp(e_prog.comp[i]->filepostfix[j - 1], value) == 0)
    break;
  if (j == 0)
  {
   strtmp = WpeStrdup(value);
   WpeExpArrayAdd((void **)&e_prog.comp[i]->filepostfix, &strtmp);
  }
 }
 else if (WpeStrccmp("MessageString", option) == 0)
 {
  if (e_prog.comp[i]->intstr)
   FREE(e_prog.comp[i]->intstr);
  e_prog.comp[i]->intstr = WpeValueToString(value);
 }
 else if (WpeStrccmp("Key", option) == 0)
  e_prog.comp[i]->key = value[0];
 else if (WpeStrccmp("StartSymbol", option) == 0)
 {
  if (e_prog.comp[i]->start_symbol)
   FREE(e_prog.comp[i]->start_symbol);
  e_prog.comp[i]->start_symbol = WpeStrdup(value);
 }
 else if (WpeStrccmp("CompilerSwitch", option) == 0)
  e_prog.comp[i]->comp_sw = atoi(value);
 else if (WpeStrccmp(E_OPT_KEY_RUN, option) == 0)
  e_opt_take_run_kind(&e_prog.comp[i]->run_kind, value);
 else if (WpeStrccmp("X", option) == 0)
  e_prog.comp[i]->x = atoi(value);
 return 0;
}

int WpeWriteLanguage(ECNT *cn, char *section, FILE *opt_file)
{
 int i, j;
 char *str_tmp;

 for (i = 0;
   (i < e_prog.num) && (WpeStrccmp(e_prog.comp[i]->language, section + strlen(OPT_SECTION_LANGUAGE) + 1) != 0);
   i++)
  ;
 if (i < e_prog.num)
 {
  fprintf(opt_file, "Compiler : %s\n", e_prog.comp[i]->compiler);
  fprintf(opt_file, "CompilerOptions : %s\n", e_prog.comp[i]->comp_str);
  fprintf(opt_file, "Libraries : %s\n", e_prog.comp[i]->libraries);
  fprintf(opt_file, "Executable : %s\n", e_prog.comp[i]->exe_name);
  for (j = WpeExpArrayGetSize(e_prog.comp[i]->filepostfix); j; j--)
   fprintf(opt_file, "FileExtension : %s\n", e_prog.comp[i]->filepostfix[j - 1]);
  str_tmp = WpeStringToValue(e_prog.comp[i]->intstr);
  fprintf(opt_file, "MessageString : %s\n", str_tmp);
  WpeFree(str_tmp);
  if (e_prog.comp[i]->start_symbol && e_prog.comp[i]->start_symbol[0])
   fprintf(opt_file, "StartSymbol : %s\n", e_prog.comp[i]->start_symbol);
  fprintf(opt_file, "CompilerSwitch : %d\n", e_prog.comp[i]->comp_sw);
  fprintf(opt_file, E_OPT_KEY_RUN " : %s\n", e_run_kind_name(e_run_kind_of(e_prog.comp[i])));
  fprintf(opt_file, "Key : %c\n", e_prog.comp[i]->key);
  fprintf(opt_file, "X : %d\n", e_prog.comp[i]->x);
 }
 return 0;
}

static void e_opt_move_user_only_keys(ECNT *cn);

/**
 * e_opt_file_missing - Is there no option file to read or write?
 * @cn: the editor.
 *
 * The only case is a configuration folder whose path is longer than the
 * system allows ($XDG_CONFIG_HOME of a deep folder): the editor runs with
 * its defaults rather than create and fill a folder cut from the one the
 * user named.
 * Return: 1 when there is none.
 */
int e_opt_file_missing(ECNT *cn)
{
 return(!cn || !cn->optfile || !cn->optfile[0]);
}

/**
 * e_opt_say_file_missing - Say on the status row why the options are not
 * read or not saved, when there is no option file.
 * @cn: the editor; @what: E_OPT_NOT_READ or E_OPT_NOT_SAVED.
 *
 * Without it the user would find his options reset, or his changes gone,
 * with nothing telling him that the folder $XDG_CONFIG_HOME names is the
 * reason.
 * Return: 1 when it spoke (there is no option file), else 0.
 */
int e_opt_say_file_missing(ECNT *cn, const char *what)
{
 char sentence[WPE_PATH_MSG_MAX], line[WPE_PATH_MSG_MAX + 32];

 if (!e_opt_file_missing(cn)) return(0);
 if (!cn || !cn->fb) return(1);
 snprintf(sentence, sizeof sentence, WPE_PATH_TOO_LONG_FMT,
          "configuration folder", WPE_PATH_MAX);
 snprintf(line, sizeof line, "%s: %s", what, sentence);
 e_status_hold(line);
 e_status_paint(cn->fb, line);
 e_refresh();
 return(1);
}

/*   save options    */
int e_save_opt(FENSTER *f)
{
 ECNT *cn = f->ed;
 FILE *fp;
 int i;
 char *str_line;

 if (e_opt_say_file_missing(cn, E_OPT_NOT_SAVED))
  return(-1);
 str_line = MALLOC((strlen(cn->optfile)+1)*sizeof(char));
 strcpy(str_line, cn->optfile);
 for (i = strlen(str_line); i > 0 && str_line[i] != DIRC; i--);
 str_line[i] = '\0';
 if (access(str_line, 0)) mkdir(str_line, 0700);
 FREE(str_line);
 fp = fopen(cn->optfile, "w");
 if (fp == NULL)
 {
  e_error(e_msg[ERR_OPEN_OPF], 0, f->fb);
  return(-1);
 }
 str_line = OPT_SECTION_GENERAL;
 fprintf(fp, "[%s]\n", str_line);
 WpeWriteGeneral(cn, str_line, fp);
 str_line = MALLOC(strlen(OPT_SECTION_COLOR) + 10);
 strcpy(str_line, OPT_SECTION_COLOR);
 if (u_fb)
 {
  strcat(str_line, "/Term");
  fprintf(fp, "[%s]\n", str_line);
  WpeWriteColor(cn, str_line, fp);
 }
 if (x_fb)
 {
  strcpy(str_line + strlen(OPT_SECTION_COLOR), "/X11");
  fprintf(fp, "[%s]\n", str_line);
  WpeWriteColor(cn, str_line, fp);
 }
 FREE(str_line);
 str_line = OPT_SECTION_PROGRAMMING;
 fprintf(fp, "[%s]\n", str_line);
 WpeWriteProgramming(cn, str_line, fp);
 str_line = MALLOC(strlen(OPT_SECTION_LANGUAGE) + 2);
 strcpy(str_line, OPT_SECTION_LANGUAGE);
 strcat(str_line, "/");
 for (i = 0; i < e_prog.num; i++)
 {
  str_line = REALLOC(str_line, strlen(OPT_SECTION_LANGUAGE) + strlen(e_prog.comp[i]->language) + 2);
  strcpy(str_line + strlen(OPT_SECTION_LANGUAGE) + 1, e_prog.comp[i]->language);
  fprintf(fp, "[%s]\n", str_line);
  WpeWriteLanguage(cn, str_line, fp);
 }
 FREE(str_line);
 fclose(fp);
 e_opt_move_user_only_keys(cn);
 return 0;
}

/**
 * e_opt_key_is_user_only - Is this a key only the user's own config may set?
 * @section: the section the key is in; @option: the key's name.
 * Return: 1 when the key is in e_opt_user_only_keys.
 */
static int e_opt_key_is_user_only(const char *section, const char *option)
{
 int i;
 if (!section || !option) return 0;
 for (i = 0; e_opt_user_only_keys[i].key; i++)
 {
  if (strncmp(e_opt_user_only_keys[i].section, section,
              strlen(e_opt_user_only_keys[i].section)) != 0)
   continue;
  if (WpeStrccmp(e_opt_user_only_keys[i].key, option) == 0)
   return 1;
 }
 return 0;
}

/**
 * e_opt_note_ignored_key - Remember a user-only key the start-folder xwperc set.
 * @option: the key's name, as the file wrote it.
 *
 * Each name is kept once, in the order the file names them, so the status
 * line the user reads lists every key that did not take effect, and no key
 * twice.
 */
static void e_opt_note_ignored_key(const char *option)
{
 size_t used = strlen(e_opt_ignored_keys), len = strlen(option);
 const char *p = e_opt_ignored_keys;
 while ((p = strstr(p, option)) != NULL)
 {
  if ((p == e_opt_ignored_keys || p[-1] == ' ') && (p[len] == '\0' || p[len] == ' '))
   return;
  p += len;
 }
 if (used + len + 2 >= sizeof e_opt_ignored_keys) return;
 if (used) strcat(e_opt_ignored_keys, " ");
 strcat(e_opt_ignored_keys, option);
}

/**
 * e_opt_line_applies - Does this key of this file take effect?
 * @section/@option: the key; @mode: how the file is read (E_OPT_READ_*).
 *
 * A start-folder xwperc keeps every ordinary key (tabs, colours, the
 * compiler of a language) and loses the user-only ones, which are noted for
 * the status line; the user's own file, read after it, gives only those.
 * Return: 1 when the key is to be applied.
 */
static int e_opt_line_applies(const char *section, const char *option, int mode)
{
 int user_only;
 if (mode == E_OPT_READ_ALL) return 1;
 user_only = e_opt_key_is_user_only(section, option);
 if (mode == E_OPT_READ_USER_ONLY) return user_only;
 if (user_only) e_opt_note_ignored_key(option);
 return !user_only;
}

/**
 * e_opt_same_file - Are two names the same file on disk?
 * @a/@b: the names.
 * Return: 1 when both exist and are one file.
 */
static int e_opt_same_file(const char *a, const char *b)
{
 struct stat sa, sb;
 if (!a || !b || stat(a, &sa) != 0 || stat(b, &sb) != 0) return 0;
 return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/**
 * e_opt_use_user_file - Name the user's own config, read after a start-folder xwperc.
 * @path: the user's own config file (malloc'd, taken), or NULL.
 *
 * Called when the editor found an xwperc in the folder it was started in and
 * reads its options from there: that file sets everything it names except
 * the user-only keys, and those come from @path, so the user's own hook,
 * engine, dial and endpoints follow him into every folder.  When the start
 * folder is the folder of the user's own file, the two are one file and it
 * is read whole.
 */
void e_opt_use_user_file(char *path)
{
 if (e_opt_user_file) FREE(e_opt_user_file);
 e_opt_user_file = path;
}

/**
 * e_opt_forget_folder - The options no longer come from a start-folder xwperc.
 */
static void e_opt_forget_folder(void)
{
 if (e_opt_folder_file) FREE(e_opt_folder_file);
 if (e_opt_folder_dir) FREE(e_opt_folder_dir);
 e_opt_folder_file = NULL;
 e_opt_folder_dir = NULL;
}

/**
 * e_opt_note_folder - Remember which start-folder xwperc the options come from.
 * @cn: the editor, whose option file is that xwperc.
 *
 * Its folder is the one the answers about its commands are kept for, and
 * only a save into that very file moves the user's own keys out of it.
 */
static void e_opt_note_folder(ECNT *cn)
{
 char *slash;

 e_opt_forget_folder();
 e_opt_folder_file = WpeStrdup(cn->optfile);
 e_opt_folder_dir = WpeStrdup(cn->optfile);
 slash = strrchr(e_opt_folder_dir, DIRC);
 if (slash == e_opt_folder_dir) slash[1] = '\0';
 else if (slash) *slash = '\0';
 else strcpy(e_opt_folder_dir, ".");
}

/* The folder the editor was started in, as it was at start. */
static char *e_opt_launch;

/**
 * e_opt_note_launch_dir - Remember the folder the editor was started in.
 * @dir: that folder.
 *
 * The editor reads ./xwperc there and the assistant its instructions; the
 * working folder changes for a while during each build, so the judge of
 * the assistant's writes asks this one instead.
 */
void e_opt_note_launch_dir(const char *dir)
{
 if (e_opt_launch) FREE(e_opt_launch);
 e_opt_launch = dir ? WpeStrdup(dir) : NULL;
}

/**
 * e_opt_launch_dir - The folder the editor was started in, or NULL.
 */
const char *e_opt_launch_dir(void)
{
 return(e_opt_launch);
}

/**
 * e_opt_start_folder - The folder whose xwperc the options come from.
 *
 * The run sites of the program keys ask about that folder's commands; when
 * the options come from the user's own file there is nothing to ask.
 * Return: the folder, or NULL.
 */
const char *e_opt_start_folder(void)
{
 return(e_opt_folder_dir);
}

/** e_opt_own_free_lang - Free the user's own language entries. */
static void e_opt_own_free_lang(void)
{
 int i;

 for (i = 0; i < e_opt_own_nlang; i++)
 {
  struct e_s_prog *p = e_opt_own_lang[i];
  if (p->language) FREE(p->language);
  if (p->compiler) FREE(p->compiler);
  if (p->comp_str) FREE(p->comp_str);
  if (p->libraries) FREE(p->libraries);
  if (p->exe_name) FREE(p->exe_name);
  FREE(p);
 }
 if (e_opt_own_lang) FREE(e_opt_own_lang);
 e_opt_own_lang = NULL;
 e_opt_own_nlang = 0;
}

/**
 * e_opt_own_find_lang - The user's own entry for a language.
 * @language: its name; @create: 1 to add an empty one when there is none.
 * Return: the entry, or NULL.
 */
static struct e_s_prog *e_opt_own_find_lang(const char *language, int create)
{
 struct e_s_prog *p, **grown;
 int i;

 for (i = 0; i < e_opt_own_nlang; i++)
  if (WpeStrccmp(e_opt_own_lang[i]->language, language) == 0)
   return(e_opt_own_lang[i]);
 if (!create)
  return(NULL);
 if (!(p = calloc(1, sizeof *p)))
  return(NULL);
 if (!(grown = realloc(e_opt_own_lang, (e_opt_own_nlang + 1) * sizeof *grown)))
 {
  free(p);
  return(NULL);
 }
 e_opt_own_lang = grown;
 p->language = WpeStrdup(language);
 p->compiler = WpeStrdup("");
 p->comp_str = WpeStrdup("");
 p->libraries = WpeStrdup("");
 p->exe_name = WpeStrdup("");
 p->run_kind = E_RUN_KIND_UNSET;   /* calloc's 0 is no kind by accident */
 e_opt_own_lang[e_opt_own_nlang++] = p;
 return(p);
}

/**
 * e_opt_own_set - Replace one field of an own entry.
 * @field: the field; @value: its new text.
 */
static void e_opt_own_set(char **field, const char *value)
{
 if (*field) FREE(*field);
 *field = WpeStrdup(value);
}

/**
 * e_opt_compiler_key - The answers' key for a language's compile line.
 * @language: its name; @out/@sz: receive "Compiler/<language>".
 */
static void e_opt_compiler_key(const char *language, char *out, size_t sz)
{
 snprintf(out, sz, "%s%s", WPE_STARTCMD_KEY_COMPILER, language);
}

/**
 * e_opt_note_own_lang - Count a language's compile line as the user's own.
 * @p: the entry.
 */
static void e_opt_note_own_lang(const struct e_s_prog *p)
{
 char key[WPE_STARTCMD_KEY_MAX];
 char *text = e_prog_command_text(p);

 e_opt_compiler_key(p->language, key, sizeof key);
 if (text)
  wpe_startcmd_note_own(key, text);
 if (text) FREE(text);
}

/**
 * e_opt_own_field_key - The answers' key of one value the user counts as his.
 * @scope: a language's name, or E_OPT_SCOPE_PROGRAMMING; @key: the key;
 * @out/@sz: receive "own/<scope>/<key>".
 */
void e_opt_own_field_key(const char *scope, const char *key, char *out, size_t sz)
{
 snprintf(out, sz, E_OPT_OWN_FIELD_FMT, scope, key);
}

/**
 * e_opt_note_own_field - Count one value as the user's own for its key.
 * @scope/@key: as e_opt_own_field_key; @value: the value.
 */
static void e_opt_note_own_field(const char *scope, const char *key, const char *value)
{
 char field[WPE_STARTCMD_KEY_MAX];

 if (!value)
  return;
 e_opt_own_field_key(scope, key, field, sizeof field);
 wpe_startcmd_note_own(field, value);
}

/**
 * e_opt_note_own_switch_and_kind - Count a language's compiler style and
 * Run kind as the user's own.
 * @p: the entry.
 *
 * They decide which file Ctrl-F9 starts, so a folder's xwperc that sets
 * either to another value is asked about, and one that repeats his own (or
 * the built-in) value is not.  A kind not yet decided is not noted: the
 * file is still being read.
 */
static void e_opt_note_own_switch_and_kind(const struct e_s_prog *p)
{
 char text[E_OPT_SWITCH_ROOM];

 snprintf(text, sizeof text, "%d", p->comp_sw);
 e_opt_note_own_field(p->language, "CompilerSwitch", text);
 if (p->run_kind != E_RUN_KIND_UNSET)
  e_opt_note_own_field(p->language, E_OPT_KEY_RUN, e_run_kind_name(p->run_kind));
}

/**
 * e_opt_note_own_lang_fields - Count each value of a language's line as his.
 * @p: the entry.
 *
 * A start plan asks about each value a folder sets on its own: a folder that
 * sets only the compiler's options is asked about the options, and a folder
 * whose compiler is his is not asked about the compiler.
 */
static void e_opt_note_own_lang_fields(const struct e_s_prog *p)
{
 e_opt_note_own_field(p->language, "Compiler", p->compiler);
 e_opt_note_own_field(p->language, "CompilerOptions", p->comp_str);
 e_opt_note_own_field(p->language, "Libraries", p->libraries);
 e_opt_note_own_field(p->language, "Executable", p->exe_name);
 e_opt_note_own_switch_and_kind(p);
}

/**
 * e_opt_own_prog_index - The place of a [Programming] key a plan compares.
 * Return: its index in e_opt_own_prog_keys, or -1.
 */
static int e_opt_own_prog_index(const char *key)
{
 int i;

 for (i = 0; e_opt_own_prog_keys[i]; i++)
  if (WpeStrccmp(e_opt_own_prog_keys[i], key) == 0)
   return(i);
 return(-1);
}

/**
 * e_opt_own_prog_set - Keep the user's own value of a [Programming] key.
 * @key: the key; @value: his value.
 */
static void e_opt_own_prog_set(const char *key, const char *value)
{
 int i = e_opt_own_prog_index(key);

 if (i < 0)
  return;
 if (e_opt_own_prog[i]) FREE(e_opt_own_prog[i]);
 if (!value)
  value = "";
 e_opt_own_prog[i] = WpeStrdup(value);
}

/**
 * e_opt_own_prog_from_defaults - Start the [Programming] keys a plan compares
 * from the built-in values, each counted as the user's own.
 */
static void e_opt_own_prog_from_defaults(void)
{
 e_opt_own_prog_set("Exedir", e_prog.exedir);
 e_opt_own_prog_set("Arguments", e_prog.arguments);
 e_opt_own_prog_set("Project", e_prog.project);
 e_opt_own_prog_set("IncludePath", e_prog.sys_include);
}

/** e_opt_note_own_prog - Count the user's own [Programming] values as his. */
static void e_opt_note_own_prog(void)
{
 int i;

 for (i = 0; e_opt_own_prog_keys[i]; i++)
  e_opt_note_own_field(E_OPT_SCOPE_PROGRAMMING, e_opt_own_prog_keys[i], e_opt_own_prog[i]);
}

/**
 * e_opt_own_value - The user's own value of a [Programming] key.
 * @key: "Exedir", "Arguments", "Project" or "IncludePath".
 *
 * "Use mine" starts the action with these.
 * Return: his own value (or the built-in one) while a start-folder xwperc
 * sets the one in force, else the one in force, which is his; "" for a key
 * this list does not hold.
 */
const char *e_opt_own_value(const char *key)
{
 int i = e_opt_own_prog_index(key);

 if (i < 0)
  return("");
 if (e_opt_folder_dir && e_opt_own_prog[i])
  return(e_opt_own_prog[i]);
 if (WpeStrccmp(key, "Exedir") == 0) return(e_prog.exedir ? e_prog.exedir : "");
 if (WpeStrccmp(key, "Arguments") == 0) return(e_prog.arguments ? e_prog.arguments : "");
 if (WpeStrccmp(key, "Project") == 0) return(e_prog.project ? e_prog.project : "");
 return(e_prog.sys_include ? e_prog.sys_include : "");
}

/**
 * e_opt_value_from - Where a value in force came from, for a start plan.
 * @scope/@key: as e_opt_own_field_key; @value: the value in force.
 *
 * A value counts as the folder's only when a start-folder xwperc gives the
 * options, the value is not empty, and it is neither the user's own, nor
 * the built-in one, nor one he typed in Options this session: a saved
 * config in a folder that repeats the defaults asks nothing.
 * Return: WPE_FROM_FOLDER or WPE_FROM_USER.
 */
int e_opt_value_from(const char *scope, const char *key, const char *value)
{
 if (!e_opt_folder_dir || !value || !*value)
  return(WPE_FROM_USER);
 if (e_opt_field_is_his(scope, key, value, e_opt_folder_dir))
  return(WPE_FROM_USER);
 return(WPE_FROM_FOLDER);
}

/**
 * e_opt_field_is_his - Is a value one the user counts as his own?
 * @scope/@key: as e_opt_own_field_key; @value: the value; @folder: the
 * folder whose file holds it, or NULL.
 * Return: 1 for his own value, the built-in one, one he typed this
 * session, or one he typed in that folder before (kept in the store).
 */
int e_opt_field_is_his(const char *scope, const char *key, const char *value,
                       const char *folder)
{
 char field[WPE_STARTCMD_KEY_MAX];

 if (!value)
  return(0);
 e_opt_own_field_key(scope, key, field, sizeof field);
 if (wpe_startcmd_is_own(field, value))
  return(1);
 return(folder && wpe_startcmd_check(folder, field, value) == WPE_STARTCMD_RUN);
}

/**
 * e_opt_value_typed_in - A value the user set is his own, in a folder.
 * @scope/@key: as e_opt_own_field_key; @value: what he set; @folder: the
 * folder whose file will hold it, or NULL.
 *
 * Counted as his for this session and, kept in the store for that folder,
 * after the next start too, when the folder's file holds what he wrote.
 */
void e_opt_value_typed_in(const char *scope, const char *key, const char *value,
                          const char *folder)
{
 char field[WPE_STARTCMD_KEY_MAX];

 e_opt_note_own_field(scope, key, value);
 if (!folder || !value || !*value)
  return;
 e_opt_own_field_key(scope, key, field, sizeof field);
 wpe_startcmd_remember(folder, field, value, WPE_STARTCMD_YES);
}

/**
 * e_opt_value_typed - A value the user set in Options is his own from now on.
 * @scope/@key: as e_opt_own_field_key; @value: what he set.
 */
void e_opt_value_typed(const char *scope, const char *key, const char *value)
{
 e_opt_value_typed_in(scope, key, value, e_opt_folder_dir);
}

/**
 * e_opt_own_from_defaults - Start the user's own program keys from the
 * built-in ones.
 * @cn: the editor, before any option file is read.
 *
 * A start-folder xwperc replaces the whole config, so what the user's own
 * file does not name is the built-in default; that default also runs
 * without a question, as it is the editor's own.
 */
static void e_opt_own_from_defaults(ECNT *cn)
{
 int i;

 wpe_startcmd_reset();
 e_opt_own_free_lang();
 if (e_opt_own_print) FREE(e_opt_own_print);
 e_opt_own_print = WpeStrdup(cn->print_cmd ? cn->print_cmd : "");
 e_opt_own_deb = e_deb_type;
 e_opt_named_deb = E_OPT_DEBUGGER_UNSET;
 wpe_startcmd_note_own(WPE_STARTCMD_KEY_PRINT, e_opt_own_print);
 wpe_startcmd_note_own(WPE_STARTCMD_KEY_DEBUGGER, e_deb_type_name(e_opt_own_deb));
 for (i = 0; i < e_prog.num; i++)
 {
  struct e_s_prog *p = e_opt_own_find_lang(e_prog.comp[i]->language, 1);
  if (!p)
   continue;
  e_opt_own_set(&p->compiler, e_prog.comp[i]->compiler);
  e_opt_own_set(&p->comp_str, e_prog.comp[i]->comp_str);
  e_opt_own_set(&p->libraries, e_prog.comp[i]->libraries);
  e_opt_own_set(&p->exe_name, e_prog.comp[i]->exe_name);
  p->comp_sw = e_prog.comp[i]->comp_sw;
  p->run_kind = e_run_kind_of(e_prog.comp[i]);
  e_opt_note_own_lang(p);
  e_opt_note_own_lang_fields(p);
 }
 e_opt_own_prog_from_defaults();
 e_opt_note_own_prog();
}

/**
 * e_opt_language_of - The language a "[Language/<name>]" section is about.
 * @section: the section's name.
 * Return: the name, or NULL for another section.
 */
static const char *e_opt_language_of(const char *section)
{
 size_t n = strlen(OPT_SECTION_LANGUAGE);

 if (strncmp(section, OPT_SECTION_LANGUAGE, n) != 0 || !section[n])
  return(NULL);
 return(section + n + 1);
}

/**
 * e_opt_capture_own_lang - Keep one compile-line key of the user's own file.
 * @language: the section's language; @option/@value: the key.
 */
static void e_opt_capture_own_lang(const char *language, const char *option,
                                   const char *value)
{
 struct e_s_prog *p;

 if (WpeStrccmp("Compiler", option) != 0 && WpeStrccmp("CompilerOptions", option) != 0 &&
     WpeStrccmp("Libraries", option) != 0 && WpeStrccmp("Executable", option) != 0 &&
     WpeStrccmp("CompilerSwitch", option) != 0 && WpeStrccmp(E_OPT_KEY_RUN, option) != 0)
  return;
 if (!(p = e_opt_own_find_lang(language, 1)))
  return;
 if (WpeStrccmp("Compiler", option) == 0)
  e_opt_own_set(&p->compiler, value);
 else if (WpeStrccmp("CompilerOptions", option) == 0)
  e_opt_own_set(&p->comp_str, value);
 else if (WpeStrccmp("Libraries", option) == 0)
  e_opt_own_set(&p->libraries, value);
 else if (WpeStrccmp("CompilerSwitch", option) == 0)
  p->comp_sw = atoi(value);
 else if (WpeStrccmp(E_OPT_KEY_RUN, option) == 0)
  e_opt_take_run_kind(&p->run_kind, value);
 else
  e_opt_own_set(&p->exe_name, value);
}

/**
 * e_opt_capture_own - Keep a program key of the user's own file, unapplied.
 * @section/@option/@value: the key.
 *
 * Read after a start-folder xwperc: the folder's print command, compile
 * lines and debugger stay in force, and the user's own values are kept to
 * compare them with and to run when he answers "Use mine".
 */
static void e_opt_capture_own(const char *section, const char *option, const char *value)
{
 const char *language = e_opt_language_of(section);

 if (language)
  e_opt_capture_own_lang(language, option, value);
 else if (strncmp(section, OPT_SECTION_GENERAL, strlen(OPT_SECTION_GENERAL)) == 0 &&
          WpeStrccmp("PrintCmd", option) == 0)
  e_opt_own_set(&e_opt_own_print, value);
 else if (strncmp(section, OPT_SECTION_PROGRAMMING, strlen(OPT_SECTION_PROGRAMMING)) == 0 &&
          WpeStrccmp("Debugger", option) == 0)
  e_opt_own_deb = atoi(value);
 else if (strncmp(section, OPT_SECTION_PROGRAMMING, strlen(OPT_SECTION_PROGRAMMING)) == 0)
  e_opt_own_prog_set(option, value);
}

/**
 * e_opt_note_named - Remember the debugger a start-folder xwperc names.
 * @section/@option/@value: a key of that file.
 *
 * The debugger in force also changes by itself (a .py file picks pdb), so
 * the question is about the one the folder named, not the one in force.
 */
static void e_opt_note_named(const char *section, const char *option, const char *value)
{
 if (strncmp(section, OPT_SECTION_PROGRAMMING, strlen(OPT_SECTION_PROGRAMMING)) == 0 &&
     WpeStrccmp("Debugger", option) == 0)
  e_opt_named_deb = atoi(value);
}

/**
 * e_opt_note_own_values - Count what the user's own file sets as his own.
 *
 * Called once his file has been read for the keys a folder may not set,
 * so a folder whose commands are his runs them without a question.
 */
static void e_opt_note_own_values(void)
{
 int i;

 wpe_startcmd_note_own(WPE_STARTCMD_KEY_PRINT, e_opt_own_print);
 wpe_startcmd_note_own(WPE_STARTCMD_KEY_DEBUGGER, e_deb_type_name(e_opt_own_deb));
 for (i = 0; i < e_opt_own_nlang; i++)
 {
  e_opt_note_own_lang(e_opt_own_lang[i]);
  e_opt_note_own_lang_fields(e_opt_own_lang[i]);
 }
 e_opt_note_own_prog();
}

/**
 * e_opt_own_print_cmd - The user's own print command.
 * @cn: the editor.
 * Return: his own (or the default) while a start-folder xwperc sets the
 * one in force, else the one in force, which is his.
 */
const char *e_opt_own_print_cmd(ECNT *cn)
{
 if (e_opt_folder_dir && e_opt_own_print)
  return(e_opt_own_print);
 return(cn->print_cmd);
}

/**
 * e_opt_own_language - The user's own entry for a language.
 * @language: its name.
 * Return: his own (or the default) while a start-folder xwperc sets the
 * one in force, else the entry in force, which is his; NULL when the
 * language is unknown.
 */
struct e_s_prog *e_opt_own_language(const char *language)
{
 int i;

 if (e_opt_folder_dir)
  return(e_opt_own_find_lang(language, 0));
 for (i = 0; i < e_prog.num; i++)
  if (WpeStrccmp(e_prog.comp[i]->language, language) == 0)
   return(e_prog.comp[i]);
 return(NULL);
}

/**
 * e_opt_own_debugger - The debugger the user's own config names.
 * Return: its type (the default when his file names none).
 */
int e_opt_own_debugger(void)
{
 if (e_opt_own_deb == E_OPT_DEBUGGER_UNSET)
  return(e_deb_type);
 return(e_opt_own_deb);
}

/**
 * e_opt_named_debugger - The debugger the start-folder xwperc names.
 * Return: its type, or E_OPT_DEBUGGER_UNSET when no start-folder file
 * names one or the user chose one since in Options.
 */
int e_opt_named_debugger(void)
{
 if (!e_opt_folder_dir)
  return(E_OPT_DEBUGGER_UNSET);
 return(e_opt_named_deb);
}

/**
 * e_opt_debugger_chosen - The user picked a debugger in Options.
 * @type: the one he picked.
 *
 * It is his own from now on: no question about it, in this session or,
 * once the folder's file holds it, after the next start.
 */
void e_opt_debugger_chosen(int type)
{
 e_opt_named_deb = E_OPT_DEBUGGER_UNSET;
 e_startcmd_typed(e_opt_folder_dir, WPE_STARTCMD_KEY_DEBUGGER, e_deb_type_name(type));
}

/* The lines of an option file, as read, without their newlines. */
typedef struct {
 char **line;
 int n;
} e_opt_lines;

/* How much a line buffer grows at a time while one long line is read. */
#define E_OPT_LINE_CHUNK 256

/**
 * e_opt_read_line - Read one line of any length, without its newline.
 * @fp: the file.
 * Return: the malloc'd line, or NULL at the end of the file.
 */
static char *e_opt_read_line(FILE *fp)
{
 size_t room = E_OPT_LINE_CHUNK, used = 0;
 char *buf = malloc(room), *grown;
 if (!buf) return NULL;
 buf[0] = '\0';
 while (fgets(buf + used, (int)(room - used), fp))
 {
  used += strlen(buf + used);
  if (used > 0 && buf[used - 1] == '\n') { buf[used - 1] = '\0'; return buf; }
  grown = realloc(buf, room + E_OPT_LINE_CHUNK);
  if (!grown) break;
  buf = grown;
  room += E_OPT_LINE_CHUNK;
 }
 if (used > 0) return buf;
 free(buf);
 return NULL;
}

/**
 * e_opt_lines_add - Append a line, taking it.
 * @l: the lines; @text: a malloc'd line.
 * Return: 0, or -1 out of memory (the line is then freed).
 */
static int e_opt_lines_add(e_opt_lines *l, char *text)
{
 char **grown = realloc(l->line, (size_t)(l->n + 1) * sizeof *grown);
 if (!grown) { free(text); return -1; }
 l->line = grown;
 l->line[l->n++] = text;
 return 0;
}

/**
 * e_opt_lines_add_copy - Append a copy of a line.
 * @l: the lines; @text: the line.
 * Return: 0, or -1 out of memory.
 */
static int e_opt_lines_add_copy(e_opt_lines *l, const char *text)
{
 char *copy = malloc(strlen(text) + 1);
 if (!copy) return -1;
 strcpy(copy, text);
 return e_opt_lines_add(l, copy);
}

/** e_opt_lines_free - Free every line and the list. */
static void e_opt_lines_free(e_opt_lines *l)
{
 int i;
 for (i = 0; i < l->n; i++) free(l->line[i]);
 free(l->line);
 l->line = NULL;
 l->n = 0;
}

/**
 * e_opt_lines_load - Read a whole option file.
 * @path: the file; @l: receives its lines (empty when it does not exist).
 * Return: 0, or -1 when it exists but could not be read whole.
 */
static int e_opt_lines_load(const char *path, e_opt_lines *l)
{
 FILE *fp = fopen(path, "r");
 char *text;
 l->line = NULL;
 l->n = 0;
 if (!fp) return 0;
 while ((text = e_opt_read_line(fp)) != NULL)
  if (e_opt_lines_add(l, text) < 0) { fclose(fp); return -1; }
 fclose(fp);
 return 0;
}

/**
 * e_opt_lines_store - Write lines to a file, each with its newline.
 * @path: the file; @l: the lines.
 * Return: 0, or -1 when the file could not be written.
 */
static int e_opt_lines_store(const char *path, const e_opt_lines *l)
{
 FILE *fp = fopen(path, "w");
 int i;
 if (!fp) return -1;
 for (i = 0; i < l->n; i++) fprintf(fp, "%s\n", l->line[i]);
 if (fclose(fp) != 0) return -1;
 return 0;
}

/**
 * e_opt_line_section - The section a "[name]" line opens.
 * @text: the line; @out/@sz: receive the name.
 * Return: 1 when the line is a section header.
 */
static int e_opt_line_section(const char *text, char *out, size_t sz)
{
 const char *end;
 while (isspace((unsigned char)*text)) text++;
 if (*text != '[') return 0;
 end = strchr(text, ']');
 if (!end) return 0;
 snprintf(out, sz, "%.*s", (int)(end - text - 1), text + 1);
 return 1;
}

/**
 * e_opt_line_is_user_only - Is this "key : value" line one of the user-only keys?
 * @text: the line; @section: the section it is in.
 */
static int e_opt_line_is_user_only(const char *text, const char *section)
{
 char key[E_OPT_IGNORED_MAX];
 const char *colon, *end;
 while (isspace((unsigned char)*text)) text++;
 colon = strchr(text, ':');
 if (!colon || colon == text) return 0;
 for (end = colon; end > text && isspace((unsigned char)end[-1]); end--)
  ;
 snprintf(key, sizeof key, "%.*s", (int)(end - text), text);
 return e_opt_key_is_user_only(section, key);
}

/* The section the user-only keys are written in. */
#define E_OPT_USER_ONLY_SECTION OPT_SECTION_PROGRAMMING

/**
 * e_opt_split_saved - Part a saved file into the folder's keys and the user's own.
 * @saved: the lines e_save_opt wrote; @kept: receives every line but the
 * user-only keys; @mine: receives the user-only lines.
 * Return: 0, or -1 out of memory.
 */
static int e_opt_split_saved(const e_opt_lines *saved, e_opt_lines *kept, e_opt_lines *mine)
{
 char section[E_OPT_IGNORED_MAX] = "";
 int i, rc = 0;
 for (i = 0; i < saved->n && rc == 0; i++)
 {
  const char *text = saved->line[i];
  e_opt_line_section(text, section, sizeof section);
  if (e_opt_line_is_user_only(text, section))
   rc = e_opt_lines_add_copy(mine, text);
  else
   rc = e_opt_lines_add_copy(kept, text);
 }
 return rc;
}

/**
 * e_opt_merge_user_only - The user's own file with its user-only keys replaced.
 * @own: the user's file as it is; @mine: the user-only lines now in force;
 * @out: receives the new file.
 *
 * Every other line of the user's file stays as he left it.  The user-only
 * lines go right below its [Programming] header, which is added at the end
 * when the file has none.
 * Return: 0, or -1 out of memory.
 */
static int e_opt_merge_user_only(const e_opt_lines *own, const e_opt_lines *mine, e_opt_lines *out)
{
 char section[E_OPT_IGNORED_MAX] = "", header[E_OPT_IGNORED_MAX];
 int i, j, placed = 0, rc = 0;
 for (i = 0; i < own->n && rc == 0; i++)
 {
  const char *text = own->line[i];
  int opens = e_opt_line_section(text, section, sizeof section);
  if (!opens && e_opt_line_is_user_only(text, section)) continue;
  rc = e_opt_lines_add_copy(out, text);
  if (!opens || placed || strcmp(section, E_OPT_USER_ONLY_SECTION) != 0) continue;
  for (j = 0; j < mine->n && rc == 0; j++) rc = e_opt_lines_add_copy(out, mine->line[j]);
  placed = 1;
 }
 if (placed || rc != 0 || mine->n == 0) return rc;
 snprintf(header, sizeof header, "[%s]", E_OPT_USER_ONLY_SECTION);
 rc = e_opt_lines_add_copy(out, header);
 for (j = 0; j < mine->n && rc == 0; j++) rc = e_opt_lines_add_copy(out, mine->line[j]);
 return rc;
}

/**
 * e_opt_say_move_failed - Say that the user's own config could not take his keys.
 * @cn: the editor.
 *
 * His hook, engine, endpoints and dial still apply until he quits, but
 * they are in no file, so he knows to save them again once his file can be
 * written.
 */
static void e_opt_say_move_failed(ECNT *cn)
{
 char line[E_OPT_IGNORED_LINE_MAX + PATH_MAX];

 if (!cn || !cn->fb) return;
 snprintf(line, sizeof line, E_OPT_MOVE_FAILED_FMT, e_opt_user_file);
 wpe_paint_trace_say(line);
 e_status_hold(line);
 e_status_paint(cn->fb, line);
 e_refresh();
}

/**
 * e_opt_move_user_only_keys - After a save into a start-folder xwperc, put the user-only keys in the user's own file.
 * @cn: the editor.
 *
 * Saving the options (Options > Save, Ok in Options > AI, Alt-G y) writes
 * every key to the file the options came from.  When that is a project's
 * xwperc, the user's own hook, engine command, endpoints and dial would land
 * in a file he may commit and push, and would not apply there anyway.  So
 * they are taken out of it and written into the user's own file, where the
 * next start in any folder reads them; the rest of his file is kept.  They
 * leave the project's file even when his own cannot be written, so they
 * never reach a file he may push; the status line then says they were not
 * saved.  A Save As to any other file writes every key, as asked.
 */
static void e_opt_move_user_only_keys(ECNT *cn)
{
 e_opt_lines saved = {NULL, 0}, kept = {NULL, 0}, mine = {NULL, 0};
 e_opt_lines own = {NULL, 0}, merged = {NULL, 0};
 int stored;
 if (!e_opt_user_file || !e_opt_folder_file) return;
 if (!e_opt_same_file(e_opt_folder_file, cn->optfile)) return;
 if (e_opt_lines_load(cn->optfile, &saved) == 0 &&
     e_opt_split_saved(&saved, &kept, &mine) == 0)
 {
  stored = e_opt_lines_load(e_opt_user_file, &own) == 0 &&
           e_opt_merge_user_only(&own, &mine, &merged) == 0 &&
           e_opt_lines_store(e_opt_user_file, &merged) == 0;
  e_opt_lines_store(cn->optfile, &kept);
  if (!stored && mine.n > 0)
   e_opt_say_move_failed(cn);
 }
 e_opt_lines_free(&own);
 e_opt_lines_free(&saved);
 e_opt_lines_free(&kept);
 e_opt_lines_free(&mine);
 e_opt_lines_free(&merged);
}

/**
 * e_opt_read_stream - Take the options of one open file.
 * @cn: the editor; @fp: the file; @mode: which keys to take (E_OPT_READ_*).
 * Return: 0, or ERR_READ_OPF for a line that is not "key : value" or a
 * section header that does not close.
 */
static int e_opt_read_stream(ECNT *cn, FILE *fp, int mode)
{
 char *str_line;
 char *section;
 char *option;
 char *value;
 char *str_tmp;
 int sz;
 int i;

 sz = 256;
 str_line = (char *)WpeMalloc(256 * sizeof(char));
 section = NULL;
 while (!feof(fp))
 {
  if (sz != 256)
  {
   sz = 256;
   str_line = (char *)WpeRealloc(str_line, sz * sizeof(char));
  }
  str_line[0] = 0;
  fgets(str_line, sz, fp);
  while ((!feof(fp)) &&
    ((str_line[0] == 0) || (str_line[strlen(str_line) - 1] != '\n')))
  {
   sz += 255;
   str_line = (char *)WpeRealloc(str_line, sz * sizeof(char));
   fgets(str_line + sz - 256, 256, fp);
  }
  i = strlen(str_line);
  if (i && (str_line[i - 1] == '\n'))
   str_line[i - 1] = 0;
  for (option = str_line; isspace(*option); option++)
   ;
  if ((*option) && (*option != '#'))
  {
   if (*option == '[')
   {
    if (section)
     WpeFree(section);
    for (value = option + 1; (*value) && (*value != ']'); value++)
     ;
    if (*value != ']')
    {
     WpeFree(str_line);
     return ERR_READ_OPF;
    }
    *value = 0;
    section = WpeStrdup(option + 1);
   }
   else
   {
    value = strchr(option, ':');
    if ((value == NULL) || (value == option))
    {
     WpeFree(str_line);
     return ERR_READ_OPF;
    }
    for (str_tmp = value - 1; isspace(*str_tmp); str_tmp--)
     ;
    for (value++; isspace(*value); value++)
     ;
    str_tmp++;
    *str_tmp = 0;
    for (i = 0;
      (i < OPTION_SECTIONS) &&
        (strncmp(WpeSectionRead[i].section, section, strlen(WpeSectionRead[i].section)) != 0);
      i++)
     ;
    if (i >= OPTION_SECTIONS)
     return ERR_READ_OPF;
    if (mode == E_OPT_READ_SHARED)
     e_opt_note_named(section, option, value);
    if (e_opt_line_applies(section, option, mode))
     (*WpeSectionRead[i].function)(cn, section, option, value);
    else if (mode == E_OPT_READ_USER_ONLY)
     e_opt_capture_own(section, option, value);
   }
  }
 }
 return 0;
}

/**
 * e_opt_open_options - Open the file the options come from.
 * @cn: the editor.
 * Return: the editor's option file, else the copy installed with the
 * editor, else NULL.
 */
static FILE *e_opt_open_options(ECNT *cn)
{
 FILE *fp = fopen(cn->optfile, "r");
 char *file;
 if (fp != NULL) return fp;
 file = e_mkfilename((char *)e_lib_dir(), OPTION_FILE);
 fp = fopen(file, "r");
 FREE(file);
 return fp;
}

/**
 * e_opt_read_user_only - Take the user-only keys from the user's own config.
 * @cn: the editor.
 *
 * After a start-folder xwperc: the hook, the engine, the dial, the web
 * switch and the endpoints the user set for himself keep applying in that
 * folder.  A missing file leaves them at their defaults.
 */
static void e_opt_read_user_only(ECNT *cn)
{
 FILE *fp = fopen(e_opt_user_file, "r");
 if (fp == NULL) return;
 e_opt_read_stream(cn, fp, E_OPT_READ_USER_ONLY);
 fclose(fp);
}

/**
 * e_opt_read - Read the options the editor starts with.
 * @cn: the editor.
 *
 * From the user's own config file, or one named with -sf, every key is
 * taken.  From an xwperc in the folder the editor was started in (a project
 * the user cloned, a folder the assistant can write), every key is taken
 * except the user-only ones (e_opt_user_only_keys), which come from the
 * user's own file instead; the ones it named are said once on the status
 * line (e_opt_say_ignored_keys).
 * Return: 0, or ERR_READ_OPF when the file the options come from is malformed.
 */
int e_opt_read(ECNT *cn)
{
 FILE *fp;
 int mode = E_OPT_READ_ALL, err;

 e_opt_ignored_keys[0] = '\0';
 if (e_opt_file_missing(cn)) return 0;   /* said at start by e_opt_say_file_missing */
 if (e_opt_user_file && e_opt_same_file(e_opt_user_file, cn->optfile))
  e_opt_use_user_file(NULL);
 e_opt_forget_folder();
 if (e_opt_user_file)
 {
  mode = E_OPT_READ_SHARED;
  e_opt_note_folder(cn);
  e_opt_own_from_defaults(cn);
 }
 fp = e_opt_open_options(cn);
 if (fp == NULL) err = 0;
 else
 {
  err = e_opt_read_stream(cn, fp, mode);
  fclose(fp);
 }
 if (e_opt_user_file)
  e_opt_read_user_only(cn);
 e_opt_lang_finish();
 if (e_opt_user_file)
  e_opt_note_own_values();
 return err;
}

/**
 * e_opt_say_ignored_keys - Tell the user which keys of the start-folder xwperc did not apply.
 * @cn: the editor.
 *
 * A project's xwperc that tries to set a hook, an engine command, the dial
 * or an endpoint has no effect, and the user would otherwise not know why
 * the setting he sees in the file is not the one in force.  The line stays
 * on the bottom row until the next key.
 */
void e_opt_say_ignored_keys(ECNT *cn)
{
 char line[E_OPT_IGNORED_LINE_MAX];
 if (!e_opt_ignored_keys[0] || !cn || !cn->fb) return;
 snprintf(line, sizeof line, E_OPT_IGNORED_FMT, e_opt_ignored_keys);
 e_status_hold(line);
 e_status_paint(cn->fb, line);
 e_refresh();
}

/**
 * e_opt_field_take - Take a dialog field's text into a buffer, whole.
 * @o: the dialog; @k: the index of its text field; @dst/@size: the buffer.
 *
 * Every dialog that copies a field out goes through here, so a long entry
 * is never written past a buffer smaller than the field and never cut to fit
 * it.  Where a field is sized from its buffer this cannot refuse; where it
 * does, the user is told the entry was too long instead of finding a
 * different text in the editor.
 * Return: 0, or -1 with @dst empty.
 */
int e_opt_field_take(W_OPTSTR *o, int k, char *dst, size_t size)
{
 if (!o || k < 0 || k >= o->wn || !o->wstr[k])
  return(-1);
 if (wpe_field_take(o->wstr[k]->txt, dst, size) == 0)
  return(0);
 e_input_limit_say((int)(size ? size - 1 : 0));
 return(-1);
}

/**
 * e_arguments_field_max - How many bytes the one-line entry window may take.
 * @size: the size of the buffer the caller keeps the answer in.
 *
 * The field's own capacity, or less when the caller's buffer is smaller, so
 * the text always fits where it is going.  A caller that keeps a path (a
 * buffer of WPE_PATH_SIZE) gets a field that takes any path the system
 * allows, so a long Info-Path or project name can be read and edited.
 */
static int e_arguments_field_max(size_t size)
{
 if (size >= (size_t)WPE_PATH_SIZE)
  return(WPE_PATH_FIELD_MAX);
 if (size > (size_t)E_ARG_FIELD_MAX)
  return(E_ARG_FIELD_MAX);
 return((int)size - 1);
}

/*  window for entering a text line; the answer goes into @str of @size bytes */
int e_add_arguments(char *str, size_t size, char *head, FENSTER *f, int n, int sw,
  struct dirfile **df)
{
 int ret;
 char *tmp;
 W_OPTSTR *o;

 if (!str || size < 2)
  return(-1);
 tmp = MALLOC((strlen(head)+2) * sizeof(char));
 o = e_init_opt_kst(f);
 if (!o || !tmp)
  return(-1);
 o->xa = 20;  o->ya = 4;  o->xe = 57;  o->ye = 11;
 o->bgsw = 0;
 o->name = head;
 o->crsw = AltO;
 sprintf(tmp, "%s:", head);
 e_add_wrstr(4, 2, 4, 3, 30, e_arguments_field_max(size), n, sw, tmp, str, df, o);
 e_add_bttstr(7, 5, 1, AltO, " Ok ", NULL, o);
 e_add_bttstr(24, 5, -1, WPE_ESC, "Cancel", NULL, o);
 FREE(tmp);
 ret = e_opt_kst(o);
 if (ret != WPE_ESC && e_opt_field_take(o, 0, str, size) != 0)
  ret = WPE_ESC;
 freeostr(o);
 return(ret == WPE_ESC ? 0 : 1);
}

W_O_TXTSTR **e_add_txtstr(int x, int y, char *txt, W_OPTSTR *o)
{
 if (o->tn == 0)
  o->tstr = MALLOC(1);
 (o->tn)++;
 if (!(o->tstr = REALLOC(o->tstr, o->tn * sizeof(W_O_TXTSTR *))))
  return(NULL);
 if (!(o->tstr[o->tn-1] = MALLOC(sizeof(W_O_TXTSTR))))
  return(NULL);
 if (!(o->tstr[o->tn-1]->txt = MALLOC((strlen(txt)+1) * sizeof(char))))
  return(NULL);
 o->tstr[o->tn-1]->x = x;
 o->tstr[o->tn-1]->y = y;
 strcpy(o->tstr[o->tn-1]->txt, txt);
 return(o->tstr);
}

W_O_WRSTR **e_add_wrstr(int xt, int yt, int xw, int yw, int nw, int wmx,
   int nc, int sw, char *header, char *txt, struct dirfile **df, W_OPTSTR *o)
{
 if (o->wn == 0)
  o->wstr = MALLOC(1);
 (o->wn)++;
 if (!(o->wstr = REALLOC(o->wstr, o->wn * sizeof(W_O_WRSTR *))))
  return(NULL);
 if (!(o->wstr[o->wn-1] = MALLOC(sizeof(W_O_WRSTR))))
  return(NULL);
 if (!(o->wstr[o->wn-1]->txt = MALLOC((wmx+1) * sizeof(char))))
  return(NULL);
 if (!(o->wstr[o->wn-1]->header = MALLOC((strlen(header)+1) * sizeof(char))))
  return(NULL);
 o->wstr[o->wn-1]->xt = xt;
 o->wstr[o->wn-1]->yt = yt;
 o->wstr[o->wn-1]->xw = xw;
 o->wstr[o->wn-1]->yw = yw;
 o->wstr[o->wn-1]->nw = nw;
 o->wstr[o->wn-1]->wmx = wmx;
 o->wstr[o->wn-1]->nc = nc;
 o->wstr[o->wn-1]->sw = sw;
 o->wstr[o->wn-1]->lines_key = 0;
 o->wstr[o->wn-1]->df = df;
 strcpy(o->wstr[o->wn-1]->header, header);
 /* The field holds wmx bytes: a longer value (a deep option file's path, a
    compiler line an xwperc made long) is cut to that, never copied past it. */
 snprintf(o->wstr[o->wn-1]->txt, wmx + 1, "%s", txt);
 return(o->wstr);
}

W_O_NUMSTR **e_add_numstr(int xt, int yt, int xw, int yw, int nw, int wmx,
   int nc, int sw, char *header, int num, W_OPTSTR *o)
{
 if (o->nn == 0)
  o->nstr = MALLOC(1);
 (o->nn)++;
 if (!(o->nstr = REALLOC(o->nstr, o->nn * sizeof(W_O_NUMSTR *))))
  return(NULL);
 if (!(o->nstr[o->nn-1] = MALLOC(sizeof(W_O_NUMSTR))))
  return(NULL);
 if (!(o->nstr[o->nn-1]->header = MALLOC((strlen(header)+1) * sizeof(char))))
  return(NULL);
 o->nstr[o->nn-1]->xt = xt;
 o->nstr[o->nn-1]->yt = yt;
 o->nstr[o->nn-1]->xw = xw;
 o->nstr[o->nn-1]->yw = yw;
 o->nstr[o->nn-1]->nw = nw;
 o->nstr[o->nn-1]->wmx = wmx;
 o->nstr[o->nn-1]->nc = nc;
 o->nstr[o->nn-1]->sw = sw;
 o->nstr[o->nn-1]->num = num;
 strcpy(o->nstr[o->nn-1]->header, header);
 return(o->nstr);
}

W_O_SSWSTR **e_add_sswstr(int x, int y, int nc, int sw, int num,
   char *header, W_OPTSTR *o)
{
 if (o->sn == 0)
  o->sstr = MALLOC(1);
 (o->sn)++;
 if (!(o->sstr = REALLOC(o->sstr, o->sn * sizeof(W_O_SSWSTR *))))
  return(NULL);
 if (!(o->sstr[o->sn-1] = MALLOC(sizeof(W_O_SSWSTR))))
  return(NULL);
 if (!(o->sstr[o->sn-1]->header = MALLOC((strlen(header)+1) * sizeof(char))))
  return(NULL);
 o->sstr[o->sn-1]->x = x;
 o->sstr[o->sn-1]->y = y;
 o->sstr[o->sn-1]->nc = nc;
 o->sstr[o->sn-1]->sw = sw;
 o->sstr[o->sn-1]->num = num;
 strcpy(o->sstr[o->sn-1]->header, header);
 return(o->sstr);
}

W_O_SPSWSTR **e_add_spswstr(int n, int x, int y, int nc, int sw,
   char *header, W_OPTSTR *o)
{
 if (n >= o->pn)
  return(NULL);
 if (n < 0)
  n = 0;
 if (o->pstr[n]->np == 0)
  o->pstr[n]->ps = MALLOC(1);
 (o->pstr[n]->np)++;
 if (!(o->pstr[n]->ps = REALLOC(o->pstr[n]->ps, o->pstr[n]->np * sizeof(W_O_SPSWSTR *))))
  return(NULL);
 if (!(o->pstr[n]->ps[o->pstr[n]->np-1] = MALLOC(sizeof(W_O_SPSWSTR))))
  return(NULL);
 if (!(o->pstr[n]->ps[o->pstr[n]->np-1]->header = MALLOC((strlen(header)+1) * sizeof(char))))
  return(NULL);
 o->pstr[n]->ps[o->pstr[n]->np-1]->x = x;
 o->pstr[n]->ps[o->pstr[n]->np-1]->y = y;
 o->pstr[n]->ps[o->pstr[n]->np-1]->nc = nc;
 o->pstr[n]->ps[o->pstr[n]->np-1]->sw = sw;
 strcpy(o->pstr[n]->ps[o->pstr[n]->np-1]->header, header);
 return(o->pstr[n]->ps);
}

W_O_PSWSTR **e_add_pswstr(int n, int x, int y, int nc, int sw, int num,
   char *header, W_OPTSTR *o)
{
 if (o->pn == 0)
  o->pstr = MALLOC(1);
 if (n >= o->pn)
 {
  n = o->pn;
  (o->pn)++;
  if (!(o->pstr = REALLOC(o->pstr, o->pn * sizeof(W_O_PSWSTR *))))
   return(NULL);
  if (!(o->pstr[o->pn-1] = MALLOC(sizeof(W_O_PSWSTR))))
   return(NULL);
  o->pstr[o->pn-1]->np = 0;
 }
 if (!e_add_spswstr(n, x, y, nc, sw, header, o))
  return(NULL);
 o->pstr[o->pn-1]->num = num;
 return(o->pstr);
}

W_O_BTTSTR **e_add_bttstr(int x, int y, int nc, int sw, char *header,
   int (*fkt)(FENSTER *f), W_OPTSTR *o)
{
 if (o->bn == 0)
  o->bstr = MALLOC(1);
 (o->bn)++;
 if (!(o->bstr = REALLOC(o->bstr, o->bn * sizeof(W_O_BTTSTR *))))
  return(NULL);
 if (!(o->bstr[o->bn-1] = MALLOC(sizeof(W_O_BTTSTR))))
  return(NULL);
 if (!(o->bstr[o->bn-1]->header = MALLOC((strlen(header)+1) * sizeof(char))))
  return(NULL);
 o->bstr[o->bn-1]->x = x;
 o->bstr[o->bn-1]->y = y;
 o->bstr[o->bn-1]->nc = nc;
 o->bstr[o->bn-1]->sw = sw;
 o->bstr[o->bn-1]->fkt = fkt;
 strcpy(o->bstr[o->bn-1]->header, header);
 return(o->bstr);
}

int freeostr(W_OPTSTR *o)
{
 int i, j;

 if (!o)
  return(0);
 for (i = 0; i < o->tn; i++)
 {
  FREE(o->tstr[i]->txt);
  FREE(o->tstr[i]);
 }
 if (o->tn)
  FREE(o->tstr);
 for (i = 0; i < o->wn; i++)
 {
  FREE(o->wstr[i]->txt);
  FREE(o->wstr[i]->header);
  FREE(o->wstr[i]);
 }
 if (o->wn)
  FREE(o->wstr);
 for (i = 0; i < o->nn; i++)
 {
  FREE(o->nstr[i]->header);
  FREE(o->nstr[i]);
 }
 if (o->nn)
  FREE(o->nstr);
 for (i = 0; i < o->pn; i++)
 {
  for (j = 0; j < o->pstr[i]->np; j++)
  {
   FREE(o->pstr[i]->ps[j]->header);
   FREE(o->pstr[i]->ps[j]);
  }
  if (o->pstr[i]->np)
   FREE(o->pstr[i]->ps);
  FREE(o->pstr[i]);
 }
 if (o->pn)
  FREE(o->pstr);
 for (i = 0; i < o->bn; i++)
 {
  FREE(o->bstr[i]->header);
  FREE(o->bstr[i]);
 }
 if (o->bn)
  FREE(o->bstr);
 FREE(o);
 return(0);
}

W_OPTSTR *e_init_opt_kst(FENSTER *f)
{
 W_OPTSTR *o = MALLOC(sizeof(W_OPTSTR));
 if (!o)
  return(NULL);
 o->frt = f->fb->nr.fb;
 o->frs = f->fb->ne.fb;
 o->ftt = f->fb->nt.fb;
 o->fts = f->fb->nsnt.fb;
 o->fst = f->fb->fs.fb;
 o->fss = f->fb->nsft.fb;
 o->fsa = f->fb->fsm.fb;
 o->fwt = f->fb->fr.fb;
 o->fws = f->fb->fa.fb;
 o->fbt = f->fb->nz.fb;
 o->fbs = f->fb->ns.fb;
 o->fbz = f->fb->nm.fb;
 o->tn = o->sn = o->pn = o->bn = o->wn = o->nn = 0;
 o->f = f;
 o->pic = NULL;
 return(o);
}

/**
 * e_opt_getch - Read the next key for a dialog, leaving a terminal paste alone.
 *
 * A bracketed paste (xterm mode 2004) streams its text through the same key
 * reader as typing does.  Only a focused text field has a use for the pasted
 * characters, and it reads them itself; anywhere else in a dialog -- on a
 * checkbox, a radio, a button, in the keyboard move mode -- a pasted letter
 * would be taken as an accelerator (an "o" pressing Ok and closing the box)
 * and a pasted newline as Enter.  So while the terminal says a paste is
 * streaming, its bytes are swallowed and the first key typed after it is
 * returned.  The terminal backend clears the flag when it sees the paste's
 * end marker, so the loop ends with the paste.
 */
static int e_opt_getch(void)
{
 extern int g_bracketed_paste;
 int c;

 do
  c = wpe_paint_modal_getch();
 while (g_bracketed_paste);
 return c;
}

/**
 * e_opt_box_follow - Tell the layout owner where the user put the dialog.
 * @o: the dialog, after it was moved (the keyboard mover, the mouse drag).
 *
 * The dialog keeps its new place when something floats over it and goes
 * away (the owner redraws it there), and the owner holds the background the
 * move saved, so a resize drops the right one.  A resize centres it again.
 */
void e_opt_box_follow(W_OPTSTR *o)
{
 WpeBox *box = o->box;

 if (box == NULL)
  return;
 box->pic = o->pic;
 wpe_box_move(box, o->xa - box->at.bx_left, o->ya - box->at.bx_top);
}

int e_opt_move(W_OPTSTR *o)
{
 int xa = o->xa, ya = o->ya, xe = o->xe, ye = o->ye;
 int c = 0;

 e_std_rahmen(o->xa, o->ya, o->xe, o->ye, o->name, 0, o->frt, o->frs);
 while ((c = e_opt_getch()) != WPE_ESC && c != WPE_CR)
 {
  switch(c)
  {
   case WPE_RESIZE:
    /* The owner laid the dialog out again at the new size: the move goes
       on from there. */
    xa = o->xa; ya = o->ya; xe = o->xe; ye = o->ye;
    e_std_rahmen(o->xa, o->ya, o->xe, o->ye, o->name, 0, o->frt, o->frs);
    break;
   case CLE:
    if (xa > 0) {  xa--; xe--;  }
    break;
   case CRI:
    if (xe < MAXSCOL-1) {  xa++; xe++;  }
    break;
   case CUP:
    if (ya > 1) {  ya--; ye--;  }
    break;
   case CDO:
    if (ye < MAXSLNS-2) {  ya++; ye++;  }
    break;
  }
  if ( xa != o->xa || ya != o->ya || xe != o->xe || ye != o->ye)
  {
   PIC *snap = e_opt_drag_snapshot(o);   /* the box as drawn at the OLD spot */
   o->xa = xa;
   o->ya = ya;
   o->xe = xe;
   o->ye = ye;
   o->pic = e_change_pic(o->xa, o->ya, o->xe, o->ye, o->pic, 1, o->frt);
   e_opt_box_follow(o);
   if (o->pic == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, o->f->fb);
   if (snap != NULL)
   {  snap->a.x = o->xa;  snap->a.y = o->ya;   /* stamp it at the NEW spot */
      snap->e.x = o->xe;  snap->e.y = o->ye;
      e_close_view(snap, 2);                    /* frees snap; never reused */
   }
  }
 }
 e_std_rahmen(o->xa, o->ya, o->xe, o->ye, o->name, 1, o->frt, o->frs);
 return(c);
}

int e_get_sw_cmp(int xin, int yin, int x, int y, int xmin, int ymin, int c)
{
 return
      (
      ( c == 0 && yin == y && (xin-1 <= x && xin+xmin >= x))  ||
      ((c == CDO || c == BDO || c == WPE_TAB) && yin > y && (yin < ymin ||
      (yin == ymin && xin <= x && xin > xmin)) ) ||
      ((c == CUP || c == BUP || c == WPE_BTAB) && yin < y && (yin > ymin ||
      (yin == ymin && xin <=x && xin > xmin)) ) ||
      ((c == CLE || c == CCLE) && yin == y && xin < x && xin > xmin) ||
      ((c == CRI || c == CCRI) && yin == y && xin > x && xin < xmin) );
}

/* A checkbox or radio is drawn as its 3-cell "[ ]" / "( )" glyph, a space, and
   then its label: the label starts this many cells past the glyph's column. */
#define OPT_GLYPH_WIDTH 4

/* Width of a check/radio widget's clickable strip: the "[ ]"/"( ) " glyph PLUS
   its label, so a click anywhere on "[ ] Enable AI" or "( ) Ollama" toggles it,
   not only the 3-cell glyph.  Buttons already hit-test their whole label; this
   brings the check and radio widgets in line, which is what a user expects when
   they click the text.  The label is drawn OPT_GLYPH_WIDTH cells past the glyph
   (see e_opt_draw_widgets). */
static int e_opt_glyph_hit_width(const char *header)
{
   return header ? OPT_GLYPH_WIDTH + (int)strlen(header) : 2;
}

static int e_get_opt_sw_spatial(int c, int x, int y, W_OPTSTR *o)
{
   int i, j, xmin, ymin, ret = 0;
   xmin = (c == CRI || c == CCRI) ? o->xe : o->xa;
   ymin = (c == CUP || c == BUP) ? o->ya : o->ye;
   x -= o->xa;  xmin -= o->xa;
   y -= o->ya;  ymin -= o->ya;
   for(i = 0; i < o->wn; i++)
   {  if(e_get_sw_cmp(o->wstr[i]->xw, o->wstr[i]->yw, x, y,
         c ? xmin : o->wstr[i]->nw, ymin, c))
      {  xmin = o->wstr[i]->xw;  ymin = o->wstr[i]->yw;
         ret = o->wstr[i]->sw;
      }
   }
   for(i = 0; i < o->nn; i++)
   {  if(e_get_sw_cmp(o->nstr[i]->xw, o->nstr[i]->yw, x, y,
         c ? xmin : o->nstr[i]->nw, ymin, c))
      {  xmin = o->nstr[i]->xw;  ymin = o->nstr[i]->yw;
         ret = o->nstr[i]->sw;
      }
   }
   for(i = 0; i < o->sn; i++)
   {  if(e_get_sw_cmp(o->sstr[i]->x, o->sstr[i]->y, x, y,
         c ? xmin : e_opt_glyph_hit_width(o->sstr[i]->header), ymin, c))
      {  xmin = o->sstr[i]->x;  ymin = o->sstr[i]->y;
         ret = o->sstr[i]->sw;
      }
   }
   for(i = 0; i < o->pn; i++)
      for(j = 0; j < o->pstr[i]->np; j++)
      {  if(e_get_sw_cmp(o->pstr[i]->ps[j]->x, o->pstr[i]->ps[j]->y, x, y,
            c ? xmin : e_opt_glyph_hit_width(o->pstr[i]->ps[j]->header), ymin, c))
         {  xmin = o->pstr[i]->ps[j]->x;  ymin = o->pstr[i]->ps[j]->y;
            ret = o->pstr[i]->ps[j]->sw;
         }
      }
   for(i = 0; i < o->bn; i++)
   {  if(e_get_sw_cmp(o->bstr[i]->x, o->bstr[i]->y, x, y,
         c ? xmin : strlen(o->bstr[i]->header), ymin, c))
      {  xmin = o->bstr[i]->x;  ymin = o->bstr[i]->y;
         ret = o->bstr[i]->sw;
      }
   }
   return ret;
}

struct tab_entry { int y, x, sw, col; };

static int tab_entry_cmp(const void *a, const void *b)
{
 const struct tab_entry *ta = a, *tb = b;
 if (ta->col != tb->col) return ta->col - tb->col;
 if (ta->y != tb->y) return ta->y - tb->y;
 return ta->x - tb->x;
}

static int e_get_opt_sw_tab(int c, int x, int y, W_OPTSTR *o)
{
 struct tab_entry tabs[128];
 int n = 0, i, cur = -1, best = -1;
 int mid = (o->xe - o->xa) / 2;
 int two_col = 0, has_left = 0, has_right = 0;
 int btn_y = 0;

 x -= o->xa;
 y -= o->ya;
 for (i = 0; i < o->wn; i++)
 {
  tabs[n].x = o->wstr[i]->xw;
  tabs[n].y = o->wstr[i]->yw;
  tabs[n].sw = o->wstr[i]->sw;
  n++;
 }
 for (i = 0; i < o->nn; i++)
 {
  tabs[n].x = o->nstr[i]->xw;
  tabs[n].y = o->nstr[i]->yw;
  tabs[n].sw = o->nstr[i]->sw;
  n++;
 }
 for (i = 0; i < o->sn; i++)
 {
  tabs[n].x = o->sstr[i]->x;
  tabs[n].y = o->sstr[i]->y;
  tabs[n].sw = o->sstr[i]->sw;
  n++;
 }
 for (i = 0; i < o->pn; i++)
 {
  if (o->pstr[i]->np > 0)
  {
   tabs[n].x = o->pstr[i]->ps[0]->x;
   tabs[n].y = o->pstr[i]->ps[0]->y;
   tabs[n].sw = o->pstr[i]->ps[0]->sw;
   n++;
  }
 }
 for (i = 0; i < o->bn; i++)
 {
  tabs[n].x = o->bstr[i]->x;
  tabs[n].y = o->bstr[i]->y;
  tabs[n].sw = o->bstr[i]->sw;
  if (tabs[n].y > btn_y) btn_y = tabs[n].y;
  n++;
 }
 if (n == 0) return c;
 for (i = 0; i < n; i++)
 {
  if (tabs[i].y < btn_y)
  {
   if (tabs[i].x < mid) has_left = 1;
   else has_right = 1;
  }
 }
 two_col = has_left && has_right;
 for (i = 0; i < n; i++)
 {
  if (tabs[i].y >= btn_y)
   tabs[i].col = 2;
  else if (two_col && tabs[i].x >= mid)
   tabs[i].col = 1;
  else
   tabs[i].col = 0;
 }
 qsort(tabs, n, sizeof(struct tab_entry), tab_entry_cmp);
 for (i = 0; i < n; i++)
  if (tabs[i].y == y && tabs[i].x == x) { cur = i; break; }
 if (cur < 0)
 {
  int dist, best_dist = 9999;
  for (i = 0; i < n; i++)
  {
   dist = (tabs[i].y - y) * 100 + (tabs[i].x - x);
   if (dist < 0) dist = -dist;
   if (dist < best_dist) { best_dist = dist; cur = i; }
  }
 }
 if (cur < 0) cur = 0;
 if (c == WPE_TAB)
  best = (cur + 1) % n;
 else
  best = (cur - 1 + n) % n;
 return tabs[best].sw;
}

int e_get_opt_sw(int c, int x, int y, W_OPTSTR *o)
{
   int ret;
   if( c != 0 && c != CUP && c != CDO && c != CLE && c != CRI && c != BUP &&
       c != BDO && c != CCLE && c != CCRI && c != WPE_TAB && c != WPE_BTAB )
	return(c);
   if (c == WPE_TAB || c == WPE_BTAB)
    return e_get_opt_sw_tab(c, x, y, o);
   ret = e_get_opt_sw_spatial(c, x, y, o);
   return(!ret ? c : ret);
}

/* The window kind of a docked tool or output pane (AI, Messages). */
#define E_OPT_PANE_INS 8

/**
 * e_opt_box_refresh - Tell the layout owner which window the dialog sits on.
 * @box: the dialog's box.
 *
 * Called before every placement, so the dialog is centred on the window that
 * is under it now, after the windows were laid out for a new screen size.  A
 * tool or output pane is a small docked strip; a dialog centred on it lands
 * squashed in a corner (and, when it is the pane the user just used,
 * unusable), so the owner centres such dialogs on the whole screen.
 */
static void e_opt_box_refresh(WpeBox *box)
{
   W_OPTSTR *o = box->self;
   FENSTER *ef = o->f->ed->f[o->f->ed->mxedt];

   wpe_box_within(&box->place, ef->a.x, ef->a.y, ef->e.x, ef->e.y,
                  ef->ins == E_OPT_PANE_INS);
}

/**
 * e_opt_element_visible - Decide whether a widget may be drawn in the frame.
 * @o:     the dialog
 * @ex:    the widget's first column, relative to the frame's left border
 * @ey:    the widget's row, relative to the frame's top border
 * @width: the cells the widget paints from @ex to the right
 *
 * When the screen is too small the dialog is clamped into the top-left corner
 * and its frame shrinks, but the user must still see a clean border rather
 * than labels and fields running through it.  A widget is drawn only when its
 * whole extent lies strictly inside the frame (the border rows and columns
 * excluded); one that would cross the right border is left out entirely, just
 * like one whose row falls below the bottom border.  All-or-nothing is the
 * honest choice: a text field cut mid-way would show a truncated value with no
 * hint that it is truncated.
 */
static int e_opt_element_visible(W_OPTSTR *o, int ex, int ey, int width)
{
 int ax = o->xa + ex;
 int ay = o->ya + ey;
 int last = ax + width - 1;
 return (ay > o->ya && ay < o->ye && ax > o->xa && last < o->xe);
}

/**
 * e_opt_field_visible - Visibility of a labelled field (text or numeric).
 *
 * The label and the entry box are drawn at separate cells; the user sees the
 * field as one thing, so it is drawn only when both parts fit.
 */
static int e_opt_field_visible(W_OPTSTR *o, int xt, int yt, const char *header,
                               int xw, int yw, int nw)
{
 if (!e_opt_element_visible(o, xt, yt, (int)strlen(header)))
  return 0;
 return e_opt_element_visible(o, xw, yw, nw);
}

/**
 * e_opt_draw_text_field - Paint a text field's current value in its box.
 *
 * Used at the first layout and again after the user has left the field, so
 * what is shown is always the value the dialog holds.  A field with a
 * drop-down history (@df) draws its arrow with the value.
 */
static void e_opt_draw_text_field(W_OPTSTR *o, int i)
{
 W_O_WRSTR *w = o->wstr[i];
 int x = o->xa + w->xw;
 int y = o->ya + w->yw;

 if (!w->df)
  e_schr_nchar(w->txt, x, y, 0, w->nw, o->fwt);
 else
  e_schr_nchar_wsv(w->txt, x, y, 0, w->nw, o->fwt, o->fws);
}

/* The two looks of a check/radio label and of a button face. */
#define OPT_LOOK_PLAIN   0
#define OPT_LOOK_FOCUSED 1

/**
 * e_opt_draw_checkbox_label - Paint a checkbox's label plain or highlighted.
 *
 * The highlight is the user's only sign of which checkbox will react to
 * Space; it goes on when the box takes the focus and off when it loses it.
 */
static void e_opt_draw_checkbox_label(W_OPTSTR *o, int i, int look)
{
 W_O_SSWSTR *s = o->sstr[i];
 int x = o->xa + s->x + OPT_GLYPH_WIDTH;
 int y = o->ya + s->y;

 if (look == OPT_LOOK_FOCUSED)
  e_pr_str(x, y, s->header, o->fsa, s->nc, 1, o->fsa, 0);
 else
  e_pr_str(x, y, s->header, o->fst, s->nc, 1, o->fss, 0);
}

/**
 * e_opt_draw_radio_label - Paint one radio option's label plain or highlighted.
 */
static void e_opt_draw_radio_label(W_OPTSTR *o, int i, int j, int look)
{
 W_O_SPSWSTR *p = o->pstr[i]->ps[j];
 int x = o->xa + p->x + OPT_GLYPH_WIDTH;
 int y = o->ya + p->y;

 if (look == OPT_LOOK_FOCUSED)
  e_pr_str(x, y, p->header, o->fsa, p->nc, 1, o->fsa, 0);
 else
  e_pr_str(x, y, p->header, o->fst, p->nc, 1, o->fss, 0);
}

/**
 * e_opt_draw_button - Paint a button face plain or in its focused colours.
 */
static void e_opt_draw_button(W_OPTSTR *o, int i, int look)
{
 W_O_BTTSTR *b = o->bstr[i];
 int x = o->xa + b->x;
 int y = o->ya + b->y;

 if (look == OPT_LOOK_FOCUSED)
  e_pr_str(x, y, b->header, o->fbz, b->nc, -1, o->fbz, o->ftt);
 else
  e_pr_str(x, y, b->header, o->fbt, b->nc, -1, o->fbs, o->ftt);
}

/**
 * e_opt_draw_widgets - Lay out every widget of a freshly framed dialog.
 *
 * Runs at the first layout and after every from-scratch redraw (a terminal
 * resize, a picker that floated over the box).  Each widget is drawn only when
 * it fits inside the frame, so a dialog clipped by a small screen keeps a
 * clean border.
 */
static void e_opt_draw_widgets(W_OPTSTR *o)
{
 int i, j;

 for (i = 0; i < o->tn; i++)
 {
  W_O_TXTSTR *t = o->tstr[i];
  if (!e_opt_element_visible(o, t->x, t->y, (int)strlen(t->txt)))
   continue;
  e_pr_str(o->xa + t->x, o->ya + t->y, t->txt, o->ftt, -1, 0, 0, 0);
 }
 for (i = 0; i < o->wn; i++)
 {
  W_O_WRSTR *w = o->wstr[i];
  if (!e_opt_field_visible(o, w->xt, w->yt, w->header, w->xw, w->yw, w->nw))
   continue;
  e_pr_str(o->xa + w->xt, o->ya + w->yt, w->header, o->ftt, w->nc, 1, o->fts, 0);
  e_opt_draw_text_field(o, i);
 }
 for (i = 0; i < o->nn; i++)
 {
  W_O_NUMSTR *n = o->nstr[i];
  if (!e_opt_field_visible(o, n->xt, n->yt, n->header, n->xw, n->yw, n->nw))
   continue;
  e_pr_str(o->xa + n->xt, o->ya + n->yt, n->header, o->ftt, n->nc, 1, o->fts, 0);
  e_schr_nzif(n->num, o->xa + n->xw, o->ya + n->yw, n->nw, o->fwt);
 }
 for (i = 0; i < o->sn; i++)
 {
  W_O_SSWSTR *s = o->sstr[i];
  if (!e_opt_element_visible(o, s->x, s->y, e_opt_glyph_hit_width(s->header)))
   continue;
  e_opt_draw_checkbox_label(o, i, OPT_LOOK_PLAIN);
  e_pr_str(o->xa + s->x, o->ya + s->y, "[ ]", o->fst, -1, 1, 0, 0);
  if (s->num)
   e_pr_char(o->xa + s->x + 1, o->ya + s->y, 'X', o->fst);
 }
 for (i = 0; i < o->pn; i++)
 {
  for (j = 0; j < o->pstr[i]->np; j++)
  {
   W_O_SPSWSTR *p = o->pstr[i]->ps[j];
   if (!e_opt_element_visible(o, p->x, p->y, e_opt_glyph_hit_width(p->header)))
    continue;
   e_pr_str(o->xa + p->x, o->ya + p->y, "( ) ", o->fst, -1, 1, 0, 0);
   e_opt_draw_radio_label(o, i, j, OPT_LOOK_PLAIN);
   e_pr_str(o->xa + p->x, o->ya + p->y, "( )", o->fst, -1, 1, 0, 0);
   if (o->pstr[i]->num == j)
    e_pr_char(o->xa + p->x + 1, o->ya + p->y, '*', o->fst);
  }
 }
 for (i = 0; i < o->bn; i++)
 {
  W_O_BTTSTR *b = o->bstr[i];
  if (!e_opt_element_visible(o, b->x, b->y, (int)strlen(b->header)))
   continue;
  e_opt_draw_button(o, i, OPT_LOOK_PLAIN);
 }
}

/**
 * e_opt_draw_marks - Refresh every [X] and (*) mark from the dialog's values.
 *
 * Drawn as Borland-style text glyphs in EVERY mode.  The former NEWSTYLE+X11
 * branch drew a 3D bevel via e_make_xrect_abs whose checked and unchecked
 * states differ only by a subtle highlight bit -- invisible to the user, so
 * toggling a box in xwpe looked like nothing happened (the value DID flip).
 * The 'X' / '*' glyphs match the first layout and the ncurses path, and
 * render clearly under Xft.
 */
static void e_opt_draw_marks(W_OPTSTR *o)
{
 int i, j;

 for (i = 0; i < o->sn; i++)
 {
  W_O_SSWSTR *s = o->sstr[i];
  if (!e_opt_element_visible(o, s->x, s->y, e_opt_glyph_hit_width(s->header)))
   continue;          /* the box is cut there: no widget, so no mark */
  if (s->num)
   e_pr_char(o->xa + s->x + 1, o->ya + s->y, 'X', o->fst);
  else
   e_pr_char(o->xa + s->x + 1, o->ya + s->y, ' ', o->fst);
 }
 for (i = 0; i < o->pn; i++)
 {
  for (j = 0; j < o->pstr[i]->np; j++)
  {
   W_O_SPSWSTR *p = o->pstr[i]->ps[j];
   if (!e_opt_element_visible(o, p->x, p->y, e_opt_glyph_hit_width(p->header)))
    continue;         /* the box is cut there: no widget, so no mark */
   if (o->pstr[i]->num == j)
    e_pr_char(o->xa + p->x + 1, o->ya + p->y, '*', o->fst);
   else
    e_pr_char(o->xa + p->x + 1, o->ya + p->y, ' ', o->fst);
  }
 }
}

/* What the loop does with the widget code it carries: give that widget the
   focus (highlight it and wait for a key) or activate it (toggle the checkbox,
   select the radio, press the button).  A mouse click or an accelerator letter
   asks for activation; everything else -- a focus move, Space on a toggle, an
   unknown key, leaving the move mode, a redraw after a resize -- asks for the
   focus only.  Keeping the two apart is what stops a mere refocus from
   flipping the checkbox that happens to be under the focus. */
#define OPT_ACTIVATE 0
#define OPT_FOCUS    1

/* Answer of a button run: go on with the loop, or redraw the box in place. */
#define OPT_CONTINUE 0
#define OPT_RESTART  1

/* The input state the dialog loop carries from one key to the next. */
struct opt_run {
 int c;      /* the key or widget code to dispatch next */
 int cold;   /* the widget that had the focus last */
 int sw;     /* OPT_FOCUS or OPT_ACTIVATE, the meaning of c */
 int ret;    /* the dialog's result: 0 applies, WPE_ESC cancels, else a button's code */
};

/**
 * e_opt_refocus - Give the focus back to the widget that had it.
 *
 * Used after anything that is neither a click nor an accelerator: an unknown
 * key, Space on a toggle, leaving the keyboard move mode, a button action
 * that kept the dialog open.  The widget is highlighted again and waits for
 * the next key; it is never activated, so moving the box or pressing a key
 * the dialog does not know cannot flip the checkbox under the focus.
 */
static void e_opt_refocus(struct opt_run *r)
{
 r->c = r->cold;
 r->sw = OPT_FOCUS;
}

/**
 * e_opt_cancel - End the dialog without applying anything.
 *
 * Esc from whatever widget has the focus, and the drawn close box (which the
 * mouse layer reports as Esc), both mean "forget it": the loop ends and the
 * caller sees WPE_ESC, so nothing is applied or saved.  One Esc is enough
 * from a checkbox or a radio as much as from a text field or a button; the
 * widget itself has no say in it.
 */
static void e_opt_cancel(struct opt_run *r)
{
 r->c = WPE_ESC;
 r->sw = OPT_ACTIVATE;
 r->ret = WPE_ESC;
}

/**
 * e_opt_submit_default - Enter on a non-button widget presses the default button.
 *
 * The Borland convention: Enter anywhere in the dialog is Ok.  The default
 * button's code is dispatched for activation, so its action runs (or the
 * dialog returns its code) exactly as if it had been clicked.
 */
static void e_opt_submit_default(W_OPTSTR *o, struct opt_run *r)
{
 r->c = o->crsw;
 r->sw = OPT_ACTIVATE;
}

/**
 * e_opt_press_button - Enter on a focused button presses that very button.
 *
 * A Cancel button carries the cancel key itself as its code, so pressing it
 * is the same as Esc: the dialog ends with nothing applied.  Any other button
 * is dispatched for activation, as a click on it would be.
 */
static void e_opt_press_button(W_OPTSTR *o, struct opt_run *r, int i)
{
 if (o->bstr[i]->sw == WPE_ESC)
 {
  e_opt_cancel(r);
  return;
 }
 r->c = o->bstr[i]->sw;
 r->sw = OPT_ACTIVATE;
}

/**
 * e_opt_focus_by_key - Turn a navigation key into the widget it leads to.
 * @x, @y: screen cell of the widget that has the focus now
 *
 * Tab, Shift-Tab and the arrows move the focus: the key is replaced by the
 * code of the widget they lead to, for focusing.  Any other key is kept as it
 * is, for activation: the loop then matches it against the accelerators (a
 * letter presses the widget whose hot letter it is) and, when nothing
 * matches, the focus simply returns to where it was.
 */
static void e_opt_focus_by_key(W_OPTSTR *o, struct opt_run *r, int x, int y)
{
 int key = r->c;

 r->c = e_get_opt_sw(key, x, y, o);
 if (r->c != key)
  r->sw = OPT_FOCUS;
 else
  r->sw = OPT_ACTIVATE;
}

/**
 * e_opt_read_key - Wait for the focused widget's next key or mouse action.
 *
 * A mouse report is resolved by the mouse layer into the code of the widget
 * under the pointer (to be activated, like an accelerator), into Esc for the
 * close box, or into -1 for a click that means nothing in this dialog.
 */
static int e_opt_read_key(W_OPTSTR *o)
{
 int c = e_opt_getch();

#if MOUSE
 if (c < 0)
  c = e_opt_mouse(o, c);
#endif
 return c;
}

/**
 * e_opt_code_matches - Does key or widget code @c name this widget?
 *
 * A widget is addressed either by its own code (the focus mover and the mouse
 * layer produce those) or by its accelerator letter, typed plain or with Alt.
 */
static int e_opt_code_matches(int c, int sw, int nc, const char *header)
{
 if (sw == c)
  return 1;
 if (nc < 0 || c < 0 || c > UCHAR_MAX)
  return 0;
 return toupper(c) == header[nc];
}

static int e_opt_find_text_field(W_OPTSTR *o, int c)
{
 int i;

 for (i = 0; i < o->wn; i++)
  if (e_opt_code_matches(c, o->wstr[i]->sw, o->wstr[i]->nc, o->wstr[i]->header))
   return i;
 return -1;
}

static int e_opt_find_num_field(W_OPTSTR *o, int c)
{
 int i;

 for (i = 0; i < o->nn; i++)
  if (e_opt_code_matches(c, o->nstr[i]->sw, o->nstr[i]->nc, o->nstr[i]->header))
   return i;
 return -1;
}

static int e_opt_find_checkbox(W_OPTSTR *o, int c)
{
 int i;

 for (i = 0; i < o->sn; i++)
  if (e_opt_code_matches(c, o->sstr[i]->sw, o->sstr[i]->nc, o->sstr[i]->header))
   return i;
 return -1;
}

/* Find the radio option named by @c; returns 1 and fills @group / @option. */
static int e_opt_find_radio(W_OPTSTR *o, int c, int *group, int *option)
{
 int i, j;

 for (i = 0; i < o->pn; i++)
  for (j = 0; j < o->pstr[i]->np; j++)
  {
   W_O_SPSWSTR *p = o->pstr[i]->ps[j];
   if (e_opt_code_matches(c, p->sw, p->nc, p->header))
   {
    *group = i;
    *option = j;
    return 1;
   }
  }
 return 0;
}

static int e_opt_find_button(W_OPTSTR *o, int c)
{
 int i;

 for (i = 0; i < o->bn; i++)
  if (e_opt_code_matches(c, o->bstr[i]->sw, o->bstr[i]->nc, o->bstr[i]->header))
   return i;
 return -1;
}

/**
 * e_opt_route_field_key - Act on the key that made the user leave a field.
 *
 * Enter submits the dialog through its default button, Esc cancels it, a
 * navigation key moves the focus, and anything else is offered to the
 * accelerators by the loop.
 */
static void e_opt_route_field_key(W_OPTSTR *o, struct opt_run *r, int x, int y)
{
 e_opt_focus_by_key(o, r, x, y);
 if (r->c == WPE_CR)
  e_opt_submit_default(o, r);
 else if (r->c == WPE_ESC)
  e_opt_cancel(r);
}

/**
 * e_opt_leaving_for_restart - Did a terminal resize end the widget's turn?
 *
 * A resize reaches the focused widget as the key WPE_RESIZE, and by then the
 * terminal layer has already laid the desk out again for the new screen: the
 * cells the widget was drawn in belong to the editor windows now, while the
 * box is torn down and drawn from scratch at the top of e_opt_kst_run.  So
 * the widget must not paint itself "plain" on the way out as it does when
 * the focus moves to a neighbour: its coordinates describe the old layout,
 * and the value of a Goto Line field or the face of an Ok button would stay
 * on the desktop, outside the new box, until something else repaints that
 * cell.
 */
static int e_opt_leaving_for_restart(const struct opt_run *r)
{
 return r->c == WPE_RESIZE;
}

/**
 * e_opt_focus_moved_on - Must the widget be repainted plain on the way out?
 *
 * True when the key moved the focus to another widget of a layout that still
 * stands; the widget then loses its highlight.  Not when the key names the
 * widget itself (it keeps the focus and is run again), nor after a resize
 * (see e_opt_leaving_for_restart).
 */
static int e_opt_focus_moved_on(const struct opt_run *r)
{
 if (e_opt_leaving_for_restart(r))
  return 0;
 return r->c != r->cold;
}

/**
 * e_opt_run_text_field - Let the user edit a text field until a key leaves it.
 *
 * The value is kept unless the user leaves with Esc; a mouse click outside the
 * field is handed to the mouse layer like any other click in the dialog.
 */
static void e_opt_run_text_field(W_OPTSTR *o, struct opt_run *r, int i)
{
 W_O_WRSTR *w = o->wstr[i];
 int x = o->xa + w->xw;
 int y = o->ya + w->yw;
 char *tmp = MALLOC((w->wmx + 1) * sizeof(char));
 int c;

 r->cold = r->c;
 /* The field holds at most wmx bytes and tmp has room for wmx + 1. */
 if (wpe_field_take(w->txt, tmp, (size_t)w->wmx + 1) != 0)
  tmp[0] = '\0';
 if (!w->df)
  c = e_schreib_leiste_lines(tmp, x, y, w->nw, w->wmx, o->fwt, o->fws,
                             w->lines_key);
 else
  c = e_schr_lst_wsv(tmp, x, y, w->nw, w->wmx, o->fwt, o->fws, w->df, o->f);
#if MOUSE
 if (c < 0)
  c = e_opt_mouse(o, c);
#endif
 if (c != WPE_ESC)
  strcpy(w->txt, tmp);
 r->c = c;
 e_opt_route_field_key(o, r, x, y);
 if (!e_opt_leaving_for_restart(r))
  e_opt_draw_text_field(o, i);
 free(tmp);
 fk_cursor(0);
}

/**
 * e_opt_run_num_field - Let the user edit a numeric field until a key leaves it.
 */
static void e_opt_run_num_field(W_OPTSTR *o, struct opt_run *r, int i)
{
 W_O_NUMSTR *n = o->nstr[i];
 int x = o->xa + n->xw;
 int y = o->ya + n->yw;
 int num = n->num;
 int c;

 r->cold = r->c;
 c = e_schreib_zif(&num, x, y, n->nw, o->fwt, o->fws);
#if MOUSE
 if (c < 0)
  c = e_opt_mouse(o, c);
#endif
 if (c != WPE_ESC)
  n->num = num;
 r->c = c;
 e_opt_route_field_key(o, r, x, y);
 if (e_opt_focus_moved_on(r))
  e_schr_nzif(n->num, x, y, n->nw, o->fwt);
 fk_cursor(0);
}

/**
 * e_opt_route_checkbox_key - Act on a key pressed while a checkbox has the focus.
 *
 * Space toggles it and keeps the focus (the mark is redrawn at the top of the
 * loop); Enter submits the dialog; Esc cancels it; a navigation key moves the
 * focus; anything else goes to the accelerators.
 */
static void e_opt_route_checkbox_key(W_OPTSTR *o, struct opt_run *r, int i)
{
 W_O_SSWSTR *s = o->sstr[i];

 if (r->c == ' ')
 {
  s->num = !s->num;
  e_opt_refocus(r);
 }
 else if (r->c == WPE_CR)
  e_opt_submit_default(o, r);
 else if (r->c == WPE_ESC)
  e_opt_cancel(r);
 else
  e_opt_focus_by_key(o, r, o->xa + s->x, o->ya + s->y);
}

/**
 * e_opt_run_checkbox - Activate a checkbox, or focus it and take one key.
 *
 * Activation (a click, an accelerator) flips the box and hands the focus back
 * to where it was.  Focusing highlights the label and waits for the user.
 */
static void e_opt_run_checkbox(W_OPTSTR *o, struct opt_run *r, int i)
{
 W_O_SSWSTR *s = o->sstr[i];

 if (r->sw == OPT_ACTIVATE)
 {
  s->num = !s->num;
  e_opt_refocus(r);
  return;
 }
 r->cold = r->c;
 e_opt_draw_checkbox_label(o, i, OPT_LOOK_FOCUSED);
 r->c = e_opt_read_key(o);
 e_opt_route_checkbox_key(o, r, i);
 if (e_opt_focus_moved_on(r))
  e_opt_draw_checkbox_label(o, i, OPT_LOOK_PLAIN);
}

/**
 * e_opt_route_radio_key - Act on a key pressed while a radio option has the focus.
 *
 * Space selects the option and keeps the focus.  Enter SELECTS it and submits
 * the dialog (Borland Enter=OK), so Down-to-it + Enter is one move and Space
 * first is optional.  Esc cancels; a navigation key moves the focus; anything
 * else goes to the accelerators.
 */
static void e_opt_route_radio_key(W_OPTSTR *o, struct opt_run *r, int i, int j)
{
 W_O_SPSWSTR *p = o->pstr[i]->ps[j];

 if (r->c == ' ')
 {
  o->pstr[i]->num = j;
  e_opt_refocus(r);
 }
 else if (r->c == WPE_CR)
 {
  o->pstr[i]->num = j;
  e_opt_submit_default(o, r);
 }
 else if (r->c == WPE_ESC)
  e_opt_cancel(r);
 else
  e_opt_focus_by_key(o, r, o->xa + p->x, o->ya + p->y);
}

/**
 * e_opt_run_radio - Activate a radio option, or focus it and take one key.
 */
static void e_opt_run_radio(W_OPTSTR *o, struct opt_run *r, int i, int j)
{
 if (r->sw == OPT_ACTIVATE)
 {
  o->pstr[i]->num = j;
  e_opt_refocus(r);
  return;
 }
 r->cold = r->c;
 e_opt_draw_radio_label(o, i, j, OPT_LOOK_FOCUSED);
 r->c = e_opt_read_key(o);
 e_opt_route_radio_key(o, r, i, j);
 if (e_opt_focus_moved_on(r))
  e_opt_draw_radio_label(o, i, j, OPT_LOOK_PLAIN);
}

/**
 * e_opt_activate_button - Press a button and act on what its action answers.
 *
 * A button without an action ends the dialog with the button's own code
 * (Ok / Cancel).  An action that returns a positive value ends the dialog
 * with it; zero keeps the dialog open with the focus back where it was; a
 * negative value means the action floated an overlay over the dialog (a
 * scrollable picker) and wants the whole box repainted in place, because the
 * overlay's own restore cannot rebuild it under the panel/schirm split.
 */
static int e_opt_activate_button(W_OPTSTR *o, struct opt_run *r, int i)
{
 W_O_BTTSTR *b = o->bstr[i];

 if (b->fkt == NULL)
 {
  r->ret = b->sw;
  r->c = WPE_ESC;
  return OPT_CONTINUE;
 }
 r->ret = b->fkt(o->f);
 if (r->ret > 0)
 {
  r->c = WPE_ESC;
  return OPT_CONTINUE;
 }
 if (r->ret < 0)
  return OPT_RESTART;
 e_opt_refocus(r);
 e_opt_draw_button(o, i, OPT_LOOK_PLAIN);
 return OPT_CONTINUE;
}

/**
 * e_opt_route_button_key - Act on a key pressed while a button has the focus.
 *
 * Enter presses this button, Esc cancels the dialog, a navigation key moves
 * the focus, anything else goes to the accelerators.
 */
static void e_opt_route_button_key(W_OPTSTR *o, struct opt_run *r, int i)
{
 W_O_BTTSTR *b = o->bstr[i];

 if (r->c == WPE_CR)
  e_opt_press_button(o, r, i);
 else if (r->c == WPE_ESC)
  e_opt_cancel(r);
 else
  e_opt_focus_by_key(o, r, o->xa + b->x, o->ya + b->y);
}

/**
 * e_opt_run_button - Press a button, or focus it and take one key.
 *
 * Returns OPT_RESTART when the button's action asks for an in-place redraw of
 * the dialog, OPT_CONTINUE otherwise.
 */
static int e_opt_run_button(W_OPTSTR *o, struct opt_run *r, int i)
{
 e_opt_draw_button(o, i, OPT_LOOK_FOCUSED);
 if (r->sw == OPT_ACTIVATE)
  return e_opt_activate_button(o, r, i);
 r->cold = r->c;
 r->c = e_opt_read_key(o);
 e_opt_route_button_key(o, r, i);
 if (e_opt_focus_moved_on(r))
  e_opt_draw_button(o, i, OPT_LOOK_PLAIN);
 return OPT_CONTINUE;
}

/**
 * e_opt_is_move_key - Is @c the key that starts the keyboard move mode?
 *
 * Alt-F2 with the classic key style, Ctrl-L with the CUA style.
 */
static int e_opt_is_move_key(W_OPTSTR *o, int c)
{
 return wpe_binding_action(c, wpe_binding_style(o->f->ed->edopt),
                           WPE_CTX_DIALOG_MOVE) == WPE_ACT_SIZE_MOVE;
}

/**
 * e_opt_box_draw - Draw the dialog where the layout owner placed it.
 * @box: the dialog's box.
 *
 * The owner calls this when the dialog opens, after a resize (it places the
 * dialog by its rule for the new screen first) and when a picker that floated
 * over the dialog has gone.  The frame, the widgets and their marks are all
 * painted from the dialog's own state, so what the user typed is kept.
 * Return: WPE_BOX_DRAWN, or WPE_BOX_NO_MEMORY.
 */
static WpeBoxDrawn e_opt_box_draw(WpeBox *box)
{
 W_OPTSTR *o = box->self;

 o->xa = box->at.bx_left;
 o->ya = box->at.bx_top;
 o->xe = box->at.bx_right;
 o->ye = box->at.bx_bottom;
 if (wpe_box_frame(box, o->name, o->frt, o->ftt, o->frs) != 0)
 {
  o->pic = NULL;
  return(WPE_BOX_NO_MEMORY);
 }
 o->pic = box->pic;
 e_opt_draw_widgets(o);
 e_opt_draw_marks(o);
 return(WPE_BOX_DRAWN);
}

/**
 * e_opt_box_init - Give the dialog its placement rule and its draw hook.
 * @o: the dialog, with the size its author gave it in xa..ye.
 * @box: receives the box.
 *
 * The dialog is centred on the window under it, as it has been since 1.5.30a.
 */
static void e_opt_box_init(W_OPTSTR *o, WpeBox *box)
{
 memset(box, 0, sizeof *box);
 box->what = "a dialog";
 box->self = o;
 box->draw = e_opt_box_draw;
 box->refresh = e_opt_box_refresh;
 box->place.anchor = WPE_BOX_CENTRED_WINDOW;
 box->place.w = o->xe - o->xa + 1;
 box->place.h = o->ye - o->ya + 1;
 o->box = box;
}

/**
 * e_opt_refocus_first - Put the focus on the dialog's first widget.
 *
 * After a resize, or when a picker floated over the dialog, the focus goes
 * to the first widget (c == 0 lets the layout pick it) and in FOCUS mode:
 * a redraw is not a click, so the widget it lands on is highlighted, never
 * toggled -- a resize used to flip the first checkbox.
 */
static void e_opt_refocus_first(W_OPTSTR *o, struct opt_run *r)
{
 r->c = e_get_opt_sw(CDO, 0, 0, o);
 r->cold = r->c;
 r->sw = OPT_FOCUS;
}

/**
 * e_opt_wait_esc_or_resize - Hold a dialog the screen cannot show.
 *
 * When the frame had to be clipped, nothing in it is safe to operate, so
 * every key is swallowed until the user gives up (Esc) or the terminal grows
 * (a resize, after which the owner has laid the box out again).
 */
static int e_opt_wait_esc_or_resize(void)
{
 int c;

 do
  c = e_opt_getch();
 while (c != WPE_ESC && c != WPE_RESIZE);
 return c;
}

/**
 * e_opt_hold_clipped - Hold the dialog while the screen is too small for it.
 * @o: the dialog; @box: its box; @r: the loop's state.
 *
 * A dialog the frame of which was cut shows no widget that could be
 * operated, so the keys are swallowed until the user gives up (Esc, the
 * answer is then WPE_ESC) or the screen is made big enough, when the dialog
 * is whole again with the focus on its first widget.
 * Return: WPE_ESC when the user gave up, else 0.
 */
static int e_opt_hold_clipped(W_OPTSTR *o, WpeBox *box, struct opt_run *r)
{
 while (!box->fits)
 {
  if (e_opt_wait_esc_or_resize() == WPE_ESC)
   return(WPE_ESC);
 }
 e_opt_refocus_first(o, r);
 return(0);
}

/**
 * e_opt_after_resize - The owner laid the dialog out again: go on.
 * @o: the dialog; @box: its box; @r: the loop's state.
 *
 * The owner has placed the dialog for the new screen and drawn its frame
 * and widgets; what a field held while the key arrived is drawn again here,
 * and the focus goes to the first widget.
 * Return: WPE_ESC when the new screen is too small and the user gave up.
 */
static int e_opt_after_resize(W_OPTSTR *o, WpeBox *box, struct opt_run *r)
{
 e_opt_draw_widgets(o);
 e_opt_refocus_first(o, r);
 if (!box->fits)
  return(e_opt_hold_clipped(o, box, r));
 return(0);
}

/**
 * e_opt_redraw_in_place - Draw the dialog again where it stands.
 * @o: the dialog; @box: its box; @r: the loop's state.
 *
 * A button's action floated a picker over the dialog and the picker's own
 * restore cannot rebuild the dialog under it; the owner draws the dialog
 * again at the place the user left it, without centring it anew.
 * Return: WPE_BOX_DRAWN, or WPE_BOX_NO_MEMORY.
 */
static WpeBoxDrawn e_opt_redraw_in_place(W_OPTSTR *o, WpeBox *box, struct opt_run *r)
{
 WpeBoxDrawn drawn = wpe_box_redraw(box);

 e_opt_refocus_first(o, r);
 return(drawn);
}

/**
 * e_opt_close - Take the box off the screen and give the editor back.
 */
static void e_opt_close(W_OPTSTR *o, WpeBox *box)
{
 e_close_view(box->pic, 1);
 box->pic = NULL;
 o->pic = NULL;
 o->box = NULL;
 e_mouse_flush();
 e_repaint_desk_nopic(o->f->ed->f[o->f->ed->mxedt]);
 wpe_box_close(box);
}

static int e_opt_kst_run(W_OPTSTR *o)
{
   struct opt_run r;
   WpeBox box;
   int i, j;

   r.c = o->bgsw;
   r.cold = 0;
   r.sw = OPT_FOCUS;
   r.ret = 0;
   fk_cursor(0);
   e_opt_box_init(o, &box);
   if (wpe_box_open(&box) != WPE_BOX_DRAWN)
   {
      o->box = NULL;
      e_error(e_msg[ERR_LOWMEM], 0, o->f->fb);
      return(-1);
   }
   if(!r.c) r.c = e_get_opt_sw(CDO, 0, 0, o);
   r.cold = r.c;
   if (!box.fits && e_opt_hold_clipped(o, &box, &r) == WPE_ESC)
   {
      e_opt_close(o, &box);
      return(WPE_ESC);
   }
   /* The loop ends on Esc in activate mode only: Esc in focus mode names the
      Cancel button (its code is WPE_ESC) and merely focuses it. */
   while (r.c != WPE_ESC || r.sw == OPT_FOCUS)
   {
      if (r.c == WPE_RESIZE)
      {
         if (e_opt_after_resize(o, &box, &r) == WPE_ESC)
         {
            e_opt_close(o, &box);
            return(WPE_ESC);
         }
         continue;
      }
      e_opt_draw_marks(o);
      if (e_opt_is_move_key(o, r.c))
      {
         e_opt_move(o);
         e_opt_refocus(&r);
         continue;
      }
      i = e_opt_find_text_field(o, r.c);
      if (i >= 0)
      {
         e_opt_run_text_field(o, &r, i);
         continue;
      }
      i = e_opt_find_num_field(o, r.c);
      if (i >= 0)
      {
         e_opt_run_num_field(o, &r, i);
         continue;
      }
      i = e_opt_find_checkbox(o, r.c);
      if (i >= 0)
      {
         e_opt_run_checkbox(o, &r, i);
         continue;
      }
      if (e_opt_find_radio(o, r.c, &i, &j))
      {
         e_opt_run_radio(o, &r, i, j);
         continue;
      }
      i = e_opt_find_button(o, r.c);
      if (i >= 0)
      {
         if (e_opt_run_button(o, &r, i) == OPT_RESTART)
         {
            /* In place: the dialog keeps the position the user gave it. */
            if (e_opt_redraw_in_place(o, &box, &r) != WPE_BOX_DRAWN)
            {
               e_error(e_msg[ERR_LOWMEM], 0, o->f->fb);
               e_opt_close(o, &box);
               return(-1);
            }
         }
         continue;
      }
      e_opt_refocus(&r);
   }
   e_opt_close(o, &box);
   return(r.ret);
}

/* e_opt_kst - public entry to the modal dialog/picker/menu engine.  The box
   owns the screen for its whole lifetime, so program output, a stop, a reply or
   a server update arriving mid-dialog waits for it to close instead of drawing
   under it.  This box pushes no window onto the editor stack; the layout owner
   (wpe_box_open) tells the screen's owner (we_paint.c) and lays the box out
   again when the screen changes size. */
static int e_opt_kst_as(W_OPTSTR *o, WpeTestCtx kind);

int e_opt_kst(W_OPTSTR *o)
{
   return(e_opt_kst_as(o, WPE_TEST_CTX_DIALOG));
}

/**
 * e_opt_kst_message - Run a message box built on the dialog engine.
 * @o: the box.
 *
 * The same box as a dialog for the user; a test watching the editor's state
 * is told the keys now answer a message (Ok, Yes, No) and not a dialog.
 * Return: as e_opt_kst.
 */
int e_opt_kst_message(W_OPTSTR *o)
{
   return(e_opt_kst_as(o, WPE_TEST_CTX_MESSAGE));
}

/**
 * e_opt_kst_as - Run the box @o; its keys are read for @kind.
 * Return: as e_opt_kst.
 */
static int e_opt_kst_as(W_OPTSTR *o, WpeTestCtx kind)
{
   int ret;
   wpe_test_ctx_enter(kind, o->name);
   ret = e_opt_kst_run(o);
   wpe_test_ctx_leave();
   return(ret);
}

/**
 * e_opt_take_print_cmd - The print command the user set in Options.
 * @cn: the editor; @text: the field's text.
 *
 * A command he typed there is his own: printing does not ask about it.  A
 * field he left alone is not typed, and neither is a folder's command too
 * long for the field: it was cut to fit, so it is never approved here.
 */
static void e_opt_take_print_cmd(ECNT *cn, const char *text)
{
 int typed = cn->print_cmd && strlen(cn->print_cmd) <= E_OPT_PRINT_FIELD_ROOM &&
             strcmp(cn->print_cmd, text) != 0;

 if (cn->print_cmd)
  WpeFree(cn->print_cmd);
 cn->print_cmd = WpeStrdup(text);
 if (typed)
  e_startcmd_typed(e_opt_folder_dir, WPE_STARTCMD_KEY_PRINT, text);
}

/**
 * e_opt_block_wordstar - Do Ctrl-K B and Ctrl-K K work as two independent
 * markers under the options @edopt?
 *
 * The selection mode wins over a WordStar bit left in a hand-edited options
 * file, so the editor never works in two block modes at once and the dialog
 * never shows them.  Every reader of the WordStar bit asks here.
 */
int e_opt_block_wordstar(int edopt)
{
 return((edopt & ED_BLOCK_WORDSTAR) && !(edopt & ED_BLOCK_SELECTION));
}

/** e_opt_wordstar_ticked - Is the WordStar blocK box ticked when the dialog opens? */
static int e_opt_wordstar_ticked(int edopt)
{
 return(e_opt_block_wordstar(edopt));
}

/**
 * e_opt_block_mode_bits - The block mode the Editor-Options boxes ask for.
 * @before: the options the dialog opened on; @wordstar, @selection: the two
 * block boxes as they stand at Ok.
 *
 * The block mode has three levels (anchor, WordStar, selection) and the
 * dialog shows two boxes, so both can be ticked: the one the user ticked in
 * this dialog wins, and the other is cleared.  A user who ticks Selection
 * while WordStar is on gets Selection, and the other way round.
 * Return: ED_BLOCK_WORDSTAR, ED_BLOCK_SELECTION or 0 (anchor).
 */
static int e_opt_block_mode_bits(int before, int wordstar, int selection)
{
 if (wordstar && selection)
 {
  if (before & ED_BLOCK_SELECTION)
   return(ED_BLOCK_WORDSTAR);
  return(ED_BLOCK_SELECTION);
 }
 if (selection)
  return(ED_BLOCK_SELECTION);
 if (wordstar)
  return(ED_BLOCK_WORDSTAR);
 return(0);
}

/**
 * e_opt_keys_radio_preset - Let a change of the Keys radio bring its preset.
 * @before: the options the dialog opened on; @taken: what the boxes say at Ok.
 *
 * A user who switches the Keys radio gets the bits that style comes with
 * (e_keys_preset); one who also sets the selection, trim or last-line-break
 * box in the same dialog keeps exactly what he set.
 * Return: the options to keep.
 */
static int e_opt_keys_radio_preset(int before, int taken)
{
 int boxes = ED_BLOCK_SELECTION | ED_TRIM_AUTO_INDENT | ED_KEEP_FILE_ENDING;

 if (((before ^ taken) & ED_CUA_STYLE) == 0)
  return(taken);
 if (((before ^ taken) & boxes) != 0)
  return(taken);
 return(e_keys_preset(taken, (taken & ED_CUA_STYLE) != 0));
}

int e_edt_options(FENSTER *f)
{
 int i, ret, edopt = f->ed->edopt;
 W_OPTSTR *o = e_init_opt_kst(f);

 if (!o) return(-1);
 o->xa = 15;  o->ya = 3;  o->xe = 64;  o->ye = 22;
 o->bgsw = AltO;
 o->name = "Editor-Options";
 o->crsw = AltO;
 e_add_txtstr(3, 2, "Display:", o);
 e_add_txtstr(25, 2, "Autosave:", o);
 e_add_txtstr(25, 6, "Keys:", o);
 e_add_txtstr(25, 10, "Auto-Indent:", o);
 e_add_txtstr(3, 5, "Tile:", o);
 e_add_numstr(3, 8, 19, 8, 3, 100, 0, AltM, "Max. Columns:", f->ed->maxcol, o);
 e_add_numstr(3, 9, 20, 9, 2, 100, 0, AltT, "Tabstops:", f->ed->tabn, o);
 e_add_numstr(3, 10, 19, 10, 3, 1000, 2, AltX, "MaX. Changes:", f->ed->maxchg, o);
 e_add_numstr(3, 11, 20, 11, 2, 100, 0, AltN, "Num. Undo:", f->ed->numundo, o);
 e_add_numstr(3, 12, 20, 12, 2, 100, 5, AltI, "Auto Ind. Col.:", f->ed->autoindent, o);
 e_add_sswstr(4, 3, 0, AltS, f->ed->edopt & ED_SHOW_ENDMARKS ? 1 : 0, "Show Endmark  ", o);
 e_add_sswstr(26, 3, 1, AltP, f->ed->autosv & 1, "OPtions         ", o);
 e_add_sswstr(26, 4, 1, AltH, f->ed->autosv & 2 ? 1 : 0, "CHanges         ", o);
 e_add_sswstr(4, 6, 2, AltD, f->ed->edopt & ED_OLD_TILE_METHOD ? 1 : 0, "OlD Style     ", o);
 e_add_sswstr(4, 4, 13, AltK, e_opt_wordstar_ticked(f->ed->edopt), "WordStar blocK", o);
 /* The AI assistant on/off + backend/model/policy now live in Options -> AI. */
 e_add_sswstr(4, 7, 3, AltE, f->ed->edopt & ED_BLOCK_SELECTION ? 1 : 0, "SelEction blocK", o);
 e_add_sswstr(4, 13, 5, AltA, f->ed->edopt & ED_TRIM_AUTO_INDENT ? 1 : 0, "Trim Auto-indent", o);
 e_add_sswstr(4, 16, 5, AltF, f->ed->edopt & ED_KEEP_FILE_ENDING ? 1 : 0, "Keep Final newline", o);
 e_add_pswstr(0, 26, 7, 3, AltL, 0, "BorLand-Style    ", o);
 e_add_pswstr(0, 26, 8, 12, AltC, f->ed->edopt & ED_CUA_STYLE, "Modern (VS Code) ", o);
 e_add_pswstr(1, 26, 11, 3, AltY, 0, "OnlY Source-Text", o);
 e_add_pswstr(1, 26, 12, 2, AltW, 0, "AlWays          ", o);
 e_add_pswstr(1, 26, 13, 2, AltV,
   (f->ed->edopt & ED_ALWAYS_AUTO_INDENT ? 1 : f->ed->edopt & ED_SOURCE_AUTO_INDENT ? 0 : 2),
   "NeVer           ", o);
 e_add_wrstr(3, 14, 3, 15, 44, E_OPT_PRINT_FIELD_ROOM, 1, AltR, "PRint Command:",
   f->ed->print_cmd, NULL, o);
 e_add_bttstr(12, 17, 1, AltO, " Ok ", NULL, o);
 e_add_bttstr(31, 17, -1, WPE_ESC, "Cancel", NULL, o);
 ret = e_opt_kst(o);
 if (ret != WPE_ESC)
 {
  f->ed->autosv = o->sstr[1]->num + (o->sstr[2]->num << 1);
  f->ed->maxcol = o->nstr[0]->num;
  f->ed->tabn = o->nstr[1]->num;
  f->ed->maxchg = o->nstr[2]->num;
  f->ed->numundo = o->nstr[3]->num;
  f->ed->autoindent = o->nstr[4]->num;
  f->ed->edopt = ((f->ed->edopt & ~ED_EDITOR_OPTIONS) | o->pstr[0]->num) |
    (o->pstr[1]->num == 0 ? ED_SOURCE_AUTO_INDENT : 0) |
    (o->pstr[1]->num == 1 ? ED_ALWAYS_AUTO_INDENT : 0) |
    (o->sstr[3]->num ? ED_OLD_TILE_METHOD : 0) |
    (o->sstr[0]->num ? ED_SHOW_ENDMARKS : 0) |
    e_opt_block_mode_bits(edopt, o->sstr[4]->num, o->sstr[5]->num) |
    (o->sstr[6]->num ? ED_TRIM_AUTO_INDENT : 0) |
    (o->sstr[7]->num ? ED_KEEP_FILE_ENDING : 0);
  f->ed->edopt = e_opt_keys_radio_preset(edopt, f->ed->edopt);
  if ((edopt ^ f->ed->edopt) & E_KEYS_MODERN_BITS)
   e_keys_session_release();   /* what he sets here is his, and is saved */
  e_opt_take_print_cmd(f->ed, o->wstr[0]->txt);
  if (edopt != f->ed->edopt)
  {
   e_switch_blst(f->ed);
#ifdef WPE_AI
   e_refresh_editor_bars(f->ed);   /* show/hide the "Alt-B AI" bar entry live */
#endif
   for (i = 0; i <= f->ed->mxedt; i++)
    if ((f->ed->edopt & ED_ALWAYS_AUTO_INDENT) ||
      ((f->ed->edopt & ED_SOURCE_AUTO_INDENT) && f->ed->f[i]->c_st))
     f->ed->f[i]->flg = 1;
    else
     f->ed->f[i]->flg = 0;
   e_repaint_desk(f);
  }
 }
 freeostr(o);
 return(0);
}

int e_read_help_str()
{
 FILE *fp;
 char str[128];
 int i, len;
 char *path = e_lib_file("help.key");

 for (i = 0; i < E_HLP_NUM; i++)
 {
  e_hlp_str[i] = MALLOC(sizeof(char));
  *e_hlp_str[i] = '\0';
 }
 fp = path ? fopen(path, "rb") : NULL;
 FREE(path);
 if (!fp)
  return(-1);
 for (i = 0; i < E_HLP_NUM && fgets(str, 128, fp); i++)
 {
  len = strlen(str);
  if (str[len-1] == '\n')
   str[--len] = '\0';
  e_hlp_str[i] = REALLOC(e_hlp_str[i], (len+1)*sizeof(char));
  strcpy(e_hlp_str[i], str);
 }
 fclose(fp);
 return(i == E_HLP_NUM ? 0 : -2);
}

