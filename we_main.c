/* we_main.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

/*
                       Window - Editor (WE)
                           Version 1.0
                                       Copyright by F.Kruse       */

/* includes */
#include <signal.h>
#include "messages.h"
#include "edit.h"
#include "we_ai.h"      /* AI assistant config init (guarded by WPE_AI) */
#include "we_lineend.h"
#include "we_testdump.h"
#include "we_startplan.h"   /* where the debugger's own folders are made */

#ifdef UNIX
#include <sys/types.h>
#include <sys/stat.h>
#include "attrb.h"
#endif

#ifdef DJGPP
#include <dpmi.h>
#endif

/* externals */
extern char *info_file;
extern char *e_tmp_dir;
extern char *e_hlp_str[];

extern int col_num;

extern FARBE *u_fb, *x_fb;

/* globals */
struct CNT *WpeEditor;
int fk__cursor = 0;

WOPT *eblst, *fblst, *mblst, *dblst, *xblst, *wblst, *rblst;
WOPT *ablst, *sblst, *hblst, *gblst, *oblst;
WOPT *eblst_lsp;   /* editor bottom bar shown while an LSP-supported file is active */
#ifdef WPE_AI
WOPT *eblst_ai;    /* editor bottom bar shown while the AI assistant is enabled */
WOPT *eblst_lspai; /* editor bottom bar when a file has BOTH an LSP server and AI */
#endif

char *e_hlp, *user_shell;
WOPT *blst;
int nblst = 7;

#if MOUSE
int fk__mouse_stat = 1;
struct mouse e_mouse = {  0, 0, 0  };
#endif

/* prototypes */
int e_read_help_str(void);

/* preinitialized arrays */
char *e_msg[] = {  "Not Enough Memory",
		   "Option-File got the Wrong Version",
		   "Can\'t read Option-File",
		   "More Than 35 Windows",
		   "String NOT found",
		   "Can\'t open File %s",             /*  Number 5  */
		   "Can\'t close File",
		   "No more Undo",
		   "Execute Command: %s\n",
		   "Command not found",
		   "\nStrike <Return> to continue\n", /*  Number 10 */
		   "Not Installed",
		   "Can\'t open Option-File",
		   "Can\'t write to Option-File",
		   "File failed to save",
		   "Not all files saved",             /*  Number 15  */
		   "Can\'t Print File",
		   "No more Redo",
                   "Can\'t open directory",
                   "Can\'t open HOME directory",
                   "System error",                    /*  Number 20 */
                   "Wastebasket is not available",
                   "Can\'t create directory",
                   "Can\'t access file or directory",
                   "Can\'t remove file or directory",
                   "Can\'t link file or directory",   /*  Number 25 */
                   "File already exist with \"new.dir\" name",
                   "Cannot change permissions",
                   "Can\'t rename file or directory",
                   "Can\'t open file for reading",
                   "Can\'t open file for writing",    /*  Number 30 */
                   "Can\'t allocate buffer for copying",
                   "Inconsistent copying"
		};

OPT opt[] = {  {"#",        3, '#', AltSYS},
	       {"File",    10, 'F', AltF},
	       {"Edit",    19, 'E', AltE},
	       {"Search",  28, 'S', AltS},
	       {"Block",   39, 'B', AltB},
	       {"Options", 49, 'O', AltO},
	       {"Window",  61, 'W', AltW},
	       {"Help",    72, 'H', AltH}
#ifdef PROG
	       , {NULL,  0, 0, 0}
	       , {NULL,  0, 0, 0}
#endif
#ifdef DEBUGGER
	       , {NULL, 0, 0, 0}
#endif
	    };

/* The bottom bars are built from the key table: see we_bars.c. */

void ECNT_Init(ECNT *cn)
{
 cn->mxedt = -1;
 cn->curedt = 0;
 cn->edt[0] = 0;
 cn->fb = NULL;
 cn->print_cmd = WpeStrdup(PRNTCMD);

 cn->dirct = WpeGetCurrentDir(cn);

 strcpy(cn->fd.search, "");
 strcpy(cn->fd.replace, "");
 strcpy(cn->fd.file, SUDIR);
 cn->fd.dirct = WpeStrdup(cn->dirct);

 cn->fd.sw = 16;
 cn->fd.sn = 0;
 cn->fd.rn = 0;

 cn->sdf = cn->rdf = cn->fdf = cn->ddf = cn->wdf = cn->hdf = cn->shdf = NULL;

 /*   standard adjustments    */
 cn->dtmd = DTMD_NORMAL;
 cn->eol_default = E_EOL_LF;
 cn->autosv = 0;
 cn->maxchg = 999;
 cn->numundo = 10;
 cn->maxcol = MAXCOLUM;
 cn->tabn = 8;
 cn->autoindent = 3;
 cn->tabs = MALLOC((cn->tabn+1)*sizeof(char));
 WpeStringBlank(cn->tabs, cn->tabn);
 cn->flopt = FM_REKURSIVE_ACTIONS | FM_REMOVE_INTO_WB | FM_MOVE_PROMPT |
   FM_MOVE_PROMPT | FM_PROMPT_DELETE;
 cn->edopt = ED_SOURCE_AUTO_INDENT | ED_ERRORS_STOP_AT | ED_SYNTAX_HIGHLIGHT;
}

