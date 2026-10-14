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

/*    About WE      */
int e_about_WE(FENSTER *f)
{
 PIC *pic = NULL;
 int xa = 8, ya = 3, xe = xa + 54, ye = ya + 17;
 char tmp[60];

 fk_cursor(0);
 pic = e_std_kst(xa, ya, xe, ye, NULL, 1, f->fb->nr.fb, f->fb->nt.fb, f->fb->ne.fb);
 if (pic == NULL)
 {
  e_error(e_msg[ERR_LOWMEM], 1, f->fb);
  return(WPE_ESC);
 }

 sprintf(tmp, "Version %s", VERSION);
#ifdef UNIX
 if (WpeIsXwin() && WpeIsProg())
  e_pr_str(xa+9, ya+2, "XWindow Programming Environment", f->fb->et.fb, 0, 0, 0, 0);
 else if (WpeIsProg())
  e_pr_str(xa+10, ya+2, "Window Programming Environment", f->fb->et.fb, 0, 0, 0, 0);
 else if (WpeIsXwin())
  e_pr_str(xa+15, ya+2, "XWindow Editor", f->fb->et.fb, 0, 0, 0, 0);
 else
#endif
  e_pr_str(xa+17, ya+2, "Window Editor", f->fb->et.fb, 0, 0, 0, 0);
 e_pr_str(xa + (54 - strlen(tmp)) / 2, ya+3, tmp, f->fb->et.fb, 0, 0, 0, 0);
 e_pr_str(xa+2, ya+5, "Created by Fred Kruse, 1993", f->fb->nt.fb, 0, 0, 0, 0);
 e_pr_str(xa+2, ya+6, "Continued by Dennis Payne, 2000-2006", f->fb->nt.fb, 0, 0, 0, 0);
 e_pr_str(xa+2, ya+7, "Maintained by Juan Manuel Mendez Rey, 2026", f->fb->nt.fb, 0, 0, 0, 0);
 e_pr_str(xa+2, ya+8, "https://codeberg.org/mendezr/xwpe", f->fb->nt.fb, 0, 0, 0, 0);
 e_pr_str(xa+2, ya+10, "Copyright (C) 1993-2026", f->fb->nt.fb, 0, 0, 0, 0);
 e_pr_str(xa+2, ya+11, "This software comes with ABSOLUTELY NO WARRANTY;", f->fb->nt.fb, 0, 0, 0, 0);
 e_pr_str(xa+2, ya+12, "This is free software, and you are welcome to", f->fb->nt.fb, 0, 0, 0, 0);
 e_pr_str(xa+2, ya+13, "redistribute it under certain conditions;", f->fb->nt.fb, 0, 0, 0, 0);
 e_pr_str(xa+2, ya+14, "See 'Help\\Editor\\GNU Pub...' for details.", f->fb->nt.fb, 0, 0, 0, 0);
#if  MOUSE
 while (e_mshit() != 0)
  ;
 e_getch();
 while (e_mshit() != 0)
  ;
#else
 e_getch();
#endif
 e_close_view(pic, 1);
 return(0);
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
 * e_pr_text - Print a plain string at (x, y) in a single color.
 * @x:   Screen column.
 * @y:   Screen row.
 * @str: The string to display.
 * @col: Color attribute (FARBE index).
 *
 * Convenience wrapper around e_pr_str() for the common case where
 * no highlighting, shadow, or button styling is needed (the last 4
 * arguments are all 0).
 */
static void e_pr_text(int x, int y, char *str, int col)
{
 e_pr_str(x, y, str, col, 0, 0, 0, 0);
}

/**
 * e_pr_str_wrap - Print a string with word-wrap across multiple lines.
 * @x:      Screen column to start printing on each line.
 * @y:      Screen row of the first line.
 * @str:    The string to display.
 * @width:  Maximum characters per line.
 * @col:    Color attribute (FARBE index) for the text.
 *
 * Prints @str starting at (@x, @y).  If the string is longer than
 * @width, it continues on the next row at column @x.
 *
 * Return: Number of rows used (1 if no wrap needed).
 */
static int e_pr_str_wrap(int x, int y, char *str, int width, int col)
{
 char buf[512];
 int len = strlen(str);
 int rows = 0;
 int pos = 0;

 if (width <= 0) return(0);
 while (pos < len)
 {
  int chunk = len - pos;
  if (chunk > width) chunk = width;
  if (chunk > 511) chunk = 511;
  strncpy(buf, str + pos, chunk);
  buf[chunk] = '\0';
  e_pr_text(x, y + rows, buf, col);
  pos += chunk;
  rows++;
 }
 return(rows > 0 ? rows : 1);
}

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
 PIC *pic = NULL;
 char tmp[512];
 char *filepath;
 int xa = 4, ya = 3, xe, ye;
 int avail, file_rows, dir_rows, row;

 /* Use most of the screen width */
 xe = MAXSCOL - 4;
 if (xe < xa + 40) xe = xa + 40;
 avail = xe - xa - 24;  /* chars available for content values */

 /* Determine what to show for Current File */
 if (strcmp(f->datnam, "Clipboard") != 0 &&
     strcmp(f->dirct, f->ed->dirct) != 0)
 {
  snprintf(tmp, sizeof(tmp), "%s%s%s", f->dirct, DIRS, f->datnam);
  filepath = tmp;
 }
 else
  filepath = f->datnam;

 /* Calculate rows needed for wrapping */
 file_rows = ((int)strlen(filepath) + avail - 1) / avail;
 if (file_rows < 1) file_rows = 1;
 dir_rows = ((int)strlen(f->ed->dirct) + avail - 1) / avail;
 if (dir_rows < 1) dir_rows = 1;

 /* Dialog height: 2 margin + file label + file rows + gap + dir label +
    dir rows + gap + numfiles row + 2 margin */
 ye = ya + 4 + file_rows + dir_rows + 2;
 if (ye > MAXSLNS - 2) ye = MAXSLNS - 2;

 fk_cursor(0);
 pic = e_std_kst(xa, ya, xe, ye, " Information ", 1, f->fb->nr.fb, f->fb->nt.fb, f->fb->ne.fb);
 if (pic == NULL) {  e_error(e_msg[ERR_LOWMEM], 1, f->fb); return(WPE_ESC);  }

 row = ya + 2;
 { int col = f->fb->nt.fb;

 e_pr_text(xa+3, row, " Current File: ", col);
 if (strcmp(f->datnam, "Clipboard") != 0)
  row += e_pr_str_wrap(xa+23, row, filepath, avail, col);
 else
  row++;
 row++;

 e_pr_text(xa+3, row, " Current Directory: ", col);
 row += e_pr_str_wrap(xa+23, row, f->ed->dirct, avail, col);
 row++;

 e_pr_text(xa+3, row, " Number of Files: ", col);
 e_pr_text(xa+23, row,
   WpeNumberToString(f->ed->mxedt, WpeNumberOfPlaces(f->ed->mxedt), tmp), col);
 }

#if  MOUSE
 while(e_mshit() != 0);
 e_getch();
 while(e_mshit() != 0);
#else
 e_getch();
#endif
 e_close_view(pic, 1);
 return(0);
}

