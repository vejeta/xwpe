/* unixmakr.h						  */
/* Copyright (C) 1993 Fred Kruse                          */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#ifdef NOSTRSTR
char *strstr(char *s1, char *s2);
char *getcwd(char *dir, int n);
#endif

#ifdef NCURSES
#define e_putp(s) 1
#else
#define e_putp(s) tputs((s), 1, fk_u_putchar)
#endif

extern int cur_on;

#ifdef DEFPGC
#define getc(fp) fgetc((fp))
#endif
#ifdef NCURSES
#define fk_getch() getch()
#else
#ifdef HAVE_LIBGPM
#define fk_getch() Gpm_Getc(stdin)
#else
#define fk_getch() fgetc(stdin)
#endif
#endif
#define WpeExit(n) e_exit((n))

extern char *cur_rc, *cur_vs, *cur_nvs, *cur_vvs;
extern char *att_no, *att_so, *att_ul, *att_rv, *att_bl, *att_dm, *att_bo;
extern int cur_x, cur_y;
extern char *user_shell;

extern char *ctree[5];

/*
 * Screen buffer: cchar_t based (Gold Standard UTF-8 support).
 * Each cell stores one wide character + attributes.
 * 1 cell = 1 visual column on screen.
 */
#include <wchar.h>

typedef struct {
 int ch;    /* character (wchar_t or special char code) */
 int attr;  /* color/attribute byte (xwpe format) */
 int flags; /* CELL_WIDE, CELL_WIDE_SPACER */
} SCREENCELL;

#define CELL_WIDE        0x01  /* first cell of a wide character (wcwidth=2) */
#define CELL_WIDE_SPACER 0x02  /* second cell, continuation of wide char */

extern SCREENCELL *schirm;
extern SCREENCELL *altschirm;

#define SCHIRM_INBOUNDS(x, y) \
 ((unsigned)(x) < (unsigned)MAXSCOL && (unsigned)(y) < (unsigned)MAXSLNS)

#define e_pr_char(x, y, c, frb) do { \
 if (SCHIRM_INBOUNDS(x, y)) { \
  int _sc_i = (y) * MAXSCOL + (x); \
  schirm[_sc_i].ch = (c); \
  schirm[_sc_i].attr = (frb); \
  schirm[_sc_i].flags = 0; \
 } } while(0)

#define e_gt_flags(x, y) \
 (SCHIRM_INBOUNDS(x, y) ? schirm[(y) * MAXSCOL + (x)].flags : 0)
#define e_pt_flags(x, y, f) do { \
 if (SCHIRM_INBOUNDS(x, y)) schirm[(y) * MAXSCOL + (x)].flags = (f); \
 } while(0)

#define e_gt_char(x, y) \
 (SCHIRM_INBOUNDS(x, y) ? schirm[(y) * MAXSCOL + (x)].ch : ' ')
#define e_gt_col(x, y) \
 (SCHIRM_INBOUNDS(x, y) ? schirm[(y) * MAXSCOL + (x)].attr : 0)
#define e_pt_col(x, y, c) do { \
 if (SCHIRM_INBOUNDS(x, y)) schirm[(y) * MAXSCOL + (x)].attr = (c); \
 } while(0)

/*  Pointer to functions for function calls  */

#define fk_locate(x, y) (*fk_u_locate)(x, y)
#define fk_cursor(x) (*fk_u_cursor)(x)
#define e_refresh() (*e_u_refresh)()
#define e_initscr(argc, argv) (*e_u_initscr)(argc, argv)
#define e_getch() (*e_u_getch)()
#define fk_putchar(c) (*fk_u_putchar)(c)
#define e_d_switch_out(c) (*e_u_d_switch_out)(c)
#define e_switch_screen(sw) (*e_u_switch_screen)(sw)
#define e_deb_out(f) (*e_u_deb_out)(f)
#define e_cp_X_to_buffer(f) (*e_u_cp_X_to_buffer)(f)
#define e_copy_X_buffer(f) (*e_u_copy_X_buffer)(f)
#define e_paste_X_buffer(f) (*e_u_paste_X_buffer)(f)

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - *\
  bioskey - Get the status of shift, alt, and control keys.

    Returns: A bit field of the following info
      Bit  Information
       3   Alt key
       2   Control key
       1   Left shift
       0   Right shift
\* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
#define bioskey() (*u_bioskey)()
#define e_sys_ini() (*e_u_sys_ini)()
#define e_sys_end() (*e_u_sys_end)()
#define e_frb_menue(sw, xa, ya, f, md) (*e_frb_u_menue)(sw, xa, ya, f, md)
#define e_pr_col_kasten(xa, ya, x, y, f, sw) \
		(*e_pr_u_col_kasten)(xa, ya, x, y, f, sw)
#define e_s_clr(f, b) (*e_s_u_clr)(f, b)
#define e_n_clr(fb) (*e_n_u_clr)(fb)

#define REALLOC(p, n) realloc((p), (n))
#define MALLOC(n) malloc(n)
#define FREE(n) free(n)

#ifdef NEWSTYLE
extern char *extbyte, *altextbyte;
#endif

#define sc_txt_1(f) { if(f->c_sw) f->c_sw = e_sc_txt(f->c_sw, f->b); }

#define sc_txt_2(f) 							\
{   if(f->c_sw) 							\
    {	if(f->s->mark_begin.y == f->s->mark_end.y)                      \
        e_sc_nw_txt(f->s->mark_end.y, f->b, 0);                         \
	else								\
	{  f->c_sw = REALLOC(f->c_sw, f->b->mx.y * sizeof(int));	\
	   f->c_sw = e_sc_txt(f->c_sw, f->b);				\
	}								\
    }									\
}
#define sc_txt_3(y, b, sw) {  if(b->f->c_sw) e_sc_nw_txt(y, b, sw);  }
#define sc_txt_4(y, b, sw)						\
{  if(b->f->c_sw && !e_undo_sw) e_sc_nw_txt(y, b, sw);  }