/**
 * e_info_search_path - Build the Help > Info file-search path, xwpe's own
 * infodir first.
 *
 * `make install` drops xwpe.info in the configured infodir -- /usr/share/info on
 * a Debian (--prefix=/usr) build, but ~/.local/share/info, a Homebrew prefix, or
 * /usr/local/share/info for the from-source installs people do on macOS and the
 * BSDs.  The old path was a fixed "/usr/share/info:...", so Help > Info found the
 * manual ONLY when the prefix happened to be /usr -- elsewhere it silently fell
 * back to the system dir node.  Putting the compiled-in infodir (XWPE_INFODIR,
 * set from $(infodir) at build time) ahead of `fallback` makes Help > Info open
 * OUR manual wherever it was installed, for any --prefix.
 *
 * Return: a malloc'd "infodir:fallback" string (caller frees), or a copy of
 * fallback when XWPE_INFODIR is unset.
 */
static char *e_info_search_path(const char *fallback)
{
 char *path;
#ifdef XWPE_INFODIR
 if (XWPE_INFODIR[0])
 {
  path = MALLOC(strlen(XWPE_INFODIR) + strlen(fallback) + 2);
  if (path)
   sprintf(path, "%s%c%s", XWPE_INFODIR, PTHD, fallback);
  return(path);
 }
#endif
 path = MALLOC(strlen(fallback) + 1);
 if (path)
  strcpy(path, fallback);
 return(path);
}

/* Create `path` and any missing parent directories (best-effort, mode 0700). */
static void e_mkdirs(const char *path)
{
 char tmp[WPE_PATH_SIZE];
 size_t i, n = strlen(path);
 if (n == 0 || n >= sizeof tmp) return;
 memcpy(tmp, path, n + 1);
 for (i = 1; i < n; i++)
  if (tmp[i] == '/') { tmp[i] = '\0'; mkdir(tmp, 0700); tmp[i] = '/'; }
 mkdir(tmp, 0700);
}

/* Copy file `src` to `dst` (best-effort).  Returns 0 on success. */
static int e_copy_file(const char *src, const char *dst)
{
 FILE *in = fopen(src, "rb"), *out;
 char buf[4096];
 size_t r;
 if (!in) return -1;
 out = fopen(dst, "wb");
 if (!out) { fclose(in); return -1; }
 while ((r = fread(buf, 1, sizeof buf, in)) > 0)
  if (fwrite(buf, 1, r, out) != r) { fclose(in); fclose(out); return -1; }
 fclose(in);
 return fclose(out) == 0 ? 0 : -1;
}

/**
 * e_xdg_config_paths - Where the configuration folder, its xwperc and the
 * legacy xwperc are.
 * @xdg: $XDG_CONFIG_HOME or NULL; @home: the home folder.
 * @dir/@xdgfile/@legacy: receive the paths, each of WPE_PATH_SIZE bytes.
 *
 * A path is whole or the call fails: cut at some length, the configuration
 * folder would be created, and the options saved, somewhere the user never
 * named.
 * Return: 0, or -1 when one of them does not fit.
 */
static int e_xdg_config_paths(const char *xdg, const char *home,
                              char *dir, char *xdgfile, char *legacy)
{
 if (xdg && *xdg)
 {
  if (wpe_path_join(dir, WPE_PATH_SIZE, xdg, "xwpe")) return -1;
 }
 else
 {
  if (wpe_path_join(dir, WPE_PATH_SIZE, home, ".config")) return -1;
  if (wpe_path_cat(dir, WPE_PATH_SIZE, "/xwpe")) return -1;
 }
 if (wpe_path_join(xdgfile, WPE_PATH_SIZE, dir, OPTION_FILE)) return -1;
 if (wpe_path_join(legacy, WPE_PATH_SIZE, home, XWPE_HOME)) return -1;
 if (wpe_path_cat(legacy, WPE_PATH_SIZE, "/" OPTION_FILE)) return -1;
 return 0;
}

/* The user config file per the XDG Base Directory spec:
   $XDG_CONFIG_HOME/xwpe/xwperc (default ~/.config/xwpe/xwperc).  A legacy
   ~/.xwpe/xwperc is migrated (copied) the first time so existing settings carry
   over; if that copy fails, the legacy file is used so nothing is lost.  Returns
   a malloc'd path, or NULL when the folder's path is longer than the system
   allows (the editor then runs with its defaults and says so). */
static char *e_xdg_config_file(void)
{
 const char *xdg = getenv("XDG_CONFIG_HOME");
 const char *home = getenv("HOME");
 char dir[WPE_PATH_SIZE], xdgfile[WPE_PATH_SIZE], legacy[WPE_PATH_SIZE];
 char *p;
 size_t n;
 if (!home || !*home) home = ".";
 if (e_xdg_config_paths(xdg, home, dir, xdgfile, legacy) != 0)
  return NULL;

 e_mkdirs(dir);                          /* so saving here works even on a fresh box */
 if (access(xdgfile, F_OK) != 0 && access(legacy, F_OK) == 0) {
  if (e_copy_file(legacy, xdgfile) != 0) {   /* migration failed: keep the legacy file */
   n = strlen(legacy) + 1; p = MALLOC(n); if (p) memcpy(p, legacy, n); return p;
  }
  /* Migrated: leave a note in the old directory so anyone who looks there knows
     where the settings went (the legacy dir is kept as a backup, not deleted). */
  {
   char note[WPE_PATH_SIZE];
   FILE *nf = NULL;
   if (wpe_path_join(note, sizeof note, home, XWPE_HOME) == 0 &&
       wpe_path_cat(note, sizeof note, "/MOVED_TO_XDG.txt") == 0)
    nf = fopen(note, "w");
   if (nf) {
    fprintf(nf,
      "xwpe now stores its files following the XDG Base Directory specification.\n"
      "Your settings were copied automatically:\n\n"
      "  configuration : %s\n"
      "  runtime state : $XDG_STATE_HOME/xwpe/  (default ~/.local/state/xwpe/)\n\n"
      "This directory (~/%s) is kept only as a backup and is no longer read.\n"
      "You can delete it once the new location works for you.\n",
      xdgfile, XWPE_HOME);
    fclose(nf);
   }
  }
 }
 n = strlen(xdgfile) + 1; p = MALLOC(n); if (p) memcpy(p, xdgfile, n); return p;
}