/*   color adjustments  */
int e_ad_colors(FENSTER *f)
{
 int n, xa = 48, ya = 2, num = 4;
 OPTK *opt = MALLOC(num * sizeof(OPTK));

 opt[0].t = "Editor Colors";     opt[0].x = 0;  opt[0].o = 'E';
 opt[1].t = "Desk Colors";       opt[1].x = 0;  opt[1].o = 'D';
 opt[2].t = "Option Colors";     opt[2].x = 0;  opt[2].o = 'O';
 opt[3].t = "Progr. Colors";     opt[3].x = 0;  opt[3].o = 'P';

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
  if ((c = e_getch()) == -1)
   c = e_opt_cw_mouse(xa, ya, md);
#else
  c = e_getch();
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
		  "Text", "Res. Wrd.", "Constants", "Pre-Proc.", "Comments"
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
  if ((c=e_getch()) == -1) c = e_opt_ck_mouse(xa, ya, md);
#else
  c = e_getch();
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
  }
 }
 frb[sw] = e_s_clr(frb[sw].f, frb[sw].b);
}

/*   Save - Options - Menu   */
int e_opt_save(FENSTER *f)
{
 int ret;
 char tmp[256];

 strcpy(tmp, f->ed->optfile);
 ret = e_add_arguments(tmp, "Save Option File", f, 0, AltS, NULL);
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

int WpeReadGeneral(ECNT *cn, char *section, char *option, char *value)
{
 if (WpeStrccmp("Data", option) == 0)
  cn->dtmd = atoi(value);
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
 fprintf(opt_file, "Data : %d\n", cn->dtmd);
 fprintf(opt_file, "Autosave : %d\n", cn->autosv);
 fprintf(opt_file, "MaxColumn : %d\n", cn->maxcol);
 fprintf(opt_file, "Tab : %d\n", cn->tabn);
 fprintf(opt_file, "MaxChanges : %d\n", cn->maxchg);
 fprintf(opt_file, "NumUndo : %d\n", cn->numundo);
 fprintf(opt_file, "Options1 : %d\n", cn->flopt);
 fprintf(opt_file, "Options2 : %d\n", cn->edopt);
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
 if (wpe_ai_cfg_int_to_save(WPE_AI_CFG_HOST_ADAPTER) == WPE_AI_HOST_ADAPTER_TEXT)
  fprintf(opt_file, "AIHostAdapter : text\n");
 fprintf(opt_file, "AICAFile : %s\n", wpe_ai_cfg_str_to_save(WPE_AI_CFG_CAFILE));
 if (*wpe_ai_cfg_str_to_save(WPE_AI_CFG_SEARCH_URL))
  fprintf(opt_file, "AISearchURL : %s\n", wpe_ai_cfg_str_to_save(WPE_AI_CFG_SEARCH_URL));
 /* The tool time limit is written only when the user set one, so an xwperc
    that never named it stays as it was. */
 if (wpe_ai_cfg_int_to_save(WPE_AI_CFG_TOOL_TIMEOUT) != WPE_AI_TOOL_TIMEOUT_DEFAULT)
  fprintf(opt_file, "AIToolTimeout : %d\n", wpe_ai_cfg_int_to_save(WPE_AI_CFG_TOOL_TIMEOUT));
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
#ifdef WPE_AI_AGENT_HOST
 fprintf(opt_file, "AIAgentEngine : %s\n",
         wpe_ai_cfg_int_to_save(WPE_AI_CFG_AGENT_ENGINE) == WPE_AI_ENGINE_CLAUDE_HOST
           ? "claude-code" : "builtin");
#endif
#endif
 return 0;
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
  fprintf(opt_file, "Key : %c\n", e_prog.comp[i]->key);
  fprintf(opt_file, "X : %d\n", e_prog.comp[i]->x);
 }
 return 0;
}

