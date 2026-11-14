/* we_fl_unix.c                                           */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include "we_ai.h"
#include "we_buffer.h"
#include "we_lineend.h"
#include "we_rowfind.h"
#include "we_child.h"
#include "we_startcmd.h"
#include "we_startplan.h"

#ifdef UNIX

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

struct dirfile *e_make_win_list(FENSTER * f);
extern char    *e_tmp_dir;

#ifdef NOSYMLINKS
#define readlink(x, y, z) -1
#define WpeRenameLink(x, y, z, f) 0
#define WpeLinkFile(x, y, sw, f) link(x, y)
#define lstat(x,y)  stat(x,y)
#undef S_ISLNK
#define S_ISLNK(x)  0
#else
#include <unistd.h>
#endif

/* buffer size for copying */
#define E_C_BUFFERSIZE 524288        /*   1/2  Mega Byte   */

int e_select_project(ECNT *cn, char *path);   /* we_prog.c */
int e_create_project(ECNT *cn, char *path);   /* we_prog.c */
int e_wrt_prj_fl(FENSTER *f);                  /* we_prog.c */

#ifdef DEBUG
int SpecialError(char *text, int sw, FARBE *f, char *file, int line)
{
  fprintf(stderr, "\nFile \"%s\" line %d\n", file, line);
  return e_error(text, sw, f);
}

#define e_error(text, sw, f) SpecialError(text, sw, f, __FILE__, __LINE__)
#endif


/* setup the file-manager structures 
   'dirct' or the current dir. is used to 
   search for directories and files */
/* sw | file manager type
   ------------------------
   0  | open/new file
   1  | read block of text from file
   2  | write block of text to file
   3  | execute file
   4  | save as
   5  | add file to project
   6  | wastebasket

*/

/* e_fm_centered_geometry - choose the File Manager box size + position for the
   current terminal.  It grows to about three-quarters of the screen, clamped to
   a sane minimum (the classic ~56x21) and maximum, then centres it.  So on a big
   terminal the dir-tree and file-list boxes get many more rows and a bit more
   width, instead of a fixed small box pinned near the top-left; on an 80x24
   terminal it stays about its historical size.  Every inner widget already
   derives from f->a/f->e (NUM_COLS/LINES_ON_SCREEN are window-relative), so this
   is the one place the size needs choosing -- the rest scales for free. */
static void e_fm_centered_geometry(FENSTER *f)
{
  int w = MAXSCOL * 3 / 4;          /* width  as an (e.x - a.x) offset */
  int h = MAXSLNS * 3 / 4;          /* height as an (e.y - a.y) offset */
  int ax, ay;

  if(w < 55)       w = 55;          /* never smaller than the old box   */
  else if(w > 77)  w = 77;          /* ...nor stretched edge to edge     */
  if(h < 20)       h = 20;
  else if(h > 29)  h = 29;          /* cap height -> ~20 rows of listing */

  ax = (MAXSCOL - w) / 2;  if(ax < 1) ax = 1;
  ay = (MAXSLNS - h) / 2;  if(ay < 1) ay = 1;
  f->a = e_set_pnt(ax, ay);
  f->e = e_set_pnt(ax + w, ay + h);
}

/* The frame title of the next Save As dialog, set by WpeSaveAsManagerTitled
   and taken (then cleared) by WpeCreateFileManager; NULL: "File-Manager". */
static char *WpeSaveAsTitleNext = NULL;

static int e_execute_file(const char *name);

