/* we_fl_fkt.c                                            */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include "makro.h"
#include "we_buffer.h"
#include "we_lineend.h"
#include "we_rowfind.h"
#include "we_ai.h"
#include "we_child.h"
#include "we_startplan.h"
#include "we_fdloop.h"
#include "we_helpbuf.h"
#include <ftw.h>
#ifdef UNIX
#include <sys/types.h>
#include <sys/stat.h>
#endif
#ifdef HAS_LIBZ
#include <zlib.h>
#endif
#ifdef DEFPGC
#define putc(c, fp) fputc(c, fp)
#endif

int e_read_help();
int e_read_info();

char *info_file = NULL;
char *e_tmp_dir = NULL;

/* create complete file name: the folder, one separator unless it ends in one,
   the name, on the heap at their real length (the path owner's join) */
char *e_mkfilename(char *dr, char *fn)
{
 return(wpe_path_join_dup(dr, fn));
}

/**
 * e_read_file_into - Read a file into a buffer at a place, as one action a
 * test can starve of memory (the "read" fault point), with the busy mouse
 * shape while it lasts.
 * Arguments and return as e_eol_read_file_into, which owns the reading.
 */
int e_read_file_into(FILE *fp, BUFFER *b, int x, int y, int manpage, WpeEolInfo *info,
                     POINT *end)
{
 int r, fault;

 WpeMouseChangeShape(WpeWorkingShape);
 fault = e_buffer_test_fault_enter(E_BUFFER_FAULT_READ);
 r = e_eol_read_file_into(fp, b, x, y, manpage, info, end);
 e_buffer_test_fault_leave(fault);
 WpeMouseRestoreShape();
 return(r);
}

/*   Open new edit window   */
int e_new(FENSTER *f)
{
 e_edit(f->ed, "");
 return(0);
}

int e_m_save(FENSTER *f)
{
 int ret;

#ifdef WPE_AI
 /* The AI pane backs no file: F2 and File > Save write its transcript. */
 if (wpe_ai_is_pane(f))
  return(wpe_ai_pane_save(f));
#endif
 ret = e_save(f);

 if (ret == WPE_ESC)
  e_message(0, e_msg[ERR_MSG_SAVE], f);
 return(ret);
}

/**
 * e_write_keeps_file_ending - Does a write of this range leave out the
 * final line break the file did not have?
 * @f: the window written; @b: its buffer; the range as e_write takes it.
 *
 * Only when the option "Keep Final newline" is on and the write is the
 * whole text: a block written to a file, or the part of a file below a
 * mark, is not the file's own end.  So a file read without a final line
 * break is saved without one, and so is a new or an empty file, and
 * `git diff` shows no change to a last line the user did not touch.
 * Return: 1 to leave the final break out when the buffer recorded none.
 */
static int e_write_keeps_file_ending(FENSTER *f, BUFFER *b, int xa, int ya, int xe, int ye)
{
 int whole = (xa == 0 && ya == 0 && xe >= E_BUFFER_ROW_END(b) && ye >= b->mxlines - 1);

 return(whole && (f->ed->edopt & ED_KEEP_FILE_ENDING) != 0);
}

/**
 * e_save_final_break_after - What the buffer records of its file's final
 * line break once the text was saved.
 * @f: the window saved; @b: its buffer.
 *
 * A save that wrote the break leaves the file ending on one.  A save that
 * kept the file's ending leaves the record as it was: a file that had no
 * final break still has none, and turning the option off later makes the
 * next save add it.
 */
static void e_save_final_break_after(FENSTER *f, BUFFER *b)
{
 if (f->ed->edopt & ED_KEEP_FILE_ENDING)
  return;
 b->final_break = E_FINAL_BREAK_HAD;
}

/**
 * e_save_nul_confirmed - Before the first Save of a file that lost NUL bytes
 * when it was read, ask whether to write it without them.
 * @f: the window; @b: its buffer.
 *
 * A row cannot hold a NUL byte, so the file on disk is longer than what the
 * window holds.  Saving over it without a word would shorten a binary file
 * silently; answering No (or Cancel) leaves the disk as it is.
 * Return: 1 when the Save may go on (nothing was lost, or the user said Yes).
 */
static int e_save_nul_confirmed(FENSTER *f, BUFFER *b)
{
 char sentence[E_NUL_SAY_MAX], question[E_NUL_ASK_MAX];

 if (b->nul_dropped <= 0)
  return(1);
 e_eol_nul_sentence(b->nul_dropped, sentence, sizeof sentence);
 snprintf(question, sizeof question, "%s.\nSaving writes the file without them.\nSave anyway?",
          sentence);
 return(e_message(1, question, f) == 'Y');
}

int e_save(FENSTER *f)
{
 BUFFER *b;
 int ret;

 if (!DTMD_ISTEXT(f->dtmd))
  return(0);
 for (ret = f->ed->mxedt; ret > 0 && f->ed->f[ret] != f; ret--)
  ;
 if (!ret)
  return(0);
 b = f->ed->f[ret]->b;
 if (!e_save_nul_confirmed(f, b))
  return(E_SAVE_DECLINED);
 WpeMouseChangeShape(WpeWorkingShape);
 ret = e_write(0, 0, E_BUFFER_ROW_END(b), b->mxlines-1, f, WPE_BACKUP);
 if (ret != WPE_ESC)
 {
  f->save = 0;
  e_save_final_break_after(f, b);
  b->nul_dropped = 0;                   /* the user knew, and the file is written */
 }
 WpeMouseRestoreShape();
 return(ret);
}

/*	save all windows */
int e_saveall(FENSTER *f)
{
 int i, ret = 0;
 ECNT *cn = f->ed;

 for (i = cn->mxedt; i > 0; i--)
 {
  if (cn->f[i]->save != 0 && cn->f[i]->ins != 8)
   if (e_save(cn->f[i]) == WPE_ESC)
    ret = WPE_ESC;
 }
 if (ret == WPE_ESC)
  e_message(0, e_msg[ERR_MSG_SAVEALL], f);
 return(ret);
}

/** The most directories nftw holds open while it walks the temp folder. */
#define E_TMP_WALK_FDS 16

/** e_tmp_remove_entry - nftw's callback: remove one entry, links as links. */
static int e_tmp_remove_entry(const char *path, const struct stat *sb,
                              int type, struct FTW *ftw)
{
 (void)sb;
 (void)type;
 (void)ftw;
 remove(path);
 return(0);
}

/**
 * e_tmp_remove - Remove the editor's temp folder and what is in it, when
 * the editor ends.
 * @dir: the folder the editor made at start-up.
 *
 * Needs no shell, and the editor's exit stays as fast as the folder is
 * small.  A link inside it is removed as a link: nothing it points at,
 * inside or outside the folder, is touched.
 */
static void e_tmp_remove(const char *dir)
{
 if (!dir || !dir[0])
  return;
 nftw(dir, e_tmp_remove_entry, E_TMP_WALK_FDS, FTW_DEPTH | FTW_PHYS);
}

/*	terminate edit session */
int e_quit(FENSTER *f)
{
 int i;
#if  MOUSE
 int g[4];
#endif
 ECNT *cn = f->ed;
#ifdef DEBUGGER
 extern int rfildes[], e_d_swtch;

 e_d_quit_basic(f);
#endif
 for (i = cn->mxedt; i > 0; i--)
 {
  if (e_close_window(cn->f[cn->mxedt]) == WPE_ESC)
   return(WPE_ESC);
 }
 if (cn->autosv & 1)
  e_save_opt(cn->f[0]);
#ifdef UNIX
 if (WpeQuitWastebasket(cn->f[0]))
  return(0);
 e_tmp_remove(e_tmp_dir);
#endif
 wpe_rowfind_release();
#if  MOUSE
 g[0] = 2;
 fk_mouse(g);
#endif
 e_cls(cn->fb->ws, ' ');
 fk_locate(0, 0);
 fk_cursor(1);
 e_refresh();
 WpeExit(0);
 return(0);
}