/**
 * e_binding_trace - Append one dispatch of the key table to the file
 * XWPE_UI_TRACE names, for the tests that drive every key of a table.
 */
static void e_binding_trace(int code, int style, int context, int action)
{
 const char *path = getenv("XWPE_UI_TRACE");
 FILE *tf;

 (void)style;
 if (!path)
  return;
 tf = fopen(path, "a");
 if (!tf)
  return;
 fprintf(tf, "binding context=%s code=%d action=%s\n", wpe_context_name(context), code,
         wpe_action_name((WpeAction)action));
 fclose(tf);
}

/**
 * e_profile_is_new - Has this user never run the editor?
 *
 * The editor creates its configuration folder at every start, so "no option
 * file" is not enough: a profile is new only when there is no xwperc in the
 * configuration folder or in the legacy ~/.xwpe, and neither folder exists.
 * An installation of 1.6 has the first, an older one the second, and neither
 * is ever asked which keys it wants, so an existing user's style never
 * changes under him.  It must run before e_xdg_config_file creates the
 * folder.  An xwperc in the folder the editor was started in is a project's
 * decision, not a new profile.
 * Return: 1 when the profile is new.
 */
static int e_profile_is_new(void)
{
 const char *home = getenv("HOME");
 char dir[WPE_PATH_SIZE], xdgfile[WPE_PATH_SIZE], legacy[WPE_PATH_SIZE], legacydir[WPE_PATH_SIZE];

 if (!home || !*home) home = ".";
 if (e_xdg_config_paths(getenv("XDG_CONFIG_HOME"), home, dir, xdgfile, legacy) != 0)
  return 0;
 if (wpe_path_join(legacydir, WPE_PATH_SIZE, home, XWPE_HOME) != 0)
  return 0;
 if (access(OPTION_FILE, F_OK) == 0)
  return 0;
 return access(xdgfile, F_OK) != 0 && access(legacy, F_OK) != 0 &&
        access(dir, F_OK) != 0 && access(legacydir, F_OK) != 0;
}

/**
 * e_xdg_config_file_or_none - The user's own option file, or "" when its
 * folder's path is longer than the system allows.
 *
 * "" is the state every reader of an option file treats as "there is none"
 * (e_opt_file_missing), so the editor runs with its defaults and says so.
 * It must never be NULL for the user's own file beside a start-folder
 * xwperc: NULL there means "the options come from the user's own file",
 * and a cloned folder's xwperc would then set the keys only the user may
 * set (a hook, an engine command, an endpoint).
 * Return: a malloc'd path, "" included.
 */
static char *e_xdg_config_file_or_none(void)
{
 char *path = e_xdg_config_file();

 if (path)
  return path;
 return WpeStrdup("");
}

