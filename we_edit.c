/* we_edit.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include "makro.h"
#include "we_fdloop.h"
#include "we_buffer.h"
#include "we_testdump.h"

#include<sys/types.h> /*  included for digital station  */
#include<sys/stat.h>
#include<stdarg.h>

/* Room for the status line that says a change was refused for memory. */
#define E_BUFFER_SAY_MAX 160
#define E_BUFFER_SAY_FMT "Out of memory: %s was not made, the text is unchanged"
/* The same for a change refused because its rows would not fit a line. */
#define E_BUFFER_SAY_SHAPE_FMT "Rows too wide: %s was not made, the text is unchanged"

static void e_say_nomem_until_key(const char *what);
static int e_del_line(int yd, BUFFER *b, SCHIRM *s);
int e_codepoint_to_utf8(int cp, unsigned char *out);

/* The byte a terminal reads for Backspace when nobody said otherwise. */
#define E_PTY_DEL_BYTE 127

/**
 * e_pty_key_bytes - What a program reading its terminal gets for a key
 * typed for it: a line end for Enter, the erase byte for Backspace, the
 * character in UTF-8 for text, nothing for any other key.
 * @key: the key; @buf: room for E_PTY_KEY_BYTES_MAX bytes.
 * Return: the bytes put in @buf.
 *
 * One encoding for Run's terminal and the debugged program's, so a program
 * reads the same keys whichever way it was started.
 */
int e_pty_key_bytes(int key, unsigned char *buf)
{
 if (key == WPE_CR)
 {
  buf[0] = '\n';
  return 1;
 }
 if (key == WPE_DC)
 {
  buf[0] = E_PTY_DEL_BYTE;
  return 1;
 }
 if (key >= E_PTY_FIRST_TEXT_KEY)
  return e_codepoint_to_utf8(key, buf);
 return 0;
}

/* What the stall line says about the program Run started, and what ends it. */
#define E_PTY_RUN_WHAT "the program"
#define E_PTY_RUN_ENDS "Ctrl-C ends it"

/**
 * e_pty_send_key - Type @key into Run's terminal.
 * @fd: the terminal's master, non-blocking.
 *
 * Never waits: a program that stopped reading its terminal leaves the keys
 * queued on the loop, the editor keeps taking keys (Ctrl-C among them), and
 * the status row says the program is not reading its input.
 */
void e_pty_send_key(int fd, int key)
{
 unsigned char buf[E_PTY_KEY_BYTES_MAX];
 int len = e_pty_key_bytes(key, buf);

 if (len > 0)
  wpe_fd_write(fd, buf, (size_t)len, E_PTY_RUN_WHAT, E_PTY_RUN_ENDS);
}

/* Key codes below this are always text; from here up a key is text only when
   the backend flagged it as a decoded character, because xwpe's own key codes
   (Alt-letters, F-keys) live in the same numeric range as Unicode.  One bound
   for the editor's character insertion and for the debugger's console. */
#define E_KEY_TEXT_LIMIT 255
/* The first key code that is a printable character, not a control key. */
#define E_KEY_FIRST_PRINTABLE 32

#ifdef DEBUGGER
/** e_key_is_program_text - Is this key text the user typed for the program?
 *
 * While a debugged program runs, what the user types belongs to the program
 * but the editor's own keys (Alt-letter menus, F-keys, Ctrl-F2, ...) must
 * still reach the editor, so Debug > Quit and the other menus keep working.
 * Same rule as character insertion in the edit loop: a code below 255 is
 * text, a larger one only when the backend flagged it as a decoded
 * character (typed Unicode); otherwise it is an xwpe key code.
 *
 * @c: key as returned by the backend reader.
 * Return: non-zero for Enter, Backspace and typed characters.
 */
static int e_key_is_program_text(int c)
{
 if (c == WPE_CR || c == WPE_DC)
  return 1;
 if (c < E_KEY_FIRST_PRINTABLE)
  return 0;
 return (c < E_KEY_TEXT_LIMIT || e_input_was_char);
}

/** e_key_cancels_answer - Does this key give up on the debugger's answer?
 *
 * While the editor waits for the debugger to answer at a stop, Esc and
 * Ctrl-C end the wait, as the waiting line says; at any other time they are
 * the editor's keys as always.
 *
 * @c: key as returned by the backend reader.
 * Return: non-zero to cancel the wait.
 */
static int e_key_cancels_answer(int c)
{
 if (c != WPE_ESC && c != CtrlC)
  return 0;
 return e_d_answer_busy();
}

static int e_debug_console_input(int c, FENSTER *f)
{
 if (!e_d_run_takes_typed_text())
  return 0;
 if (e_key_is_program_text(c))
 {
  e_d_pty_type(c);
  return 1;
 }
 return 0;
}
#endif

/* "The last getch returned a decoded text character" -- see edit.h.  Defined
   here because we_edit.c is always compiled (the backend that read the key sets
   it); character insertion below reads it to admit typed Unicode >= 256 that
   would otherwise look like an xwpe key code. */
int e_input_was_char = 0;

char *e_make_postf();
int e_del_a_ind();
int e_tab_a_ind();
static int e_window_auto_indents(FENSTER *f);
static void e_backspace_a_ind(BUFFER *b, SCHIRM *s, POINT at_key);
int e_help_next();
int e_codepoint_to_utf8(int cp, unsigned char *out);
int e_utf8_visual_len(unsigned char *s, int bytes);
static int e_char_bytes_at(BUFFER *b, int x, int y);
static int e_char_start_before(BUFFER *b, int x, int y);

/* What e_char_bytes_at gives for ASCII or a byte that starts no character. */
#define E_CHAR_ONE_BYTE 1

#ifdef PROG
BUFFER *e_p_m_buffer = NULL;
#ifdef DEBUGGER
BUFFER *e_p_w_buffer = NULL;
#endif
#endif

/**
 * e_line_make_hard_empty - Turn an empty line into an empty line that ends
 * with a line break.
 * @l: a line of a fresh buffer.
 *
 * A new file and the clipboard start with one such line, so the first key
 * typed and the file written both see a line, as they always did.
 */
void e_line_make_hard_empty(STRING *l)
{
 l->s[0] = WPE_WR;
 l->s[1] = '\0';
 l->len = 0;
 l->nrc = 1;
}

/**
 * e_edit_read_file - Read the file of a new window into its fresh buffer.
 * @f: the new window; @fp: its file.
 *
 * Reading a file is no edit: the lines go in with no undo step, no modified
 * mark, and with the breakpoints the debugger keeps for a file of that name
 * left on the lines they were set on instead of moving down with the new
 * ones.  Return: E_BUFFER_OK, or E_BUFFER_NOMEM with the buffer as fresh as
 * it was.
 */
static int e_edit_read_file(FENSTER *f, FILE *fp)
{
 BUFFER *b = f->b;
 int history = b->history, r;
 POINT end;

 b->history = E_BUFFER_NO_HISTORY;
 r = e_read_file_into(fp, b, 0, 0, &f->dtmd, &end);
 b->history = history;
 return(r);
}

/**
 * e_edit_drop_window - Take back a window that is being opened, before it
 * was ever drawn: its buffer, its screen and its names are freed and the
 * desk is as it was.
 * @cn: the editor; @f: the window, the newest on the desk.
 *
 * A file that could not be read for want of memory must not open as an
 * empty window under its own name: saving that window would empty the file
 * on disk.  Nothing else is touched (an emergency save of the file stays).
 */
static void e_edit_drop_window(ECNT *cn, FENSTER *f)
{
 e_close_buffer(f->b);
 if (f->c_sw != NULL)
  FREE(f->c_sw);
#ifdef DEBUGGER
 if (f->s->brp != NULL)
  FREE(f->s->brp);
#endif
 FREE(f->s);
 if (f->datnam != NULL)
  FREE(f->datnam);
 if (f->dirct != NULL)
  FREE(f->dirct);
 (cn->mxedt)--;
 cn->curedt = cn->edt[cn->mxedt];
 FREE(f);
}

/**
 * e_edit_refuse_file - The file of a window being opened could not be read
 * for want of memory: the window is not opened and the status row says so.
 * @cn: the editor; @f: the window; @fp: its file, closed here.
 */
static void e_edit_refuse_file(ECNT *cn, FENSTER *f, FILE *fp)
{
 fclose(fp);
 e_edit_drop_window(cn, f);
 e_say_nomem_until_key("opening the file");
}

/* What e_edit opens: a file the user edits, or one of the views below. */
#define E_EDIT_A_FILE 0

/**
 * e_edit_read_refused - The read of a window being opened was refused for
 * want of memory: decide whether the window opens.
 * @cn: the editor; @f: the window; @fp: its file; @ftype: E_EDIT_A_FILE, or
 * the view (Help, Messages, Watches, Stack) it is.
 *
 * A file the user edits does not open, so it cannot be saved over as an
 * empty window.  A view the editor writes itself (Messages, Watches, Stack)
 * opens empty, as before: its old file is not the user's text, and the
 * build and the debugger go on writing into it.  The status row says so.
 * Return: 1 when the window was dropped (e_edit returns -1), else 0.
 */
static int e_edit_read_refused(ECNT *cn, FENSTER *f, FILE *fp, int ftype)
{
 if (ftype == E_EDIT_A_FILE)
 {
  e_edit_refuse_file(cn, f, fp);
  return(1);
 }
 wpe_buffer_say_nomem(f->b, "reading the file");
 return(0);
}

/**
 * e_edit_drop_fresh_line - A file was read into a fresh buffer: take away
 * the empty line the buffer started with, which the reader pushed to the end.
 * @f: the new window.
 *
 * The window then holds exactly the file's lines, reads unmodified and has
 * nothing to undo.
 */
static void e_edit_drop_fresh_line(FENSTER *f)
{
 BUFFER *b = f->b;

 if (b->mxlines > 1 && b->bf[b->mxlines - 1].s[0] == '\0')
  e_buffer_delete_lines(b, b->mxlines - 1, 1, E_BUFFER_STEP_COVERED);
 e_buffer_history_drop(b);
 f->save = 0;
}

/* open edit window */
int e_edit(ECNT *cn, char *filename)
{
 extern char *e_hlp_str[];
 extern WOPT *eblst, *hblst, *mblst, *dblst;
 extern WOPT *eblst_lsp;                 /* editor bar variant for LSP-backed files */
 extern const char *e_lsp_server_label(FENSTER *);  /* non-NULL => file has a server */
#ifdef WPE_AI
 extern WOPT *eblst_ai;                  /* editor bar variant while AI is enabled */
 extern WOPT *eblst_lspai;               /* combined bar: LSP server + AI both on   */
 extern int e_ai_bar_wanted(void);       /* AI enabled AND programming mode        */
#endif
 FILE *fp = NULL;
 FENSTER *f, *fo;
 char *complete_fname, *path, *file;
 int ftype = 0, i, j, st = 0;
 struct stat buf[1];

 /* Allows project files to be open automatically */
 j = strlen(filename);
 if ((WpeIsProg()) && (j > 4) && (!strcmp(&filename[j - 4], ".prj")))
 {
  if (!e_prog.project)
  {
   e_prog.project = MALLOC(1);
   e_prog.project[0] = '\0';
  }
  else
  {
   for (i = cn->mxedt; i > 0 &&
     (cn->f[i]->dtmd != DTMD_DATA || cn->f[i]->ins != 4); i--)
    ;
   if (i > 0)
   {
    e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
    e_close_window(cn->f[cn->mxedt]);
   }
  }
  e_prog.project = REALLOC(e_prog.project, j + 1);
  strcpy(e_prog.project, filename);
  e_make_prj_opt(cn->f[cn->mxedt]);
/************************************/  
  e_rel_brkwtch(cn->f[cn->mxedt]);
/************************************/  
  e_prj_ob_file(cn->f[cn->mxedt]);
  return 0;
 }

 /* Check to see if the file is already opened BD */
 WpeFilenameToPathFile(filename, &path, &file);
 /* Should check for error here */
 for (i = cn->mxedt; i >= 0; i--)
 {
  if ((strcmp(cn->f[i]->datnam, file) == 0) &&
    (strcmp(cn->f[i]->dirct, path) == 0))
  {
   e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
   WpeFree(path);
   WpeFree(file);
   return(0);
  }
 }

 if (cn->mxedt >= MAXEDT)
 {
  e_error(e_msg[ERR_MAXWINS], 0, cn->fb);
  return(-1);
 }
 if (stat(filename, buf) == 0)
 {
  if (!S_ISREG(buf->st_mode))
  {
   /* error message should go here */
   return(-1);
  }
 }
 for (j = 1; j <= MAXEDT; j++)
 {
  for (i = 1; i <= cn->mxedt && cn->edt[i] != j; i++);
  if( i > cn->mxedt) break;
 }
 cn->curedt=j;
 (cn->mxedt)++;
 cn->edt[cn->mxedt]=j;
   
 if ((f = (FENSTER *)MALLOC(sizeof(FENSTER))) == NULL)
  e_error(e_msg[ERR_LOWMEM], 1, cn->fb);
   
 f->fb = cn->fb;
 cn->f[cn->mxedt] = f;

 if ((f->b = (BUFFER *)MALLOC(sizeof(BUFFER))) == NULL)
  e_error(e_msg[ERR_LOWMEM], 1, f->fb);
 if ((f->s = (SCHIRM*)MALLOC(sizeof(SCHIRM))) == NULL)
  e_error(e_msg[ERR_LOWMEM], 1, f->fb);
#ifdef DEBUGGER
 f->s->brp = NULL;
#endif
 e_buffer_init(f->b, cn, f, cn->maxcol, E_BUFFER_HISTORY_FULL);
 if (f->b->mxlines != 1)
  e_error(e_msg[ERR_LOWMEM], 1, f->fb);
#ifdef PROG
 for (i = cn->mxedt-1;
   i > 0 && (!strcmp(cn->f[i]->datnam, "Messages") || !DTMD_ISTEXT(cn->f[i]->dtmd) ||
     !strcmp(cn->f[i]->datnam, "Watches") ||
     !strcmp(cn->f[i]->datnam, "Stack")); i--)
  ;
 for (j = cn->mxedt-1; j > 0 && !st; j--)
  if (!strcmp(cn->f[j]->datnam, "Stack"))
   st = 1;
#else
 for(i = cn->mxedt-1; i > 0 && !DTMD_ISTEXT(cn->f[i]->dtmd); i--)
  ;
#endif
#ifdef PROG
 if (WpeIsProg())
 {
  if ((e_we_sw & 8) || !strcmp(filename, "Messages") ||
    !strcmp(filename, "Watches"))
  {
   e_position_messages_window(f, cn);
  }
  else if (!strcmp(filename, "Stack"))
  {
   f->a = e_set_pnt(2*MAXSCOL/3, 1);
   f->e = e_set_pnt(MAXSCOL-1, 2*MAXSLNS/3);
  }
  else
  {
   if (i < 1)
   {
    f->a = e_set_pnt(0, 1);
    f->e = e_set_pnt(st ? 2*MAXSCOL/3-1 : MAXSCOL-1, 2*MAXSLNS/3);
   }
   else
   {
    f->a = e_set_pnt(cn->f[i]->a.x+1, cn->f[i]->a.y+1);
    f->e = e_set_pnt(st ? 2*MAXSCOL/3-1 : cn->f[i]->e.x, cn->f[i]->e.y);
   }
  }
 }
 else
#endif
 {
  if (i < 1)
  {
   f->a = e_set_pnt(0, 1);
   f->e = e_set_pnt(MAXSCOL-1, MAXSLNS-2);
  }
  else
  {
   f->a = e_set_pnt(cn->f[i]->a.x + 1, cn->f[i]->a.y + 1);
   f->e = e_set_pnt(cn->f[i]->e.x, cn->f[i]->e.y);
  }
 }
 if (NUM_COLS_ON_SCREEN < 26)
  f->a.x = f->e.x - 26;
 if(NUM_LINES_ON_SCREEN < 3)
  f->a.y = f->e.y - 3;
 f->winnum = cn->curedt;
 f->dtmd = cn->dtmd;
 f->ins = 1;
 f->save = 0;
 f->zoom = 0;
 f->ed = cn;
 f->pic = NULL;
 f->hlp_str = e_hlp_str[0];
 f->blst = eblst;
 f->nblst = 7;
 f->b->fb = f->fb;
 f->fd.dirct = NULL;
 if (WpeIsProg())
  e_add_synt_tl(filename, f);
 else
 {
  f->c_st = NULL;
  f->c_sw = NULL;
 }
 if ((f->ed->edopt & ED_ALWAYS_AUTO_INDENT) ||
   ((f->ed->edopt & ED_SOURCE_AUTO_INDENT) && f->c_st))
 {
  f->flg = 1;
 }
 else
 {
  f->flg = 0;
 }
 f->s->c = e_set_pnt(0, 0);
 f->s->ks = e_set_pnt(0, 0);
 f->s->mark_begin = e_set_pnt(0, 0);
 f->s->mark_end = e_set_pnt(0, 0);
 f->s->fa = e_set_pnt(0, 0);
 f->s->fe = e_set_pnt(0, 0);
 f->s->fb = f->fb;
#ifdef DEBUGGER
 f->s->da.y = -1;
#endif
 f->dirct = path;
 for (i = 0; i < E_MARKERS; i++)
  f->s->pt[i] = e_set_pnt(-1, -1);
 if (cn->mxedt == 0)    /*  Clipboard  */
 {
  cn->curedt=0;
  cn->edt[cn->mxedt]=0;
  WpeFree(file);
  file = f->datnam = WpeStrdup(BUFFER_NAME);
#ifdef UNIX
  f->filemode = 0600;
#endif
  e_buffer_mark_view(f->b);
  e_line_make_hard_empty(&f->b->bf[0]);
  return(0);
 }
 if (strcmp(file,"") == 0)
 {
  WpeFree(file);
  file = f->datnam = WpeStrdup("Noname");
 }
 else
 {
  f->datnam = file;
 }
 if (strcmp(filename, "Help") == 0)
 {
  complete_fname = e_lib_file(HELP_FILE);
  f->dtmd = DTMD_HELP;
  f->ins = 8;
  f->hlp_str = e_hlp_str[25];
  f->nblst = 7;
  f->blst = hblst;
  ftype = 1;
 }
 else
  complete_fname = e_mkfilename(f->dirct, f->datnam);
#ifdef PROG
 if (WpeIsProg())
 {
  if (!strcmp(filename, "Messages"))
  {
   f->ins = 8;
   f->hlp_str = e_hlp_str[3];
   f->nblst = 8;
   f->blst = mblst;
   ftype = 2;
  }
  else if (!strcmp(filename, "Watches"))
  {
   f->ins = 8;
   f->hlp_str = e_hlp_str[1];
   f->blst = dblst;
   ftype = 3;
  }
  else if (!strcmp(filename, "Stack"))
  {
   f->ins = 8;
   f->hlp_str = e_hlp_str[2];
   f->blst = dblst;
   ftype = 4;
  }
 }
#endif
#ifdef DEBUGGER
 /* A plain editor window (not Help/Messages/Watches/Stack) holding a file a
    language server understands gets the LSP bottom bar -- the contextual "LSP
    Menu" entry -- so the IDE actions are discoverable.  Per-window: switching to
    a non-LSP file shows its normal bar, exactly the desired contextual behaviour. */
 if (ftype == 0 && e_lsp_bar_wanted(f))
  f->blst = eblst_lsp;
#endif
#ifdef WPE_AI
 /* AI enabled: a plain editor file shows the "Alt-G AI" bottom-bar entry so the
    assistant is discoverable and mouse-clickable.  On a language-server file the
    COMBINED bar is used instead, so BOTH the "Alt-Q ? LSP" and "Alt-G AI" hints
    are shown -- LSP and AI are usable at the same time and neither loses its
    on-screen hint (Alt-Q keeps working regardless).  The bar is re-selected here
    on every open AND refreshed live by e_ai_refresh_bars when the runtime toggle
    flips in Options > AI.  Programming mode only (wpe, xwpe): under we Alt-G is
    Go to Line and a click on the entry would open nothing, so the plain editor
    keeps its classic bar. */
 if (ftype == 0 && e_ai_bar_wanted()) {
#ifdef DEBUGGER
  if (e_lsp_bar_wanted(f))
   f->blst = eblst_lspai;               /* LSP + AI: show both hints */
  else
#endif
   f->blst = eblst_ai;                  /* AI only */
 }
#endif
 /* An editor bar of the programming modes names F9 and has one entry more;
    the window paints and takes clicks on as many entries as its bar has. */
 if (ftype == 0 && !e_give_debug_bar(f))
  e_use_editor_bar(f, f->blst);
 if (ftype != 0)
  e_buffer_mark_view(f->b);       /* Help, Messages, Watches and Stack */
 if (ftype != 1)
  fp = fopen(complete_fname, "rb");
 if (fp != NULL && access(complete_fname, 2) != 0)
  f->ins = 8;
 /* A file we could not even OPEN for reading but which EXISTS and is not
    writable (mode 000) is still a locked file -- mark it read-only so the title
    shows the lock, instead of a silently-empty editable buffer that would fail
    to save.  access(,2) is W_OK; access(,0) is F_OK (exists). */
 else if (fp == NULL && ftype != 1 &&
          access(complete_fname, 0) == 0 && access(complete_fname, 2) != 0)
  f->ins = 8;
#ifdef UNIX
 if (fp != NULL)
 {
  stat(complete_fname, buf);
  f->filemode = buf->st_mode;
 }
 else
 {
  umask(i = umask(077));
  f->filemode = 0666 & ~i;
 }
#endif
 FREE(complete_fname);

 if (fp != NULL && ftype != 1 && e_edit_read_file(f, fp) != E_BUFFER_OK &&
     e_edit_read_refused(cn, f, fp, ftype))
  return(-1);
 if (fp != NULL && ftype != 1)
 {
  e_edit_drop_fresh_line(f);
  if (fclose(fp) != 0)
   e_error(e_msg[ERR_FCLOSE], 0, cn->fb);
  if (cn->dtmd == DTMD_HELP)
   cn->dtmd = DTMD_NORMAL;
#ifdef PROG
  if (WpeIsProg())
  {
   if (e_we_sw & 8)
   {
    strcpy(f->datnam, "Messages");
    e_we_sw &= ~8;
   }
   if (!strcmp(f->datnam, "Messages"))
   {
    e_buffer_mark_view(f->b);    /* compiler lines appended later stay one row each */
    e_make_error_list(f);
    f->ins = 8;
   }
  }
#endif
 }
 else
  e_line_make_hard_empty(&f->b->bf[0]);
#ifdef PROG
 if (ftype == 2)
 {
  if(e_p_m_buffer != NULL)
  {
   e_close_buffer(f->b);
   f->b = e_p_m_buffer;
   f->b->f = f;
  }
  else
  {
   e_p_m_buffer = f->b;
   e_buffer_clear(f->b);          /* a stale Messages file read above is not shown */
  }
 }
#ifdef DEBUGGER
 if (ftype == 3)
 {
  if (e_p_w_buffer != NULL)
  {
   e_close_buffer(f->b);
   f->b = e_p_w_buffer;
   f->b->f = f;
  }
  else
  {
   e_p_w_buffer = f->b;
/*  e_ins_nchar(f->b, f->s, "No Watches", 0, 0, 10);*/
  }
 }
#endif
#endif
 sc_txt_1(f);
 if (cn->mxedt > 1)
 {
  fo = cn->f[cn->mxedt-1];
  e_ed_rahmen(fo, 0);
 }
 e_firstl(f, 1);
 e_zlsplt(f);
 e_brk_schirm(f);
 e_schirm(f, 1);
 e_cursor(f, 1);
#ifdef DEBUGGER
 /* The "3" of the 3+1 LSP start: opening a language-server file boots the server
    in the background (only if its binary is installed and none runs yet), so the
    first Alt-Q does not wait through the cold start. */
 {
  extern void e_lsp_open_eager(FENSTER *f);
  e_lsp_open_eager(f);
 }
#endif
 return(0);
}