/*	write file to disk */
int e_write(int xa, int ya, int xe, int ye, FENSTER *f, int backup)
{
 BUFFER *b;
 int j;
 char *tmp, *ptmp;
 FILE *fp;

 for (j = f->ed->mxedt; j > 0 && f->ed->f[j] != f; j--)
  ;
 b = f->ed->f[j]->b;
 if (f->ins == 8)
  return(WPE_ESC);
 ptmp = e_mkfilename(f->dirct, f->datnam);
 if ((backup == WPE_BACKUP) && (access(ptmp, 0) == 0))
 {
  tmp = e_bakfilename(ptmp);
  if (access(tmp, 0) == 0)
   remove(tmp);
  WpeRenameCopy(ptmp, tmp, f, 1);
  WpeFree(tmp);
 }
 if ((fp = fopen(ptmp, "wb")) == NULL)
 {
  e_error(e_msg[ERR_FOPEN], 0, f->fb);
  FREE(ptmp);
  return(WPE_ESC);
 }
 if (f->filemode >= 0)
  chmod(ptmp, f->filemode);
 e_eol_write(fp, b, ya, xa, ye, xe, e_write_keeps_file_ending(f, b, xa, ya, xe, ye));
 if (ferror(fp))
 {
  e_error("Error writing file (disk full?)", 0, f->fb);
  fclose(fp);
  FREE(ptmp);
  return(WPE_ESC);
 }
 if (fclose(fp) != 0)
 {
  e_error("Error writing file (disk full?)", 0, f->fb);
  FREE(ptmp);
  return(WPE_ESC);
 }
 FREE(ptmp);
 return(0);
}

/*	append new qualifier (suffix) to file name */
char *e_new_qual(char *s, char *ns, char *sb)
{
 int i, j;

 for (i = strlen(s); i >= 0 && s[i] != '.' && s[i] != DIRC; i--)
  ;
 if (i < 0 || s[i] == DIRC)
  strcpy(sb, s);
#if !defined(DJGPP)
 else if (i == 0 || s[i-1] == DIRC)
  strcpy(sb, s);
#endif
 else
 {
  for (j = 0; j < i; j++)
   sb[j] = s[j];
  sb[j] = '\0';
 }
 strcat(sb, ".");
 strcat(sb, ns);
 return(sb);
}

/*     make up the bak-file name */
char *e_bakfilename(char* s)
{
 /* "" is the special code for TurboC replace-extension style */
#ifndef SIMPLE_BACKUP_SUFFIX
#define SIMPLE_BACKUP_SUFFIX ""
#endif

 static char* bak = NULL;
 char *result;
 if (!bak)
 {
  bak = getenv("SIMPLE_BACKUP_SUFFIX");
  if (!bak) bak = SIMPLE_BACKUP_SUFFIX;
 }

 result = WpeMalloc(strlen(s) + strlen(bak) + 5);
 if (!*bak)
  return e_new_qual(s, "bak", result); /* TurboC style */
 else
 {
  strcpy(result, s);
  strcat(result, bak);
  return result;
 }
}

/*    Clear file-/directory-structure   */
int freedf(struct dirfile *df)
{
 if (df == NULL) return(-1);
 if(df->name)
 {
  for (; df->anz > 0; (df->anz)--)
   if (*(df->name+df->anz-1))
    FREE(*(df->name+df->anz-1));
   FREE(df->name);
 }
 FREE(df);
 df = NULL;
 return(0);
}



/**
 * e_fl_sel_len - Width, in characters, of the file panel's selected name.
 * @fw: file-manager list window whose highlighted entry (fw->nf) is measured.
 *
 * Returns strlen() of the currently selected directory entry, or 0 when the
 * panel holds no entries (anz == 0) or the selection index has drifted out of
 * range. The file manager calls this to size the horizontal scrollbar and to
 * bound left/right scrolling of a long filename. It must stay safe on an empty
 * directory: there the name array has no valid element, so measuring
 * name[nf] would read past its end -- clicking the scrollbar of an empty panel
 * used to do exactly that.
 *
 * Return: length of the selected name in characters, 0 when nothing is selected.
 */
int e_fl_sel_len(FLWND *fw)
{
   if(fw->df->anz <= 0) return(0);
   if(fw->nf < 0 || fw->nf >= fw->df->anz) return(0);
   return(strlen(*(fw->df->name + fw->nf)));
}

/**
 * WPE_TYPEAHEAD_MAX - Size of the type-ahead buffer of a file panel.
 *
 * Typing in the Files or DirTree panel jumps to the first name that starts
 * with the typed text.  The buffer is as large as a whole path, so any name
 * the panel can show, however long, can be typed in full and found.  Keys
 * typed after the buffer is full are ignored, never written past its end.
 */
#define WPE_TYPEAHEAD_MAX WPE_PATH_MAX

int e_file_window(int sw, FLWND *fw, int ft, int fz)
{
   int i, c, nsu = 0;
   int len = 0;
   char sustr[WPE_TYPEAHEAD_MAX];
   if(fw->df->anz > 0)
   {  if(fw->nf >= fw->df->anz) fw->nf = fw->df->anz-1;
      len = e_fl_sel_len(fw);
      /* Ensure the selected item is visible when entering the panel.
         Without this, Tab into a panel can land on an off-screen item,
         making it look like the panel was skipped. */
      if(fw->nf - fw->ia >= fw->ye - fw->ya)
        fw->ia = fw->nf + fw->ya - fw->ye + 1;
      else if(fw->nf - fw->ia < 0)
        fw->ia = fw->nf;
   }
   e_mouse_bar(fw->xe, fw->ya, fw->ye-fw->ya, 0, fw->f->fb->em.fb);
   e_mouse_bar(fw->xa, fw->ye, fw->xe-fw->xa, 1, fw->f->fb->em.fb);
   while (1)
   {  e_pr_file_window(fw, 1, 1, ft, fz, 0);
#if  MOUSE
      if((c = wpe_paint_modal_getch()) < 0) c = fl_wnd_mouse(sw, c, fw);
#else
      c = wpe_paint_modal_getch();
#endif
      /* Empty list: there is nothing to navigate, but still hand the key
         back so commands like Add (AltA) work on a new, empty project. */
      if(fw->df->anz <= 0) return(c ? c : WPE_ESC);
      if (c == CUP || c == CtrlP)
      {  if(fw->nf <= 0)  return(c);
	 else  fw->nf--;
	 if(fw->srcha < 0) nsu = 0;
      }
      else if ((c == CDO || c == CtrlN) && fw->nf < fw->df->anz-1)
      {  fw->nf++;
	 if(fw->srcha < 0) nsu = 0;
      }
      else if (c == CLE || c == CtrlB)
      {  if(fw->ja <= 0)  return(c);
	 else  fw->ja--;
      }
      else if (c == CRI || c == CtrlF)
      {  if(fw->ja > len-2)  return(c);
	 else  fw->ja++;
      }
      else if (c == BUP)
      {  if(fw->nf <= 0)  return(c);
	 if((fw->nf -= (fw->ye-fw->ya-1)) < 0) fw->nf = 0;
	 if(fw->srcha < 0) nsu = 0;
      }
      else if (c == BDO)
      {  if((fw->nf += (fw->ye-fw->ya-1)) > fw->df->anz-1)
	 fw->nf = fw->df->anz-1;
	 if(fw->srcha < 0) nsu = 0;
      }
      else if (c == POS1 || c == CtrlA || c == CBUP)
      {  fw->nf = 0; nsu = 0;  fw->ja = fw->srcha > 0 ? fw->srcha : 0;  }
      else if (c == ENDE || c == CtrlE || c == CBDO)
      fw->nf = fw->df->anz-1;
      else if (c >= 32 && c < 127 && nsu >= WPE_TYPEAHEAD_MAX - 1)
      {  /* buffer full: keep what is typed, ignore the key */  }
      else if (c >= 32 && c < 127)
      {  sustr[nsu] = c;  sustr[++nsu] = '\0';
	 if(fw->srcha < 0)
	 {  for(i = nsu > 1 ? fw->nf : fw->nf+1; i < fw->df->anz; i++)
	    {  for(c = 0; *(fw->df->name[i]+c)
			&& ( *(fw->df->name[i]+c) <= 32
			  || *(fw->df->name[i]+c) >= 127); c++);
#ifdef UNIX
	       if(!WpeIsXwin() && *(fw->df->name[i]+c-1) == 32) c += 3;
#endif
	       if(strncmp(fw->df->name[i]+c, sustr, nsu) >= 0) break;
	    }
	 }
	 else
	 {  for(i = 0; i < fw->df->anz
		  && strncmp(fw->df->name[i]+fw->srcha, sustr, nsu) < 0; i++);
	 }
	 fw->nf = i < fw->df->anz ? i : fw->df->anz-1;
      }
      else if (c == CCLE) return(c);
      else if (c == CCRI) return(c);
      else if(c) {  nsu = 0;  return(c);  }
      else if(fw->srcha < 0) nsu = 0;
      if(fw->nf > fw->df->anz-1) fw->nf = fw->df->anz-1;
      len = e_fl_sel_len(fw);
      if(fw->ja >= len) fw->ja = !len ? len : len-1;
      if(fw->nf-fw->ia >= fw->ye - fw->ya) fw->ia = fw->nf+fw->ya-fw->ye+1;
      else if(fw->nf-fw->ia < 0) fw->ia = fw->nf;
   }
   return(0);
}