int main(int argc, char **argv)
{
 FARBE *fb;
 ECNT *cn;
 int i, err = 0, g[4];
 int so = 0, sd = 1;
 char *tp;
 const char *bad_keys = NULL;
 int new_profile;

 /* Non-interactive query flags, answered before any terminal/UI setup so they
    work with no controlling tty -- in scripts, pipes and packaging test suites.
    They never start the editor. */
 for (i = 1; i < argc; i++)
 {
  if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V"))
  {
   printf("xwpe %s\n", VERSION);
   return 0;
  }
  if (!strcmp(argv[i], "--help"))
  {
   printf("Usage: %s [file ...]\n"
          "Wpe/Xwpe programming editor.  With no file it starts the full-screen\n"
          "editor; file arguments are opened on start.\n"
          "  --keys STYLE    key style for this run: borland or modern\n"
          "                  (also $XWPE_KEYS; the option file is not changed)\n"
          "  --version, -V   print the version and exit\n"
          "  --help          print this help and exit\n", argv[0]);
   return 0;
  }
 }
 if (e_keys_session_read(argc, argv, &bad_keys) != 0)
 {
  fprintf(stderr, "%s: unknown key style '%s' (borland or modern)\n", argv[0], bad_keys);
  return 2;
 }
 new_profile = e_profile_is_new();   /* before anything makes the configuration folder */

 if ((cn = (ECNT *)MALLOC(sizeof(ECNT))) == NULL) {
  printf(" Fatal Error: %s\n", e_msg[ERR_LOWMEM]);
  return 0;
 }
 ECNT_Init(cn);
 e_paint_desk_ready();
 e_layout_desk_ready();
 e_ini_unix(&argc, argv);
 e_switch_screen(1);
 fb = e_ini_farbe();
 WpeEditor = cn;
 cn->fb = fb;
 wpe_buffer_fault_from_env();
 wpe_test_dump_from_env();

 info_file = e_info_search_path(INFO_DIR);   /* our infodir first, then the system dirs */
 e_read_help_str();
 e_hlp = e_hlp_str[0];
 if (!(user_shell = getenv("SHELL"))) user_shell = DEF_SHELL;
#ifdef HAVE_MKDTEMP
 e_tmp_dir = strdup("/tmp/xwpe_XXXXXX");
 if (mkdtemp(e_tmp_dir) == NULL)
#else
#if defined(HAVE_TEMPNAM)
 e_tmp_dir = tempnam(NULL, "xwpe_");
#else
 e_tmp_dir = MALLOC(128);
 sprintf(e_tmp_dir, "/tmp/we_%u", (unsigned) getpid());
 e_tmp_dir = REALLOC(e_tmp_dir, (strlen(e_tmp_dir)+1)*sizeof(char));
#endif
 if ((e_tmp_dir == NULL) || (mkdir(e_tmp_dir, 0700) != 0))
#endif
 {
  perror("Xwpe: ");
  return 1;
 }
 /* A debugger the editor starts runs in a folder of its own under the
    editor's temporary folder, which goes with the editor. */
 wpe_startplan_debug_base_set(e_tmp_dir);

 for (i = 1; i < argc; i++)
 {
  if (*argv[i] == '-')
  {
   if (*(argv[i]+1) == 's' && *(argv[i]+2) == 'o') so = 1;
   else if (*(argv[i]+1) == 's' && *(argv[i]+2) == 'f')
   {
    sd = 0;
    cn->optfile = MALLOC((strlen(argv[i+1])+1)*sizeof(char));
    strcpy(cn->optfile, argv[i+1]);
   }
  }
 }
#ifdef PROG
 e_ini_prog(cn);
#endif
 e_opt_note_launch_dir(cn->dirct);   /* where ./xwperc and the instructions are read */
 if (sd != 0)
 {
  FILE *f = fopen(OPTION_FILE, "r");
  if (f)
  {
   fclose(f);
   cn->optfile = e_mkfilename(cn->dirct, OPTION_FILE);
   e_opt_use_user_file(e_xdg_config_file_or_none());   /* the keys a project may not set */
  }
  else
  {
   /* ~/.config/xwpe/xwperc (XDG), legacy migrated; "" when its folder's
      path is longer than the system allows. */
   cn->optfile = e_xdg_config_file_or_none();
  }
 }
 if (so == 0) err = e_opt_read(cn);
 e_keys_migrate(cn);          /* a 1.6 CUA profile becomes Modern, once */
 e_keys_session_apply(cn);    /* --keys and $XWPE_KEYS over the file, this run only */
#ifdef DEBUGGER
 wpe_binding_set_session_probe(e_d_session_runs);   /* F10 steps while a session is open */
#endif
 if (getenv("XWPE_UI_TRACE"))
  wpe_binding_set_trace(e_binding_trace);
#ifdef WPE_AI
 wpe_ai_config_init();  /* bake defaults + apply XWPE_AI_* env over the config */
#endif
 e_edit(cn, ""); /* Clipboard (must first read option file) */
 if ((tp = getenv("INFOPATH")) != NULL)
 {
  if (info_file) FREE(info_file);
  info_file = e_info_search_path(tp);    /* keep our infodir ahead of $INFOPATH too */
 }
 blst = eblst_bar;
 e_ini_desk(cn);
 e_use_editor_bar(cn->f[0], eblst);
#if MOUSE
 g[0] = 4; g[2] = 0; g[3] = 0;
 fk_mouse(g);
 g[0] = 1;
 fk_mouse(g);
#endif
 /* this error comes from reading the options file */
 if (err > 0) e_error(e_msg[err], 0, cn->fb);
 e_opt_say_ignored_keys(cn);
 e_opt_say_file_missing(cn, E_OPT_NOT_READ);
 if (e_keys_notice_wanted()) e_keys_notice_shown(cn);
 if (new_profile && e_keys_ask_allowed()) e_keys_first_run(cn);
 if (WpeIsProg()) WpeSyntaxReadFile(cn);
#ifdef UNIX
 for (i = 1; i < argc; i++)
  if (!strcmp(argv[i], "-r")) e_recover(cn);
#endif
 for (i = 1; i < argc; i++)
 {
  if (*argv[i] == '-')
  {
   if (*(argv[i]+1) == 's' && *(argv[i]+2) == 'f') i++;
   else if (e_keys_arg_takes_value(argv[i])) i++;   /* the style, not a file */
#if defined(UNIX) && defined(PROG)
   else if (*(argv[i]+1) == 'p' && *(argv[i]+2) == 'm') e_we_sw |= 8;
#endif
   continue;
  }
  else e_edit(cn, argv[i]);
 }
 if (cn->mxedt == 0) WpeManager(cn->f[cn->mxedt]);
 do
 {
  if (cn->mxedt == 0)
  {
   /* No user windows left: cn->f[0] is the internal clipboard "Buffer" and
      must NEVER be drawn as a real editor window (no frame, no [X], not
      draggable).  Repaint the desktop, clear the status hints leftover from
      whichever window was last active, hide the cursor and drive the top
      menu bar.  Picking File>New / File-Manager / etc. bumps mxedt and the
      next iteration resumes the normal editor / FM dispatch. */
   e_show_empty_desk(cn);
   i = WpeHandleMainmenu(-1, cn->f[0]);
  }
  else if (cn->f[cn->mxedt]->dtmd == DTMD_FILEMANAGER) i = WpeHandleFileManager(cn);
  else if (cn->f[cn->mxedt]->dtmd == DTMD_DATA) i = e_data_eingabe(cn);
  else i = e_eingabe(cn);
  if (i == WPE_RESULT_QUIT) i = e_quit(cn->f[cn->mxedt]);
 }
 while (i != WPE_RESULT_QUIT);
 WpeExit(0);
 return 0;
}

