/* we_term.c                                              */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "edit.h"
#include "we_lsp.h"           /* e_lsp_sem_slot_rgb: semantic-token truecolor */
#ifdef UNIX

#include <termios.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <langinfo.h>
#include <string.h>
#include <unistd.h>
#include "we_fdloop.h"
#include "we_clip.h"

#include<signal.h>
#define KEYFN 42
#define NSPCHR 13

#ifndef XWPE_DLL
#define WpeDllInit WpeTermInit
#endif

/* e_clip_t_os_set - terminal OS-clipboard writer.  Frames the text as OSC 52
   and writes it to the controlling terminal (STDOUT_FILENO); the emulator
   performs the real OS-clipboard write, so a plain ^C / ^Ins copies to the
   system clipboard locally AND over SSH.  Installed as e_clip_os_set at
   terminal start-up (below). */
static void e_clip_t_os_set(const char *utf8, int len)
{
 e_clip_osc52_write(STDOUT_FILENO, utf8, len);
}

/*    we_term.c    */
char *init_key(char *key);
char *init_kkey(char *key);
int init_cursor(void);
int e_begscr(void);
void e_endwin(void);
int fk_t_cursor(int x);
int fk_t_putchar(int c);
int fk_attrset(int a);
int e_t_refresh(void);
int e_t_sys_ini(void);
int e_t_sys_end(void);
int e_t_getch(void);
int e_find_key(int c, int j, int sw);
void e_exitm(char *s, int n);
int fk_t_locate(int x, int y);
int fk_t_mouse(int *g);
static int e_t_mouse_decode_button(mmask_t bstate);
static int e_t_mouse_is_released(mmask_t bstate);
static int e_t_mouse_apply_event(MEVENT *mev);
static void e_t_truecolor_detect(void);

/* Input/mouse timing and scaling constants. */
#define INPUT_POLL_MS     50   /* ncurses getch() wait between input polls */
#define MOUSE_CELL_SUBDIV 8    /* g[] reports mouse pos in 1/8-cell units */

int g_mouse_buttons = 0;
/* CLICKED events carry no following RELEASED; the press bit accumulated into
   g_mouse_buttons would stay set and any while(e_mshit()) spin-loop in
   we_mouse.c / we_menue.c would hang.  Set when a CLICKED is observed and
   consumed in fk_t_mouse's no-event branch to synthesize the release. */
static int s_t_pending_click_release = 0;
int e_t_initscr(void);
int e_t_kbhit(void);
int e_t_d_switch_out(int sw);
int e_t_switch_screen(int sw);
int e_t_deb_out(FENSTER *f);
int e_s_sys_end();
int e_s_sys_ini();

extern int MAXSLNS;
extern int MAXSCOL;


#ifndef NCURSES
char *key_f[KEYFN], *key_key;
#endif
char *cur_rc, *cur_vs, *cur_nvs, *cur_vvs, cur_attr;
char *att_so, *att_ul, *att_rv, *att_bl, *att_dm, *att_bo;
char *ratt_no, *ratt_so, *ratt_ul, *ratt_rv, *ratt_bl, *ratt_dm, *ratt_bo;
char *beg_scr, *swt_scr, *sav_cur, *res_cur; 
extern SCREENCELL *altschirm;
extern char *att_no;
char *col_fg, *col_bg, *spc_st, *spc_in, *spc_bg, *spc_nd;

extern int col_num;
#ifdef NCURSES
chtype sp_chr[NSPCHR];
#else
char *sp_chr[NSPCHR];
#endif

/* Whether the console can display the Unicode "symbol" chrome glyphs (the
   close/zoom title-bar boxes).  Box-drawing and the scrollbar already go
   through ncurses ACS, which ncurses downgrades to the VT100/ASCII line set on
   its own; but the title-bar buttons are written into schirm as raw codepoints
   (U+2715 close, U+25A1/U+25FB zoom, U+25A3 restore) by the shared chrome code,
   and on a non-UTF-8 terminal wcwidth() reports them unprintable so the emit
   loop would blank them -- the buttons would silently vanish.  Detected once
   from the locale; see e_t_detect_unicode().  Defaults to 1 so a build without
   the detection still behaves as before on the common UTF-8 console. */
static int e_t_unicode_term = 1;

/* e_t_detect_unicode - Decide once whether the terminal's encoding is UTF-8,
   from the C locale's CODESET (setlocale(LC_ALL,"") already ran in we_unix.c).
   Anything that is not UTF-8 (the C locale's ANSI_X3.4-1968, ISO-8859-*, a raw
   serial console) takes the ASCII chrome fallback. */
static void e_t_detect_unicode(void)
{
 const char *cs = nl_langinfo(CODESET);
 e_t_unicode_term = (cs != NULL &&
   (strstr(cs, "UTF-8") || strstr(cs, "UTF8") || strstr(cs, "utf8")));
}

/* e_t_chrome_ascii - ASCII stand-in for a Unicode chrome symbol glyph, for a
   terminal that cannot show it.  Returns 0 when cp is not a known chrome glyph,
   so ordinary text keeps its normal (wcwidth-guarded) path.  The stand-ins read
   as their action: 'x' close, '^' zoom/maximize, 'v' restore. */
static int e_t_chrome_ascii(int cp)
{
 switch (cp)
 {
  case 0x2715:           return 'x';   /* close box (multiplication X) */
  case 0x25A1:                         /* white square (console zoom) */
  case 0x25FB:           return '^';   /* white medium square (X11 zoom) */
  case 0x25A3:           return 'v';   /* square with inner square (restore) */
  case 0x1F512:          return '#';   /* lock (read-only file title bar) */
  case 0x2699:           return '%';   /* gear (tool/output pane title bar) */
  default:               return 0;
 }
}

extern int cur_x, cur_y;
extern struct termios otermio, ntermio, ttermio;
#ifdef TERMCAP
char area[315];
char *ap = area;
char tcbuf[1024];
char *tc_screen;
char *tgetstr();
int tgetnum();
char *tgoto();
#define tigetstr(k) tgetstr(k, &ap)

#define term_move(x,y) e_putp(tgoto(cur_rc, x, y))
#define term_refresh() fflush(stdout)
#else
/* Because term.h defines "buttons" it messes up we_gpm.c if in edit.h */
#include <term.h>

/* AIX requires that tparm has 10 arguments. */
#define tparm1(a,b) tparm((a), (b), 0, 0, 0, 0, 0, 0, 0, 0)
#define tparm2(a,b,c) tparm((a), (b), (c), 0, 0, 0, 0, 0, 0, 0)

#ifdef NCURSES
#define term_move(x,y) move(y, x)
#define term_refresh() refresh()
#else
#define term_move(x,y) e_putp(tparm2(cur_rc, y, x))
#define term_refresh() fflush(stdout)
#endif
#endif

int WpeGpmMouseInit(void);

int WpeDllInit(int *argc, char **argv)
{
 fk_u_cursor = fk_t_cursor;
 fk_u_locate = fk_t_locate;
 e_u_d_switch_out = e_t_d_switch_out;
 e_u_switch_screen = e_t_switch_screen;
 e_u_deb_out = e_t_deb_out;
 e_u_kbhit = e_t_kbhit;
 e_u_s_sys_end = e_s_sys_end;
 e_u_s_sys_ini = e_s_sys_ini;
 e_u_refresh = e_t_refresh;
 e_u_getch = e_t_getch;
 e_u_sys_ini = e_t_sys_ini;
 e_u_sys_end = e_t_sys_end;
 e_u_system = system;
 fk_u_putchar = fk_t_putchar;
 e_clip_os_set = e_clip_t_os_set;   /* ^C / ^Ins -> OS clipboard via OSC 52 */
#ifdef HAVE_LIBGPM
 if (WpeGpmMouseInit() == 0)
 {
  fk_mouse = WpeGpmMouse;
 }
 else
#endif
  fk_mouse = fk_t_mouse;
 WpeMouseChangeShape = (void (*)(WpeMouseShape))WpeNullFunction;
 WpeMouseRestoreShape = WpeNullFunction;
 WpeDisplayEnd = e_endwin;
#ifdef __linux__
 u_bioskey = WpeLinuxBioskey;
#else
 u_bioskey = WpeZeroFunction;
#endif
 e_t_initscr();
 if (col_num > 0)
 {
  e_pr_u_col_kasten = e_pr_x_col_kasten;
  e_frb_u_menue = e_frb_x_menue;
  e_s_u_clr = e_s_x_clr;
  e_n_u_clr = e_n_x_clr;
 }
 else
 {
  e_pr_u_col_kasten = e_pr_t_col_kasten;
  e_frb_u_menue = e_frb_t_menue;
  e_s_u_clr = e_s_t_clr;
  e_n_u_clr = e_n_t_clr;
 }
 MCI = 7;
 MCA = 11;
 RD1 = '\01';
 RD2 = '\02';
 RD3 = '\03';
 RD4 = '\04';
 RD5 = '\05';
 RD6 = '\06';
 RE1 = '\01';
 RE2 = '\02';
 RE3 = '\03';
 RE4 = '\04';
 RE5 = '\05';
 RE6 = '\06';
 WBT = '\13';
 ctree[0] = "\11\12\07";   /*  07 -> 30  */
 ctree[1] = "\11\12\12";	/*  11 -> 16  */
 ctree[2] = "\11\07\12";   /*  12 -> 22  */
 ctree[3] = "\10\12\12";   /*  10 -> 25  */
 ctree[4] = "\11\12\12";
/*
 RD1 = '+';
 RD2 = '+';
 RD3 = '+';
 RD4 = '+';
 RD5 = '-';
 RD6 = '|';
*//*
 RE1 = '.';
 RE2 = '.';
 RE3 = '.';
 RE4 = '.';
 RE5 = '.';
 RE6 = ':';
 WBT = '#';
 ctree[0] = "|__";
 ctree[1] = "|__";
 ctree[2] = "|__";
 ctree[3] = "|__";
 ctree[4] = "|__";
*/
 return 0;
}