int e_pr_file_window(FLWND *fw, int c, int sw, int ft, int fz, int fs)
{
#ifdef NEWSTYLE
 int xrt = 0;
#endif
 int i = fw->ia, len;

 if (fw->df != NULL)
 {
  for (; i < fw->df->anz && i-fw->ia < fw->ye-fw->ya; i++)
  {
   if ((len=strlen(*(fw->df->name+i))) < 0)
    len = 0;
   if (i == fw->nf && c && len >= fw->ja)
   {
    e_pr_nstr(fw->xa+1, fw->ya+i-fw->ia, fw->xe-fw->xa, *(fw->df->name+i)+fw->ja, fz, fz);
#ifdef NEWSTYLE
    xrt = 1;
#endif
   }
   else if (i == fw->nf && len >= fw->ja)
    e_pr_nstr(fw->xa+1, fw->ya+i-fw->ia, fw->xe-fw->xa, *(fw->df->name+i)+fw->ja, fs, fs);
   else if (len >= fw->ja)
    e_pr_nstr(fw->xa+1, fw->ya+i-fw->ia, fw->xe-fw->xa, *(fw->df->name+i)+fw->ja, ft, ft);
   else if (len < fw->ja)
    e_blk(fw->xe-fw->xa-1, fw->xa+1, fw->ya+i-fw->ia, ft);
/*
   if ((len -= fw->ja) < 0) len = 0;
   e_blk(fw->xe-fw->xa-len-1, fw->xa+len+1, fw->ya+i-fw->ia, ft);
*/
   if (sw)
   {
    fw->nyfo = e_lst_zeichen(fw->xe, fw->ya, fw->ye-fw->ya, 0,
      fw->f->fb->em.fb, fw->df->anz, fw->nyfo, fw->nf);
    fw->nxfo = e_lst_zeichen(fw->xa, fw->ye, fw->xe-fw->xa, 1,
      fw->f->fb->em.fb, len, fw->nxfo, fw->ja);
   }
  }
 }
 for (; i-fw->ia < fw->ye-fw->ya; i++)
  e_blk(fw->xe-fw->xa, fw->xa, fw->ya+i-fw->ia, ft);
#ifdef NEWSTYLE
 e_make_xrect(fw->xa, fw->ya, fw->xe-1, fw->ye-1, 1);
 if (xrt)
  e_make_xrect_abs(fw->xa, fw->ya+fw->nf-fw->ia, fw->xe-1, fw->ya+fw->nf-fw->ia, 0);
#endif
 return(0);
}


struct help_ud
{
 struct help_ud *next;
 char *str, *nstr, *pstr, *file;
 int x, y, sw;
} *ud_help;

#ifdef HAS_LIBZ
typedef gzFile IFILE;

#define e_i_fgets(s, n, p) gzgets(p, s, n)
#define e_i_fclose(p) gzclose(p)
#else
typedef struct
{
 FILE *fp;
 int sw;
} IFILE;

#define e_i_fgets(s, n, p) fgets(s, n, p->fp)
#endif

int e_mkdir_path(char *path)
{
 int i, len;
 char *tmp = MALLOC(((len=strlen(path))+1)*sizeof(char));

 if (!tmp)
  return(-1);
 strcpy(tmp, path);
 for (i = len; i > 0 && tmp[i] != DIRC; i--)
  ;
 if (i > 0)
 {
  tmp[i] = '\0';
  if (access(tmp, 0))
  {
   e_mkdir_path(tmp);
   mkdir(tmp, 0700);
  }
 }
 FREE(tmp);
 return(0);
}

#ifndef HAS_LIBZ
/* The longest an unpack may take: a help or info file unpacks in well under
   a second, and one that does not end (a damaged or endless input) must not
   hold the editor, whose keys wait meanwhile. */
#define E_UNPACK_MS   30000L
#define E_UNPACK_WHAT "gunzip"
static int e_unpack_was_stopped;

/**
 * e_unpack_to_file - Unpack a compressed file into @dest, for the help and
 * info readers.
 * @src: the compressed file; @dest: where its text goes.
 *
 * The names are arguments of the program, whatever characters they hold, so
 * a file named "a b;c.gz" opens like any other.  A failed, cut-short or
 * endless unpack leaves no file behind: the reader would otherwise take the
 * part that was written for the whole text, now and on every later open.
 * Esc ends the wait: only the part written into @dest is removed, never
 * @src, and e_unpack_stopped then tells the readers not to try another file.
 * Return: 1 when @dest holds the whole text, 0 otherwise.
 */
static int e_unpack_to_file(const char *src, const char *dest)
{
 char *argv[] = { "gunzip", "-c", "--", NULL, NULL };
 WpeStartPlan *plan;
 int status = 0, ran;

 argv[3] = (char *)src;
 plan = wpe_startplan_fixed(WPE_START_UNPACK, E_UNPACK_WHAT, argv);
 ran = wpe_startplan_run_to_file(plan, dest, E_UNPACK_MS, &status, E_UNPACK_WHAT);
 wpe_startplan_free(plan);
 e_unpack_was_stopped = (ran == WPE_CAPTURE_CANCELLED);
 if (wpe_capture_succeeded(ran, status))
  return(1);
 remove(dest);
 return(0);
}

/**
 * e_unpack_stopped - Did the user press Esc during the last unpack?
 *
 * The help and info readers ask after an open that gave nothing: a user who
 * stopped the wait does not want the next candidate file unpacked, and
 * waited for, in its place.
 * Return: non-zero when the last unpack was stopped.
 */
static int e_unpack_stopped(void)
{
 return(e_unpack_was_stopped);
}
#else
#define e_unpack_stopped() 0
#endif