/* The editor-window bars: the ones that name F9 have one more entry than
   the plain one.  The debug bar belongs here too, so a window that gets
   it paints and takes clicks on exactly its own entries. */
static WOPT *const e_editor_bars[] = {
 eblst_bar, eblst_prog_bar, eblst_lsp_bar,
 dblst_src_bar, dblst_run_bar,
#ifdef WPE_AI
 eblst_ai_bar, eblst_lspai_bar,
#endif
};

#define E_EDITOR_BARS ((int)(sizeof(e_editor_bars) / sizeof(e_editor_bars[0])))

/**
 * e_is_editor_bar - Is @bar one of the editor-window bars?
 */
static int e_is_editor_bar(WOPT *bar)
{
 int i;

 for (i = 0; i < E_EDITOR_BARS; i++)
  if (e_editor_bars[i] == bar)
   return(1);
 return(0);
}

/**
 * e_editor_bar_size - How many entries an editor-window bar has.
 * @bar: one of the editor bars.
 *
 * The bars of the programming modes name F9 and have one entry more than
 * the plain editor's, and the CUA style's bars have fewer than the
 * Borland style's, so the count travels with the bar: a window that
 * changes bar (the AI switch, the CUA keys) paints and takes clicks on
 * exactly the entries its new bar has in the style in use.
 * Return: the number of entries, 0 for a bar that is not an editor bar.
 */
int e_editor_bar_size(WOPT *bar)
{
 return(e_is_editor_bar(bar) ? e_bar_length(bar) : 0);
}

/**
 * e_use_editor_bar - Give an editor window @bar, with its entry count.
 * @f: the window; @bar: one of the editor bars.
 */
void e_use_editor_bar(FENSTER *f, WOPT *bar)
{
 f->blst = bar;
 f->nblst = e_editor_bar_size(bar);
}

/**
 * e_plain_editor_bar - The bar of an editor window with no language
 * server and no assistant.
 * @style: WPE_STYLE_*.
 *
 * wpe and xwpe show F9, which builds there; we does not build and keeps
 * the classic bar.
 */
WOPT *e_plain_editor_bar(int style)
{
 if (WpeIsProg())
  return(eblst_prog_bar);
 return(eblst_bar);
}

/**
 * e_use_style_bars - Make the bars of a key style the ones every kind of window
 * draws from.
 * @style: WPE_STYLE_*.
 *
 * Every bar is one array that windows point at; this fills them for the style
 * and names the plain editor bar the style and the mode call for.
 */
static void e_use_style_bars(int style)
{
 e_bars_use_style(style);
 eblst = e_plain_editor_bar(style);
 eblst_lsp = eblst_lsp_bar;
 fblst = fblst_bar; mblst = mblst_bar; dblst = dblst_bar;
 xblst = xblst_bar; wblst = wblst_bar; rblst = rblst_bar; ablst = ablst_bar;
 sblst = sblst_bar; hblst = hblst_bar; gblst = gblst_bar; oblst = oblst_bar;
#ifdef WPE_AI
 eblst_ai = eblst_ai_bar;
 eblst_lspai = eblst_lspai_bar;
#endif
}

/**
 * e_switch_blst - The key style changed: draw the bars again for it.
 * @cn: the editor.
 *
 * The bars are filled for the new style and every editor window takes the
 * bar its style offers, with the length that style gives it.  The debugger's
 * bar belongs to the Borland style alone, so the editor bars are all picked
 * again.
 */
int e_switch_blst(ECNT *cn)
{
 e_use_style_bars(wpe_binding_style(cn->edopt));
 e_refresh_editor_bars(cn);
 return(0);
}

#ifdef WPE_AI
extern const char *e_lsp_server_label(FENSTER *);  /* non-NULL => file has a server */
extern int wpe_ai_enabled(void);                   /* runtime AI toggle (ED_AI_ENABLE bit) */

/**
 * e_ai_bar_wanted - Should an editor file window show the "Alt-G AI" bar entry?
 *
 * Only when the assistant is enabled AND the editor runs in programming mode
 * (wpe, xwpe).  Under we Alt-G is Go to Line and a click on the entry would
 * open nothing, so the plain editor keeps its classic bar and the user is not
 * shown a key that does something else.
 */
int e_ai_bar_wanted(void)
{
 if (!wpe_ai_enabled()) return 0;
 if (!WpeIsProg()) return 0;
 return 1;
}
#endif

#ifdef DEBUGGER
extern const char *e_lsp_server_label(FENSTER *);  /* non-NULL => file has a server */

/**
 * e_lsp_bar_wanted - Should an editor window show the language-server bar?
 * @f: the window.
 *
 * Only in the programming modes, where Alt-Q reaches the server: under we
 * the whole programming layer is off, and the bar would offer a key and
 * an F9 that do nothing there.
 */
int e_lsp_bar_wanted(FENSTER *f)
{
 if (!WpeIsProg())
  return(0);
 return(e_lsp_server_label(f) != NULL);
}
#endif

/**
 * e_editor_bar_for - The editing bar a file window shows right now.
 * @f: the window; @style: the key style.
 *
 * One place decides which editing keys a file window offers, so a window
 * never keeps a bar that a change made wrong: the assistant switched off, a
 * language server that answered for a different file.  A file with both a
 * server and the assistant gets the combined bar, so neither hint is lost.
 * Return: the bar to give the window.
 */