char *init_key(char *key)
{
 char *tmp, *keystr;

 tmp = (char *) tigetstr(key);
 if (tmp == NULL || tmp == ((char *) -1))
  return(NULL);
 else
 {
  keystr = MALLOC(strlen(tmp)+1);
  strcpy(keystr, tmp);
 }
 return(keystr);
}

#ifndef NCURSES
char *init_kkey(char *key)
{
 char *tmp;
 int i;

 tmp = init_key(key);
 if (tmp == NULL)
  return(NULL);
 if (!key_key)
 {
  key_key = MALLOC(2);
  key_key[0] = tmp[1];
  key_key[1] = '\0';
  return(tmp);
 }
 else
 {
  for (i = 0; key_key[i] != '\0'; i++)
   if (key_key[i] == tmp[1])
    return(tmp);
  key_key = REALLOC(key_key, i + 2);
  key_key[i] = tmp[1];
  key_key[i + 1] = '\0';
 }
 return(tmp);
}
#endif

char *init_spchr(char c)
{
 int i;
 char *pt = NULL;

 if (!spc_st || !spc_bg || !spc_nd)
  return(NULL);
 for (i = 0; spc_st[i] && spc_st[i+1] && spc_st[i] != c; i+=2)
  ;
 if (spc_st[i] && spc_st[i+1])
 {
  pt = MALLOC((strlen(spc_bg)+strlen(spc_nd)+2)*sizeof(char));
  if(pt)
   sprintf(pt, "%s%c%s", spc_bg, spc_st[i+1], spc_nd);
 }
 return(pt);
}

int init_cursor()
{
#ifndef TERMCAP
   if (!(cur_rc = init_key("cup")))
    return(-1);
   if ((col_fg = init_key("setaf")) && (col_bg = init_key("setab")))
   { /* terminfo 10.2.x color code */
    if ((col_num = tigetnum("colors")) < 0) col_num = 8;
   }
   else if ((col_fg = init_key("setf")) && (col_bg = init_key("setb")))
   { /* Older terminfo color code */
    if((col_num = tigetnum("colors")) < 0) col_num = 8;
   }
   else
   { /* No colors */
    if(col_fg) {  free(col_fg);  col_fg = NULL;   }
    if(col_bg) {  free(col_bg);  col_bg = NULL;   }
   }

   spc_st = init_key("acsc");
   spc_in = init_key("enacs");
   spc_bg = init_key("smacs");
   spc_nd = init_key("rmacs");
   att_no = init_key("sgr0");
   att_so = init_key("smso");
   att_ul = init_key("smul");
   att_rv = init_key("rev");
   att_bl = init_key("blink");
   att_dm = init_key("dim");
   att_bo = init_key("bold");
   ratt_no = init_key("sgr0");
   ratt_so = init_key("rmso");
   ratt_ul = init_key("rmul");
   ratt_rv = init_key("sgr0");
   ratt_bl = init_key("sgr0");
   ratt_dm = init_key("sgr0");
   ratt_bo = init_key("sgr0");
   beg_scr = init_key("smcup");
   swt_scr = init_key("rmcup");
   sav_cur = init_key("sc");
   res_cur = init_key("rc");

#ifndef NCURSES
   key_f[0] = init_kkey("kf1");
   key_f[1] = init_kkey("kf2");
   key_f[2] = init_kkey("kf3");
   key_f[3] = init_kkey("kf4");
   key_f[4] = init_kkey("kf5");
   key_f[5] = init_kkey("kf6");
   key_f[6] = init_kkey("kf7");
   key_f[7] = init_kkey("kf8");
   key_f[8] = init_kkey("kf9");
   key_f[9] = init_kkey("kf10");
   key_f[10] = init_kkey("kcuu1");
   key_f[11] = init_kkey("kcud1");
   key_f[12] = init_kkey("kcub1");
   key_f[13] = init_kkey("kcuf1");
   key_f[14] = init_kkey("kich1");
   key_f[15] = init_kkey("khome");
   key_f[16] = init_kkey("kpp");
   if(!(key_f[17] = init_kkey("kdch1"))) key_f[17] = "\177";
   key_f[18] = init_kkey("kend");
   key_f[19] = init_kkey("knp");
   key_f[20] = init_kkey("kbs");
   key_f[21] = init_kkey("khlp");
   key_f[22] = init_kkey("kll");
   key_f[23] = init_kkey("kf17");
   key_f[24] = init_kkey("kf18");
   key_f[25] = init_kkey("kf19");
   key_f[26] = init_kkey("kf20");
   key_f[27] = init_kkey("kf21");
   key_f[28] = init_kkey("kf22");
   key_f[29] = init_kkey("kf23");
   key_f[30] = init_kkey("kf24");
   key_f[31] = init_kkey("kf25");
   key_f[32] = init_kkey("kf26");
   key_f[33] = init_kkey("kPRV");
   key_f[34] = init_kkey("kNXT");
   key_f[35] = init_kkey("kLFT");
   key_f[36] = init_kkey("kRIT");
   key_f[37] = init_kkey("kHOM");
   key_f[38] = init_kkey("kri");
   key_f[39] = init_kkey("kEND");
   key_f[40] = init_kkey("kind");
   key_f[41] = init_kkey("kext");
#endif

#else

   if(!(cur_rc = init_key("cm"))) return(-1);
   if((col_fg = init_key("Sf")) && (col_bg = init_key("Sb")))
   {  if((col_num = tgetnum("Co")) < 0) col_num = 8;  }
   else
   {  if(col_fg) {  free(col_fg);  col_fg = NULL;   }
      if(col_bg) {  free(col_bg);  col_bg = NULL;   }
   }
   spc_st = init_key("ac");
   spc_in = init_key("eA");
   spc_bg = init_key("as");
   spc_nd = init_key("ae");
   att_no = init_key("me");
   att_so = init_key("so");
   att_ul = init_key("us");
   att_rv = init_key("mr");
   att_bl = init_key("mb");
   att_dm = init_key("mh");
   att_bo = init_key("md");
   ratt_no = init_key("me");
   ratt_so = init_key("se");
   ratt_ul = init_key("ue");
   ratt_rv = init_key("me");
   ratt_bl = init_key("me");
   ratt_dm = init_key("me");
   ratt_bo = init_key("me");
   beg_scr = init_key("ti");
   swt_scr = init_key("te");
   sav_cur = init_key("sc");
   res_cur = init_key("rc");

   key_f[0] = init_kkey("k1");
   key_f[1] = init_kkey("k2");
   key_f[2] = init_kkey("k3");
   key_f[3] = init_kkey("k4");
   key_f[4] = init_kkey("k5");
   key_f[5] = init_kkey("k6");
   key_f[6] = init_kkey("k7");
   key_f[7] = init_kkey("k8");
   key_f[8] = init_kkey("k9");
   key_f[9] = init_kkey("k;");
   key_f[10] = init_kkey("ku");
   key_f[11] = init_kkey("kd");
   key_f[12] = init_kkey("kl");
   key_f[13] = init_kkey("kr");
   key_f[14] = init_kkey("kI");
   key_f[15] = init_kkey("kh");
   key_f[16] = init_kkey("kP");
   key_f[17] = init_kkey("kD");
   key_f[18] = init_kkey("@7");
   key_f[19] = init_kkey("kN");
   key_f[20] = init_kkey("kb");
   key_f[21] = init_kkey("%1");
   key_f[22] = init_kkey("kH");
   key_f[23] = init_kkey("F7");
   key_f[24] = init_kkey("F8");
   key_f[25] = init_kkey("F9");
   key_f[26] = init_kkey("FA");
   key_f[27] = init_kkey("FB");
   key_f[28] = init_kkey("FC");
   key_f[29] = init_kkey("FD");
   key_f[30] = init_kkey("FE");
   key_f[31] = init_kkey("FF");
   key_f[32] = init_kkey("FG");
   key_f[33] = init_kkey("%e");
   key_f[34] = init_kkey("%c");
   key_f[35] = init_kkey("#4");
   key_f[36] = init_kkey("%i");
   key_f[37] = init_kkey("#2");
   key_f[38] = init_kkey("kR");
   key_f[39] = init_kkey("*7");
   key_f[40] = init_kkey("kF");
   key_f[41] = init_kkey("@1");
#endif
#ifdef NCURSES
   sp_chr[0] = ' ';
   sp_chr[1] = ACS_ULCORNER;
   sp_chr[2] = ACS_URCORNER;
   sp_chr[3] = ACS_LLCORNER;
   sp_chr[4] = ACS_LRCORNER;
   sp_chr[5] = ACS_HLINE;
   sp_chr[6] = ACS_VLINE;
   /* Scrollbar track/thumb: use the shaded board (ACS_CKBOARD, the CP437
      stipple) for the track and the solid block (ACS_BLOCK) for the thumb,
      mirroring the X11/Xft look (U+2591 light shade + U+2588 full block).
      The old ACS_S9 (a thin scan line, rendered as a column of 's' on
      terminals that lack the alt charset) and ACS_DIAMOND thumb looked
      broken on the Linux console.  CKBOARD/BLOCK exist in every VGA/console
      font, so the scrollbar reads as a real shaded gutter with a solid
      slider with no special font. */
   sp_chr[7] = ACS_CKBOARD;
   sp_chr[8] = ACS_VLINE;
   sp_chr[9] = ACS_VLINE;
   sp_chr[10] = ACS_CKBOARD;
   sp_chr[11] = ACS_BLOCK;
   sp_chr[12] = ' ';
#else
   sp_chr[0] = "";
   if(!(sp_chr[1] = init_spchr('l'))) sp_chr[1] = "+";
   if(!(sp_chr[2] = init_spchr('k'))) sp_chr[2] = "+";
   if(!(sp_chr[3] = init_spchr('m'))) sp_chr[3] = "+";
   if(!(sp_chr[4] = init_spchr('j'))) sp_chr[4] = "+";
   if(!(sp_chr[5] = init_spchr('q'))) sp_chr[5] = "-";
   if(!(sp_chr[6] = init_spchr('x'))) sp_chr[6] = "|";
   if(!(sp_chr[7] = init_spchr('w'))) sp_chr[7] = "_";
   if(!(sp_chr[8] = init_spchr('t'))) sp_chr[8] = "|";
   if(!(sp_chr[9] = init_spchr('m'))) sp_chr[9] = "|";
   if(!(sp_chr[10] = init_spchr('q'))) sp_chr[10] = "_";
   if(!(sp_chr[11] = init_spchr('`'))) sp_chr[11] = "#";
   if(!(sp_chr[12] = init_spchr('a'))) sp_chr[12] = " ";
#endif
   return(0);
}

