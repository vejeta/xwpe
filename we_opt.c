/* we_opt.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include "WeExpArr.h"
#include <ctype.h>
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
 int xa = 10, ya = 4, xe = xa + 50, ye = ya + 13;
 char tmp[40];

 fk_cursor(0);
 pic = e_std_kst(xa, ya, xe, ye, NULL, 1, f->fb->nr.fb, f->fb->nt.fb, f->fb->ne.fb);
 if (pic == NULL)
 {
  e_error(e_msg[ERR_LOWMEM], 1, f->fb);
  return(WPE_ESC);
 }
   
 sprintf(tmp, "          Version %s          ", VERSION);
#ifdef UNIX
 if (WpeIsXwin() && WpeIsProg())
 {
  e_pr_str(xa+7, ya+3, "  XWindow Programming Environment  ", f->fb->et.fb, 0, 0, 0, 0);
  e_pr_str(xa+7, ya+4, "             ( XWPE )              ", f->fb->et.fb, 0, 0, 0, 0);
 }
 else if (WpeIsProg())
 {
  e_pr_str(xa+7, ya+3, "   Window Programming Environment  ", f->fb->et.fb, 0, 0, 0, 0);
  e_pr_str(xa+7, ya+4, "              ( WPE )              ", f->fb->et.fb, 0, 0, 0, 0);
 }
 else if (WpeIsXwin())
 {
  e_pr_str(xa+7, ya+3, "          XWindow Editor           ", f->fb->et.fb, 0, 0, 0, 0);
  e_pr_str(xa+7, ya+4, "              ( XWE )              ", f->fb->et.fb, 0, 0, 0, 0);
 }
 else
#endif
 {
  e_pr_str(xa+7, ya+3, "           Window Editor           ", f->fb->et.fb, 0, 0, 0, 0);
  e_pr_str(xa+7, ya+4, "               ( WE )              ", f->fb->et.fb, 0, 0, 0, 0);
 }
 e_pr_str(xa+7, ya+5, tmp, f->fb->et.fb, 0, 0, 0, 0);
 e_pr_str(xa+2, ya+8, "Copyright (C) 1993 Fred Kruse", f->fb->nt.fb, 0, 0, 0, 0);
 e_pr_str(xa+2, ya+9, "This Sofware comes with ABSOLUTELY NO WARRANTY;", f->fb->nt.fb, 0, 0, 0, 0);
 e_pr_str(xa+2, ya+10, "This is free software, and you are welcome to", f->fb->nt.fb, 0, 0, 0, 0);
 e_pr_str(xa+2, ya+11, "redistribute it under certain conditions;", f->fb->nt.fb, 0, 0, 0, 0);
 e_pr_str(xa+2, ya+12, "See \'Help\\Editor\\GNU Pub...\' for details.", f->fb->nt.fb, 0, 0, 0, 0);
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
 return 0;
}

int WpeWriteProgramming(ECNT *cn, char *section, FILE *opt_file)
{
 fprintf(opt_file, "Arguments : %s\n", e_prog.arguments);
 fprintf(opt_file, "Project : %s\n", e_prog.project);
 fprintf(opt_file, "Exedir : %s\n", e_prog.exedir);
 fprintf(opt_file, "IncludePath : %s\n", e_prog.sys_include);
 fprintf(opt_file, "Debugger : %d\n", e_deb_type);
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
  char *file = e_mkfilename(LIBRARY_DIR, OPTION_FILE);
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
 e_add_wrstr(4, 2, 4, 3, 30, 128, n, sw, tmp, str, df, o);
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

int e_opt_move(W_OPTSTR *o)
{
 int xa = o->xa, ya = o->ya, xe = o->xe, ye = o->ye;
 int c = 0;
 PIC *pic;

 e_std_rahmen(o->xa, o->ya, o->xe, o->ye, o->name, 0, o->frt, o->frs);
#ifndef NEWSTYLE
 if (!WpeIsXwin())
  pic = e_open_view(o->xa, o->ya, o->xe, o->ye, 0, 2);
 else 
 {
  pic = e_open_view(o->xa, o->ya, o->xe-2, o->ye-1, 0, 2);
  e_close_view(pic, 2);
 }
#else
 pic = e_open_view(o->xa, o->ya, o->xe, o->ye, 0, 2);
#endif
 while ((c = e_getch()) != WPE_ESC && c != WPE_CR)
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
   o->xa = xa;
   o->ya = ya;
   o->xe = xe;
   o->ye = ye;
   o->pic = e_change_pic(o->xa, o->ya, o->xe, o->ye, o->pic, 1, o->frt);
   if (o->pic == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, o->f->fb);
   pic->a.x = o->xa;  pic->a.y = o->ya;
   pic->e.x = o->xe;  pic->e.y = o->ye;
   e_close_view(pic, 2);
  }
 }
 pic->a.x = o->xa;  pic->a.y = o->ya;
 pic->e.x = o->xe;  pic->e.y = o->ye;
 e_close_view(pic, 1);
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
         c ? xmin : 2, ymin, c))
      {  xmin = o->sstr[i]->x;  ymin = o->sstr[i]->y;
         ret = o->sstr[i]->sw;
      }
   }
   for(i = 0; i < o->pn; i++)
      for(j = 0; j < o->pstr[i]->np; j++)
      {  if(e_get_sw_cmp(o->pstr[i]->ps[j]->x, o->pstr[i]->ps[j]->y, x, y,
            c ? xmin : 2, ymin, c))
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
   o->xa = ef->a.x + (ef->e.x - ef->a.x - dlg_w) / 2;
   o->ya = ef->a.y + (ef->e.y - ef->a.y - dlg_h) / 2;
   if (o->xa < 0) o->xa = 0;
   if (o->ya < 1) o->ya = 1;
   o->xe = o->xa + dlg_w;
   o->ye = o->ya + dlg_h;
   if (o->xe >= MAXSCOL) o->xe = MAXSCOL - 1;
   if (o->ye >= MAXSLNS - 1) o->ye = MAXSLNS - 2;
   if (o->xa >= o->xe - 4) o->xa = 0;
   if (o->ya >= o->ye - 2) o->ya = 1;
}

static int e_opt_element_visible(W_OPTSTR *o, int ex, int ey)
{
 int ax = o->xa + ex;
 int ay = o->ya + ey;
 return (ay > o->ya && ay < o->ye && ax > o->xa && ax < o->xe);
}

int e_opt_kst(W_OPTSTR *o)
{
   int ret = 0, csv, sw = 1, i, j, num, cold, c = o->bgsw;
   char *tmp;
   int dlg_w = o->xe - o->xa;
   int dlg_h = o->ye - o->ya;
   fk_cursor(0);
e_opt_kst_restart:
   e_opt_center_dialog(o, dlg_w, dlg_h);
   { FILE *_df = fopen("/tmp/xwpe-dlg.txt", "a");
     if (_df) { fprintf(_df, "RESTART: dlg=(%d,%d)-(%d,%d) dlg_w=%d dlg_h=%d MAXSLNS=%d MAXSCOL=%d\n",
       o->xa, o->ya, o->xe, o->ye, dlg_w, dlg_h, MAXSLNS, MAXSCOL); fclose(_df); }
   }
   o->pic = e_std_kst(o->xa, o->ya, o->xe, o->ye, o->name, 1, o->frt, o->ftt, o->frs);
   if(o->pic == NULL) {  e_error(e_msg[ERR_LOWMEM], 0, o->f->fb); return(-1);  }
   if(!c) c = e_get_opt_sw(CDO, 0, 0, o);
   for(i = 0; i < o->tn; i++)
      if (e_opt_element_visible(o, o->tstr[i]->x, o->tstr[i]->y))
      e_pr_str(o->xa+o->tstr[i]->x, o->ya+o->tstr[i]->y, o->tstr[i]->txt,
      o->ftt, -1, 0, 0, 0);
   for(i = 0; i < o->wn; i++)
   {  if (!e_opt_element_visible(o, o->wstr[i]->xw, o->wstr[i]->yw)) continue;
      e_pr_str(o->xa+o->wstr[i]->xt, o->ya+o->wstr[i]->yt, o->wstr[i]->header,
         o->ftt, o->wstr[i]->nc, 1, o->fts, 0);
      if(!o->wstr[i]->df)
         e_schr_nchar(o->wstr[i]->txt, o->xa+o->wstr[i]->xw,
         o->ya+o->wstr[i]->yw, 0, o->wstr[i]->nw, o->fwt);
      else
         e_schr_nchar_wsv(o->wstr[i]->txt, o->xa+o->wstr[i]->xw,
         o->ya+o->wstr[i]->yw, 0, o->wstr[i]->nw, o->fwt, o->fws);
   }
   for(i = 0; i < o->nn; i++)
   {  if (!e_opt_element_visible(o, o->nstr[i]->xw, o->nstr[i]->yw)) continue;
      e_pr_str(o->xa+o->nstr[i]->xt, o->ya+o->nstr[i]->yt, o->nstr[i]->header,
         o->ftt, o->nstr[i]->nc, 1, o->fts, 0);
      e_schr_nzif(o->nstr[i]->num, o->xa+o->nstr[i]->xw, o->ya+o->nstr[i]->yw,
         o->nstr[i]->nw, o->fwt);
   }
   for(i = 0; i < o->sn; i++)
   {  if (!e_opt_element_visible(o, o->sstr[i]->x, o->sstr[i]->y)) continue;
      e_pr_str(o->xa+o->sstr[i]->x+4, o->ya+o->sstr[i]->y, o->sstr[i]->header,
         o->fst, o->sstr[i]->nc, 1, o->fss, 0);
      e_pr_str(o->xa+o->sstr[i]->x, o->ya+o->sstr[i]->y, "[ ]", o->fst, -1, 1, 0, 0);
      if(o->sstr[i]->num)
         e_pr_char(o->xa+o->sstr[i]->x+1, o->ya+o->sstr[i]->y, 'X', o->fst);
   }
   for(i = 0; i < o->pn; i++)
   {  for(j = 0; j < o->pstr[i]->np; j++)
      {  if (!e_opt_element_visible(o, o->pstr[i]->ps[j]->x, o->pstr[i]->ps[j]->y)) continue;
         e_pr_str(o->xa+o->pstr[i]->ps[j]->x, o->ya+o->pstr[i]->ps[j]->y, "( ) ",
            o->fst, -1, 1, 0, 0);
         e_pr_str(o->xa+o->pstr[i]->ps[j]->x+4, o->ya+o->pstr[i]->ps[j]->y,
            o->pstr[i]->ps[j]->header, o->fst, o->pstr[i]->ps[j]->nc,
            1, o->fss, 0);
         e_pr_str(o->xa+o->pstr[i]->ps[j]->x, o->ya+o->pstr[i]->ps[j]->y, "( )", o->fst, -1, 1, 0, 0);
         if(o->pstr[i]->num == j)
            e_pr_char(o->xa+o->pstr[i]->ps[j]->x+1, o->ya+o->pstr[i]->ps[j]->y, '*', o->fst);
      }
   }
   for(i = 0; i < o->bn; i++)
   {  if (!e_opt_element_visible(o, o->bstr[i]->x, o->bstr[i]->y)) continue;
      e_pr_str(o->xa+o->bstr[i]->x, o->ya+o->bstr[i]->y, o->bstr[i]->header,
         o->fbt, o->bstr[i]->nc, -1, o->fbs, o->ftt);
   }
   cold = c;
   if (o->xe - o->xa < dlg_w || o->ye - o->ya < dlg_h)
   {
      while ((c = e_getch()) != WPE_ESC && c != WPE_RESIZE)
         ;
      if (c == WPE_RESIZE)
      {
         e_close_view(o->pic, 0);
         o->pic = NULL;
         c = 0;
         goto e_opt_kst_restart;
      }
      e_close_view(o->pic, 1);
      e_mouse_flush();
      e_repaint_desk_nopic(o->f->ed->f[o->f->ed->mxedt]);
      return(WPE_ESC);
   }
   while (c != WPE_ESC || sw)
   {
      if (c == WPE_RESIZE)
      {
         { FILE *_df = fopen("/tmp/xwpe-dlg.txt", "a");
           if (_df) { fprintf(_df, "OPT_KST: WPE_RESIZE pic=(%d,%d)-(%d,%d) MAXSLNS=%d MAXSCOL=%d LINES=%d COLS=%d\n",
             o->xa, o->ya, o->xe, o->ye, MAXSLNS, MAXSCOL, LINES, COLS); fclose(_df); }
         }
         e_close_view(o->pic, 0);
         o->pic = NULL;
         c = 0;
         goto e_opt_kst_restart;
      }
#ifdef NEWSTYLE
      if (WpeIsXwin())
      {  for(i = 0; i < o->sn; i++)
         {  if(o->sstr[i]->num)
               e_make_xrect_abs(o->xa+o->sstr[i]->x, o->ya+o->sstr[i]->y,
               o->xa+o->sstr[i]->x + 2, o->ya+o->sstr[i]->y, 1);
            else
               e_make_xrect_abs(o->xa+o->sstr[i]->x, o->ya+o->sstr[i]->y,
               o->xa+o->sstr[i]->x + 2, o->ya+o->sstr[i]->y, 0);
         }
         for(i = 0; i < o->pn; i++)
            for(j = 0; j < o->pstr[i]->np; j++)
            {  if(o->pstr[i]->num == j)
                  e_make_xrect_abs(o->xa+o->pstr[i]->ps[j]->x, o->ya+o->pstr[i]->ps[j]->y,
                  o->xa+o->pstr[i]->ps[j]->x + 2, o->ya+o->pstr[i]->ps[j]->y, 1);
               else
                  e_make_xrect_abs(o->xa+o->pstr[i]->ps[j]->x, o->ya+o->pstr[i]->ps[j]->y,
                  o->xa+o->pstr[i]->ps[j]->x+2, o->ya+o->pstr[i]->ps[j]->y, 0);
            }
      }
      else
#endif
      {  for(i = 0; i < o->sn; i++)
         {  if(o->sstr[i]->num)
               e_pr_char(o->xa+o->sstr[i]->x+1, o->ya+o->sstr[i]->y, 'X', o->fst);
            else
               e_pr_char(o->xa+o->sstr[i]->x+1, o->ya+o->sstr[i]->y, ' ', o->fst);
         }
         for(i = 0; i < o->pn; i++)
            for(j = 0; j < o->pstr[i]->np; j++)
            {  if(o->pstr[i]->num == j)
                  e_pr_char(o->xa+o->pstr[i]->ps[j]->x+1, o->ya+o->pstr[i]->ps[j]->y, '*', o->fst);
               else
                  e_pr_char(o->xa+o->pstr[i]->ps[j]->x+1, o->ya+o->pstr[i]->ps[j]->y, ' ', o->fst);
            }
      }
      if((c == AF2 && !(o->f->ed->edopt & ED_CUA_STYLE))
         || (o->f->ed->edopt & ED_CUA_STYLE && c == CtrlL))
      {  e_opt_move(o);  c = cold;  continue;  }
      for(i = 0; i < o->wn; i++)
         if(o->wstr[i]->sw == c || (o->wstr[i]->nc >= 0 &&
         toupper(c) == o->wstr[i]->header[o->wstr[i]->nc]))
      {  cold = c;
         tmp = MALLOC((o->wstr[i]->wmx + 1) * sizeof(char));
         strcpy(tmp, o->wstr[i]->txt);
#if MOUSE
         if(!o->wstr[i]->df && (c = e_schreib_leiste(tmp,
            o->xa+o->wstr[i]->xw, o->ya+o->wstr[i]->yw,
            o->wstr[i]->nw, o->wstr[i]->wmx, o->fwt, o->fws)) < 0)
            c = e_opt_mouse(o);
         else if(o->wstr[i]->df && (c = e_schr_lst_wsv(tmp,
            o->xa+o->wstr[i]->xw, o->ya+o->wstr[i]->yw,
            o->wstr[i]->nw, o->wstr[i]->wmx, o->fwt, o->fws,
            o->wstr[i]->df, o->f)) < 0)
            c = e_opt_mouse(o);
#else
         if(!o->wstr[i]->df)
            c = e_schreib_leiste(tmp,
            o->xa+o->wstr[i]->xw, o->ya+o->wstr[i]->yw,
            o->wstr[i]->nw, o->wstr[i]->wmx, o->fwt, o->fws);
         else if(o->wstr[i]->df)
            c = e_schr_lst_wsv(tmp,
            o->xa+o->wstr[i]->xw, o->ya+o->wstr[i]->yw,
            o->wstr[i]->nw, o->wstr[i]->wmx, o->fwt, o->fws,
            o->wstr[i]->df, o->f);
#endif
         if(c != WPE_ESC) strcpy(o->wstr[i]->txt, tmp);
         csv = c;
         if(!o->wstr[i]->df)
            e_schr_nchar(o->wstr[i]->txt, o->xa+o->wstr[i]->xw,
            o->ya+o->wstr[i]->yw, 0, o->wstr[i]->nw, o->fwt);
         else
            e_schr_nchar_wsv(o->wstr[i]->txt, o->xa+o->wstr[i]->xw,
            o->ya+o->wstr[i]->yw, 0, o->wstr[i]->nw, o->fwt, o->fws);
         if((c = e_get_opt_sw(c, o->xa+o->wstr[i]->xw,
            o->ya+o->wstr[i]->yw, o)) != csv)  sw = 1;
         else sw = 0;
         if(c == WPE_CR)  c = o->crsw;
         else if(c == WPE_ESC) ret = WPE_ESC;
         free(tmp);
         fk_cursor(0);
         break;
      }
      if(i < o->wn) continue;
      for(i = 0; i < o->nn; i++)
         if(o->nstr[i]->sw == c || (o->nstr[i]->nc >= 0 &&
         toupper(c) == o->nstr[i]->header[o->nstr[i]->nc]))
      {  cold = c;
         num = o->nstr[i]->num;
#if MOUSE
         if((c = e_schreib_zif(&num, o->xa+o->nstr[i]->xw, o->ya+o->nstr[i]->yw,
            o->nstr[i]->nw, o->fwt, o->fws)) < 0)
            c = e_opt_mouse(o);
#else
         c = e_schreib_zif(&num, o->xa+o->nstr[i]->xw, o->ya+o->nstr[i]->yw,
            o->nstr[i]->nw, o->fwt, o->fws);
#endif
         if(c != WPE_ESC) o->nstr[i]->num = num;
         csv = c;
         if((c = e_get_opt_sw(c, o->xa+o->nstr[i]->xw,
            o->ya+o->nstr[i]->yw, o)) != csv)  sw = 1;
         else sw = 0;
         if(c != cold) e_schr_nzif(o->nstr[i]->num, o->xa+o->nstr[i]->xw,
            o->ya+o->nstr[i]->yw, o->nstr[i]->nw, o->fwt);
         if(c == WPE_CR)  c = o->crsw;
         else if(c == WPE_ESC) ret = WPE_ESC;
         fk_cursor(0);
         break;
      }
      if(i < o->nn) continue;
      for(i = 0; i < o->sn; i++)
         if(o->sstr[i]->sw == c || (o->sstr[i]->nc >= 0 &&
         toupper(c) == o->sstr[i]->header[o->sstr[i]->nc]))
      {  if(!sw)
         {  o->sstr[i]->num = !o->sstr[i]->num;
            sw = 1;
            c = cold;
            break;
         }
         cold = c;
         e_pr_str(o->xa+o->sstr[i]->x+4, o->ya+o->sstr[i]->y, o->sstr[i]->header,
            o->fsa, o->sstr[i]->nc, 1, o->fsa, 0);
#if MOUSE
         if((c = e_getch()) < 0) c = e_opt_mouse(o);
#else
         c = e_getch();
#endif
         if(c == WPE_CR) {  sw = 0;  c = cold;  break;  }
         else if(c == WPE_ESC)  sw = 1;
         else
         {  csv = c;
            if((c = e_get_opt_sw(c, o->xa+o->sstr[i]->x,
               o->ya+o->sstr[i]->y, o)) != csv)  sw = 1;
            else sw = 0;
         }
         if(c != cold)
            e_pr_str(o->xa+o->sstr[i]->x+4, o->ya+o->sstr[i]->y, o->sstr[i]->header,
            o->fst, o->sstr[i]->nc, 1, o->fss, 0);
         break;
      }
      if(i < o->sn) continue;
      for(i = 0; i < o->pn; i++)
      {  for(j = 0; j < o->pstr[i]->np; j++)
            if(o->pstr[i]->ps[j]->sw == c || (o->pstr[i]->ps[j]->nc >= 0 &&
            toupper(c) == o->pstr[i]->ps[j]->header[o->pstr[i]->ps[j]->nc]))
         {  if(!sw)
            {  o->pstr[i]->num = j;
               sw = 1;
               c = cold;
               break;
            }
            cold = c;
            e_pr_str(o->xa+o->pstr[i]->ps[j]->x+4, o->ya+o->pstr[i]->ps[j]->y, o->pstr[i]->ps[j]->header,
               o->fsa, o->pstr[i]->ps[j]->nc, 1, o->fsa, 0);
#if MOUSE
            if((c = e_getch()) < 0) c = e_opt_mouse(o);
#else
            c = e_getch();
#endif
            if(c == WPE_CR)  {  sw = 0;  c = cold;  break;  }
            else if(c == WPE_ESC)  sw = 1;
            {  csv = c;
               if((c = e_get_opt_sw(c, o->xa+o->pstr[i]->ps[j]->x,
                  o->ya+o->pstr[i]->ps[j]->y, o)) != csv)  sw = 1;
               else sw = 0;
            }
            if(c != cold)
               e_pr_str(o->xa+o->pstr[i]->ps[j]->x+4, o->ya+o->pstr[i]->ps[j]->y, o->pstr[i]->ps[j]->header,
               o->fst, o->pstr[i]->ps[j]->nc, 1, o->fss, 0);
            break;
         }
         if(j < o->pstr[i]->np) break;
      }
      if(i < o->pn) continue;
      for(i = 0; i < o->bn; i++)
         if(o->bstr[i]->sw == c || (o->bstr[i]->nc >= 0 &&
         toupper(c) == o->bstr[i]->header[o->bstr[i]->nc]))
      {  e_pr_str(o->xa+o->bstr[i]->x, o->ya+o->bstr[i]->y, o->bstr[i]->header,
            o->fbz, o->bstr[i]->nc, -1, o->fbz, o->ftt);
         if(!sw)
         {  if(o->bstr[i]->fkt != NULL)
            {  if((ret = o->bstr[i]->fkt(o->f)) > 0) c = WPE_ESC;
               else
               {  c = cold;
                  e_pr_str(o->xa+o->bstr[i]->x, o->ya+o->bstr[i]->y, o->bstr[i]->header,
                     o->fbt, o->bstr[i]->nc, -1, o->fbs,  o->ftt);
               }
            }
            else {  ret = o->bstr[i]->sw;  c = WPE_ESC;  }
            break;
         }
         cold = c;
#if MOUSE
         if((c = e_getch()) < 0) c = e_opt_mouse(o);
#else
         c = e_getch();
#endif
         if(c == WPE_CR) {  ret = c = o->bstr[i]->sw;  sw = 0;  break;  }
         else if(c == WPE_ESC) {  sw = 0;  ret = WPE_ESC;  break;  }
         csv = c;
         if((c = e_get_opt_sw(c, o->xa+o->bstr[i]->x,
            o->ya+o->bstr[i]->y, o)) != csv)  sw = 1;
         else sw = 0;
         if(c != cold)
            e_pr_str(o->xa+o->bstr[i]->x, o->ya+o->bstr[i]->y, o->bstr[i]->header,
            o->fbt, o->bstr[i]->nc, -1, o->fbs,  o->ftt);
         break;
      }
      if(i < o->bn) continue;

      c = cold;
      sw = 1;
   }
   { FILE *_df = fopen("/tmp/xwpe-dlg.txt", "a");
     if (_df) { fprintf(_df, "CLOSE: pic=(%d,%d)-(%d,%d) ret=%d\n",
       o->pic->a.x, o->pic->a.y, o->pic->e.x, o->pic->e.y, ret); fclose(_df); }
   }
   e_close_view(o->pic, 1);
   e_mouse_flush();
   e_repaint_desk_nopic(o->f->ed->f[o->f->ed->mxedt]);
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
 e_add_sswstr(4, 3, 0, AltS, f->ed->edopt & ED_SHOW_ENDMARKS ? 1 : 0, "Show Endmark ", o);
 e_add_sswstr(26, 3, 1, AltP, f->ed->autosv & 1, "OPtions         ", o);
 e_add_sswstr(26, 4, 1, AltH, f->ed->autosv & 2 ? 1 : 0, "CHanges         ", o);
 e_add_sswstr(4, 6, 2, AltD, f->ed->edopt & ED_OLD_TILE_METHOD ? 1 : 0, "OlD Style    ", o);
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
    (o->sstr[0]->num ? ED_SHOW_ENDMARKS : 0);
  if (f->ed->print_cmd)
   WpeFree(f->ed->print_cmd);
  f->ed->print_cmd = WpeStrdup(o->wstr[0]->txt);
  if (edopt != f->ed->edopt)
  {
   e_switch_blst(f->ed);
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

 sprintf(str, "%s/help.key", LIBRARY_DIR);
 for (i = 0; i < E_HLP_NUM; i++)
 {
  e_hlp_str[i] = MALLOC(sizeof(char));
  *e_hlp_str[i] = '\0';
 }
 if (!(fp = fopen(str, "rb")))
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