IFILE *e_i_fopen(char *path, char *stat)
{
 char *tmp2;
#ifdef HAS_LIBZ
 IFILE *fp;

 if (!path) {  return(NULL);  }
 if ((fp = gzopen(path, stat))) {  return(fp);  }
 if (!(tmp2 = MALLOC((strlen(path)+11)*sizeof(char))))
 {
  FREE(fp);
  return(NULL);
 }
 strcpy(tmp2, path);
 strcat(tmp2, ".info");
 if ((fp = gzopen(tmp2, stat)))
 {
  FREE(tmp2);
  return(fp);
 }
 strcpy(tmp2, path);
 strcat(tmp2, ".gz");
 if ((fp = gzopen(tmp2, stat)))
 {
  FREE(tmp2);
  return(fp);
 }
 strcpy(tmp2, path);
 strcat(tmp2, ".Z");
 if ((fp = gzopen(tmp2, stat)))
 {
  FREE(tmp2);
  return(fp);
 }
 strcpy(tmp2, path);
 strcat(tmp2, ".info.gz");
 if ((fp = gzopen(tmp2, stat)))
 {
  FREE(tmp2);
  return(fp);
 }
 strcpy(tmp2, path);
 strcat(tmp2, ".info.Z");
 if ((fp = gzopen(tmp2, stat)))
 {
  FREE(tmp2);
  return(fp);
 }
 FREE(fp);
 FREE(tmp2);
 return(NULL);
#else
 extern char *e_tmp_dir;
 char *tmp;
 int len;
 IFILE *fp = MALLOC(sizeof(IFILE));

 /* An Esc from an earlier open must not stop the readers' search now. */
 e_unpack_was_stopped = 0;
 if (!fp) return(NULL);
 if (!path) {  FREE(fp); return(NULL);  }
 if ((fp->fp = fopen(path, stat))) {  fp->sw = 0;  return(fp);  }
 if (!(tmp2 = MALLOC((strlen(path)+11)*sizeof(char)))) 
 {
  FREE(fp);
  return(NULL);
 }
 strcpy(tmp2, path);
 strcat(tmp2, ".info");
 if ((fp->fp = fopen(tmp2, stat))) 
 {
  FREE(tmp2);
  fp->sw = 0;
  return(fp);
 }
 strcpy(tmp2, path);
 strcat(tmp2, ".gz");
 if (access(tmp2, 0))
 {
  strcpy(tmp2, path);
  strcat(tmp2, ".Z");
  if (access(tmp2, 0))
  {
   strcpy(tmp2, path);
   strcat(tmp2, ".info.gz");
   if (access(tmp2, 0))
   {
    strcpy(tmp2, path);
    strcat(tmp2, ".info.Z");
    if (access(tmp2, 0))
    {
     FREE(fp);
     FREE(tmp2);
     return(NULL);
    }
   }
  }
 }
 if (!(tmp = MALLOC((strlen(path)+(len=strlen(e_tmp_dir))+2)*sizeof(char)))) 
 {
  FREE(fp);
  FREE(tmp2);
  return(NULL);
 }
 strcpy(tmp, e_tmp_dir);
 if (path[0] != DIRC) {  tmp[len] = DIRC;  tmp[len+1] = '\0';  }
 strcat(tmp, path);
 if ((fp->fp = fopen(tmp, stat)))
 {
  fp->sw = 1;
  FREE(tmp);
  FREE(tmp2);
  return(fp);
 }
 e_mkdir_path(tmp);
 if (!e_unpack_to_file(tmp2, tmp))
 {
  FREE(fp);
  FREE(tmp);
  FREE(tmp2);
  return(NULL);
 }
 FREE(tmp2);
 fp->fp = fopen(tmp, stat);
 FREE(tmp);
 if (!fp->fp) {  FREE(fp);  return(NULL);  }
 fp->sw = 1;
 return(fp);
#endif
}

#ifndef HAS_LIBZ
int e_i_fclose(IFILE *fp)
{
 int ret = fclose(fp->fp);

 FREE(fp);
 return(ret);
}
#endif

/**
 * e_help_add_row - Add a row of text to the Help or Info window being filled.
 * @b: the window's buffer; @text: the row, its line break included.
 *
 * Without memory the window keeps the rows it has and the status line says
 * so.  Return: 0, or -1 when memory ran out.
 */
static int e_help_add_row(BUFFER *b, const char *text)
{
 if (e_eol_append_text(b, text, 0, E_EOL_NORMALIZE) == E_BUFFER_OK)
  return(0);
 wpe_buffer_say_nomem(b, "the help text");
 return(-1);
}

/**
 * e_help_set_first_row - Make @line the first row of a help page or Info node.
 *
 * A page opens on the row the reader found, whole, however long the file's
 * line is: the row grows to the line instead of the line running past a row
 * sized for the window's width.  If memory is short the user is told and
 * the page opens without its first row.
 * Return: 0, or -1 when the row could not be set.
 */
static int e_help_set_first_row(BUFFER *b, const char *line)
{
 int n = strlen(line);

 if (n > 0 && line[n-1] == '\n')
  n--;
 if (e_buffer_set_line_ended(b, b->mxlines-1, line, n, WPE_WR) == E_BUFFER_OK)
  return(0);
 wpe_buffer_say_nomem(b, "the help text");
 return(-1);
}

/* The byte that ends a link, a header or a file name in a help row. */
static const char help_end_mark[] = { HED, '\0' };

/* The headline mark the Info reader puts in front of a node's first row. */
static const char help_head_mark[] = { HHD, '\0' };

/**
 * e_info_node_key - The text a node header must contain for the node named
 * @name to be the one opened: "Node: " and the name, without its end mark.
 *
 * The key is held at the name's real length, so a long node name is looked
 * up whole, never by its first bytes.
 * Return: the key in malloc'd storage, or NULL when memory is short.
 */
static char *e_info_node_key(const char *name)
{
 size_t room = strlen(name) + sizeof("Node: ");
 char *key = MALLOC(room);
 size_t len;

 if (!key)
  return(NULL);
 wpe_help_lcpy(key, room, "Node: ");
 len = wpe_help_lcat(key, room, name);
 if (len > 0 && key[len-1] == HED)
  key[len-1] = '\0';
 return(key);
}

/**
 * e_help_take_name - Take the name that starts at byte @from of a help row.
 * @name: where it goes, @size bytes.
 *
 * The row's own bytes are the limit: the line break after the last byte of
 * a row is never part of a name.  A name is a key (a page to jump to, a file
 * to open), so one that does not fit is not used cut: the caller refuses.
 * Return: 0 when the name fits whole, -1 when it would have been cut.
 */
static int e_help_take_name(const STRING *row, int from, char *name, size_t size)
{
 const char *src;
 int avail, cut;

 if (from > row->len)
  from = row->len;
 avail = row->len - from;
 src = (const char *) row->s + from;
 wpe_help_token(name, size, src, avail, help_end_mark, NULL, &cut);
 if (cut)
  return(-1);
 return(0);
}

/**
 * e_help_take_header - Build the key "headline mark, name, end mark" of the
 * page the link at byte @from of a row points to.
 *
 * It is the text e_read_help and e_read_info search the file for, and what
 * Back returns to.
 * Return: 0, or -1 when the name does not fit @key whole.
 */
static int e_help_take_header(const STRING *row, int from, char *key, size_t size)
{
 if (size < 3)
  return(-1);
 key[0] = HHD;
 if (e_help_take_name(row, from, key + 1, size - 2) != 0)
  return(-1);
 wpe_help_lcat(key, size, help_end_mark);
 return(0);
}

/**
 * e_help_refuse_name - Tell the user a link names something too long to follow.
 *
 * A very long link in a help page or a long name in an Info file is not
 * followed to a page chosen by its first bytes; the user sees why nothing
 * opened.
 * Return: 1, the "no jump" result of e_help_ret.
 */
static int e_help_refuse_name(FENSTER *f)
{
 e_error("Link name too long", 0, f->fb);
 return(1);
}