/* The line that names a paint the screen's owner could not keep. */
#define E_PAINT_LOST_FMT "Not shown while a box was open: %s"
#define E_PAINT_LOST_MAX 120

/**
 * e_paint_text_on_top - Is a text window on top of the desk?
 *
 * The screen's owner asks this before content is drawn from outside the
 * user's keys: the File Manager, the Window list or a project dialog is a
 * window with a key loop of its own, and nothing is drawn under it.
 */
static int e_paint_text_on_top(void)
{
 FENSTER *top;

 if (!WpeEditor || WpeEditor->mxedt < 1)
  return(0);
 top = WpeEditor->f[WpeEditor->mxedt];
 if (!top)
  return(0);
 return(DTMD_ISTEXT(top->dtmd));
}

/** e_paint_read_key - The display's key reader, for a box's key. */
static int e_paint_read_key(void)
{
 return(e_getch());
}

/**
 * e_paint_take_key - The display's key taken without waiting, for a loop
 * that must not wait (Run, a short command's Esc).
 * @want: WPE_PAINT_ANY_KEY, or one key.
 */
static int e_paint_take_key(int want)
{
 if (!e_u_key_take)
  return(0);
 return((*e_u_key_take)(want));
}

/**
 * e_paint_box_opens - A box takes the screen: a status line kept until the
 * next key gives way, or it would come back on the box's bottom row.
 */
static void e_paint_box_opens(void)
{
 e_status_drop_held();
}

/**
 * e_paint_say_lost - Name on the status row an update that was not shown
 * because too many waited behind a box, so the user knows to look again.
 */
static void e_paint_say_lost(const char *what)
{
 char line[E_PAINT_LOST_MAX];

 if (!WpeEditor || WpeEditor->mxedt < 1)
  return;
 snprintf(line, sizeof line, E_PAINT_LOST_FMT, what);
 e_status_say(WpeEditor->f[WpeEditor->mxedt], line);
}

/**
 * e_paint_desk_ready - Tell the screen's owner how to see the editor's
 * desk.  Called once, before anything is drawn.
 */
void e_paint_desk_ready(void)
{
 static const WpePaintDesk desk = {
  e_paint_text_on_top, e_paint_read_key, e_paint_box_opens, e_paint_say_lost,
  e_paint_take_key
 };

 wpe_paint_desk_set(&desk);
}

/* The bytes Enter and Tab add to a window, as far as a size limit counts
   them: the line break is one byte, and Tab is counted as one character. */
#define E_LINE_BREAK_BYTES 1
#define E_TAB_KEY_BYTES    1
/* The most bytes one character takes in UTF-8. */
#define E_UTF8_CHAR_MAX    4

/**
 * e_key_text_bytes - The bytes character key @c puts into a buffer.
 *
 * A character past ASCII goes in as its UTF-8 bytes, so a size limit is
 * checked against the whole character and never splits one.
 */
static int e_key_text_bytes(int c)
{
 unsigned char u8[E_UTF8_CHAR_MAX];

 if (c < 0x80)
  return(1);
 return(e_codepoint_to_utf8(c, u8));
}

/**
 * e_window_full - Would @n more bytes take window @f past its size limit?
 *
 * Only the assistant's composer has one (a request is at most so many
 * bytes); there the key is refused and the status row says so, instead of
 * the text growing past what will be sent.  Every other window takes it.
 */
static int e_window_full(FENSTER *f, int n)
{
#ifdef WPE_AI
 return(e_ai_window_full(f, n));
#else
 return(0);
#endif
}

/*   keyboard output routine */
int e_eingabe(ECNT *e)
{
 BUFFER *b = e->f[e->mxedt]->b;
 SCHIRM *s = e->f[e->mxedt]->s;
 FENSTER *f = e->f[e->mxedt];
 int ret, c = 0;
 unsigned char cc;

 fk_cursor(1);
 while (c != WPE_ESC)
 {
  if (e->mxedt < 1) c = WpeHandleMainmenu(-1, f);
  else if (!DTMD_ISTEXT(f->dtmd)) return(0);
  else
  {
   /* What came behind a box (a reply, a build's end, a stop, an answer,
      program output) is shown now, before the next key is read. */
   wpe_paint_replay_if_idle();
   if (e->mxedt < 1)
    continue;                     /* what was shown closed the last window */
   f = e->f[e->mxedt];            /* or put another one on top */
   if (f->save > f->ed->maxchg) e_autosave(f);
   e_buffer_depth_check(f->b);
   e_buffer_marks_inside(f->b);   /* no mark or Shift anchor past a row's end */
   c = e_getch();
   /* A build ending while the editor waited for this key may have put
      another window on top (the jump to its first error): the key belongs
      to the window the user now sees. */
   f = e->f[e->mxedt];
   b = f->b;
   s = f->s;
   e_status_key_pressed(f);       /* a line kept until a key has been read */
#if  MOUSE
   if (c < 0) cc = c = e_edt_mouse(c, f);
   else cc = c;
#else
   cc = c;
#endif
  }
#ifdef WPE_AI
  wpe_ai_flash_clear(f);          /* any key clears a one-shot status-line flash */
#endif
  if (c == WPE_RESIZE)
   continue;
  if (c == WPE_SCROLL_UP || c == WPE_SCROLL_DOWN)
  {
   int delta = (c == WPE_SCROLL_UP) ? -WPE_WHEEL_LINES : WPE_WHEEL_LINES;
#ifdef WPE_AI
   /* The armed chat pane browses its transcript through its own scroller, so
      the wheel locks the view exactly as PgUp and the arrows do; any other
      window scrolls as before. */
   if (e_ai_chat_wheel(f, c))
    continue;
#endif
   s->c.y += delta;
   if (s->c.y < 0) s->c.y = 0;
   if (s->c.y > b->mxlines - 1) s->c.y = b->mxlines - 1;
   e_schirm(f, 0);
   e_cursor_pos_only(f);
   e_refresh();
   continue;
  }
#ifdef DEBUGGER
  if (e_debug_console_input(c, f))
   continue;
  {
   extern void e_lsp_on_edit(FENSTER *f, int c);  /* in we_debug.c */
   e_lsp_on_edit(f, c);          /* live LSP diagnostics + didChange */
  }
#endif
#ifdef PROG
  /* A build is running: Esc cancels it, as the waiting line in Messages says. */
  if (c == WPE_ESC && e_build_busy()) { e_build_cancel(); c = 0; continue; }
#endif
#ifdef DEBUGGER
  /* A debugger is starting and has not answered: Esc ends it, as the waiting
     line in Messages says. */
  if (c == WPE_ESC && e_d_start_busy()) { e_d_start_cancel(); c = 0; continue; }
  /* `scala-cli setup-ide` runs before Metals or a Scala debug session: Esc
     cancels it, as its waiting line says. */
  if (c == WPE_ESC && e_scala_setup_busy_any()) { e_scala_setup_cancel(); c = 0; continue; }
  /* The Scala build server answers the debug start's requests: Esc gives
     the start up, as its waiting line says. */
  if (c == WPE_ESC && e_d_bsp_busy()) { e_d_bsp_cancel(); c = 0; continue; }
  /* A language server owes an Alt-Q answer, or starts for one: Esc gives it
     up, as the waiting line on the status row says. */
  if (c == WPE_ESC && e_lsp_answer_busy()) { e_lsp_answer_cancel(); c = 0; continue; }
  /* A Rust program compiles for its debug session: Esc cancels it, as its
     waiting line says. */
  if (c == WPE_ESC && e_d_compile_busy()) { e_d_compile_cancel(); c = 0; continue; }
  /* The editor waits for the debugger's answer at a stop: Esc or Ctrl-C
     gives up on it, as the waiting line on the status row says. */
  if (e_key_cancels_answer(c)) { e_d_answer_cancel(); c = 0; continue; }
#endif
#ifdef WPE_AI
  /* A background AI task is running: Esc cancels it (what the spinner promises)
     instead of leaving the editor or closing the chat pane. */
  if (c == WPE_ESC && wpe_ai_busy()) { wpe_ai_cancel(); c = 0; continue; }
  /* The hosted agent engine is streaming a turn: the same key asks it to stop
     that turn, and the session stays alive for the next Alt-G g. */
  if (c == WPE_ESC && wpe_ai_host_turn_active()) { wpe_ai_host_interrupt(); c = 0; continue; }
  /* When the AI chat pane is focused, its input row eats the editing keys (and
     Esc closes the chat); everything else -- mouse (already handled above),
     window switching, function keys -- falls through to normal editing, so the
     pane behaves as an ordinary window. */
  if (e_ai_chat_key(f, c)) { if (c == WPE_ESC) c = 0; continue; }
#endif
  /* xwpe's own key codes (Alt-<letter> 271..305, synthetics 2000..2006) share
     the numeric range of real Unicode codepoints, so a value >= 255 is inserted
     as text only when the backend flagged it as a decoded character; a key code
     leaves e_input_was_char 0 and falls through to its handler below.  ASCII and
     Latin-1 (< 255) are always text. */
  if ((c > 31 || (c == WPE_TAB && !(f->flg & 1)) ||
    (f->ins > 1 && f->ins != WPE_INS_READONLY)) && (c < E_KEY_TEXT_LIMIT || e_input_was_char))
  {
   if (e_window_text_locked(f)) continue;
   if (e_window_full(f, e_key_text_bytes(c))) continue;
   if (c >= 0x80)
   {
    unsigned char u8[4];
    int u8len = e_codepoint_to_utf8(c, u8);
    e_ins_nchar(b, s, u8, b->b.x, b->b.y, u8len);
   }
   else if (f->ins == 0 || f->ins == 2)
    e_put_char(c, b, s);
   else e_ins_nchar(b, s, &cc, b->b.x, b->b.y, 1);
   e_schirm(f, 1);
  }
  else if (c == WPE_DC)
  {
   if (e_window_text_locked(f))
   {
    /* In the Help viewer Backspace is "go back one page", not a deletion. */
    if (f->dtmd == DTMD_HELP) e_help_last(f);
    continue;
   }
   if (b->b.y > 0 || b->b.x > 0)
   {
    if (b->bf[b->b.y].len == 0)
    {
     /* The deletion already moved the cursor off a deleted last row, so
        the row above is named, not stepped to. */
     int row_above = b->b.y - 1;
     e_del_line(b->b.y, b, s);
     if (row_above < 0) row_above = 0;
     b->b.y = row_above;
     b->b.x = b->bf[b->b.y].len;
     if (*(b->bf[b->b.y].s+b->b.x) == '\0') b->b.x--;
    }
    else
    {
     POINT at_key = b->b;

     if (b->b.x > 0)
     {
      int old_x = b->b.x;
      b->b.x = e_char_start_before(b, old_x, b->b.y);
      if (e_window_auto_indents(f)) e_backspace_a_ind(b, s, at_key);
      else e_del_nchar(b, s, b->b.x, b->b.y, old_x - b->b.x);
     }
     else
     {
      b->b.y--;
      b->b.x = b->bf[b->b.y].len;
      if (*(b->bf[b->b.y].s+b->b.x) == '\0') b->b.x--;
      if (e_window_auto_indents(f)) e_backspace_a_ind(b, s, at_key);
      else e_del_nchar(b, s, b->b.x, b->b.y, 1);
     }
    }
    e_schirm(f, 1);
   }
  }
  else if (c == ENTF || c == 4)
  {
   if (e_window_text_locked(f))
   {
#ifdef DEBUGGER
    /* Del removes an entry in the read-only Watches pane; a file window
       waiting for an assistant reply has no watch list of its own. */
    if (WpeIsProg() && f->ins == WPE_INS_READONLY) e_d_is_watch(c, f);
#endif
    continue;
   }
   if (*(b->bf[b->b.y].s + b->b.x) != '\0' &&
     (b->b.y < b->mxlines-1 || *(b->bf[b->b.y].s + b->b.x) != WPE_WR))
   {
    e_del_nchar(b, s, b->b.x, b->b.y, e_char_bytes_at(b, b->b.x, b->b.y));
    e_schirm(f, 1);
   }
  }
  else if (c == WPE_CR)
  {
#ifdef PROG
   if (e_window_text_locked(f))
   {
    /* Enter follows a link in Help and submits a line to a running program
       in the console pane; neither inserts a line break. */
    if (f->dtmd == DTMD_HELP) {  e_help_ret(f);  goto weiter;  }
    if (WpeIsProg() && f->ins == WPE_INS_READONLY)
    {  e_d_car_ret(f);  goto weiter;  }
    else continue;
   }
#else
   if (e_window_text_locked(f)) continue;
#endif
   if (e_window_full(f, E_LINE_BREAK_BYTES)) continue;
   e_car_ret(b, s);
   e_schirm(f, 1);
  }
  else if (c == WPE_TAB)
  {
   if (e_window_text_locked(f)) continue;
   if (e_window_full(f, E_TAB_KEY_BYTES)) continue;
   e_tab_a_ind(b, s);
   e_schirm(f, 1);
  }
/*
          else if(c == WPE_TAB)
          {    if(f->ins == 8) continue;
               if (f->ins == 0 || f->ins == 2)  
                 for(c = f->ed->tabn - b->b.x % f->ed->tabn; c > 0 
                                              && b->b.x < b->mx.x; c--) 
              		{   e_put_char(' ', b, s);  b->b.x++;  }
               else  e_ins_nchar(b, s, f->ed->tabs, b->b.x, b->b.y, 
                                       f->ed->tabn - b->b.x % f->ed->tabn);
               e_schirm(b, s, f, 1);
          }
*/
  else
  {
   ret = e_tst_cur(c, e); /*up/down arrows go this way*/
   if (ret != 0) ret = e_tst_fkt(c, e);
  }
weiter:
  f = e->f[e->mxedt];
  if (e->mxedt > 0 && DTMD_ISTEXT(f->dtmd))
  {
   b = f->b;
   s = f->s;
   s->ks.x = b->b.x;  s->ks.y = b->b.y;
   e_cursor(f, 1);
   if ((c & 511) != CUP && (c & 511) != CDO) b->clsv = b->cl;
   e_zlsplt(f);
  }
 }
 return(c);
}