int e_t_initscr()
{
 int ret, i, k;
#ifndef TERMCAP
 WINDOW * stdscr;
#endif

 e_t_detect_unicode();
 ret = tcgetattr(1, &otermio); /* save old settings */
/*
 if(ret)
 {
  printf("Error in Terminal Initialisation Code: %d\n", ret);
  printf("c_iflag = %o, c_oflag = %o, c_cflag = %o,\n",
    otermio.c_iflag, otermio.c_oflag, otermio.c_cflag);
  printf("c_lflag = %o, c_line = %o, c_cc = {\"\\%o\\%o\\%o\\%o\\%o\\%o\\%o\\%o\"}\n",
    otermio.c_lflag, otermio.c_line, otermio.c_cc[0], otermio.c_cc[1], 
    otermio.c_cc[2], otermio.c_cc[3], otermio.c_cc[4], otermio.c_cc[5], 
    otermio.c_cc[6], otermio.c_cc[7]);
  WpeExit(1);
 }
*/
#ifndef TERMCAP
 /* initscr() sets the global stdscr itself; do not assign to it -- a
    reentrant-built ncursesw (e.g. openSUSE) makes stdscr a non-lvalue macro. */
 if (initscr()==(WINDOW *)ERR) exit(27);
#ifdef NCURSES
 cbreak();
 noecho();
 nonl();
 intrflush(stdscr,FALSE);
 keypad(stdscr,TRUE);
 /* Deliver a bare Esc after 25ms instead of the ~1s default, so a single
    Esc closes dialogs/menus (no 3-press workaround that can be misread as
    an escape sequence and spin the File Manager). */
 set_escdelay(25);
#if MOUSE
 mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
 mouseinterval(0);
 e_mouse_tracking_enable();
#endif
#endif
 if (has_colors())
 {
  int ncol;
  start_color();
  /* Only the 16x16 base pairs are ever selected (fk_colset uses bg,fg 0..15),
     so cap the init at 16 -- never COLORS, which is 16777216 on a direct-colour
     terminal and would loop forever. */
  ncol = COLORS < 16 ? COLORS : 16;
  for (i = 0; i < ncol; i++)
  {
   for (k = 0; k < ncol; k++)
   {
    if (i != 0 || k != 0)
    {
     init_pair(i * 8 + k, k, i);
    }
   }
  }
  e_t_truecolor_detect();
 }
#endif
 e_begscr();
 schirm = calloc(MAXSCOL * MAXSLNS, sizeof(SCREENCELL));
 altschirm = calloc(MAXSCOL * MAXSLNS, sizeof(SCREENCELL));
#if !defined(NO_XWINDOWS) && defined(NEWSTYLE)
 extbyte = MALLOC(MAXSCOL * MAXSLNS);
#endif
 if(init_cursor())
 {
  printf("Terminal Not in the right mode\n");
  e_exit(1);
 }
 tcgetattr(0, &ntermio);
 ntermio.c_iflag = 0;            /* setup new settings */
 ntermio.c_oflag = 0;
 ntermio.c_lflag = 0;
 ntermio.c_cc[VMIN] = 1;
 ntermio.c_cc[VTIME] = 0;
#ifdef VSWTCH
 ntermio.c_cc[VSWTCH] = 0;
#endif
 tcsetattr(0, TCSADRAIN, &ntermio);
 if (spc_in) e_putp(spc_in);
 return(0);
}

int svflgs, kbdflgs;

int e_begscr()
{
 int cols, lns;

 kbdflgs = fcntl( 0, F_GETFL, 0 );
#ifndef TERMCAP
#ifndef NCURSES
 setupterm((char *)0, 1, (int *)0);
#endif
 if ((lns = tigetnum("lines")) > 0)
  MAXSLNS = lns;
 if ((cols = tigetnum("cols")) > 0)
  MAXSCOL = cols;
#else
 if ((tc_screen = getenv("TERM")) == NULL)
  e_exitm("Environment variable TERM not defined!", 1);
 if ((tgetent(tcbuf, tc_screen)) != 1)
  e_exitm("Unknown terminal type!", 1);
 if ((lns = tgetnum("li")) > 0)
  MAXSLNS = lns;
 if ((cols = tgetnum("co")) > 0)
  MAXSCOL = cols;
#endif
 return(0);
}

#define fk_putp(p) ( p ? e_putp(p) : e_putp(att_no) )

void e_endwin()
{
#ifdef NCURSES
#if MOUSE
 e_mouse_tracking_disable();
#endif
 endwin();
#else
 fk_putp(ratt_bo);
#endif
 tcsetattr(0, TCSADRAIN, &otermio);
}

int fk_t_cursor(int x)
{
 return(x);
}

int fk_t_putchar(int c)
{
#ifdef NCURSES
 addch(c);
 return c;
#else
 return(fputc(c, stdout));
#endif
}

int fk_attrset(int a)
{
 if(cur_attr == a) return(0);
#ifdef NCURSES
 switch(a)
 {  case 0:  attrset(A_NORMAL);  break;
    case 1:  attrset(A_STANDOUT);  break;
    case 2:  attrset(A_UNDERLINE);  break;
    case 4:  attrset(A_REVERSE);  break;
    case 8:  attrset(A_BLINK);  break;
    case 16:  attrset(A_DIM);  break;
    case 32:  attrset(A_BOLD);  break;
 }
#else
 switch(cur_attr)
 {  case 0:  fk_putp(ratt_no);  break;
    case 1:  fk_putp(ratt_so);  break;
    case 2:  fk_putp(ratt_ul);  break;
    case 4:  fk_putp(ratt_rv);  break;
    case 8:  fk_putp(ratt_bl);  break;
    case 16:  fk_putp(ratt_dm);  break;
    case 32:  fk_putp(ratt_bo);  break;
 }
 switch(a)
 {  case 0:  fk_putp(att_no);  break;
    case 1:  fk_putp(att_so);  break;
    case 2:  fk_putp(att_ul);  break;
    case 4:  fk_putp(att_rv);  break;
    case 8:  fk_putp(att_bl);  break;
    case 16:  fk_putp(att_dm);  break;
    case 32:  fk_putp(att_bo);  break;
 }
#endif
 return(cur_attr = a);
}