int e_read_help(char *str, FENSTER *f, int sw)
{
 IFILE *fp;
 char *ptmp, tstr[WPE_HELP_LINE_SIZE];
 int i;

 ptmp = e_lib_file(HELP_FILE);
 fp = e_i_fopen(ptmp, "rb");
 FREE(ptmp);
 if (!fp)
  return(1);
 e_close_buffer(f->b);
 if ((f->b = (BUFFER *) MALLOC(sizeof(BUFFER))) == NULL)
  e_error(e_msg[ERR_LOWMEM], 1, f->fb);
 e_buffer_init(f->b, f->ed, f, f->ed->maxcol, E_BUFFER_HISTORY_FULL);
 e_buffer_mark_view(f->b);
 if (f->b->mxlines != 1)
  e_error(e_msg[ERR_LOWMEM], 1, f->fb);
 f->b->fb = f->fb;
 if (str && str[0])
 {
  while ((ptmp = e_i_fgets(tstr, WPE_HELP_LINE_MAX, fp)) && !WpeStrcstr(tstr, str))
   ;
  if (ptmp && !sw && e_help_set_first_row(f->b, tstr) != 0)
  {
   e_i_fclose(fp);
   return(2);
  }
 }
 if (sw)
 {
  char *tp = NULL, tmp[WPE_HELP_LINE_SIZE], ts[2];
  int cut;

  ts[0] = HHD; ts[1] = '\0';
  while ((ptmp = e_i_fgets(tstr, WPE_HELP_LINE_MAX, fp)) && !(tp = strstr(tstr, ts)))
   ;
  if (!str || !str[0])
   while((ptmp = e_i_fgets(tstr, WPE_HELP_LINE_MAX, fp)) && !(tp = strstr(tstr, ts)))
    ;
  if (!ptmp)
  {
   e_i_fclose(fp);
   return(e_error("No Next Page", 0, f->fb));
  }
  else
  {
   wpe_help_token(tmp, sizeof tmp - 1, tp, strlen(tp), help_end_mark, NULL, &cut);
   if (cut)
   {
    e_i_fclose(fp);
    return(e_error("Header name too long", 0, f->fb));
   }
   wpe_help_lcat(tmp, sizeof tmp, help_end_mark);
   if ((ud_help->str = MALLOC((strlen(tmp)+1)*sizeof(char))) != NULL)
    strcpy(ud_help->str, tmp);
   if (e_help_set_first_row(f->b, tstr) != 0)
   {
    e_i_fclose(fp);
    return(2);
   }
  }
 }
 while(e_i_fgets(tstr, WPE_HELP_LINE_MAX, fp))
 {
  for(i = 0; tstr[i]; i++)
  if (tstr[i] == HFE)  {  e_i_fclose(fp);  return(0);  }
  if (e_help_add_row(f->b, tstr) != 0)
   break;
 }
 e_i_fclose(fp);
 return(2);
}

int e_help_ret(FENSTER *f)
{
 BUFFER *b = f->b;
 int i;
 char str[WPE_HELP_LINE_SIZE];
 struct help_ud *next;

   for(i = b->b.x; i >= 0 && b->bf[b->b.y].s[i] != HED; i--)
   {  if(b->bf[b->b.y].s[i] == HBG)
      {  if(e_help_take_header(&b->bf[b->b.y], i+1, str, sizeof str) != 0)
	    return(e_help_refuse_name(f));
	 if((next = MALLOC(sizeof(struct help_ud))) != NULL)
	 {  next->str = MALLOC((strlen(str)+1) * sizeof(char));
	    if(next->str) strcpy(next->str, str);
            if(ud_help && ud_help->file)
            {  next->file = MALLOC((strlen(ud_help->file)+1) * sizeof(char));
               if(next->file) strcpy(next->file, ud_help->file);
            }
            else next->file = NULL;
            next->nstr = next->pstr = NULL;
            next->x = b->b.x;
            next->y = b->b.y;
            next->sw = ud_help ? ud_help->sw : 0;
	    next->next = ud_help;
	    ud_help = next;
	 }
         if(ud_help->sw) e_read_info(str, f, ud_help->file);
	 else e_read_help(str, f, 0);
	 b = f->b; e_buffer_cursor_set(b, 0, 0);
	 e_cursor(f, 1);
	 e_schirm(f, 1);
	 return(0);
      }
      else if(b->bf[b->b.y].s[i] == HNF)
      {  for(i++; i < b->bf[b->b.y].len && b->bf[b->b.y].s[i] != HED; i++);
	 if(e_help_take_name(&b->bf[b->b.y], i+1, str, sizeof str) != 0)
	    return(e_help_refuse_name(f));
	 if((next = MALLOC(sizeof(struct help_ud))) != NULL)
	 {  next->str = MALLOC(4 * sizeof(char));
	    if(next->str) strcpy(next->str, "Top");
            next->file = MALLOC((strlen(str)+1) * sizeof(char));
            if(next->file) strcpy(next->file, str);
            next->sw = 1;
	    next->next = ud_help;
            next->x = b->b.x;
            next->y = b->b.y;
            next->nstr = next->pstr = NULL;
	    ud_help = next;
	 }
         e_read_info("Top", f, ud_help->file);
	 b = f->b; e_buffer_cursor_set(b, 0, 0);
	 e_cursor(f, 1);
	 e_schirm(f, 1);
	 return(0);
      }
      else if(b->bf[b->b.y].s[i] == HFB)
      {  if(e_help_take_name(&b->bf[b->b.y], i+1, str, sizeof str) != 0)
	    return(e_help_refuse_name(f));
	 return(e_ed_man(str, f));
      }
      else if(b->bf[b->b.y].s[i] == HHD) return(e_help_last(f));
   }
   return(1);
}

int e_help_last(FENSTER *f)
{
 struct help_ud *last = ud_help;

 if (!last)
  return(1);
 if (last->sw)
 {
  if (last->next == NULL)
   e_read_info(NULL, f, NULL);
  else
   e_read_info(last->next->str, f, last->next->file);
 }
 else
 {
  if (last->next == NULL)
   e_read_help(NULL, f, 0);
  else
   e_read_help(last->next->str, f, 0);
 }
 e_buffer_cursor_set(f->b, last->y, last->x);
 e_cursor(f, 1);
 e_schirm(f, 1);
 ud_help = last->next;
 FREE(last->str);
 FREE(last);
 return(0);
}

int e_help_next(FENSTER *f, int sw)
{
   struct help_ud *last = ud_help;
   if(last && last->sw)
   {  if(sw && last->nstr)
      {  if(last->str) FREE(last->str);
         if(last->pstr) FREE(last->pstr);
         last->str = last->nstr;
         last->nstr = NULL;
      }
      else if(!sw && last->pstr)
      {  if(last->str) FREE(last->str);
         if(last->nstr) FREE(last->nstr);
         last->str = last->pstr;
         last->pstr = NULL;
      }
      else
         return(e_error(sw ? "No Next Page" : "No Previous Page", 0, f->fb));
      e_read_info(last->str, f, last->file);
      e_buffer_cursor_set(f->b, 0, 0);
      e_cursor(f, 1);
      e_schirm(f, 1);
      return(0);
   }
   else
   {  if(sw)
      {  if((last = MALLOC(sizeof(struct help_ud))) != NULL)
	 {  last->str = NULL;
            last->file = NULL;
	    last->x = last->y = 0;
	    last->nstr = last->pstr = NULL;
	    last->sw = 0;
	    last->next = ud_help;
	    ud_help = last;
	    e_read_help(ud_help->next ? ud_help->next->str : NULL, f, 1);
            e_buffer_cursor_set(f->b, 0, 0);
            e_cursor(f, 1);
            e_schirm(f, 1);
            return(0);
	 }
      }
      else return(e_help_last(f));
   }
   return(e_error(sw ? "No Next Page" : "No Previous Page", 0, f->fb));
}

int e_help_free(FENSTER *f)
{
 struct help_ud *next, *last = ud_help;

 while (last)
 {
  next = last->next;
  if (last->str) FREE(last->str);
  if (last->pstr) FREE(last->pstr);
  if (last->nstr) FREE(last->nstr);
  if (last->file) FREE(last->file);
  FREE(last);
  last = next;
 }
 ud_help = NULL;
 return(0);
}

/* The bytes that end a switch or a header name when the help is checked. */
static const char help_comp_stops[] = { HED, HBG, HHD, HFE, '\0' };

/**
 * e_help_comp_name - The switch or header name that follows the mark at byte
 * @mark of row @row, at its real length.
 * @stop: set to the byte where the name ended; @closed: set to 1 when an
 * end mark ended it.
 *
 * Checking a help file for missing headers compares names, so none is cut.
 * Return: the name in malloc'd storage, or NULL when memory is short.
 */
static char *e_help_comp_name(const STRING *row, int mark, int *stop, int *closed)
{
 const char *src = (const char *) row->s + mark + 1;
 int avail = row->len - (mark + 1);
 const char *end;
 char *name;

 name = wpe_help_token_dup(src, avail, help_comp_stops, &end);
 *stop = mark + 1 + (int) (end - src);
 *closed = (end < src + avail && *end == HED);
 return(name);
}