/**
 * e_at_end_of_text - Is the cursor at the end of the last line?
 * @b: the buffer.
 *
 * Nothing stands to the right of it, so a key that works on what follows
 * the cursor (Ctrl-T, Ctrl-Right) has nothing to work on there, as in the
 * Borland editors.
 */
static int e_at_end_of_text(BUFFER *b)
{
 return(b->b.y >= b->mxlines - 1 && b->b.x >= b->bf[b->b.y].len);
}

/**
 * e_delete_word_right - Ctrl-T: delete from the cursor to the start of the
 * next word; at the end of a line, the line break, which joins the next line
 * to it.
 * @b, @s: the buffer and its screen.
 *
 * At the end of the text there is neither, and nothing is deleted: the last
 * line keeps its line break and the cursor stays where it is.
 */
static void e_delete_word_right(BUFFER *b, SCHIRM *s)
{
 int next;

 if (e_at_end_of_text(b))
  return;
 if (b->b.x < b->bf[b->b.y].len)
 {
  next = e_su_lblk(b->b.x, (char *)b->bf[b->b.y].s);
  e_del_nchar(b, s, b->b.x, b->b.y, next - b->b.x);
 }
 else if (b->bf[b->b.y].s[b->b.x] == WPE_WR)
  e_del_nchar(b, s, b->b.x, b->b.y, 1);
 else
 {
  b->b.x = 0;
  b->b.y++;
 }
}

/**
 * e_delete_to_end_of_row - Ctrl-Z and Ctrl-O Y: delete from the cursor to
 * the end of its row.
 * @b, @s: the buffer and its screen.
 *
 * At the end of a row there is nothing to delete, and nothing happens: no
 * Undo step is kept, so the next Ctrl-U takes back the change before it,
 * as in the Borland editors.
 */
static void e_delete_to_end_of_row(BUFFER *b, SCHIRM *s)
{
 int n = b->bf[b->b.y].len - b->b.x;

 if (n <= 0)
  return;
 e_del_nchar(b, s, b->b.x, b->b.y, n);
}

/*              Interpretation of cursor keys
                     ( Primitive editor )                    */
int e_tst_cur(int c, ECNT *e)
{
   FENSTER *f = e->f[e->mxedt];
   BUFFER *b = f->b;
   SCHIRM *s = f->s;
   
   switch (c)
   {  case CtrlP:
      case CUP:
      case CUP+512:
	 if(b->b.y > 0) (b->b.y)--;
	 b->b.x = e_chr_sp(b->clsv, b, f);
	 break;
      case CtrlN:
      case CDO:
      case CDO+512:
	 if(b->b.y < b->mxlines - 1) (b->b.y)++;
	 b->b.x = e_chr_sp(b->clsv, b, f);
	 break;
      case CtrlB:
      case CLE:
      case CLE+512:
	 if (b->b.x > 0 && b->b.x <= b->bf[b->b.y].len)
	  b->b.x = e_char_start_before(b, b->b.x, b->b.y);
	 else
	  (b->b.x)--;
	 if(b->b.x < 0)
	 {  if(b->b.y > 0)
	    {  (b->b.y)--; b->b.x = b->bf[b->b.y].len;  }
	    else {  b->b.x = 0;  }
	 }
	 break;
      case CtrlF:
      case CRI:
      case CRI+512:
	 if (b->b.x < b->bf[b->b.y].len)
	  b->b.x += e_char_bytes_at(b, b->b.x, b->b.y);
	 else
	 {
	  (b->b.x)++;
	 }
	 if(b->b.x > b->bf[b->b.y].len)
	 {  if(b->b.y < b->mxlines - 1)  {  (b->b.y)++; b->b.x = 0;  }
	    else {  b->b.x = b->bf[b->b.y].len;  }
	 }
	 break;
	 
      case CCLE:
      case CCLE+512:
	 if(b->b.x <= 0 && b->b.y > 0)
	 {  b->b.y--;
	    b->b.x = b->bf[b->b.y].len;
	 }
	 else if( b->b.x > 0 )
	 b->b.x = e_su_rblk(b->b.x - 1, b->bf[b->b.y].s);
	 break;
      case CCRI:
      case CCRI+512:
	 if(b->b.x >= b->bf[b->b.y].len && b->b.y < b->mxlines - 1)
	 {  b->b.x = 0; b->b.y++;  }
	 else if( b->b.x < b->bf[b->b.y].len )
	 b->b.x = e_su_lblk(b->b.x, b->bf[b->b.y].s);
	 break;
      case BDO:
      case BDO+512:
	 b->b.y = b->b.y + NUM_LINES_ON_SCREEN - 2;
	 if(b->b.y > b->mxlines - 1) b->b.y = b->mxlines - 1;
	 e_schirm(f, 1);
	 e_cursor(f, 1);
	 break;
      case BUP:
      case BUP+512:
	 b->b.y = b->b.y - f->e.y + f->a.y + 2;
	 if(b->b.y < 0) b->b.y = 0;
	 e_schirm(f, 1);
	 e_cursor(f, 1);
	 break;
      case CBDO:
      case CBDO+512:
	 b->b.y = b->mxlines - 1;
	 b->b.x = b->bf[b->mxlines - 1].len;
	 e_schirm(f, 1);
	 break;
      case CBUP:
      case CBUP+512:
	 if(b->b.y != 0)
	 {  b->b.x = 0;
	    b->b.y = 0;
	    e_schirm(f, 1);  }
	 break;
      case CEND:
      case CEND+512:
	 if(LINE_NUM_ON_SCREEN_BOTTOM < b->mxlines)
	 b->b.y = LINE_NUM_ON_SCREEN_BOTTOM - 1;
	 else
	 b->b.y = b->mxlines - 1;
	 b->b.x = b->bf[b->b.y].len;
	 break;
      case CPS1:
      case CPS1+512:
	 b->b.x = 0;
	 b->b.y = s->c.y;
	 break;
      case AltI:
      case EINFG:
	 if (f->ins == WPE_INS_READONLY) {
#ifdef DEBUGGER
	    if (WpeIsProg()) e_d_is_watch(c, f);
#endif
	    break;  }
	 if(f->ins & 1) f->ins &= ~1;
	 else f->ins |= 1;
#ifdef NEWSTYLE
	 e_ed_rahmen(f, 1);
#else
	 e_pr_filetype(f);
#endif
	 break;
      case AltJ:
	 if (f->ins == WPE_INS_READONLY) break;
	 if(f->ins & 2) f->ins &= ~2;
	 else f->ins |= 2;
#ifdef NEWSTYLE
	 e_ed_rahmen(f, 1);
#else
	 e_pr_filetype(f);
#endif
	 break;
      case CtrlA:
      case POS1:
      case POS1+512:
	 b->b.x = 0;
	 break;
      case CtrlE:
      case ENDE:
      case ENDE+512:
	 b->b.x = b->bf[b->b.y].len;
	 break;
      case CtrlT:
	 if(e_window_text_locked(f)) break;
	 e_delete_word_right(b, s);
	 e_schirm(f, 1);
	 break;
      case CtrlZ:
	 if(e_window_text_locked(f)) break;
	 e_delete_to_end_of_row(b, s);
	 e_schirm(f, 1);
	 break;
      case DGZ:
	 if(e_window_text_locked(f)) break;
	 e_del_line(b->b.y, b, s);
	 if(b->b.y > b->mxlines - 1) (b->b.y)--;
	 e_schirm(f, 1);
	 break;
      case AF7:
      case AltV:
	 if(f->dtmd != DTMD_HELP || f->ins != WPE_INS_READONLY) return(c);
         e_help_next(f, 0);
	 break;
      case AF8:
      case AltT:
	 if(f->dtmd != DTMD_HELP || f->ins != WPE_INS_READONLY) return(c);
         e_help_next(f, 1);
	 break;
      default:
	 return(c);
   }
   if(c >= 512 )
   {  if(s->ks.y == s->mark_begin.y && s->ks.x == s->mark_begin.x &&
		(s->mark_begin.y != s->mark_end.y || s->mark_begin.x != s->mark_end.x))
      {  s->mark_begin.x = b->b.x;  s->mark_begin.y = b->b.y;  }
      else if(s->ks.y == s->mark_end.y && s->ks.x == s->mark_end.x &&
		(s->mark_begin.y != s->mark_end.y || s->mark_begin.x != s->mark_end.x))
      {  s->mark_end.x = b->b.x;  s->mark_end.y = b->b.y;  }
      else if(s->ks.y < b->b.y || ( s->ks.y == b->b.y && s->ks.x < b->b.x))
      {  s->mark_begin.x = s->ks.x;  s->mark_begin.y = s->ks.y;
	 s->mark_end.x = b->b.x;  s->mark_end.y = b->b.y;
      }
      else
      {  s->mark_end.x = s->ks.x;  s->mark_end.y = s->ks.y;
	 s->mark_begin.x = b->b.x;  s->mark_begin.y = b->b.y;
      }
      e_schirm(f, 1);
   }
   return(0);
}

/*   function key (F1-F12) evaluation, editor only */
int e_tst_fkt(int c, ECNT *e)
{
 extern OPT opt[];
 int i;
 FENSTER *f = e->f[e->mxedt];

#ifdef PROG
 if (e_tst_dfkt(f, c) == 0 || ((WpeIsProg()) && e_prog_switch(f, c) == 0))
#else
 if (e_tst_dfkt(f, c) == 0)
#endif
 {
  f = e->f[e->mxedt];
  fk_cursor(1);
  if (e->mxedt > 0)
   e_cursor(f, 1);
  return(0);
 }

 for (i = 0; i < MENOPT; i++)
  if (c == opt[i].as)
  {
   WpeHandleMainmenu(i, f);
   /* Menu draws over a saved PIC; redraw active window chrome and
      status bar so project state (blst, project name) survives. */
   e_ed_rahmen(e->f[e->mxedt], 1);
   e_refresh_area(0, MAXSLNS - 1, MAXSCOL, 1);
   /* WpeHandleMainmenu may close the active window (Window > Close, ^W,
      Clear Desktop, ...).  Our cached `f = e->f[mxedt]` is then a freed
      pointer, so subsequent dispatch (`switch (c)` below, return value
      consumers) would use-after-free.  Refresh from the editor state. */
   f = e->mxedt > 0 ? e->f[e->mxedt] : NULL;
   if (!f)
    return(0);
  }

 switch (c)
 {
  case CtrlK:
   e_ctrl_k(f);    /*  ctrl k  */
   break;
  case CtrlO:      /*  ctrl o  */
   e_ctrl_o(f);
   break;
  case AltG:
   e_goto_line(f);
   break;
  case CF10:
  case AltY:        /* emacs M-y: browse the paste buffer (kill ring) */
   e_show_clipboard(f);
   break;
  case CtrlDel:
   e_blck_del(f);
   break;
  case UNDO:
  case AltBS:
  case CtrlU:
   if (e_window_text_locked(f)) break;
   e_make_undo(f);
   break;
  case AGAIN:
  case SABS:
  case CtrlR:
   if (e_window_text_locked(f)) break;
   e_make_redo(f);
   break;
  case CUT:
  case ShiftDel:    /*  shift Del  :  Delete to Clipboard  */
/*               case 402:     */
  case CtrlX:
   e_edt_del(f);
   break;
  case PASTE:
  case ShiftEin:    /*  shift Einf  */
  case AltEin:      /*  alt Einf: the same paste, whichever display  */
  case CtrlV:
   e_edt_einf(f);
   break;
  case COPY:
  case CtrlEin:    /*  ctrl Einf  */
/*               case 401:    */
  case AltDel:     /*  alt Del: the same copy, whichever display  */
  case CtrlC:
   e_edt_copy(f);
   break;
  default:
   if (f->ed->edopt & ED_CUA_STYLE)
   {
    switch (c)
    {
     case AF2:
      e_m_save(f);
      break;
     case FID:
     case AF3:
      e_find(f);
      break;
     case SF3:
      e_replace(f);
      break;
     case F3:
      e_rep_search(f);
      break;
     default:
      return(c);
    }
   }
   else
   {
    switch(c)
    {
     case F2:
      e_m_save(f);
      break;
     case FID:
     case F4:
      e_find(f);
      break;
     case AF4:
      e_replace(f);
      break;
     case CtrlL:
     case CF4:
      e_rep_search(f);
      break;
     default:
      return(c);
    }
   }
   break;
 }
 fk_cursor(1);
 return(0);
}

/**
 * e_prefix_read_letter - Read the letter that follows a prefix key.
 *
 * Ctrl-K and Ctrl-O read their command's letter here, so a test watching
 * the editor's state knows the key it sends next is that letter and not
 * text to type.
 * Return: the key, upper case.
 */
static int e_prefix_read_letter(void)
{
 int c;

 wpe_test_ctx_enter(WPE_TEST_CTX_PREFIX, NULL);
 c = e_toupper(wpe_paint_modal_getch());
 wpe_test_ctx_leave();
 return(c);
}

/**
 * e_ctrl_k - Ctrl-K: read the block command's letter and run it.
 * @f: the window the prefix was pressed in.
 *
 * While the prefix waits for its letter, a stop, a build's end or a reply
 * that comes is shown after the command, on the window the user then sees.
 * Return: 0.
 */
int e_ctrl_k(FENSTER *f)
{
 BUFFER *b = f->ed->f[f->ed->mxedt]->b;
 SCHIRM *s = f->ed->f[f->ed->mxedt]->s;
 int c;

 c = e_prefix_read_letter();
 if (c < 32)
  c = c + 'A' - 1;
 switch (c)
 {
  case 'A':
   b->b = s->mark_begin;
   e_schirm(f, 1);
   break;
  case 'B':
   /* Route the keyboard Begin-Mark through e_blck_begin so it honours the
      block-marking mode (ED_BLOCK_WORDSTAR): modern resets begin AND end so
      re-marking leaves no stale block; WordStar moves only begin.  Setting
      mark_begin inline here bypassed that, so the mode flag had no effect
      from the keyboard (only from the Block menu). */
   e_blck_begin(f);
   break;
  case 'C':
   e_blck_copy(f);
   break;
  case 'D':
   e_changecase_dialog(f);
   break;
  case 'F':
   e_mk_beauty(1, 3, f);
   break;
  case 'H':
   e_blck_hide(f);
   break;
  case 'I':
   e_blck_to_right(f);
   break;
  case 'K':
   s->mark_end = e_set_pnt(b->b.x, b->b.y);
   e_schirm(f, 1);
   break;
  case 'L':
   f->s->mark_begin.x = 0;
   f->s->mark_begin.y = f->b->b.y;
   if (f->b->b.y < f->b->mxlines-1)
   {
    f->s->mark_end.x = 0;
    f->s->mark_end.y = f->b->b.y+1;
   }
   else
   {
    f->s->mark_end.x = f->b->bf[f->b->b.y].len;
    f->s->mark_end.y = f->b->b.y;
   }
   e_schirm(f, 1);
   break;
  case 'R':
   e_blck_read(f);
   break;
  case 'U':
   e_blck_to_left(f);
   break;
  case 'V':
   e_blck_move(f);
   break;
  case 'W':
   e_blck_write(f);
   break;
  case 'X':
   s->mark_begin.x = 0;
   s->mark_begin.y = 0;
   s->mark_end.y = b->mxlines-1;
   s->mark_end.x = b->bf[b->mxlines-1].len;
   e_schirm(f, 1);
   break;
  case 'Y':
   e_blck_del(f);
   break;
  case 'Z':
   b->b = s->mark_end;
   e_schirm(f, 1);
   break;
  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9':
   s->pt[c-'0'] = e_set_pnt(b->b.x, b->b.y);
   s->fa.y = b->b.y;  s->fa.x = b->b.x;  s->fe.x = b->b.x+1;
   e_schirm(f, 1);
   break;
 }
 return 0;
}

/* The bytes of the line Ctrl-O E puts in: the end-of-page mark, then the
   line break. */
#define E_HELP_PAGE_END_BYTES 2

/**
 * e_help_page_end_insert - Ctrl-O E: put the help file's end-of-page mark on
 * a line of its own above the cursor's line.
 * @b: the buffer.
 *
 * The new line ends with a line break, so the mark never runs into the
 * user's line; one Ctrl-U takes the whole line away and one Ctrl-R brings it
 * back, and the user's lines stay as they were.
 * Return: E_BUFFER_OK, or the owner's refusal, said on the status row.
 */
static int e_help_page_end_insert(BUFFER *b)
{
 STRING line;
 int r;

 if (e_buffer_line_new(b, &line) != E_BUFFER_OK)
 {
  wpe_buffer_say_nomem(b, "a new line");
  return(E_BUFFER_NOMEM);
 }
 line.s[0] = HFE;
 line.s[1] = WPE_WR;
 line.s[E_HELP_PAGE_END_BYTES] = '\0';
 line.len = 1;
 line.nrc = E_HELP_PAGE_END_BYTES;
 r = e_buffer_splice(b, b->b.y, 0, &line, 1, NULL, 0, E_BUFFER_STEP_NEW);
 if (r != E_BUFFER_OK)
 {
  free(line.s);
  if (r == E_BUFFER_NOMEM)
   wpe_buffer_say_nomem(b, "a new line");
  return(r);
 }
 b->b.x = line.len;
 return(E_BUFFER_OK);
}