/*   save options    */
int e_save_opt(FENSTER *f)
{
 ECNT *cn = f->ed;
 FILE *fp;
 int i;
 char *str_line;

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
 return 0;
}

int e_opt_read(ECNT *cn)
{
 FILE *fp;
 char *str_line;
 char *section;
 char *option;
 char *value;
 char *str_tmp;
 int sz;
 int i;

 fp = fopen(cn->optfile, "r");
 if (fp == NULL)
 {
  char *file = e_mkfilename((char *)e_lib_dir(), OPTION_FILE);
  fp = fopen(file, "r");
  FREE(file);
 }
 if (fp == NULL) return(0);
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
    if (i < OPTION_SECTIONS)
     (*WpeSectionRead[i].function)(cn, section, option, value);
    else
     return ERR_READ_OPF;
   }
  }
 }
 fclose(fp);
 return 0;
}

/*  window for entering a text line */
int e_add_arguments(char *str, char *head, FENSTER *f, int n, int sw,
  struct dirfile **df)
{
 int ret;
 char *tmp = MALLOC((strlen(head)+2) * sizeof(char));
 W_OPTSTR *o = e_init_opt_kst(f);

 if (!o || !tmp)
  return(-1);
 o->xa = 20;  o->ya = 4;  o->xe = 57;  o->ye = 11;
 o->bgsw = 0;
 o->name = head;
 o->crsw = AltO;
 sprintf(tmp, "%s:", head);
 e_add_wrstr(4, 2, 4, 3, 30, E_ARG_FIELD_MAX, n, sw, tmp, str, df, o);
 e_add_bttstr(7, 5, 1, AltO, " Ok ", NULL, o);
 e_add_bttstr(24, 5, -1, WPE_ESC, "Cancel", NULL, o);
 FREE(tmp);
 ret = e_opt_kst(o);
 if (ret != WPE_ESC)
  strcpy(str, o->wstr[0]->txt);
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
 o->wstr[o->wn-1]->df = df;
 strcpy(o->wstr[o->wn-1]->header, header);
 strcpy(o->wstr[o->wn-1]->txt, txt);
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
  c = e_getch();
 while (g_bracketed_paste);
 return c;
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

static void e_opt_center_dialog(W_OPTSTR *o, int dlg_w, int dlg_h)
{
   FENSTER *ef = o->f->ed->f[o->f->ed->mxedt];
   int bx = ef->a.x, by = ef->a.y, bex = ef->e.x, bey = ef->e.y;
   /* A tool/output pane (AI, Messages -- ins == 8) is a small docked strip; a
      dialog centred on it lands squashed in a corner (and, when it is the pane
      the user just used, unusable).  Centre such dialogs on the whole screen. */
   if (ef->ins == 8) { bx = 0; by = 1; bex = MAXSCOL - 1; bey = MAXSLNS - 2; }
   o->xa = bx + (bex - bx - dlg_w) / 2;
   o->ya = by + (bey - by - dlg_h) / 2;
   if (o->xa < 0) o->xa = 0;
   if (o->ya < 1) o->ya = 1;
   o->xe = o->xa + dlg_w;
   o->ye = o->ya + dlg_h;
   if (o->xe >= MAXSCOL) o->xe = MAXSCOL - 1;
   if (o->ye >= MAXSLNS - 1) o->ye = MAXSLNS - 2;
   if (o->xa >= o->xe - 4) o->xa = 0;
   if (o->ya >= o->ye - 2) o->ya = 1;
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
 strcpy(tmp, w->txt);
 if (!w->df)
  c = e_schreib_leiste(tmp, x, y, w->nw, w->wmx, o->fwt, o->fws);
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
 if (o->f->ed->edopt & ED_CUA_STYLE)
  return c == CtrlL;
 return c == AF2;
}

/**
 * e_opt_prepare_restart - Tear the box down for a from-scratch redraw.
 *
 * After a terminal resize, or when a button's action floated a picker over
 * the dialog, the box is drawn again from the top of e_opt_kst_run.  The
 * focus goes to the first widget (c == 0 lets the layout pick it) and in
 * FOCUS mode: a redraw is not a click, so the widget it lands on is
 * highlighted, never toggled -- a resize used to flip the first checkbox.
 */
static void e_opt_prepare_restart(W_OPTSTR *o, struct opt_run *r)
{
 e_close_view(o->pic, 0);
 o->pic = NULL;
 r->c = 0;
 r->sw = OPT_FOCUS;
}

/**
 * e_opt_wait_esc_or_resize - Hold a dialog the screen cannot show.
 *
 * When the frame had to be clipped, nothing in it is safe to operate, so
 * every key is swallowed until the user gives up (Esc) or the terminal grows
 * (a resize, after which the box is laid out again).
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
 * e_opt_close - Take the box off the screen and give the editor back.
 */
static void e_opt_close(W_OPTSTR *o)
{
 e_close_view(o->pic, 1);
 e_mouse_flush();
 e_repaint_desk_nopic(o->f->ed->f[o->f->ed->mxedt]);
}

static int e_opt_kst_run(W_OPTSTR *o)
{
   struct opt_run r;
   int i, j, keep_pos = 0;
   int dlg_w = o->xe - o->xa;
   int dlg_h = o->ye - o->ya;

   r.c = o->bgsw;
   r.cold = 0;
   r.sw = OPT_FOCUS;
   r.ret = 0;
   fk_cursor(0);
e_opt_kst_restart:
   /* Keep a user-dragged position across an in-place repaint (a picker button
      that returns -1, e.g. Alt-M / Alt-V): centre only on the first layout and
      on a screen resize, not on every restart -- otherwise the dialog jumps back
      to centre and forgets where the user moved it. */
   if (!keep_pos) e_opt_center_dialog(o, dlg_w, dlg_h);
   keep_pos = 0;
   o->pic = e_std_kst(o->xa, o->ya, o->xe, o->ye, o->name, 1, o->frt, o->ftt, o->frs);
   if(o->pic == NULL) {  e_error(e_msg[ERR_LOWMEM], 0, o->f->fb); return(-1);  }
   if(!r.c) r.c = e_get_opt_sw(CDO, 0, 0, o);
   e_opt_draw_widgets(o);
   r.cold = r.c;
   if (o->xe - o->xa < dlg_w || o->ye - o->ya < dlg_h)
   {
      if (e_opt_wait_esc_or_resize() == WPE_RESIZE)
      {
         e_opt_prepare_restart(o, &r);
         goto e_opt_kst_restart;
      }
      e_opt_close(o);
      return(WPE_ESC);
   }
   /* The loop ends on Esc in activate mode only: Esc in focus mode names the
      Cancel button (its code is WPE_ESC) and merely focuses it. */
   while (r.c != WPE_ESC || r.sw == OPT_FOCUS)
   {
      if (r.c == WPE_RESIZE)
      {
         e_opt_prepare_restart(o, &r);
         goto e_opt_kst_restart;
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
            e_opt_prepare_restart(o, &r);
            keep_pos = 1;   /* in-place repaint: keep the dragged position */
            goto e_opt_kst_restart;
         }
         continue;
      }
      e_opt_refocus(&r);
   }
   e_opt_close(o);
   return(r.ret);
}