/**
 * e_help_comp_add - Append @name to the list @list of @count names.
 * Return: 0, or -1 (@name freed) when memory is short.
 */
static int e_help_comp_add(char ***list, int *count, char *name)
{
 char **grown;

 if (!name)
  return(-1);
 grown = REALLOC(*list, (*count + 1) * sizeof(char *));
 if (!grown)
 {
  FREE(name);
  return(-1);
 }
 grown[(*count)++] = name;
 *list = grown;
 return(0);
}

/**
 * e_help_comp_collect - Collect the name after every @mark of the help window.
 * @what: the message for a mark whose name is not closed by an end mark.
 *
 * On a mark without its end mark the user is told and the cursor goes to the
 * place, so the author of the help file sees which one to fix.
 * Return: 0, or 2 when a mark is broken or memory is short.
 */
static int e_help_comp_collect(FENSTER *f, int mark, const char *what,
                               char ***list, int *count)
{
 BUFFER *b = f->b;
 int i, j, stop, closed;
 char *name;

 for (j = 0; j < b->mxlines; j++)
 {
  for (i = 0; i < b->bf[j].len; i++)
  {
   if (b->bf[j].s[i] != mark)
    continue;
   name = e_help_comp_name(&b->bf[j], i, &stop, &closed);
   if (name && !closed)
   {
    FREE(name);
    e_error((char *) what, 0, f->fb);
    e_buffer_cursor_set(b, j, stop);
    e_cursor(f, 1);
    return(2);
   }
   if (e_help_comp_add(list, count, name) != 0)
   {
    e_error(e_msg[ERR_LOWMEM], 0, f->fb);
    return(2);
   }
  }
 }
 return(0);
}

/**
 * e_help_comp_pairs - Tell the user of the first switch without a header, or
 * header without a switch.
 *
 * The message shows the name cut to the line buffer: it is display only.
 */
static void e_help_comp_pairs(FENSTER *f, char **swtch, int sn, char **hdrs, int hn)
{
 char str[WPE_HELP_LINE_SIZE];
 int i, j;

 for (j = 0; j < sn; j++)
 {
  for (i = 0; i < hn; i++) if(!WpeStrccmp(swtch[j], hdrs[i])) break;
  if (i >= hn)
  {
   snprintf(str, sizeof str, "Switch \'%s\' has no Header!", swtch[j]);
   e_error(str, 0, f->fb);
   return;
  }
 }
 for (j = 0; j < hn; j++)
 {
  for (i = 0; i < sn; i++) if(!WpeStrccmp(swtch[i], hdrs[j])) break;
  if (i >= sn)
  {
   snprintf(str, sizeof str, "No Jump to Header \'%s\'!", hdrs[j]);
   e_error(str, 0, f->fb);
   return;
  }
 }
}

int e_help_comp(FENSTER *f)
{
 int i, hn = 0, sn = 0, result;
 char **swtch = NULL;
 char **hdrs = NULL;

 if (f->dtmd != DTMD_HELP) return(1);
 result = e_help_comp_collect(f, HBG, "Error in Switch!", &swtch, &sn);
 if (result == 0)
  result = e_help_comp_collect(f, HHD, "Error in Header!", &hdrs, &hn);
 if (result == 0)
  e_help_comp_pairs(f, swtch, sn, hdrs, hn);
 for (i = 0; i < sn; i++) FREE(swtch[i]);
 for (i = 0; i < hn; i++) FREE(hdrs[i]);
 FREE(swtch);
 FREE(hdrs);
 return(result);
}

/*          Help window    */
int e_help(FENSTER *f)
{
 extern char *e_hlp;

 e_hlp = NULL;
 return(e_help_loc(f, 0));
}

int e_info(FENSTER *f)
{
 extern char *e_hlp;

 e_hlp = NULL;
 return(e_help_loc(f, 1));
}

#define IFE 31

/**
 * info_mark_at - Put one markup byte into an Info line at an offset of @str.
 * @line/@n: the whole line buffer; @str: the part of it being scanned, so
 * that @at counts from @str.
 *
 * Return: 0 inserted, -1 when the line has no room left.
 */
static int info_mark_at(char *line, size_t n, const char *str, int at,
                        char mark)
{
 return(wpe_help_line_insert(line, n, (int) (str - line) + at, &mark, 1));
}

/**
 * info_mark_cross_reference - Turn a menu entry or *note into a button.
 * @line/@n: the whole line buffer; @str: the scanned part; @bg/@nd: where
 * the reference text starts and where its end mark goes.
 *
 * The end mark and the start mark are two inserts, so the room for both is
 * checked first: a reference that cannot be marked up stays whole, readable
 * text instead of a button with only one end.
 * Return: 0 marked up or left as plain text.
 */
static int info_mark_cross_reference(char *line, size_t n, const char *str,
                                     int bg, int nd)
{
 if (strlen(line) + 2 + 1 > n)
  return(0);
 if (info_mark_at(line, n, str, nd, HED) != 0)
  return(0);
 info_mark_at(line, n, str, bg, HBG);
 return(0);
}

/**
 * info_mark_file_link - Turn a menu entry "(file)node" into a button that
 * opens another Info file.
 * @line/@n: the whole line buffer; @str: the scanned part; @bg: where the
 * entry starts; @nd: the offset of its '('.
 *
 * Only one byte is inserted, in front of the entry; the parentheses are
 * overwritten with end marks afterwards, so a full line leaves the entry as
 * plain text.
 * Return: 0 marked up or left as plain text.
 */
static int info_mark_file_link(char *line, size_t n, char *str, int bg,
                               int nd)
{
 if (info_mark_at(line, n, str, bg, HNF) != 0)
  return(0);
 str[nd+1] = HED;
 for (nd++; str[nd] && str[nd] != ')'; nd++)
  ;
 if (str[nd])
  str[nd] = HED;
 return(0);
}

int e_mk_info_button(char *line, size_t n, char *str)
{
 int bg, nd;

 if (str[0] == '\n' || str[0] == '\0')
  return(1);
 for (bg = 0; str[bg] && isspace(str[bg]); bg++)
  ;
 for (nd = bg; str[nd] && (str[nd] != ':' || 
   (!isspace(str[nd+1]) && (str[nd+1] != '(') && (str[nd+1] != ':' || 
   (!isspace(str[nd+2]) && str[nd+2] != '.')))); nd++)
  ;
 if (!str[nd]) return(-1);
 if (str[nd+1] != ':')
 {
  for (nd++; str[nd] && isspace(str[nd]); nd++);
  if (str[nd] == '(')
   return(info_mark_file_link(line, n, str, bg, nd));
  for (; str[nd] && (str[nd] != '.' || isalnum1(str[nd+1])); nd++);
  if (!str[nd]) return(-1);
  for (bg = nd; str[bg] != ':' || !isspace(str[bg+1]); bg--);
  for (bg++; isspace(str[bg]); bg++);
 }
 return(info_mark_cross_reference(line, n, str, bg, nd));
}

int e_mk_info_mrk(char *str)
{
 int bg, nd = -1;

 do
 {
  for (bg = nd+1; str[bg] && str[bg] != '`'; bg++)
   ;
  for (nd = bg; str[nd] && str[nd] != '\''; nd++)
   ;
  if (str[nd])
  {
   str[bg] = HBB;
   str[nd] = HED;
  }
 } while(str[nd]);
 return(0);
}

/* One subfile of an indirect Info file: its name and the first line it holds. */
struct FL_INFO {  char *name; int line;  };

/* The byte between a node's name and its line in the tag table of an Info file. */
#define INFO_NODE_SEP '\177'
static const char info_name_end[] = { ':', '\0' };

/**
 * e_info_table_add - Add the subfile named by a row "name: line" of the
 * Indirect table to @files.
 *
 * The name is kept at its real length: it is the file that gets opened.  A
 * row without a colon names nothing and is skipped.
 * Return: 0 added, -1 skipped.
 */