/**
 * e_is_help_mark - Is @c one of the control bytes a help file marks its
 * buttons, headlines, highlights and page ends with?
 */
static int e_is_help_mark(unsigned char c)
{
 return(c == HBG || c == HED || c == HHD || c == HBB || c == HFE || c == HFB);
}

/**
 * e_help_marks_delete - Ctrl-O L: delete the help file's marks from the
 * cursor's line, so a line copied from a help page reads as plain text.
 * @b, @s: the buffer and its screen.
 *
 * Stops at the first mark that could not be deleted (a window the user
 * cannot change), so a refusal never turns into a loop.
 */
static void e_help_marks_delete(BUFFER *b, SCHIRM *s)
{
 int i = 0;

 while (i < b->bf[b->b.y].len)
 {
  if (!e_is_help_mark(b->bf[b->b.y].s[i]))
   i++;
  else if (e_del_nchar(b, s, i, b->b.y, 1) == E_CHAR_REFUSED)
   return;
 }
}

/**
 * e_help_mark_block_fits - Can Ctrl-O U, M or H mark the block?  Only a
 * block inside one row, at least one character long, is a button, a
 * highlight or a headline.
 */
static int e_help_mark_block_fits(SCHIRM *s)
{
 if (s->mark_begin.y != s->mark_end.y || s->mark_begin.y < 0)
  return(0);
 return(s->mark_begin.x < s->mark_end.x);
}

/**
 * e_help_mark_pieces - The two marks of Ctrl-O U, M or H, put in around
 * the block of @s.
 * @plan: the mark that opens it (HBG, HBB or HHD).
 *
 * Runs on the owner's copy of the block's row, so a wrap refused for want
 * of memory leaves no Ctrl-U behind for a mark that never went in.
 */
static void e_help_mark_pieces(BUFFER *b, SCHIRM *s, int top, void *plan)
{
 unsigned char end = HED;
 unsigned char *start = plan;

 e_ins_nchar(b, s, &end, s->mark_end.x, s->mark_end.y, 1);
 e_ins_nchar(b, s, start, s->mark_begin.x, s->mark_end.y, 1);
}

/**
 * e_help_mark_block - Ctrl-O U, M and H: make the block a button, a
 * highlight or a headline of a help file.
 * @f: the window; @b, @s: its buffer and screen; @start: the help mark
 * that opens it (HBG, HBB or HHD).
 *
 * The end mark goes behind the block and @start in front of it, as one
 * action: one Ctrl-U takes both marks away, one Ctrl-R puts both back.  On
 * a full row the end mark makes it wrap; when memory runs out for that,
 * neither mark goes in and the status row says so.
 */
static void e_help_mark_block(FENSTER *f, BUFFER *b, SCHIRM *s, unsigned char start)
{
 if (!e_help_mark_block_fits(s))
  return;
 e_edit_action_run(b, s->mark_end.y, s->mark_end.y, E_BUFFER_FAULT_HELP_MARKS,
                   "the help marks", e_help_mark_pieces, &start);
 e_schirm(f, 1);
}

/**
 * e_ctrl_o - Ctrl-O: read the editing command's letter and run it.
 * @f: the window the prefix was pressed in.
 *
 * Like Ctrl-K, what comes while the prefix waits is shown after the command.
 * Return: 0.
 */
int e_ctrl_o(FENSTER *f)
{
 BUFFER *b = f->ed->f[f->ed->mxedt]->b;
 SCHIRM *s = f->ed->f[f->ed->mxedt]->s;
 int i, c;

 c = e_prefix_read_letter();
 if (c < 32) c = c + 'A' - 1;
 switch(c)
 {
  case 'Y':     /*  delete end of line    */
   if (e_window_text_locked(f)) break;
   e_delete_to_end_of_row(b, s);
   e_schirm(f, 1);
   e_cursor(f, 1);
   break;
  case 'T':     /*  delete up to beginning of next word    */
   if (e_window_text_locked(f)) break;
   if (b->b.x <= 0 && b->b.y > 0)
   {
    b->b.y--;
    b->b.x = b->bf[b->b.y].len;
   }
   else if (b->b.x > 0)
   {
    c = b->b.x;
    b->b.x = e_su_rblk(b->b.x - 1, b->bf[b->b.y].s);
    e_del_nchar(b, s, b->b.x, b->b.y, c-b->b.x);
   }
   e_schirm(f, 1);
   break;
  case 'F':     /*  find string    */
   e_find(f);
   break;
  case 'A':     /*  replace string    */
   e_replace(f);
   break;
#ifdef PROG
  case 'S':     /*  find declaration    */
   e_sh_def(f);
   break;
  case 'N':     /*  ...next...  */
   e_sh_nxt_def(f);
   break;
  case 'K':     /*  next bracket  */
   e_nxt_brk(f);
   break;
  case 'B':     /*  beautify text  */
   e_p_beautify(f);
   break;
#endif
  case 'U':     /*   for help file: create button */
   e_help_mark_block(f, b, s, HBG);
   break;
  case 'M':     /*   for help file: Mark-Line  */
   e_help_mark_block(f, b, s, HBB);
   break;
  case 'H':     /*   for help file: create Header  */
   e_help_mark_block(f, b, s, HHD);
   break;
  case 'E':     /*   for help file: create end mark  */
   e_help_page_end_insert(b);
   e_schirm(f, 1);
   break;
  case 'L':     /*   for help file: delete "help" special chars  */
   e_help_marks_delete(b, s);
   e_schirm(f, 1);
   break;
  case 'C':     /*   for help file: check help file   */
   e_help_comp(f);
   break;
  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9':
   b->b.x = s->pt[c-'0'].x;  b->b.y = s->pt[c-'0'].y;
   s->fa.y = b->b.y;  s->fa.x = b->b.x;  s->fe.x = b->b.x+1;
   e_cursor(f, 1);
   break;
 }
 return(0);
}

/* general function key dispatcher 
   basically, every time when it returns with zero
   something has happened */
int e_tst_dfkt(FENSTER *f, int c)
{
 if (c >= Alt1 && c <= Alt9)
 {
  e_switch_window(c - Alt1 + 1, f);
  return(0);
 }
 if (c >= 1024 && c <= 1049)
 {
  e_switch_window(c - 1014, f);
  return(0);
 }
 switch (c)
 {
  case F1:
  case HELP:
   e_help_loc(f, 0);
   break;
#if defined(PROG)
  case CF1:
   if (WpeIsProg())
    e_topic_search(f);
   break;
  case AF1:
   if (WpeIsProg())
    e_funct_in(f);
   break;
#endif
  case WPE_ESC:
   /* Borland/Turbo Vision faithful: a bare Esc opens nothing.  F10 (and
      Alt-Space) enter the menu; Esc only CANCELS an already-open dialog or
      menu, and those have their own input loops that consume it -- so here,
      with the editor on top and nothing open, Esc is a no-op.  (It must not
      fall through to the default branch, which would insert it as text.)
      In Messages it lets go of the list, back to the source window. */
#if defined(PROG)
   if (WpeIsProg())
    e_messages_return(f);
#endif
   break;
  case F10:
  case AltBl:
   WpeHandleMainmenu(-1, f);
   break;
  case Alt0:
   e_list_all_win(f);
   break;
  case AF5:
   e_user_screen(f);
   break;
  default:
   if (f->ed->edopt & ED_CUA_STYLE)
   {
    switch (c)
    {
     case CtrlL:
      e_size_move(f);
      break;
     case OPEN:
     case F2:
      WpeManager(f);
      break;
     case SF2:
      WpeManagerFirst(f);
      break;
     case CF4:
     case AltX:
     case CtrlW:        /* modern close-window (browser/VSCode/IntelliJ tab) */
      e_close_window(f);
      break;
     case FRONT:
     case AltZ:
     case SF4:
      e_ed_cascade(f);
      break;
     case SF5:
      e_ed_tile(f);
      break;
     case SF6:
      e_ed_zoom(f);
      break;
     case CF6:
     case AltN:
      e_ed_next(f);
      break;
     case STOP:
     case AF4:
      e_quit(f);
      break;
     default:
      return(c);
    }
   }
   else
   {
    switch(c)
    {
     case CF5:
     case AF2:
      e_size_move(f);
      break;
     case OPEN:
     case F3:
      WpeManager(f);
      break;
     case CF3:
      WpeManagerFirst(f);
      break;
     case CtrlW:        /* modern close-window; Alt-F3 dropped (WM/kernel grabs it) */
      e_close_window(f);
      break;
     case FRONT:
     case AltZ:
     case F5:
      e_ed_zoom(f);
      break;
     case F6:
     case AltN:
      e_ed_next(f);
      break;
     case STOP:
     case AltX:
      e_quit(f);
      break;
     default:
      return(c);
    }
   }
 }
 return(0);
}

int e_chr_sp(int x, BUFFER *b, FENSTER *f)
{
 int i, j;

 for (i = j = 0; i + j < x && i < b->bf[b->b.y].len; i++)
 {
  unsigned char uc = (unsigned char) *(b->bf[b->b.y].s + i);
  if (uc == WPE_TAB)
   j += (f->ed->tabn - ((j + i) % f->ed->tabn) - 1);
#ifdef UNIX
  else if (uc < ' ')
   j++;
  else if (UTF8_IS_LEAD(uc))
   i += e_utf8_visual_step((unsigned char *)b->bf[b->b.y].s, i,
                           b->bf[b->b.y].len, &j);
  if (f->dtmd == DTMD_HELP)
  {
   if (b->bf[b->b.y].s[i] == HBG || b->bf[b->b.y].s[i] == HFB ||
     b->bf[b->b.y].s[i] == HED || b->bf[b->b.y].s[i] == HHD ||
     b->bf[b->b.y].s[i] == HFE || b->bf[b->b.y].s[i] == HBB ||
     b->bf[b->b.y].s[i] == HNF)
    j -= 2;
  }
#endif
 }
 return(i);
}

/****************************************/
int FindFirstNospaceChar(BUFFER *b, int line)
/* returns char number in the line, -1 otherwise */
{
  int k;
  /* advance x while char[k] == space() */
  for(k = 0; k < b->bf[line].len; k++) {
    if(!isspace(b->bf[line].s[k])) {
      return k;
    }
  }
  return -1;
}

/****************************************/
int GetXOfCharNum(BUFFER *b, int line, int char_num)
{
   int x, k;
   
   for(x = 0, k = 0; k < b->bf[line].len; k++) {
     if(k == char_num) {
       return x;
     }
     x = ((b->bf[line].s[k] == '\t') ?
          (x / b->cn->tabn + 1) * b->cn->tabn : x + 1);
   }
   /* return 0 to secure result interpretation */
   return 0;
}

/****************************************/
int GetCharNumOfX(BUFFER *b, int line, int char_x)
{
   int x, k, next_x;
   
   for(x = 0, k = 0; k < b->bf[line].len; k++) {
     next_x = ((b->bf[line].s[k] == '\t') ?
          (x / b->cn->tabn + 1) * b->cn->tabn : x + 1);
     if(next_x > char_x) {
       return x;
     }
     x = next_x;
   }
   /* return 0 to secure result interpretation */
   return 0;
}
/****************************************/

/**
 * e_edit_action_run - Make an editing action of several pieces whole, or
 * not at all.
 * @b: the user's buffer; @y0, @y1: the rows its pieces work on; @point:
 * its test fault point (E_BUFFER_FAULT_*); @what: the action, in the
 * user's words; @pieces: the action's own code; @plan: what the action
 * read from the user's text before, handed to @pieces.
 *
 * The pieces run on the owner's copy of those rows and the copy becomes
 * the user's text in one change.  When memory runs out in the middle, the
 * user keeps the text, cursor and block exactly as they were and reads one
 * status line, never most of an action with no Ctrl-U for it.  The pieces
 * are one step: one Ctrl-U takes them all back.
 * Return: E_BUFFER_OK; E_BUFFER_NOTHING when there are no rows to work on;
 * E_BUFFER_LOCKED, E_BUFFER_NOMEM or E_BUFFER_RANGE when nothing changed.
 */
int e_edit_action_run(BUFFER *b, int y0, int y1, int point, const char *what,
                      EEditPieces pieces, void *plan)
{
 WpeBufferAction a;
 int r;

 e_buffer_step_begin(b);
 r = e_buffer_action_begin(&a, b, y0, y1, point, what);
 if (r == E_BUFFER_OK)
 {
  pieces(a.shell, a.shell->f->s, a.y0 - a.g_top, plan);
  r = e_buffer_action_commit(&a);
 }
 e_buffer_step_end(b);
 return(r);
}

/**
 * e_edit_action_row - The row an action at the cursor's row @y works on.
 *
 * The cursor can stand on the row past the last one (a deletion that took
 * the last row); the action then works on the last row and the piece opens
 * the row it needs on the copy, as it would on the user's text.
 */
static int e_edit_action_row(BUFFER *b, int y)
{
 if (y >= b->mxlines)
  return(b->mxlines - 1);
 return(y);
}

/**
 * e_ins_blanks - Put @n blanks in at (@x, @y): the indent Tab, Backspace
 * and Enter with auto-indent write.
 */
static void e_ins_blanks(BUFFER *b, SCHIRM *s, int x, int y, int n)
{
 char *str = MALLOC(n * sizeof(char));
 int i;

 for (i = 0; i < n; i++)
  str[i] = ' ';
 e_ins_nchar(b, s, (unsigned char *)str, x, y, n);
 FREE(str);
}

/* What Tab with auto-indent does to the cursor's row, read before it is
   changed: the blanks behind the cursor it erases, then what it puts in. */
typedef struct {
 int erase;        /* 1: erase @erase_n bytes at the cursor first */
 int erase_n;
 int ins_tab;      /* 1: a TAB character goes in; 0: @ins_n blanks */
 int ins_n;
} ETabPlan;

/**
 * e_tab_a_ind_plan - Read what Tab with auto-indent will do on the cursor's
 * row of @b: the rows above give the indent the line takes.
 *
 * Every read happens before the first change, and the change on the
 * cursor's row moves neither the cursor nor any row above it, so the plan
 * holds for the pieces that follow.
 */
static void e_tab_a_ind_plan(BUFFER *b, ETabPlan *p)
{
 int a_indent = b->cn->autoindent;
 int line, x, k;
 int do_auto_indent = 0;
 int first_nospace_k;

 p->erase = 0;
 p->erase_n = 0;
 p->ins_tab = 1;
 p->ins_n = 1;

 /* If at the right of current pos there are only space chars, tab up to
    first nospace position (auto-indent pos) of previous lines. 
    Tail spaces of current line are erased.
    If left current pos there are only spaces and even if there
    are nospaces at the right, tab up to auto-indent pos.
    Otherwise, insert TAB char.
 */

 line = b->b.y; /* current line */

 first_nospace_k = FindFirstNospaceChar(b, line);
 if (first_nospace_k > -1)
 {
  /* there is a nospace char in the line */
  /* get its x */
  if(b->b.x > GetXOfCharNum(b, line, first_nospace_k))
  {
   /* nospace char before curr pos */

   /* are there nospace chars under and after cursor pos */
   do_auto_indent = 1;
   for (k = GetCharNumOfX(b, line, b->b.x); k < b->bf[line].len; k++)
   {
    if (!isspace(b->bf[line].s[k]))
    {
     /* yes, there are */
     do_auto_indent = 0;
     break;
    }
   }
   if (do_auto_indent)
   {
    /* erase all tail spaces */
    p->erase = 1;
    p->erase_n = b->bf[line].len - b->b.x;
   }
  }
  else
  {
   /* nospace char only at or after curr pos */
   do_auto_indent = 1;
  }
 }
 else
 {
  /* there are only space chars in the line */
  do_auto_indent = 1;
  if (b->bf[line].len > 0)
  {
   /* erase all tail spaces */
   p->erase = 1;
   p->erase_n = b->bf[line].len - b->b.x;
  }
 }

 if (!do_auto_indent)
  return;                       /* insert TAB char */

 /* find x of the first nospace char of lines from above */
 x = 0;
 for (line = b->b.y - 1; (line > 0); line--)
 {
  k = FindFirstNospaceChar(b, line);
  if (k > -1)
  {
   x = GetXOfCharNum(b, line, k);
   break;
  }
 }

 if (b->b.x < x)
 {
  /* indent to x with spaces */
  p->ins_tab = 0;
  p->ins_n = x - b->b.x;
 }
 else if (b->b.x < (x + a_indent))
 {
  /* indent to x + a_indent with spaces */
  p->ins_tab = 0;
  p->ins_n = x + a_indent - b->b.x;
 }
}

/**
 * e_tab_a_ind_pieces - Erase the blanks behind the cursor of @b and put the
 * indent or the TAB in, as @plan says.
 */
static void e_tab_a_ind_pieces(BUFFER *b, SCHIRM *s, int top, void *plan)
{
 ETabPlan *p = plan;
 unsigned char tab = '\t';

 if (p->erase)
  e_del_nchar(b, s, b->b.x, b->b.y, p->erase_n);
 if (p->ins_tab)
  e_ins_nchar(b, s, &tab, b->b.x, b->b.y, 1);
 else
  e_ins_blanks(b, s, b->b.x, b->b.y, p->ins_n);
}

/**
 * e_tab_a_ind - Tab with auto-indent: indent the line like the ones above.
 * @b: the buffer; @s: its screen.
 *
 * Erasing the blanks behind the cursor and putting the indent in are one
 * action: one Ctrl-U gives the line back as it was before the Tab, one
 * Ctrl-R indents it again.  When memory runs out, the line stays as it was
 * and the status row says so.
 * Return: the cursor's column, or E_CHAR_REFUSED when nothing changed.
 */
int e_tab_a_ind(BUFFER *b, SCHIRM *s)
{
 ETabPlan p;
 int y = e_edit_action_row(b, b->b.y);

 e_tab_a_ind_plan(b, &p);
 if (e_edit_action_run(b, y, y, E_BUFFER_FAULT_INDENT, "the indent", e_tab_a_ind_pieces,
                       &p) != E_BUFFER_OK)
  return(E_CHAR_REFUSED);
 return(b->b.x);
}

/**
 * e_char_is_blank_at - Is the character at (@x, @y) a blank or a tab?
 *
 * A byte of text that is not ASCII is never a blank, so Backspace with
 * auto-indent over a Latin-1 letter or half of a cut character takes just
 * that byte.
 */
