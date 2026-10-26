/* we_hfkt.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "edit.h"
#include "we_fdloop.h"
#include <regex.h>

/*        find string in text line    */
int e_strstr(int x, int n, unsigned char *s, unsigned char *f)
{
 int i, j, nf = strlen(f);

 if (x > n)
 {
  for (i = x - nf; i >= n; i--)
  {
   for (j = 0; j < nf; j++)
    if (s[i+j] != f[j])
     break;
   if (j == nf)
    return(i);
  }
 }
 else
 {
  for (i = x >= 0 ? x : 0; i <= n - nf /* && s[i] != '\0' && s[i] != WR */; i++)
  {
   for (j = 0; j < nf; j++)
    if (s[i+j] != f[j])
     break;
   if (j == nf)
    return(i);
  }
 }
 return(-1);
}

/*        Find string in line (ignoring case)   */
int e_ustrstr(int x, int n, unsigned char *s, unsigned char *f)
{
 int i, j, nf = strlen(f);

 if(x > n)
 {
  for (i = x-nf; i >= n; i--)
  {
   for (j = 0; j < nf; j++)
    if (e_toupper(s[i+j]) != e_toupper(f[j]))
     break;
   if (j == nf)
    return(i);
  }
 }
 else
 {
  for (i = x < 0 ? 0 : x; i <= n - nf; i++)
  {
   for (j = 0; j < nf; j++)
    if (e_toupper(s[i+j]) != e_toupper(f[j]))
     break;
   if (j == nf)
    return(i);
  }
 }
 return(-1);
}

/*   find string in text line (including control chars), case insensitive */
int e_urstrstr(int x, int n, unsigned char *s, unsigned char *f, int *nn)
{
 int i;
 unsigned char *str;
 unsigned char *ft = MALLOC((strlen(f)+1)*sizeof(unsigned char));

 if (x <= n)
 {
  str = MALLOC((n+1)*sizeof(unsigned char));
  for (i = 0; i < n; i++)
   str[i] = e_toupper(s[i]);
  str[n] = '\0';
 }
 else
 {
  str = MALLOC((x+1)*sizeof(unsigned char));
  for (i = 0; i < x; i++)
   str[i] = e_toupper(s[i]);
  str[x] = '\0';
 }
 for (i = 0; (ft[i] = e_toupper(f[i])) != '\0'; i++)
  ;

 i = e_rstrstr(x, n, str, ft, nn);
 FREE(str);
 FREE(ft);
 return(i);
}

/*   find string in text line (including control chars) */
int e_rstrstr(int x, int n, unsigned char *s, unsigned char *f, int *nn)
{
 regex_t *regz;
 regmatch_t *matches = NULL;
 int start, end, i, len;
 int res;
 unsigned char old;

 regz = MALLOC(sizeof(regex_t));
 if (regcomp(regz,f,REG_EXTENDED))
 {
  free(regz);
  return (-1);
 }
 len = regz->re_nsub;
 if (len)
  matches = MALLOC(len*sizeof(regmatch_t));
 start = (x < n) ? x : n;
 end = (n > x) ? n : x;
 if (start < 0)
  start = 0;
 old = s[end];/* Save char */
 s[end] = '\0';
 res=regexec(regz,&s[start],len,matches,REG_NOTBOL|REG_NOTEOL);
 s[end] = old;/* Restore char */
 regfree(regz);
 free(regz);
 if (res != 0)
 { 
  free(matches);
  return (-1); /* Can't find any occurences */
 }
 start=strlen(s);
 end=0;

 for (i=0;i<len;i++)
 {
  start = (matches[i].rm_so<start) ? matches[i].rm_so : start;
  end = (matches[i].rm_eo>end) ? matches[i].rm_eo : end;
 }
 if (start > end)/* Whole line matches regex */
 {
  end = start;
  start = 0;
 }
 if (matches)
  free(matches);
 *nn=end;
 return start;
}

/*   numbers box (numbers input/edit)     */
int e_num_kst(char *s, int num, int max, FENSTER *f, int n, int sw)
{
 int ret, nz = WpeNumberOfPlaces(max);
 int field_w = nz < 10 ? 10 : nz;
 int label_len = strlen(s) + 1;
 int inner_w = label_len + 2 + field_w + 4;
 int dlg_w = inner_w + 2;
 int xa = f->a.x + (f->e.x - f->a.x - dlg_w) / 2;
 int ya = f->a.y + (f->e.y - f->a.y - 8) / 2;
 char *tmp = MALLOC((strlen(s)+2) * sizeof(char));
 W_OPTSTR *o = e_init_opt_kst(f);

 if (!o || !tmp)
  return(-1);
 if (xa < 0) xa = 0;
 if (ya < 1) ya = 1;
 o->xa = xa;  o->ya = ya;
 o->xe = xa + dlg_w;  o->ye = ya + 8;
 o->bgsw = 0;
 o->name = s;
 o->crsw = AltO;
 sprintf(tmp, "%s:", s);
 e_add_numstr(3, 2, label_len + 3, 2, field_w, max, n, sw, tmp, num, o);
 FREE(tmp);
 { int btn_w = inner_w - 4;
   e_add_bttstr(btn_w / 3 - 1, 5, 1, AltO, " Ok ", NULL, o);
   e_add_bttstr(2 * btn_w / 3 - 1, 5, -1, WPE_ESC, "Cancel", NULL, o);
 }
 ret = e_opt_kst(o);
 if (ret != WPE_ESC)
  num = o->nstr[0]->num;
 freeostr(o);
 return(num);
}

