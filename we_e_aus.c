/* we_e_aus.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "edit.h"

extern int e_utf8_charlen(unsigned char c);
extern int e_utf8_to_codepoint(unsigned char *buf, int len);

static int e_utf8_decode_at(unsigned char *s, int pos, int *codepoint)
{
 int len = e_utf8_charlen(s[pos]);

 if (len == 1)
  *codepoint = s[pos];
 else
  *codepoint = e_utf8_to_codepoint(s + pos, len);
 return len;
}

static int e_utf8_prev(unsigned char *s, int pos)
{
 if (pos <= 0) return 0;
 pos--;
 while (pos > 0 && (s[pos] & 0xC0) == 0x80)
  pos--;
 return pos;
}

static int e_utf8_next(unsigned char *s, int pos, int len)
{
 if (pos >= len) return len;
 return pos + e_utf8_charlen(s[pos]);
}

static int e_utf8_visual_width(unsigned char *s, int from, int to)
{
 int w = 0, pos = from;

 while (pos < to && s[pos])
 {
  if ((unsigned char)s[pos] < ' ')
   w += 2;
  else
   w++;
  pos += e_utf8_charlen(s[pos]);
 }
 return w;
}

static int e_dialog_encode_char(int c, unsigned char *out)
{
 extern int e_codepoint_to_utf8(int cp, unsigned char *out);

 if (c < 0x80)
 {
  out[0] = c;
  return 1;
 }
 return e_codepoint_to_utf8(c, out);
}

/*
   draw entire screen with uniform chars and color */
void e_cls(int frb, int chr)
{
 int i, j;

 for (j = 0; j < MAXSLNS; j++)
  for (i = 0; i < MAXSCOL; i++)
   e_pr_char(i, j, chr, frb);
}

/*
   write text string */
int e_puts(char *s, int xa, int ya, int frb)
{
 int i;

 if (xa >= MAXSCOL || ya > MAXSLNS)
  return(-1);
 for (i = 0; s[i] != '\0' && i < 2000; i++)
  e_pr_char(i + xa, ya, s[i], frb);
 return(0);
}

/* write text string (sic!, R.H.) 
   - characters between b2 and n2 will be drawn with color 'col2'
     other characters in the string with color 'col' 
   - one character before the text is space with color 'col'
   - one character after the text is space with color 'col'
   - color 'col3' is used in old style
*/
void e_pr_str(int x, int y, char *str, int col, int b2, int n2, int col2,
  int col3)
{
 int i, sw = 0;

 if (n2 < 0)
 {
  sw = 1; n2 = -n2;
 }
 e_pr_char(x-1, y, 32, col);
 for (i=0; *(str+i) != '\0'; i++)
 {
  if (i>=b2 && i<b2+n2)
   e_pr_char(x+i, y, *(str+i), col2);
  else
   e_pr_char(x+i, y, *(str+i), col);
 }
 e_pr_char(x+i, y, 32, col);
 if (sw == 1 && WpeIsXwin())
#ifdef NEWSTYLE
  e_make_xrect(x-1, y, x+i, y, 0);
#else
 {
  i++;
  e_pr_char(x+i, y, SCR, col3);
  for (; i >= 0; i--)
   e_pr_char(x+i+MAXSCOL, y, SCD, col3);
 }
#endif
}

/* - characters between b2 and n2 will be drawn with color 'col2',
     other characters in the string with color 'col' 
   - spaces between bg and x are drawn with 'col'
   - spaces after text and before nd are drawnd with 'col'
*/
int e_pr_str_wsd(int x, int y, char *str, int col, int b2, int n2, int col2,
  int bg, int nd)
{
 int i;

 for (i = bg; i < x; i++)
  e_pr_char(i, y, ' ', col);
 for (i = 0; str[i] && i <= nd-x; i++)
 {
  if (i>=b2 && i<b2+n2)
   e_pr_char(i+x, y, *(str+i), col2);
  else
   e_pr_char(i+x, y, *(str+i), col);
 }
 for (i+=x; i <= nd; i++)
  e_pr_char(i, y, ' ', col);
#ifdef NEWSTYLE
 if (WpeIsXwin())
  e_make_xrect(bg, y, nd, y, 0);
#endif
 return(0);
}