/* e_opt_kst - public entry to the modal dialog/picker/menu engine.  Runs the
   real loop (e_opt_kst_run) with the async LSP fd-loop's painting suspended for
   its whole lifetime, so a server update (diagnostics, inlay, status line, a
   Doctor or worksheet redraw) arriving mid-dialog never draws under the box and
   corrupts it.  This box pushes no window onto the editor stack, so the suspend
   is by depth counter (e_lsp_modal_enter/leave), not the window check. */
int e_opt_kst(W_OPTSTR *o)
{
   int ret;
#ifdef DEBUGGER
   e_lsp_modal_enter();
#endif
   ret = e_opt_kst_run(o);
#ifdef DEBUGGER
   e_lsp_modal_leave();
#endif
   return(ret);
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
 e_add_sswstr(4, 4, 13, AltK, f->ed->edopt & ED_BLOCK_WORDSTAR ? 1 : 0, "WordStar blocK", o);
 /* The AI assistant on/off + backend/model/policy now live in Options -> AI. */
 e_add_pswstr(0, 26, 7, 1, AltL, 0, "OLd-Style       ", o);
 e_add_pswstr(0, 26, 8, 0, AltC, f->ed->edopt & ED_CUA_STYLE, "CUA-Style       ", o);
 e_add_pswstr(1, 26, 11, 3, AltY, 0, "OnlY Source-Text", o);
 e_add_pswstr(1, 26, 12, 2, AltW, 0, "AlWays          ", o);
 e_add_pswstr(1, 26, 13, 2, AltV,
   (f->ed->edopt & ED_ALWAYS_AUTO_INDENT ? 1 : f->ed->edopt & ED_SOURCE_AUTO_INDENT ? 0 : 2),
   "NeVer           ", o);
 e_add_wrstr(3, 14, 3, 15, 44, 128, 1, AltR, "PRint Command:",
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
  f->ed->edopt = ((f->ed->edopt & ~ED_EDITOR_OPTIONS) + o->pstr[0]->num) +
    (o->pstr[1]->num == 0 ? ED_SOURCE_AUTO_INDENT : 0) +
    (o->pstr[1]->num == 1 ? ED_ALWAYS_AUTO_INDENT : 0) +
    (o->sstr[3]->num ? ED_OLD_TILE_METHOD : 0) +
    (o->sstr[0]->num ? ED_SHOW_ENDMARKS : 0) +
    (o->sstr[4]->num ? ED_BLOCK_WORDSTAR : 0);
  if (f->ed->print_cmd)
   WpeFree(f->ed->print_cmd);
  f->ed->print_cmd = WpeStrdup(o->wstr[0]->txt);
  if (edopt != f->ed->edopt)
  {
   e_switch_blst(f->ed);
#ifdef WPE_AI
   e_ai_refresh_bars(f->ed);   /* show/hide the "Alt-B AI" bar entry live */
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