/*   determine string length */
int e_str_len(unsigned char *s)
{
 int i;

 for (i = 0; *(s+i) != '\0' && *(s+i) != WPE_WR; i++)
  ;
 return (i);
}

#if 0
/*   determine number of chars in a string */
int e_str_nrc(unsigned char *s)
{
 int i;

 for (i = 0; *(s+i) != '\0'; i++)
  ;
 return (i);
}

/*   capitalize letters (German) */
int e_toupper(int c)
{
 if (c >= 'a' && c <= 'z')
  c = c - 'a' + 'A';
 else if (c >= 0xe0 && c <= 0xfe)
  c = c - 0x20;
 return (c);
}
#endif

/*           COLOR - fill struct with constants           */
COLOR e_s_x_clr(int f, int b)
{
 COLOR c;

 c.f = f;
 c.b = b;
 c.fb = 16*b + f;
 return(c);
}

COLOR e_n_x_clr(int fb)
{
 COLOR f;

 f.fb = fb;
 f.b = fb / 16;
 f.f = fb % 16;
 return(f);
}

COLOR e_s_t_clr(int f, int b)
{
 COLOR c;

 c.f = f;
 c.b = b;
 c.fb = f;
 return(c);
}

COLOR e_n_t_clr(int fb)
{
 COLOR f;

 f.fb = fb;
 f.b = fb;
 f.f = fb;
 return(f);
}

/*            POINT - fill struct with constants            */
POINT e_set_pnt(int x, int y)
{
 POINT p;

 p.x = x;
 p.y = y;
 return(p);
}

/* What a status line that does not fit the screen starts with: it keeps
   its end, where the part of a long file name the user knows is. */
#define E_STATUS_CUT_MARK "..."
/* Columns the bottom row keeps free: the one it is drawn from, and the
   last one. */
#define E_STATUS_MARGIN 2

/* A status line that stays on the bottom row until the next key, or NULL. */
static char *e_status_held_line;

/**
 * e_status_paint - Write one line of text over the bottom row.
 * @fb: the colours; @text: the line.
 *
 * The bottom row is where the user reads why a key did nothing or what the
 * editor just decided for him; a line too long for the screen keeps its end.
 * The caller refreshes the screen.
 */
void e_status_paint(FARBE *fb, const char *text)
{
 int room = MAXSCOL - E_STATUS_MARGIN, len = (int)strlen(text);
 char *shown;

 if (!fb || room <= (int)strlen(E_STATUS_CUT_MARK))
  return;
 shown = MALLOC(room + 1);
 if (len <= room)
  strcpy(shown, text);
 else
 {
  strcpy(shown, E_STATUS_CUT_MARK);
  strcat(shown, text + len - (room - (int)strlen(E_STATUS_CUT_MARK)));
 }
 e_blk(MAXSCOL, 0, MAXSLNS - 1, fb->mt.fb);
 e_pr_str(1, MAXSLNS - 1, shown, fb->ms.fb, -1, -1, fb->ms.fb, fb->mt.fb);
 FREE(shown);
}

/**
 * e_status_hold - Keep a status line on the bottom row until the next key.
 * @text: the line.
 *
 * For a line the editor says while it goes on working by itself (a debugger
 * chosen for the file's language, then started): the windows that work
 * repaints next would put the key hints back at once and the user would
 * never read it.  Every repaint of the bar paints this line instead until
 * the user presses a key.
 */
void e_status_hold(const char *text)
{
 e_status_drop_held();
 e_status_held_line = WpeStrdup(text);
}

/**
 * e_status_drop_held - Forget the status line kept until the next key.
 *
 * A newer status line takes the row, so an older one must not come back
 * with the next repaint.
 * Return: 1 when a line was being kept.
 */
int e_status_drop_held(void)
{
 if (!e_status_held_line)
  return(0);
 WpeFree(e_status_held_line);
 e_status_held_line = NULL;
 return(1);
}

