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

#include<sys/types.h> /*  included for digital station  */
#include<sys/stat.h>
#include<stdarg.h>

/* Room for the status line that says a change was refused for memory. */
#define E_BUFFER_SAY_MAX 160

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
int e_help_next();
int e_codepoint_to_utf8(int cp, unsigned char *out);
int e_utf8_charlen(unsigned char c);
int e_utf8_to_codepoint(unsigned char *buf, int len);
int e_utf8_visual_len(unsigned char *s, int bytes);

/* True for a UTF-8 lead byte (start of a 2..4 byte sequence). */
#define UTF8_IS_LEAD(c) ((c) >= 0xC0 && (c) < 0xFE)

/* Walk one UTF-8 character starting at s[i] (line length `len`) while computing
   the screen column.  Decodes the character, adds its (display width - byte
   count) to *j -- a multi-byte sequence is one b->b.x step per byte but only
   wcwidth() columns wide -- and returns the number of EXTRA (continuation)
   bytes so the caller's loop can skip them.  0 for a 1-byte/invalid sequence.
   Folds three identical inline copies that computed cursor screen columns. */
static int e_utf8_visual_step(unsigned char *s, int i, int len, int *j)
{
 wchar_t wc = 0;
 mbstate_t mbs = {0};
 int nb = mbrtowc(&wc, (char *)(s + i), len - i, &mbs);

 if (nb > 1)
 {
  int cw = wcwidth(wc);
  if (cw < 1) cw = 1;
  *j += cw - nb;
  return nb - 1;
 }
 return 0;
}

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
 * ones.  Without memory the window opens empty and the status line says so.
 */