static int e_char_is_blank_at(BUFFER *b, int x, int y)
{
 if (e_char_bytes_at(b, x, y) != E_CHAR_ONE_BYTE)
  return(0);
 return(isspace(b->bf[y].s[x]) != 0);
}

/* The last deletion of Backspace with auto-indent takes the character at
   the cursor, however many bytes it has. */
#define E_DEL_A_IND_CHAR (-1)

/* What Backspace with auto-indent does to the cursor's row, read before it
   is changed. */
typedef struct {
 int reindent;     /* 1: the indent's tabs become blanks first */
 int del0;         /* the bytes deleted at column 0 for that */
 int blanks;       /* the blanks put back there */
 int x;            /* the column of the last deletion */
 int n;            /* its bytes, or E_DEL_A_IND_CHAR */
 int taken;        /* the bytes the last deletion took, once made */
} EDelAIndPlan;

/**
 * e_del_a_ind_plan - Read what Backspace with auto-indent will do on the
 * cursor's row of @b: the rows above give the indent the line goes back to.
 *
 * The pieces change only the cursor's row, so the rows above read here are
 * the rows the line is indented like, a '#' line or a far indent between
 * them included.
 */
static void e_del_a_ind_plan(BUFFER *b, EDelAIndPlan *p)
{
 int i = 1, j = -1, k, x = b->b.x, y = b->b.y;

 p->reindent = 0;
 p->del0 = 0;
 p->blanks = 0;
 if (y > 0)
 {
  for (i = 0; i < b->bf[y].len && i < x && isspace(b->bf[y].s[i]); i++)
   ;
  if (i == x && e_char_is_blank_at(b, x, y))
  {
   for (j = 0, i = 0; j <= x; j++)
    i = b->bf[y].s[j] == '\t' ? (i / b->cn->tabn + 1) * b->cn->tabn : i + 1;
   if (i != j)
   {
    p->reindent = 1;
    p->del0 = j;
    p->blanks = i;
    x = i - 1;
   }
   for (j = y - 1; j >= 0; j--)
   {
    for (i = 0, k = 0; k < b->bf[j].len && isspace(b->bf[j].s[k]); k++)
     i = b->bf[j].s[k] == '\t' ? (i / b->cn->tabn + 1) * b->cn->tabn : i + 1;
    if (k < b->bf[j].len && b->bf[j].s[k] != '#' && i <= x)
    {
     i = x - i + 1;
     x -= i - 1;
     break;
    }
   }
   if (j < 0)
    x = 0;
  }
 }
 p->x = x;
 p->n = i;
 if (j < 0)
  p->n = E_DEL_A_IND_CHAR;
}

/**
 * e_del_a_ind_pieces - Turn the indent's tabs of the cursor's row of @b
 * into blanks when @plan says so, then make the deletion it names.
 */
static void e_del_a_ind_pieces(BUFFER *b, SCHIRM *s, int top, void *plan)
{
 EDelAIndPlan *p = plan;

 if (p->reindent)
 {
  e_del_nchar(b, s, 0, b->b.y, p->del0);
  e_ins_blanks(b, s, 0, b->b.y, p->blanks);
 }
 b->b.x = p->x;
 p->taken = p->n;
 if (p->n == E_DEL_A_IND_CHAR)
  p->taken = e_char_bytes_at(b, b->b.x, b->b.y);
 e_del_nchar(b, s, b->b.x, b->b.y, p->taken);
}

/**
 * e_del_a_ind - Backspace with auto-indent.
 * @b: the buffer; @s: its screen.
 *
 * Inside the indent, with the cursor among the leading blanks, the line is
 * re-indented like the last line that started before the cursor: its tabs
 * become blanks and the blanks past that line's indent go.  Those pieces
 * are one action: one Ctrl-U gives the line back as it was before the
 * Backspace, one Ctrl-R deletes again.  When memory runs out the line stays
 * as it was and the status row says so.
 * Elsewhere, and over a character that is not a blank (the first letter
 * after the indent, a character of several bytes), Backspace takes the
 * whole character at the cursor, as it does without auto-indent.
 * Return: how many bytes the last deletion took, or E_CHAR_REFUSED when
 * nothing changed.
 */
int e_del_a_ind(BUFFER *b, SCHIRM *s)
{
 EDelAIndPlan p;
 int y = e_edit_action_row(b, b->b.y);

 e_del_a_ind_plan(b, &p);
 if (e_edit_action_run(b, y, y, E_BUFFER_FAULT_INDENT, "the deletion", e_del_a_ind_pieces,
                       &p) != E_BUFFER_OK)
  return(E_CHAR_REFUSED);
 return(p.taken);
}

/**
 * e_backspace_a_ind - Backspace in a window that indents by itself, the
 * key having moved the cursor of @b onto what it deletes.
 * @at_key: where the cursor was when the key came.
 *
 * Refused (memory ran out), the text is unchanged and so is the cursor:
 * the user sees nothing move, and the status row says why.
 */
static void e_backspace_a_ind(BUFFER *b, SCHIRM *s, POINT at_key)
{
 if (e_del_a_ind(b, s) == E_CHAR_REFUSED)
  b->b = at_key;
}

/**
 * e_row_byte_within - The byte @k of row @y of @b, read as a row of @len
 * bytes: a byte at or past @len reads as the end of the row.
 */
static int e_row_byte_within(BUFFER *b, int y, int k, int len)
{
 if (k >= len)
  return('\0');
 return(b->bf[y].s[k]);
}

/**
 * e_car_a_ind_width - How far Enter indents the line it makes when it
 * breaks row @y at byte @head_len.
 *
 * The line above the new one is row @y's first @head_len bytes, the head
 * the break leaves; a '#' line takes the indent from the line above it, and
 * a '{' that ends the line gives one column less.  Read on the user's text
 * before the break, so every line above counts, however far up.
 * Return: the blanks to put in, 0 for none.
 */
static int e_car_a_ind_width(BUFFER *b, int y, int head_len)
{
 int i, j = y + 1, k, len;

 do
 {
  j--;
  len = b->bf[j].len;
  if (j == y)
   len = head_len;
  for (i = 0, k = 0;
    k < len && (isspace(b->bf[j].s[k]) || e_row_byte_within(b, j, i, len) == '{'); k++)
   i = b->bf[j].s[k] == '\t' ? (i / b->cn->tabn + 1) * b->cn->tabn : i + 1;
 } while (j > 0 && e_row_byte_within(b, j, k, len) == '#');
 if (k == len && k > 0 && b->bf[j].s[k-1] == '{')
  i--;
 return(i);
}

/*   write n blanks */
int e_blk(int anz, int xa, int ya, int col)
{
 for (anz--; anz >= 0 ; anz--)
  e_pr_char(xa+anz, ya, ' ', col);
 return(anz);
}

/**
 * e_char_count_change - Count @n changed characters toward the window's
 * modified mark.
 * @b: the buffer written.
 *
 * The mark lands on the window that owns the buffer, never on the one on
 * top, and a view the editor writes (Messages, a man page) never reads as
 * modified.
 */
static void e_char_count_change(BUFFER *b, int n)
{
 if (b->f != NULL && b->history == E_BUFFER_HISTORY_FULL)
  b->f->save += n;
}

/**
 * e_char_row_missing - Is there no row at @y for @n bytes to go into?
 * @b: the buffer; @what: what the status row names if memory runs out.
 *
 * A deletion that emptied the last row of the text took the row with it,
 * and the change that puts text back there (Undo of Ctrl-T on the last
 * word, the replacement Search > Replace writes where it deleted) names
 * the row it was on.  The owner opens that row again, so the text goes
 * back on a line of its own and never into memory the row no longer has.
 * Nothing to put there needs no row.
 * Return: 1 when the change must not go on (nothing was written).
 */
static int e_char_row_missing(BUFFER *b, int y, int n, const char *what)
{
 int r;

 if (y < b->mxlines)
  return(0);
 if (n <= 0)
  return(1);
 r = e_buffer_open_row(b, y);
 if (r == E_BUFFER_NOMEM)
  wpe_buffer_say_nomem(b, what);
 return(r != E_BUFFER_OK);
}

/**
 * e_car_ret_split - Break the line at the cursor, without auto-indent.
 * @b: the buffer; @s: its screen.
 *
 * The line break the user typed, and what Undo of a joined line puts back:
 * the line keeps its breakpoint and the text behind the cursor moves down
 * with its marks.  At the end of a row without a line break the row gets
 * its break and the row below becomes a line of its own, a full row of
 * Max. Columns bytes included.  Undo never indents again, so Redo of an
 * undone Enter gives the same text.  Return: E_BUFFER_OK, or E_BUFFER_NOMEM
 * when the line could not be split, E_BUFFER_LOCKED in a window the user
 * cannot change (nothing changed either way, the status row says so).
 */
int e_car_ret_split(BUFFER *b, SCHIRM *s)
{
 int x = b->b.x, y = b->b.y, r;

 if (e_buffer_change_refused(b))
  return(E_BUFFER_LOCKED);
 if (e_char_row_missing(b, y, 1, "a line break"))
  return(E_BUFFER_RANGE);
 r = e_buffer_split_line(b, y, x, E_BUFFER_STEP_COVERED, E_BUFFER_HEAD_WR);
 if (r != E_BUFFER_OK)
 {
  wpe_buffer_say_nomem(b, "a line break");
  return(r);
 }
 e_add_undo('a', b, x, y, 1);
 e_char_count_change(b, 1);
 if (y < b->mxlines - 1)
 {
  b->b.y = y + 1;
  b->b.x = 0;
 }
 return(E_BUFFER_OK);
}

/* f->flg: the window indents automatically (a program's text). */
#define E_FLG_AUTO_INDENT 1

/**
 * e_window_auto_indents - Does window @f indent a new line by itself?
 *
 * Read from the user's window, never from the owner's copy the pieces run
 * on, which has no settings of its own.
 */
static int e_window_auto_indents(FENSTER *f)
{
 return(f != NULL && (f->flg & E_FLG_AUTO_INDENT) != 0);
}

/**
 * e_car_ret_head_len - How many bytes of the cursor's row of @b Enter
 * leaves above the new line: the bytes before the cursor.
 */
static int e_car_ret_head_len(BUFFER *b)
{
 if (b->b.x > b->bf[b->b.y].len)
  return(b->bf[b->b.y].len);
 return(b->b.x);
}

/**
 * e_car_ret_pieces - Break the line at the cursor of @b and, when the break
 * was made, indent the new line by the blanks @plan counts.
 */
static void e_car_ret_pieces(BUFFER *b, SCHIRM *s, int top, void *plan)
{
 int *indent = plan;

 if (e_car_ret_split(b, s) != E_BUFFER_OK || *indent <= 0)
  return;
 e_ins_blanks(b, s, 0, b->b.y, *indent);
 b->b.x = *indent;
}

/**
 * e_car_ret - Enter: break the line at the cursor, then auto-indent.
 * @b: the buffer; @s: its screen.
 *
 * The break and its indent are one action: one Ctrl-U takes both back.
 * When memory runs out for either, the line stays as it was and the status
 * row says so.  Whether the new line is indented is the user's window's
 * setting, and how far is read from the lines above before the break.
 * Return: the cursor's new line.
 */
int e_car_ret(BUFFER *b, SCHIRM *s)
{
 int indent = 0, y = e_edit_action_row(b, b->b.y);

 if (e_window_auto_indents(b->f) && b->b.y < b->mxlines)
  indent = e_car_a_ind_width(b, b->b.y, e_car_ret_head_len(b));
 e_edit_action_run(b, y, y, E_BUFFER_FAULT_ENTER, "a line break", e_car_ret_pieces, &indent);
 return(b->b.y);
}

/*   cursor placement */
static int e_cursor_visual_adjust(FENSTER *f)
{
 BUFFER *b = f->b;
 int i, j;

 for (i = j = 0; i < b->b.x; i++)
 {
  unsigned char uc = (unsigned char) *(b->bf[b->b.y].s + i);
  if (uc == WPE_TAB)
   j += (f->ed->tabn - ((j + i) % f->ed->tabn) - 1);
  else if (uc < ' ') j++;
  else if (UTF8_IS_LEAD(uc))
   i += e_utf8_visual_step((unsigned char *)b->bf[b->b.y].s, i,
                           b->bf[b->b.y].len, &j);
 }
 return j;
}

static int e_cursor_in_viewport(FENSTER *f, int sx, int sy)
{
 return sx > f->a.x && sx < f->e.x && sy > f->a.y && sy < f->e.y;
}

/**
 * e_cursor_keep_on_text - Before the cursor is drawn, make sure it stands on
 * a place of the text.
 * @b: the buffer whose cursor is about to be painted.
 *
 * Every key, click and replay should put the cursor on a place through the
 * owner of the text; this is the last defence for a writer that did not.  A
 * cursor off the text (past a row's end, on a row that is gone) is put back
 * in it, as it always was, and one between the bytes of a character is
 * traced as well.  The trace line is how a test sees the writer that was
 * missed.  A cursor between the bytes is left where it is for now: it is
 * put on the character's start once every writer is the owner.
 */
static void e_cursor_keep_on_text(BUFFER *b)
{
 int len = -1;

 if (e_buffer_cursor_fits(b))
  return;
 if (b->b.y >= 0 && b->b.y < b->mxlines)
  len = b->bf[b->b.y].len;
 wpe_buffer_trace("buffer position off a glyph y=%d x=%d len=%d", b->b.y, b->b.x, len);
 if (e_buffer_cursor_in_text(b))
  return;
 e_buffer_cursor_set(b, b->b.y, b->b.x);
}

void e_cursor_pos_only(FENSTER *f)
{
 BUFFER *b = f->b;
 SCHIRM *s = f->s;
 int j, sx, sy;
 extern int cur_on;

 if (!DTMD_ISTEXT(f->dtmd)) return;
 e_cursor_keep_on_text(b);

 j = e_cursor_visual_adjust(f);
 b->cl = b->b.x + j;
 sx = f->a.x + b->b.x - s->c.x + j + 1;
 sy = f->a.y + b->b.y - s->c.y + 1;

 if (e_cursor_in_viewport(f, sx, sy))
  fk_locate(sx, sy);
 else
 {
  int visible = f->e.y - f->a.y - 1;
  b->b.y = s->c.y + visible / 2;
  if (b->b.y >= b->mxlines) b->b.y = b->mxlines - 1;
  if (b->b.y < 0) b->b.y = 0;
  b->b.x = 0;
  j = e_cursor_visual_adjust(f);
  b->cl = b->b.x + j;
  fk_locate(f->a.x + b->b.x - s->c.x + j + 1,
    f->a.y + b->b.y - s->c.y + 1);
 }
}

void e_cursor(FENSTER *f, int sw)
{
 BUFFER *b = f->b;
 SCHIRM *s = f->s;
 static int iold = 0, jold = 0;
 int i, j;

 if (!DTMD_ISTEXT(f->dtmd)) return;
 e_cursor_keep_on_text(b);
 for (i = j = 0; i < b->b.x; i++)
 {
  unsigned char uc = (unsigned char) *(b->bf[b->b.y].s + i);
  if (uc == WPE_TAB)
   j += (f->ed->tabn - ((j + i) % f->ed->tabn) - 1);
  else if (uc < ' ') j++;
  else if (UTF8_IS_LEAD(uc))
   i += e_utf8_visual_step((unsigned char *)b->bf[b->b.y].s, i,
                           b->bf[b->b.y].len, &j);
  if (f->dtmd == DTMD_HELP)
  {
   if (b->bf[b->b.y].s[i] == HBG || b->bf[b->b.y].s[i] == HED ||
     b->bf[b->b.y].s[i] == HHD || b->bf[b->b.y].s[i] == HFE ||
     b->bf[b->b.y].s[i] == HFB || b->bf[b->b.y].s[i] == HBB ||
     b->bf[b->b.y].s[i] == HNF)
    j -= 2;
  }
 }
 if (b->b.y - s->c.y < 0 || b->b.y - s->c.y >= NUM_LINES_ON_SCREEN - 1 ||
   s->c.y < 0 || s->c.y >= b->mxlines ||
   b->b.x + j - s->c.x < 0 ||
   b->b.x + j- s->c.x >= NUM_COLS_ON_SCREEN - 1 )
 {
#if defined(UNIX)
      /*if(b->b.y - s->c.y < 0) s->c.y = b->b.y - (f->e.y - f->a.y)/2;
      else if(b->b.y - s->c.y >= f->e.y - f->a.y -1)
      s->c.y = b->b.y - (f->e.y - f->a.y)/2;*/
  if (b->b.y - s->c.y < -1)
  {
   s->c.y = b->b.y - (NUM_LINES_ON_SCREEN)/2;
  }
  else if (b->b.y - s->c.y == -1)
  {
   s->c.y -= 1;
  }
  else if (b->b.y - s->c.y > NUM_LINES_ON_SCREEN -1)
  {
   s->c.y = b->b.y - (NUM_LINES_ON_SCREEN)/2;
  }
  else if (b->b.y - s->c.y == NUM_LINES_ON_SCREEN -1)
  {
   s->c.y += 1;
  }
#else
  if (b->b.y - s->c.y < 0) s->c.y = b->b.y;
  else if (b->b.y - s->c.y >= NUM_LINES_ON_SCREEN -1)
   (s->c.y) = b->b.y - f->e.y + f->a.y + 2;
#endif
  if (s->c.y >= b->mxlines - 1) s->c.y = b->mxlines - 2 ;
  if (s->c.y < 0) s->c.y = 0 ;

  if (b->b.x + j - s->c.x < 0)
   (s->c.x) = b->b.x + j - (NUM_COLS_ON_SCREEN)/2 ;
  else if (b->b.x + j - s->c.x >= NUM_COLS_ON_SCREEN - 1)
   (s->c.x) = b->b.x + j - (NUM_COLS_ON_SCREEN)/2 ;
  if (s->c.x < 0) s->c.x = 0 ;
  else if (s->c.x >= b->bf[b->b.y].len + j) s->c.x = b->bf[b->b.y].len + j;
  e_schirm(f, sw);
 }
 if (s->fa.y == -1) {  e_schirm(f, sw);  s->fa.y--;  }
 else if (s->fa.y > -1) s->fa.y = -1;
 if (sw != 0)
 {
  iold = e_lst_zeichen(f->e.x, f->a.y+1, f->e.y-f->a.y-1, 0,
    f->fb->em.fb, b->mxlines, iold, b->b.y);
  jold = e_lst_zeichen(f->a.x+19, f->e.y, f->e.x-f->a.x-20, 1,
    f->fb->em.fb, b->mx.x, jold, b->b.x);
 }
 b->cl = b->b.x+j;
 fk_locate(f->a.x+b->b.x-s->c.x+j+1, f->a.y+b->b.y-s->c.y+1);
}