static WOPT *e_editor_bar_for(FENSTER *f, int style)
{
#ifdef WPE_AI
#ifdef DEBUGGER
 if (e_ai_bar_wanted() && e_lsp_bar_wanted(f))
  return(eblst_lspai_bar);
#endif
 if (e_ai_bar_wanted())
  return(eblst_ai_bar);
#endif
#ifdef DEBUGGER
 if (e_lsp_bar_wanted(f))
  return(eblst_lsp_bar);
#endif
 return(e_plain_editor_bar(style));
}

/**
 * e_debug_bar_wanted - Should the editor windows show the debugger's bar?
 * @cn: the editor.
 *
 * Only while a session runs; both key styles have decided debugger keys and
 * the bar names the ones of the style in use.
 * Return: 1 when the debugger's bar is the one to show.
 */
static int e_debug_bar_wanted(ECNT *cn)
{
#ifndef DEBUGGER
 return(0);
#else
 (void)cn;
 return(e_d_session_runs());
#endif
}

/**
 * e_bottom_bar_repaint - Draw the bottom row again from the active window.
 * @cn: the editor.
 *
 * A bar chosen after the window was drawn (a session that started, the
 * assistant switched off) reaches the screen at once, so the user reads the
 * keys that work now instead of the ones the last repaint happened to show.
 */
void e_bottom_bar_repaint(ECNT *cn)
{
 FENSTER *f = cn->f[cn->mxedt];

 if (!f || !f->fb)
  return;
 blst = f->blst;
 nblst = f->nblst;
 e_pr_uul(cn->fb);
 e_refresh();
}

/**
 * e_debug_program_runs - Does the debugger's bar say the program runs?
 * @cn: the editor.
 * Return: 1 while the program runs under the debugger in the Borland style.
 */
static int e_debug_program_runs(ECNT *cn)
{
#ifndef DEBUGGER
 return(0);
#else
 return(e_debug_bar_wanted(cn) && e_d_program_runs());
#endif
}

/**
 * e_is_messages_window - Is @f the Messages window?
 */
static int e_is_messages_window(FENSTER *f)
{
 return(f && f->datnam && !strcmp(f->datnam, "Messages"));
}

/**
 * e_use_messages_bar - Pick the bottom bar of the Messages window for a
 * debug session that runs the program.
 * @f: Messages; @running: whether the program runs under the debugger.
 *
 * The program's output makes Messages the window the user types in, so it
 * is the window on top for most of a run under gdb: its bar must name Pause
 * too, or a terminal user whose Ctrl-Break never arrives would have nothing
 * to click.  Once the program stops, Messages has its own bar again.
 */
static void e_use_messages_bar(FENSTER *f, int running)
{
 if (running)
 {
  f->blst = dblst_run_bar;
  f->nblst = e_bar_length(dblst_run_bar);
 }
 else if (f->blst == dblst_run_bar)
 {
  f->blst = mblst;
  f->nblst = e_bar_length(mblst_bar);
 }
}

/**
 * e_refresh_editor_bars - Re-pick every editor window's bottom bar.
 * @cn: the editor.
 *
 * Called when a debug session starts or ends, when the assistant's runtime
 * toggle flips in Options > AI, and when the key style changes: without it an
 * open window keeps the bar it was given when it was opened, so the user is
 * shown keys that no longer do what they say.  Only the swappable file bars
 * are touched; Help, Watches and Stack keep their own, and Messages keeps
 * its own except while the program runs (see e_use_messages_bar).
 */
void e_refresh_editor_bars(ECNT *cn)
{
 e_pick_editor_bars(cn);
 e_bottom_bar_repaint(cn);
}

/**
 * e_pick_editor_bars - Choose every editor window's bottom bar, without
 * drawing the bottom row.
 * @cn: the editor.
 *
 * For a change that happens while a box may be in front: the bars are set
 * now and the row is drawn (e_bottom_bar_repaint) once the box is gone,
 * so the row under a box keeps showing the bar that box's window had.
 */
void e_pick_editor_bars(ECNT *cn)
{
 int i, style = wpe_binding_style(cn->edopt);
 int debugging = e_debug_bar_wanted(cn);

 for (i = 0; i <= cn->mxedt; i++)
 {
  FENSTER *f = cn->f[i];

  if (e_is_messages_window(f))
  {
   e_use_messages_bar(f, e_debug_program_runs(cn));
   continue;
  }
  if (!e_is_editor_bar(f->blst))
   continue;                            /* not an editor-file bar -- leave it   */
  if (!debugging || !e_give_debug_bar(f))
   e_use_editor_bar(f, e_editor_bar_for(f, style));
 }
}

/**
 * e_give_debug_bar - Give a source window the debugger's bar while a
 * session runs.
 * @f: a file window.
 *
 * Used for every source window, the ones open when the session starts and
 * the ones it opens later (a step into another file, a stop in a file not
 * yet open, F3 in the middle of a session), so F4 is Go to cursor in each
 * of them and none shows the editing keys until the session ends.  While
 * the program runs the bar names Pause and the keys that work then; at a
 * stop it names the stepping keys.
 * Return: 1 when the window got the debugger's bar.
 */
int e_give_debug_bar(FENSTER *f)
{
 if (!f || !e_debug_bar_wanted(f->ed))
  return(0);
#ifdef DEBUGGER
 if (e_d_program_runs())
  e_use_editor_bar(f, dblst_run_bar);
 else
  e_use_editor_bar(f, dblst_src_bar);
#endif
 return(1);
}

/**
 * e_bar_offers_goto_cursor - Does this window's bar offer F4 Go to cursor?
 * @f: the window F4 was pressed in.
 *
 * F4 runs to the cursor only where the bottom row says it does: a source
 * window during a session.  In Messages, Watches or Stack, and in the CUA
 * style, F4 keeps doing what it did before.
 * Return: 1 when F4 is Go to cursor in @f.
 */