/**
 * e_status_key_pressed - The user pressed a key: put the key hints back.
 * @f: the window on top.
 *
 * The kept status line has been read once the user acts, so the bottom row
 * names the keys again, as it does after any other status line.
 */
void e_status_key_pressed(FENSTER *f)
{
 if (!e_status_drop_held() || !f || !f->fb)
  return;
 e_pr_uul(f->fb);
 e_refresh();
}

/* Room for "could not watch <what>: too many open streams". */
#define E_FD_REFUSAL_LINE_MAX 160

/**
 * e_status_say_anywhere - Keep a line on the bottom row until the next key,
 * from any part of the editor.
 * @text: the line.
 *
 * For a line said from code that has no window at hand (a stream's
 * callback): it is painted at once when the screen is up, and every repaint
 * of the bar keeps it until the user presses a key.
 */
static void e_status_say_anywhere(const char *text)
{
 e_status_hold(text);
 if (!WpeEditor || !WpeEditor->fb)
  return;
 e_status_paint(WpeEditor->fb, text);
 e_refresh();
}

/**
 * e_fd_watch - Let the editor's loop wake for @fd, or say why it cannot.
 * @fd, @events, @cb, @data: as wpe_fd_add; @what: what the user would miss,
 * in his words ("the AI reply", "the build output").
 *
 * The loop watches a fixed number of streams.  When they are all taken, an
 * AI reply, a build's output, a debugger's answer or a language server's
 * updates would otherwise never arrive and the user would wait for them;
 * instead he reads "could not watch <what>: too many open streams" on the
 * bottom row, and the caller ends what it started (closes the stream, ends
 * the session) so nothing is left waiting.
 * Return: 0 when the fd is watched, -1 when the caller must give it up.
 */
int e_fd_watch(int fd, int events, wpe_fd_callback cb, void *data, const char *what)
{
 char line[E_FD_REFUSAL_LINE_MAX];

 if (wpe_fd_add(fd, events, cb, data) == 0)
  return(0);
 wpe_fd_refusal(what, fd, line, sizeof line);
 e_status_say_anywhere(line);
 return(-1);
}

/**
 * e_fd_key_wait - Sleep in the editor's loop until a key, a stream or a
 * timer wakes it.
 * @keyboard_watched: whether the loop watches the keyboard's descriptor.
 *
 * With the keyboard watched the editor sleeps until something happens; when
 * it could not be watched, the sleep is cut short so a key is still read
 * within a moment and the editor never stops answering the user.
 */
void e_fd_key_wait(int keyboard_watched)
{
 if (keyboard_watched)
  wpe_fd_poll(-1);
 else
  wpe_fd_poll(WPE_FD_POLL_MS);
}

/**
 * e_status_bar_row_sync - Make the next refresh copy the whole bottom row.
 *
 * The incremental Xft refresh copies only the bounding box of cells whose
 * char/attr differ from altschirm[].  When the bar is repainted after a
 * window that overlapped this row closes (e.g. the File-Manager), cells
 * whose glyph happens to be unchanged are skipped, so stale pixels survive
 * in the backbuf.  Breaking the altschirm sync for the whole row makes the
 * next e_refresh re-render and copy row MAXSLNS-1 in full.
 */
static void e_status_bar_row_sync(void)
{
 extern SCREENCELL *altschirm;
 int j;

 for (j = 0; j < MAXSCOL; ++j)
  altschirm[(MAXSLNS - 1) * MAXSCOL + j].ch = -1;
}

int e_pr_uul(FARBE *fb)
{
 extern WOPT *blst;
 extern int nblst;
 extern const char *e_prj_status_label(void);
 int i, rlim;
 const char *prj;

 if (e_status_held_line)
 {
  e_status_paint(fb, e_status_held_line);
  e_status_bar_row_sync();
  return(0);
 }
 /* Reserve the right edge for the project label so the last button never
    draws into it, regardless of bar contents or label length. */
 prj = e_prj_status_label();
 rlim = prj ? MAXSCOL - 3 - (int)strlen(prj) : MAXSCOL - 1;
 e_blk(MAXSCOL, 0, MAXSLNS-1, fb->mt.fb);
 for (i = 0; i < nblst && blst[i].x < MAXSCOL; ++i)
  e_pr_str_scan(blst[i].x+1, MAXSLNS-1, blst[i].t, fb->mt.fb,
			blst[i].s, blst[i].n, fb->ms.fb, blst[i].x,
			i == nblst-1 ? rlim : blst[i+1].x-1);
 if (prj)
  e_pr_str(MAXSCOL - 2 - strlen(prj), MAXSLNS - 1,
           prj, fb->ms.fb, -1, -1, fb->ms.fb, fb->mt.fb);
 e_status_bar_row_sync();
 return(i);
}