/* --- Semantic-token truecolor on the console (1.6.6) ------------------------
 * How a 24-bit semantic-token colour (ATTR_TC cells) reaches the terminal,
 * decided once at start_color() time by e_t_truecolor_detect():
 *   E_TC_DIRECT  -- the terminfo advertises RGB (xterm-direct, kitty, tmux with
 *                   the RGB override): an ncurses colour NUMBER is itself a
 *                   packed 24-bit RGB, so we hand init_extended_pair the packed
 *                   fg/bg and ncurses emits the exact "ESC[38:2::r:g:b" SGR.
 *   E_TC_PALETTE -- 256 colours and can_change_color(): redefine a high palette
 *                   slot to the exact RGB (ncurses emits OSC 4) and reference it
 *                   as a 256-colour index.
 *   E_TC_NONE    -- neither (plain tmux/screen, 8/16-colour): fall back to the
 *                   cell's 16-colour foreground (orange -> red), no regression.
 * No raw escapes are written behind ncurses' back -- it owns the emission, so
 * its attribute-state tracking stays in sync. */
enum { E_TC_NONE = 0, E_TC_DIRECT, E_TC_PALETTE };
static int e_t_tc_mode = E_TC_NONE;
#define E_TC_PAIR_BASE   512    /* extended-pair numbers for semantic truecolor */
#define E_TC_COLOR_BASE  240    /* palette slots (re)defined in PALETTE mode     */

/* Standard ANSI-16 RGB.  In DIRECT mode every colour number is a raw RGB triple
   (no palette), so a truecolor token cell's BACKGROUND must be given as the RGB
   matching its 16-colour index, or it would render as a near-black RGB(0,0,n). */
static const int e_t_ansi16_rgb[16] = {
 0x000000, 0xCD0000, 0x00CD00, 0xCDCD00, 0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
 0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00, 0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF
};

static void e_t_truecolor_detect(void)
{
#if defined(NCURSES) && defined(HAVE_INIT_EXTENDED_PAIR)
 e_t_tc_mode = E_TC_NONE;
 if (tigetflag("RGB") > 0 || COLORS >= (1 << 24))
  e_t_tc_mode = E_TC_DIRECT;
 else if (COLORS >= 256 && can_change_color())
  e_t_tc_mode = E_TC_PALETTE;
#endif
}

/* Select an ncurses extended pair painting semantic-token truecolor @slot (its
   24-bit fg from e_lsp_sem_slot_rgb) over 16-colour background @bg.  Pairs are
   allocated lazily into a (slot x bg) cache and initialised per the detected
   tier.  Returns 1 if a truecolor pair was selected, 0 if truecolor is
   unavailable (the caller then paints the 16-colour fallback). */
static int e_t_sem_tc_set(int slot, int bg)
{
#if defined(NCURSES) && defined(HAVE_INIT_EXTENDED_PAIR)
 static unsigned char inited[256];
 int r, g, b, key, pairno, ep;

 if (e_t_tc_mode == E_TC_NONE)
  return(0);
 if (!e_lsp_sem_slot_rgb(slot, &r, &g, &b))
  return(0);
 key = ((slot & 0x0F) << 4) | (bg & 0x0F);
 pairno = E_TC_PAIR_BASE + key;
 if (!inited[key])
 {
  if (e_t_tc_mode == E_TC_DIRECT)
   init_extended_pair(pairno, (r << 16) | (g << 8) | b,
                      e_t_ansi16_rgb[bg & 0x0F]);
  else
  {
   int cno = E_TC_COLOR_BASE + (slot & 0x0F);
   init_extended_color(cno, r * 1000 / 255, g * 1000 / 255, b * 1000 / 255);
   init_extended_pair(pairno, cno, bg & 0x0F);
  }
  inited[key] = 1;
 }
 ep = pairno;
 attr_set(A_NORMAL, 0, &ep);
 return(1);
#else
 (void)slot; (void)bg;
 return(0);
#endif
}

void fk_colset(int c)
{
 int bg, base;
 if (cur_attr == c) return;
 cur_attr = c;
 base = ATTR_BASE(c);
 bg = base / 16;
#ifdef NCURSES
 /* A semantic-token cell that wants a 24-bit colour: paint it through an
    extended truecolor pair if the terminal can; otherwise fall through to the
    16-colour foundation below (the fallback fg is already in `base`). */
 if (ATTR_IS_TC(c) && e_t_sem_tc_set(ATTR_TC_SLOT(c), bg))
  return;
#endif
 c = base;          /* below here colour is the plain 0..255 attribute */
#ifdef TERMCAP
 if ((c %= 16) >= col_num)
 {
  fk_putp(att_bo);
  e_putp(tgoto(col_fg, 0, c % col_num));
  e_putp(tgoto(col_bg, 0, bg));
 }
 else
 {
  fk_putp(ratt_bo);
  e_putp(tgoto(col_fg, 0, c));
  e_putp(tgoto(col_bg, 0, bg));
 }
#else
#ifdef NCURSES
 if (c & 8)
  attrset(A_BOLD);
 else
  attrset(A_NORMAL);
 color_set((bg * 8) + c % 8, NULL);
#else
 if ((c %= 16) >= col_num)
 {
  fk_putp(att_bo);
  e_putp(tparm1(col_fg, c % col_num));
  e_putp(tparm1(col_bg, bg));
 }
 else
 {
  fk_putp(ratt_bo);
  e_putp(tparm1(col_fg, c));
  e_putp(tparm1(col_bg, bg));
 }
#endif
#endif
}

int e_t_refresh()
{
 int x = cur_x, y = cur_y, i, j, c;
 int ref_slns, ref_scol;
 sigset_t block, prev;
 sigemptyset(&block);
 sigaddset(&block, SIGWINCH);
 sigprocmask(SIG_BLOCK, &block, &prev);
 ref_slns = MAXSLNS < LINES ? MAXSLNS : LINES;
 ref_scol = MAXSCOL < COLS  ? MAXSCOL : COLS;
 fk_cursor(0);
 for(i = 0; i < ref_slns; i++)
  for(j = 0; j < ref_scol; j++)
  {
   if (i == ref_slns-1 && j == ref_scol-1) break;
   if (schirm[i * MAXSCOL + j].ch != altschirm[i * MAXSCOL + j].ch ||
     schirm[i * MAXSCOL + j].attr != altschirm[i * MAXSCOL + j].attr)
   {
    if (cur_x != j || cur_y != i)
     term_move(j, i);
    if (cur_x < MAXSCOL)
    {  cur_x = j + 1;  cur_y = i;  }
    else
    {  cur_x = 0;  cur_y = i+1;  }
    if (col_num <= 0)
     fk_attrset(e_gt_col(j, i));
    else
     fk_colset(e_gt_col(j, i));
    c = e_gt_char(j, i);
    /* On a non-UTF-8 terminal, swap the few Unicode chrome symbol glyphs
       (close/zoom title-bar boxes) for an ASCII stand-in BEFORE the wcwidth()
       guard below, which would otherwise see them as unprintable and blank the
       buttons.  Borders/scrollbar are unaffected (they use ncurses ACS). */
    if (!e_t_unicode_term && c > 127)
    {
     int fb = e_t_chrome_ascii(c);
     if (fb)
      c = fb;
    }
    /* Sanitise: reject values that xwpe cannot have produced.
       Valid: 0-12 (sp_chr border/scrollbar chars), 32-127 (ASCII),
       128+ only if wcwidth >= 0 (printable Unicode from UTF-8 decode).
       Everything else is uninitialised SCREENCELL data.
       Fix schirm in place so the garbage is not saved by e_open_view
       and propagated through pic->p save/restore cycles. */
    if (c < 0 || (c >= NSPCHR && c < 32) ||
        (c > 127 && wcwidth((wchar_t)c) < 0))
    {
     c = ' ';
     e_pr_char(j, i, ' ', 0);
    }
#ifdef NCURSES
    if (c < NSPCHR)
     addch(sp_chr[c]);
    else if (c > 127)
    {
     /* Wide character: output via add_wch for proper positioning */
     cchar_t cc;
     wchar_t wc[2] = { (wchar_t)c, 0 };
     int cw = wcwidth((wchar_t)c);
     setcchar(&cc, wc, A_NORMAL, 0, NULL);
     add_wch(&cc);
     /* Skip placeholder cells for wide chars (emoji, CJK = 2 columns) */
     if (cw > 1)
     {
      int k;
      for (k = 1; k < cw && j+k < MAXSCOL; k++)
       altschirm[(i) * MAXSCOL + (j+k)] = schirm[(i) * MAXSCOL + (j+k)];
      j += cw - 1;
      cur_x = j + 1;
     }
    }
    else if (c >= NSPCHR)
     addch(c);
    /* else: c < 0 -- skip uninitialised/invalid cell */
#else
    if (c < NSPCHR)
     e_putp(sp_chr[c]);
    else
     fputc(c, stdout);
#endif
    altschirm[i * MAXSCOL + j] = schirm[i * MAXSCOL + j];
   }
  }
 fk_cursor(1);
 fk_locate(x, y);
 term_refresh();
 sigprocmask(SIG_SETMASK, &prev, NULL);
 return(0);
}