int e_bar_offers_goto_cursor(FENSTER *f)
{
 return(f && f->blst == dblst_src_bar);
}

/**
 * e_pack_button_bar - Arrange a status-bar's clickable shortcuts compactly.
 *
 * Used to give a window's bottom shortcut bar (F1 Help, Add, Delete, ...) the
 * same tight, left-aligned look as the editor's, instead of leaving wide gaps
 * the user reads as "broken". Each label is placed right after the previous
 * one with a uniform gap, and the stored position drives BOTH where the label
 * is drawn and where a mouse click on it lands -- so what the user sees and
 * what they can click always agree. Works for any shortcut bar.
 *
 * @bar: the button list to lay out (modified in place).
 * @n:   number of buttons in @bar.
 * @x0:  first column.
 * @gap: blank columns left between adjacent buttons.
 */
void e_pack_button_bar(WOPT *bar, int n, int x0, int gap)
{
 int i, x = x0;

 for (i = 0; i < n; i++)
 {
  bar[i].x = x;
  x += (int)strlen(bar[i].t) + gap;
 }
}

/* Render the empty desktop shown when no user windows are open.  Keep blst
   pointing at a valid array (defensive: e_pr_uul indexes blst[i] up to nblst-1)
   but force nblst=0 so the bottom hint bar is BLANK -- the previous window's
   hints (FM "Edit Move COpy Remove ^W Close W. Alt-X Quit" etc.) would
   otherwise stay on screen and lie about what is currently actionable.  Then
   repaint background + top menu bar via e_ini_desk and hide the cursor (the
   internal clipboard Buffer at f[0] has no visible frame to host it). */
void e_show_empty_desk(ECNT *cn)
{
 blst = eblst_bar;
 nblst = 0;
 e_ini_desk(cn);
 fk_cursor(0);
 e_refresh();
}

void e_ini_desk(ECNT *cn)
{
 extern int e_mn_men;
 int i;

 e_use_style_bars(wpe_binding_style(cn->edopt));
 /* Pack the project (Data window) status bar compactly like the editor bar,
    leaving the right edge free for the "Project: <name>" label drawn by
    e_pr_uul. Two-column gap matches the editor's button spacing. */
 e_pack_button_bar(oblst, 5, 0, 2);
 e_cls(cn->fb->df.fb, cn->fb->dc);
 e_blk(MAXSCOL, 0, 0, cn->fb->mt.fb);

 /* put out the main menu */
 for (i = 0; i < MENOPT; ++i)
 {
    e_pr_str_wsd(opt[i].x, 0, opt[i].t, cn->fb->mt.fb, 0, 1, cn->fb->ms.fb,
                 ( i == 0 ? 0 : opt[i].x-e_mn_men),
                 (i == MENOPT-1) ? MAXSCOL-1 : opt[i+1].x-e_mn_men-1);
 }

 e_pr_uul(cn->fb);
}

void FARBE_Init(FARBE *fb)
{
 fb->er = e_s_x_clr(15,4);  /*  Editor Frames         */
 fb->et = e_s_x_clr(11,4);  /*  Editor Text           */
 fb->ez = e_s_x_clr(4,7);   /*  Editors Text highlighted  */
 fb->es = e_s_x_clr(10,4);  /*  Editors Window-Button */
 fb->ek = e_s_x_clr(11,6);  /*  Editors highlighted (find) */
 fb->em = e_s_x_clr(4,6);   /*  Mouse slider bar         */
 fb->mr = e_s_x_clr(0,7);   /*  Menu Frame             */
 fb->mt = e_s_x_clr(0,7);   /*  Menu Text               */
 fb->mz = e_s_x_clr(0,2);   /*  Menu Text highlighted      */
 fb->ms = e_s_x_clr(1,7);   /*  Menu-Switch           */
 fb->nr = e_s_x_clr(0,7);   /*  Options/Data/FileManager Frame + title: black
                               on light-grey. Was (15,7) white-on-white, which
                               made the frame border and the window TITLE
                               (drawn in this colour by e_std_rahmen) invisible
                               on Options, Message, File-Manager and Project
                               (Data) windows. */
 fb->nt = e_s_x_clr(0,7);   /*  Options Text           */
 fb->nsnt = e_s_x_clr(11,7);/*  Options Text-Switch  */
 fb->ne = e_s_x_clr(1, 7);  /*  Options Window-Button */
 fb->fr = e_s_x_clr(15,4);  /*  Editor Control Bar (?) (Schreibleiste) passive */
 fb->fa = e_s_x_clr(15,5);  /*  Field active: white on magenta (unique focus) */
 fb->ft = e_s_x_clr(0,6);   /*  Data Text		  */
 fb->fz = e_s_x_clr(15,2);  /*  Data active highlighted	  */
 fb->frft = e_s_x_clr(15,6);/*  Data passive highlighted	  */
 fb->fs = e_s_x_clr(0,6);   /*  Switch Text		  */
 fb->nsft = e_s_x_clr(15,6);/*  Switch shortcut (white on cyan)  */
 fb->fsm = e_s_x_clr(15,5); /*  Switch active: white on magenta (matches focus) */
 fb->nz = e_s_x_clr(0,6);   /*  Button passive: black on cyan (neutral) */
 fb->ns = e_s_x_clr(15,6);  /*  Button shortcut: white on cyan  */
 fb->nm = e_s_x_clr(15,1);  /*  Button active: white on red (action) */
 fb->hh = e_s_x_clr(12,6);  /*  Help Header		  */
 fb->hb = e_s_x_clr(11,2);  /*  Help Button		  */
 fb->hm = e_s_x_clr(10,4);  /*  Help highlighted		  */
 fb->df = e_s_x_clr(7,0);   /*  Background		  */
 fb->of = e_s_x_clr(8,0);   /*  void	(unused)	  */
 fb->db = e_s_x_clr(0,1);   /*  Breakpoint: black on Red3             */
 fb->dy = e_s_x_clr(0,10);  /*  Debugger Stop (current line): black on
                                Green.  Was Blue(12)-on-Turquoise(6), which is
                                low-contrast against the Dark Slate Blue editor
                                background; a green bar reads clearly as
                                "executing here" and stays distinct from the
                                red breakpoint bar. */
 fb->ct = e_s_x_clr(11, 4); /*  C-Prog. Text                 */
 fb->cr = e_s_x_clr(15, 4); /*  C-Prog. res. Words           */
 fb->ck = e_s_x_clr(13, 4); /*  C-Prog. Constants (bright magenta on blue) */
 fb->cp = e_s_x_clr(10, 4); /*  C-Prog. Pre-processor         */
 fb->cc = e_s_x_clr( 7, 4); /*  C-Prog. Comments               */
 /* The AI pane's roles on its blue background: the user's words stand out in
    white, the machinery steps back in the grey of a comment, a line that
    waits for a key wears a dialog's default-button green, an error is red. */
 fb->aiu = e_s_x_clr(15, 4); /*  AI pane: user, white on blue        */
 fb->aim = e_s_x_clr( 7, 4); /*  AI pane: machinery, light grey      */
 fb->aia = e_s_x_clr(15, 2); /*  AI pane: attention, white on green  */
 fb->aie = e_s_x_clr( 9, 4); /*  AI pane: error, light red           */
 /* A breakpoint the debugger has not confirmed: the breakpoint's red as the
    text colour on the editor's own background, so it reads as the outline of
    the solid red bar. */
 fb->du = e_s_x_clr( 1, 4); /*  Breakpoint not confirmed: red on blue */
 fb->dc = ' ';
 fb->ws = 7;
}