static void e_edit_read_file(FENSTER *f, FILE *fp)
{
 BUFFER *b = f->b;
 int history = b->history;
 POINT end;

 b->history = E_BUFFER_NO_HISTORY;
 if (e_read_file_into(fp, b, 0, 0, &f->dtmd, &end) != E_BUFFER_OK)
  wpe_buffer_say_nomem(b, "reading the file");
 b->history = history;
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
 for (i = 0; i < 9; i++)
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

 if (fp != NULL && ftype != 1)
 {
  e_edit_read_file(f, fp);
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
     e_del_line(b->b.y, b, s);
     b->b.y--;
     b->b.x = b->bf[b->b.y].len;
     if (*(b->bf[b->b.y].s+b->b.x) == '\0') b->b.x--;
    }
    else
    {
     if (b->b.x > 0)
     {
      /* Move back over UTF-8 continuation bytes */
      int old_x = b->b.x;
      b->b.x--;
      while (b->b.x > 0 &&
             ((unsigned char)b->bf[b->b.y].s[b->b.x] & 0xC0) == 0x80)
       b->b.x--;
      if (f->flg & 1) e_del_a_ind(b, s);
      else e_del_nchar(b, s, b->b.x, b->b.y, old_x - b->b.x);
     }
     else
     {
      b->b.y--;
      b->b.x = b->bf[b->b.y].len;
      if (*(b->bf[b->b.y].s+b->b.x) == '\0') b->b.x--;
      if (f->flg & 1) e_del_a_ind(b, s);
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
    /* Determine number of bytes in the character at cursor */
    unsigned char uc = (unsigned char)b->bf[b->b.y].s[b->b.x];
    int del_n = 1;
    if (uc >= 0xC0 && uc < 0xFE)
    {
     int pos = b->b.x + 1;
     while (pos < b->bf[b->b.y].len &&
            ((unsigned char)b->bf[b->b.y].s[pos] & 0xC0) == 0x80)
      pos++;
     del_n = pos - b->b.x;
    }
    e_del_nchar(b, s, b->b.x, b->b.y, del_n);
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
	 (b->b.x)--;
	 /* Skip UTF-8 continuation bytes (10xxxxxx) when moving left */
	 while (b->b.x > 0 &&
	        ((unsigned char)b->bf[b->b.y].s[b->b.x] & 0xC0) == 0x80)
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
	 {
	  /* Skip entire UTF-8 sequence when moving right */
	  unsigned char uc = (unsigned char)b->bf[b->b.y].s[b->b.x];
	  (b->b.x)++;
	  if (uc >= 0xC0 && uc < 0xFE)
	  {
	   while (b->b.x < b->bf[b->b.y].len &&
	          ((unsigned char)b->bf[b->b.y].s[b->b.x] & 0xC0) == 0x80)
	    (b->b.x)++;
	  }
	 }
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
	 if(b->b.x >= b->bf[b->b.y].len && b->b.y < b->mxlines)
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
	 if( b->b.x < b->bf[b->b.y].len )
	 {  c = e_su_lblk(b->b.x, b->bf[b->b.y].s);
	    e_del_nchar(b, s, b->b.x, b->b.y, c-b->b.x);
	 }
	 else if(*(b->bf[b->b.y].s+b->b.x) == WPE_WR)
	 e_del_nchar(b, s, b->b.x, b->b.y, 1);
	 else if(b->b.x >= b->bf[b->b.y].len && b->b.y < b->mxlines)
	 {  b->b.x = 0; (b->b.y)++;  }
	 e_schirm(f, 1);
	 break;
      case CtrlZ:
	 if(e_window_text_locked(f)) break;
	 e_del_nchar(b, s, b->b.x, b->b.y, b->bf[b->b.y].len-b->b.x);
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

 c = e_toupper(wpe_paint_modal_getch());
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
 unsigned char cc;

 c = e_toupper(wpe_paint_modal_getch());
 if (c < 32) c = c + 'A' - 1;
 switch(c)
 {
  case 'Y':     /*  delete end of line    */
   if (e_window_text_locked(f)) break;
   e_del_nchar(b, s, b->b.x, b->b.y, b->bf[b->b.y].len-b->b.x);
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
   if (s->mark_begin.y == s->mark_end.y && s->mark_begin.y >= 0 && s->mark_begin.x < s->mark_end.x)
   {
    cc = HED;
    e_ins_nchar(b, s, &cc, s->mark_end.x, s->mark_end.y, 1);
    cc = HBG;
    e_ins_nchar(b, s, &cc, s->mark_begin.x, s->mark_end.y, 1);
    e_schirm(f, 1);
   }
   break;
  case 'M':     /*   for help file: Mark-Line  */
   if (s->mark_begin.y == s->mark_end.y && s->mark_begin.y >= 0 && s->mark_begin.x < s->mark_end.x)
   {
    cc = HED;
    e_ins_nchar(b, s, &cc, s->mark_end.x, s->mark_end.y, 1);
    cc = HBB;
    e_ins_nchar(b, s, &cc, s->mark_begin.x, s->mark_end.y, 1);
    e_schirm(f, 1);
   }
   break;
  case 'H':     /*   for help file: create Header  */
   if (s->mark_begin.y == s->mark_end.y && s->mark_begin.y >= 0 && s->mark_begin.x < s->mark_end.x)
   {
    cc = HED;
    e_ins_nchar(b, s, &cc, s->mark_end.x, s->mark_end.y, 1);
    cc = HHD;
    e_ins_nchar(b, s, &cc, s->mark_begin.x, s->mark_end.y, 1);
    e_schirm(f, 1);
   }
   break;
  case 'E':     /*   for help file: create end mark  */
   if (e_buffer_insert_blank(b, b->b.y, E_BUFFER_STEP_NEW) != E_BUFFER_OK)
   {
    wpe_buffer_say_nomem(b, "a new line");
    break;
   }
   cc = HFE;
   e_ins_nchar(b, s, &cc, 0, b->b.y, 1);
   e_schirm(f, 1);
   break;
  case 'L':     /*   for help file: delete "help" special chars  */
   for (i = 0; i < b->bf[b->b.y].len; i++)
    if (b->bf[b->b.y].s[i] == HBG || b->bf[b->b.y].s[i] == HED ||
      b->bf[b->b.y].s[i] == HHD || b->bf[b->b.y].s[i] == HBB ||
      b->bf[b->b.y].s[i] == HFE || b->bf[b->b.y].s[i] == HFB ||
      b->bf[b->b.y].s[i] == HFE)
    {
     e_del_nchar(b, s, i, b->b.y, 1);
     i--;
    }
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

/* New version of auto-indent */
int e_tab_a_ind(BUFFER *b, SCHIRM *s)
{
 int a_indent = b->cn->autoindent;
 int line, x, k, char_to_ins;
 char *str;
 int do_auto_indent = 0;
 int first_nospace_k;

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
    e_del_nchar(b, s, b->b.x, line, b->bf[line].len - b->b.x);
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
   e_del_nchar(b, s, b->b.x, line, b->bf[line].len - b->b.x);
  }
 }

 if (!do_auto_indent)
 {
  /* insert TAB char */
  str = MALLOC(sizeof(char));
  str[0] = '\t';
  char_to_ins = 1;
 }
 else
 {
  /* auto-indent */ 

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
   /* insert chars */
   k = x - b->b.x;
   str = MALLOC(k * sizeof(char));
   for (x = 0; x < k; x++)
    str[x] = ' ';
   char_to_ins = k;
  }
  else if (b->b.x < (x + a_indent))
  {
   /* indent to x + a_indent with spaces */
   /* insert chars */
   k = x + a_indent - b->b.x;
   str = MALLOC(k * sizeof(char));
   for (x = 0; x < k; x++)
    str[x] = ' ';
   char_to_ins = k;
  }
  else
  {
   /* insert TAB char */
   str = MALLOC(sizeof(char));
   str[0] = '\t';
   char_to_ins = 1;
  }
 }

 e_ins_nchar(b, s, str, b->b.x, b->b.y, char_to_ins);
 FREE(str);
 return(b->b.x);
}

int e_del_a_ind(BUFFER *b, SCHIRM *s)
{
 int i = 1, j = -1, k;

 if (b->b.y > 0)
 {
  for (i = 0;
    i < b->bf[b->b.y].len && i < b->b.x && isspace(b->bf[b->b.y].s[i]); i++)
   ;
  if (i == b->b.x)
  {
   for (j = 0, i = 0; j <= b->b.x; j++)
   {
    i = b->bf[b->b.y].s[j] == '\t' ? (i / b->cn->tabn + 1) * b->cn->tabn :
      i + 1;
   }
   if (i != j)
   {
    char *str = MALLOC(i * sizeof(char));
    e_del_nchar(b, s, 0, b->b.y, j);
    for (j = 0; j < i; j++)
     str[j] = ' ';
    e_ins_nchar(b, s, str, 0, b->b.y, i);
    b->b.x = i - 1;
    FREE(str);
   }
   for (j = b->b.y-1; j >= 0; j--)
   {
    for (i = 0, k = 0; k < b->bf[j].len && isspace(b->bf[j].s[k]); k++)
     i = b->bf[j].s[k] == '\t' ? (i / b->cn->tabn + 1) * b->cn->tabn : i + 1;
    if (k < b->bf[j].len && b->bf[j].s[k] != '#' && i <= b->b.x)
    {
     i = b->b.x - i + 1;
     b->b.x -= i - 1;
     break;
    }
   }
   if (j < 0)
   {
    i = b->b.x;
    b->b.x = 0;
   }
  }
 }
 if (j < 0)
  i = 1;
 e_del_nchar(b, s, b->b.x, b->b.y, i);
 return(i);
}

int e_car_a_ind(BUFFER *b, SCHIRM *s)
{
 int i, j, k;
 unsigned char *str;

 if (b->b.y == 0)
  return(0);
 j = b->b.y;
 do
 {
  j--;
  for (i = 0, k = 0;
    k < b->bf[j].len && (isspace(b->bf[j].s[k]) || b->bf[j].s[i] == '{'); k++)
   i = b->bf[j].s[k] == '\t' ? (i / b->cn->tabn + 1) * b->cn->tabn : i + 1;
 } while (j > 0 && b->bf[j].s[k] == '#');
 if (k == b->bf[j].len && k > 0 && b->bf[j].s[k-1] == '{')
  i--;
 if (i > 0)
 {
  str = MALLOC(i * sizeof(char));
  for (j = 0; j < i; j++)
   str[j] = ' ';
  e_ins_nchar(b, s, str, 0, b->b.y, i);
  b->b.x = i;
  FREE(str);
 }
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
 * e_line_end_soft_line - Enter at the end of a soft-wrapped line: the line
 * gets its own line break and the continuation below becomes a line of its
 * own, with no new line inserted.
 */
static void e_line_end_soft_line(BUFFER *b, int y, int x)
{
 b->bf[y].s[x] = WPE_WR;
 b->bf[y].s[x+1] = '\0';
 b->bf[y].len = e_str_len(b->bf[y].s);
 b->bf[y].nrc = e_str_nrc(b->bf[y].s);
 sc_txt_4(y, b, 0);
}

/**
 * e_car_ret_split - Break the line at the cursor, without auto-indent.
 * @b: the buffer; @s: its screen.
 *
 * The line break the user typed, and what Undo of a joined line puts back:
 * the line keeps its breakpoint and the text behind the cursor moves down
 * with its marks.  Undo never indents again, so Redo of an undone Enter
 * gives the same text.  Return: E_BUFFER_OK, or E_BUFFER_NOMEM when the
 * line could not be split (nothing changed, the status row says so).
 */
int e_car_ret_split(BUFFER *b, SCHIRM *s)
{
 int x = b->b.x, y = b->b.y, len = b->bf[y].len, r;

 if (x == len && b->bf[y].s[len] == '\0')
  e_line_end_soft_line(b, y, x);
 else
 {
  r = e_buffer_split_line(b, y, x, E_BUFFER_STEP_COVERED, E_BUFFER_HEAD_WR);
  if (r != E_BUFFER_OK)
  {
   wpe_buffer_say_nomem(b, "a line break");
   return(r);
  }
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

/**
 * e_car_ret - Enter: break the line at the cursor, then auto-indent.
 * @b: the buffer; @s: its screen.
 *
 * The break and its indent are one action: one Ctrl-U takes both back.
 * Return: the cursor's new line.
 */
int e_car_ret(BUFFER *b, SCHIRM *s)
{
 e_buffer_step_begin(b);
 if (e_car_ret_split(b, s) == E_BUFFER_OK && (b->f->flg & 1))
  e_car_a_ind(b, s);
 e_buffer_step_end(b);
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

void e_cursor_pos_only(FENSTER *f)
{
 BUFFER *b = f->b;
 SCHIRM *s = f->s;
 int j, sx, sy;
 extern int cur_on;

 if (!DTMD_ISTEXT(f->dtmd)) return;
 if (b->b.y < 0) b->b.y = 0;
 if (b->b.y > b->mxlines - 1) b->b.y = b->mxlines - 1;
 if (b->b.x < 0) b->b.x = 0;
 if (b->mxlines == 0) b->b.x = 0;
 else if (b->b.x > b->bf[b->b.y].len) b->b.x = b->bf[b->b.y].len;

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
 if (b->b.y > b->mxlines-1) b->b.y = b->mxlines-1;
 if (b->b.y < 0) b->b.y = 0;
 if (b->b.x < 0) b->b.x = 0;
 if (b->mxlines==0) b->b.x=0; /* the else branch needs b->b.y < b->mxlines, which is not true for b->mxlines==0 */
 else if (b->b.x > b->bf[b->b.y].len) b->b.x = b->bf[b->b.y].len;
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
int e_del_line(int yd, BUFFER *b, SCHIRM *s)
{
 if (e_buffer_delete_lines(b, yd, 1, E_BUFFER_STEP_NEW) == E_BUFFER_NOMEM)
  wpe_buffer_say_nomem(b, "the line deletion");
 return(b->mxlines);
}

/**
 * e_new_line - Put an empty line at @yd of @b, and nothing else.
 *
 * The file reader and the views the editor writes add their lines this way
 * until they splice: no mark, breakpoint, colour or undo
 * moves.  None of them can go on without the line, so running out of memory
 * here ends the editor through the low-memory error, which offers to save
 * the user's files first, as it always did.  Return: the number of lines.
 */
int e_new_line(int yd, BUFFER *b)
{
 if (e_buffer_insert_raw(b, yd) != E_BUFFER_OK)
  e_error(e_msg[ERR_LOWMEM], 1, b->fb);
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
  if (b->mx.x+n-len > b->bf[y+1].len)
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

/**
 * e_ins_wrap_mark - A block mark on a line that just wrapped: behind the
 * break it moves to the new line, at or after the insertion it moves right.
 */
static void e_ins_wrap_mark(POINT *p, int ya, int xa, int n, int i)
{
 if (p->y != ya)
  return;
 if (p->x > i)
 {
  p->y++;
  p->x -= i + 1;
 }
 else if (p->x >= xa)
  p->x += n;
}

/**
 * e_ins_wrap_tail_down - Typing past the margin of a full line: its tail,
 * behind column @i, goes to a new line below it.
 *
 * The breakpoints and marks below move down with their lines, so a wrapped
 * paragraph never leaves a breakpoint on the wrong code.  Return: 1 when
 * the tail moved; 0 when no line could be added, and then the line keeps
 * its bytes up to its width and the status row says why the wrap waits.
 */
static int e_ins_wrap_tail_down(BUFFER *b, SCHIRM *sch, int ya, int xa, int n, int i)
{
 int j;

 if (e_buffer_insert_blank(b, ya+1, E_BUFFER_STEP_COVERED) != E_BUFFER_OK)
 {
  wpe_buffer_say_nomem(b, "the line wrap");
  return(0);
 }
 e_ins_wrap_mark(&sch->mark_begin, ya, xa, n, i);
 e_ins_wrap_mark(&sch->mark_end, ya, xa, n, i);
 /* Copy the wrapped tail into the fresh next line, bounded to mx.x-1 columns
    so the WPE_WR terminator and the following '\0' both land inside its
    mx.x+1 byte buffer.  The trailing '\0' is essential: e_str_nrc is strlen
    (NOT WPE_WR-aware, unlike e_str_len), so without it strlen runs past the
    WPE_WR into adjacent memory when the tail fills the line -- a heap
    overflow when editing a very long unbroken run. */
 for (j = i+1; *(b->bf[ya].s+j) != WPE_WR && *(b->bf[ya].s+j) != '\0'
      && (j-i-1) < b->mx.x - 1; j++)
  *(b->bf[ya+1].s+j-i-1) = *(b->bf[ya].s+j);
 *(b->bf[ya+1].s+j-i-1) = WPE_WR;
 *(b->bf[ya+1].s+j-i) = '\0';
 b->bf[ya+1].len = e_str_len(b->bf[ya+1].s);
 b->bf[ya+1].nrc = e_str_nrc(b->bf[ya+1].s);
 return(1);
}

/*   insert N chars in buffer */
int e_ins_nchar(BUFFER *b, SCHIRM *sch, unsigned char *s, int xa, int ya,
  int n)
{
 Undo *rec;
 int i, j, wrapped = 1;

 e_char_count_change(b, n);
 rec = e_char_record('a', b, xa, ya, n);
 e_buffer_cover_begin(b, rec);
 /* Defensive normalisation: each line buffer is e_new_line()-allocated at
    b->mx.x+1 bytes, so b->mx.x is the last valid index.  Some callers (the
    block move/copy paths in we_block.c, which reshuffle line buffers by
    raw struct aliasing) can leave bf[ya].len smaller than the real
    content and the buffer without a NUL within bounds.  e_ins_nchar then
    trusts the stale len, the capacity check below is fooled, and the
    e_str_len() calls run strlen past the buffer (ASan heap-buffer-overflow,
    Ctrl-K V).  Pin a terminator at the capacity limit and recompute the
    length so everything downstream operates on a valid, bounded line. */
 b->bf[ya].s[b->mx.x] = '\0';
 b->bf[ya].len = e_str_len(b->bf[ya].s);
 b->bf[ya].nrc = e_str_nrc(b->bf[ya].s);
 if (b->bf[ya].len+n >= b->mx.x-1)
 {
  if (xa < b->bf[ya].len)
  {
   i = b->mx.x-n-1;
   if (i >= b->bf[ya].len-1)
    i = b->bf[ya].len-1;
  }
  else
  {
   for (; xa < b->mx.x-1; xa++, s++, n--)
   {
    *(b->bf[ya].s + xa + 1) = *(b->bf[ya].s + xa);
    *(b->bf[ya].s + xa) = *s;
   }
   *(b->bf[ya].s + xa + 1) = '\0';
   i = b->mx.x;
   b->bf[ya].len = e_str_len(b->bf[ya].s);
   b->bf[ya].nrc = e_str_nrc(b->bf[ya].s);
  }
  for (; i > 0 && *(b->bf[ya].s+i) != ' ' && *(b->bf[ya].s+i) != '-'; i--);
  if (i <= 0)                          /* no word boundary within reach: break */
   i = b->bf[ya].len - 2;              /* near the end so only a short tail (a */
  if (i < 0)                           /* few columns) wraps -- never the whole */
   i = 0;                              /* line, which fed a runaway recursion   */
  if (*(b->bf[ya].s+b->bf[ya].len) == WPE_WR || ya == b->mxlines)
   wrapped = e_ins_wrap_tail_down(b, sch, ya, xa, n, i);
  else
  {
   e_ins_nchar(b, sch, b->bf[ya].s+i+1, 0, ya+1, b->bf[ya].len-i-1);
   if (sch->mark_begin.y == ya)
   {
    if (sch->mark_begin.x > i)
    {
     (sch->mark_begin.y)++;
     (sch->mark_begin.x) -= (i+1);
    }
    else if (sch->mark_begin.x >= xa)
     sch->mark_begin.x += n;
   }
   if (sch->mark_end.y == ya)
   {
    if (sch->mark_end.x > i)
    {
     (sch->mark_end.y)++;
     (sch->mark_end.x) -= (i+1);
    }
    else if (sch->mark_end.x >= xa)
     sch->mark_end.x += n;
   }
  }
/*	 if(*(b->bf[ya].s+i) == ' ') *(b->bf[ya].s+i) = '\0';
         else
*/
  if (wrapped)
  {
   *(b->bf[ya].s+i+1) = '\0';
   b->bf[ya].len = e_str_len(b->bf[ya].s);
   b->bf[ya].nrc = e_str_nrc(b->bf[ya].s);
  }
  if (wrapped && xa > b->bf[ya].len)
  {
   xa -= (b->bf[ya].len);
   ya++;
   b->bf[ya].len = e_str_len(b->bf[ya].s);
   b->bf[ya].nrc = e_str_nrc(b->bf[ya].s);
   if (sch->mark_begin.y == ya && sch->mark_begin.x >= xa)
    sch->mark_begin.x += n;
   if (sch->mark_end.y == ya && sch->mark_end.x >= xa)
    sch->mark_end.x += n;
  }
 }
 else
 {
  if (sch->mark_begin.y == ya && sch->mark_begin.x >= xa)
   sch->mark_begin.x += n;
  if(sch->mark_end.y == ya && sch->mark_end.x >= xa)
   sch->mark_end.x += n;
 }
 /* Insert s at xa, shifting the tail right by n.  Bound every write to the
    mx.x+1 line buffer: the wrap logic above keeps a normal keystroke inside the
    line, but a pathological caller (a huge unbroken run the wrap recursion hands
    down, or a large block/undo insert) can present an n that would not fit --
    clamp rather than run off the end and corrupt the heap. */
 for (j = b->bf[ya].len; j >= xa; --j)
  if (j + n <= b->mx.x)
   *(b->bf[ya].s+j+n) = *(b->bf[ya].s+j);
 for (j = 0; j < n && xa + j <= b->mx.x; ++j)
  *(b->bf[ya].s+xa+j) = *(s+j);
 if (b->bf[ya].s[b->bf[ya].len] == WPE_WR)
  b->bf[ya].s[b->bf[ya].len+1] = '\0';
 /* If the clamp above dropped this line's terminator (a pathological over-long
    insert), pin one at the buffer's last valid index so the length recomputes
    below -- e_str_nrc is strlen -- cannot read past the mx.x+1 buffer.  A
    well-formed line already ends within bounds, so this is a no-op for it. */
 b->bf[ya].s[b->mx.x] = '\0';
 b->b.x = xa + n;
 b->b.y = ya;
 b->bf[ya].len = e_str_len(b->bf[ya].s);
 b->bf[ya].nrc = e_str_nrc(b->bf[ya].s);
 e_buffer_cover_end(b);
 sc_txt_4(ya, b, 0);
 return(xa+n);
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

int e_utf8_charlen(unsigned char c)
{
 if (c < 0x80) return 1;
 if ((c & 0xE0) == 0xC0) return 2;
 if ((c & 0xF0) == 0xE0) return 3;
 if ((c & 0xF8) == 0xF0) return 4;
 return 1;
}

int e_utf8_to_codepoint(unsigned char *buf, int len)
{
 if (len >= 2 && (buf[0] & 0xE0) == 0xC0)
  return ((buf[0] & 0x1F) << 6) | (buf[1] & 0x3F);
 if (len >= 3 && (buf[0] & 0xF0) == 0xE0)
  return ((buf[0] & 0x0F) << 12) | ((buf[1] & 0x3F) << 6) | (buf[2] & 0x3F);
 if (len >= 4 && (buf[0] & 0xF8) == 0xF0)
  return ((buf[0] & 0x07) << 18) | ((buf[1] & 0x3F) << 12) |
         ((buf[2] & 0x3F) << 6) | (buf[3] & 0x3F);
 return -1;
}

int e_utf8_visual_len(unsigned char *s, int bytes)
{
 int pos = 0, vcols = 0;

 while (pos < bytes && s[pos])
 {
  vcols++;
  pos += e_utf8_charlen(s[pos]);
 }
 return vcols;
}

/*     Overwriting of a character       */
int e_put_char(int c, BUFFER *b, SCHIRM *s)
{
 unsigned char cc = c;

 if (b->b.x == b->bf[b->b.y].len)
  e_ins_nchar(b, s, &cc, b->b.x, b->b.y, 1);
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
  e_write(0, 0, f->b->mx.x, f->b->mxlines-1, f, WPE_NOBACKUP);
  WpeMouseRestoreShape();
  f->datnam = tmp;
  f->save = 1;
  FREE(str);
 }
 return(0);
}

/**
 * e_undo_text_copy - The @n characters a record must put back, copied.
 * Return: the copy, or NULL when memory ran out.
 */
static char *e_undo_text_copy(BUFFER *b, int x, int y, int n)
{
 char *str = MALLOC(n > 0 ? n : 1);
 int i;

 if (str == NULL)
  return(NULL);
 for (i = 0; i < n; i++)
  str[i] = b->bf[y].s[x+i];
 return(str);
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
  if ((next->u.pt = e_undo_text_copy(b, x, y, n)) == NULL)
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
 * e_replace_occurrence - Replace @sn characters at (@x, @y) by the @rn
 * bytes of @text, as one Ctrl-U: what Search > Replace does with each
 * occurrence it changes.
 * @b, @s: the buffer and its screen.
 *
 * The record is kept first, then the two halves of the swap run covered
 * with it as their pocket.  A replacement that makes its line wrap moves
 * the breakpoints below it down with their code, as typing does, and Undo
 * moves them back.
 */
void e_replace_occurrence(BUFFER *b, SCHIRM *s, int x, int y, int sn,
                          unsigned char *text, int rn)
{
 Undo *rec;

 b->f->fd.rn = rn;     /* the record keeps the replacement's length */
 rec = e_char_record('s', b, x, y, sn);
 e_buffer_cover_begin(b, rec);
 e_del_nchar(b, s, x, y, sn);
 e_ins_nchar(b, s, text, x, y, rn);
 e_buffer_cover_end(b);
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
 * e_rudo_text_back - Put back the characters a record removed, a line
 * break as a line break (never auto-indented again), and mark them.
 * @b, @s: the buffer and its screen; @rec: an 'r' or 's' record.
 */
static void e_rudo_text_back(BUFFER *b, SCHIRM *s, Undo *rec)
{
 unsigned char *text = (unsigned char *) rec->u.pt;
 int n = rec->a.x;

 if (text[0] == '\n' && n == 1)
  e_car_ret_split(b, s);
 else if (text[n - 1] == '\n')
 {
  e_ins_nchar(b, s, text, rec->b.x, rec->b.y, n - 1);
  e_car_ret_split(b, s);
 }
 else
  e_ins_nchar(b, s, text, rec->b.x, rec->b.y, n);
 s->mark_begin = rec->b;
 s->mark_end.y = rec->b.y;
 s->mark_end.x = rec->b.x + n;
}

/**
 * e_rudo_replace_back - Undo or redo one replacement of Search > Replace.
 *
 * The inverse is recorded first, then the swap of the two strings runs
 * covered, so one Ctrl-R gives the replacement back.
 */
static void e_rudo_replace_back(BUFFER *b, SCHIRM *s, Undo *rec)
{
 /* Set b->f->fd.rn to the length of the string being re-inserted
    (rec->a.x).  e_add_undo reads b->f->fd.rn to store as a.y in
    the reverse entry, so the next undo/redo knows the correct
    length of the OTHER string.  Kruse's original code assumed
    b->f->fd.rn was always current, but it was never synced from
    f->ed->fd.rn (the global search/replace state), causing a.y
    to contain uninitialized garbage (e.g. 189 instead of 5). */
 b->f->fd.rn = rec->a.x;
 e_add_undo('s', b, rec->b.x, rec->b.y, rec->a.y);
 e_buffer_cover_begin(b, NULL);
 e_del_nchar(b, s, rec->b.x, rec->b.y, rec->a.y);
 e_rudo_text_back(b, s, rec);
 e_buffer_cover_end(b);
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
 FENSTER *f = b->f;
 POINT mark_begin = s->mark_begin;
 POINT mark_end = s->mark_end;
 POINT shell_begin = sn->mark_begin;
 POINT shell_end = sn->mark_end;

 s->mark_begin = shell_begin;
 s->mark_end = shell_end;
 if (e_move_block_covered(rec->b.x, rec->b.y, bn, b, f) != E_BLOCK_MOVED)
 {
  /* the lines went back into the shell, which moved its marks: the shell's
     block is its whole text again for the next try */
  sn->mark_begin = shell_begin;
  sn->mark_end = shell_end;
  s->mark_begin = mark_begin;
  s->mark_end = mark_end;
  return(E_BUFFER_NOMEM);
 }
 e_buffer_shell_free(bn);
 rec->u.pt = NULL;
 e_block_record_push('c', b, rec->b.x, rec->b.y);
 return(E_BUFFER_OK);
}

/**
 * e_rudo_block_copy_back - Undo a block copy: the copied block, marked as
 * the record saw it, is deleted again (which records the way back).
 */
static void e_rudo_block_copy_back(BUFFER *b, SCHIRM *s, Undo *rec)
{
 b->b = rec->a;
 s->mark_begin = rec->a;
 s->mark_end = rec->e;
 e_blck_del(b->f);
}

/**
 * e_rudo_block_move_again - Undo or redo a block move: the block, marked as
 * the record saw it, moves back to where the record's cursor is.
 */
static void e_rudo_block_move_again(BUFFER *b, SCHIRM *s, Undo *rec)
{
 b->b = rec->b;
 s->mark_begin = rec->a;
 s->mark_end = rec->e;
 e_blck_move(b->f);
}

/**
 * wpe_buffer_replay_kind - Replay a character or block record for Undo or
 * Redo; its inverse goes to the other list through the primitives' own
 * records.
 * @b: the buffer; @s: its screen; @rec: the record ('a' 'r' 's' 'p' 'c' 'v' 'd').
 *
 * The block kinds run uncovered: their operations take their own covers and
 * push their record after them, so Redo finds it.  An overwrite ('p') puts
 * the old character back and records nothing, as it always did.
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM when a block delete's lines could
 * not come back (its record keeps them for the next try).
 */
int wpe_buffer_replay_kind(BUFFER *b, SCHIRM *s, Undo *rec)
{
 if (rec->type == 'r')
  e_rudo_text_back(b, s, rec);
 else if (rec->type == 's')
  e_rudo_replace_back(b, s, rec);
 else if (rec->type == 'a')
  e_del_nchar(b, s, rec->b.x, rec->b.y, rec->a.x);
 else if (rec->type == 'p')
  b->bf[rec->b.y].s[rec->b.x] = rec->u.c;
 else if (rec->type == 'c')
  e_rudo_block_copy_back(b, s, rec);
 else if (rec->type == 'v')
  e_rudo_block_move_again(b, s, rec);
 else if (rec->type == 'd')
  return(e_rudo_block_back(b, s, rec));
 return(E_BUFFER_OK);
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
 * wpe_buffer_say_nomem - Say on the status row that a change was refused
 * for want of memory; the text is as it was.
 * @what: the change, in the user's words.
 */
void wpe_buffer_say_nomem(BUFFER *b, const char *what)
{
 char line[E_BUFFER_SAY_MAX];

 snprintf(line, sizeof line, "Out of memory: %s was not made, the text is unchanged", what);
 wpe_buffer_trace("buffer nomem %s", what);
 if (WpeEditor != NULL && WpeEditor->mxedt > 0)
  e_status_say(WpeEditor->f[WpeEditor->mxedt], line);
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