int e_t_sys_ini()
{
 e_refresh();
 tcgetattr(0, &ttermio);
 svflgs = fcntl( 0, F_GETFL, 0 );
 e_endwin();
 return(0);   
}

/**
 * e_t_rearm_mouse - Make window drag/resize work again after running a tool.
 *
 * Used when returning to the editor from a sub-process (e.g. an F9 compile),
 * which had to hand the real terminal to the tool and so turned off mouse
 * motion reporting. Without re-arming it the user can still click, but
 * dragging a title bar to move a window or a corner to resize it silently does
 * nothing. The motion escape must be emitted AFTER the screen is restored,
 * because some terminals reset the mouse mode when the screen is switched back;
 * we flush that screen switch first, then re-enable tracking last.
 */
static void e_t_rearm_mouse(void)
{
 refresh();                  /* flush ncurses ca-mode re-entry (1049h) first */
 e_mouse_tracking_enable();  /* then the motion-tracking escape, emitted last */
}

int e_t_sys_end()
{
 tcsetattr(0, TCSADRAIN, &ttermio);
 fcntl( 0, F_SETFL, svflgs );
 e_abs_refr();
 e_t_rearm_mouse();
 fk_locate(0, 0);
 return(0);
}

int e_t_kbhit()
{
 int ret;
 char kbdflgs, c;

 e_refresh();
 kbdflgs = fcntl(0, F_GETFL, 0 );
 fcntl(0, F_SETFL, kbdflgs | O_NONBLOCK);
 ret = read(0, &c, 1);
 fcntl(0, F_SETFL, kbdflgs & ~O_NONBLOCK);
 return (ret == 1 ? c : 0);
}

#ifdef NCURSES
static int e_t_utf8_assemble(int first)
{
 int cp, i, expect;
 int cont;

 if ((first & 0xE0) == 0xC0)
  { cp = first & 0x1F; expect = 1; }
 else if ((first & 0xF0) == 0xE0)
  { cp = first & 0x0F; expect = 2; }
 else if ((first & 0xF8) == 0xF0)
  { cp = first & 0x07; expect = 3; }
 else
  return first;
 for (i = 0; i < expect; i++)
 {
  timeout(INPUT_POLL_MS);
  cont = fk_getch();
  if (cont == ERR)
   return first;            /* sequence stalled: emit the lead byte as-is */
  if ((cont & 0xC0) != 0x80)
  {
   ungetch(cont);           /* not a continuation byte: requeue it so the
                               next read sees it instead of dropping it */
   return first;
  }
  cp = (cp << 6) | (cont & 0x3F);
 }
 return cp;
}

/* Discard any typed-ahead keys (ncurses).  Called after a long synchronous
   operation (e.g. the first language-server start, which can freeze the UI for
   seconds while a JVM boots): keys mashed during the freeze would otherwise be
   replayed afterwards and fire unintended actions. */
void e_t_flush_input(void)
{
 /* The type-ahead sits in the KERNEL tty queue: ncurses never getch()'d during
    the freeze, so flushinp() (which only clears ncurses' own FIFO) does not see
    it and the bytes get read later as stray commands (an F9 there fired a
    spurious Make).  Discard the kernel queue first, then ncurses' buffer. */
 tcflush(0, TCIFLUSH);
 flushinp();
 { const char *tp = getenv("XWPE_UI_TRACE");   /* XWPE_UI_TRACE: diag only */
   if (tp) { FILE *tf = fopen(tp, "a");
     if (tf) { fprintf(tf, "e_t_flush_input RAN\n"); fclose(tf); } } }
}

static int e_t_getch_poll(void)
{
 static int stdin_registered = 0;
 extern int e_d_async_pending;
 extern void e_d_place_cursor_in_messages(void);
 int c;
#ifdef HAVE_LIBGPM
 /* GPM (bare Linux console) delivers mouse events on its own fd, not stdin.
    ncurses does not pump it here (we opened the gpm connection ourselves), so
    the input loop must poll gpm_fd and drain it via WpeGpmReadable -- otherwise
    the pointer never moves and clicks never register on the console. */
 static int gpm_registered = 0;
 extern int g_gpm_click_pending;
 void WpeGpmReadable(int fd, void *data);
 int WpeGpmFd(void);
#endif

 if (!stdin_registered)
 {
  wpe_fd_add(STDIN_FILENO, POLLIN, NULL, NULL);
  stdin_registered = 1;
 }
#ifdef HAVE_LIBGPM
 if (!gpm_registered)
 {
  int gfd = WpeGpmFd();
  if (gfd >= 0)
  {
   wpe_fd_add(gfd, POLLIN, WpeGpmReadable, NULL);
   gpm_registered = 1;
  }
 }
#endif
 for (;;)
 {
  timeout(INPUT_POLL_MS);
  c = fk_getch();
  timeout(-1);
  if (c != ERR)
  {
   if ((unsigned int)c >= 0xC0 && (unsigned int)c <= 0xF7)
    c = e_t_utf8_assemble(c);
   return c;
  }
  if (e_d_async_pending)
   e_d_place_cursor_in_messages();
  wpe_fd_poll(-1);
#ifdef HAVE_LIBGPM
  if (g_gpm_click_pending)
  {
   g_gpm_click_pending = 0;   /* e_mouse already filled by WpeGpmHandler */
   return (-1);
  }
#endif
 }
}

/* After a bare ESC, wait at most this long (ms) for a following byte before
   deciding it was Esc alone. An Alt-<key> combo sends ESC and the key together,
   so they arrive within this window; a lone Esc does not. Matches the ncurses
   set_escdelay(25) above. */
#define ESC_ALT_DELAY_MS 25

/* Map an SGR (1006) mouse report's button field @b and final byte @final to the
   ncurses bstate bits e_t_mouse_apply_event() expects, so a hand-decoded SGR
   event is indistinguishable from one getmouse() would have returned.  Bit 0x40
   marks the wheel (64 = up, 65 = down); otherwise the low two bits pick the
   button and @final is 'M' for a press/motion or 'm' for a release. */
static mmask_t e_t_sgr_bstate(int b, int final)
{
 if (b & 0x40)
#if defined(BUTTON4_PRESSED) && defined(BUTTON5_PRESSED)
  return (b & 1) ? BUTTON5_PRESSED : BUTTON4_PRESSED;   /* wheel down / up */
#else
  /* OpenBSD's base curses defines no wheel-button constants; the wheel is
     not dispatched through the button bitmask anyway, so report no buttons. */
  return 0;
#endif
 if (final == 'm')
 {
  switch (b & 3)
  {
   case 0:  return BUTTON1_RELEASED;
   case 1:  return BUTTON2_RELEASED;
   case 2:  return BUTTON3_RELEASED;
   default: return 0;
  }
 }
 switch (b & 3)
 {
  case 0:  return BUTTON1_PRESSED;
  case 1:  return BUTTON2_PRESSED;
  case 2:  return BUTTON3_PRESSED;
  default: return 0;     /* b&3 == 3: motion with no button held */
 }
}

/* Decode an SGR mouse report "ESC [ < b ; x ; y M|m" whose introducer bytes
   (ESC '[' '<') have already been read by e_t_csi_key().  ncurses folds an SGR
   report into a KEY_MOUSE event only when its terminfo entry advertises an
   SGR-capable kmous; where it does not -- FreeBSD's termcap console, OpenBSD's
   base curses -- the raw bytes arrive here instead, so the console mouse would
   silently do nothing.  Parse the report directly, fold it into the persistent
   button state via e_t_mouse_apply_event(), and return the mouse sentinel (-1)
   so e_t_getch() reports the event exactly as it does for the KEY_MOUSE path.
   On Linux/NetBSD, ncurses consumes SGR into KEY_MOUSE and the '<' branch never
   fires, so this is inert there -- no double handling, no regression.  Returns
   0 on a malformed report so the caller can fall through to its generic path. */
static int e_t_sgr_mouse(void)
{
 int n[3], i, c, final;
 MEVENT mev;

 timeout(ESC_ALT_DELAY_MS);
 n[0] = n[1] = n[2] = 0;
 for (i = 0; i < 3; i++)
 {
  while ((c = fk_getch()) >= '0' && c <= '9')
   n[i] = n[i] * 10 + (c - '0');
  if (i < 2 && c != ';')
  {
   timeout(-1);
   return(0);
  }
 }
 final = c;
 timeout(-1);
 if (final != 'M' && final != 'm')
  return(0);

 mev.bstate = e_t_sgr_bstate(n[0], final);
 mev.x = n[1] - 1;     /* SGR coordinates are 1-based; cells are 0-based */
 mev.y = n[2] - 1;
 mev.z = 0;
 e_t_mouse_apply_event(&mev);
 return(-1);
}