int e_pr_str_scan(int x, int y, char *str, int col, int b2, int n2, int col2,
  int bg, int nd)
{
 char txt[30], *pt;

 if (WpeIsXwin())
  return(e_pr_str_wsd(x, y, str, col, b2, n2, col2, bg, nd));
 strcpy(txt, str);
 if ((pt = strstr(txt, "Alt")) != NULL)
 {
  pt[0] = 'E'; pt[1] = 'S'; pt[2] = 'C';
 }
 else if (!strncmp(txt, "^F4 Close", 9))
  strcpy(txt, "ESC X Cl.");
 return(e_pr_str_wsd(x, y, txt, col, b2, n2, col2, bg, nd));
}

/*
   write string of numbers */
int e_pr_zstring(char *s, int x, int y, int n, int fb)
{
 int i, len = strlen(s);

 for (i = 0; i < n - len; i++)
  e_pr_char(x+i, y, ' ', fb);
 for(; i < n; i++)
  e_pr_char(x+i, y, s[i-n+len], fb);
#ifdef NEWSTYLE
 if (WpeIsXwin())
  e_make_xrect(x, y, x+n-1, y, 1);
#endif
 return(len);
}

/*
   Write N chars to screen */
int e_schr_nchar(char *s, int x, int y, int n, int max, int frb)
{
 int col, pos, cp, clen;
 unsigned char *us = (unsigned char *)s;

 e_pr_char(x, y, ' ', frb);
 for (col = 1, pos = n; col < max-1 && us[pos] != '\0'; )
 {
  if (us[pos] < ' ')
  {
   e_pr_char(x+col, y, '^', frb);
   col++;
   if (col < max-1)
    e_pr_char(x+col, y, us[pos] + '@', frb);
   col++;
   pos++;
  }
  else
  {
   clen = e_utf8_decode_at(us, pos, &cp);
   e_pr_char(x+col, y, cp, frb);
   col++;
   pos += clen;
  }
 }
 for (; col < max; col++)
  e_pr_char(x+col, y, ' ', frb);
#ifdef NEWSTYLE
 e_make_xrect(x, y, x+max-1, y, 1);
#endif
 return(col);
}

/*
	  Writing of a text string       */
void e_pr_nstr(int x, int y, int n, char *str, int col, int col2)
{
 int i;

 e_pr_char(x-1, y, ' ', col);
 for (i=0; *(str+i) != '\0' && i < n-1; i++)
  e_pr_char(x+i, y, *(str+i), col);
 if (i < n-1)
  e_pr_char(x+i, y, ' ', col);
 for (i++; i < n-1; i++)
  e_pr_char(x+i, y, ' ', col2);
}

/*
   Enter digits */
int e_schreib_zif(int *num, int x, int y, int max, int ft, int fs)
{
#if  MOUSE
 extern struct mouse e_mouse;
#endif
 int c, i, jc = max-1, ntmp = *num, nnum = WpeNumberOfPlaces(ntmp);
 int first = 1;
 char *s = MALLOC((max+1)*sizeof(char));

 e_pr_zstring(WpeNumberToString(ntmp, max, s), x, y, max, fs);
 fk_locate(x+jc, y);
 fk_cursor(1);
 while ((c = e_getch()) != WPE_ESC)
 {
  if (c == WPE_RESIZE)
  {
   *num = WpeStringToNumber(s);
   FREE(s);
   return WPE_RESIZE;
  }
#if  MOUSE
  if (c < 0)
  {
   if (e_mouse.y == y && e_mouse.x >= x && e_mouse.x < x+max)
   {
    jc = e_mouse.x-x > max-nnum ? e_mouse.x-x : max-nnum;
   }
   else
   {
    if (c > -2)
     *num = WpeStringToNumber(s);
    FREE(s);
    return(c);
   }
  }
#endif
  if (c == 332) {  if(jc < max-1) jc++;  }
  else if (c == 330) {  if(jc > max-nnum) jc--;  }
  else if (c == 326) jc = max-nnum;
  else if (c == 334) jc = max-1;
  else if (c == WPE_DC)
  {
   if (jc > max-nnum)
   {
    for (i = jc-1; i > 0; i--)
     s[i] = s[i-1];
    s[0] = 32;
    nnum--;
   }
  }
  else if (c == ENTF)
  {
   if (nnum == 1)
   {
    s[jc] = '0';
   }
   else if (jc < max)
   {
    for (i = jc; i > 0; i--)
     s[i] = s[i-1];
    s[0] = 32;
    nnum--;
   }
  }
  else if (first == 1 && c >='0' && c <= '9')
  {
   for (i = 0; i < max - 1; i++)
    s[i] = 32;
   s[i] = c;
   s[max] = '\0';
   nnum = 1;
  }
  else if (c >='0' && c <= '9')
  {
   for (i = 0; i < jc; i++)
    s[i] = s[i+1];
   s[jc] = c;
   if (nnum < max)
    nnum++;
  }
  else if (c > 0 && (c < 32 || c > 255))
  {
   if (c != WPE_ESC)
    *num = WpeStringToNumber(s);
   FREE(s);
   return(c);
  }
  if (jc > max -1)
   jc = max-1;
  else if (jc < max - nnum) jc = max-nnum;

  e_pr_zstring(s, x, y, max, ft);
  fk_locate(x+jc, y);
  first = 0;
 }
 return(c);
}