int WpeCreateFileManager(int sw, ECNT *cn, char *dirct)
{
  extern char    *e_hlp_str[];
  extern WOPT    *fblst, *rblst, *wblst, *xblst, *sblst, *ablst;
  FENSTER        *f;
  int             i, j;
  FLBFFR         *b;
  int             allocate_size;  /* inital memory size for allocation */
  char           *sfile;

  /* check whether we reached the maximum number of windows */
  if(cn->mxedt >= MAXEDT)
  {
    e_error(e_msg[ERR_MAXWINS], 0, cn->fb);
    return(-1);
  }

  /* search for a not used window ID number (j) */
  for(j = 1; j <= MAXEDT; j++)
  {
    for(i = 1; i <= cn->mxedt && cn->edt[i] != j; i++)
      ;
    if(i > cn->mxedt)
      break;
  }

  /* change the shape of the mouse :-) */
  WpeMouseChangeShape(WpeWorkingShape);

  /* currently active window, 
     one more window in the system, 
     its number is j */
  cn->curedt = j;
  (cn->mxedt)++;
  cn->edt[cn->mxedt] = j;

  /* allocate window structure */
  if((f = (FENSTER *)MALLOC(sizeof(FENSTER))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

  /* allocate buffer related to the window (NOT proper type, later casted) */
  if((b = (FLBFFR *) MALLOC(sizeof(FLBFFR))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

  f->fb = cn->fb;
  cn->f[cn->mxedt] = f;        /* store the window structure at appropriate place */
  e_fm_centered_geometry(f);   /* centred, grows with the terminal (capped) */
  f->winnum = cn->curedt;
  f->dtmd = DTMD_FILEMANAGER;
  f->ins = 1;
  f->save = 0;
  f->zoom = 0;
  f->ed = cn;
  f->c_sw = NULL;
  f->c_st = NULL;
  f->pic = NULL;
  if(sw == 6)
  {
    sw = 0;
    f->datnam = "Wastebasket";
    f->save = 1;
  }
  else if(sw == 5)
    f->datnam = "Add Files to Project";  /* add-to-project mode */
  else
    f->datnam = "File-Manager";  /* window header text */
  /* a Save As for something that is not a file says what it saves */
  if(sw == 4 && WpeSaveAsTitleNext)
    f->datnam = WpeSaveAsTitleNext;
  WpeSaveAsTitleNext = NULL;

  /* status line text for different mode */
  if(sw == 0)
  {
    f->blst = fblst;
    f->nblst = 8;
  }
  else if(sw == 1)
  {
    f->blst = rblst;
    f->nblst = 4;
  }
  else if(sw == 2)
  {
    f->blst = wblst;
    f->nblst = 4;
  }
  else if(sw == 3)
  {
    f->blst = xblst;
    f->nblst = 4;
  }
  else if(sw == 4)
  {
    f->blst = sblst;
    f->nblst = 5;
  }
  else if(sw == 5)
  {
    f->blst = ablst;
    f->nblst = 4;
  }

  if(sw == 3)
    f->hlp_str = e_hlp_str[5];
  else
    f->hlp_str = e_hlp_str[4];

  if(!dirct || dirct[0] == '\0')
  {
    /* no working directory has been given */
    f->dirct = WpeGetCurrentDir(cn);
  }
  else
  {
    /* working directory is given, copy it over */
    allocate_size = strlen(dirct);
    if((f->dirct = MALLOC(allocate_size+1)) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

    strcpy(f->dirct, dirct);
  }

  strcpy(f->fd.search, "");
  strcpy(f->fd.replace, "");
  strcpy(f->fd.file, SUDIR);
  f->fd.dirct = WpeStrdup(f->dirct);

  f->fd.sw = 16;
  f->fd.sn = 0;
  f->fd.rn = 0;

  f->b = (BUFFER *)b;
  /* the find pattern can only be 79 see FIND structure */
  if((b->rdfile = MALLOC(80)) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
  strcpy(b->rdfile, f->fd.file);               /* find file pattern */

  b->sw = sw;

  /* Open-Project mode: filter to *.prj and tag this manager so that
     selecting a file opens it as the project (see WpeHandleFileManager). */
  b->prj_sel = FM_PRJ_NONE;
  {
    extern int e_prj_select_pending, e_prj_new_pending;
    if (sw == 0 && e_prj_new_pending)
    {
      strcpy(b->rdfile, "*.prj");
      b->prj_sel = FM_PRJ_NEW;
      e_prj_new_pending = 0;
      f->datnam = "New Project";
    }
    else if (sw == 0 && e_prj_select_pending)
    {
      strcpy(b->rdfile, "*.prj");
      b->prj_sel = FM_PRJ_OPEN;
      e_prj_select_pending = 0;
      f->datnam = "Select Project";
    }
  }

  /* window for files */
  if((b->fw = (FLWND *) MALLOC(sizeof(FLWND))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

  /* window for directory */
  if((b->dw = (FLWND *) MALLOC(sizeof(FLWND))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, cn->fb);


  if((sfile = MALLOC(strlen(f->dirct)+strlen(b->rdfile)+2)) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

  /* determine current directory */
  b->cd = WpeCreateWorkingDirTree(f->save, cn);
  /* it is necessary to do this, because the file manager may not be
     in the appropriate directory here */
  sprintf(sfile, "%s/%s", f->dirct, SUDIR);
  /* find all other directories in the current */
  b->dd = e_find_dir(sfile, f->ed->flopt & FM_SHOW_HIDDEN_DIRS ? 1 : 0);
  /* setup the drawing in the dir tree window */
  b->dw->df = WpeGraphicalDirTree(b->cd, b->dd, cn);

  i = f->ed->flopt & FM_SHOW_HIDDEN_FILES ? 1 : 0;
  if(sw == 3)
    i |= 2;

  /* finds all files matching the pattern */
  sprintf(sfile, "%s/%s", f->dirct, b->rdfile);
  b->df = e_find_files(sfile, i);

  FREE(sfile);

  /* setup the drawing in the file list window */
  b->fw->df = WpeGraphicalFileList(b->df, f->ed->flopt >> 9, cn);

  /* file box - geometry and vertical slider settings */
  b->fw->mxa = f->a.x;
  b->fw->mxe = f->e.x;
  b->fw->mya = f->a.y;
  b->fw->mye = f->e.y;
  b->fw->xa = f->e.x - 33;
  b->fw->xe = f->e.x - 17;
  b->fw->ya = f->a.y + 6;
  b->fw->ye = f->a.y + 17;
  b->fw->f = f;
  b->fw->ia = b->fw->nf = b->fw->nxfo = b->fw->nyfo = 0;
  b->fw->srcha = b->fw->ja = 12;

  /* directory box - geometry and vertical slider settings */
  b->dw->mxa = f->a.x;
  b->dw->mxe = f->e.x;
  b->dw->mya = f->a.y;
  b->dw->mye = f->e.y;
  b->dw->xa = f->a.x + 3;
  b->dw->xe = f->a.x + 28;
  b->dw->ya = f->a.y + 6;
  b->dw->ye = f->a.y + 17;
  b->dw->f = f;
  b->dw->ia = b->dw->ja = b->dw->nxfo = 0;
  b->dw->srcha = -1;
  b->dw->nf = b->dw->nyfo = b->cd->anz - 1;

  if(cn->mxedt > 1)
    e_ed_rahmen(cn->f[cn->mxedt - 1], 0);

  e_firstl(f, 1);

  /* basically it draws the window out */
  WpeDrawFileManager(f);

  /* restore the shape of the mouse */
  WpeMouseRestoreShape();
  return(0);
}

/* Draw one bottom-row key hint (e.g. "Esc:Cancel") in the File Manager's
   plain hint colour. Hides the e_pr_str colour boilerplate so the call sites
   read as "put this hint here". */
static void e_fm_key_hint(FENSTER *f, int x, int y, char *text)
{
  int col = f->fb->nt.fb;

  e_pr_str(x, y, text, col, -1, -1, col, col);
}

/* drawing out the file-manager,
   first buttons, than the dir tree and file list */
int WpeDrawFileManager(FENSTER * f)
{
  FLBFFR         *b = (FLBFFR *)f->b;
  int             i, j;
  int             bx1 = 1, bx2 = 1, bx3 = 1, by = 4;

  for(j = f->a.y + 1; j < f->e.y; j++)
    for(i = f->a.x + 1; i < f->e.x; i++)
      e_pr_char(i, j, ' ', f->fb->nt.fb);

  if(NUM_LINES_ON_SCREEN <= 17)
    by = -1;
  else if(b->sw != 0 || NUM_LINES_ON_SCREEN <= 19)
    by = 2;

  if(NUM_LINES_ON_SCREEN > 17)
  {
    /* For Add-to-project, Esc keeps the added files and returns to the
       project window, so label it Done rather than Cancel. */
    if(b->sw == 5)
      e_fm_key_hint(f, f->a.x + 4, f->e.y - by, "Esc:Done");
    else
      e_fm_key_hint(f, f->a.x + 4, f->e.y - by, "Esc:Cancel");
    e_fm_key_hint(f, f->a.x + 17, f->e.y - by, "Alt-C:ChDir");

    if(b->sw == 1 && NUM_COLS_ON_SCREEN >= 34)
      e_fm_key_hint(f, f->a.x + 31, f->e.y - by, "Alt-R:Read");
    else if(b->sw == 2 && NUM_COLS_ON_SCREEN >= 35)
      e_fm_key_hint(f, f->a.x + 31, f->e.y - by, "Alt-W:Write");
    else if(b->sw == 4 && NUM_COLS_ON_SCREEN >= 34)
      e_fm_key_hint(f, f->a.x + 31, f->e.y - by, "Alt-S:Save");
    else if(b->sw == 3 && NUM_COLS_ON_SCREEN >= 37)
      e_fm_key_hint(f, f->a.x + 31, f->e.y - by, "Alt-E:Exec");
    else if(b->sw == 5 && NUM_COLS_ON_SCREEN >= 33)
      e_fm_key_hint(f, f->a.x + 31, f->e.y - by, "Enter:Add");
    else if(b->sw == 0 && b->prj_sel)
    {
      if(NUM_COLS_ON_SCREEN >= 33)
      {
        if(b->prj_sel == FM_PRJ_NEW)
          /* New Project: Enter on the typed name creates the project. */
          e_fm_key_hint(f, f->a.x + 31, f->e.y - by, "Enter:Create");
        else
          /* Open Project: Enter moves into the file list to pick a .prj. */
          e_fm_key_hint(f, f->a.x + 31, f->e.y - by, "Enter:Select File");
      }
    }
    else if(b->sw == 0)
    {
      if(NUM_COLS_ON_SCREEN >= 35)
        e_pr_str((f->a.x + 28), f->e.y - by, "MKdir", f->fb->nz.fb, 1, -1,
                 f->fb->ns.fb, f->fb->nt.fb);
      if(NUM_COLS_ON_SCREEN >= 49)
        e_pr_str((f->a.x + 37), f->e.y - by, "Attributes", f->fb->nz.fb, 0, -1,
                 f->fb->ns.fb, f->fb->nt.fb);
    }
  }
  if(b->sw == 0 && !b->prj_sel && NUM_LINES_ON_SCREEN > 19)
  {
    e_pr_str((f->a.x + 4), f->e.y - 2, "Move", f->fb->nz.fb, 0, -1,
             f->fb->ns.fb, f->fb->nt.fb);

    if(NUM_COLS_ON_SCREEN >= 21)
      e_pr_str((f->a.x + 13), f->e.y - 2, "Remove", f->fb->nz.fb, 0, -1,
               f->fb->ns.fb, f->fb->nt.fb);

    if(NUM_COLS_ON_SCREEN >= 30)
      e_pr_str((f->a.x + 24), f->e.y - 2, "Link", f->fb->nz.fb, 0, -1,
               f->fb->ns.fb, f->fb->nt.fb);

    if(NUM_COLS_ON_SCREEN >= 39)
      e_pr_str((f->a.x + 33), f->e.y - 2, "COpy", f->fb->nz.fb, 1, -1,
               f->fb->ns.fb, f->fb->nt.fb);

    if(NUM_COLS_ON_SCREEN >= 48)
      e_pr_str((f->a.x + 42), f->e.y - 2, "Edit", f->fb->nz.fb, 0, -1,
               f->fb->ns.fb, f->fb->nt.fb);

  }
  if(NUM_COLS_ON_SCREEN < 45)
    bx3 = 0;
  if(NUM_COLS_ON_SCREEN < 44)
    bx2 = 0;
  if(NUM_COLS_ON_SCREEN < 43)
    bx1 = 0;
  b->xfd = (NUM_COLS_ON_SCREEN - bx1 - bx2 - bx3 - 6) / 2;
  b->xdd = NUM_COLS_ON_SCREEN - bx1 - bx2 - bx3 - b->xfd - 6;
  b->xda = 2 + bx1;
  b->xfa = 4 + bx1 + bx2 + b->xdd;

  e_pr_str((f->a.x + b->xfa), f->a.y + 2, "Name:", f->fb->nt.fb, 0, 1,
           f->fb->nsnt.fb, f->fb->nt.fb);
/*    e_schr_nchar(b->rdfile, f->a.x+b->xfa, f->a.y+3, 0, b->xfd+1, f->fb->fr.fb);   */
  e_schr_nchar_wsv(b->rdfile, f->a.x + b->xfa, f->a.y + 3, 0, b->xfd + 1,
                   f->fb->fr.fb, f->fb->fz.fb);
  e_pr_str((f->a.x + b->xfa), f->a.y + 5, "Files:", f->fb->nt.fb, 0, 1,
           f->fb->nsnt.fb, f->fb->nt.fb);

  e_pr_str((f->a.x + b->xda), f->a.y + 2, "Directory:", f->fb->nt.fb, 0, 1,
           f->fb->nsnt.fb, f->fb->nt.fb);
/*    e_schr_nchar(f->dirct, f->a.x+b->xda, f->a.y+3, 0, b->xdd+1, f->fb->fr.fb);   */
  e_schr_nchar_wsv(f->dirct, f->a.x + b->xda, f->a.y + 3, 0, b->xdd + 1,
                   f->fb->fr.fb, f->fb->fz.fb);
  e_pr_str((f->a.x + b->xda), f->a.y + 5, "DirTree:", f->fb->nt.fb, 3, 1,
           f->fb->nsnt.fb, f->fb->nt.fb);

  b->fw->mxa = f->a.x;
  b->fw->mxe = f->e.x;
  b->fw->mya = f->a.y;
  b->fw->mye = f->e.y;
  b->fw->xa = f->a.x + b->xfa;
  b->fw->xe = b->fw->xa + b->xfd;
  b->fw->ya = f->a.y + 6;
  b->fw->ye = f->e.y - 2 - by;
  b->dw->mxa = f->a.x;
  b->dw->mxe = f->e.x;
  b->dw->mya = f->a.y;
  b->dw->mye = f->e.y;
  b->dw->xa = f->a.x + b->xda;
  b->dw->xe = b->dw->xa + b->xdd;
  b->dw->ya = f->a.y + 6;
  b->dw->ye = f->e.y - 2 - by;

  /* slider bars for file list */
  e_mouse_bar(b->fw->xe, b->fw->ya, b->fw->ye - b->fw->ya, 0, b->fw->f->fb->em.fb);
  e_mouse_bar(b->fw->xa, b->fw->ye, b->fw->xe - b->fw->xa, 1, b->fw->f->fb->em.fb);
  /* file list window */
  e_pr_file_window(b->fw, 0, 1, f->fb->ft.fb, f->fb->fz.fb, f->fb->frft.fb);

  /* slide bars for directory window */
  e_mouse_bar(b->dw->xe, b->dw->ya, b->dw->ye - b->dw->ya, 0, b->dw->f->fb->em.fb);
  e_mouse_bar(b->dw->xa, b->dw->ye, b->dw->xe - b->dw->xa, 1, b->dw->f->fb->em.fb);
  /* directory window */
  e_pr_file_window(b->dw, 0, 1, f->fb->ft.fb, f->fb->fz.fb, f->fb->frft.fb);
  return(0);
}

/* tries to find the required style file manager */
int WpeCallFileManager(int sw, FENSTER * f)
{
  int             i, ret;
  FLBFFR         *b;

  for(i = f->ed->mxedt; i > 0; i--)
    if(f->ed->f[i]->dtmd == DTMD_FILEMANAGER)     /* check only file manager windows */
    {
      b = (FLBFFR *)f->ed->f[i]->b;
      /* open/new file manager and it is not in save mode */
      if(sw == 0 && b->sw == sw && f->ed->f[i]->save != 1)
        break;
      /* wastebasket mode required, 
         the window is "open/new" file manager and save mode turned on */
      else if(sw == 6 && b->sw == 0 && f->ed->f[i]->save == 1)
        break;
      /* not open/new or wastebasket filemanager and it is the required style */
      else if(sw != 0 && sw != 6 && b->sw == sw)
        break;
    }

  if(i <= 0) /* we did not find the required style file-manager */
  {
    if(sw == 6) /* wastebasket mode */
    {
      char *tmp;

      if ((tmp = WpeGetWastefile("")))
      {
        ret = WpeCreateFileManager(sw, f->ed, tmp);
        FREE(tmp);
        return(ret);
      }
      else
      {
        e_error(e_msg[ERR_NOWASTE], 0, f->ed->fb);
        return 0;                /* Error -- no wastebasket */
      }
    }
    /* create the required style file manager */
    return(WpeCreateFileManager(sw, f->ed, ""));
  }
  /* switch to the found file manager */
  e_switch_window(f->ed->edt[i], f);
  return(0);
}

/* It will always create a new file manager */
int WpeManagerFirst(FENSTER * f)
{
  return(WpeCreateFileManager(0, f->ed, ""));
}

/* try to find an "open/new" style file manager or create one */
int WpeManager(FENSTER * f)
{
  return(WpeCallFileManager(0, f));
}

/* try to find an "execute" style file manager or create one */
int WpeExecuteManager(FENSTER * f)
{
  return(WpeCallFileManager(3, f));
}

/* try to find a "save as" style file manager or create one */
int WpeSaveAsManager(FENSTER * f)
{
#ifdef WPE_AI
  /* the AI pane saves its transcript: the same dialog, under its own title */
  if(wpe_ai_is_pane(f))
    return(wpe_ai_pane_save_as(f));
#endif
  return(WpeCreateFileManager(4, f->ed, ""));
}

/**
 * WpeSaveAsManagerTitled - Open the Save As dialog under the frame title @title.
 * @f: the window whose content is to be saved; @title: a string that outlives
 *     the dialog (a literal).
 *
 * A window that is not a plain file (the assistant's transcript) is saved
 * through the same dialog the user knows, and the title says what is being
 * saved.  Return: what WpeCreateFileManager returned.
 */
int WpeSaveAsManagerTitled(FENSTER * f, char *title)
{
  WpeSaveAsTitleNext = title;
  return(WpeCreateFileManager(4, f->ed, ""));
}

/**
 * WpeSaveAsIsTranscript - Is this Save As confirmation for the AI pane?
 * @b: the file manager's state; @fe: the window being saved.
 *
 * The pane is read-only and backs no file, so the dialog writes its
 * transcript instead of refusing the name in silence.  Always 0 in a build
 * without the assistant.
 */
static int WpeSaveAsIsTranscript(FLBFFR *b, FENSTER *fe)
{
#ifdef WPE_AI
  return(b->sw == 4 && fe && wpe_ai_is_pane(fe));
#else
  return(0);
#endif
}

/**
 * WpeFileManagerReturnHome - Put the editor back in the directory it was in.
 * @cn: the desktop; @svmode: the file-list options to restore, or -1;
 * @svdir: the saved directory (freed and cleared), or NULL.
 *
 * The file manager walks the directory tree with chdir; once it closes, the
 * user's relative paths must mean what they meant before it opened.
 */
static void WpeFileManagerReturnHome(ECNT *cn, int svmode, char **svdir)
{
  char *dirtmp;

  if(svmode >= 0)
    cn->flopt = svmode;
  if(*svdir == NULL)
    return;
  if(chdir(*svdir))
  {
    e_error(e_msg[ERR_WORKDIRACCESS], 0, cn->fb);
    /* we cannot determine where we are, try the home */
    if((dirtmp = getenv("HOME")) == NULL)
      e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
    if(chdir(dirtmp))
      e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
  }
  dirtmp = WpeGetCurrentDir(cn);
  FREE(cn->dirct);
  cn->dirct = dirtmp;
  FREE(*svdir);
  *svdir = NULL;
}

/**
 * WpeSaveAsTranscript - Write the AI pane's transcript under @filen and close.
 * @f: the file manager; @fe: the pane; @svmode, @svdir: what to restore.
 *
 * The transcript is written while the dialog's directory is still the
 * current one, so a relative name lands where the user was looking; the
 * dialog then closes and the status row says how the save went.
 */
static int WpeSaveAsTranscript(FENSTER *f, FENSTER *fe, char *filen,
                               int svmode, char **svdir)
{
  ECNT *cn = f->ed;

#ifdef WPE_AI
  wpe_ai_pane_write_transcript(fe, filen);
#endif
  WpeFileManagerReturnHome(cn, svmode, svdir);
  e_close_window(f);
#ifdef WPE_AI
  wpe_ai_pane_report_save(fe);
#endif
  return(0);
}

int e_prj_add_file_unique(FLWND *fw, char *ftmp, FENSTER *f)
{
  int i, row;
  char msg[256];

  for (i = 0; i < wpe_listsel_count(fw->df, 1); i++)
  {
    if (strcmp(wpe_listsel_entry(fw->df, i, 1), ftmp) == 0)
    {
      e_error("File already in project", 0, f->fb);
      FREE(ftmp);
      return -1;
    }
  }
  row = wpe_listsel_insert(fw->df, fw->nf, ftmp);
  if (row < 0)
  {
    FREE(ftmp);
    return -1;
  }
  fw->nf = row + 1;
  if (fw->nf - fw->ia >= fw->ye - fw->ya)
    fw->ia = fw->nf + fw->ya - fw->ye + 1;
  /* Persist immediately (before the notification, which is modal) so the
     on-disk .prj never drifts from the project window (#137: save on each
     Add, not only on window close). */
  e_wrt_prj_fl(f);
  snprintf(msg, sizeof(msg), "Added to project: %s", ftmp);
  e_message(0, msg, f);
  FREE(ftmp);   /* the list holds its own copy */
  return 0;
}

/**
 * WpeFileNameTake - Keep the name of the file picked in the manager.
 * @f: the manager's window; @filen: a buffer of WPE_NAME_SIZE bytes;
 * @name: the picked entry or typed name.
 *
 * A name the file system gave (readdir) is at most WPE_NAME_MAX bytes, so it
 * always fits and a file with a long name can be opened, copied or added to a
 * project like any other.  A name that does not fit is never cut (a cut name
 * is another file): the user is told and the buffer is left empty.
 */
static void WpeFileNameTake(FENSTER *f, char *filen, const char *name)
{
  char msg[WPE_PATH_MSG_MAX];

  if(wpe_path_copy(filen, WPE_NAME_SIZE, name) == 0)
    return;
  snprintf(msg, sizeof msg, WPE_PATH_TOO_LONG_FMT, "file name", WPE_NAME_MAX);
  e_error(msg, 0, f->fb);
}

/**
 * e_is_list_close_key - Is @c the key that closes a file manager list?
 * @f: the list's window.
 *
 * The key is the one the editor's key style gives the lists, so a user of
 * either style closes a Window list, a project list or the project add
 * manager with the key the bottom bar names.
 */
static int e_is_list_close_key(FENSTER *f, int c)
{
  return(wpe_binding_action(c, wpe_binding_style(f->ed->edopt), WPE_CTX_LIST)
         == WPE_ACT_CLOSE_LIST);
}

/**
 * e_list_close_key - The key code that closes a file manager list, as if
 * it had been pressed.
 * @f: the list's window.
 */
static int e_list_close_key(FENSTER *f)
{
  return(wpe_binding_first_code(WPE_ACT_CLOSE_LIST,
                                wpe_binding_style(f->ed->edopt), WPE_CTX_LIST));
}

/**
 * WpeProjectPathOf - The path of the project picked or named in the manager.
 * @f: the manager's window; @out/@n: receive it; @name: what the Name field
 * holds, a full path or a name in the window's folder.
 *
 * Open Project reads and New Project writes this path, so it is whole or it
 * is refused: a cut path would open "not found" or write a project into a
 * folder the user never named.  The refusal is said here, once.
 * Return: 0, or -1 when the path does not fit.
 */
static int WpeProjectPathOf(FENSTER *f, char *out, size_t n, const char *name)
{
  char msg[WPE_PATH_MSG_MAX];
  int  refused;

  if(name[0] == DIRC)
    refused = wpe_path_copy(out, n, name);
  else
    refused = wpe_path_join(out, n, f->dirct, name);
  if(refused == 0)
    return(0);
  snprintf(msg, sizeof msg, WPE_PATH_TOO_LONG_FMT, "project", WPE_PATH_MAX);
  e_error(msg, 0, f->fb);
  return(-1);
}

/**
 * WpeTargetNameNew - The name Move, Copy and Link offer for the user to edit.
 * @f: the file manager's window; @name: the picked file or folder.
 *
 * The user types the new name over the old one, so the field holds the whole
 * name (it never starts as a prefix that Enter would then rename the file
 * to) and has room for a path such as "../x.c".  The buffer is a path's size
 * because the field takes WPE_PATH_FIELD_MAX bytes.
 * Return: the buffer, to FREE; NULL when there is no memory or the name does
 * not fit (the user is told).
 */
static char *WpeTargetNameNew(FENSTER *f, const char *name)
{
  char msg[WPE_PATH_MSG_MAX];
  char *buf = MALLOC(WPE_PATH_SIZE);

  if(buf == NULL)
  {
    e_error(e_msg[ERR_LOWMEM], 0, f->fb);
    return(NULL);
  }
  if(wpe_path_copy(buf, WPE_PATH_SIZE, name) == 0)
    return(buf);
  FREE(buf);
  snprintf(msg, sizeof msg, WPE_PATH_TOO_LONG_FMT, "file name", WPE_PATH_MAX);
  e_error(msg, 0, f->fb);
  return(NULL);
}

/**
 * e_say_nul_read - Say on the status row that a file read into the text
 * (Block > Read) lost NUL bytes, when it did.
 * @f: the window the file went into; @info: what the read found.
 *
 * The inserted text is the file without them, so the user knows the bytes
 * are not in the window.  The file on disk is not touched.
 */
static void e_say_nul_read(FENSTER *f, const WpeEolInfo *info)
{
  char line[E_NUL_SAY_MAX];

  if (info->nul <= 0)
    return;
  e_eol_nul_sentence(info->nul, line, sizeof line);
  e_status_say_until_key(f, line);
}

/**
 * WpeSaveAsSettleName - After Save As tried to write the file under its new
 * name, keep the new name only when the file was written.
 * @fe: the window; @old_dir, @old_name: its name before (taken over here);
 * @saved: the status the write returned.
 *
 * Answering No to the question before the write, or a write that fails,
 * leaves the window under the name it had, so its next Save does not go to a
 * file the user never wrote.
 */
static void WpeSaveAsSettleName(FENSTER *fe, char *old_dir, char *old_name, int saved)
{
  if(saved == WPE_WRITE_WRITTEN)
  {
    FREE(old_dir);
    FREE(old_name);
    return;
  }
  FREE(fe->dirct);
  FREE(fe->datnam);
  fe->dirct = old_dir;
  fe->datnam = old_name;
}

/**
 * e_fm_tree_subdir - The subdirectory highlighted in the file manager's tree.
 * @b: the file manager's buffer.
 *
 * Move, Copy, Remove and the drag of a folder act on the highlighted
 * subdirectory; on one of the ancestor rows above them there is nothing to
 * act on, and none of them may start.
 *
 * Return: the subdirectory's name, or NULL for an ancestor row.
 */
char *e_fm_tree_subdir(const FLBFFR *b)
{
  return(wpe_listsel_tree_subdir(b->cd, b->dd, b->dw->nf));
}

/**
 * WpeWriteBlockToFile - Write the marked block to the file the user named.
 * @fe: the window the block is marked in; @se: its screen, with the marks;
 * @filen: the name typed in the file manager.
 *
 * The window keeps its own name and its text.  A write that fails says why
 * and where, so the user knows the file is not there.
 */
static void WpeWriteBlockToFile(FENSTER *fe, SCHIRM *se, const char *filen)
{
  WpeWriteOutcome  o;
  char            *dir = NULL, *name = NULL, *path;

  WpeFilenameToPathFile((char *)filen, &dir, &name);
  path = (dir && name) ? e_mkfilename(dir, name) : NULL;
  FREE(dir);
  FREE(name);
  if(path == NULL)
    return;
  if(wpe_write_range(fe, path, se->mark_begin.x, se->mark_begin.y,
                     se->mark_end.x, se->mark_end.y, 1, &o) != 0)
    wpe_write_say(fe, &o, WPE_WRITE_THEN_KEPT);
  FREE(path);
}

/* File Manager Handler */
int WpeHandleFileManager(ECNT * cn)
{
  FENSTER        *f = cn->f[cn->mxedt], *fe = NULL;
  WpeEolInfo      read_info;
  FLBFFR         *b = (FLBFFR *) f->b;
  BUFFER         *be = NULL;
  SCHIRM         *se = NULL;
  int             c = AltC, i, j;
  char           *subdir;          /* the directory tree's selected subdirectory */
  int             winnum = 0, nco, svmode = -1, fmode, len;
  POINT           mark_end;
  int             g[4], cold = AltN;
  char            filen[WPE_NAME_SIZE], *ftmp, *dtp = NULL, *ftp = NULL, *svdir = NULL;
  char           *dirtmp = NULL;
  PIC            *outp = NULL;
  FILE           *fp;
  struct stat     buf;
  int             open_prj = 0;      /* 1 = open selected .prj after FM closes */
  int             new_prj = 0;        /* 1 = create (New Project) vs open existing */
  char            prj_path[WPE_PATH_SIZE];

  /* check whether we really get a file-manager window here */
  if(f->dtmd != DTMD_FILEMANAGER)
    return(0);

  if(f->save == 1)
  {
    svmode = f->ed->flopt;
    f->ed->flopt = FM_SHOW_HIDDEN_FILES | FM_SHOW_HIDDEN_DIRS |
      FM_MOVE_OVERWRITE | FM_REKURSIVE_ACTIONS;
  }

  /* if it is project management or saving mode 
     save the current directory to return to it */
  if(f->save == 1 || b->sw == 5)
  {
    if((svdir = MALLOC(strlen(f->ed->dirct) + 1)) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
    strcpy(svdir, f->ed->dirct);
  }

  nco = b->cd->anz - 1;
  /* when searching among files, search hidden ones as well */
  fmode = f->ed->flopt & FM_SHOW_HIDDEN_FILES ? 1 : 0;
  /* in execution mode show hidden dirs as well */
  if(b->sw == 3)
    fmode |= 2;

  /* searching for the last edited/touched file on the desktop */
  for(i = cn->mxedt; i > 0; i--)
  {
    if (DTMD_ISTEXT(cn->f[i]->dtmd))
    {
      fe = cn->f[i];
      be = fe->b;
      se = fe->s;
      winnum = cn->edt[i];
      break;
    }
  }
  strcpy(f->fd.file, b->rdfile);

  /* go until quit */
  while(c != WPE_ESC)
  {
    /* draw out dir tree and file list windows */
    e_pr_file_window(b->fw, 0, 1, f->fb->ft.fb, f->fb->fz.fb, f->fb->frft.fb);
    e_pr_file_window(b->dw, 0, 1, f->fb->ft.fb, f->fb->fz.fb, f->fb->frft.fb);

    switch(c)
    {
      /* filename entry box activation */
      case AltN:
        cold = c;
        fk_cursor(1);
        /* get some answer from the name entry box, 
           result file copied into b->rdfile, max 79 char + '\0' */
        c = e_schr_lst_wsv(b->rdfile, f->a.x + b->xfa, f->a.y + 3, b->xfd + 1, 79,
                           f->fb->fr.fb, f->fb->fz.fb, &f->ed->fdf, f);

        /* determine the entered filename, going backward */
        for(i = strlen(b->rdfile); i >= 0 && b->rdfile[i] != DIRC; i--)
          ;
        strcpy(f->fd.file, b->rdfile + 1 + i);

        /* there is some directory structure in the filename */
        if(i >= 0)
        {
          if(i == 0)
            i++;
          b->rdfile[i] = '\0';
          /* change the working directory */
          FREE(f->dirct);
          if((f->dirct = MALLOC(strlen(b->rdfile) + 1)) == NULL)
            e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
          strcpy(f->dirct, b->rdfile);

          /* restore original filename */
          strcpy(b->rdfile, f->fd.file);
          c = AltC;
        }
#if  MOUSE
        /* if mouse was used (the reason it returned) get appropriate key interpretation */
        if(c == -1)
          c = WpeMngMouseInFileManager(f);
#endif
        if((c >= Alt1 && c <= Alt9) || (c >= 1024 && c <= 1049))
        {
          /* window changing, make the entry unhighlighted */
          e_schr_nchar_wsv(b->rdfile, f->a.x + b->xfa, f->a.y + 3, 0, b->xfd + 1,
                           f->fb->fr.fb, f->fb->fz.fb);
          break;
        }
        if(c == CLE || c == CCLE) /* goto dir name window */
          c = AltD;
        else if(c == CDO || c == BDO || c == WPE_TAB) /* goto file list window */
          c = AltF;
        else if(c == WPE_BTAB) /* goto dir tree window */
          c = AltT;
        else if(    (   c == WPE_CR 
                     || (b->sw == 0 && c == AltE)
                     || (b->sw == 1 && c == AltR) 
                     || (b->sw == 2 && c == AltW) 
                     || (b->sw == 3 && c == AltE) 
                     || (b->sw == 5 && c == AltA) 
                     || (b->sw == 4 && (c == AltS || c == AltY)) ) 
                 && (   strstr(b->rdfile, "*") 
                     || strstr(b->rdfile, "?")
                     || strstr(b->rdfile, "[") ) )
        {
          WpeMouseChangeShape(WpeWorkingShape);
          /* free up existing structures */
          freedf(b->df);
          freedf(b->fw->df);
          /* find files according to the new pattern */
          b->df = e_find_files(b->rdfile, fmode);
          /* setup the drawing in the dir tree window */
          b->fw->df = WpeGraphicalFileList(b->df, f->ed->flopt >> 9, cn);
          b->fw->ia = b->fw->nf = 0;
          b->fw->ja = b->fw->srcha;
          /* jump to file list window */
          c = AltF;
          WpeMouseRestoreShape();
        }
        else
        {
          WpeFileNameTake(f, filen, b->rdfile);
          if(c == WPE_CR)
          {
            if(b->sw == 1)
              c = AltR;
            else if(b->sw == 2)
              c = AltW;
            else if(b->sw == 4)
              c = AltS;
            else if(b->sw == 5)
              c = AltA;
            else
              c = AltE;
          }
        }
        /* entry window is left, make the entry unhighlighted */
        if(c != AltN)
          e_schr_nchar_wsv(b->rdfile, f->a.x + b->xfa, f->a.y + 3, 0, b->xfd + 1,
                           f->fb->fr.fb, f->fb->fz.fb);
        fk_cursor(0);
        break;

      /* directory name entry box activation */
      case AltD:
        cold = c;
        fk_cursor(1);

        /* get the directory name */
        /* The field takes WPE_PATH_FIELD_MAX bytes and its buffer holds one
           more for the terminator; a window's folder always fits. */
        if ((dirtmp = WpeMalloc(WPE_PATH_SIZE)) == NULL)        /* dirct mat not have enough memory */
          e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

        if (wpe_path_copy(dirtmp, WPE_PATH_SIZE, f->dirct) != 0)
          dirtmp[0] = '\0';

        c = e_schr_lst_wsv(dirtmp, f->a.x + b->xda, f->a.y + 3, b->xdd + 1, WPE_PATH_FIELD_MAX,
                           f->fb->fr.fb, f->fb->fz.fb, &f->ed->ddf, f);
        FREE(f->dirct);
        f->dirct = WpeStrdup(dirtmp);
        WpeFree(dirtmp);

#if  MOUSE
        if(c == -1)
          c = WpeMngMouseInFileManager(f);
#endif
        if(c == CRI || c == CCRI) /* goto name entry windwow */
          c = AltN;
        else if(c == CDO || c == BDO || c == WPE_TAB) /* goto dir tree window */
          c = AltT;
        else if(c == WPE_BTAB) /* goto file list window */
          c = AltF;
        else if(c == WPE_CR) /* change dir */
          c = AltC;
        /* window left, make the entry unhighlighted */
        if(c != AltD)
          e_schr_nchar_wsv(f->dirct, f->a.x + b->xda, f->a.y + 3, 0, b->xdd + 1,
                           f->fb->fr.fb, f->fb->fz.fb);
        fk_cursor(0);
        break;

      /* directory tree list window activation */
      case AltT:
        cold = c;
        c = e_file_window(1, b->dw, f->fb->ft.fb, f->fb->fz.fb);
#if  MOUSE
        if(c == MBKEY)         /* handle mouse actions in the window */
          c = WpeMngMouseInFileManager(f);
        else if(c < 0)
          c = WpeMouseInFileDirList(c, 1, f);
#endif
        if(c == CCRI)
          c = AltF;
        else if(c == BUP)
          c = AltD;
        else if(c == WPE_TAB)
          c = AltN;
        else if(c == WPE_BTAB)
          c = AltD;
        else if(c == AltC || (c == WPE_CR && b->dw->nf != nco))
        {
          if ((dirtmp = WpeAssemblePath(f->dirct, b->cd, b->dd, b->dw->nf, f)))
          {
            FREE(f->dirct);
            f->dirct = dirtmp;
            e_schr_nchar_wsv(f->dirct, f->a.x + b->xda, f->a.y + 3, 0, b->xdd + 1,
                             f->fb->fr.fb, f->fb->fz.fb);
            f->ed->ddf = e_add_df(f->dirct, f->ed->ddf);
            c = AltC;
          }
          /* there is only one case when it cannot assemble the path,
             that it cannot access wastebasket, then quit the file manager */
          else
            c = WPE_ESC;
        }
        else if(c == WPE_CR)
          c = AltT;
        break;

      /* file list window activation */
      case AltF:
        if(b->df->anz < 1)
        {
          c = cold;
          break;
        }
        cold = c;
        c = e_file_window(1, b->fw, f->fb->ft.fb, f->fb->fz.fb);
#if  MOUSE
        if(c == MBKEY)
          c = WpeMngMouseInFileManager(f);
        else if(c < 0)
          c = WpeMouseInFileDirList(c, 0, f);
#endif
        if(c == BUP)  /* goto file name entry window */
          c = AltN;
        else if(c == CCLE) /* goto dir tree window */
          c = AltT;
        else if(c == WPE_TAB) /* goto dir entry window */
          c = AltD;
        else if(c == WPE_BTAB) /* goto file name entry window */
          c = AltN;
        else if(c == WPE_CR)  /* action selected */
        {
          if(b->sw == 1)
            c = AltR;
          else if(b->sw == 2)
            c = AltW;
          else if(b->sw == 4)
            c = AltS;
          else if(b->sw == 5)
            c = AltA;
          else
            c = AltE;
        }
        if(   (b->sw == 1 && c == AltR)  /* in case of action store the filename */
           || (b->sw == 2 && c == AltW)
           || (b->sw == 3 && c == AltE)
           || (b->sw == 0 && c == AltE) 
           || (b->sw == 4 && (c == AltS || c == AltY)) )
        {
          WpeFileNameTake(f, filen, *(b->df->name + b->fw->nf));
        }
        break;

      /* change dir button activation */
      case AltC:
        c = cold;
        /* if the current dir is equal to the "newly" entered dir, break */
        if(!strcmp(f->ed->dirct, f->dirct))
          break;

        /* in wastebasket mode, we do not allow to go out of it through
           a soft link */
        if((b->sw == 0) && (f->save == 1))
        {
          if(lstat(f->dirct, &buf))
          {
            e_error(e_msg[ERR_ACCFILE], 0, f->ed->fb);
            break;
          }
          if(S_ISLNK(buf.st_mode))
          {
            /* cannot go out through a softlink, restore dir name */
            if((f->dirct = REALLOC(f->dirct, strlen(f->ed->dirct) + 1)) == NULL)
              e_error(e_msg[ERR_LOWMEM], 1, f->fb);
            else
              strcpy(f->dirct, f->ed->dirct);
            break;
          }
        }

        /* change to the desired dir with system error checking */
        if(chdir(f->dirct))
        {
          e_error(e_msg[ERR_WORKDIRACCESS], 0, f->fb);

          /* we cannot determine where we are, try the home */
          if((dirtmp = getenv("HOME")) == NULL)
            e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
          if(chdir(dirtmp))
            e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
        }

        /* get current directory */
        dirtmp = WpeGetCurrentDir(f->ed);

        /* change the shape of the mouse */
        WpeMouseChangeShape(WpeWorkingShape);

        FREE(f->dirct);
        f->dirct = dirtmp;

        /* free up all relevant structures */
        freedf(b->df);
        freedf(b->fw->df);
        freedf(b->cd);
        freedf(b->dw->df);
        freedf(b->dd);

        /* reset the current dir path in the control structure */
        if((f->ed->dirct = REALLOC(f->ed->dirct, strlen(f->dirct) + 1)) == NULL)
          e_error(e_msg[ERR_LOWMEM], 1, f->fb);
        else
          strcpy(f->ed->dirct, f->dirct);

        /* setup current directory structure */
        b->cd = WpeCreateWorkingDirTree(f->save, cn);
        /* find all other directories in the current dir */
        b->dd = e_find_dir(SUDIR, f->ed->flopt & FM_SHOW_HIDDEN_DIRS ? 1 : 0);
        /* setup the drawing in the dir tree window */
        b->dw->df = WpeGraphicalDirTree(b->cd, b->dd, cn);

        nco = b->dw->nf = b->cd->anz - 1;
        b->dw->ia = b->dw->ja = 0;

        /* finds all files matching the pattern */
        b->df = e_find_files(b->rdfile, fmode);
        /* setup the drawing in the file list window */
        b->fw->df = WpeGraphicalFileList(b->df, f->ed->flopt >> 9, cn);
        b->fw->nf = b->fw->ia = 0;
        b->fw->ja = 12;

        /* change the shape of the mouse back */
        WpeMouseRestoreShape();
        break;

      /* link file activation */
      case AltL:
      /* copy file activation */
      case AltO:
      /* move file activation */
      case AltM:
        /* moving/copying a file is valid only in mode 0 (open/new file) */
        if(b->sw != 0)
        {
          c = cold;
          break;
        }
        j = c;

        /* we are coming from files */
        if(cold == AltF)
        {
          if((ftmp = WpeTargetNameNew(f, *(b->df->name + b->fw->nf))) == NULL)
          {
            c = cold;
            cold = AltN;
            break;
          }

          /* make the file name editable */
          c = e_schreib_leiste(ftmp, b->fw->xa, b->fw->ya + b->fw->nf - b->fw->ia,
                               b->fw->xe - b->fw->xa, WPE_PATH_FIELD_MAX, f->fb->fr.fb, f->fb->fz.fb);
          if(c == WPE_CR)
          {
            if(j == AltM)
              e_rename(*(b->df->name + b->fw->nf), ftmp, f); /* move */
            else if(j == AltL)
              WpeLinkFile(*(b->df->name + b->fw->nf), ftmp, /* link */
                          f->ed->flopt & FM_TRY_HARDLINK, f);
            else if(j == AltO)
              e_copy(*(b->df->name + b->fw->nf), ftmp, f); /* copy */

            /* after copying/moving/linking, free up the old structures */
            freedf(b->df);
            freedf(b->fw->df);

            /* generate the new file list */
            b->df = e_find_files(b->rdfile, fmode);
            /* setup the drawing of it */
            b->fw->df = WpeGraphicalFileList(b->df, f->ed->flopt >> 9, cn);
            b->fw->ia = b->fw->nf = 0;
            b->fw->ja = b->fw->srcha;
          }
          FREE(ftmp);
        }
        /* we are coming from dirs */
        else if(cold == AltT && (subdir = e_fm_tree_subdir(b)) != NULL)
        {
          /* selected dir: subdir */
          if((ftmp = WpeTargetNameNew(f, subdir)) == NULL)
          {
            c = cold;
            cold = AltN;
            break;
          }

          /* separate the dir name from other drawings in the line */
          for(i = 0; *(b->dw->df->name[b->dw->nf] + i) &&
              (*(b->dw->df->name[b->dw->nf] + i) <= 32 ||
               *(b->dw->df->name[b->dw->nf] + i) >= 127); i++)
            ;

          if(!WpeIsXwin())
            i += 3;
          b->dw->ja = i;
          e_pr_file_window(b->dw, 0, 1, f->fb->ft.fb, f->fb->fz.fb, f->fb->frft.fb);
          /* make the name editable */
          c = e_schreib_leiste(ftmp, b->dw->xa, b->dw->ya + b->dw->nf - b->dw->ia,
                               b->dw->xe - b->dw->xa, WPE_PATH_FIELD_MAX, f->fb->fr.fb, f->fb->fz.fb);
          if(c == WPE_CR)
          {
            if(j == AltM)
              e_rename(subdir, ftmp, f); /* move */
            else if(j == AltL)
              e_link(subdir, ftmp, f); /* link */
            else if(j == AltO)
              e_copy(subdir, ftmp, f); /* copy */

            /* free up structures */
            freedf(b->cd);
            freedf(b->dw->df);
            freedf(b->dd);

            /* determine current directory */
            b->cd = WpeCreateWorkingDirTree(f->save, cn);
            /* find all other directories in the current */
            b->dd = e_find_dir(SUDIR, f->ed->flopt & FM_SHOW_HIDDEN_DIRS ? 1 : 0);
            /* setup drawing */
            b->dw->df = WpeGraphicalDirTree(b->cd, b->dd, cn);
            nco = b->dw->nf = b->cd->anz - 1;
            b->dw->ia = b->dw->ja = 0;
          }
          FREE(ftmp);
        }
        c = cold;
        cold = AltN; /* go back to name entry */
        break;

      /* remove button activation */
      case ENTF:
        if(b->sw != 0)
        {
          c = cold;
          break;
        }
      /* remove button activation */
      case AltR:
        if(b->sw == 0)
        {
          /* coming from file list */
          if(cold == AltF)
          {
            WpeRemove(*(b->df->name + b->fw->nf), f); /* remove the file */

            /* free up structures */
            freedf(b->df);
            freedf(b->fw->df);

            /* find files according to the pattern */
            b->df = e_find_files(b->rdfile, fmode);

            /* setup drawing */
            b->fw->df = WpeGraphicalFileList(b->df, f->ed->flopt >> 9, cn);
            b->fw->ia = b->fw->nf = 0;
            b->fw->ja = b->fw->srcha;
          }
          /* coming from the dir tree list and the selected dir is a subdir of
             the current directory */
          else if(cold == AltT && (subdir = e_fm_tree_subdir(b)) != NULL)
          {
            WpeRemove(subdir, f);  /* remove the dir */

            /* free up structures */
            freedf(b->cd);
            freedf(b->dw->df);
            freedf(b->dd);

            /* determine current directory */
            b->cd = WpeCreateWorkingDirTree(f->save, cn);
            /* find all other directories in the current */
            b->dd = e_find_dir(SUDIR, f->ed->flopt & FM_SHOW_HIDDEN_DIRS ? 1 : 0);
            /* setup drawing */
            b->dw->df = WpeGraphicalDirTree(b->cd, b->dd, cn);
            nco = b->dw->nf = b->cd->anz - 1;
            b->dw->ia = b->dw->ja = 0;
          }
          c = cold;
          cold = AltN; /* go back to name entry */
          break;
        }

      /* edit/execute button activation */
      case AltE:
        if(   (c == AltE && b->sw != 0 && b->sw != 3) 
           || (c == AltR && b->sw != 1) )
        {
          c = cold;
          break;
        }
        if(b->sw == 3) /* file-manager in execution mode */
        {
          if(cold == AltF)
            WpeFileNameTake(f, filen, *(b->df->name + b->fw->nf));
          if(!WpeIsXwin())
          {
            outp = e_open_view(0, 0, MAXSCOL - 1, MAXSLNS - 1, f->fb->ws, 1);
            fk_locate(0, 0);
            fk_cursor(1);
#if  MOUSE
            g[0] = 2;
            fk_mouse(g);
#endif
            e_sys_ini();
            printf(e_msg[ERR_EXEC], filen);
            fflush(stdout);
          }
          if (e_execute_file(filen))
          {
            if(!WpeIsXwin())
              e_sys_end();
            e_error(e_msg[ERR_COMMAND], 0, f->fb);
          }
          else if(!WpeIsXwin())
          {
            printf("%s", e_msg[ERR_HITCR]);
            fflush(stderr);
            fflush(stdout);
            e_t_wait_for_enter_on_raw_terminal();
          }
          if(!WpeIsXwin())
          {
            e_sys_end();
            e_close_view(outp, 1);
            fk_cursor(0);
#if MOUSE
            g[0] = 1;
            fk_mouse(g);
#endif
          }
          c = cold;
          break;
        }
        else
        {
          /* if there is only a pattern get back to file selection */
          if(strstr(filen, "*") || strstr(filen, "?"))
          {
            c = AltF;
            break;
          }
          /* Open-Project mode: remember the chosen .prj and leave the
             manager; the post-loop close opens it (avoids double close). */
          if(b->prj_sel)
          {
            if(WpeProjectPathOf(f, prj_path, sizeof(prj_path), filen) != 0)
            {
              c = AltN;
              break;
            }
            open_prj = 1;
            new_prj = (b->prj_sel == FM_PRJ_NEW);  /* New vs Open Project */
            c = WPE_ESC;
            break;
          }
          /* there is no open ??? file */
          if(b->sw == 0 || !fe)
          {
            /* close on open request */
            if(f->ed->flopt & FM_CLOSE_WINDOW)
            {
              e_close_window(f);
            }
            /* editing the file */
            e_edit(cn, filen);
          }
          else
          {
            /* try to open the file, no success return */
            if((fp = fopen(filen, "rb")) == NULL)
            {
              e_error(e_msg[ERR_ACCFILE], 0, f->fb);
              c = cold;
              break;
            }
            if(access(filen, 2) != 0)
              f->ins = 8;
            e_close_window(f);
            e_switch_window(winnum, fe);
            fe = cn->f[cn->mxedt];
            be = fe->b;
            se = fe->s;
            f = cn->f[cn->mxedt];
            /* the buffer's style stands: the file's own is not kept */
            memset(&read_info, 0, sizeof(read_info));
            if(e_read_file_into(fp, be, be->b.x, be->b.y, 0, &read_info, &mark_end) == E_BUFFER_OK)
            {
              /* the inserted text is the block */
              e_buffer_mark_from(be, E_BUFFER_MARK_BEGIN, E_BUFFER_POS_CURSOR);
              e_buffer_mark_set(be, E_BUFFER_MARK_END, mark_end.y, mark_end.x);
            }
            else
            {
              wpe_buffer_say_nomem(be, "the insert");
              read_info.nul = 0;                /* nothing was inserted: nothing to tell */
            }
            fclose(fp);

            e_schirm(fe, 1);
            e_say_nul_read(fe, &read_info);
          }

          /* if there was no error */
          dirtmp = WpeGetCurrentDir(cn);
          FREE(cn->dirct);
          cn->dirct = dirtmp;

          if (svmode >= 0)
            cn->flopt = svmode;

          if (svdir != NULL)
          {
            /* go back to the saved directory */
            if (chdir(svdir))
            {
              e_error(e_msg[ERR_WORKDIRACCESS], 0, cn->fb);

              /* we cannot determine where we are, try the home */
              if((dirtmp = getenv("HOME")) == NULL)
                e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
              if(chdir(dirtmp))
                e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
            }

            /* determine current dir */
            dirtmp = WpeGetCurrentDir(cn);

            FREE(cn->dirct);
            cn->dirct = dirtmp;

            FREE(svdir);
            svdir = NULL;
          }

          return(0);
        }

      case AltW: /* write activation */
      case AltS: /* save activation */
        if(   (c == AltW && b->sw != 2) 
           || (c == AltS && b->sw != 4)
           || !fe
           || (fe->ins == 8 && !WpeSaveAsIsTranscript(b, fe)) )
        {
          c = cold;
          break;
        }
        /* only file pattern, return */
        if(strstr(filen, "*") || strstr(filen, "?"))
        {
          c = AltF;
          break;
        }
        /* check whether the file exist */
        if(!access(filen, F_OK))
        {
          if((ftmp = MALLOC(strlen(filen) + 42)) == NULL)
            e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

          sprintf(ftmp, "File %s exist\nDo you want to overwrite it ?", filen);
          i = e_message(1, ftmp, f);
          FREE(ftmp);

          if(i == WPE_ESC)
          {
            c = WPE_ESC;
            break;
          }
          else if(i == 'N')
          {
            c = AltF;
            break;
          }
        }
        if(WpeSaveAsIsTranscript(b, fe))
          return(WpeSaveAsTranscript(f, fe, filen, svmode, &svdir));
        if(b->sw == 4) /* save as mode */
        {
          /* the window's own name, kept until the file is written */
          dtp = fe->dirct;
          ftp = fe->datnam;
          WpeFilenameToPathFile(filen, &fe->dirct, &fe->datnam);
          WpeSaveAsSettleName(fe, dtp, ftp, wpe_write_save(fe, WPE_WRITE_THEN_NAME_KEPT, ftp));
        }
        else
          WpeWriteBlockToFile(fe, se, filen);

        if(b->sw == 4 && (f->ed->edopt & ED_SYNTAX_HIGHLIGHT))
        {
          if(fe->c_sw)
            FREE(fe->c_sw);
          if(WpeIsProg())
            e_add_synt_tl(fe->datnam, fe);
          if(fe->c_st)
          {
            if(fe->c_sw)
              FREE(fe->c_sw);
            fe->c_sw = e_sc_txt(NULL, fe->b);
          }
          e_rep_win_tree(f->ed);
        }
        if(svmode >= 0)
          f->ed->flopt = svmode;

        if(svdir != NULL)
        {
          /* go back to the saved directory */
          if(chdir(svdir))
          {
            e_error(e_msg[ERR_WORKDIRACCESS], 0, cn->fb);

            /* we cannot determine where we are, try the home */
            if((dirtmp = getenv("HOME")) == NULL)
              e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
            if(chdir(dirtmp))
              e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
          }

          /* determine current dir */
          dirtmp = WpeGetCurrentDir(cn);

          FREE(cn->dirct);
          cn->dirct = dirtmp;

          FREE(svdir);
          svdir = NULL;
        }

        e_close_window(f);
        return(0);

      /* make dir button activation */
      case EINFG:
      case AltK:
        if(b->sw != 0)
        {
          c = cold;
          break;
        }
        /* create new directory */
        if(WpeMakeNewDir(f) != 0)
        {
          c = cold;
          break;
        }

        /* free up old structures */
        freedf(b->dd);
        freedf(b->dw->df);

        /* create new directory structure */
        b->dd = e_find_dir(SUDIR, f->ed->flopt & FM_SHOW_HIDDEN_DIRS ? 1 : 0);
        b->dw->df = WpeGraphicalDirTree(b->cd, b->dd, cn);
        /* go to the line where the new dir is */
        for(i = 0; i < b->dd->anz && strcmp(b->dd->name[i], "new.dir"); i++)
          ;
        /* set the slidebar variables */
        if((b->dw->nf = b->cd->anz + i) >= b->dw->df->anz)
          b->dw->nf = b->cd->anz - 1;
        if(b->dw->nf - b->dw->ia >= b->dw->ye - b->dw->ya)
          b->dw->ia = b->dw->nf + b->dw->ya - b->dw->ye + 1;
        else if(b->dw->nf - b->dw->ia < 0)
          b->dw->ia = b->dw->nf;
        cold = AltT;
        /* let the user modify the newly created dir */
        c = AltM;
        break;

      /* attribute/add file button activation */
      case AltA:
        /* not valid mode */
        if(b->sw != 0 && b->sw != 5)
        {
          c = cold;
          break;
        }
        /* attribute button */
        if(b->sw == 0)
        {
          if(cold == AltF) /* coming from file list */
          {
            WpeFileNameTake(f, filen, *(b->df->name + b->fw->nf));
            /* change the file attributes */
            WpeFileDirAttributes(filen, f);

            /* free up old file list structures */
            freedf(b->df);
            freedf(b->fw->df);

            /* create new file list */
            b->df = e_find_files(b->rdfile, fmode);
            /* setup drawing */
            b->fw->df = WpeGraphicalFileList(b->df, f->ed->flopt >> 9, cn);
          }
          else if(cold == AltT && (subdir = e_fm_tree_subdir(b)) != NULL) /* coming from dir tree */
          {
            if((ftmp = MALLOC(strlen(subdir) + 1)) == NULL)
              e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

            strcpy(ftmp, subdir);
            /* change the dir attributes */
            WpeFileDirAttributes(ftmp, f);

            FREE(ftmp);

            /* free up old file list structures */
            freedf(b->dd);
            freedf(b->dw->df);
            /* create new dir list */
            b->dd = e_find_dir(SUDIR, f->ed->flopt & FM_SHOW_HIDDEN_DIRS ? 1 : 0);
            /* setup drawing */
            b->dw->df = WpeGraphicalDirTree(b->cd, b->dd, cn);
          }
          c = cold;
        }
        else if(b->sw == 5) /* it is in project management */
        {
          FLWND *fw = (FLWND *)cn->f[cn->mxedt - 1]->b;
          if (cold != AltN)
            WpeFileNameTake(f, filen, *(b->df->name + b->fw->nf));
          dirtmp = cn->f[cn->mxedt - 1]->dirct;
          ftmp = MALLOC(strlen(f->dirct) + strlen(filen) + 2);
          len = strlen(dirtmp);
          if (strncmp(dirtmp, f->dirct, len) == 0)
            sprintf(ftmp, "%s%s", f->dirct + len, filen);
          else
            sprintf(ftmp, "%s%s", f->dirct, filen);
          e_prj_add_file_unique(fw, ftmp, f);
          c = AltN;
        }
        break;

      /* like save */
      case F2:
        c = AltS;
        break;
      /* leave file manager */
      case AltBl:
        c = WPE_ESC;

      default:
        /* not project management */
        if(b->sw != 5)
        {
          if(svmode >= 0)
          {
            /* restore options */
            f->ed->flopt = svmode;
          }

          if(svdir != NULL)
          {
            /* go back to the saved directory */
            if(chdir(svdir))
            {
              e_error(e_msg[ERR_WORKDIRACCESS], 0, cn->fb);

              /* we cannot determine where we are, try the home */
              if((dirtmp = getenv("HOME")) == NULL)
                e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
              if(chdir(dirtmp))
                e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
            }

            /* determine current dir */
            dirtmp = WpeGetCurrentDir(f->ed);
            FREE(f->ed->dirct);
            f->ed->dirct = dirtmp;
          }


          /* the key dispatcher returns zero when something has happened
             and this means that the file-manager lost focus, just return */
#ifdef PROG
          if(!e_tst_dfkt(f, c) || (WpeIsProg() && !e_prog_switch(f, c)))
#else
          if(!e_tst_dfkt(f, c))
#endif
          {
            if(svdir != NULL)
            {
              FREE(svdir);
              svdir = NULL;
            }

            return(0);
          }
          /* file manager is still active */
          if(svmode >= 0)
            f->ed->flopt = FM_SHOW_HIDDEN_FILES | FM_SHOW_HIDDEN_DIRS |
              FM_MOVE_OVERWRITE | FM_REKURSIVE_ACTIONS;
        }
        else if(c == WPE_ESC || e_is_list_close_key(f, c))
        {
          if(svdir != NULL)
          {
            /* go back to the saved directory */
            if(chdir(svdir))
            {
              e_error(e_msg[ERR_WORKDIRACCESS], 0, cn->fb);

              /* we cannot determine where we are, try the home */
              if((dirtmp = getenv("HOME")) == NULL)
                e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
              if(chdir(dirtmp))
                e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
            }

            /* determine current dir */
            dirtmp = WpeGetCurrentDir(cn);

            FREE(cn->dirct);
            cn->dirct = dirtmp;

            FREE(svdir);
            svdir = NULL;
          }

          /* close file manager */
          e_close_window(f);

          /* restore options */
          if(svmode >= 0)
            cn->flopt = svmode;

          return(WPE_ESC);
        }
        /* A window/menu switch key (Alt-W, Alt-0..9, function key) was
           pressed: close the add manager and let the switch happen, just
           like the non-project modes do above. Without this the add-to-
           project manager ignores such keys and spins. */
        else if(!e_tst_dfkt(f, c) || (WpeIsProg() && !e_prog_switch(f, c)))
        {
          if(svdir != NULL)
          {
            if(chdir(svdir) && (dirtmp = getenv("HOME")) != NULL)
              if(chdir(dirtmp)) ;
            dirtmp = WpeGetCurrentDir(cn);
            FREE(cn->dirct);
            cn->dirct = dirtmp;
            FREE(svdir);
            svdir = NULL;
          }
          e_close_window(f);
          if(svmode >= 0)
            cn->flopt = svmode;
          return(0);
        }
        c = cold;
        break;
    }
  }

  /* change to saved directory and lets hope it works */
  if(svdir != NULL)
  {
    /* go back to the saved directory */
    if(chdir(svdir))
    {
      e_error(e_msg[ERR_WORKDIRACCESS], 0, cn->fb);

      /* we cannot determine where we are, try the home */
      if((dirtmp = getenv("HOME")) == NULL)
        e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
      if(chdir(dirtmp))
        e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
    }

    /* determine current dir */
    dirtmp = WpeGetCurrentDir(cn);

    FREE(cn->dirct);
    cn->dirct = dirtmp;

    FREE(svdir);
    svdir = NULL;
  }

  /* restore options */
  if(svmode >= 0)
    cn->flopt = svmode;

  /* if in save mode or project management */
  if(f->save == 1 || b->sw == 5)
  {
    e_close_window(f);
    return(WPE_ESC);
  }
  else
  {
    e_close_window(f);
    if(open_prj)
    {
      if(new_prj)
        e_create_project(cn, prj_path);
      else
        e_select_project(cn, prj_path);
    }
    return(0);
  }
}


/**
 * WpeGrepFile - Does the Find text of @fd occur in the file @file?
 *
 * Search > Grep lists the files with an occurrence.  Each line is a whole
 * line to the search, so `^` and `$` are its ends and a whole word is a word
 * on that line, with the rules of Find.
 * Return: 1 when it occurs, 0 when it does not (or the file cannot be read),
 * -1 when the Find text is a regular expression that does not compile.
 */
int WpeGrepFile(char *file, const FIND *fd)
{
  FILE           *fp;
  char            str[256];
  WpeRowHit       hit;
  int             rc;

  if((fp = fopen(file, "r")) == NULL)
    return(0);

  while(fgets(str, 256, fp))
  {
    rc = wpe_rowfind_row(fd, (unsigned char *)str, strcspn(str, "\n"),
                         WPE_ROW_STARTS_LINE | WPE_ROW_ENDS_LINE, 0,
                         strcspn(str, "\n"), 0, &hit);
    if(rc != 0)
    {
      fclose(fp);
      return(rc);
    }
  }
  fclose(fp);
  return(0);
}

int WpeMakeNewDir(FENSTER *f)
{
  char           *dirct;
  int             msk, mode, ret;
  struct stat     buf;

  umask(msk = umask(077));
  mode = 0777 & ~msk;

  if((dirct = MALLOC(strlen(f->dirct) + 9)) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

  if(f->dirct[strlen(f->dirct) - 1] != DIRC)
    sprintf(dirct, "%s/new.dir", f->dirct);
  else
    sprintf(dirct, "%snew.dir", f->dirct);

  /* check existence and status (whether it is directory) */
  if(stat(dirct, &buf))
  {
    if((ret = mkdir(dirct, mode)) != 0)
      e_error(e_msg[ERR_NONEWDIR], 0, f->ed->fb);
  }
  else
  {
    /* check whether the existing file is a directory */
    if(!(buf.st_mode & S_IFDIR))
    {
      e_error(e_msg[ERR_NEWDIREXIST], 0, f->ed->fb);
      ret = 1;
    }
    else
      ret = 0;
  }

  FREE(dirct);
  return(ret);
}

int WpeFileDirAttributes(char *filen, FENSTER * f)
{
  struct stat     buf[1];
  int             mode, ret;
  W_OPTSTR       *o = e_init_opt_kst(f);

  /* if cannot access or error */
  if(stat(filen, buf))
  {
    e_error(e_msg[ERR_ACCFILE], 0, f->ed->fb);
    return 1;
  }
  mode = buf->st_mode;
  if(o == NULL)
    return(1);
  o->xa = 14;
  o->ya = 4;
  o->xe = 62;
  o->ye = 13;
  o->bgsw = AltO;
  o->name = "Attributes";
  o->crsw = AltO;
  e_add_txtstr(3, 2, "User:", o);
  e_add_txtstr(33, 2, "Other:", o);
  e_add_txtstr(18, 2, "Group:", o);
  e_add_sswstr(4, 3, 0, AltR, mode & 256 ? 1 : 0, "Read   ", o);
  e_add_sswstr(4, 4, 0, AltW, mode & 128 ? 1 : 0, "Write  ", o);
  e_add_sswstr(4, 5, 1, AltX, mode & 64 ? 1 : 0, "EXecute", o);
  e_add_sswstr(19, 3, 2, AltA, mode & 32 ? 1 : 0, "ReAd   ", o);
  e_add_sswstr(19, 4, 2, AltI, mode & 16 ? 1 : 0, "WrIte  ", o);
  e_add_sswstr(19, 5, 0, AltE, mode & 8 ? 1 : 0, "Execute", o);
  e_add_sswstr(34, 3, 3, AltD, mode & 4 ? 1 : 0, "ReaD   ", o);
  e_add_sswstr(34, 4, 3, AltT, mode & 2 ? 1 : 0, "WriTe  ", o);
  e_add_sswstr(34, 5, 4, AltU, mode & 1, "ExecUte", o);
  e_add_bttstr(12, 7, 1, AltO, " Ok ", NULL, o);
  e_add_bttstr(29, 7, -1, WPE_ESC, "Cancel", NULL, o);
  ret = e_opt_kst(o);
  if(ret != WPE_ESC)
  {
    mode = (o->sstr[0]->num << 8) + (o->sstr[1]->num << 7) +
      (o->sstr[2]->num << 6) + (o->sstr[3]->num << 5) +
      (o->sstr[4]->num << 4) + (o->sstr[5]->num << 3) +
      (o->sstr[6]->num << 2) + (o->sstr[7]->num << 1) +
      o->sstr[8]->num;
    if(chmod(filen, mode))
    {
      e_error(e_msg[ERR_CHGPERM], 0, f->ed->fb);
      freeostr(o);
      return(1);
    }
  }
  freeostr(o);
  return(0);
}

/* determines the current working directory,
   it returns the string
   in case of an error - if the directory cannot be accessed try to access HOME,
                         if it is impossible, give it up and exit with "system error"
                       - otherwise exit with "system error"
 */
char *WpeGetCurrentDir(ECNT *cn)
{
  int allocate_size;
  char *current_dir = NULL, *check_dir, *dirtmp;
  short home;

  home = 0;
  allocate_size = 256;
  if ((current_dir = (char *)WpeMalloc(allocate_size + 1)) == NULL)
  {
   e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
   return NULL;
  }

  do
  {
    /* allocate space for the directory name */
    if ((current_dir = (char *)WpeRealloc(current_dir, allocate_size + 1)) == NULL)
    {
     e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
     return NULL;
    }

    check_dir = getcwd(current_dir, allocate_size);
    if(check_dir == NULL)
    {
      switch(errno)
      {
        case EACCES:  /* directory cannot be read */
          if (home == 0)
          {
            e_error(e_msg[ERR_WORKDIRACCESS], 0, cn->fb);

            /* we cannot determine where we are, try the home */
            if ((dirtmp = getenv("HOME")) == NULL)
              e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);
            if (chdir(dirtmp))
              e_error(e_msg[ERR_HOMEDIRACCESS], 1, cn->fb);

            home = 1;
          }
          else
            e_error(e_msg[ERR_SYSTEM], 1, cn->fb); /* will not return */
          break;
        case EINVAL:  /* size is equal to 0 */
          allocate_size = 256; /* impossible !!! */
          break;
        case ERANGE:  /* not enough space for the pathname */
          allocate_size <<= 1;
          break;
        default:      /* System error */
          e_error(e_msg[ERR_SYSTEM], 1, cn->fb); /* will not return */
          break;
      }
    }
  } while (check_dir == NULL);

  if (current_dir[strlen(current_dir) - 1] != DIRC)
   strcat(current_dir, DIRS);
  return(current_dir);
}

/* It always returns the assembled file path, except
   when it is in wastebasket mode and there is an error, return NULL */
char *WpeAssemblePath(char *pth, struct dirfile *cd, struct dirfile *dd, int n, 
                      FENSTER *f)
{
  int             i = 0, k = 0, j = 0, t;
  char           *adir= NULL;  /* assembled directory */
  int             totall = 0;

#ifdef UNIX
  if (!strcmp(cd->name[0], "Wastebasket"))
  {
    if ((adir = WpeGetWastefile("")))
    {
      totall = strlen(adir) + 16;
      if ((adir = REALLOC(adir, totall)) == NULL)
        e_error(e_msg[ERR_LOWMEM], 1, f->fb);

      strcat(adir, DIRS);
      i = strlen(adir);
      k++;
    }
    else
    {
      /* Error failed to find wastebasket */
      e_error(e_msg[ERR_NOWASTE], 0, f->ed->fb);
      return NULL;
    }
  }
#endif
  for(; k <= n && k < cd->anz; i++, j++)
  {
    if(i >= totall-1)
    {
      totall += 16;
      if((adir = REALLOC(adir, totall)) == NULL)
        e_error(e_msg[ERR_LOWMEM], 1, f->fb);
    }
    *(adir + i) = *(*(cd->name + k) + j);

    if(*(adir + i) == '\0' || *(adir + i) == DIRC)
    {
      *(adir + i) = DIRC;
      k++;
      j = -1;
    }
  }
  if(n >= k)
  {
    t = n - cd->anz;
    j = 0;

    do
    {
      if(i >= totall-2)
      {
        totall += 16;
        if((adir = REALLOC(adir, totall)) == NULL)
          e_error(e_msg[ERR_LOWMEM], 1, f->fb);
      }
      *(adir + i) = *(*(dd->name + t) + j);
      i++;
      j++;
    }
    while(*(adir + i - 1) != '\0');

/*
    for(j = 0; (*(pth + i) = *(*(dd->name + t) + j)) != '\0'; i++, j++)
      ;
*/

  }
  if(n == 0 || n >= k)
    i++;
  *(adir + i - 1) = '\0';
  return(adir);
}


/* create tree structure up to working directory */
struct dirfile *WpeCreateWorkingDirTree(int sw, ECNT *cn)
{
  struct dirfile *df;
  char           *buf;
  char           *tmp, *tmp2;
  char          **dftmp;
  int             buflen = 256;
  int             maxd = 10;    /* inital number of directory levels */
  int             i, j, k;

  buf = WpeGetCurrentDir(cn);

  buflen = strlen(buf);
  if((tmp = MALLOC(buflen+1)) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

  /* initialise directory list */
  if(   ((df = MALLOC(sizeof(struct dirfile))) == NULL)
     || ((df->name = MALLOC(sizeof(char *) * maxd)) == NULL) )
    e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

  df->anz = 0;

  if(sw == 1)                        /* file-manager open to wastebasket */
  {
    /* instead of saving the real wastebasket dir into the structure
       the "Wastebasket" name will appear */
    if((tmp2 = WpeGetWastefile("")) == NULL)
    {
      e_error(e_msg[ERR_NOWASTE], 0, cn->fb);    /* more error check ??? */
      i = 0;
    }
    else
    {
      i = strlen(tmp2);
      /* increase the level in the dir tree */
      df->anz = 1;
      /* save the name into first level */
      if((*(df->name) = MALLOC(12 * sizeof(char))) == NULL)
        e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
      strcpy(*(df->name), "Wastebasket");

      if(!strncmp(tmp2, buf, i) && buf[i])
      {
        FREE(tmp2);
        i++;
      }
      else
      {
        FREE(tmp2);
        FREE(buf);
        FREE(tmp);
        return(df);
      }
    }
  }
  else
  {
    i = 0;
  }


  for(j = 0; i <= buflen; i++, j++)
  {
    tmp[j] = buf[i];
    /* if directory separator or end of string */
    if(tmp[j] == DIRC || tmp[j] == '\0')
    {
      if(buf[i] == '\0' && j == 0)
        return(df);
      if(df->anz == 0)  /* for the very first time save the '/' sign */
        j++;
      tmp[j] = '\0';

      /* if we need more space for the directories */
      if(df->anz >= maxd)
      {
        maxd += 10;
        dftmp = df->name;
        if((df->name = MALLOC(sizeof(char *) * maxd)) == NULL)
          e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
        for(k = 0; k < maxd - 10; k++)
          *(df->name + k) = *(dftmp + k);
        FREE(dftmp);
      }
      /* save the current directory */
      if((*(df->name + df->anz) = MALLOC((strlen(tmp) + 1) * sizeof(char))) == NULL)
        e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
      strcpy(*(df->name + df->anz), tmp);
      df->anz++;
      j = -1;
    }
  }
  FREE(buf);
  FREE(tmp);
  return(df);
}

/* This function creates a wastebasket directory if needed, then returns a
   string made up of the last filename in the string pointed to by 'file' and
   the wastebasket path.  It returns NULL if there is a memory allocation
   error or the pointer to 'path' otherwise.  The original function did no
   error checking on wastebasket path creation.  The modified version does --
   it also returns NULL on an error in the chdir function or mkdir
   function. */
char  *WpeGetWastefile(char *file)
{
  static char    *wastebasket = NULL;
  int             i, lw;
  char           *tmp;             /* storage for wastebasket path */
  char           *tmp2;

  if(!wastebasket)  /* setup and test access to wastebasket directory */
  {
    if((tmp2 = getenv("HOME")) == NULL)
      return NULL;

    lw = strlen(tmp2) + 1 + strlen(WASTEBASKET) + 1;
    /*    HOME          /     WASTEBASKET       \0    */
    if((tmp = (char *)MALLOC(lw)) == NULL)
      return NULL;

    sprintf(tmp, "%s/%s", tmp2, WASTEBASKET);

    /* if wastebasket dir does not exist, create it with error checking */
    if(access(tmp, F_OK))
    {
      if(mkdir(tmp, 0700))
      {
        FREE(tmp);
        return NULL;
      }
    }
    /* check for wastebasket's permissions */
    if(access(tmp, R_OK | W_OK | X_OK))
    {
      FREE(tmp);
      return NULL;
    }
    wastebasket = tmp;
  }

  /* verify that wastebasket directory still exists before proceeding */
  if(access(wastebasket, F_OK | R_OK | W_OK | X_OK))
  {
    /* try to recreate it */
    if(mkdir(wastebasket, 0700))
      return NULL;
  }

  /* return wastebasket directory path if no filename in 'file' */
  if(file[0] == '\0')
  {
    if((tmp2 = MALLOC(strlen(wastebasket)+1)) == NULL)
      return NULL;
    strcpy(tmp2, wastebasket);
    return(tmp2);
  }

  /* else get filename from end of 'file' string, 
     append to wastebasket sting */
  for(i = strlen(file) - 1; i >= 0 && file[i] != DIRC; i--)
    ;
  if((tmp2 = MALLOC(strlen(wastebasket) + strlen(file + 1 + i) + 2)) == NULL)
    return NULL;

  sprintf(tmp2, "%s/%s", wastebasket, file + 1 + i);
  return(tmp2);
}

struct dirfile *WpeGraphicalFileList(struct dirfile *df, int sw, ECNT *cn)
{
 struct dirfile *edf;
 char          **name, **ename, *stmp, str[E_FILE_INFO_ROW_SIZE];
 int             ntmp, n, i, *num;

 /* allocate the same structure as the argument */
 if ((edf = MALLOC(sizeof(struct dirfile))) == NULL)
  e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

 edf->anz = df->anz;
 edf->name = NULL;

/* OSF and AIX fix, for malloc(0) they return NULL */
 if (df->anz)
 {
  if ((edf->name = MALLOC(df->anz * sizeof(char *))) == NULL)
   e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

  if ((num = MALLOC(df->anz * sizeof(int))) == NULL)
   e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

  for (i = 0; i < df->anz; i++)
  {
   e_file_info(*(df->name + i), str, sizeof str, num + i, sw);
   if ((*(edf->name + i) = MALLOC(strlen(str) + 1)) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
   strcpy(*(edf->name + i), str);
  }

  /* sort by time or size mode */
  if (sw & 3)
  {
   if ((ename = MALLOC(df->anz * sizeof(char *))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

   if ((name = MALLOC(df->anz * sizeof(char *))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

   for (i = 0; i < df->anz; i++)
   {
    for (ntmp = num[i], n = i; n > 0 && ntmp > num[n - 1]; n--)
    {
     *(ename + n) = *(ename + n - 1);
     *(name + n) = *(name + n - 1);
     num[n] = num[n - 1];
    }
    *(ename + n) = *(edf->name + i);
    *(name + n) = *(df->name + i);
    num[n] = ntmp;
   }
   FREE(edf->name);
   FREE(df->name);
   edf->name = ename;
   df->name = name;
  }

  FREE(num);

  /* reverse order */
  if (sw & 4)
  {
   for (i = 0; i < (df->anz) / 2; i++)
   {
    stmp = edf->name[i];
    edf->name[i] = edf->name[edf->anz - i - 1];
    edf->name[edf->anz - i - 1] = stmp;
    stmp = df->name[i];
    df->name[i] = df->name[df->anz - i - 1];
    df->name[df->anz - i - 1] = stmp;
   }
  }
 }

 return(edf);
}

/**
 * WpeTreeLabel - One row of the directory tree: indentation, branch, name.
 * @indent: how many blanks come first; @lead/@glyph: the blank and the branch
 * drawing in front of the name; @name: the folder's name.
 *
 * The row lives on the heap at its own length.  It is as long as the folder
 * is deep (two blanks per level) plus a name of up to WPE_NAME_MAX bytes, so
 * no fixed buffer holds every row the file manager can be asked to draw.
 * Return: the row, or NULL when out of memory.
 */
static char *WpeTreeLabel(int indent, const char *lead, const char *glyph, const char *name)
{
  size_t          pad = indent > 0 ? (size_t)indent : 0;
  char           *row = MALLOC(pad + strlen(lead) + strlen(glyph) + strlen(name) + 1);

  if(row == NULL)
    return(NULL);
  memset(row, ' ', pad);
  sprintf(row + pad, "%s%s%s", lead, glyph, name);
  return(row);
}

/**
 * WpeTreeRootText - What the first row of the directory tree shows.
 * @first: the first folder of the path: "/" is drawn as "Root".
 */
static const char *WpeTreeRootText(const char *first)
{
#ifndef DJGPP
  if(*first == DIRC && *(first + 1) == '\0')
    return("Root");
#endif
  return(first);
}

struct dirfile *WpeGraphicalDirTree(struct dirfile *cd, struct dirfile *dd, ECNT *cn)
{
  extern char    *ctree[5];
  struct dirfile *edf;
  const char     *glyph;
  int             i = 0;

  if((edf = MALLOC(sizeof(struct dirfile))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

  /* for the OSF and AIX this should never be zero, we are always somewhere */
  if(cd->anz + dd->anz > 0)
  {
    if((edf->name = MALLOC((cd->anz + dd->anz) * sizeof(char *))) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, cn->fb);

    for(i = 0; i < cd->anz; i++)
    {
      if(!i)
        *(edf->name + i) = WpeTreeLabel(0, "", "", WpeTreeRootText(*(cd->name + i)));
      else
      {
        if(i == cd->anz - 1 && dd->anz < 1)
          glyph = ctree[1];
        else if(i == cd->anz - 1)
          glyph = ctree[2];
        else
          glyph = ctree[0];
        *(edf->name + i) = WpeTreeLabel(2 * (i - 1), "", glyph, *(cd->name + i));
      }
      if(*(edf->name + i) == NULL)
        e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
    }

    for(; i < cd->anz + dd->anz; i++)
    {
      if(i == cd->anz + dd->anz - 1)
        glyph = ctree[4];
      else
        glyph = ctree[3];
      *(edf->name + i) = WpeTreeLabel(2 * (cd->anz - 2), " ", glyph, *(dd->name + (i - cd->anz)));
      if(*(edf->name + i) == NULL)
        e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
    }
  }

  edf->anz = i;
  return(edf);
}

int WpeDelWastebasket(FENSTER *f)
{
  char            *tmp;
  int             ret, mode = f->ed->flopt;

  WpeMouseChangeShape(WpeWorkingShape);
  f->ed->flopt = FM_SHOW_HIDDEN_FILES | FM_SHOW_HIDDEN_DIRS |
                 FM_MOVE_OVERWRITE | FM_REKURSIVE_ACTIONS;
  if ((tmp = WpeGetWastefile("")))
  {
    ret = WpeRemoveDir(tmp, "*", f, 0);
    FREE(tmp);

    /* Unfortunately there is this racing condition, so
       this is necessary to get back the deleted wastebasket. */
    if ((tmp = WpeGetWastefile("")))
    {
      FREE(tmp);
      ret = 0;
    }
    else
    {
      e_error(e_msg[ERR_NOWASTE], 0, f->ed->fb);
      ret = 1;
    }
  }
  else
  {
    /* Error failed to find wastebasket */
    e_error(e_msg[ERR_NOWASTE], 0, f->ed->fb);
    ret = 1;
  }
  f->ed->flopt = mode;
  WpeMouseRestoreShape();
  return(ret);
}

int WpeShowWastebasket(FENSTER *f)
{
  return(WpeCallFileManager(6, f));
}

int WpeQuitWastebasket(FENSTER * f)
{
  char            *tmp;
  int             ret = 0, mode = f->ed->flopt;

  if(mode & FM_PROMPT_DELETE)
    f->ed->flopt = FM_SHOW_HIDDEN_FILES | FM_SHOW_HIDDEN_DIRS |
                   FM_REMOVE_PROMPT | FM_MOVE_OVERWRITE | FM_REKURSIVE_ACTIONS;
  else if(mode & FM_DELETE_AT_EXIT)
    f->ed->flopt = FM_SHOW_HIDDEN_FILES | FM_SHOW_HIDDEN_DIRS |
                   FM_MOVE_OVERWRITE | FM_REKURSIVE_ACTIONS;

  if((mode & FM_PROMPT_DELETE) || (mode & FM_DELETE_AT_EXIT))
  {
    WpeMouseChangeShape(WpeWorkingShape);
    if ((tmp = WpeGetWastefile("")))
    {
      ret = WpeRemoveDir(tmp, "*", f, 0);
      FREE(tmp);
    }
    else
    {
      /* Error failed to find wastebasket */
      e_error(e_msg[ERR_NOWASTE], 0, f->ed->fb);
      ret = 0;
    }
    WpeMouseRestoreShape();
  }

  f->ed->flopt = mode;
  return(ret);
}


int WpeRemoveDir(char *dirct, char *file, FENSTER * f, int rec)
{
  PIC            *pic = NULL;
  char           *tmp;
  int             i, ret, svmode = f->ed->flopt;
  struct dirfile *dd;

  if(rec > MAXREC)
    return(0);

  /* only copy it to the wastebasket */
  if(f->ed->flopt & FM_REMOVE_INTO_WB)
  {
    if ((tmp = WpeGetWastefile(dirct)))
    {
      i = strlen(tmp);
      /* we should not use the fact, that the wasbasket is always something
         meaningful !!! */
      if(strncmp(tmp, dirct, i))
      {
        ret = WpeRenameCopyDir(dirct, file, tmp, f, 0, 0);
        FREE(tmp);
        return(ret);
      }
      FREE(tmp);
    }
    else
    {
      e_error(e_msg[ERR_NOWASTE], 0, f->ed->fb);
      return 1;
    }
  }

  if((tmp = MALLOC(strlen(dirct) + strlen(file)+2)) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

  /* search for files in the directory */
  sprintf(tmp, "%s%c%s", dirct, DIRC, file);
  dd = e_find_files(tmp, f->ed->flopt & FM_SHOW_HIDDEN_FILES ? 1 : 0);

  /* it is called for the first time and the user should be asked about
     the deletion */
  if(!rec && (f->ed->flopt & FM_REMOVE_PROMPT) && dd->anz > 0)
  {
    if((ret = WpeDirDelOptions(f)) < 0)
    {
      freedf(dd);
      FREE(tmp);
      return(ret == WPE_ESC ? 1 : 0);
    }
    if(ret)
      f->ed->flopt |= FM_REMOVE_PROMPT;
    else
      f->ed->flopt &= ~FM_REMOVE_PROMPT;
    rec = -1;
  }
  FREE(tmp);

  /* cleans up the files in the directory */
  for(i = 0; i < dd->anz; i++)
  {
    if((tmp = MALLOC(strlen(dirct) + strlen(dd->name[i])+15)) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

    sprintf(tmp, "Remove File:\n%s%c%s", dirct, DIRC, dd->name[i]);
    if(f->ed->flopt & FM_REMOVE_PROMPT)
    {
      ret = e_message(1, tmp, f);
    }
    else
      ret = 'Y';
    if(ret == WPE_ESC)
    {
      freedf(dd);
      FREE(tmp);
      f->ed->flopt = svmode;
      return(1);
    }
    else if(ret == 'Y')
    {
      /* this should definitely fit in */
      sprintf(tmp, "%s%c%s", dirct, DIRC, dd->name[i]);

      /* put message out */
      if(e_mess_win("Remove", tmp, &pic, f))
      {
        FREE(tmp);
        break;
      }

      if(pic)
      {
        e_close_view(pic, 1);
        pic = NULL;
      }

      /* try to remove it */
      if(remove(tmp))
      {
        e_error(e_msg[ERR_DELFILE], 0, f->ed->fb);
        freedf(dd);
        FREE(tmp);
        f->ed->flopt = svmode;
        return(1);
      }
    }
    FREE(tmp);
  }

  if(pic)
    e_close_view(pic, 1);
  freedf(dd);

  /* if recursive action is specified clean up the 
     subdirectories as well */
  if(f->ed->flopt & FM_REKURSIVE_ACTIONS)
  {
    if((tmp = MALLOC(strlen(dirct) + strlen(SUDIR)+2)) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

    /* search for subdirectories */
    sprintf(tmp, "%s%c%s", dirct, DIRC, SUDIR);
    dd = e_find_dir(tmp, f->ed->flopt & FM_SHOW_HIDDEN_DIRS ? 1 : 0);

    /* should the user be asked about deletion ? */
    if(!rec && (f->ed->flopt & FM_REMOVE_PROMPT) && dd->anz > 0)
    {
      if((ret = WpeDirDelOptions(f)) < 0)
      {
        freedf(dd);
        FREE(tmp);
        return(ret == WPE_ESC ? 1 : 0);
      }
      if(ret)
        f->ed->flopt |= FM_REMOVE_PROMPT;
      else
        f->ed->flopt &= ~FM_REMOVE_PROMPT;
    }
    else if(rec < 0)
      rec = 0;

    FREE(tmp);

    /* call recursively itself to delete the subdirectories */
    for(rec++, i = 0; i < dd->anz; i++)
    {
      if((tmp = MALLOC(strlen(dirct) + strlen(dd->name[i])+2)) == NULL)
        e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

      sprintf(tmp, "%s%c%s", dirct, DIRC, dd->name[i]);
      if(WpeRemoveDir(tmp, file, f, rec))
      {
        freedf(dd);
        FREE(tmp);
        f->ed->flopt = svmode;
        return(1);
      }
      FREE(tmp);
    }
    freedf(dd);
  }

  f->ed->flopt = svmode;

  /* remove finally the directory itself */
  if(rmdir(dirct))
  {
    e_error(e_msg[ERR_DELFILE], 0, f->ed->fb);
    return(1);
  }
  else
    return(0);
}

int WpeRemove(char *file, FENSTER * f)
{
  struct stat     buf;
  struct stat     lbuf;
  char           *tmp2;
  int             ret;

  if(lstat(file, &lbuf))
  {
    e_error(e_msg[ERR_ACCFILE], 0, f->ed->fb);
    return 1;
  }

  WpeMouseChangeShape(WpeWorkingShape);

  /* this is important, first we check whether it is a file,
     this check works even when it is a pointer, if it is not
     a file, then it can be an "invalid" symbolic link, check for that */
  if(((stat(file, &buf) == 0) && S_ISREG(buf.st_mode)) || S_ISLNK(lbuf.st_mode))
  {
    if((f->ed->flopt & FM_REMOVE_INTO_WB))
    {
      if ((tmp2 = WpeGetWastefile(file)))
      {
        ret = strlen(tmp2);
        if(strncmp(tmp2, file, ret))
        {
          e_rename(file, tmp2, f);
        }
        FREE(tmp2);
        ret = 0;
      }
      else
      {
        e_error(e_msg[ERR_NOWASTE], 0, f->ed->fb);
        ret = 1;
      }
    }
    else
    {
      if(f->ed->flopt & FM_REMOVE_PROMPT)
      {
        if((tmp2 = MALLOC(strlen(file) + 14)) == NULL)
          e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

        sprintf(tmp2, "Remove File:\n%s", file);
        ret = e_message(1, tmp2, f);
        FREE(tmp2);
      }
      else
        ret = 'Y';

      if(ret == 'Y')
      {
        if(remove(file))
        {
          e_error(e_msg[ERR_DELFILE], 0, f->ed->fb);
          ret = 1;
        }
        else
          ret = 0;
      }
      else
        ret = 0;
    }
  }
  else
  {
    ret = WpeRemoveDir(file, f->fd.file, f, 0);
  }

  WpeMouseRestoreShape();
  return(ret);
}

/* sw = 0  -> renames the directory
   sw = 1  -> copies the directory
   sw = 2  -> links directory */
int WpeRenameCopyDir(char *dirct, char *file, char *newname, FENSTER *f,
                     int rec, int sw)
{
  char            *tmp, *ntmp, *mtmp;
  int             i, ret, mode;
  struct dirfile *dd;
  struct stat     buf;
  PIC            *pic = NULL;

  if(rec > MAXREC)
    return(0);

  /* check whether the dir already exist */
  ret = access(newname, F_OK);
  /* rename mode and dir does not exist */
  if (sw == 0 && ret && file[0] == '*' && file[1] == '\0')
  {
    if ((ret = rename(dirct, newname)))
      e_error(e_msg[ERR_RENFILE], 0, f->ed->fb);
    return(ret);
  }

  /* directory does not exist */
  if (ret != 0)
  {
    /* get the permissions */
    if (stat(dirct, &buf))
    {
      e_error(e_msg[ERR_ACCFILE], 0, f->ed->fb);
      return(1);
    }
    /* with that permission create the new dir */
    if(mkdir(newname, buf.st_mode))
    {
      e_error(e_msg[ERR_NONEWDIR], 0, f->ed->fb);
      return(1);
    }
  }

  if(f->ed->flopt & FM_REKURSIVE_ACTIONS)
  {
    if((tmp = MALLOC(strlen(dirct) + 2 + strlen(SUDIR))) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

    sprintf(tmp, "%s%c%s", dirct, DIRC, SUDIR);
    dd = e_find_dir(tmp, f->ed->flopt & FM_SHOW_HIDDEN_DIRS ? 1 : 0);
    FREE(tmp);

    for(rec++, i = 0; i < dd->anz; i++)
    {

      if((tmp = MALLOC(strlen(dirct) + 2 + strlen(dd->name[i]))) == NULL)
        e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

      if((ntmp = MALLOC(strlen(newname) + 2 + strlen(dd->name[i]))) == NULL)
        e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

      sprintf(tmp, "%s%c%s", dirct, DIRC, dd->name[i]);
      sprintf(ntmp, "%s%c%s", newname, DIRC, dd->name[i]);

      if(WpeRenameCopyDir(tmp, file, ntmp, f, rec, sw))
      {
        FREE(tmp);
        FREE(ntmp);
        freedf(dd);
        return(1);
      }
      FREE(tmp);
      FREE(ntmp);
    }
    freedf(dd);
  }

  if((tmp = MALLOC(strlen(dirct) + 2 + strlen(file))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);
  sprintf(tmp, "%s%c%s", dirct, DIRC, file);
  dd = e_find_files(tmp, f->ed->flopt & FM_SHOW_HIDDEN_FILES ? 1 : 0);
  FREE(tmp);

  mode = f->ed->flopt;
  f->ed->flopt &= ~FM_REMOVE_PROMPT;

  for(i = 0; i < dd->anz; i++)
  {
    if((ntmp = MALLOC(strlen(newname) + 2 + strlen(dd->name[i]))) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

    sprintf(ntmp, "%s%c%s", newname, DIRC, dd->name[i]);
    ret = 'Y';

    if(access(ntmp, 0) == 0)
    {
      if(f->ed->flopt & FM_MOVE_PROMPT)
      {
        if((tmp = MALLOC(strlen(ntmp) + 31)) == NULL)
          e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

        sprintf(tmp, "File %s exist !\nOverwrite File ?", ntmp);
        if(pic)
        {
          e_close_view(pic, 1);
          pic = NULL;
        }
        ret = e_message(1, tmp, f);
        FREE(tmp);

        if(ret == 'Y')
        {
          if(WpeRemove(ntmp, f))
          {
            FREE(ntmp);
            freedf(dd);
            return(1);
          }
        }
        else if(ret == WPE_ESC)
        {
          FREE(ntmp);
          freedf(dd);
          return(1);
        }
      }
      else if(f->ed->flopt & FM_MOVE_OVERWRITE)
      {
        if(WpeRemove(ntmp, f))
        {
          FREE(ntmp);
          freedf(dd);
          return(1);
        }
      }
    }

    if((tmp = MALLOC(strlen(dirct) + 2 + strlen(dd->name[i]))) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

    sprintf(tmp, "%s%c%s", dirct, DIRC, dd->name[i]);

    if(ret == 'Y')
    {
      if((mtmp = MALLOC(strlen(tmp) + 2 + strlen(ntmp))) == NULL)
        e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

      sprintf(mtmp, "%s %s", tmp, ntmp);
      if(e_mess_win(!sw ? "Rename" : "Copy", mtmp, &pic, f))
      {
        FREE(tmp);
        FREE(ntmp);
        FREE(mtmp);
        break;
      }
      FREE(mtmp);

      if (sw == 0)
      {
        if ((ret = rename(tmp, ntmp)))
        {
          e_error(e_msg[ERR_RENFILE], 0, f->ed->fb);
          FREE(tmp);
          FREE(ntmp);
          freedf(dd);
          return(1);
        }
      }
      else if (sw == 1)
      {
        if (WpeCopyFileCont(tmp, ntmp, f))
        {
          FREE(tmp);
          FREE(ntmp);
          freedf(dd);
          return(1);
        }
      }
      else if(sw == 2)
      {
        if(WpeLinkFile(tmp, ntmp, f->ed->flopt & FM_TRY_HARDLINK, f))
        {
          FREE(tmp);
          FREE(ntmp);
          freedf(dd);
          return(1);
        }
      }
    }

    FREE(tmp);
    FREE(ntmp);
  }

  if(pic)
    e_close_view(pic, 1);

  f->ed->flopt = mode;

  if(sw == 0)
  {
    if(rmdir(dirct))
      e_error(e_msg[ERR_DELFILE], 0, f->ed->fb);
  }

  freedf(dd);
  return(0);
}


int WpeRenameCopy(char *file, char *newname, FENSTER *f, int sw)
{
 struct stat     buf;
 struct stat     lbuf;
 char           *tmp, *tmpl;
 int             ln = -1, ret = 'Y';
 int             allocate_size, retl = 0;

 WpeMouseChangeShape(WpeWorkingShape);

 /* in copy mode check whether it is a link */
 if (sw == 0)
 {
  if (lstat(file, &lbuf))
  {
   e_error(e_msg[ERR_ACCFILE], 0, f->ed->fb);
   return 1;
  }

  if (S_ISLNK(lbuf.st_mode))
   ln = 1;
 }

 if ((stat(file, &buf) == 0) && S_ISDIR(buf.st_mode) && ln < 0)
  retl = WpeRenameCopyDir(file, f->fd.file, newname, f, 0, sw);
 else
 {
  /* check whether file exist */
  if (access(newname, 0) == 0)
  {
   if (f->ed->flopt & FM_MOVE_OVERWRITE)
   {
    if (WpeRemove(newname, f))
    {
     WpeMouseRestoreShape();
     return(1);
    }
   }
   else if (f->ed->flopt & FM_MOVE_PROMPT)
   {
    if ((tmp = MALLOC(strlen(newname) + 26)) == NULL)
     e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);
    sprintf(tmp, "File %s exist\nRemove File ?", newname);
    ret = e_message(1, tmp, f);
    FREE(tmp);
    if (ret == 'Y')
    {
     if (WpeRemove(newname, f))
     {
      WpeMouseRestoreShape();
      return(1);
     }
    }
   }
  }

  if (ret == 'Y')
  {
   if (sw == 1)
   {
    retl = WpeCopyFileCont(file, newname, f);
   }
   else if (sw == 2)
   {
    retl = WpeLinkFile(file, newname, f->ed->flopt & FM_TRY_HARDLINK, f);
   }
   else if (sw == 0 && ln < 0) /* rename mode, no softlink */
   {
    if ((retl = rename(file, newname)) == -1)
    {
     if (errno == EXDEV)
     {
      if ((retl = WpeCopyFileCont(file, newname, f)) == 0)
      {
       if ((retl = remove(file)))
        e_error(e_msg[ERR_DELFILE], 0, f->ed->fb);
      }
     }
     else
     {
      e_error(e_msg[ERR_RENFILE], 0, f->ed->fb);
      retl = 1;
     }
    }
   }
   else if (sw == 0)
   {
    allocate_size = 2;
    tmp = NULL;
    do
    {
     if (tmp)
      FREE(tmp);
     allocate_size += 4;
     if ((tmp = MALLOC(allocate_size)) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

      ln = readlink(file, tmp, allocate_size-1);
    } while (!(ln < allocate_size-1));
    tmp[ln] = '\0';

    for (; ln >= 0 && tmp[ln] != DIRC; ln--)
     ;
    if (ln < 0)
    {
     if ((tmpl = MALLOC(strlen(f->dirct) + 2 + strlen(tmp))) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

     sprintf(tmpl, "%s%c%s", f->dirct, DIRC, tmp);
     retl = WpeRenameLink(file, newname, tmpl, f);
     FREE(tmpl);
    }
    else
    {
     retl = WpeRenameLink(file, newname, tmp, f);
    }
   }
  }
 }
 WpeMouseRestoreShape();
 return(retl);
}

int WpeCopyFileCont(char *oldfile, char *newfile, FENSTER *f)
{
  struct stat     buf;
  int             ret;
  char           *buffer;
  FILE           *fpo, *fpn;

  /* get the status of the file */
  if(stat(oldfile, &buf))
  {
    e_error(e_msg[ERR_ACCFILE], 0, f->ed->fb);
    return(1);
  }

  /* open files for copying */
  if((fpo = fopen(oldfile, "rb")) == NULL)
  {
    e_error(e_msg[ERR_OREADFILE], 0, f->ed->fb);
    return(1);
  }
  if((fpn = fopen(newfile, "wb")) == NULL)
  {
    e_error(e_msg[ERR_OWRITEFILE], 0, f->ed->fb);
    return(1);
  }

  /* allocate buffer for copying */
  if((buffer = malloc(E_C_BUFFERSIZE)) == NULL)
  {
    e_error(e_msg[ERR_ALLOC_CBUF], 0, f->ed->fb);
    return(1);
  }

  /* copy until end of file */
  do
  {
    ret = fread(buffer, 1, E_C_BUFFERSIZE, fpo);
    /* we should be able to write the same amount of info */
    if (fwrite(buffer, 1, ret, fpn) != ret)
    {
      fclose(fpo);
      fclose(fpn);
      free(buffer);
      e_error(e_msg[ERR_INCONSCOPY], 0, f->ed->fb);
      return(1);
    }
  } while(!feof(fpo));

  fclose(fpo);
  fclose(fpn);
  free(buffer);

  /* Well, we just created the file, so theoretically this 
     should succed. Of course this is a racing condition !!! */
  if(chmod(newfile, buf.st_mode))
    e_error(e_msg[ERR_CHGPERM], 0, f->ed->fb);
  return(0);
}

#ifndef NOSYMLINKS
/* Link a file according to the required mode
   sw != 0 -> symbolic link
   sw == 0 -> hard link
   When hard linking does not work, it tries to do
   symbolic linking
*/
int WpeLinkFile(char *fl, char *ln, int sw, FENSTER *f)
{
  int ret;

  if(sw || link(fl, ln))
  {
    ret = symlink(fl, ln);
    if (ret)
    {
      e_error(e_msg[ERR_LINKFILE], 0, f->ed->fb);
    }
    return(ret);
  }
  return(0);
}

int WpeRenameLink(char *old, char *ln, char *fl, FENSTER *f)
{
  int ret;

  ret = symlink(fl, ln);
  if(ret)
  {
    e_error(e_msg[ERR_LINKFILE], 0, f->ed->fb);
    return(1);
  }
  else
  {
    if(remove(old))
    {
      e_error(e_msg[ERR_DELFILE], 0, f->ed->fb);
      return(1);
    }
    else
      return(0);
  }
}
#endif

int e_rename(char *file, char *newname, FENSTER * f)
{
  return(WpeRenameCopy(file, newname, f, 0));
}

int e_rename_dir(char *dirct, char *file, char *newname, FENSTER * f, int rec)
{
  return(WpeRenameCopyDir(dirct, file, newname, f, rec, 0));
}


int e_link(char *file, char *newname, FENSTER *f)
{
  return(WpeRenameCopy(file, newname, f, 2));
}


int e_copy(char *file, char *newname, FENSTER * f)
{
  return(WpeRenameCopy(file, newname, f, 1));
}


int WpeFileManagerOptions(FENSTER * f)
{
  int             ret;
  W_OPTSTR       *o = e_init_opt_kst(f);

  if(o == NULL)
    return(1);

  o->xa = 8;
  o->ya = 1;
  o->xe = 69;
  o->ye = 22;
  o->bgsw = AltO;
  o->name = "File-Manager-Options";
  o->crsw = AltO;
  e_add_txtstr(4, 2, "Directories:", o);
  e_add_txtstr(35, 13, "Wastebasket:", o);
  e_add_txtstr(4, 7, "Move/Copy:", o);
  e_add_txtstr(35, 8, "Remove:", o);
  e_add_txtstr(35, 2, "Sort Files by:", o);
  e_add_txtstr(4, 12, "Links on Files:", o);
  e_add_txtstr(4, 16, "On Open:", o);

  e_add_sswstr(5, 3, 12, AltF, 
               f->ed->flopt & FM_SHOW_HIDDEN_FILES ? 1 : 0, 
               "Show Hidden Files      ", o);
  e_add_sswstr(5, 4, 12, AltD, 
               f->ed->flopt & FM_SHOW_HIDDEN_DIRS ? 1 : 0, 
               "Show Hidden Directories", o);
  e_add_sswstr(5, 5, 2, AltK, 
               f->ed->flopt & FM_REKURSIVE_ACTIONS ? 1 : 0, 
               "ReKursive Actions      ", o);
  e_add_sswstr(5, 17, 0, AltC, 
               f->ed->flopt & FM_CLOSE_WINDOW ? 1 : 0, 
               "Close File Manager     ", o);

  e_add_sswstr(36, 6, 0, AltR, 
               f->ed->flopt & FM_REVERSE_ORDER ? 1 : 0, 
               "Reverse Order    ", o);

  e_add_pswstr(0, 36, 14, 0, AltP, 0, "Prompt for Delete", o);
  e_add_pswstr(0, 36, 15, 10, AltE, 0, "Delete at Exit   ", o);
  e_add_pswstr(0, 36, 16, 8, AltL, f->ed->flopt & FM_PROMPT_DELETE ? 0 :
               (f->ed->flopt & FM_DELETE_AT_EXIT ? 1 : 2), "Don't DeLete     ", o);
  e_add_pswstr(1, 36, 3, 0, AltN, 0, "Name             ", o);
  e_add_pswstr(1, 36, 4, 1, AltI, 0, "TIme             ", o);
  e_add_pswstr(1, 36, 5, 0, AltB, f->ed->flopt & FM_SORT_NAME ? 1 :
               (f->ed->flopt & FM_SORT_TIME ? 2 : 0), "Bytes            ", o);
  e_add_pswstr(2, 5, 8, 12, AltQ, 0, "Prompt for eQual Files ", o);
  e_add_pswstr(2, 5, 9, 1, AltV, 0, "OVerwrite equal Files  ", o);
  e_add_pswstr(2, 5, 10, 4, AltT, f->ed->flopt & FM_MOVE_PROMPT ? 0 :
               (f->ed->flopt & FM_MOVE_OVERWRITE ? 1 : 2), "Don'T overwrite        ", o);
  e_add_pswstr(3, 5, 13, 4, AltH, 0, "Try Hardlink           ", o);
  e_add_pswstr(3, 5, 14, 7, AltS, 
               f->ed->flopt & FM_TRY_HARDLINK ? 1 : 0,
               "Always Symbolic Link   ", o);
  e_add_pswstr(4, 36, 9, 5, AltW, 0, "into Wastebasket ", o);
  e_add_pswstr(4, 36, 10, 0, AltA, 0, "Absolute (Prompt)", o);
  e_add_pswstr(4, 36, 11, 6, AltM, f->ed->flopt & FM_REMOVE_INTO_WB ? 0 :
               (f->ed->flopt & FM_REMOVE_PROMPT ? 1 : 2), "No ProMpt        ", o);
  e_add_bttstr(16, 19, 1, AltO, " Ok ", NULL, o);
  e_add_bttstr(38, 19, -1, WPE_ESC, "Cancel", NULL, o);

  ret = e_opt_kst(o);
  if(ret != WPE_ESC)
  {
    f->ed->flopt = o->sstr[0]->num +
      (o->sstr[1]->num << 1) +
      (o->sstr[2]->num ? FM_REKURSIVE_ACTIONS : 0) +
      (o->sstr[3]->num ? FM_CLOSE_WINDOW : 0) +
      (o->sstr[4]->num ? FM_REVERSE_ORDER : 0) +
      (o->pstr[0]->num ? (o->pstr[0]->num == 1 ? 4 : 0) : 8) +
      (o->pstr[2]->num ? (o->pstr[2]->num == 1 ? 16 : 0) : 32) +
      (o->pstr[4]->num ? (o->pstr[4]->num == 1 ? 128 : 0) : 64) +
      (o->pstr[1]->num ? (o->pstr[1]->num == 1 ? 01000 : 02000) : 0) +
      (o->pstr[3]->num ? FM_TRY_HARDLINK : 0);
  }

  freeostr(o);
  return(0);
}

int WpeDirDelOptions(FENSTER * f)
{
  int             ret;
  W_OPTSTR       *o = e_init_opt_kst(f);

  if(o == NULL)
    return(1);

  o->xa = 19;
  o->ya = 11;
  o->xe = 53;
  o->ye = 19;
  o->bgsw = AltO;
  o->name = "Message";
  o->crsw = AltO;
  e_add_txtstr(4, 2, "Delete Directory:", o);
  e_add_pswstr(0, 5, 3, 0, AltD, 0, "Delete without Prompt", o);
  e_add_pswstr(0, 5, 4, 0, AltP, 1, "Prompt for Files     ", o);
  e_add_bttstr(7, 6, 1, AltO, " Ok ", NULL, o);
  e_add_bttstr(22, 6, -1, WPE_ESC, "Cancel", NULL, o);

  ret = e_opt_kst(o);

  ret = (ret == WPE_ESC) ? -1 : o->pstr[0]->num;

  freeostr(o);
  return(ret);
}

/* The key names of File > Shell and of the file manager's Execute. */
#define E_SHELL_PLAN_NAME   "shell"
#define E_EXECUTE_PLAN_NAME "execute"
#define E_EXECUTE_SOURCE    "the file manager's file"
#define E_EXECUTE_HERE      "."
#define E_EXECUTE_PATH_MAX  512   /* "./" and a name of the file manager's */

/**
 * e_shell_plan - The plan of File > Shell: the user's own shell, in the
 * editor's folder.
 * Return: the judged plan (the caller frees it), or NULL out of memory.
 */
static WpeStartPlan *e_shell_plan(void)
{
  WpeStartPlan *plan = wpe_startplan_new(WPE_START_SHELL, E_SHELL_PLAN_NAME);

  if (!plan)
    return(NULL);
  wpe_startplan_set_cwd(plan, E_EXECUTE_HERE);
  wpe_startplan_word(plan, user_shell, WPE_WORD_PROGRAM, WPE_FROM_USER, NULL);
  wpe_startplan_judge(plan);
  return(plan);
}

/**
 * e_execute_plan - The plan of Execute in the file manager.
 * @name: the file's name, as the user picked or typed it.
 *
 * The file runs as a program of its own, "./<name>" (an absolute name as
 * typed), with no shell between:
 * a file named "a';touch x;'.c" runs that file (or fails to), and nothing
 * else.  The user chose it, so it starts without a question; the box would
 * say only that it runs the folder's own program.
 * Return: the judged plan (the caller frees it), or NULL when the name
 * does not fit or out of memory.
 */
static WpeStartPlan *e_execute_plan(const char *name)
{
  WpeStartPlan *plan = wpe_startplan_new(WPE_START_SHELL, E_EXECUTE_PLAN_NAME);
  char path[E_EXECUTE_PATH_MAX];

  if (!plan)
    return(NULL);
  if (wpe_startplan_operand_in(path, sizeof path, E_EXECUTE_HERE, name))
  {
    wpe_startplan_free(plan);
    return(NULL);
  }
  wpe_startplan_set_cwd(plan, E_EXECUTE_HERE);
  wpe_startplan_word(plan, path, WPE_WORD_OPERAND, WPE_FROM_USER, E_EXECUTE_SOURCE);
  wpe_startplan_path(plan, path, WPE_PATH_INPUT, WPE_FROM_USER, E_EXECUTE_SOURCE, NULL);
  plan->honest = 1;
  wpe_startplan_judge(plan);
  return(plan);
}

/**
 * e_execute_file - Execute the file the file manager names.
 * @name: its name.
 * Return: the command's wait status, or -1 when it could not be started.
 */
static int e_execute_file(const char *name)
{
  WpeStartPlan *plan = e_execute_plan(name);
  int ret = -1;

  if (plan)
    ret = (*e_u_system)(plan);
  wpe_startplan_free(plan);
  return(ret);
}

int WpeShell(FENSTER * f)
{
  PIC            *outp = NULL;
  WpeStartPlan   *plan;
  int             g[4];

  if (!WpeIsXwin())
  {
    outp = e_open_view(0, 0, MAXSCOL - 1, MAXSLNS - 1, f->fb->ws, 1);
    fk_locate(0, 0);
    fk_cursor(1);
#if  MOUSE
    g[0] = 2;
    fk_mouse(g);
#endif
    (*e_u_s_sys_ini) ();
  }
  if ((plan = e_shell_plan()) != NULL)
    (*e_u_system) (plan);
  wpe_startplan_free(plan);
  if(!WpeIsXwin())
  {
    (*e_u_s_sys_end) ();
    e_close_view(outp, 1);
    fk_cursor(0);
#if  MOUSE
    g[0] = 1;
    fk_mouse(g);
#endif
  }
  return(0);
}

/*   print file */
#ifndef NOPRINTER
/* The longest the print command may take to hand the file over. */
#define E_PRINT_MS      60000L
#define E_PRINT_OUT_MAX 256      /* what it says, read and dropped */
#define E_PRINT_WHAT    "the print command"
/* What e_print_run answers. */
#define E_PRINT_FAILED  0
#define E_PRINT_DONE    1
#define E_PRINT_STOPPED 2
/* The user's print command as shell text, run in the folder given as $1 on
   the file given as $2: the editor's own words never enter the text. */
#define E_PRINT_SCRIPT_FMT "cd \"$1\" && %s \"$2\""

/* The word of the print command line that holds the print command. */
#define E_PRINT_SCRIPT_WORD 2

/**
 * e_print_plan - The plan of a print, each word labelled by whose it is.
 * @argv: the print command line; @cmd: the print command in force.
 *
 * The shell and its arguments are the editor's; the print command is the
 * user's own, or the folder's when a start-folder xwperc named it.  A
 * folder's print command reaches here only after the print question was
 * answered with Run it (e_print_approve), and that answer is the plan's.
 * Return: the plan (the caller frees it), or NULL out of memory.
 */
static WpeStartPlan *e_print_plan(char *const argv[], const char *cmd)
{
 WpeStartPlan *plan = wpe_startplan_fixed(WPE_START_PRINT, WPE_STARTCMD_KEY_PRINT, argv);
 int from = WPE_FROM_USER;

 if (!plan)
  return(NULL);
 if (e_opt_start_folder() && !wpe_startcmd_is_own(WPE_STARTCMD_KEY_PRINT, cmd))
  from = WPE_FROM_FOLDER;
 wpe_startplan_relabel(plan, E_PRINT_SCRIPT_WORD, WPE_WORD_TEXT, from);
 if (from == WPE_FROM_FOLDER)
  wpe_startplan_answered(plan, WPE_PLAN_RUN);
 return(plan);
}

/**
 * e_print_run - Run the user's print command on a file.
 * @dir: where the file is; @cmd: the print command from the options, shell
 * text as the user wrote it; @file: the file's name.
 *
 * The command is the user's own setting, so it stays shell text; the file's
 * name goes to it as an argument, so a name with a space or a ';' prints
 * that file and nothing else.  The editor goes on serving its loop while the
 * command hands the file over.
 * Esc ends the wait: the file is then not printed and the status row says
 * the wait was stopped, which is not a command that failed.
 * Return: E_PRINT_DONE when the command ended with status 0,
 * E_PRINT_STOPPED when the user pressed Esc, else E_PRINT_FAILED.
 */
static int e_print_run(const char *dir, const char *cmd, const char *file)
{
 char out[E_PRINT_OUT_MAX];
 char *script, *argv[7];
 size_t size = strlen(E_PRINT_SCRIPT_FMT) + strlen(cmd) + 1;
 WpeStartPlan *plan;
 int status = 0, ran;

 if (!(script = MALLOC(size)))
  return(E_PRINT_FAILED);
 snprintf(script, size, E_PRINT_SCRIPT_FMT, cmd);
 argv[0] = "sh";
 argv[1] = "-c";
 argv[2] = script;
 argv[3] = "sh";               /* $0 */
 argv[4] = (char *)dir;        /* $1 */
 argv[5] = (char *)file;       /* $2 */
 argv[6] = NULL;
 plan = e_print_plan(argv, cmd);
 ran = wpe_startplan_capture(plan, out, sizeof out, E_PRINT_MS, &status, E_PRINT_WHAT);
 wpe_startplan_free(plan);
 FREE(script);
 if (ran == WPE_CAPTURE_CANCELLED)
  return(E_PRINT_STOPPED);
 return(wpe_capture_succeeded(ran, status) ? E_PRINT_DONE : E_PRINT_FAILED);
}

/* What the question calls the print command. */
#define E_PRINT_WHAT_ASKED "print command"

/**
 * e_print_approve - Ask before the print command a start-folder xwperc names runs.
 * @f: the window being printed.
 *
 * Printing hands the file to a shell command, so a cloned folder's xwperc
 * could run anything at the user's first print; it is shown first unless
 * it is his own, the default, or answered for in this folder.  "Use mine"
 * puts his own command in force.
 * Return: 0 to print with f->ed->print_cmd, -1 to print nothing.
 */
static int e_print_approve(FENSTER *f)
{
 ECNT *cn = f->ed;
 char *own = WpeStrdup(e_opt_own_print_cmd(cn));
 E_STARTCMD c;
 int verdict;

 c.folder = e_opt_start_folder();
 c.source = E_STARTCMD_XWPERC;
 c.who = E_STARTCMD_XWPERC_WHO;
 c.key = WPE_STARTCMD_KEY_PRINT;
 c.what = E_PRINT_WHAT_ASKED;
 c.text = cn->print_cmd;
 c.own = own;
 verdict = e_startcmd_approve(f, &c);
 if (verdict == WPE_STARTCMD_USE_OWN)
 {
  WpeFree(cn->print_cmd);
  cn->print_cmd = own;
  own = NULL;
 }
 if (own)
  WpeFree(own);
 if (verdict == WPE_STARTCMD_CANCEL)
  return(-1);
 if (strcmp(cn->print_cmd, "") == 0)
 {
  e_error(e_msg[ERR_NOPRINT], 0, f->fb);
  return(-1);
 }
 return(0);
}

/**
 * WpePrintMakeCopy - Write the copy of the window the print command reads.
 * @f: the text window to print.
 * Return: 0 when the copy is in the temp folder, -1 when it is not (the
 * user has been told, and nothing is printed).
 *
 * The copy is written as autosave writes: the file on disk, the window's
 * modified mark and its NUL question are not touched.  A read-only window
 * is printed like any other.
 */
static int WpePrintMakeCopy(FENSTER *f)
{
 WpeWriteOutcome o;
 char           *path = e_mkfilename(e_tmp_dir, f->datnam);
 int             window_ins = f->ins, rc;

 if (path == NULL)
  return(-1);
 f->ins = 0;
 rc = wpe_write_range(f, path, 0, 0, E_BUFFER_ROW_END(f->b), f->b->mxlines - 1, 0, &o);
 f->ins = window_ins;
 FREE(path);
 if (rc != 0)
  wpe_write_say(f, &o, WPE_WRITE_THEN_NO_PRINT);
 return(rc);
}

int WpePrintFile(FENSTER *f)
{
 char           *str;
 size_t          size;
 int             c;

 for (c = f->ed->mxedt; c > 0 && !DTMD_ISTEXT(f->ed->f[c]->dtmd); c--)
  ;
 if (c <= 0)
   return(0);
 f = f->ed->f[c];

 if (strcmp(f->ed->print_cmd, "") == 0)
 {
  return(e_error(e_msg[ERR_NOPRINT], 0, f->fb));
 }
 size = strlen(f->datnam) + 32;
 if ((str = MALLOC(size)) == NULL)
  e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

 snprintf(str, size, "File: %s\nDo you want to print it?", f->datnam);
 c = e_message(1, str, f);
 FREE(str);
 if (c != 'Y')
  return(0);
 if (e_print_approve(f))
  return(0);

 if (WpePrintMakeCopy(f) != 0)
  return(0);

 if (e_print_run(e_tmp_dir, f->ed->print_cmd, f->datnam) == E_PRINT_FAILED)
  e_error(e_msg[ERR_NOTINSTALL], 0, f->fb);

 size = strlen(e_tmp_dir) + strlen(f->datnam) + 2;
 if ((str = MALLOC(size)) == NULL)
  e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);
 snprintf(str, size, "%s/%s", e_tmp_dir, f->datnam);
 if (remove(str))
  e_error(e_msg[ERR_DELFILE], 0, f->ed->fb);

 FREE(str);
 return(0);
}
#else
int WpePrintFile(FENSTER * f)
{
  return(e_error(e_msg[ERR_NOPRINT], 0, f->fb));
}
#endif

struct dirfile *WpeSearchFiles(FENSTER *f, char *dirct, char *file, const FIND *fd,
                               struct dirfile *df)
{
  int             sw = fd->sw;
  struct dirfile *dd;
  char          **tname, *tp, *tmp, *tmp2;
  int             i;
  static int      rec = 0;

  if(rec > MAXREC)
    return(df);

  /* if not absolute path is given */
  if(*dirct != DIRC)
  {
    tmp2 = WpeGetCurrentDir(f->ed);

    tmp = REALLOC(tmp2, strlen(tmp2) + strlen(dirct) + 4);
    if(tmp == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

    tmp2 = tmp;
    if(tmp2[strlen(tmp2) - 1] != DIRC)
      sprintf(tmp2 + strlen(tmp2), "%c%s%c", DIRC, dirct, DIRC);
    else
      sprintf(tmp2 + strlen(tmp2), "%s%c", dirct, DIRC);
  }
  else
  {
    if((tmp2 = MALLOC(strlen(dirct) + 2)) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

    if(dirct[strlen(dirct) - 1] != DIRC)
      sprintf(tmp2, "%s%c", dirct, DIRC);
    else
      sprintf(tmp2, "%s", dirct);
  }

  /* assemble total path, dir + filename */
  if((tmp = MALLOC(strlen(tmp2) + strlen(file) + 2)) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);
  sprintf(tmp, "%s%s", tmp2, file);

  /* initialise structure, only once */
  if(df == NULL)
  {
    if((df = MALLOC(sizeof(struct dirfile))) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

    df->anz = 0;
    if((df->name = MALLOC(sizeof(char *))) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);
  }

  /* search all matching file */
  dd = e_find_files(tmp, 0);

  /* if we found something */
  if(dd && dd->anz > 0)
  {

    /* file find */
    if(!(sw & 1024))
    {
      for(i = 0; i < dd->anz; i++)
      {
        if((tp = MALLOC(strlen(tmp2) + strlen(dd->name[i]) + 2)) == NULL)
          e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

        sprintf(tp, "%s%s", tmp2, dd->name[i]);

        df->anz++;

        if((tname = REALLOC(df->name, df->anz * sizeof(char *))) == NULL)
          e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

        df->name = tname;
        df->name[df->anz - 1] = tp;
      }
    }
    /* file grep */
    else
    {
      for(i = 0; i < dd->anz; i++)
      {
        if((tp = MALLOC(strlen(tmp2) + strlen(dd->name[i]) + 2)) == NULL)
          e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

        sprintf(tp, "%s%s", tmp2, dd->name[i]);

        if(WpeGrepFile(tp, fd) > 0)
        {
          df->anz++;
          if((tname = REALLOC(df->name, df->anz * sizeof(char *))) == NULL)
            e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

          df->name = tname;
          df->name[df->anz - 1] = tp;
        }
        else
          FREE(tp);
      }
    }
  }

  freedf(dd);
  FREE(tmp2);
  FREE(tmp);

  /* whether recursive action */
  if(!(sw & 512))
    return(df);

  if((tmp = MALLOC(strlen(dirct) + strlen(SUDIR) + 2)) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, f->ed->fb);

  if(dirct[strlen(dirct) - 1] != DIRC)
    sprintf(tmp, "%s%c%s", dirct, DIRC, SUDIR);
  else
    sprintf(tmp, "%s%s", dirct, SUDIR);

  /* find directories */
  dd = e_find_dir(tmp, 0);

  FREE(tmp);

  if(!dd)
    return(df);

  rec++;
  for(i = 0; i < dd->anz; i++)
  {
    if((tmp = MALLOC(strlen(dirct) + strlen(dd->name[i]) + 3)) == NULL)
    {
      e_error(e_msg[ERR_LOWMEM], 0, f->ed->fb);
      rec--;
      freedf(dd);
      return(df);
    }

    if(dirct[strlen(dirct) - 1] != DIRC)
      sprintf(tmp, "%s%c%s", dirct, DIRC, dd->name[i]);
    else
      sprintf(tmp, "%s%s", dirct, dd->name[i]);

    df = WpeSearchFiles(f, tmp, file, fd, df);
    FREE(tmp);
  }

  freedf(dd);
  rec--;
  return(df);

}


/**
 * e_grep_say_bad_text - Tell the user why a Grep listed no file when its
 * Find text is a regular expression that does not compile.
 *
 * An empty list and a mistyped expression then do not look alike.
 */
static void e_grep_say_bad_text(FENSTER *f, const FIND *fd)
{
  WpeRowHit       hit;

  if(wpe_rowfind_row(fd, (const unsigned char *)"", 0,
                     WPE_ROW_STARTS_LINE | WPE_ROW_ENDS_LINE, 0, 0, 0, &hit) < 0)
    e_message(0, (char *)wpe_rowfind_error(), f);
}

int WpeGrepWindow(FENSTER * f)
{
  FIND           *fd = &(f->ed->fd);
  int             ret;
  char            strTemp[80];
  W_OPTSTR       *o = e_init_opt_kst(f);

  if(!o)
    return(-1);
  if(e_blck_dup(strTemp, f))
    strcpy(fd->search, strTemp);
  o->xa = 7;
  o->ya = 3;
  o->xe = 63;
  o->ye = 19;
  o->bgsw = 0;
  o->name = "Grep";
  o->crsw = AltO;
  e_add_txtstr(4, 4, "Options:", o);
  e_add_wrstr(4, 2, 18, 2, 35, E_FIND_TEXT_MAX, 0, AltT, "Text to Find:", fd->search, &f->ed->sdf, o);
  e_add_wrstr(4, 10, 17, 10, 36, WPE_PATH_FIELD_MAX, 0, AltF, "File:", fd->file, &f->ed->fdf, o);
  e_add_wrstr(4, 12, 17, 12, 36, WPE_PATH_FIELD_MAX, 0, AltD, "Directory:", fd->dirct, &f->ed->ddf, o);
  e_add_sswstr(5, 5, 0, AltC, fd->sw & 128 ? 1 : 0, "Case sensitive    ", o);
  e_add_sswstr(5, 6, 0, AltW, fd->sw & 64 ? 1 : 0, "Whole words only  ", o);
  e_add_sswstr(5, 7, 0, AltR, fd->sw & 32 ? 1 : 0, "Regular expression", o);
  e_add_sswstr(5, 8, 0, AltS, 0, "Search Recursive  ", o);
  e_add_bttstr(16, 14, 1, AltO, " Ok ", NULL, o);
  e_add_bttstr(34, 14, -1, WPE_ESC, "Cancel", NULL, o);
  ret = e_opt_kst(o);
  if(ret != WPE_ESC && (e_opt_field_take(o, 0, fd->search, sizeof fd->search) != 0 ||
                        e_opt_field_take(o, 1, fd->file, sizeof fd->file) != 0))
    ret = WPE_ESC;
  if(ret != WPE_ESC)
  {
    fd->sw = 1024 + (o->sstr[0]->num << 7) + (o->sstr[1]->num << 6) +
      (o->sstr[2]->num << 5) + (o->sstr[3]->num << 9);
    wpe_rowfind_forget_resume();
    if (fd->dirct)
    {
     WpeFree(fd->dirct);
    }
    fd->dirct = WpeStrdup(o->wstr[2]->txt);
  }
  freeostr(o);
  if(ret != WPE_ESC)
  {
    ret = e_data_first(2, f->ed, fd->dirct);
    e_grep_say_bad_text(f, fd);
  }
  return(ret);
}


int WpeFindWindow(FENSTER * f)
{
  FIND           *fd = &(f->ed->fd);
  int             ret;
  W_OPTSTR       *o = e_init_opt_kst(f);

  if(!o)
    return(-1);
  o->xa = 7;
  o->ya = 3;
  o->xe = 61;
  o->ye = 14;
  o->bgsw = 0;
  o->name = "Find File";
  o->crsw = AltO;
  e_add_txtstr(4, 6, "Options:", o);
  e_add_wrstr(4, 2, 15, 2, 36, WPE_PATH_FIELD_MAX, 0, AltF, "File:", fd->file, &f->ed->fdf, o);
  e_add_wrstr(4, 4, 15, 4, 36, WPE_PATH_FIELD_MAX, 0, AltD, "Directory:", fd->dirct, &f->ed->ddf, o);
  e_add_sswstr(5, 7, 0, AltS, 1, "Search Recursive  ", o);
  e_add_bttstr(13, 9, 1, AltO, " Ok ", NULL, o);
  e_add_bttstr(33, 9, -1, WPE_ESC, "Cancel", NULL, o);
  ret = e_opt_kst(o);
  if(ret != WPE_ESC && e_opt_field_take(o, 0, fd->file, sizeof fd->file) != 0)
    ret = WPE_ESC;
  if(ret != WPE_ESC)
  {
    fd->sw = (o->sstr[0]->num << 9);
    if (fd->dirct)
    {
     WpeFree(fd->dirct);
    }
    fd->dirct = WpeStrdup(o->wstr[1]->txt);
  }
  freeostr(o);
  if(ret != WPE_ESC)
    ret = e_data_first(3, f->ed, fd->dirct);
  return(ret);
}






















/* The longest the manual page program may take to write a page. */
#define E_MAN_MS        30000L
#define E_MAN_WORD_MAX  80       /* the page's name, with its end */
#define E_MAN_SECT_MAX  10       /* the section's name, with its end */
#define E_MAN_WHAT      "the manual page"
/* What e_man_run answers. */
#define E_MAN_WRITTEN   0
#define E_MAN_STOPPED   1

/**
 * e_man_split - Take the page's name and its section out of a word such as
 * "printf(3)".
 * @str: what the user asked help for; @hstr, @hsize: receive the name;
 * @nstr, @nsize: receive the section, empty when there is none.
 *
 * A word longer than the room is cut: help on any text the user selects
 * must not write past the end of the editor's own buffers.
 */
static void e_man_split(const char *str, char *hstr, size_t hsize,
                        char *nstr, size_t nsize)
{
  size_t i = 0, h = 0, j = 0;

  for(; str[i] && str[i] != '(' && str[i] != ')'; i++)
    if(h + 1 < hsize)
      hstr[h++] = str[i];
  hstr[h] = '\0';
  nstr[0] = '\0';
  if(str[i] != '(')
    return;
  for(++i; str[i] && str[i] != ')' && str[i] != '('; i++)
    if(j + 1 < nsize)
      nstr[j++] = str[i];
  nstr[j] = '\0';
  /* Some SEE ALSO's are list as "foobar(3X)" but are in section 3 not 3X.
     This is a quick hack to fix the problem.  -- Dennis */
  if(isdigit((unsigned char)nstr[0]))
    nstr[1] = '\0';
}

/**
 * e_man_outfile - The file in the editor's temp folder the page is written
 * into, named after what the user asked for.
 * @tstr, @size: receive the path; @str: the word.
 *
 * A '/' in the word becomes '_', so the page can never be written outside
 * the temp folder, and a word longer than a file name is cut (the name is
 * only a label for the page).  A temp folder too long to hold the file is
 * refused, never cut: the page would be written to another folder.
 * Return: 0, or -1 with @tstr empty when the path does not fit.
 */
static int e_man_outfile(char *tstr, size_t size, const char *str)
{
  char name[WPE_NAME_SIZE];
  size_t n;

  for(n = 0; *str && n + 1 < sizeof name; str++, n++)
    name[n] = (*str == '/') ? '_' : *str;
  name[n] = '\0';
  return(wpe_path_join(tstr, size, e_tmp_dir, name));
}

/**
 * e_man_run_plan - Run man's command line through its start plan.
 * @argv: man's command line; @tstr: the file the page goes to; @status:
 * receives its status.
 * Return: as wpe_startplan_run_to_file.
 */
static int e_man_run_plan(char *const argv[], const char *tstr, int *status)
{
 WpeStartPlan *plan = wpe_startplan_fixed(WPE_START_MAN, E_MAN_WHAT, argv);
 int ran;

 *status = 0;
 ran = wpe_startplan_run_to_file(plan, tstr, E_MAN_MS, status, E_MAN_WHAT);
 wpe_startplan_free(plan);
 return(ran);
}

/**
 * e_man_run - Write the manual page into @tstr.
 * @nstr: the section, or ""; @hstr: the page's name; @tstr: the file.
 *
 * `man` gets the name as an argument, so no word the user selected is read
 * as shell text, and the editor serves its loop while the page is made.  A
 * name that could be taken for an option, or a program that fails or hangs,
 * leaves the file empty, which the caller shows as "No manual entry".  Esc
 * ends the wait instead: the user chose not to wait, which says nothing
 * about the page, so the caller shows no window.
 * Return: E_MAN_STOPPED when the user pressed Esc, else E_MAN_WRITTEN.
 */
static int e_man_run(const char *nstr, const char *hstr, const char *tstr)
{
  char *argv[6];
  int n = 0, status, ran;
  FILE *fp;

  if(hstr[0] != '-')
  {
    argv[n++] = "man";
#ifdef MAN_S_OPT
    if(nstr[0])
    {
      argv[n++] = "-s";
      argv[n++] = (char *)nstr;
    }
#else
    if(nstr[0])
      argv[n++] = (char *)nstr;
#endif
    argv[n++] = (char *)hstr;
    argv[n] = NULL;
    ran = e_man_run_plan(argv, tstr, &status);
    if(ran == WPE_CAPTURE_CANCELLED)
      return(E_MAN_STOPPED);
    if(ran == WPE_CAPTURE_ENDED)
      return(E_MAN_WRITTEN);
  }
  if((fp = fopen(tstr, "w")))
    fclose(fp);
  return(E_MAN_WRITTEN);
}

/**
 * e_man_give_up - Leave Help on a word after the user pressed Esc.
 * @tstr: the page's file, partly written or empty.
 *
 * Takes the file away and gives the pointer its shape back, so the editor
 * is as it was before the user asked.
 */
static void e_man_give_up(const char *tstr)
{
  chmod(tstr, 0600);
  remove(tstr);
  WpeMouseRestoreShape();
}

int e_ed_man(char *str, FENSTER * f)
{
  char            tstr[WPE_PATH_SIZE], msg[WPE_PATH_MSG_MAX];
  char            cc, hstr[E_MAN_WORD_MAX], nstr[E_MAN_SECT_MAX];
  int             mdsv = f->ed->dtmd, bg, i, j = 0, opened;
  BUFFER         *b;

  if(!str)
    return(0);
  while(isspace(*str++));
  if(!*--str)
    return(0);
  for(i = f->ed->mxedt; i >= 0; i--)
  {
    if(!strcmp(f->ed->f[i]->datnam, str))
    {
      e_switch_window(f->ed->edt[i], f);
      return(0);
    }
  }
  WpeMouseChangeShape(WpeWorkingShape);
  if(e_man_outfile(tstr, sizeof tstr, str) != 0)
  {
    snprintf(msg, sizeof msg, WPE_PATH_TOO_LONG_FMT, "temporary folder", WPE_PATH_MAX);
    e_error(msg, 0, f->fb);
    WpeMouseRestoreShape();
    return(0);
  }
  e_man_split(str, hstr, sizeof hstr, nstr, sizeof nstr);

  while(1)
  {
    if(e_man_run(nstr, hstr, tstr) == E_MAN_STOPPED)
    {
      e_man_give_up(tstr);
      return(0);
    }
    chmod(tstr, 0400);
    f->ed->dtmd = DTMD_HELP;
    opened = e_edit(f->ed, tstr);
    f->ed->dtmd = mdsv;
    if(opened != 0)
    {
      e_man_give_up(tstr);       /* no page window: the user's window is untouched */
      return(0);
    }
    f = f->ed->f[f->ed->mxedt];
    b = f->b;
    e_buffer_mark_view(b);
    if(b->mxlines > 1 || !nstr[1])
      break;
    nstr[1] = '\0';
    chmod(tstr, 0600);
    remove(tstr);
    e_close_window(f);
  }
  if(b->mxlines == 1 && b->bf[0].len == 0)
  {
    e_ins_nchar(f->b, f->s, "No manual entry for ", 0, 0, 20);
    e_ins_nchar(f->b, f->s, hstr, b->b.x, b->b.y, strlen(hstr));
    e_ins_nchar(f->b, f->s, ".", b->b.x, b->b.y, 1);
  }
  for(i = 0; i < b->mxlines; i++)
    if(b->mxlines > 1 && b->bf[i].len == 0 && (i == 0 || b->bf[i - 1].len == 0))
    {
      e_buffer_delete_lines(b, i, 1, E_BUFFER_STEP_NEW);
      i--;
    }
  for(bg = 0; bg < b->bf[0].len && isspace(b->bf[0].s[bg]); bg++);
  if(bg == b->bf[0].len)
    bg = 0;
  for(i = 0;
      i < b->mxlines &&
      WpeStrnccmp(b->bf[i].s + bg, "\017SEE\005 \017ALSO\005", 12) &&
      WpeStrnccmp(b->bf[i].s + bg, "SEE ALSO", 8);
      i++);
  if(i < b->mxlines)
    for(bg = 0, i++; i < b->mxlines && b->bf[i].len > 0 && bg >= 0; i++)
    {
      bg = 0;
      while(b->bf[i].s[bg])
      {
        for(; isspace(b->bf[i].s[bg]); bg++);
        if(!b->bf[i].s[bg])
          continue;
        for(j = bg + 1;
            b->bf[i].s[j] && b->bf[i].s[j] != ',' && b->bf[i].s[j] != '.' &&
            b->bf[i].s[j] != ' ' && b->bf[i].s[j] != '(';
            j++);
        if(b->bf[i].s[j] != '(')
        {
          bg = -1;
          break;
        }
        if(b->bf[i].s[j - 1] == 5)
          e_del_nchar(b, f->s, j - 1, i, 1);
        for(j++; b->bf[i].s[j] && b->bf[i].s[j] != ',' && b->bf[i].s[j] != '.';
            j++);
        if(b->bf[i].s[bg] == 15)
          b->bf[i].s[bg] = HFB;
        else
        {
          cc = HFB;
          e_ins_nchar(b, f->s, &cc, bg, i, 1);
          j++;
        }
        cc = HED;
        e_ins_nchar(b, f->s, &cc, j, i, 1);
        j++;
        if(b->bf[i].s[j])
          j++;
        bg = j;
      }
    }
  e_buffer_cursor_set(b, 0, 0);
  chmod(tstr, 0600);
  remove(tstr);
  WpeMouseRestoreShape();
  e_schirm(f, 1);
  return(0);
}

int e_funct(FENSTER * f)
{
 char            str[E_ARG_FIELD_MAX + 1];

 if (f->ed->hdf && f->ed->hdf->anz > 0)
  snprintf(str, sizeof str, "%s", f->ed->hdf->name[0]);
 else
  str[0] = '\0';
 if (e_add_arguments(str, sizeof str, "Function", f, 0, AltF, &f->ed->hdf))
 {
  f->ed->hdf = e_add_df(str, f->ed->hdf);
  e_ed_man(str, f);
 }
 return(0);
}

struct dirfile *e_make_funct(char *man)
{
 struct dirfile *df = NULL, *dout = MALLOC(sizeof(struct dirfile));
 char *sustr, *subpath, *manpath;
 int ret = 0, n, i = 0, j, k, l = 0;

 manpath = NULL;
 WpeMouseChangeShape(WpeWorkingShape);
 dout->anz = 0;
 dout->name = NULL;
 if (getenv("MANPATH"))
 {
  manpath = strdup(getenv("MANPATH"));
 }
 if ((!manpath) || (manpath[0] == '\0'))
 {
  manpath = strdup("/usr/man:/usr/share/man:/usr/X11R6/man:/usr/local/man");
 }
 /* Allocate the maximum possible rather than continually realloc. */
 sustr = malloc(strlen(manpath) + 10);
 while (manpath[i])
 {
  subpath = manpath + i;
  for (n = 0; (manpath[i]) && (manpath[i] != PTHD); i++, n++);
  if (manpath[i] == PTHD)
  {
   manpath[i] = '\0';
   i++;
  }
  sprintf(sustr, "%s/man%s/*", subpath, man);
  df = e_find_files(sustr, 0);
  if (!df->anz)
  {
   freedf(df);
   continue;
  }
  for (j = 0; j < df->anz; j++)
  {
   k = strlen(df->name[j]) - 1;

   /* If the file is gzipped strip the .gz ending. */
   if ((k > 2) && (strcmp(df->name[j] + k - 2, ".gz") == 0))
   {
    k -= 3;
    *(df->name[j] + k + 1) = '\0';
   }
   else if ((k > 3) && (strcmp(df->name[j] + k - 3, ".bz2") == 0))
   {
    k -= 4;
    *(df->name[j] + k + 1) = '\0';
   }
   else if ((k > 1) && (strcmp(df->name[j] + k - 1, ".Z") == 0))
   {
    k -= 2;
    *(df->name[j] + k + 1) = '\0';
   }

   for (k = strlen(df->name[j]) - 1; k >= 0 && *(df->name[j] + k) != '.'; k--)
    ;
   if (k >= 0 /**(df->name[j]+k)*/ )
   {
    df->name[j] = REALLOC(df->name[j],
      (l = strlen(df->name[j]) + 2) * sizeof(char));
    *(df->name[j] + k) = '(';
    *(df->name[j] + l - 2) = ')';
    *(df->name[j] + l - 1) = '\0';
   }
  }
  if (!dout->name)
   dout->name = MALLOC(df->anz * sizeof(char *));
  else
   dout->name = REALLOC(dout->name, (df->anz + dout->anz) * sizeof(char *));
  for (j = 0; j < df->anz; j++)
  {
   for (k = 0; k < dout->anz; k++)
   {
    if (!(ret = strcmp(df->name[j], dout->name[k])))
    {
     FREE(df->name[j]);
     break;
    }
    else if (ret < 0)
     break;
   }
   if (!ret && dout->anz)
    continue;
   for (l = dout->anz; l > k; l--)
    dout->name[l] = dout->name[l - 1];
   dout->name[k] = df->name[j];
   dout->anz++;
  }
  FREE(df);
 }
 free(manpath);
 free(sustr);
 WpeMouseRestoreShape();
 return(dout);
}


#ifdef PROG
extern struct dirfile **e_p_df;
#endif

int e_data_first(int sw, ECNT *cn, char *nstr)
{
  extern char    *e_hlp_str[];
  extern WOPT    *gblst, *oblst;
  FENSTER        *f;
  int             i, j;
  struct dirfile *df = NULL;
  FLWND          *fw;

  if(cn->mxedt >= MAXEDT)
  {
    e_error(e_msg[ERR_MAXWINS], 0, cn->fb);
    return(-1);
  }
  for(j = 1; j <= MAXEDT; j++)
  {
    for(i = 1; i <= cn->mxedt && cn->edt[i] != j; i++);
    if(i > cn->mxedt)
      break;
  }
  cn->curedt = j;
  (cn->mxedt)++;
  cn->edt[cn->mxedt] = j;

  if((f = (FENSTER *) MALLOC(sizeof(FENSTER))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
  if((fw = (FLWND *) MALLOC(sizeof(FLWND))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
  f->fb = cn->fb;
  cn->f[cn->mxedt] = f;
  f->a = e_set_pnt(22, 3);
  f->e = e_set_pnt(f->a.x + 35, f->a.y + 18);
  f->winnum = cn->curedt;
  f->dtmd = DTMD_DATA;
  f->ins = sw;
  f->save = 0;
  f->zoom = 0;
  f->ed = cn;
  f->c_sw = NULL;
  f->c_st = NULL;
  f->pic = NULL;
  f->fd.dirct = NULL;

  if(!nstr)
    f->dirct = NULL; /* how about a free up ??? */
  else
  {
    f->dirct = MALLOC(strlen(nstr) + 1);
    strcpy(f->dirct, nstr);
  }
  WpeMouseChangeShape(WpeWorkingShape);
  if(sw == 1)
  {
    f->datnam = "Function-Index";
    df = e_make_funct(nstr);
  }
  else if(sw == 2)
  {
    f->datnam = "Grep";
    df = WpeSearchFiles(f, nstr, cn->fd.file, &cn->fd, NULL);
  }
  else if(sw == 3)
  {
    f->datnam = "Find";
    df = WpeSearchFiles(f, nstr, cn->fd.file, &cn->fd, NULL);
  }
  else if(sw == 7)
  {
    f->datnam = "Windows";
    df = e_make_win_list(f);
  }
#ifdef PROG
  else if(sw == 4)
  {
    f->datnam = WpeStrdup("Project");
    df = e_p_df[0];
  }
  else if(sw == 5)
  {
    f->datnam = "Variables";
    df = e_p_df[1];
  }
  else if(sw == 6)
  {
    f->datnam = "Install";
    df = e_p_df[2];
  }
#endif
  f->hlp_str = e_hlp_str[16 + sw];
  if(sw < 4)
  {
    f->blst = gblst;
    f->nblst = 4;
  }
  else
  {
    f->blst = oblst;
    f->nblst = 5;
  }
  WpeMouseRestoreShape();
  f->b = (BUFFER *) fw;
  fw->df = df;

  fw->mxa = f->a.x;
  fw->mxe = f->e.x;
  fw->mya = f->a.y;
  fw->mye = f->e.y;
  fw->xa = f->a.x + 3;
  fw->xe = f->e.x - 13;
  fw->ya = f->a.y + 3;
  fw->ye = f->e.y - 1;
  fw->f = f;
  fw->ia = fw->nf = fw->nxfo = fw->nyfo = 0;
  fw->srcha = fw->ja = 0;

  if(cn->mxedt > 1 && (f->ins < 5 || f->ins == 7))
    e_ed_rahmen(cn->f[cn->mxedt - 1], 0);
  e_firstl(f, 1);
  e_data_schirm(f);
  return(0);
}

int e_data_schirm(FENSTER * f)
{
  int             i, j;
  FLWND          *fw = (FLWND *) f->b;

  for(j = f->a.y + 1; j < f->e.y; j++)
    for(i = f->a.x + 1; i < f->e.x; i++)
      e_pr_char(i, j, ' ', f->fb->nt.fb);

  if(NUM_COLS_ON_SCREEN > 25)
  {
    if(f->ins < 4 || f->ins == 7)
      e_pr_str((f->e.x - 9), f->e.y - 4, "Show", f->fb->nz.fb, 0, -1,
               f->fb->ns.fb, f->fb->nt.fb);
    else if(f->ins > 3)
    {
      e_pr_str((f->e.x - 9), f->e.y - 8, "Add", f->fb->nz.fb, 0, -1,
               f->fb->ns.fb, f->fb->nt.fb);
      e_pr_str((f->e.x - 9), f->e.y - 6, "Edit", f->fb->nz.fb, 0, -1,
               f->fb->ns.fb, f->fb->nt.fb);
      e_pr_str((f->e.x - 9), f->e.y - 4, "Delete", f->fb->nz.fb, 0, -1,
               f->fb->ns.fb, f->fb->nt.fb);
      if(f->ins == 4 && f->a.y < f->e.y - 10)
      {
        e_pr_str((f->e.x - 9), f->e.y - 10, "Options", f->fb->nz.fb,
                 0, -1, f->fb->ns.fb, f->fb->nt.fb);
      }
    }
    /* Project window auto-saves the .prj on close, so the exit button is
       labelled Save (Esc does the same -- consistent, no surprise). */
    e_pr_str((f->e.x - 9), f->e.y - 2, "Save", f->fb->nz.fb, -1, -1,
             f->fb->ns.fb, f->fb->nt.fb);
  }

  if(NUM_COLS_ON_SCREEN > 25)
  {
    fw->xa = f->a.x + 3;
    fw->xe = f->e.x - 13;
  }
  else
  {
    fw->xa = f->a.x + 3;
    fw->xe = f->e.x - 2;
  }
  fw->mxa = f->a.x;
  fw->mxe = f->e.x;
  fw->mya = f->a.y;
  fw->mye = f->e.y;
  fw->xa = f->a.x + 3;
  fw->ya = f->a.y + 3;
  fw->ye = f->e.y - 1;
#ifdef PROG
  if(f->ins == 4)
    fw->df = e_p_df[0];
#endif
  if(f->ins == 1)
    e_pr_str(fw->xa, f->a.y + 2, "Functions:", f->fb->nt.fb, 0, 1,
             f->fb->nsnt.fb, f->fb->nt.fb);
  else if(f->ins == 3)
    e_pr_str(fw->xa, f->a.y + 2, "Directories:", f->fb->nt.fb, 0, 1,
             f->fb->nsnt.fb, f->fb->nt.fb);
  e_mouse_bar(fw->xe, fw->ya, fw->ye - fw->ya, 0, fw->f->fb->em.fb);
  e_mouse_bar(fw->xa, fw->ye, fw->xe - fw->xa, 1, fw->f->fb->em.fb);
  e_pr_file_window(fw, 0, 1, f->fb->ft.fb, f->fb->fz.fb, f->fb->frft.fb);
  return(0);
}

/** What the list says when a key asks for the entry it does not have. */
#define E_DATA_NO_ENTRY_MSG "No entry in this list"

/**
 * e_data_list_action - Tell what a key asks of a list window.
 * @ins: the window's kind (Function-Index, Grep, Find, Project, Variables,
 *       Install or Windows).
 * @c: the key.
 *
 * Enter (or Alt-S) opens the entry of a Function-Index, Grep, Find or Window
 * list. In the Project list Enter (or Alt-E) opens the file, Alt-A adds one,
 * Alt-D deletes one and Alt-O opens the project options. In Variables and
 * Install Alt-A adds, Alt-D deletes and Enter (or Alt-E) edits. This is the
 * one place a key becomes a list action; the rest of the dispatch and the
 * check for an entry work on the action.
 *
 * Return: the action, WPE_LIST_NONE for any other key.
 */
static enum wpe_list_action
e_data_list_action(int ins, int c)
{
  if(ins < 4 || ins == 7)
  {
    if(c == WPE_CR || c == AltS)
      return(WPE_LIST_OPEN);
    return(WPE_LIST_NONE);
  }
#ifdef PROG
  if(wpe_listsel_is_sealed_kind(ins))
  {
    if(c == AltA || c == EINFG)
      return(WPE_LIST_ADD);
    if(c == AltD || c == ENTF)
      return(WPE_LIST_DELETE);
    if(c == AltE || c == WPE_CR)
    {
      if(ins == 4)
        return(WPE_LIST_OPEN);
      return(WPE_LIST_EDIT);
    }
    if(ins == 4 && c == AltO)
      return(WPE_LIST_OPTIONS);
  }
#endif
  return(WPE_LIST_NONE);
}

int e_data_eingabe(ECNT * cn)
{
  FENSTER        *f = cn->f[cn->mxedt];
  FLWND          *fw = (FLWND *) f->b;
  int             c = AltF;
  int             sealed;
  enum wpe_list_action action;

  fk_cursor(0);
  if(f->ins == 7)
  {
    freedf(fw->df);
    fw->df = e_make_win_list(f);
  }
  while(c != WPE_ESC)
  {
    if(f->dtmd != DTMD_DATA)
      return(0);
#ifdef PROG
    if(f->ins == 4)
      fw->df = e_p_df[0];
#endif
    if(c == AltF)
      c = e_file_window(0, fw, f->fb->ft.fb, f->fb->fz.fb);
#if  MOUSE
    if(c == MBKEY)
      c = e_data_ein_mouse(f);
#endif
    action = e_data_list_action(f->ins, c);
    sealed = wpe_listsel_is_sealed_kind(f->ins);
    if(wpe_listsel_action_needs_entry(f->ins, action) &&
       !wpe_listsel_has(fw->df, fw->nf, sealed))
    {
      e_error(E_DATA_NO_ENTRY_MSG, 0, f->fb);
      c = AltF;
      continue;
    }
    if((action == WPE_LIST_OPEN && !sealed) || action == WPE_LIST_ADD)
    {
      if(f->ins == 1)
        e_ed_man(wpe_listsel_name(fw->df, fw->nf, sealed), f);
      else if(f->ins == 2 && e_edit(f->ed, wpe_listsel_name(fw->df, fw->nf, sealed)) == 0)
        e_rep_search(f->ed->f[f->ed->mxedt]);   /* only in the window just opened */
      else if(f->ins == 3)
        e_edit(f->ed, wpe_listsel_name(fw->df, fw->nf, sealed));
      else if(f->ins == 7)
        e_switch_window(f->ed->edt[fw->df->anz - fw->nf], f);
#ifdef PROG
      else if(f->ins == 4)
      {
        /* WpeHandleFileManager runs its own internal loop until WPE_ESC,
           so call it ONCE (as e_p_add_item does). Wrapping it in
           while(... != WPE_ESC) spins at 100% CPU when it returns 0 after
           losing focus (the FM window is no longer active). */
        WpeCreateFileManager(5, f->ed, NULL);
        WpeHandleFileManager(f->ed);
        e_p_df[f->ins - 4] = fw->df;
        c = AltF;
        f->save = 1;
      }
      else if(f->ins > 4 && f->ins < 7)
      {
        e_p_add_df(fw, f->ins);
        e_p_df[f->ins - 4] = fw->df;
        c = AltF;
      }
#endif
      if(f->ins < 4 || f->ins == 7)
        return(0);
    }
#ifdef PROG
    else if(action == WPE_LIST_DELETE)
    {
      e_p_del_df(fw, f->ins);
      e_wrt_prj_fl(f);   /* persist on each Delete, same as the menu path (#137) */
      c = AltF;
      f->save = 1;
    }
    else if(action == WPE_LIST_EDIT)
    {
      e_p_edit_df(fw, f->ins);
      c = AltF;
      f->save = 1;
    }
    else if(action == WPE_LIST_OPEN)
    {
      e_edit(f->ed, wpe_listsel_name(fw->df, fw->nf, sealed));
      c = WPE_ESC;
    }
    else if(action == WPE_LIST_OPTIONS)
    {
      e_project_options(f);
      c = AltF;
    }
#endif
    else if(c != AltF)
    {
      if(c == AltBl)
        c = WPE_ESC;
      else if(c == WPE_ESC)
        c = e_list_close_key(f);
      if(f->ins == 7 && e_is_list_close_key(f, c))
      {
        e_close_window(f);   /* frees f: the Window list is gone */
        return(0);
      }
      if(f->ins == 4 && e_is_list_close_key(f, c))
      {
        FENSTER        *pf = f->ed->f[f->ed->mxedt];
        FLWND          *fw = (FLWND *) pf->b;
        /* Persist added/removed files to the .prj on close, otherwise the
           in-memory list is lost when the project window is reopened. */
        if(pf->save)
          e_wrt_prj_fl(pf);
        fw->df = NULL;
        e_close_window(pf);
        return(0);
      }
      if(f->ins == 4 && (!e_tst_dfkt(f, c) || !e_prog_switch(f, c)))
        return(0);
      if(f->ins > 4 && e_is_list_close_key(f, c))
        return(c);
      else if((f->ins < 4 || f->ins == 7) && !e_tst_dfkt(f, c))
        return(0);
      else
        c = AltF;
    }
  }
  return(e_list_close_key(f));
}

int e_get_funct_in(char *nstr, FENSTER * f)
{
  return(e_data_first(1, f->ed, nstr));
}

int e_funct_in(FENSTER * f)
{
  int             n, xa = 37, ya = 2, num = 8;
  OPTK           *opt = MALLOC(num * sizeof(OPTK));
  char            nstr[2];

  opt[0].t = "User Commands";
  opt[0].x = 0;
  opt[0].o = 'U';
  opt[1].t = "System Calls";
  opt[1].x = 0;
  opt[1].o = 'S';
  opt[2].t = "C-Lib.-Functions";
  opt[2].x = 0;
  opt[2].o = 'C';
  opt[3].t = "Devices & Netw. I.";
  opt[3].x = 0;
  opt[3].o = 'D';
  opt[4].t = "File Formats";
  opt[4].x = 0;
  opt[4].o = 'F';
  opt[5].t = "Games & Demos";
  opt[5].x = 0;
  opt[5].o = 'G';
  opt[6].t = "Environment, ...";
  opt[6].x = 0;
  opt[6].o = 'E';
  opt[7].t = "Maintenance Com.";
  opt[7].x = 0;
  opt[7].o = 'M';

  n = e_opt_sec_box(xa, ya, num, opt, f, 1);

  FREE(opt);
  if(n < 0)
    return(WPE_ESC);

  nstr[0] = '1' + n;
  nstr[1] = '\0';
  return(e_get_funct_in(nstr, f));
}

#endif