/* Decode the tail of a VT100 cursor/navigation escape after we have already read
   ESC and the introducer byte (@intro is '[' for CSI or 'O' for SS3).  ncurses
   keypad() normally folds a whole arrow sequence into one KEY_DOWN/etc. code, but
   the raw escape can slip through un-assembled -- e.g. inside a modal dialog's
   poll loop, where the bytes arrive split across poll cycles -- and then the
   generic ESC handler would mistake ESC '[' for Alt-'[' (e_tast_sim -> 0) and
   leak the final letter ('A'/'B'/...) as a literal, so arrow keys did nothing in
   the Options dialogs and the LSP action picker.  Read the remaining byte(s) and
   return the proper xwpe key, or 0 if it is not a sequence we recognise. */
static int e_t_csi_key(int intro)
{
 int c;

 (void)intro;
 timeout(ESC_ALT_DELAY_MS);
 c = fk_getch();
 timeout(-1);
 switch (c)
 {
  case 'A':  return(CUP);
  case 'B':  return(CDO);
  case 'C':  return(CRI);
  case 'D':  return(CLE);
  case 'H':  return(POS1);
  case 'F':  return(ENDE);
  /* "ESC [ < ..." is an SGR (1006) mouse report ncurses did not fold into
     KEY_MOUSE (FreeBSD termcap console / OpenBSD base curses); decode it here. */
  case '<':  return(e_t_sgr_mouse());
  /* "ESC [ <n> ~" forms: consume the trailing '~' (or modifier digits). */
  case '1': case '7':  { int t; timeout(ESC_ALT_DELAY_MS); do t = fk_getch(); while (t >= '0' && t <= '9'); timeout(-1); return(POS1); }
  case '4': case '8':  { int t; timeout(ESC_ALT_DELAY_MS); do t = fk_getch(); while (t >= '0' && t <= '9'); timeout(-1); return(ENDE); }
  case '2':  { int t; timeout(ESC_ALT_DELAY_MS); do t = fk_getch(); while (t >= '0' && t <= '9'); timeout(-1); return(EINFG); }
  case '3':  { int t; timeout(ESC_ALT_DELAY_MS); do t = fk_getch(); while (t >= '0' && t <= '9'); timeout(-1); return(ENTF); }
  case '5':  { int t; timeout(ESC_ALT_DELAY_MS); do t = fk_getch(); while (t >= '0' && t <= '9'); timeout(-1); return(BUP); }
  case '6':  { int t; timeout(ESC_ALT_DELAY_MS); do t = fk_getch(); while (t >= '0' && t <= '9'); timeout(-1); return(BDO); }
  default:   return(0);
 }
}

int e_t_getch()
{
 int c, bk;
 extern int g_mouse_buttons;

 e_refresh();
 c = e_t_getch_poll();
 if (c > KEY_CODE_YES)
 {
  switch (c)
  {
   case KEY_F(1):  c = F1; break;
   case KEY_F(2):  c = F2; break;
   case KEY_F(3):  c = F3; break;
   case KEY_F(4):  c = F4; break;
   case KEY_F(5):  c = F5; break;
   case KEY_F(6):  c = F6; break;
   case KEY_F(7):  c = F7; break;
   case KEY_F(8):  c = F8; break;
   case KEY_F(9):  c = F9; break;
   case KEY_F(10):  c = F10; break;
   case KEY_UP:  c = CUP; break;
   case KEY_DOWN:  c = CDO; break;
   case KEY_LEFT:  c = CLE; break;
   case KEY_RIGHT:  c = CRI; break;
   case KEY_IC:  c = EINFG; break;
   case KEY_DC:  c = ENTF; break;
   case KEY_PPAGE:  c = BUP; break;
   case KEY_NPAGE:  c = BDO; break;
   case KEY_HOME:  c = POS1; break;
   case KEY_END:  c = ENDE; break;
   case KEY_BTAB:  c = WPE_BTAB; break;
   case KEY_RESIZE:
   {
    int i, old_scol = MAXSCOL, old_slns = MAXSLNS;
    /* Drain any queued KEY_RESIZE events (resize often arrives in
       bursts).  Same technique as dialog(1) dlg_will_resize(). */
    { int _ch;
      nodelay(stdscr, TRUE);
      while ((_ch = getch()) == KEY_RESIZE)
       ;
      if (_ch != ERR)
       ungetch(_ch);
      nodelay(stdscr, FALSE);
    }
    /* ncurses already called resizeterm() and updated LINES/COLS */
    MAXSCOL = COLS;
    MAXSLNS = LINES;
    if (MAXSLNS < 6) MAXSLNS = 6;
    if (MAXSCOL < 30) MAXSCOL = 30;
    if (MAXSCOL != old_scol || MAXSLNS != old_slns)
    {
     schirm = REALLOC(schirm, sizeof(SCREENCELL) * MAXSCOL * MAXSLNS);
     altschirm = REALLOC(altschirm, sizeof(SCREENCELL) * MAXSCOL * MAXSLNS);
     memset(schirm, 0, sizeof(SCREENCELL) * MAXSCOL * MAXSLNS);
     memset(altschirm, 0, sizeof(SCREENCELL) * MAXSCOL * MAXSLNS);
#if !defined(NO_XWINDOWS) && defined(NEWSTYLE)
     extbyte = REALLOC(extbyte, MAXSCOL * MAXSLNS);
#endif
     e_relayout_windows(WpeEditor, old_scol, old_slns);
     e_free_all_pics(WpeEditor);
     e_repaint_desk(WpeEditor->f[WpeEditor->mxedt]);
    }
    return WPE_RESIZE;
   }
   case KEY_BACKSPACE:  c = WPE_DC; break;
#if MOUSE
   case KEY_MOUSE:
   {
    MEVENT mev;
    if (getmouse(&mev) == OK)
    {
     e_t_mouse_apply_event(&mev);
     return(-1);
    }
    c = 0;
    break;
   }
#endif
   case KEY_HELP:  c = HELP; break;
   case KEY_LL:  c = ENDE; break;
   case KEY_F(17):  c = SF1; break;
   case KEY_F(18):  c = SF2; break;
   case KEY_F(19):  c = SF3; break;
   case KEY_F(20):  c = SF4; break;
   case KEY_F(21):  c = SF5; break;
   case KEY_F(22):  c = SF6; break;
   case KEY_F(23):  c = SF7; break;
   case KEY_F(24):  c = SF8; break;
   case KEY_F(25):  c = SF9; break;
   case KEY_F(26):  c = SF10; break;
   /* Modern terminals (xterm modifyOtherKeys, gnome/vte, kitty) report
      Ctrl-/Alt-modified function keys as distinct ncurses keycodes in the
      standard ranges -- Ctrl-Fn = KEY_F(24+n), Alt-Fn = KEY_F(48+n) -- which
      the offset-via-bioskey() path cannot recover because bioskey() reads the
      Linux-console keyboard state and returns 0 in an emulator.  Map the ones
      xwpe binds as shortcuts so they work in a terminal emulator too, not
      only on a bare VT.  (A full Shift/Ctrl/Alt function-key table for all
      terminals is a 1.6.4 cleanup -- see TODO.) */
   case KEY_F(33):  c = CF9; break;   /* Ctrl-F9  -> Run program        */
   case KEY_F(53):  c = AF5; break;   /* Alt-F5   -> Borland User Screen */
   case KEY_PREVIOUS:  c = CUP+512; break;
   case KEY_NEXT:  c = CDO+512; break;
   default:  c = 0; break;
  }
  bk = bioskey();
  if (bk & 3)
   c += 512;
  else if (bk & 4)
   c += 514;
 }
 else if ( c == WPE_TAB )
 {
  bk = bioskey(); 
  if ( bk & 3)
   c = WPE_BTAB;
  else
   c = WPE_TAB;
 }
 else if (c == WPE_ESC)
 {
  /* A lone Esc must register on the first press. Wait only briefly for a
     following byte: an Alt-<key> combo sends ESC+key together (arrives within
     the window), a lone Esc does not -- so do not block for a second press.
     Fixes "Esc needs several presses" on the Linux console. */
  timeout(ESC_ALT_DELAY_MS);
  c = fk_getch();
  timeout(-1);
  if (c == ERR)
   return(WPE_ESC);
  if (c > KEY_CODE_YES)
  {
   switch (c)
   {
    case KEY_F(1):  c = AF1; break;
    case KEY_F(2):  c = AF2; break;
    case KEY_F(3):  c = AF3; break;
    case KEY_F(4):  c = AF4; break;
    case KEY_F(5):  c = AF5; break;
    case KEY_F(6):  c = AF6; break;
    case KEY_F(7):  c = AF7; break;
    case KEY_F(8):  c = AF8; break;
    case KEY_F(9):  c = AF9; break;
    case KEY_F(10):  c = AF10; break;
    case KEY_UP:  c = BUP; break;
    case KEY_DOWN:  c = BDO; break;
    case KEY_LEFT:  c = CCLE; break;
    case KEY_RIGHT:  c = CCRI; break;
    case KEY_IC:  c = EINFG+512; break;
    case KEY_DC:  c = ENTF+512; break;
    case KEY_PPAGE:  c = CBUP; break;
    case KEY_NPAGE:  c = CBDO; break;
    case KEY_HOME:  c = CPS1; break;
    case KEY_END:  c = CEND; break;
    case KEY_BACKSPACE:  c = AltBS; break;
    case KEY_HELP:  c = AF1; break;
    case KEY_LL:  c = CEND; break;
    case KEY_PREVIOUS:  c = BUP+512; break;
    case KEY_NEXT:  c = BDO+512; break;
    default:  c = 0; break;
   }
   bk = bioskey();
   if (bk & 3)
    c += 512;
   else if (bk & 4)
    c += 514;
  }
  else if (c == '[' || c == 'O')
  {
   /* ESC '[' / ESC 'O' that ncurses did not fold into a KEY_ code: decode the
      cursor/nav sequence so arrow keys still work (dialogs, the LSP picker). */
   int k = e_t_csi_key(c);
   if (k)
    c = k;
   else
    c = e_tast_sim(c);
  }
  else if (c != WPE_ESC)
   c = e_tast_sim(c);
 }
 return(c);
}