/*
   write chars in text line */
int e_schreib_leiste(char *s, int x, int y, int n, int max, int ft, int fs)
{
#if  MOUSE
 extern struct mouse e_mouse;
#endif
 int c, i, ja = 0, jc, l = strlen(s);
 int sond = 0, first = 1;
 unsigned char *tmp = MALLOC(max+1);
 int vcur;

 fk_cursor(1);
 strcpy(tmp, s);
 jc = l;
 vcur = e_utf8_visual_width(tmp, 0, jc);
 if (vcur > n-3) jc = 0;
 e_schr_nchar(tmp, x, y, 0, n, fs);
 e_pr_char(x, y, ' ', ft);
 e_pr_char(x+n-1, y, ' ', ft);
 vcur = e_utf8_visual_width(tmp, ja, jc);
 fk_locate(x+vcur+1, y);
#ifdef NEWSTYLE
 e_make_xrect(x, y, x+n-1, y, 1);
#endif
 while ((c = e_getch()) != WPE_ESC)
 {
  if (c == WPE_RESIZE)
  {
   strcpy(s, tmp);
   FREE(tmp);
   fk_cursor(0);
   return WPE_RESIZE;
  }
#if  MOUSE
  if (c < 0)
  {
   if (e_mouse.y == y && e_mouse.x > x && e_mouse.x < x+n)
   {
    jc = e_mouse.x-x-1 < l ? e_mouse.x-x-1 : l;
    if (c == -2)
    {
     int len = WpeEditor->f[0]->b->bf[0].len;
#ifndef NO_XWINDOWS
     if (bioskey() & 8)
     e_cp_X_to_buffer(WpeEditor->f[WpeEditor->mxedt]);
#endif
     while (e_mshit())
      ;
     if (first == 1)
     {
      tmp[0] = '\0';
      l = 0;
      jc = 0;
     }
     if (len + l > max) len = max - l;
     for (i = l; i >= jc; i--)
      tmp[i+len] = tmp[i];
     for (i = jc; i < jc + len; i++)
      tmp[i] = WpeEditor->f[0]->b->bf[0].s[i-jc];
     jc += len;
     l += len;
    }
   }
   else
   {
    if (c > -4) strcpy(s, tmp);
    FREE(tmp);
    fk_cursor(0);
    return(c);
   }
  }
#endif
  if (c == CRI || (!sond && c == CtrlF)) {  if(jc < l) jc = e_utf8_next(tmp, jc, l);  }
  else if (c == CLE || (!sond && c == CtrlB)) {  if(jc > 0) jc = e_utf8_prev(tmp, jc);  }
  else if (c == CCLE && jc > 0) jc = e_su_rblk(jc, tmp);
  else if (c == CCRI && jc < l) jc = e_su_lblk(jc, tmp);
  else if (c == POS1 || (!sond && c == CtrlA)) jc = 0;
  else if (c == ENDE || (!sond && c == CtrlE)) jc = l;
  else if (c == AltJ || c == F9) sond = !sond;
  else if (c == PASTE || c == ShiftEin || c == CtrlV || c == AltEin)
  {
   int len = WpeEditor->f[0]->b->bf[0].len;
#ifndef NO_XWINDOWS
   if (WpeIsXwin() && c == AltEin)
    e_cp_X_to_buffer(WpeEditor->f[WpeEditor->mxedt]);
#endif
   if (first == 1)
   {
    tmp[0] = '\0';
    l = 0;
    jc = 0;
   }
   if (len + l > max) len = max - l;
   for (i = l; i >= jc; i--)
    tmp[i+len] = tmp[i];
   for (i = jc; i < jc + len; i++)
    tmp[i] = WpeEditor->f[0]->b->bf[0].s[i-jc];
   jc += len;
   l += len;
  }
  else if (c == WPE_DC && !sond)
  {
   if (jc > 0)
   {
    int prev = e_utf8_prev(tmp, jc);
    int clen = jc - prev;
    for (i = prev; i <= l - clen; i++)
     tmp[i] = tmp[i+clen];
    jc = prev;
    l -= clen;
   }
  }
  else if (c == ENTF || (!sond && c == CtrlD))
  {
   if (jc < l)
   {
    int clen = e_utf8_charlen(tmp[jc]);
    for (i = jc; i <= l - clen; i++)
     tmp[i] = tmp[i+clen];
    l -= clen;
   }
  }
  else if (!sond && c == CtrlT)
  {
   c = e_su_lblk(jc, tmp) - jc;
   {
    for (i = jc; i <= l-c; i++)
     tmp[i] = tmp[i+c];
    l -= c;
   }
  }
  else if (!sond && c == CtrlO)
  {
   if ((c = e_getch()) != CtrlT && toupper(c) != 'T') continue;
   c = jc - e_su_rblk(jc, tmp);
   {
    for( i = jc-c; i <= l-c; i++)
    tmp[i] = tmp[i+c];
    jc -= c;
    l -= c;
   }
  }
  else if (first == 1 &&
    ((c != WPE_CR && c != CtrlP && c != CtrlN && c != WPE_TAB &&
    c != WPE_BTAB) || sond) && c > 0 && c < 0xff)
  {
   int u8len = e_dialog_encode_char(c, tmp);
   tmp[u8len] = '\0';
   l = u8len;
   jc = u8len;
  }
  else if (((c != WPE_CR && c != WPE_TAB && c != WPE_BTAB && c != CtrlP &&
    c != CtrlN ) || sond) && c > 0 && c < 0xff)
  {
   unsigned char u8[4];
   int u8len = e_dialog_encode_char(c, u8);
   if (l + u8len <= max)
   {
    for (i = l; i >= jc; i--) tmp[i+u8len] = tmp[i];
    for (i = 0; i < u8len; i++) tmp[jc+i] = u8[i];
    l += u8len; jc += u8len;
   }
  }
  else if (c > 0)	
  { 
   if (c != WPE_ESC) strcpy(s, tmp);
   FREE(tmp);fk_cursor(0);
   if (c == CtrlP) c = CUP;
   else if (c == CtrlN) c = CDO;
   return(c);
  }
  { int vcur = e_utf8_visual_width(tmp, ja, jc);
    int vtot = e_utf8_visual_width(tmp, ja, l);
    if (vcur > n-3) ja = jc;
    else if (jc < ja) ja = jc;
    vcur = e_utf8_visual_width(tmp, ja, jc);

    e_schr_nchar(tmp, x, y, ja, n, ft);

    e_pr_char(x, y, ja > 0 ? MCL : ' ', ft);
    e_pr_char(x+n-1, y, vtot > n-2 ? MCR : ' ', ft);
    fk_locate(x+vcur+1, y);
  }
  first = 0;
#ifdef NEWSTYLE
  e_make_xrect(x, y, x+n-1, y, 1);
#endif
 }
 fk_cursor(0);
 return(c);
}

int e_schr_nzif(int num, int x, int y, int max, int col)
{
 char *str = MALLOC((max+1)*sizeof(char));
 int i, nt;

 for (i = 0, nt = 1; i < max; i++) nt *= 10;
 if (num >= nt)
  num = nt - 1;
 e_pr_zstring(WpeNumberToString(num, max, str), x, y, max, col);
 FREE(str);
 return(0);
}