/**
 * e_del_line - Delete line @yd (Ctrl-Y, Backspace on an empty line).
 * @b: the buffer; @s: its screen (the owner keeps the window's own marks).
 *
 * One Ctrl-U brings the line back with its breakpoint, the cursor and the
 * block where they were; the only line of a file is undoable too.
 * Return: the number of lines left.
 */
static int e_del_line(int yd, BUFFER *b, SCHIRM *s)
{
 if (e_buffer_delete_lines(b, yd, 1, E_BUFFER_STEP_NEW) == E_BUFFER_NOMEM)
  wpe_buffer_say_nomem(b, "the line deletion");
 return(b->mxlines);
}

/**
 * e_char_record - Compose and keep the record of a character change.
 * Return: the record as kept (the cover's pocket), or NULL when covered,
 * on a view, or out of memory.
 */
static Undo *e_char_record(int sw, BUFFER *b, int x, int y, int n);

/*   delete N chars from buffer */
int e_del_nchar(BUFFER *b, SCHIRM *s, int x, int y, int n)
{
 Undo *rec;
 int len, i, j;

 if (e_buffer_change_refused(b))
  return(E_CHAR_REFUSED);
 e_char_count_change(b, n);
 rec = e_char_record('r', b, x, y, n);
 e_buffer_cover_begin(b, rec);
 len = b->bf[y].len;
 if (*(b->bf[y].s+len) == WPE_WR) len++;
 for (j = x; j <= len - n; ++j) *(b->bf[y].s+j) = *(b->bf[y].s+j+n)
  ;
 if (s->mark_begin.y == y && s->mark_begin.x > x)
  s->mark_begin.x = (s->mark_begin.x-n > x) ? s->mark_begin.x-n : x;
 if (s->mark_end.y == y && s->mark_end.x > x)
  s->mark_end.x = (s->mark_end.x-n > x) ? s->mark_end.x-n : x;

 if (len <= n) e_del_line(y, b, s);
 else if (y < b->mxlines - 1 && *(b->bf[y].s+len-n-1) != WPE_WR )
 {
  /* A next row that fits the room left, exactly filling it included, comes
     up whole with its line break; a longer one only up to its last word. */
  if (b->mx.x+n-len >= b->bf[y+1].len)
   i = b->bf[y+1].len;
  else
   i = e_su_rblk(b->mx.x+n-len, b->bf[y+1].s);
  if (b->bf[y+1].s[i] == WPE_WR) i++;
  if (i > 0)
  {
   for (j = 0; j < i; ++j)
    *(b->bf[y].s+len-n+j) = *(b->bf[y+1].s+j);
   *(b->bf[y].s+len-n+i) = '\0';
   if (s->mark_begin.y == y+1 && s->mark_begin.x <= i)
   {  s->mark_begin.y--;  s->mark_begin.x += (len-n);  }
   if (s->mark_end.y == y+1 && s->mark_end.x <= i)
   {  s->mark_end.y--;  s->mark_end.x += (len-n);  }
   e_del_nchar(b, s, 0, y+1, i);
  }
 }
 if (y < b->mxlines)
 {
  b->bf[y].len = e_str_len(b->bf[y].s);
  b->bf[y].nrc = e_str_nrc(b->bf[y].s);
 }
 e_buffer_cover_end(b);
 sc_txt_4(y, b, 0);
 return(x+n);
}

/* A wrap that keeps nothing on its row: the run takes the row by itself and
   what the row held goes to a new row below it. */
#define E_INS_CUT_NONE 0
/* The fewest bytes a row must keep beside a run for a cut to be looked for. */
#define E_INS_ROOM_MIN 1

/**
 * e_ins_mark_follows_cut - A block mark on a row that wraps at column @c:
 * a mark on the text that moved goes with it to the start of the row below.
 */
static void e_ins_mark_follows_cut(POINT *p, int ya, int c)
{
 if (p->y != ya || p->x < c)
  return;
 p->y++;
 p->x -= c;
}

/**
 * e_ins_mark_follows_run - A block mark at or after the place @n bytes go
 * in on row @ya moves right with the text behind it.
 */
static void e_ins_mark_follows_run(POINT *p, int ya, int xa, int n)
{
 if (p->y == ya && p->x >= xa)
  p->x += n;
}

/**
 * e_ins_wrap_tail_down - Typing past the margin of a row: the text behind
 * column @c goes to a new row below it.
 *
 * The breakpoints and marks below move down with their rows, so a wrapped
 * paragraph never leaves a breakpoint on the wrong code.  The new row ends
 * as the wrapped one did: with its line break, or without one when it was
 * the last row of a text that ends without a line break or the line goes
 * on below, so the file written keeps its bytes.
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM when no row could be added (the
 * text is unchanged and the status row says why).
 */
static int e_ins_wrap_tail_down(BUFFER *b, int ya, int c)
{
 STRING *row, *below;
 int tail, hard;

 if (e_buffer_insert_blank(b, ya + 1, E_BUFFER_STEP_COVERED) != E_BUFFER_OK)
 {
  wpe_buffer_say_nomem(b, "the line wrap");
  return(E_BUFFER_NOMEM);
 }
 row = &b->bf[ya];
 below = &b->bf[ya + 1];
 hard = (row->s[row->len] == WPE_WR);
 tail = row->len - c;
 memcpy(below->s, row->s + c, (size_t) tail);
 below->s[tail] = '\0';
 if (hard)
 {
  below->s[tail] = WPE_WR;
  below->s[tail + 1] = '\0';
 }
 below->len = e_str_len(below->s);
 below->nrc = e_str_nrc(below->s);
 return(E_BUFFER_OK);
}

/**
 * e_ins_tail_needs_new_row - Must the text a wrap moves get a row of its
 * own?  It must when the row ends its line or the text, so nothing else
 * can take it, and when the whole row moves to make room for the run.
 */
static int e_ins_tail_needs_new_row(BUFFER *b, int ya, int c)
{
 if (c == E_INS_CUT_NONE || ya == b->mxlines - 1)
  return(1);
 return(b->bf[ya].s[b->bf[ya].len] == WPE_WR);
}

/**
 * e_ins_cut_row - Wrap row @ya at column @c: the text behind @c goes to the
 * row below, through a new row or into the start of the next row of the
 * same line, and row @ya ends there without a line break.
 * @b, @sch: the buffer and its screen.
 *
 * The moved text keeps its marks, so a block the user marked stays on the
 * same characters when typing pushes them down.
 * Return: E_BUFFER_OK, or a refusal with the text unchanged.
 */
static int e_ins_cut_row(BUFFER *b, SCHIRM *sch, int ya, int c)
{
 int tail = b->bf[ya].len - c;

 if (e_ins_tail_needs_new_row(b, ya, c))
 {
  if (e_ins_wrap_tail_down(b, ya, c) != E_BUFFER_OK)
   return(E_BUFFER_NOMEM);
 }
 else if (tail > 0)
 {
  if (e_ins_nchar(b, sch, b->bf[ya].s + c, 0, ya + 1, tail) == E_CHAR_REFUSED)
   return(E_BUFFER_RANGE);
 }
 e_ins_mark_follows_cut(&sch->mark_begin, ya, c);
 e_ins_mark_follows_cut(&sch->mark_end, ya, c);
 b->bf[ya].s[c] = '\0';
 b->bf[ya].len = e_str_len(b->bf[ya].s);
 b->bf[ya].nrc = e_str_nrc(b->bf[ya].s);
 return(E_BUFFER_OK);
}

/**
 * e_ins_cut_at_blank - Does the cut at @c come after a blank or a dash, the
 * places a word wrap prefers?
 */
static int e_ins_cut_at_blank(const unsigned char *row, int c)
{
 if (c <= 0)
  return(0);
 return(row[c - 1] == ' ' || row[c - 1] == '-');
}

/* The top bits of a byte that continues a UTF-8 character, and their mask. */
#define E_INS_UTF8_CONT      0x80
#define E_INS_UTF8_CONT_MASK 0xC0

/**
 * e_ins_cut_inside_char - Would a cut at @c split a UTF-8 character?  The
 * owner's cut keeps at least one byte, so with no room for a whole first
 * character it can fall inside one.
 */
static int e_ins_cut_inside_char(const unsigned char *row, int c)
{
 return((row[c] & E_INS_UTF8_CONT_MASK) == E_INS_UTF8_CONT);
}

/**
 * e_ins_wrap_point - Where a row that @n bytes typed at @xa would make
 * wider than Max. Columns is cut.
 * @b: the buffer; @ya: the row.
 *
 * The owner's rule for every row: after the last blank or dash that leaves
 * room for the run, else at the last character that does, never inside a
 * UTF-8 character.  A word being typed goes down with its start; an
 * unbroken run is never cut before the cursor, it is cut at the cursor and
 * what is typed starts the next row.
 * Return: the column the row keeps up to; the run stays on the row when it
 * starts before it.
 */
static int e_ins_wrap_point(BUFFER *b, int ya, int xa, int n)
{
 const unsigned char *row = b->bf[ya].s;
 int room = E_BUFFER_ROW_TEXT_MAX(b) - n, c;

 if (room < E_INS_ROOM_MIN)
  return(xa);
 c = e_buffer_wrap_cut(row, room);
 if (e_ins_cut_at_blank(row, c))
  return(c);
 if (c <= xa || e_ins_cut_inside_char(row, c))
  return(xa);
 return(c);
}

/**
 * e_ins_run_stays - Does a run typed at @xa stay on the row cut at @c?  It
 * does when it starts before the cut, and when the whole row moved down to
 * make room for it.
 */
static int e_ins_run_stays(int xa, int c)
{
 return(xa < c || c == E_INS_CUT_NONE);
}

/**
 * e_char_record_place - Keep a character record where the bytes it is about
 * stand once their line has wrapped.
 * @rec: the record (NULL under a cover: the caller's record replays it);
 * @x, @y: where the run starts now.
 *
 * Typing or a replacement past the margin moves the new text to the next
 * row with the word it belongs to; Ctrl-U takes back that text there and
 * nothing else, and Ctrl-R puts it in again at the same place.  The rows of
 * breakpoints the record keeps stay on the lines they belong to.
 */
static void e_char_record_place(Undo *rec, int x, int y)
{
 int i;

 if (rec == NULL)
  return;
 for (i = 0; i < rec->nbrk; i++)
  rec->brk[i] -= y - rec->b.y;
 rec->b.x = x;
 rec->b.y = y;
}

/**
 * e_ins_refused_wide - Refuse a change that would leave a row wider than
 * Max. Columns, before any byte moves.
 * @what: the bytes the row would have held.
 *
 * Typing wraps a row before it would overflow, so this never refuses a key;
 * when it does, the text is as it was and the trace names the row, so a test
 * sees the refusal instead of a corrupted heap or a lost byte.
 * Return: 1 (refused).
 */
static int e_ins_refused_wide(BUFFER *b, int ya, int n)
{
 wpe_buffer_trace("buffer row wider than the window y=%d len=%d n=%d width=%d",
                  ya, b->bf[ya].len, n, b->mx.x);
 return(1);
}

/**
 * e_ins_run_is_long - Is a run of @n bytes longer than any row can hold?
 * Then it goes in over several rows (e_ins_long_run).
 */
static int e_ins_run_is_long(BUFFER *b, int n)
{
 return(n > E_BUFFER_ROW_TEXT_MAX(b));
}

/**
 * e_ins_run_refused - Can @n bytes not go into row @ya at all?  A row
 * already wider than Max. Columns (a view's compiler line) has no room for
 * one more byte.  A run longer than a row always goes in (e_ins_long_run),
 * except under another change's cover, whose record could not give it back:
 * no caller puts one there.
 * Return: 1 when refused (traced, the text unchanged).
 */
static int e_ins_run_refused(BUFFER *b, int ya, int n)
{
 if (b->bf[ya].len > E_BUFFER_ROW_TEXT_MAX(b))
  return(e_ins_refused_wide(b, ya, n));
 if (e_ins_run_is_long(b, n) && e_buffer_covered(b))
  return(e_ins_refused_wide(b, ya, n));
 return(0);
}

/* The pieces a long run is joined from: the row before it, the run, the
   rest of the row with its line break. */
#define E_INS_LONG_PARTS 3
/* A block mark the long run does not move: not behind it on its row. */
#define E_INS_MARK_BEFORE (-1)

/**
 * e_ins_point_at - The row and column @off bytes into the line that starts
 * at row @y, its soft rows followed: where a run's end or a mark lands
 * after the line was wrapped again, and where a replacement that wrapped
 * now starts, so Replace searching back goes on from there.
 */
void e_ins_point_at(BUFFER *b, int y, int off, POINT *p)
{
 while (y < b->mxlines - 1 && off > b->bf[y].len
        && b->bf[y].s[b->bf[y].len] != WPE_WR)
 {
  off -= b->bf[y].len;
  y++;
 }
 if (off > b->bf[y].len)
  off = b->bf[y].len;
 p->y = y;
 p->x = off;
}

/**
 * e_ins_long_run - Put in a run longer than a row (a replacement or a
 * completion wider than Max. Columns): the row is built again from the text
 * before the run, the run and the rest of the row, wrapped by the owner over
 * as many rows as it needs and never inside a UTF-8 character.
 * @b, @sch: the buffer and its screen.
 *
 * The change is one line record of its own, so Ctrl-U gives the row back
 * exactly and Ctrl-R puts every byte in again.  The cursor ends behind the
 * run and a block mark behind the insertion stays on its character.
 * Return: the column behind the run, or E_CHAR_REFUSED when memory ran out
 * (nothing changed, the status row says so).
 */
static int e_ins_long_run(BUFFER *b, SCHIRM *sch, const unsigned char *s,
                          int xa, int ya, int n)
{
 STRING *row = &b->bf[ya], *lines;
 WpeBufferPart parts[E_INS_LONG_PARTS];
 POINT *marks[2];
 int behind[2], i, k;

 parts[0].s = row->s;
 parts[0].n = xa;
 parts[1].s = s;
 parts[1].n = n;
 parts[2].s = row->s + xa;
 parts[2].n = row->len - xa + (row->s[row->len] == WPE_WR);
 lines = e_buffer_lines_from_parts(E_BUFFER_ROW_TEXT_MAX(b), parts, E_INS_LONG_PARTS, &k);
 if (k < 0)
 {
  wpe_buffer_say_nomem(b, "the text put in");
  return(E_CHAR_REFUSED);
 }
 marks[0] = &sch->mark_begin;
 marks[1] = &sch->mark_end;
 for (i = 0; i < 2; i++)
 {
  behind[i] = E_INS_MARK_BEFORE;
  if (marks[i]->y == ya && marks[i]->x >= xa)
   behind[i] = marks[i]->x;
 }
 if (e_buffer_splice(b, ya, 1, lines, k, NULL, 0, E_BUFFER_STEP_NEW) != E_BUFFER_OK)
 {
  e_buffer_lines_free(lines, k);
  wpe_buffer_say_nomem(b, "the text put in");
  return(E_CHAR_REFUSED);
 }
 FREE(lines);
 for (i = 0; i < 2; i++)
  if (behind[i] != E_INS_MARK_BEFORE)
   e_ins_point_at(b, ya, behind[i] + n, marks[i]);
 e_ins_point_at(b, ya, xa + n, &b->b);
 sc_txt_4(ya, b, 0);
 return(b->b.x);
}

/**
 * e_ins_row_overflows - Would @n more bytes make row @ya wider than Max.
 * Columns?  Then it wraps first.
 */
static int e_ins_row_overflows(BUFFER *b, int ya, int n)
{
 return(b->bf[ya].len + n > E_BUFFER_ROW_TEXT_MAX(b));
}

/**
 * e_ins_bytes_at - Put @n bytes of @s into row @ya at column @xa, the rest
 * of the row, its line break and its zero moved right by @n.
 *
 * The bytes the user typed land where the cursor was and the row stays a
 * row: its line break and the zero after it move with its text, so its
 * length and byte count are right when measured again.  The row has room
 * (e_ins_row_overflows is false).
 */
static void e_ins_bytes_at(BUFFER *b, int ya, int xa, const unsigned char *s, int n)
{
 unsigned char *row = b->bf[ya].s;
 int j, end = b->bf[ya].len;

 if (row[end] == WPE_WR)
  end++;
 for (j = end; j >= xa; --j)
  row[j + n] = row[j];
 for (j = 0; j < n; ++j)
  row[xa + j] = s[j];
}

/**
 * e_ins_run_here - Put the @n bytes of @s into row @ya at @xa, which has
 * room for them, and leave the cursor behind them.
 *
 * The block marks behind the insertion move right with their text.
 */
static void e_ins_run_here(BUFFER *b, SCHIRM *sch, const unsigned char *s,
                           int xa, int ya, int n)
{
 if (e_ins_row_overflows(b, ya, n) && e_ins_refused_wide(b, ya, n))
  return;
 e_ins_mark_follows_run(&sch->mark_begin, ya, xa, n);
 e_ins_mark_follows_run(&sch->mark_end, ya, xa, n);
 e_ins_bytes_at(b, ya, xa, s, n);
 b->b.x = xa + n;
 b->b.y = ya;
 b->bf[ya].len = e_str_len(b->bf[ya].s);
 b->bf[ya].nrc = e_str_nrc(b->bf[ya].s);
}

/**
 * e_ins_wrapped_run - Put a run that does not fit on row @ya in, wrapping
 * the row first.
 * @rec: the run's record (NULL under a cover).
 *
 * The row is cut at its wrap point and its tail goes down; the run then
 * goes in on this row, or, when it starts at or after the cut, on the row
 * below through the same wrapping insert, which may wrap that row in turn.
 * The run is never split, so what the user typed stands together in one
 * row and Ctrl-U takes it back there and nothing else; the record follows
 * it to the row it went to.
 */