#else
int e_t_getch()
{
 int c, c2, pshift, bk;

 pshift = 0;
 e_refresh();
 if ((c = fk_getch()) != WPE_ESC)
 {
  if (key_f[20] && c == *key_f[20])
   return(WPE_DC);
  else if (key_f[17] && c == *key_f[17])
   return(ENTF);
  else if ( c == WPE_TAB )
  {
   bk = bioskey(); 
   if (bk & 3) 
    return (WPE_BTAB); 
   else
    return (WPE_TAB); 
  }
  else
   return(c);
 }
 else if ((c = fk_getch()) == WPE_CR)
  return(WPE_ESC);
 bk = bioskey();
 if (bk & 3)
  pshift = 512;
 else if (bk & 4)
  pshift = 514;
 if (c == WPE_ESC)
 {
  if ((c = fk_getch()) == WPE_ESC)
   return(WPE_ESC);
  if ((c2 = e_find_key(c, 1, 1)))
   return(c2 + pshift);
 }
 if ((c2 = e_find_key((char)c, 1, 0)))
  return(c2 + pshift);
 return(e_tast_sim(c + pshift));
}

char e_key_save[10];

int e_find_key(int c, int j, int sw)
{
   int i, k;
   e_key_save[j] = c;
   e_key_save[j+1] = '\0';
   for(i = 0; i < KEYFN; i++)
   {  if(key_f[i] == NULL) continue;
      for(k = 1; k <= j && e_key_save[k] == *(key_f[i] + k); k++);
      if(k > j)
      {  if(*(key_f[i] + k) != '\0') return(e_find_key(fk_getch(), j+1, sw));
	 else if(sw)
	 {  switch(i)
	    {  case 0:  return(AF1);
	       case 1:  return(AF2);
	       case 2:  return(AF3);
	       case 3:  return(AF4);
	       case 4:  return(AF5);
	       case 5:  return(AF6);
	       case 6:  return(AF7);
	       case 7:  return(AF8);
	       case 8:  return(AF9);
	       case 9:  return(AF10);
	       case 10:  return(BUP);
	       case 11:  return(BDO);
	       case 12:  return(CCLE);
	       case 13:  return(CCRI);
	       case 14:  return(EINFG);
	       case 15:  return(CPS1);
	       case 16:  return(CBUP);
	       case 17:  return(ENTF);
	       case 18:  return(CEND);
	       case 19:  return(CBDO);
	       case 20:  return(AltBS);
	       case 21:  return(AF1);
	       case 22:  return(CEND);
	       case 33:  return(BUP+512);
	       case 34:  return(BDO+512);
	       case 35:  return(CCLE+512);
	       case 36:  return(CCRI+512);
	       case 37:  return(CPS1+512);
	       case 38:  return(CBUP+512);
	       case 39:  return(CEND+512);
	       case 40:  return(CBDO+512);
	       case 41:  return(CEND);
	       default:  return(0);
	    }
	 }
	 else
	 {  switch(i)
	    {  case 0:  return(F1);
	       case 1:  return(F2);
	       case 2:  return(F3);
	       case 3:  return(F4);
	       case 4:  return(F5);
	       case 5:  return(F6);
	       case 6:  return(F7);
	       case 7:  return(F8);
	       case 8:  return(F9);
	       case 9:  return(F10);
	       case 10:  return(CUP);
	       case 11:  return(CDO);
	       case 12:  return(CLE);
	       case 13:  return(CRI);
	       case 14:  return(EINFG);
	       case 15:  return(POS1);
	       case 16:  return(BUP);
	       case 17:  return(ENTF);
	       case 18:  return(ENDE);
	       case 19:  return(BDO);
	       case 20:  return(WPE_DC);
	       case 21:  return(HELP);
	       case 22:  return(ENDE);
	       case 23:  return(SF1);
	       case 24:  return(SF2);
	       case 25:  return(SF3);
	       case 26:  return(SF4);
	       case 27:  return(SF5);
	       case 28:  return(SF6);
	       case 29:  return(SF7);
	       case 30:  return(SF8);
	       case 31:  return(SF9);
	       case 32:  return(SF10);
	       case 33:  return(CUP+512);
	       case 34:  return(CDO+512);
	       case 35:  return(CLE+512);
	       case 36:  return(CRI+512);
	       case 37:  return(POS1+512);
	       case 38:  return(BUP+512);
	       case 39:  return(ENDE+512);
	       case 40:  return(BDO+512);
	       case 41:  return(ENDE);
	       default:  return(0);
	    }
	 }
      }
   }
   return(0);
}
#endif

int fk_t_locate(int x, int y)
{
 if (col_num > 0) 
 {
  fk_colset(e_gt_col(cur_x, cur_y));
#ifdef NCURSES
  /* Causes problems.  Reason unknown. - Dennis */
  /*mvaddch(cur_y,cur_x,e_gt_char(cur_x, cur_y));*/
#else
  fputc(e_gt_char(cur_x, cur_y), stdout);
#endif
 }
 cur_x = x;
 cur_y = y;
 term_move(x, y);
 return(y);
}

static int e_t_mouse_decode_button(mmask_t bstate)
{
 if (bstate & (BUTTON1_PRESSED|BUTTON1_CLICKED)) return 1;
 if (bstate & (BUTTON2_PRESSED|BUTTON2_CLICKED)) return 2;
 if (bstate & (BUTTON3_PRESSED|BUTTON3_CLICKED)) return 4;
 return -1;
}

static int e_t_mouse_is_released(mmask_t bstate)
{
 return (bstate & (BUTTON1_RELEASED|BUTTON2_RELEASED|BUTTON3_RELEASED)) != 0;
}

/* Fold one ncurses mouse event into the persistent button state.
   e_t_getch (KEY_MOUSE) and fk_t_mouse both receive press/release events one
   at a time; g_mouse_buttons accumulates the pressed-button bitmask across
   events so a drag (press, then motion reports) keeps reporting the held
   button until the matching release arrives.  Updates e_mouse.{x,y,k} from the
   event and returns the new button bitmask.

   A fast click can arrive as a single synthesized BUTTON*_CLICKED (or
   _DOUBLE_/_TRIPLE_) bstate with no matching _RELEASED to follow -- this
   happens whenever the terminal/ncurses pair coalesces press+release (e.g.
   macOS Terminal / iTerm; ncurses still synthesizes _CLICKED for same-poll
   press+release sequences even with mouseinterval(0) in some terminfo
   combinations).  Without an explicit release, the bit accumulated here would
   stay set and any while(e_mshit()) drag-tracking loop (submenu, scroll
   thumb, FM resize, ...) would spin forever.  Mark a pending synthetic
   release; fk_t_mouse consumes it on the next no-event poll, so dispatch
   still observes the click but the spin-loop exits one tick later. */