static int e_info_table_add(struct FL_INFO ***files, int *anz, const char *row)
{
 const char *end;
 char *name;
 struct FL_INFO *entry, **grown;

 name = wpe_help_token_dup(row, strlen(row), info_name_end, &end);
 if (!name)
  return(-1);
 if (*end != ':')
 {
  FREE(name);
  return(-1);
 }
 entry = MALLOC(sizeof(struct FL_INFO));
 grown = REALLOC(*files, (*anz + 1) * sizeof(struct FL_INFO *));
 if (!entry || !grown)
 {
  FREE(name);
  FREE(entry);
  if (grown)
   *files = grown;
  return(-1);
 }
 entry->name = name;
 entry->line = atoi(end + 1);
 grown[(*anz)++] = entry;
 *files = grown;
 return(0);
}

/**
 * e_info_tag_line - The line a tag table row gives for the node @key names,
 * or -1 when the row is another node's.
 * @key/@keylen: "Node: " and the name searched for.
 *
 * Only a row whose name ends where @key ends is that node's, so a node whose
 * name merely starts with the one asked for ("Topaz" when the user follows
 * "Top") does not send the reader to the other node's subfile.  The number is
 * read after the separator that ends the name, never at a fixed place.
 */
static int e_info_tag_line(const char *row, const char *key, size_t keylen)
{
 if (strncmp(row, key, keylen) != 0 || row[keylen] != INFO_NODE_SEP)
  return(-1);
 return(atoi(row + keylen + 1));
}

IFILE *e_info_jump(char *str, char **path, IFILE *fp)
{
 IFILE *fpn;
 int i, j, n, anz = 0;
 char *ptmp, *fstr, tstr[WPE_HELP_LINE_SIZE];
 struct FL_INFO **files = NULL;

 fstr = e_info_node_key(str);
 if (!fstr)
  return(fp);
 while (e_i_fgets(tstr, WPE_HELP_LINE_MAX, fp) && tstr[0] != IFE)
  e_info_table_add(&files, &anz, tstr);
 if (anz == 0)
 {
  FREE(files);
  FREE(fstr);
  return(fp);
 }
 i = strlen(fstr);
 n = -1;
 while ((ptmp = e_i_fgets(tstr, WPE_HELP_LINE_MAX, fp))
        && (n = e_info_tag_line(tstr, fstr, i)) < 0)
  ;
 if (ptmp && n >= 0)
 {
  for (i = 1; i < anz && n >= files[i]->line; i++)
   ;
  if (files[i-1]->name[0] == DIRC)
  {
   ptmp = MALLOC((strlen(files[i-1]->name)+1)*sizeof(char));
   strcpy(ptmp, files[i-1]->name);
  }
  else
  {
   for (n = strlen(*path)-1; n >= 0 && *(*path+n) != DIRC; n--)
    ;
   n++;
   ptmp = MALLOC((strlen(files[i-1]->name)+n+1)*sizeof(char));
   for (j = 0; j < n; j++)
    ptmp[j] = *(*path+j);
   ptmp[j] = '\0';
   strcat(ptmp, files[i-1]->name);
  }
  if ((fpn = e_i_fopen(ptmp, "rb")) != NULL)
  {
   e_i_fclose(fp);
   fp = fpn;
   FREE(*path);
   *path = ptmp;
  }
  else
   FREE(ptmp);
 }
 FREE(fstr);
 for (i = 0; i < anz; i++)
 {
  FREE(files[i]->name);
  FREE(files[i]);
 }
 FREE(files);
 return(fp);
}

/* The bytes that end the name after "Next:" or "Prev:" in a node header. */
static const char info_link_end[] = { ',', '\n', '\0' };

/**
 * e_mk_info_pt - The name after "@node:" ("Next" or "Prev") in an Info node
 * header line, so Next and Prev can open the neighbouring node.
 *
 * The name is a key, so it is returned at its real length.  The search for
 * the field goes on after a word that only starts with @node (a file called
 * "Nextfile.info" in the same header), so such a header cannot hold the
 * editor in a loop.
 * Return: the name in malloc'd storage, or NULL when there is none.
 */
char *e_mk_info_pt(char *str, char *node)
{
 char *ptmp, *name;
 const char *from = str;

 do
 {
  if (!(ptmp = strstr(from, node)))
   return(NULL);
  while (*ptmp && *ptmp != ',' && *ptmp != ':')
   ptmp++;
  if (!*ptmp)
   return(NULL);
  from = ptmp + 1;
 } while (*ptmp == ',');
 for (ptmp++; isspace(*ptmp); ptmp++)
  ;
 name = wpe_help_token_dup(ptmp, strlen(ptmp), info_link_end, NULL);
 if (name && name[0] == '\0')
 {
  FREE(name);
  return(NULL);
 }
 return(name);
}

char *e_mk_info_path(char *path, char *file)
{
 int n;
 char *tp;

 if (!info_file || !*info_file) return(NULL);
 if (file[0] == DIRC)
 {
  if (path && !strcmp(path, file)) {  FREE(path);  return(NULL);  }
  else if(path) FREE(path);
  if (!(path = MALLOC((strlen(file) + 1) * sizeof(char)))) return(NULL);
  return (strcpy(path, file));
 }
 tp = info_file;
 if (path)
 {
  n = strlen(path) - strlen(file);
  if (n > 1) path[n - 1] = PTHD;
  else if (n == 1) path[n++] = PTHD;
  while (strncmp(tp, path, n) && (tp = strchr(tp, PTHD)) != NULL && *++tp);
  if (tp == NULL || !*tp || !*(tp += n)) return(NULL);
  FREE(path);
 }
 for (n = 0; tp[n] && tp[n] != PTHD; n++);
 if (!(path = MALLOC((strlen(file) + n + 2) * sizeof(char)))) return(NULL);
 strncpy(path, tp, n);
 if (n > 1) path[n++] = DIRC;
 strcpy(path + n, file);
 return(path);
}

int e_read_info(char *str, FENSTER *f, char *file)
{
   IFILE *fp = NULL;
   char *path = NULL, *ptmp, tstr[WPE_HELP_LINE_SIZE], *fstr;
   int i, sw = 0, bsw = 0;
   if(!str) str = "Top";
   if(str[0] == HHD) str++;
   fstr = e_info_node_key(str);
   if(!fstr) e_error(e_msg[ERR_LOWMEM], 1, f->fb);
   if(!file || !file[0]) file = "dir";
   do
   {  path = e_mk_info_path(path, file);
      fp = e_i_fopen(path, "rb");
   } while(!fp && path && !e_unpack_stopped());
   if(!fp) {  FREE(fstr);  return(1);  }
   e_close_buffer(f->b);
   if( (f->b = (BUFFER *) MALLOC(sizeof(BUFFER))) == NULL)
   e_error(e_msg[ERR_LOWMEM], 1, f->fb);
   e_buffer_init(f->b, f->ed, f, f->ed->maxcol, E_BUFFER_HISTORY_FULL);
   e_buffer_mark_view(f->b);
   if (f->b->mxlines != 1)
    e_error(e_msg[ERR_LOWMEM], 1, f->fb);
   f->b->fb = f->fb;
   while((ptmp = e_i_fgets(tstr, WPE_HELP_LINE_MAX, fp)) != NULL)
   {  if(!strncmp(tstr, "Indirect:", 9)) fp = e_info_jump(str, &path, fp);
      else if(!strncmp(tstr, "File:", 5) && strstr(tstr, fstr)) break;
   }
   FREE(fstr);
   FREE(path);
   if(ptmp)
   {  ud_help->nstr = e_mk_info_pt(tstr, "Next");
      ud_help->pstr = e_mk_info_pt(tstr, "Prev");
   }
   if(!strcmp(file, "dir"))
      while((ptmp = e_i_fgets(tstr, WPE_HELP_LINE_MAX, fp))
         && WpeStrnccmp(tstr, "* Menu:", 7));
   else while((ptmp = e_i_fgets(tstr, WPE_HELP_LINE_MAX, fp)) && tstr[0] == '\n');
   if(ptmp)
   {  if(!sw && !WpeStrnccmp(tstr, "* Menu:", 7)) sw = 1;
      wpe_help_line_insert(tstr, sizeof tstr, 0, help_head_mark, 1);
      if(e_help_set_first_row(f->b, tstr) != 0)
      {  e_i_fclose(fp);
         return(2);
      }
   }
   while(e_i_fgets(tstr, WPE_HELP_LINE_MAX, fp))
   {  for(i = 0; tstr[i]; i++)
      if(tstr[i] == IFE)  {  e_i_fclose(fp);  return(0);  }
      if(bsw == 1)
      {  bsw = e_mk_info_button(tstr, sizeof tstr, tstr);
         for(ptmp = tstr; (ptmp = WpeStrcstr(ptmp, "*note")); ptmp += 5)
            bsw = e_mk_info_button(tstr, sizeof tstr, ptmp+5);
      }
      else if(!sw && !WpeStrnccmp(tstr, "* Menu:", 7)) sw = 1;
      else if((ptmp = WpeStrcstr(tstr, "*note")))
      {  bsw = e_mk_info_button(tstr, sizeof tstr, ptmp+5);
         for(ptmp += 5; (ptmp = WpeStrcstr(ptmp, "*note")); ptmp += 5)
            bsw = e_mk_info_button(tstr, sizeof tstr, ptmp+5);
      }
      else if(sw && tstr[0] == '*') 
      {  bsw = e_mk_info_button(tstr, sizeof tstr, tstr+1);
         for(ptmp = tstr+1; (ptmp = WpeStrcstr(ptmp, "*note")); ptmp += 5)
            bsw = e_mk_info_button(tstr, sizeof tstr, ptmp+5);
      }
      e_mk_info_mrk(tstr);
      if (e_help_add_row(f->b, tstr) != 0)
         break;
   }
   e_i_fclose(fp);
   return(2);
}