FARBE *e_ini_farbe()
{
 if (WpeIsXwin())
 {
  if (!x_fb)
   x_fb = WpeMalloc(sizeof(FARBE));
  FARBE_Init(x_fb);
  return x_fb;
 }
 else
 {
  if (!u_fb)
   u_fb = WpeMalloc(sizeof(FARBE));
  if (col_num)
  {
   FARBE_Init(u_fb);
  }
  else
  {
   u_fb->er = e_n_t_clr(0);
   u_fb->et = e_n_t_clr(0);
   u_fb->ez = e_n_t_clr(A_REVERSE);
   u_fb->es = e_n_t_clr(0);
   u_fb->ek = e_n_t_clr(A_UNDERLINE);
   u_fb->em = e_n_t_clr(A_STANDOUT);
   u_fb->mr = e_n_t_clr(A_STANDOUT);
   u_fb->mt = e_n_t_clr(A_STANDOUT);
   u_fb->mz = e_n_t_clr(0);
   u_fb->ms = e_n_t_clr(0);
   u_fb->nr = e_n_t_clr(A_STANDOUT);
   u_fb->nt = e_n_t_clr(A_REVERSE);
   u_fb->nsnt = e_n_t_clr(A_BOLD);
   u_fb->ne = e_n_t_clr(0);
   u_fb->fr = e_n_t_clr(0);
   u_fb->fa = e_n_t_clr(A_REVERSE);
   u_fb->ft = e_n_t_clr(0);
   u_fb->fz = e_n_t_clr(A_STANDOUT);
   u_fb->frft = e_n_t_clr(0);
   u_fb->fs = e_n_t_clr(0);
   u_fb->nsft = e_n_t_clr(A_BOLD);
   u_fb->fsm = e_n_t_clr(A_BOLD);
   u_fb->nz = e_n_t_clr(0);
   u_fb->ns = e_n_t_clr(A_BOLD);
   u_fb->nm = e_n_t_clr(A_BOLD);
   u_fb->hh = e_n_t_clr(A_REVERSE);
   u_fb->hb = e_n_t_clr(A_REVERSE);
   u_fb->hm = e_n_t_clr(A_BOLD);
   u_fb->of = e_n_t_clr(A_STANDOUT);
   u_fb->df = e_n_t_clr(0);
   u_fb->db = e_n_t_clr(A_STANDOUT);
   u_fb->dy = e_n_t_clr(A_STANDOUT);
   u_fb->ct = e_n_t_clr(0);
   u_fb->cr = e_n_t_clr(0);
   u_fb->ck = e_n_t_clr(0);
   u_fb->cp = e_n_t_clr(0);
   u_fb->cc = e_n_t_clr(0);
   /* No colour: the user's words bold, the machinery dim, a line awaiting a
      key in reverse, an error bold. */
   u_fb->aiu = e_n_t_clr(A_BOLD);
   u_fb->aim = e_n_t_clr(A_DIM);
   u_fb->aia = e_n_t_clr(A_REVERSE);
   u_fb->aie = e_n_t_clr(A_BOLD);
   u_fb->du = e_n_t_clr(A_UNDERLINE);   /* not confirmed: underlined */
   u_fb->dc = 0x20;
   u_fb->ws = 0;
  }
  return u_fb;
 }
}

void e_free_find(FIND *fd)
{
 if (fd->dirct)
 {
  free(fd->dirct);
  fd->dirct = NULL;
 }
}