static void e_ins_wrapped_run(BUFFER *b, SCHIRM *sch, unsigned char *s,
                              int xa, int ya, int n, Undo *rec)
{
 int c = e_ins_wrap_point(b, ya, xa, n);

 if (e_ins_cut_row(b, sch, ya, c) != E_BUFFER_OK)
  return;
 if (e_ins_run_stays(xa, c))
 {
  e_ins_run_here(b, sch, s, xa, ya, n);
  return;
 }
 if (e_ins_nchar(b, sch, s, xa - c, ya + 1, n) != E_CHAR_REFUSED)
  e_char_record_place(rec, b->b.x - n, b->b.y);
}

/**
 * e_ins_nchar - Insert the @n bytes of @s at (@xa, @ya), wrapping the row at
 * Max. Columns as the manual says.
 * @b, @sch: the buffer and its screen.
 *
 * Typing, the indent of Enter and Tab, a replacement and Undo of a deletion
 * all come here: every byte goes in, every row stays within Max. Columns
 * and no UTF-8 character is cut.  A run longer than a row goes in over as
 * many rows as it needs, as one line record (e_ins_long_run).
 * Return: the column behind the run, or E_CHAR_REFUSED (nothing changed).
 */
int e_ins_nchar(BUFFER *b, SCHIRM *sch, unsigned char *s, int xa, int ya,
  int n)
{
 Undo *rec;

 if (e_buffer_change_refused(b))
  return(E_CHAR_REFUSED);
 if (e_char_row_missing(b, ya, n, "the text put back"))
  return(E_CHAR_REFUSED);
 if (e_ins_run_refused(b, ya, n))
  return(E_CHAR_REFUSED);
 if (e_ins_run_is_long(b, n))
  return(e_ins_long_run(b, sch, s, xa, ya, n));
 e_char_count_change(b, n);
 rec = e_char_record('a', b, xa, ya, n);
 e_buffer_cover_begin(b, rec);
 if (e_ins_row_overflows(b, ya, n))
  e_ins_wrapped_run(b, sch, s, xa, ya, n, rec);
 else
  e_ins_run_here(b, sch, s, xa, ya, n);
 e_buffer_cover_end(b);
 sc_txt_4(ya, b, 0);
 return(b->b.x);
}

int e_codepoint_to_utf8(int cp, unsigned char *out)
{
 if (cp < 0x80)
  { out[0] = cp; return 1; }
 if (cp < 0x800)
  { out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); return 2; }
 if (cp < 0x10000)
  { out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F);
    out[2] = 0x80 | (cp & 0x3F); return 3; }
 out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F);
 out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F);
 return 4;
}

int e_utf8_visual_len(unsigned char *s, int bytes)
{
 int pos = 0, vcols = 0;

 while (pos < bytes && s[pos])
 {
  vcols++;
  pos += e_utf8_seq_len(s + pos, bytes - pos);
 }
 return vcols;
}

/**
 * e_char_bytes_at - How many bytes the character at (@x, @y) takes: one for
 * ASCII or a byte that starts no UTF-8 character, never past the row's end.
 */
static int e_char_bytes_at(BUFFER *b, int x, int y)
{
 return(e_utf8_seq_len((unsigned char *)b->bf[y].s + x, b->bf[y].len - x));
}

/**
 * e_char_start_before - Where the character before (@x, @y) starts, @x 1
 * or more: Left and Backspace take what Right and Delete take, the glyph
 * the user sees, never a byte more or less.
 */
static int e_char_start_before(BUFFER *b, int x, int y)
{
 return(e_utf8_char_before((unsigned char *)b->bf[y].s, x, b->bf[y].len));
}

/**
 * e_put_char_over_wide - Overwrite a character of several bytes (an `é`,
 * a `€`) with the one byte typed.
 * @b, @s: the buffer and its screen; @cc: the byte; @k: the bytes replaced.
 *
 * The whole character goes, so the row stays valid UTF-8, and the record
 * keeps every byte it replaced: Ctrl-U gives the character back whole and
 * Ctrl-R types over it again.  The cursor is left on the byte typed, as for
 * any overwrite, for the caller to step past.
 */
static void e_put_char_over_wide(BUFFER *b, SCHIRM *s, unsigned char cc, int k)
{
 e_replace_occurrence(b, s, b->b.x, b->b.y, k, &cc, 1);
 b->b.x--;
}

/*     Overwriting of a character       */
int e_put_char(int c, BUFFER *b, SCHIRM *s)
{
 unsigned char cc = c;
 int k;

 if (e_buffer_change_refused(b))
  return(E_CHAR_REFUSED);
 if (b->b.x == b->bf[b->b.y].len)
 {
  /* the insert leaves the cursor behind the character, on the row a
     wrap took it to: it is not stepped again */
  e_ins_nchar(b, s, &cc, b->b.x, b->b.y, 1);
  return(b->b.x);
 }
 k = e_char_bytes_at(b, b->b.x, b->b.y);
 if (k > 1)
  e_put_char_over_wide(b, s, cc, k);
 else
 {
  e_add_undo('p', b, b->b.x, b->b.y, 1);
  e_char_count_change(b, 1);
  *(b->bf[b->b.y].s+b->b.x) = c;
 }
 (b->b.x)++;
 return(b->b.x);
}

/*   search right (left end of word) */
int e_su_lblk(int xa, char *s)
{
 int len = strlen(s);

 if (len == 0)
  return(0);
 if (xa >= len)
  xa = len - 1;
 for (; xa < len  && isalnum1(s[xa]); xa++)
  ;
 for (; xa < len && !isalnum1(s[xa]); xa++)
  ;
 return(xa);
}

/*     Search left (left end of word)     */
int e_su_rblk(int xa, char *s)
{
 int len = strlen(s);

 if (xa <= 0)
  return(xa);
 if (xa > len)
  xa = len;
 for (xa--; xa > 0 && !isalnum1(s[xa]); xa--)
  ;
 if (!xa)
  return(xa);
 for (; xa > 0 && isalnum1(s[xa]); xa--)
  ;
 return(!xa ? xa : xa + 1);
}

/* Prints out the line number and column of cursor */
void e_zlsplt(FENSTER *f)
{
 char str[20];

 if (!DTMD_ISTEXT(f->dtmd))
  return;
 sprintf(str,"%5d:%-4d", f->b->b.y + 1, f->b->cl + 1);
 e_puts(str,f->a.x+5,f->e.y,f->fb->er.fb);
 if (f->save)
  e_pr_char(f->a.x+3, f->e.y, '*', f->fb->er.fb);
 else
  e_pr_char(f->a.x+3, f->e.y, ' ', f->fb->er.fb);
#ifdef NEWSTYLE
 if (WpeIsXwin())
 {
  e_make_xrect(f->a.x + 1, f->e.y, f->e.x - 1, f->e.y, 0);
  e_make_xrect(f->a.x + 5, f->e.y, f->a.x + 14, f->e.y, 0);
 }
#endif
}

void WpeFilenameToPathFile(char *filename, char **path, char **file)
{
 char *tmp;
 char *cur_dir;
 int len;

 *path = NULL;
 tmp = strrchr(filename, DIRC);
 if (tmp)
 {
  *file = WpeStrdup(tmp + 1);
 }
 else
 {
  *file = WpeStrdup(filename);
 }
 if ((!tmp) || ((filename + 1 == tmp) && (*filename == '.')))
 {
  *path = WpeGetCurrentDir(WpeEditor);
 }
 else
 {
  if (*filename != DIRC)
  {
   if ((cur_dir = WpeGetCurrentDir(WpeEditor)) == NULL)
   {
    WpeFree(*file);
    *file = NULL;
    return ;
   }
   /* Delete the slash at the end. */
   len = strlen(cur_dir);
   cur_dir[len - 1] = 0;
   len = tmp - filename + 1;
   while (strncmp(filename, "../", 3) == 0)
   {
    tmp = strrchr(cur_dir, DIRC);
    if (tmp == cur_dir)
    {
     cur_dir[1] = 0;
    }
    else
    {
     *tmp = 0;
    }
    len -= 3;
    filename += 3;
   }
   *path = WpeMalloc((strlen(cur_dir) + len + 2) * sizeof(char));
   strcpy(*path, cur_dir);
   strcat(*path, DIRS);
   strncat(*path, filename, len);
   if ((*path)[strlen(cur_dir) + len] != DIRC)
   {
    (*path)[strlen(cur_dir) + len + 1] = 0;
   }
   else
   {
    (*path)[strlen(cur_dir) + len] = DIRC;
    (*path)[strlen(cur_dir) + len + 1] = 0;
   }
   WpeFree(cur_dir);
  }
  else
  {
   len = tmp - filename + 1;
   *path = WpeMalloc(len + 1 * sizeof(char));
   strncpy(*path, filename, len);
   (*path)[len] = 0;
  }
 }
}

/*   draw mouse slider cursor */
int e_lst_zeichen(int x, int y, int n, int sw, int frb, int max, int iold,
  int new)
{
 int inew;
 double d = max ? 1./(float)max : 0;

 if (n < 3)
  return(1);

 if ((inew = (int) (new*(n-2)*d + 1.5)) > n-2)
  inew = n-2;
 if (iold < 1)
  iold = 1;
 if (inew < 1)
  inew = 1;

 if (sw == 0)
 {
  if (iold < n-1)
   e_pr_char(x, y+iold, MCI, frb);
  e_pr_char(x, y+inew, MCA, frb);
  e_make_xrect(x, y+1, x, y+n-2, 0);
  e_make_xrect(x, y+inew, x, y+inew, 0);
 }
 else
 {
  if (iold < n-1) e_pr_char(x+iold, y, MCI, frb);
   e_pr_char(x+inew, y, MCA, frb);
  e_make_xrect(x+1, y, x+n-2, y, 0);
  e_make_xrect(x+inew, y, x+inew, y, 0);
 }
 return(inew);
}

/*   draw mouse slider bar */
void e_mouse_bar(int x, int y, int n, int sw, int frb)
{
 int sv = n;

 if (sw == 0)
 {
  e_pr_char(x, y, MCU, frb);
  e_pr_char(x, y+n-1, MCD, frb);
  for (; n > 2; n--)
   e_pr_char(x, y+n-2, MCI, frb);
  e_make_xrect(x, y+1, x, y+sv-2, 0);
  e_make_xrect(x, y, x, y, 0);
  e_make_xrect(x, y+sv-1, x, y+sv-1, 0);
 }
 else
 {
  e_pr_char(x, y, MCL, frb);
  e_pr_char(x+n-1, y, MCR, frb);
  for (; n > 2; n--)
   e_pr_char(x+n-2, y, MCI, frb);
  e_make_xrect(x+1, y, x+sv-2, y, 0);
  e_make_xrect(x, y, x, y, 0);
  e_make_xrect(x+sv-1, y, x+sv-1, y, 0);
 }
}

int e_autosave(FENSTER *f)
{
 char *tmp, *str;
 long maxname;

 f->save = 1;
 if (!(f->ed->autosv & 2)) return(0);
 /* Check if file system could have an autosave or emergency save file
    >12 check is to eliminate dos file systems */
 if (((maxname = pathconf(f->dirct, _PC_NAME_MAX)) >= strlen(f->datnam) + 4) &&
   (maxname > 12))
 {
  str = MALLOC(strlen(f->datnam) + 5);
  str = e_make_postf(str, f->datnam, ".ASV");
  tmp = f->datnam;
  f->datnam = str;
  /*e_save(f);*/
  WpeMouseChangeShape(WpeWorkingShape);
  e_write(0, 0, E_BUFFER_ROW_END(f->b), f->b->mxlines-1, f, WPE_NOBACKUP);
  WpeMouseRestoreShape();
  wpe_test_dump_note_autosave();
  f->datnam = tmp;
  f->save = 1;
  FREE(str);
 }
 return(0);
}

/**
 * e_undo_record_compose - Fill a character record of kind @sw.
 * Return: 0, or -1 when memory ran out (the record is then incomplete).
 */
static int e_undo_record_compose(Undo *next, int sw, BUFFER *b, int x, int y, int n)
{
 next->b.x = x;
 next->b.y = y;
 next->a.x = n;
 if (sw == 'p')
  next->u.c = b->bf[y].s[x];
 else if (sw == 'r' || sw == 's')
 {
  if (e_buffer_record_text(next, b->bf[y].s + x, n) != E_BUFFER_OK)
   return(-1);
  /* For type 's' (search/replace undo), a.y must store the length
     of the OTHER string -- the one that will be re-inserted when this
     undo entry is later undone/redone.  Kruse's original code read
     this from b->f->fd.rn (the global replace-string length), but
     that value can change between operations, causing Redo to use
     a wrong length and crash with SIGSEGV.
     Fix: during undo/redo, the replay sets b->f->fd.rn to the correct
     length before it records its inverse. */
  next->a.y = b->f->fd.rn;
 }
 return(0);
}

static Undo *e_char_record(int sw, BUFFER *b, int x, int y, int n)
{
 Undo *next;

 if (e_buffer_covered(b))
  return(NULL);
 if ((next = e_buffer_record_new(sw)) == NULL)
 {
  wpe_buffer_say_nomem(b, "the undo record");
  return(NULL);
 }
 if (e_undo_record_compose(next, sw, b, x, y, n) != 0)
 {
  e_buffer_record_free(next);
  wpe_buffer_say_nomem(b, "the undo record");
  return(NULL);
 }
 return(e_buffer_record_push(b, next));
}

/**
 * e_add_undo - Record a character change of kind @sw for Ctrl-U.
 * @b: the buffer; @x, @y, @n: where and how much.
 *
 * 'a' 'r' 's' 'p' are the character kinds; the block kinds 'c' 'v' 'd' are
 * composed by the block code (we_block.c).  A whole-buffer swap ('B') needs
 * no record of its own: the swap records exactly the lines it changes.
 * Nothing is recorded under a cover.
 * Return: 0, or -1 when memory ran out.
 */
int e_add_undo(int sw, BUFFER *b, int x, int y, int n)
{
 if (sw == 'B' || e_buffer_covered(b))
  return(0);
 if (e_char_record(sw, b, x, y, n) == NULL && b->history == E_BUFFER_HISTORY_FULL)
  return(-1);
 return(0);
}

/**
 * e_replace_long - Replace @sn characters at (@x, @y) by @rn bytes, more
 * than a row holds, as one Ctrl-U.
 * @b, @s: the buffer and its screen; @text: the replacement.
 *
 * The deletion keeps its own record and the long run its line record, both
 * in one step: one Ctrl-U takes the replacement out and gives the old text
 * back, one Ctrl-R puts it in again, every byte.  Nothing is deleted that
 * the insert could then refuse: a long run always goes in.
 * Return: 1 (the window was already asked).
 */
static int e_replace_long(BUFFER *b, SCHIRM *s, int x, int y, int sn,
                          unsigned char *text, int rn)
{
 e_buffer_step_begin(b);
 e_del_nchar(b, s, x, y, sn);
 e_ins_nchar(b, s, text, x, y, rn);
 e_buffer_step_end(b);
 return(1);
}

/**
 * e_replace_occurrence - Replace @sn characters at (@x, @y) by the @rn
 * bytes of @text, as one Ctrl-U: what Search > Replace does with each
 * occurrence it changes.
 * @b, @s: the buffer and its screen.
 *
 * The record is kept first, then the two halves of the swap run covered
 * with it as their pocket.  A replacement that makes its line wrap moves
 * the breakpoints below it down with their code, as typing does, and Undo
 * moves them back; the record follows the replacement to the row it
 * wrapped onto, so Ctrl-U puts the old text back where the new one is.
 * A replacement longer than a row takes several rows: e_replace_long.
 * Return: 1 when the occurrence was replaced, 0 when the window refused it.
 */
int e_replace_occurrence(BUFFER *b, SCHIRM *s, int x, int y, int sn,
                         unsigned char *text, int rn)
{
 Undo *rec;

 if (e_buffer_change_refused(b))
  return(0);
 if (e_ins_run_is_long(b, rn))
  return(e_replace_long(b, s, x, y, sn, text, rn));
 b->f->fd.rn = rn;     /* the record keeps the replacement's length */
 rec = e_char_record('s', b, x, y, sn);
 e_buffer_cover_begin(b, rec);
 e_del_nchar(b, s, x, y, sn);
 e_ins_nchar(b, s, text, x, y, rn);
 e_char_record_place(rec, b->b.x - rn, b->b.y);
 e_buffer_cover_end(b);
 return(1);
}

/* What the screen's owner calls the paint of a window's new text. */
#define E_REPLACED_TEXT_WHAT "a window's new text"

/**
 * e_window_paint_replaced_text - Draw a window whose lines were just replaced.
 * @f: the window.
 *
 * The window is opened over its new lines and the desk is drawn again, so
 * the user sees the new text, with the cursor where they were working,
 * instead of the old rows until the next key.
 */
static void e_window_paint_replaced_text(FENSTER *f)
{
 e_firstl(f, 1);                    /* re-open the view over the new lines */
 e_schirm(f, 1);
 e_rep_win_tree(f->ed);             /* full repaint */
 e_refresh();                       /* flush to the terminal now, not next key */
}

/* The screen's owner's turn for a window's new text that waited behind a
   box.  The window may have closed meanwhile: it is looked up, never
   followed. */
static void e_window_replaced_text_replay(void *data)
{
 int i;

 for (i = 1; i <= WpeEditor->mxedt; i++)
  if (WpeEditor->f[i] == (FENSTER *)data)
  {
   e_window_paint_replaced_text((FENSTER *)data);
   return;
  }
}

/**
 * e_window_show_replaced_text - Show the new text of a window a tool rewrote.
 * @f: the window whose lines an accepted Edit, a language server's edit or a
 * reload just replaced.
 *
 * What the user gains: the rewritten file is on the screen at once when
 * nothing is in the way, and under a box or a menu it is drawn the moment
 * that closes, on the window the user then sees.
 */
void e_window_show_replaced_text(FENSTER *f)
{
 wpe_paint_request(e_window_replaced_text_replay, f, E_REPLACED_TEXT_WHAT);
}

int e_make_undo(FENSTER *f)
{
 return(e_make_rudo(f, 0));
}

int e_make_redo(FENSTER *f)
{
 return(e_make_rudo(f, 1));
}

#ifdef WPE_AI
/**
 * e_undo_waits_for_the_assistant - Must Undo or Redo leave this window alone?
 * @f: the window Undo would change.
 *
 * An Edit or a Complete splices its reply over the lines it sent, so while
 * one waits the window's text must stay as sent; Ctrl-U already asks, and
 * Edit > Undo and Redo from the menu ask here, so the status row says why
 * and Esc frees the window.
 * Return: 1 when the window is locked for a pending reply.
 */