/**
 * e_read_xwpe_info - Open xwpe's own manual in the Info viewer (Help > Info).
 *
 * Help > Info should land in OUR documentation -- the xwpe Texinfo manual,
 * installed as @file{xwpe.info} -- not the system-wide Info directory that
 * lists every package on the machine.  Loads the @code{Top} node of
 * @file{xwpe.info} and records the file on the help stack so in-manual
 * navigation (Goto/Next/Prev/Back) stays within it.  If @file{xwpe.info} is
 * not installed on the Info path (e.g. running uninstalled from the build
 * tree, or a stripped-down system), it falls back to the system directory so
 * the menu still does something useful.
 *
 * Return: 0.
 */
static int e_read_xwpe_info(FENSTER *hw)
{
 int not_opened = e_read_info("Top", hw, "xwpe");

 if (not_opened == 1 && e_unpack_stopped())
  return(0);
 if (not_opened != 1)
 {
  if (ud_help)
  {
   ud_help->file = MALLOC(5 * sizeof(char));
   if (ud_help->file)
    strcpy(ud_help->file, "xwpe");
  }
 }
 else
  e_read_info(NULL, hw, NULL);
 return(0);
}

int e_help_loc(FENSTER *f, int sw)
{
 extern char *e_hlp;
 int i;
 char *tmp = NULL;
 struct help_ud *next;

 if (!sw)
  tmp = e_hlp;
 for (i = f->ed->mxedt; i >= 0; i--)
 {
  if (!strcmp(f->ed->f[i]->datnam, "Help"))
  {
   e_switch_window(f->ed->edt[i], f);
   if (ud_help && sw != ud_help->sw)
   {
    e_close_window(f->ed->f[f->ed->mxedt]);
    i = -1;
   }
   break;
  }
 }
 if (i < 0)
  e_edit(f->ed, "Help");
 if ((tmp || sw) && (next = MALLOC(sizeof(struct help_ud))))
 {
  if (tmp)
  {
   next->str = MALLOC((strlen(tmp)+1) * sizeof(char));
   if (next->str)
    strcpy(next->str, tmp);
  }
  else
   next->str = NULL;
  next->file = NULL;
  next->x = next->y = 0;
  next->nstr = next->pstr = NULL;
  next->sw = sw;
  next->next = ud_help;
  ud_help = next;
 }
 if (sw)
  e_read_xwpe_info(f->ed->f[f->ed->mxedt]);
 else
  e_read_help(tmp, f->ed->f[f->ed->mxedt], 0);
 e_schirm(f->ed->f[f->ed->mxedt], 1);
 return(0);
}

int e_help_options(FENSTER *f)
{
 char str[WPE_PATH_SIZE];

 if (!info_file)
 {
  info_file = MALLOC(1);
  info_file[0] = '\0';
 }
 /* A path from an option file longer than the entry takes is not offered
    for editing: a cut path would be written back as the Info-Path. */
 if (wpe_field_take(info_file, str, sizeof str) != 0)
 {
  e_input_limit_say(WPE_PATH_FIELD_MAX);
  return(0);
 }
 if (e_add_arguments(str, sizeof str, "Info-Path", f, 0 , AltI, NULL))
 {
  info_file = REALLOC(info_file, strlen(str) + 1);
  strcpy(info_file, str);
 }
 return(0);
}

int e_hp_next(FENSTER *f)
{
 int i;

 for (i = f->ed->mxedt; i >= 0; i--)
 {
  if (!strcmp(f->ed->f[i]->datnam, "Help"))
  {
   e_switch_window(f->ed->edt[i], f);
   f = f->ed->f[f->ed->mxedt];
   break;
  }
 }
 if (i < 0)
  return(e_help_loc(f, 0));
 else
  return(e_help_next(f, 1));
}   

int e_hp_prev(FENSTER *f)
{
 int i;

 for (i = f->ed->mxedt; i >= 0; i--)
 {
  if (!strcmp(f->ed->f[i]->datnam, "Help"))
  {
   e_switch_window(f->ed->edt[i], f);
   f = f->ed->f[f->ed->mxedt];
   break;
  }
 }
 if (i < 0)
  return(e_help_loc(f, 0));
 else
  return(e_help_next(f, 0));
}   

int e_hp_back(FENSTER *f)
{
 int i;

 for (i = f->ed->mxedt; i >= 0; i--)
 {
  if (!strcmp(f->ed->f[i]->datnam, "Help"))
  {
   e_switch_window(f->ed->edt[i], f);
   f = f->ed->f[f->ed->mxedt];
   break;
  }
 }
 if (i < 0)
  return(e_help_loc(f, 0));
 else
  return(e_help_last(f));
}   

int e_hp_ret(FENSTER *f)
{
 int i;

 for (i = f->ed->mxedt; i >= 0; i--)
 {
  if (!strcmp(f->ed->f[i]->datnam, "Help"))
  {
   e_switch_window(f->ed->edt[i], f);
   f = f->ed->f[f->ed->mxedt];
   break;
  }
 }
 if (i < 0)
  return(e_help_loc(f, 0));
 else
  return(e_help_ret(f));
}   

/* Give a context-sensitive help for the identifier under cursor */
int e_topic_search(FENSTER *f)
{
 int x, y;
 char *s;
 BUFFER *b;
 char item[100], *ptr=item;

 if (!DTMD_ISTEXT(f->ed->f[f->ed->mxedt]->dtmd))
  return(0);
 b = f->ed->f[f->ed->mxedt]->b;
 y=b->b.y;
 x=b->b.x;
 s=b->bf[y].s;
 if(!isalnum(s[x]) && s[x]!='_')
  return (0);
 for(;x>=0 && (isalnum(s[x])||s[x]=='_');x--);
 if(x<0 && !isalnum(s[0]) && s[0]!='_')
  return(0);
 x++;
 for(; x<=b->bf[y].len && (isalnum(s[x])||s[x]=='_') &&
      ptr < item + sizeof(item) - 1; x++,ptr++)
  *ptr=s[x];
 *ptr=0; 
 e_ed_man(item, f);
 return(0); 
}