static int e_t_mouse_apply_event(MEVENT *mev)
{
 extern struct mouse e_mouse;
 mmask_t click_mask = BUTTON1_CLICKED | BUTTON2_CLICKED | BUTTON3_CLICKED
                    | BUTTON1_DOUBLE_CLICKED | BUTTON2_DOUBLE_CLICKED
                    | BUTTON3_DOUBLE_CLICKED | BUTTON1_TRIPLE_CLICKED
                    | BUTTON2_TRIPLE_CLICKED | BUTTON3_TRIPLE_CLICKED;
 mmask_t press_mask = BUTTON1_PRESSED | BUTTON2_PRESSED | BUTTON3_PRESSED;
 int is_click   = (mev->bstate & click_mask) != 0;
 int is_press   = (mev->bstate & press_mask) != 0;
 int is_release = e_t_mouse_is_released(mev->bstate);
 int btn = e_t_mouse_decode_button(mev->bstate);

 if (btn >= 0)
  g_mouse_buttons |= btn;
 else if (is_release)
  g_mouse_buttons = 0;
 e_mouse.x = mev->x;
 e_mouse.y = mev->y;
 e_mouse.k = g_mouse_buttons;
 if (is_click)
  s_t_pending_click_release = 1;
 else if (is_press || is_release)
  s_t_pending_click_release = 0;
 return g_mouse_buttons;
}

/* Publish the current mouse button state and position into the drag-poll result
   array g[] (g[1] = button bitmask, g[2]/g[3] = x/y in 1/8-cell units).  After
   e_t_mouse_apply_event() e_mouse.{x,y} already mirror the event coordinates, so
   both the KEY_MOUSE and the raw-SGR paths report through this one helper. */
static void e_t_mouse_publish(int *g)
{
 extern struct mouse e_mouse;
 g[1] = g_mouse_buttons;
 g[2] = e_mouse.x * MOUSE_CELL_SUBDIV;
 g[3] = e_mouse.y * MOUSE_CELL_SUBDIV;
}

/* During a drag-poll, getch() returned a bare ESC.  On terminals whose terminfo
   lacks an SGR-capable kmous (FreeBSD termcap console, OpenBSD base curses) a
   mouse report arrives as the raw bytes "ESC [ < ..." instead of KEY_MOUSE, so
   without this the drag-tracking loops (scrollbar thumb, submenu, window move)
   would never see the motion.  Try to consume the introducer and decode the
   report; return 1 if a mouse event was applied, else push the two peeked bytes
   back (the caller pushes the ESC) and return 0 for normal key handling. */
static int e_t_mouse_poll_sgr(void)
{
 int b1, b2;

 timeout(ESC_ALT_DELAY_MS);
 b1 = fk_getch();
 b2 = (b1 == '[') ? fk_getch() : ERR;
 timeout(-1);
 if (b1 == '[' && b2 == '<' && e_t_sgr_mouse() == -1)
  return(1);
 if (b2 != ERR)
  ungetch(b2);
 if (b1 != ERR)
  ungetch(b1);
 return(0);
}

int fk_t_mouse(int *g)
{
#if MOUSE
 extern struct mouse e_mouse;
 MEVENT mev;
 int ch;

 if (g[0] == 2)
  return(0);
 timeout(INPUT_POLL_MS);
 ch = getch();
 timeout(-1);
 if (ch == KEY_MOUSE && getmouse(&mev) == OK)
 {
  e_t_mouse_apply_event(&mev);
  e_t_mouse_publish(g);
 }
 else if (ch == WPE_ESC && e_t_mouse_poll_sgr())
 {
  e_t_mouse_publish(g);
 }
 else
 {
  if (s_t_pending_click_release)
  {
   g_mouse_buttons = 0;
   e_mouse.k = 0;
   s_t_pending_click_release = 0;
  }
  else if (ch != ERR && ch != WPE_ESC && g_mouse_buttons != 0)
  {
   /* A non-mouse byte arrived while a drag-poll still thinks a button is
      held.  The matching release SGR may be queued behind it in the kernel
      tty buffer, but ungetch() pushes this byte to the front of ncurses'
      pushback stack so the next getch() never reaches the release -- the
      caller's while(e_mshit()) spins on the same byte.  Cause observed on
      macOS Terminal: a cursor key pressed right after a menu-bar click is
      delivered before the release SGR.  Synthesize the release here: clear
      g_mouse_buttons so the drag loop returns and ungetch the key so the
      next e_getch() through the main path picks it up normally. */
   g_mouse_buttons = 0;
   e_mouse.k = 0;
  }
  e_t_mouse_publish(g);
  if (ch != ERR)
   ungetch(ch);
 }
 return(0);
#else
 return(0);
#endif
}

int e_t_switch_screen(int sw)
{
 static int sav_sw = -1;

 if (sw == sav_sw)
  return(0);
 sav_sw = sw;
 if (sw && beg_scr) 
 {
  term_refresh();
  if (sav_cur)
   e_putp(sav_cur);
  e_putp(beg_scr);
 }
 else if (!sw && swt_scr)
 {
  e_putp(swt_scr);
  if (res_cur)
   e_putp(res_cur);
  term_refresh();
 }
 else
  return(-1);
 return(0);
}

/**
 * e_t_deb_out - Show the program's output (Ctrl-G P / Alt-F5 "Output").
 *
 * The program's stdout/stderr is captured into the Messages window (via the
 * pty drain) in BOTH the terminal and X11 builds, so this is uniform across
 * backends now: raise and focus the Messages panel and scroll to the latest
 * output, the way a modern IDE focuses its integrated output panel.  No
 * backend-specific behaviour, no modal popup, no screen switch.
 *
 * The classic full-screen "user screen" (for programs that paint with cursor
 * positioning or ANSI colour) lives in e_t_user_screen() below; it is not
 * yet bound to a key -- a faithful version (integrated terminal) is planned
 * for a later release.
 */
int e_t_deb_out(FENSTER *f)
{
 extern int e_p_show_program_output(FENSTER *);
 return e_p_show_program_output(f);
}

/**
 * e_t_user_screen - Switch the console to the program's raw full-screen output.
 *
 * Terminal-only.  Drops out of ncurses (endwin), clears the screen and
 * replays the captured program output verbatim, then waits for a keypress
 * before returning to the editor -- the Borland "User Screen" (Alt-F5).
 * Unlike the Messages panel this can show a program that paints (cursor
 * positioning, ANSI colour), because it hands the real terminal back to it.
 *
 * NOT bound to a key yet: the captured buffer is empty after a plain Run and
 * cannot faithfully reproduce a painting program.  The proper version (an
 * integrated VT terminal shared by both backends) is planned for 1.6.4; this
 * function is kept ready to wire to Alt-F5 then.
 */
int e_t_user_screen(FENSTER *f)
{
 extern char *e_d_prog_output;
 extern int e_d_prog_output_len;
 extern void e_d_pty_drain(void);
 int i;
 (void)f;

 e_d_pty_drain();
 endwin();
 /* Borland's "User Screen": leave the editor and show the program's own
    full screen.  Replay the captured output VERBATIM -- the bytes the program
    wrote to its terminal, including cursor positioning, ANSI colour and box
    drawing -- so a program that PAINTS shows exactly as it left the screen.
    No headers and no \n->\r\n rewriting (the pty already emits \r\n), which
    would otherwise corrupt cursor-addressed output. */
 (void) i;
 fputs("\033[2J\033[H", stderr);
 if (e_d_prog_output && e_d_prog_output_len > 0)
  fwrite(e_d_prog_output, 1, e_d_prog_output_len, stderr);
 else
  fputs("(no program output yet -- run with Ctrl-F9 or start a debug session)",
    stderr);
 /* A discreet, reverse-video prompt on its own line so it does not paint over
    the program's last line. */
 fputs("\033[0m\r\n\033[7m xwpe: press any key to return to the editor \033[0m",
   stderr);
 fflush(stderr);
 { struct termios _old, _raw; char _c;
   tcgetattr(0, &_old);
   _raw = _old;
   _raw.c_lflag &= ~(ICANON | ECHO);
   _raw.c_cc[VMIN] = 1;
   _raw.c_cc[VTIME] = 0;
   tcsetattr(0, TCSANOW, &_raw);
   read(0, &_c, 1);
   tcsetattr(0, TCSANOW, &_old);
 }
 clearok(stdscr, TRUE);
 refresh();
 return(0);
}

int e_d_switch_screen(int sw)
{
#ifdef DEBUGGER
 if (!sw)
  e_g_sys_ini();
 else
  e_g_sys_end();
#endif
 return(e_switch_screen(sw));
}

int e_t_d_switch_out(int sw)
{
 static int save_sw = 32000;

 if (save_sw == sw)
  return(0);
 save_sw = sw;
 /* In terminal mode, program output is redirected to a file (not the
    tty), so no screen switching is needed during F7/F8 stepping.
    This eliminates the flicker that the original design caused by
    toggling between alternate and normal screen on every step.
    Just repaint the editor when returning from a debug command. */
 if (!sw)
 {
  /* No clearok needed: with pty output capture, the screen was never
     switched away, so ncurses' state is still valid. Just refresh. */
  e_refresh();
 }
 return(sw);
}

void e_exitm(char *s, int n)
{
 e_endwin();
 if (n != 0)
  printf("\n%s\n", s);
 exit(n);
}

int e_s_sys_ini()
{
 e_sys_ini();
 return(e_switch_screen(0));
}

int e_s_sys_end()
{
 e_switch_screen(1);
 return(e_sys_end());
}

#endif  /*  Is UNIX       */