static int e_undo_waits_for_the_assistant(FENSTER *f)
{
 if (!wpe_ai_window_locked(f))
  return(0);
 wpe_ai_report_window_locked(f);
 return(1);
}
#endif

/**
 * e_record_text_ends_line - Does the text an 'r' or 's' record keeps end
 * with a line break?  Putting it back then breaks the line, and the cursor
 * is no longer behind the text.
 */
static int e_record_text_ends_line(const Undo *rec)
{
 const unsigned char *text = (const unsigned char *) rec->u.pt;

 return(rec->a.x > 0 && text[rec->a.x - 1] == '\n');
}

/**
 * e_rudo_text_back - Put back the characters a record removed, a line
 * break as a line break (never auto-indented again), and mark them.
 * @b, @s: the buffer and its screen; @rec: an 'r' or 's' record.
 *
 * The block is the text as it now stands: where the characters went in,
 * on the row a wrap took them to, up to the cursor behind them, which is
 * the start of the next row when a line break came back.  So Ctrl-K C or
 * Ctrl-K V right after the Undo copies exactly that text.
 */
static void e_rudo_text_back(BUFFER *b, SCHIRM *s, Undo *rec)
{
 unsigned char *text = (unsigned char *) rec->u.pt;
 int n = rec->a.x, chars = n;
 POINT begin = rec->b;

 if (n <= 0)
  return;                 /* a record of no characters puts nothing back */
 if (e_record_text_ends_line(rec))
  chars = n - 1;
 if (chars > 0 && e_ins_nchar(b, s, text, rec->b.x, rec->b.y, chars) != E_CHAR_REFUSED)
 {
  begin.y = b->b.y;
  begin.x = b->b.x - chars;
 }
 if (chars < n)
  e_car_ret_split(b, s);
 s->mark_begin = begin;
 s->mark_end = b->b;
}

/**
 * e_rudo_replace_back - Undo or redo one replacement of Search > Replace.
 *
 * The inverse is recorded first, then the swap of the two strings runs
 * covered, so one Ctrl-R gives the replacement back; when the text put back
 * makes its line wrap, the inverse follows it to the row it went to.
 */
static void e_rudo_replace_back(BUFFER *b, SCHIRM *s, Undo *rec)
{
 Undo *inv;

 /* Set b->f->fd.rn to the length of the string being re-inserted
    (rec->a.x).  e_add_undo reads b->f->fd.rn to store as a.y in
    the reverse entry, so the next undo/redo knows the correct
    length of the OTHER string.  Kruse's original code assumed
    b->f->fd.rn was always current, but it was never synced from
    f->ed->fd.rn (the global search/replace state), causing a.y
    to contain uninitialized garbage (e.g. 189 instead of 5). */
 b->f->fd.rn = rec->a.x;
 inv = e_char_record('s', b, rec->b.x, rec->b.y, rec->a.y);
 e_buffer_cover_begin(b, NULL);
 e_del_nchar(b, s, rec->b.x, rec->b.y, rec->a.y);
 e_rudo_text_back(b, s, rec);
 if (!e_record_text_ends_line(rec))
  e_char_record_place(inv, b->b.x - rec->a.x, b->b.y);
 e_buffer_cover_end(b);
}

/**
 * e_rudo_block_refusal - What Undo makes of a block delete whose lines
 * could not come back: the record keeps them for the next try either way.
 * @r: what the move returned; @rec: the record.
 * Return: E_BUFFER_NOMEM for want of memory (Undo says so), or
 * E_BUFFER_RANGE when the block could not land there (written to the
 * trace; nothing to say about memory).
 */
static int e_rudo_block_refusal(int r, Undo *rec)
{
 if (r == E_BLOCK_NOMEM)
  return(E_BUFFER_NOMEM);
 wpe_buffer_trace("buffer replay %c not moved", rec->type);
 return(E_BUFFER_RANGE);
}

/**
 * e_rudo_block_back - Undo a block delete: the lines come back from the
 * record's shell, and the block is marked as it was.
 *
 * When memory runs out the lines stay in the shell and the record stays on
 * its list, so the next Ctrl-U can still bring them back.
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM with the text and the marks as
 * they were.
 */
static int e_rudo_block_back(BUFFER *b, SCHIRM *s, Undo *rec)
{
 BUFFER *bn = (BUFFER *) rec->u.pt;
 SCHIRM *sn = bn->f->s;
 POINT mark_begin = s->mark_begin;
 POINT mark_end = s->mark_end;
 Undo *way_back = NULL;
 int r;

 s->mark_begin = sn->mark_begin;
 s->mark_end = sn->mark_end;
 r = e_move_block_covered(rec->b.x, rec->b.y, bn, b, b->f, 'c', &way_back);
 if (r != E_BLOCK_MOVED)
 {
  s->mark_begin = mark_begin;
  s->mark_end = mark_end;
  return(e_rudo_block_refusal(r, rec));
 }
 e_buffer_shell_free(bn);
 rec->u.pt = NULL;
 e_block_record_keep(way_back, b, rec->b.x, rec->b.y);
 return(E_BUFFER_OK);
}

/**
 * e_rudo_view_keep - Remember the cursor and the block before a block
 * replay sets the record's.
 */
static WpeReplayView e_rudo_view_keep(BUFFER *b, SCHIRM *s)
{
 WpeReplayView v;

 v.cursor = b->b;
 v.mark_begin = s->mark_begin;
 v.mark_end = s->mark_end;
 return(v);
}

/**
 * e_rudo_view_back - A refused block replay changed nothing: the cursor
 * and the block are put back as the user had them.
 */
static void e_rudo_view_back(BUFFER *b, SCHIRM *s, const WpeReplayView *v)
{
 b->b = v->cursor;
 s->mark_begin = v->mark_begin;
 s->mark_end = v->mark_end;
}

/**
 * e_rudo_block_replayed - What a copy or move replay tells Undo, from what
 * the block operation returned.
 * @r: E_BLOCK_*; @rec: the record; @v: the view before the replay.
 *
 * Refused for want of memory, the record stays for the next try and the
 * view is as it was.  A block that did not move for its own reasons is
 * written to the trace and the record is done, as it always was.
 */
static int e_rudo_block_replayed(int r, BUFFER *b, SCHIRM *s, Undo *rec,
                                 const WpeReplayView *v)
{
 if (r == E_BLOCK_NOMEM)
 {
  e_rudo_view_back(b, s, v);
  return(E_BUFFER_NOMEM);
 }
 if (r != E_BLOCK_MOVED)
  wpe_buffer_trace("buffer replay %c not moved", rec->type);
 return(E_BUFFER_OK);
}

/**
 * e_rudo_block_copy_back - Undo a block copy: the copied block, marked as
 * the record saw it, is deleted again (which records the way back).
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM with the text, the block and the
 * record as they were.
 */
static int e_rudo_block_copy_back(BUFFER *b, SCHIRM *s, Undo *rec)
{
 WpeReplayView v = e_rudo_view_keep(b, s);

 b->b = rec->a;
 s->mark_begin = rec->a;
 s->mark_end = rec->e;
 return(e_rudo_block_replayed(e_block_delete_marked(b->f), b, s, rec, &v));
}

/**
 * e_rudo_block_move_again - Undo or redo a block move: the block, marked as
 * the record saw it, moves back to where the record's cursor is.
 * Return: as e_rudo_block_copy_back.
 */
static int e_rudo_block_move_again(BUFFER *b, SCHIRM *s, Undo *rec)
{
 WpeReplayView v = e_rudo_view_keep(b, s);

 b->b = rec->b;
 s->mark_begin = rec->a;
 s->mark_end = rec->e;
 return(e_rudo_block_replayed(e_block_move_marked(b->f), b, s, rec, &v));
}

/**
 * e_overwrite_back - Undo or Redo one character typed over another.
 * @b: the buffer; @rec: the 'p' record, which keeps the character to write.
 *
 * The character that stands there now is kept first, on the other list, so
 * Ctrl-U of overwrite typing is followed by a Ctrl-R that types it again.
 */
static void e_overwrite_back(BUFFER *b, const Undo *rec)
{
 e_add_undo('p', b, rec->b.x, rec->b.y, 1);
 b->bf[rec->b.y].s[rec->b.x] = rec->u.c;
}

/**
 * wpe_buffer_replay_kind - Replay a block record for Undo or Redo; its
 * inverse goes to the other list through the block operation's own record.
 * @b: the buffer; @s: its screen; @rec: the record ('c' 'v' 'd').
 *
 * The block kinds run uncovered: their operations take their own covers and
 * push their record after them, so Redo finds it.  The character kinds are
 * replayed on a shell instead (wpe_buffer_replay_on_shell), so the step
 * they belong to is made whole or not at all.
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM when a block operation was refused
 * for want of memory (the text is as it was and the record stays for the
 * next try); E_BUFFER_RANGE when a deleted block could not land (its record
 * keeps the lines).
 */
int wpe_buffer_replay_kind(BUFFER *b, SCHIRM *s, Undo *rec)
{
 if (rec->type == 'c')
  return(e_rudo_block_copy_back(b, s, rec));
 else if (rec->type == 'v')
  return(e_rudo_block_move_again(b, s, rec));
 else if (rec->type == 'd')
  return(e_rudo_block_back(b, s, rec));
 return(E_BUFFER_OK);
}

/**
 * wpe_buffer_replay_on_shell - Replay character record @rec on a shell
 * whose row 0 is row @y0 of the buffer the record belongs to.
 * @shell: the shell, filled with copies of the rows the replay may touch;
 * @s: its screen; @rec: an 'a' 'r' 's' or 'p' record, read only.
 *
 * The same primitive that made the change replays it, so an Undo joins
 * and wraps exactly as typing did; on the shell, a refusal for want of
 * memory counts there (wpe_buffer_say_nomem) and the user's text is never
 * touched.  The inverses the primitives record stay on the shell's list
 * until the owner moves them onto the buffer's.  An overwrite ('p') puts
 * the other character back and records the one it replaces, so Redo of an
 * undone overwrite types it over again; that record is memory too, so the
 * Undo of overwrite typing is refused whole when it cannot be had, never
 * made without its Redo.
 */
void wpe_buffer_replay_on_shell(BUFFER *shell, SCHIRM *s, const Undo *rec, int y0)
{
 Undo at = *rec;

 at.b.y -= y0;
 at.next = NULL;
 if (at.type == 'r')
  e_rudo_text_back(shell, s, &at);
 else if (at.type == 's')
  e_rudo_replace_back(shell, s, &at);
 else if (at.type == 'a')
  e_del_nchar(shell, s, at.b.x, at.b.y, at.a.x);
 else if (at.type == 'p')
  e_overwrite_back(shell, &at);
}

int e_make_rudo(FENSTER *f, int sw)
{
 BUFFER *b;
 SCHIRM *s;
 int i, r;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--);
 if (i <= 0) return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
#ifdef WPE_AI
 if (e_undo_waits_for_the_assistant(f))
  return(0);
#endif
 b = f->b;  s = f->s;
 if (!sw)
  r = e_buffer_undo(b, s);
 else
  r = e_buffer_redo(b, s);
 if (r == E_BUFFER_NOTHING)
 {
  e_error((!sw ? e_msg[ERR_UNDO] : e_msg[ERR_REDO]), 0, b->fb);
  return(-1);
 }
 e_schirm(f, 1);
 e_cursor(f, 1);
#ifdef DEBUGGER
 {
  /* The buffer just changed wholesale (undo or redo).  The per-keystroke LSP
     hook debounces on Enter, which an undo never sends -- so push the restored
     text to the server now, or its diagnostics/inlays would describe the
     pre-undo text (e.g. the errors vanish after undoing a quick-fix). */
  extern void e_lsp_after_bulk_edit(FENSTER *f);
  e_lsp_after_bulk_edit(f);
 }
#endif
 return(0);
}

char *e_make_postf(char *out, char *name, char *pf)
{
 strcpy(out, name);
 strcat(out, pf);
 return(out);
}

/* ---- the editor's side of the buffer owner (we_buffer.h) ---- */

/**
 * e_syntax_state_after - The syntax state line @i of a window leaves the
 * next line in.
 */
static int e_syntax_state_after(BUFFER *b, int i)
{
 int *c_sw = b->f->c_sw;
 struct wpeSyntaxRule *st = b->f->c_st;

 if (st->continue_column >= 0 && isspace(b->bf[i+1].s[st->continue_column]))
  return(0);
 return(e_scfbol(b->bf[i].len, c_sw[i], b->bf[i].s, st));
}

/**
 * wpe_buffer_syntax_changed - Recompute the colours of a window from line @y.
 * @b: the buffer whose lines changed.
 *
 * Every line the change touched is recomputed, then on to the first line
 * whose starting state did not change: after a tool rewrote the file, its
 * colours are the ones the same text has when opened fresh.
 */
void wpe_buffer_syntax_changed(BUFFER *b, int y)
{
 int i, out, until;

 if (b->f == NULL || b->f->c_sw == NULL || b->f->c_st == NULL || y >= b->mxlines)
  return;
 if (y < 0)
  y = 0;
 until = b->syn_hi;
 if (b->f->c_st->continue_column >= 0 && y > 0)
  b->f->c_sw[y] = e_syntax_state_after(b, y - 1);
 for (i = y; i < b->mxlines - 1; i++)
 {
  out = e_syntax_state_after(b, i);
  if (out == b->f->c_sw[i+1] && i + 1 > until)
   break;
  b->f->c_sw[i+1] = out;
 }
}

/**
 * wpe_buffer_say_read_only - Say on the status row that a command was not
 * made because its window is closed to changes (a file opened read-only,
 * the clipboard viewer); the text is as it was.
 * @b: the buffer the command would have changed.
 *
 * The line stays until the next key, so the redraw that follows the
 * command does not take it away before the user has read it.
 */
void wpe_buffer_say_read_only(BUFFER *b)
{
 (void) b;
 if (WpeEditor != NULL && WpeEditor->mxedt >= 0)
  e_status_say_until_key(WpeEditor->f[WpeEditor->mxedt], E_SAY_READ_ONLY);
}

/**
 * wpe_buffer_say_nomem - Say on the status row that a change was refused
 * for want of memory; the text is as it was.
 * @what: the change, in the user's words.
 */
void wpe_buffer_say_nomem(BUFFER *b, const char *what)
{
 char line[E_BUFFER_SAY_MAX];

 if (e_buffer_is_shell(b))
 {
  e_buffer_shell_refused(b);
  wpe_buffer_trace("buffer shell nomem %s", what);
  return;
 }
 snprintf(line, sizeof line, E_BUFFER_SAY_FMT, what);
 wpe_buffer_trace("buffer nomem %s", what);
 if (WpeEditor != NULL && WpeEditor->mxedt >= 0)
  e_status_say(WpeEditor->f[WpeEditor->mxedt], line);
}

/**
 * wpe_buffer_say_shape - Say on the status row that a change was refused
 * because the rows it would make do not fit a line (wider than Max.
 * Columns, or past the text); the text is as it was.
 * @what: the change, in the user's words.
 *
 * Without it the key would seem to do nothing, and the user would not know
 * whether to try again or to look for another way.
 */
void wpe_buffer_say_shape(BUFFER *b, const char *what)
{
 char line[E_BUFFER_SAY_MAX];

 if (e_buffer_is_shell(b))
 {
  e_buffer_shell_refused(b);
  wpe_buffer_trace("buffer shell shape %s", what);
  return;
 }
 snprintf(line, sizeof line, E_BUFFER_SAY_SHAPE_FMT, what);
 wpe_buffer_trace("buffer shape %s", what);
 if (WpeEditor != NULL && WpeEditor->mxedt >= 0)
  e_status_say(WpeEditor->f[WpeEditor->mxedt], line);
}

/**
 * e_say_nomem_until_key - As wpe_buffer_say_nomem, with the line kept on
 * the status row until the user presses a key.
 * @what: the action, in the user's words.
 *
 * For a refusal the editor goes on drawing over: a file named on the
 * command line that could not be opened is followed at once by the File
 * Manager, which would put its key hints back before the line was read.
 */
static void e_say_nomem_until_key(const char *what)
{
 wpe_buffer_trace("buffer nomem %s", what);
 e_status_keep_nomem(what);
}

/**
 * e_status_keep_nomem - Keep the line that says @what was not made for want
 * of memory on the status row until the user presses a key.
 *
 * For a command run from a menu: the redraw that follows it would take the
 * line away before the user read it.  The refusal itself was already said
 * and traced.
 */
void e_status_keep_nomem(const char *what)
{
 char line[E_BUFFER_SAY_MAX];

 snprintf(line, sizeof line, E_BUFFER_SAY_FMT, what);
 if (WpeEditor != NULL && WpeEditor->mxedt >= 0)
  e_status_say_until_key(WpeEditor->f[WpeEditor->mxedt], line);
}

/**
 * wpe_buffer_trace - Write one line about the buffer owner to the file
 * $XWPE_UI_TRACE names, so a test can count the records of one action.
 */
void wpe_buffer_trace(const char *fmt, ...)
{
 const char *path = getenv("XWPE_UI_TRACE");
 FILE *tf;
 va_list ap;

 if (!path || !(tf = fopen(path, "a")))
  return;
 va_start(ap, fmt);
 vfprintf(tf, fmt, ap);
 va_end(ap);
 fputc('\n', tf);
 fclose(tf);
}

/* The longest point name $XWPE_TEST_FAULT may carry, its end included. */
#define E_BUFFER_FAULT_NAME_MAX 32

/**
 * wpe_buffer_fault_from_env - Arm the test fault $XWPE_TEST_FAULT names.
 *
 * Read once at startup.  "block-move:4" refuses memory to the first block
 * move or delete from its fourth allocation on, "undo:1" to the first
 * Ctrl-U, and so on (we_buffer.h), so a test of the running editor can show
 * that a change refused for want of memory leaves the user's text, marks
 * and undo as they were.  Unset, or a value it cannot read, arms nothing.
 */
void wpe_buffer_fault_from_env(void)
{
 const char *spec = getenv("XWPE_TEST_FAULT");
 const char *colon;
 char name[E_BUFFER_FAULT_NAME_MAX];
 size_t len;

 if (spec == NULL || (colon = strchr(spec, ':')) == NULL)
  return;
 len = (size_t)(colon - spec);
 if (len >= sizeof name)
  return;
 memcpy(name, spec, len);
 name[len] = '\0';
 e_buffer_test_fault_arm(e_buffer_test_fault_point(name), atoi(colon + 1));
}

