/* we_menue.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include "we_menurows.h"
#include "we_testdump.h"

int             e_p_show_messages(FENSTER *f);
int             e_p_show_watches(FENSTER *f);
int             e_blck_gt_beg(FENSTER *f);
int             e_blck_gt_end(FENSTER *f);
int             e_blck_mrk_line(FENSTER *f);
int             e_blck_mrk_all(FENSTER *f);
int             e_blck_to_left(FENSTER *f);
int             e_blck_to_right(FENSTER *f);
int             e_cl_project(FENSTER *f);
int             e_p_add_item(FENSTER *f);
int             e_p_del_item(FENSTER *f);
int             e_show_project(FENSTER *f);
int             e_info(FENSTER *f);
int             e_help_options(FENSTER *f);
int             e_hp_ret(FENSTER *f);
int             e_hp_back(FENSTER *f);
int             e_hp_prev(FENSTER *f);
int             e_hp_next(FENSTER *f);

/**
 * WpeMenuGetch - Read one key for the menu bar or a dropdown.
 *
 * A test watching the editor's state learns from here that the key it
 * sends next picks a menu item, not text.
 * Return: the key, as wpe_paint_modal_getch gives it.
 */
static int WpeMenuGetch(void)
{
  int c;

  wpe_test_ctx_enter(WPE_TEST_CTX_MENU, NULL);
  c = wpe_paint_modal_getch();
  wpe_test_ctx_leave();
  return c;
}

/**
 * WpeMainmenuReadKey - Read the next key, or click, for the menu bar.
 *
 * While the bar waits for the user it owns the screen: a stop, a build's
 * end or a reply that comes meanwhile is shown once the bar has closed, so
 * the command picked from it acts on the editor the user was looking at.
 * Return: the key; a click on the bar is turned into the key it stands for.
 */
static int WpeMainmenuReadKey(void)
{
  int c = WpeMenuGetch();

#if MOUSE
  if (c == -1)
    c = e_m1_mouse();
#endif
  return c;
}

/* The text of the items whose keys come from the key table.  An item holds a
   pointer to its text, so the texts are kept for as long as the menus are
   open; they are written again, with the same content, each time the menu
   bar opens. */
#define E_MENU_TEXT_LEN 64
#define E_MENU_TEXTS_MAX 128
static char e_menu_texts[E_MENU_TEXTS_MAX][E_MENU_TEXT_LEN];
static int e_menu_texts_used;

/* The width of the item text of each menu: the keys end at this column. */
#define E_MENU_WIDTH_FILE   19
#define E_MENU_WIDTH_EDIT   22
#define E_MENU_WIDTH_SEARCH 23
#define E_MENU_WIDTH_BLOCK  20
#define E_MENU_WIDTH_RUN    30
#define E_MENU_WIDTH_DEBUG  28
#define E_MENU_WIDTH_WINDOW 23
#define E_MENU_WIDTH_HELP   22

/**
 * WpeFillBoundItemSep - A menu item whose keys the key table names, with the
 * keys joined by @sep.
 * @style: the editor's key style; @stem: the item's words.
 * @x: the index of its hot letter; @o: the letter; @fkt: what it runs.
 * @action: what it runs, in the key table; @contexts: the contexts whose keys
 * it shows; @width: the width of the item text, the keys end at its edge.
 *
 * The item reads the keys the editor's style binds to its action, so a menu
 * never advertises a key that does something else.  An action with no key in
 * the style is shown with its words alone.
 */
static OPTK WpeFillBoundItemSep(int style, const char *stem, int x, char o,
                                int (*fkt)(), WpeAction action,
                                unsigned contexts, int width, const char *sep)
{
  char *text;

  /* A menu with more items than the pool holds shows its words alone. */
  if (e_menu_texts_used >= E_MENU_TEXTS_MAX)
    return(WpeFillSubmenuItem((char *)stem, x, o, fkt));
  text = e_menu_texts[e_menu_texts_used++];
  wpe_menu_item_text(text, E_MENU_TEXT_LEN, stem, action, style, contexts,
                     width, sep);
  return(WpeFillSubmenuItem(text, x, o, fkt));
}

/** WpeFillBoundItemSep with the usual " / " between keys. */
static OPTK WpeFillBoundItem(int style, const char *stem, int x, char o,
                             int (*fkt)(), WpeAction action,
                             unsigned contexts, int width)
{
  return(WpeFillBoundItemSep(style, stem, x, o, fkt, action, contexts, width, NULL));
}

/* main menu control bar */
int WpeHandleMainmenu(int n, FENSTER *f)
{
  extern int      e_mn_men;
  int             i, c = 255, nold = n <= 0 ? 1 : n - 1;
  extern OPT      opt[];
  extern char    *e_hlp, *e_hlp_str[];
  ECNT           *cn = f->ed;
  MENU           *mainmenu = MALLOC(MENOPT * sizeof(MENU));
  int             style = wpe_binding_style(cn->edopt);

  e_menu_texts_used = 0;
  for(i = 0; i < MENOPT; i++)
    mainmenu[i].width = 0;
  fk_cursor(0);
  if(n < 0)
    n = 0;
  else
    c = WPE_CR;
#ifdef UNIX
#ifdef NEWSTYLE
  if(WpeIsXwin())
    mainmenu[0].position = -3;
  else
#endif
    mainmenu[0].position = -2;
  mainmenu[0].no_of_items = 6;
  mainmenu[0].width = 23;
#else
  mainmenu[0].position = -3;
  mainmenu[0].no_of_items = 4;
  mainmenu[0].width = 20;
#endif
  if((mainmenu[0].menuitems = MALLOC(mainmenu[0].no_of_items * sizeof(OPTK))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, f->fb);
  mainmenu[0].menuitems[0] = WpeFillSubmenuItem("About WE", 0, 'A', e_about_WE);
  mainmenu[0].menuitems[1] = WpeFillSubmenuItem("Clear Desktop", 0, 'C', e_clear_desk);
  mainmenu[0].menuitems[2] = WpeFillSubmenuItem("Repaint Desktop", 0, 'R', e_repaint_desk);
  mainmenu[0].menuitems[3] = WpeFillSubmenuItem("System Info", 0, 'S', e_sys_info);
#ifdef UNIX
  mainmenu[0].menuitems[4] = WpeFillSubmenuItem("Show Wastebasket", 5, 'W', WpeShowWastebasket);
  mainmenu[0].menuitems[5] = WpeFillSubmenuItem("Delete Wastebasket", 0, 'D', WpeDelWastebasket);
#endif
  mainmenu[1].position = -3;
#ifdef UNIX
  mainmenu[1].width = 24;
  mainmenu[1].no_of_items = 11;
  if((mainmenu[1].menuitems = MALLOC(mainmenu[1].no_of_items * sizeof(OPTK))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, f->fb);
  mainmenu[1].menuitems[0] = WpeFillBoundItem(style, "File-Manager", 0, 'M', WpeManager,
                                              WPE_ACT_FILE_MANAGER, WPE_CTXM_MENU, E_MENU_WIDTH_FILE);
  mainmenu[1].menuitems[2] = WpeFillBoundItem(style, "Save", 0, 'S', e_m_save,
                                              WPE_ACT_SAVE, WPE_CTXM_MENU, E_MENU_WIDTH_FILE);
  mainmenu[1].menuitems[10] = WpeFillBoundItem(style, "Quit", 0, 'Q', e_quit,
                                               WPE_ACT_QUIT, WPE_CTXM_MENU, E_MENU_WIDTH_FILE);
  mainmenu[1].menuitems[1] = WpeFillSubmenuItem("New", 0, 'N', e_new);
  mainmenu[1].menuitems[3] = WpeFillSubmenuItem("Save As", 5, 'A', WpeSaveAsManager);
  mainmenu[1].menuitems[4] = WpeFillSubmenuItem("Save aLl", 6, 'L', e_saveall);
  mainmenu[1].menuitems[5] = WpeFillSubmenuItem("Execute", 0, 'E', WpeExecuteManager);
  mainmenu[1].menuitems[6] = WpeFillSubmenuItem("SHell", 1, 'H', WpeShell);
  mainmenu[1].menuitems[7] = WpeFillSubmenuItem("Find", 0, 'F', WpeFindWindow);
  mainmenu[1].menuitems[8] = WpeFillSubmenuItem("Grep", 0, 'G', WpeGrepWindow);
  mainmenu[1].menuitems[9] = WpeFillSubmenuItem("Print File", 0, 'P', WpePrintFile);
#endif
  mainmenu[2].position = -3;
  mainmenu[2].width = 27;
  /* Cut/Copy/Paste now talk to the OS clipboard directly (we_clip.c), the same
     in terminal and X11, so the old X11-only "(XBuffer)" bridge items are gone. */
  mainmenu[2].no_of_items = 7;
  if((mainmenu[2].menuitems = MALLOC(mainmenu[2].no_of_items * sizeof(OPTK))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, f->fb);
  mainmenu[2].menuitems[0] = WpeFillBoundItem(style, "Cut", 2, 'T', e_edt_del,
                                              WPE_ACT_EDIT_CUT, WPE_CTXM_DIRECT, E_MENU_WIDTH_EDIT);
  mainmenu[2].menuitems[1] = WpeFillBoundItem(style, "Copy", 0, 'C', e_edt_copy,
                                              WPE_ACT_EDIT_COPY, WPE_CTXM_DIRECT, E_MENU_WIDTH_EDIT);
  mainmenu[2].menuitems[2] = WpeFillBoundItem(style, "Paste", 0, 'P', e_edt_einf,
                                              WPE_ACT_EDIT_PASTE, WPE_CTXM_DIRECT, E_MENU_WIDTH_EDIT);
  mainmenu[2].menuitems[3] = WpeFillBoundItem(style, "Show Buffer", 0, 'S', e_show_clipboard,
                                              WPE_ACT_SHOW_CLIPBOARD, WPE_CTXM_DIRECT, E_MENU_WIDTH_EDIT);
  mainmenu[2].menuitems[4] = WpeFillBoundItem(style, "Delete", 0, 'D', e_blck_del,
                                              WPE_ACT_BLOCK_DELETE, WPE_CTXM_DIRECT, E_MENU_WIDTH_EDIT);
  mainmenu[2].menuitems[5] = WpeFillBoundItem(style, "Undo", 0, 'U', e_make_undo,
                                              WPE_ACT_EDIT_UNDO, WPE_CTXM_DIRECT, E_MENU_WIDTH_EDIT);
  mainmenu[2].menuitems[6] = WpeFillBoundItem(style, "Redo", 0, 'R', e_make_redo,
                                              WPE_ACT_EDIT_REDO, WPE_CTXM_DIRECT, E_MENU_WIDTH_EDIT);
  mainmenu[3].position = -3;
  mainmenu[3].width = 28;
  mainmenu[3].no_of_items = 4;
  if((mainmenu[3].menuitems = MALLOC(mainmenu[3].no_of_items * sizeof(OPTK))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, f->fb);
  mainmenu[3].menuitems[0] = WpeFillBoundItem(style, "Find", 0, 'F', e_find,
                                              WPE_ACT_FIND, WPE_CTXM_MENU, E_MENU_WIDTH_SEARCH);
  mainmenu[3].menuitems[1] = WpeFillBoundItem(style, "Replace", 0, 'R', e_replace,
                                              WPE_ACT_REPLACE, WPE_CTXM_MENU, E_MENU_WIDTH_SEARCH);
  mainmenu[3].menuitems[2] = WpeFillBoundItem(style, "Search again", 0, 'S', e_rep_search,
                                              WPE_ACT_SEARCH_AGAIN, WPE_CTXM_MENU, E_MENU_WIDTH_SEARCH);
  /* The programming editor gives Alt-G to the assistant, so the item must not
     offer a key that does nothing there: the accelerator is advertised only in
     the mode that honours it.  The letter still reaches this item with the menu
     open, in both modes. */
  if(WpeIsProg())
    mainmenu[3].menuitems[3] = WpeFillSubmenuItem("Go to Line", 0, 'G', e_goto_line);
  else
    mainmenu[3].menuitems[3] = WpeFillBoundItem(style, "Go to Line", 0, 'G', e_goto_line,
                                                WPE_ACT_GOTO_LINE, WPE_CTXM_MENU, E_MENU_WIDTH_SEARCH);
  mainmenu[4].position = -3;
  mainmenu[4].width = 25;
  mainmenu[4].no_of_items = 15;
  if((mainmenu[4].menuitems = MALLOC(mainmenu[4].no_of_items * sizeof(OPTK))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, f->fb);
  mainmenu[4].menuitems[0] = WpeFillBoundItem(style, "Begin Mark", 0, 'B', e_blck_begin,
                                              WPE_ACT_BLOCK_BEGIN, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
  mainmenu[4].menuitems[1] = WpeFillBoundItem(style, "End Mark", 0, 'E', e_blck_end,
                                              WPE_ACT_BLOCK_END, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
  mainmenu[4].menuitems[2] = WpeFillBoundItem(style, "Mark WhOle", 7, 'O', e_blck_mrk_all,
                                              WPE_ACT_BLOCK_ALL, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
  mainmenu[4].menuitems[3] = WpeFillBoundItem(style, "Mark Line", 5, 'L', e_blck_mrk_line,
                                              WPE_ACT_BLOCK_LINE, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
  mainmenu[4].menuitems[4] = WpeFillBoundItem(style, "Goto Begin", 0, 'G', e_blck_gt_beg,
                                              WPE_ACT_BLOCK_GOTO_BEGIN, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
  mainmenu[4].menuitems[5] = WpeFillBoundItem(style, "Goto ENd", 6, 'N', e_blck_gt_end,
                                              WPE_ACT_BLOCK_GOTO_END, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
  mainmenu[4].menuitems[6] = WpeFillBoundItem(style, "Copy", 0, 'C', e_blck_copy,
                                              WPE_ACT_BLOCK_COPY, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
  mainmenu[4].menuitems[7] = WpeFillBoundItem(style, "Move", 0, 'M', e_blck_move,
                                              WPE_ACT_BLOCK_MOVE, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
  mainmenu[4].menuitems[8] = WpeFillBoundItem(style, "Delete", 0, 'D', e_blck_del,
                                              WPE_ACT_BLOCK_DELETE, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
  mainmenu[4].menuitems[9] = WpeFillBoundItem(style, "Hide", 0, 'H', e_blck_hide,
                                              WPE_ACT_BLOCK_HIDE, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
  mainmenu[4].menuitems[10] = WpeFillBoundItem(style, "Read", 0, 'R', e_blck_read,
                                               WPE_ACT_BLOCK_READ, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
  mainmenu[4].menuitems[11] = WpeFillBoundItem(style, "Write", 0, 'W', e_blck_write,
                                               WPE_ACT_BLOCK_WRITE, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
  mainmenu[4].menuitems[12] = WpeFillBoundItem(style, "Move to RIght", 9, 'I', e_blck_to_right,
                                               WPE_ACT_BLOCK_INDENT, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
  mainmenu[4].menuitems[13] = WpeFillBoundItem(style, "Move to LefT", 11, 'T', e_blck_to_left,
                                               WPE_ACT_BLOCK_UNINDENT, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
  mainmenu[4].menuitems[14] = WpeFillBoundItem(style, "ChAnge Case", 2, 'A', e_changecase_dialog,
                                               WPE_ACT_BLOCK_CASE, WPE_CTXM_PREFIX, E_MENU_WIDTH_BLOCK);
#ifdef PROG
  if(WpeIsProg())
  {
    mainmenu[5].position = -3;
    mainmenu[5].width = 35;
    mainmenu[5].no_of_items = 12;
    if((mainmenu[5].menuitems = MALLOC(mainmenu[5].no_of_items * sizeof(OPTK))) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, f->fb);
    /* Only keys a terminal delivers are shown.  Alt-F9, Alt-F8 and Alt-F7
       still compile and walk where they arrive (X11, Wayland, the Linux
       console) but terminal emulators keep them for themselves. */
    mainmenu[5].menuitems[0] = WpeFillBoundItem(style, "Compile", 0, 'C', e_compile,
                                                WPE_ACT_COMPILE, WPE_CTXM_MENU, E_MENU_WIDTH_RUN);
    mainmenu[5].menuitems[1] = WpeFillBoundItem(style, "Make", 0, 'M', e_p_make,
                                                WPE_ACT_MAKE, WPE_CTXM_MENU, E_MENU_WIDTH_RUN);
    mainmenu[5].menuitems[2] = WpeFillBoundItem(style, "Run", 0, 'R', e_run,
                                                WPE_ACT_RUN, WPE_CTXM_MENU, E_MENU_WIDTH_RUN);
    mainmenu[5].menuitems[3] = WpeFillBoundItem(style, "Install", 0, 'I', e_install,
                                                WPE_ACT_INSTALL, WPE_CTXM_MENU, E_MENU_WIDTH_RUN);
    mainmenu[5].menuitems[4] = WpeFillBoundItem(style, "Execute Make", 0, 'E', e_exec_make,
                                                WPE_ACT_EXEC_MAKE, WPE_CTXM_MENU, E_MENU_WIDTH_RUN);
    mainmenu[5].menuitems[5] = WpeFillBoundItem(style, "Next Error", 0, 'N', e_next_error,
                                                WPE_ACT_NEXT_ERROR, WPE_CTXM_MENU, E_MENU_WIDTH_RUN);
    mainmenu[5].menuitems[6] = WpeFillBoundItem(style, "Previous Error", 0, 'P', e_previous_error,
                                                WPE_ACT_PREV_ERROR, WPE_CTXM_MENU, E_MENU_WIDTH_RUN);
    mainmenu[5].menuitems[7] = WpeFillBoundItem(style, "Show Definition", 0, 'S', e_sh_def,
                                                WPE_ACT_SHOW_DEFINITION, WPE_CTXM_MENU, E_MENU_WIDTH_RUN);
    mainmenu[5].menuitems[8] = WpeFillBoundItem(style, "Show NeXt Definition", 7, 'X', e_sh_nxt_def,
                                                WPE_ACT_NEXT_DEFINITION, WPE_CTXM_MENU, E_MENU_WIDTH_RUN);
    mainmenu[5].menuitems[9] = WpeFillBoundItem(style, "Matching BracKet", 13, 'K', e_nxt_brk,
                                                WPE_ACT_MATCH_BRACKET, WPE_CTXM_MENU, E_MENU_WIDTH_RUN);
    mainmenu[5].menuitems[10] = WpeFillBoundItem(style, "Beautify", 0, 'B', e_p_beautify,
                                                 WPE_ACT_BEAUTIFY, WPE_CTXM_MENU, E_MENU_WIDTH_RUN);
    mainmenu[5].menuitems[11] = WpeFillSubmenuItem("Arguments                     ", 0, 'A', e_arguments);

    mainmenu[MENOPT - 4].position = -3;
    mainmenu[MENOPT - 4].width = 23;
    mainmenu[MENOPT - 4].no_of_items = 6;
    if((mainmenu[MENOPT - 4].menuitems = MALLOC(mainmenu[MENOPT - 4].no_of_items * sizeof(OPTK))) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, f->fb);
    mainmenu[MENOPT - 4].menuitems[0] = WpeFillSubmenuItem("New Project", 0, 'N', e_new_project);
    mainmenu[MENOPT - 4].menuitems[1] = WpeFillSubmenuItem("Open Project", 5, 'P', e_project);
    mainmenu[MENOPT - 4].menuitems[2] = WpeFillSubmenuItem("Close Project", 0, 'C', e_cl_project);
    mainmenu[MENOPT - 4].menuitems[3] = WpeFillSubmenuItem("Add Item", 0, 'A', e_p_add_item);
    mainmenu[MENOPT - 4].menuitems[4] = WpeFillSubmenuItem("Delete Item", 0, 'D', e_p_del_item);
    mainmenu[MENOPT - 4].menuitems[5] = WpeFillSubmenuItem("Options", 0, 'O', e_project_options);
  }
#endif
#ifdef DEBUGGER
  if(WpeIsProg())
  {
    mainmenu[MENOPT - 5].position = -3;
    mainmenu[MENOPT - 5].width = 33;
    mainmenu[MENOPT - 5].no_of_items = 16;
    if((mainmenu[MENOPT - 5].menuitems = MALLOC(mainmenu[MENOPT - 5].no_of_items * sizeof(OPTK))) == NULL)
      e_error(e_msg[ERR_LOWMEM], 1, f->fb);

    mainmenu[MENOPT - 5].menuitems[0] = WpeFillBoundItem(style, "Toggle Breakpoint", 7, 'B', e_breakpoint,
                                                         WPE_ACT_DEBUG_BREAKPOINT, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
    mainmenu[MENOPT - 5].menuitems[1] = WpeFillBoundItem(style, "ReMove all Breakp.", 2, 'M', e_remove_breakpoints,
                                                         WPE_ACT_DEBUG_REMOVE_BREAKPOINTS, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
    mainmenu[MENOPT - 5].menuitems[2] = WpeFillBoundItem(style, "Make Watch", 5, 'W', e_make_watches,
                                                         WPE_ACT_DEBUG_WATCH, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
    mainmenu[MENOPT - 5].menuitems[3] = WpeFillBoundItem(style, "Edit watch", 0, 'E', e_edit_watches,
                                                         WPE_ACT_DEBUG_EDIT_WATCH, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
    mainmenu[MENOPT - 5].menuitems[4] = WpeFillBoundItem(style, "Delete watch", 0, 'D', e_delete_watches,
                                                         WPE_ACT_DEBUG_DELETE_WATCH, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
    mainmenu[MENOPT - 5].menuitems[5] = WpeFillBoundItem(style, "Remove All watches", 7, 'A', e_remove_all_watches,
                                                         WPE_ACT_DEBUG_REMOVE_ALL_WATCHES, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
    mainmenu[MENOPT - 5].menuitems[6] = WpeFillBoundItem(style, "Show stacK", 9, 'K', e_deb_stack,
                                                         WPE_ACT_DEBUG_STACK, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
    mainmenu[MENOPT - 5].menuitems[7] = WpeFillBoundItem(style, "Goto cursor", 0, 'U', e_d_goto_cursor,
                                                         WPE_ACT_DEBUG_GOTO_CURSOR, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
    mainmenu[MENOPT - 5].menuitems[8] = WpeFillBoundItem(style, "Finish function", 0, 'F', e_d_finish_func,
                                                         WPE_ACT_DEBUG_FINISH, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
    mainmenu[MENOPT - 5].menuitems[9] = WpeFillBoundItem(style, "Trace", 0, 'T', e_deb_trace,
                                                         WPE_ACT_DEBUG_TRACE, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
    mainmenu[MENOPT - 5].menuitems[10] = WpeFillBoundItem(style, "Step", 0, 'S', e_deb_next,
                                                          WPE_ACT_DEBUG_STEP, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
    mainmenu[MENOPT - 5].menuitems[11] = WpeFillBoundItem(style, "Run/Continue", 0, 'R', e_deb_run,
                                                          WPE_ACT_DEBUG_RUN, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
    mainmenu[MENOPT - 5].menuitems[12] = WpeFillBoundItemSep(style, "Interrupt (pause)", 0, 'I', e_d_pause,
                                                             WPE_ACT_DEBUG_PAUSE, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG, "/");
    mainmenu[MENOPT - 5].menuitems[13] = WpeFillBoundItem(style, "Program output", 0, 'P', e_u_deb_out,
                                                          WPE_ACT_DEBUG_OUTPUT, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
    mainmenu[MENOPT - 5].menuitems[14] = WpeFillBoundItem(style, "debugger Options", 9, 'O', e_deb_options,
                                                          WPE_ACT_DEBUG_OPTIONS, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
    mainmenu[MENOPT - 5].menuitems[15] = WpeFillBoundItem(style, "Quit", 0, 'Q', e_d_quit,
                                                          WPE_ACT_DEBUG_QUIT, WPE_CTXM_MENU, E_MENU_WIDTH_DEBUG);
  }
#endif
  mainmenu[MENOPT - 3].position = -4;
  mainmenu[MENOPT - 3].width = 22;
#ifdef PROG
  if(WpeIsProg())
#ifdef DEBUGGER
    mainmenu[MENOPT - 3].no_of_items = 8;
#else
    mainmenu[MENOPT - 3].no_of_items = 7;
#endif
  else
#endif
    mainmenu[MENOPT - 3].no_of_items = 5;
#ifdef WPE_AI
  mainmenu[MENOPT - 3].no_of_items += 1;   /* + "AI" settings entry (last) */
#endif
  if((mainmenu[MENOPT - 3].menuitems = MALLOC(mainmenu[MENOPT - 3].no_of_items * sizeof(OPTK))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, f->fb);
  mainmenu[MENOPT - 3].menuitems[0] = WpeFillSubmenuItem("Adjust Colors", 0, 'A', e_ad_colors);
  mainmenu[MENOPT - 3].menuitems[1] = WpeFillSubmenuItem("Save Options", 0, 'S', e_opt_save);
  mainmenu[MENOPT - 3].menuitems[2] = WpeFillSubmenuItem("Editor", 0, 'E', e_edt_options);
  mainmenu[MENOPT - 3].menuitems[3] = WpeFillSubmenuItem("File-Manager", 0, 'F', WpeFileManagerOptions);
  mainmenu[MENOPT - 3].menuitems[4] = WpeFillSubmenuItem("Help", 0, 'H', e_help_options);
#ifdef PROG
  if(WpeIsProg())
  {
    mainmenu[MENOPT - 3].menuitems[5] = WpeFillSubmenuItem("ProGramming", 3, 'G', e_program_opt);
    mainmenu[MENOPT - 3].menuitems[6] = WpeFillSubmenuItem("Compiler", 0, 'C', e_run_options);
#ifdef DEBUGGER
    mainmenu[MENOPT - 3].menuitems[7] = WpeFillSubmenuItem("Debugger", 0, 'D', e_deb_options);
#endif
  }
#endif
#ifdef WPE_AI
  mainmenu[MENOPT - 3].menuitems[mainmenu[MENOPT - 3].no_of_items - 1] =
    WpeFillSubmenuItem("AI", 1, 'I', e_ai_options);
#endif
#ifdef NEWSTYLE
  if(WpeIsXwin())
    mainmenu[MENOPT - 2].position = -13;
  else
#endif
    mainmenu[MENOPT - 2].position = -14;
  mainmenu[MENOPT - 2].width = 28;
#ifdef DJGPP
#ifdef PROG
  mainmenu[MENOPT - 2].no_of_items = 10;
#else
  mainmenu[MENOPT - 2].no_of_items = 8;
#endif
#else
#ifdef UNIX
#ifdef PROG
  if(WpeIsProg())
#ifdef DEBUGGER
    mainmenu[MENOPT - 2].no_of_items = !WpeIsXwin()? 11 : 10;
#else
    mainmenu[MENOPT - 2].no_of_items = !WpeIsXwin()? 10 : 9;
#endif
  else
#endif
    mainmenu[MENOPT - 2].no_of_items = !WpeIsXwin()? 8 : 7;
#else
  mainmenu[MENOPT - 2].no_of_items = !WpeIsXwin()? 7 : 6;
#endif
#endif
  if((mainmenu[MENOPT - 2].menuitems = MALLOC(mainmenu[MENOPT - 2].no_of_items * sizeof(OPTK))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, f->fb);
  mainmenu[MENOPT - 2].menuitems[0] = WpeFillBoundItem(style, "Size/Move", 0, 'S', e_size_move,
                                                       WPE_ACT_SIZE_MOVE, WPE_CTXM_MENU, E_MENU_WIDTH_WINDOW);
  mainmenu[MENOPT - 2].menuitems[1] = WpeFillBoundItem(style, "Zoom", 0, 'Z', e_ed_zoom,
                                                       WPE_ACT_ZOOM, WPE_CTXM_MENU, E_MENU_WIDTH_WINDOW);
  mainmenu[MENOPT - 2].menuitems[2] = WpeFillBoundItem(style, "Tile", 0, 'T', e_ed_tile,
                                                       WPE_ACT_TILE, WPE_CTXM_MENU, E_MENU_WIDTH_WINDOW);
  mainmenu[MENOPT - 2].menuitems[3] = WpeFillBoundItem(style, "Cascade", 1, 'A', e_ed_cascade,
                                                       WPE_ACT_CASCADE, WPE_CTXM_MENU, E_MENU_WIDTH_WINDOW);
  mainmenu[MENOPT - 2].menuitems[4] = WpeFillBoundItem(style, "Next", 2, 'X', e_ed_next,
                                                       WPE_ACT_NEXT_WINDOW, WPE_CTXM_MENU, E_MENU_WIDTH_WINDOW);
  mainmenu[MENOPT - 2].menuitems[5] = WpeFillBoundItem(style, "Close", 0, 'C', e_close_window,
                                                       WPE_ACT_CLOSE_WINDOW, WPE_CTXM_MENU, E_MENU_WIDTH_WINDOW);
  mainmenu[MENOPT - 2].menuitems[6] = WpeFillBoundItem(style, "List All", 0, 'L', e_list_all_win,
                                                       WPE_ACT_LIST_WINDOWS, WPE_CTXM_MENU, E_MENU_WIDTH_WINDOW);
#ifndef DJGPP
  if(!WpeIsXwin())
#endif
    mainmenu[MENOPT - 2].menuitems[7] = WpeFillBoundItem(style, "User Screen", 0, 'U', e_user_screen,
                                                         WPE_ACT_USER_SCREEN, WPE_CTXM_MENU, E_MENU_WIDTH_WINDOW);
#ifdef PROG
  if(WpeIsProg())
  {
    mainmenu[MENOPT - 2].menuitems[mainmenu[MENOPT - 2].no_of_items - 2] = 
      WpeFillSubmenuItem("Messages", 0, 'M', e_p_show_messages);
    mainmenu[MENOPT - 2].menuitems[mainmenu[MENOPT - 2].no_of_items - 1] = 
      WpeFillSubmenuItem("Project", 0, 'P', e_show_project);
#ifdef DEBUGGER
    mainmenu[MENOPT - 2].menuitems[mainmenu[MENOPT - 2].no_of_items - 3] = 
      WpeFillSubmenuItem("Watches", 0, 'W', e_p_show_watches);
#endif
  }
#endif
#ifdef NEWSTYLE
  if(WpeIsXwin())
    mainmenu[MENOPT - 1].position = -21;
  else
#endif
    mainmenu[MENOPT - 1].position = -22;
  mainmenu[MENOPT - 1].width = 27;
#if defined(PROG) && !defined(DJGPP)
  mainmenu[MENOPT - 1].no_of_items = 8;
#else
  mainmenu[MENOPT - 1].no_of_items = 6;
#endif
  if((mainmenu[MENOPT - 1].menuitems = MALLOC(mainmenu[MENOPT - 1].no_of_items * sizeof(OPTK))) == NULL)
    e_error(e_msg[ERR_LOWMEM], 1, f->fb);
  mainmenu[MENOPT - 1].menuitems[0] = WpeFillBoundItem(style, "Editor", 0, 'E', e_help,
                                                       WPE_ACT_HELP_EDITOR, WPE_CTXM_MENU, E_MENU_WIDTH_HELP);
#if defined(PROG) && !defined(DJGPP)
  mainmenu[MENOPT - 1].menuitems[1] = WpeFillBoundItem(style, "Topic Search", 0, 'T', e_topic_search,
                                                       WPE_ACT_TOPIC_SEARCH, WPE_CTXM_MENU, E_MENU_WIDTH_HELP);
  mainmenu[MENOPT - 1].menuitems[2] = WpeFillBoundItem(style, "FUnction Index", 1, 'U', e_funct_in,
                                                       WPE_ACT_FUNCTION_INDEX, WPE_CTXM_MENU, E_MENU_WIDTH_HELP);
/*   mainmenu[MENOPT-1].menuitems[2] = WpeFillSubmenuItem("Functions      Alt F1", 0, 'F', e_funct); */
#endif
  mainmenu[MENOPT - 1].menuitems[mainmenu[MENOPT - 1].no_of_items - 5] = 
    WpeFillSubmenuItem("Info                  ", 0, 'I', e_info);
  mainmenu[MENOPT - 1].menuitems[mainmenu[MENOPT - 1].no_of_items - 4] = 
    WpeFillSubmenuItem("Goto              <CR>", 0, 'G', e_hp_ret);
  mainmenu[MENOPT - 1].menuitems[mainmenu[MENOPT - 1].no_of_items - 3] = 
    WpeFillSubmenuItem("Back             <BSP>", 0, 'B', e_hp_back);
  mainmenu[MENOPT - 1].menuitems[mainmenu[MENOPT - 1].no_of_items - 2] = 
    WpeFillBoundItem(style, "Next", 0, 'G', e_hp_next,
                     WPE_ACT_HELP_NEXT, WPE_CTXM_HELP, E_MENU_WIDTH_HELP);
  mainmenu[MENOPT - 1].menuitems[mainmenu[MENOPT - 1].no_of_items - 1] =
    WpeFillBoundItem(style, "Prev.", 0, 'P', e_hp_prev,
                     WPE_ACT_HELP_PREV, WPE_CTXM_HELP, E_MENU_WIDTH_HELP);

  /* check for valid n */
  if(n < 0 || n > MENOPT - 1)
    n = 0;

  /* go until the user leaves the main control bar */
  while(c != WPE_ESC)
  {

    f = cn->f[cn->mxedt];
    if(e_tst_dfkt(f, c) == 0
#ifdef PROG
       || ((WpeIsProg()) && e_prog_switch(f, c) == 0)
#endif
      )
    {
      c = 0;
      break;
    }

    /* check for a menu shortcut */
    for(i = 0; i < MENOPT; i++)
      if(c == opt[i].s || c == opt[i].as)
      {
        n = i;
        c = WPE_CR;
      }

    /* if the selection is not the same as before do some drawing */
    if(nold != n)
    {
      /* paint unselected the option */
      e_pr_str_wsd(opt[nold].x, 0, opt[nold].t, f->fb->mt.fb, 0, 1,
                   f->fb->ms.fb, (nold == 0 ? 0 : opt[nold].x - e_mn_men),
       (nold == MENOPT - 1) ? MAXSCOL - 1 : opt[nold + 1].x - e_mn_men - 1);

      /* paint selected the option */
      e_pr_str_wsd(opt[n].x, 0, opt[n].t, f->fb->mz.fb, 0, 1,
                   f->fb->mz.fb, (n == 0 ? 0 : opt[n].x - e_mn_men),
             (n == MENOPT - 1) ? MAXSCOL - 1 : opt[n + 1].x - e_mn_men - 1);

      /* store the selected menu option for later use */
      nold = n;
    }

    if(c == WPE_CR)
#ifdef PROG
#ifdef DEBUGGER
    {
      if(!WpeIsProg() && n > 4)
        e_hlp = e_hlp_str[9 + n];
      else
        e_hlp = e_hlp_str[6 + n];
#else
    {
      if(!WpeIsProg() && n > 4)
        e_hlp = e_hlp_str[8 + n];
      else
        e_hlp = e_hlp_str[6 + n];
#endif
#else
    {
      e_hlp = e_hlp_str[6 + n];
#endif
      /* Handle user interaction with the submenu.
         If the user moved to another main option, the function returns. */
      if(mainmenu[n].width != 0)
        c = WpeHandleSubmenu(opt[n].x + mainmenu[n].position, 1,
                             opt[n].x + mainmenu[n].width + mainmenu[n].position, 
                             mainmenu[n].no_of_items + 2, n, mainmenu[n].menuitems, f);
      if(c < MENOPT)
      {
        n = c;
        c = WPE_CR;
      }
      else if(c != WPE_ESC)
        c = 0;
    }
    else
    {
      e_hlp = e_hlp_str[24];
      c = e_toupper(WpeMainmenuReadKey());
    }
    if(c == CDO || c == CtrlN)        /* down -> submenu open */
      c = WPE_CR;
    else if(c == CLE || c == CtrlB)   /* going left in the main menu */
      n--;
    else if(c == CRI || c == CtrlF)   /* going right in the main menu */
      n++;
    else if(c == POS1 || c == CtrlA)  /* most left in the main menu */
      n = 0;
    else if(c == ENDE || c == CtrlE)  /* most right in the main menu */
      n = MENOPT - 1;
    else if(c == AltX)                /* quit the whole business */
      c = e_quit(f);

    /* adjust the selected main menu */
    if(n < 0)
      n = MENOPT - 1;
    else if(n >= MENOPT)
      n = 0;
  }

  /* paint unselected the last selected main menu option */
  f = cn->f[cn->mxedt];
  e_pr_str_wsd(opt[nold].x, 0, opt[nold].t, f->fb->mt.fb, 0, 1,
               f->fb->ms.fb, (nold == 0 ? 0 : opt[nold].x - e_mn_men),
       (nold == MENOPT - 1) ? MAXSCOL - 1 : opt[nold + 1].x - e_mn_men - 1);

  /* free up the submenu structure */
  for(i = 0; i < MENOPT; i++)
    if(mainmenu[i].width != 0)
      FREE(mainmenu[i].menuitems);
  FREE(mainmenu);

  fk_cursor(1);
  return(c);
}

/* A row whose hotkey is this is a divider: a blank or dashed line that
   separates two groups of rows and runs nothing. */
#define WPE_SUBMENU_NO_HOTKEY 0

/* What e_m2_mouse returns for a click on a divider row (that row's hotkey). */
#define WPE_SUBMENU_NO_KEY 0

/**
 * WpeSubmenuHangsFromBar - Does this dropdown belong to a menu-bar entry?
 * @nm: the bar index the dropdown was opened with.
 *
 * The top File/Edit/... menus do: Left and Right move to the neighbouring
 * menu and the Alt form of a row letter is the row's advertised shortcut
 * ("Go to Line  Alt G").  The assistant's Alt-G dropdown does not, and the
 * engine must not borrow either behaviour for it.
 */
static int WpeSubmenuHangsFromBar(int nm)
{
  return nm != WPE_SUBMENU_NO_BAR_MENU;
}

/**
 * WpeSubmenuRowSelectable - Can the highlight rest on row @i?
 * @fopt: the rows; @i: a row index.
 *
 * A divider is drawn like a row but is not one the user can pick, so the
 * arrow keys step over it and Enter and the mouse never run it.
 */
static int WpeSubmenuRowSelectable(OPTK *fopt, int i)
{
  return fopt[i].o != WPE_SUBMENU_NO_HOTKEY;
}

/* The stepping rule itself lives in we_menurows.c, where it is unit tested;
   this adapter answers its question from the engine's own rows. */
static int WpeSubmenuPick(const void *rows, int i)
{
  return WpeSubmenuRowSelectable((OPTK *)rows, i);
}

/**
 * WpeSubmenuStepRow - The next selectable row from @from in direction @step.
 * @fopt: the rows; @count: how many; @from: the current row; @step: +1 or -1.
 *
 * Wraps at both ends and steps over a divider (wpe_menu_step_row).  With no
 * selectable row at all the highlight stays where it is.
 */
static int WpeSubmenuStepRow(OPTK *fopt, int count, int from, int step)
{
  return wpe_menu_step_row(fopt, count, from, step, WpeSubmenuPick);
}

/**
 * WpeSubmenuFirstRow - The first selectable row (where Home and the open go).
 * @fopt: the rows; @count: how many.
 */
static int WpeSubmenuFirstRow(OPTK *fopt, int count)
{
  return wpe_menu_first_row(fopt, count, WpeSubmenuPick);
}

/**
 * WpeSubmenuLastRow - The last selectable row (where End goes).
 * @fopt: the rows; @count: how many.
 */
static int WpeSubmenuLastRow(OPTK *fopt, int count)
{
  return wpe_menu_last_row(fopt, count, WpeSubmenuPick);
}

/**
 * WpeSubmenuAltFormOf - The Alt-letter key a row's hotkey stands for.
 * @hk: the row's hotkey letter.
 * Return: the Alt code (AltG for 'g' or 'G'), or 0 when the hotkey is not a letter.
 */
static int WpeSubmenuAltFormOf(int hk)
{
  if(hk >= 'A' && hk <= 'Z')
    return e_tast_sim(hk - 'A' + 'a');
  if(hk >= 'a' && hk <= 'z')
    return e_tast_sim(hk);
  return 0;
}

/**
 * WpeSubmenuRowAltShortcut - The Alt form of a row letter, when it is a shortcut here.
 * @nm: the bar index the dropdown was opened with; @hk: the row's hotkey.
 *
 * A bar menu's row advertises its Alt form ("Go to Line  Alt G"), so Alt-G
 * runs it with the menu open.  A dropdown with no bar menu advertises a
 * prefix and a letter ("Alt-G g"), so its Alt form is no shortcut: 0 here.
 */
static int WpeSubmenuRowAltShortcut(int nm, int hk)
{
  if(WpeSubmenuHangsFromBar(nm))
    return WpeSubmenuAltFormOf(hk);
  return 0;
}

/**
 * WpeSubmenuKeyIsRowAltForm - Is @c the Alt form of one of this dropdown's letters?
 * @fopt: the rows; @count: how many; @c: the key read.
 *
 * Used for a dropdown with no bar menu: such a key (Alt-G while the Alt-G
 * menu is open, Alt-A, Alt-C, ...) is neither a row's shortcut nor an
 * editor command the user meant, so the dropdown keeps the keyboard and
 * stays as it is instead of running the prefix handler a second time.
 */
static int WpeSubmenuKeyIsRowAltForm(OPTK *fopt, int count, int c)
{
  int i;
  for(i = 0; i < count; i++)
    if(WpeSubmenuRowSelectable(fopt, i) && c == WpeSubmenuAltFormOf(fopt[i].o))
      return 1;
  return 0;
}

/**
 * WpeSubmenuKeyIsIgnored - Should the dropdown stay as it is on key @c?
 * @nm: the bar index the dropdown was opened with; @prefix: the key that
 * opened it (0 when none); @fopt, @count: the rows; @c: the key read.
 *
 * True for a click on a divider (it carries no key), and, in a dropdown with
 * no bar menu, for the prefix pressed again (Alt-Q with the Alt-Q menu open)
 * and for the Alt form of a row letter.  The user sees the dropdown
 * unchanged, the highlight where it was.
 */
static int WpeSubmenuKeyIsIgnored(int nm, int prefix, OPTK *fopt, int count, int c)
{
  if(c == WPE_SUBMENU_NO_KEY)
    return 1;
  if(WpeSubmenuHangsFromBar(nm))
    return 0;
  if(prefix && c == prefix)
    return 1;                   /* the prefix that opened this dropdown, again */
  return WpeSubmenuKeyIsRowAltForm(fopt, count, c);
}

/* What WpeSubmenuOpenBox highlights when the dropdown opens: no row yet,
   the loop draws the first highlight itself. */
#define WPE_SUBMENU_NO_HIGHLIGHT (-1)

/**
 * WpeSubmenuOpenBox - Draw the dropdown over the desk.
 * @xa, @ya, @xe, @ye: the box; @n: the row drawn highlighted, or
 * WPE_SUBMENU_NO_HIGHLIGHT; @fopt: the rows; @f: the current window.
 *
 * Saves what is behind the box first, so closing it gives the user back
 * the windows as they were.  Used when the dropdown opens, when a key it
 * passed on to the editor has run, and after a resize.
 * Return: the saved background, or NULL when there is no memory for it.
 */
static PIC *WpeSubmenuOpenBox(int xa, int ya, int xe, int ye, int n,
                              OPTK * fopt, FENSTER * f)
{
  PIC *pic;
  int  i;

#ifdef NEWSTYLE
  if(WpeIsXwin())
    pic = e_open_view(xa + 1, ya, xe - 1, ye, f->fb->mt.fb, 1);
  else
#endif
    pic = e_open_view(xa, ya, xe, ye, f->fb->mt.fb, 1);
  if(pic == NULL)
    return(NULL);
  e_std_rahmen(xa + 1, ya, xe - 1, ye, NULL, 0, f->fb->mr.fb, 0);
  for(i = ya + 1; i < ye; i++)
    e_pr_str_scan(xa + 3, i, fopt[i - ya - 1].t, f->fb->mt.fb, fopt[i - ya - 1].x, 1,
                  f->fb->ms.fb, xa + 2, xe - 2);
  if(n != WPE_SUBMENU_NO_HIGHLIGHT)
    e_pr_str_scan(xa + 3, n + ya + 1, fopt[n].t, f->fb->mz.fb, fopt[n].x, 1,
                  f->fb->mz.fb, xa + 2, xe - 2);
  return(pic);
}

/**
 * WpeSubmenuFits - Does the dropdown still fit on the screen?
 * @xe, @ye: its right and bottom edges.
 *
 * After the terminal or the window shrank, a dropdown hanging from the bar
 * may reach past the status line; it is then closed rather than drawn
 * over cells the screen no longer has.
 */
static int WpeSubmenuFits(int xe, int ye)
{
  return(xe < MAXSCOL && ye < MAXSLNS - 1);
}

/**
 * WpeSubmenuAfterResize - Lay the dropdown out again for the resized screen.
 * @pic: the background saved before the resize; the other arguments as
 * WpeSubmenuOpenBox.
 *
 * The display has drawn the desk again at the new size before the dropdown
 * hears of the resize, so the cells saved behind the box are the old
 * screen's: they are dropped, never put back, and the box is drawn again
 * over the new desk with the highlight where it was.
 * Return: the new background, or NULL when the dropdown no longer fits (it
 * is then closed).
 */
static PIC *WpeSubmenuAfterResize(PIC *pic, int xa, int ya, int xe, int ye,
                                  int n, OPTK * fopt, FENSTER * f)
{
  e_close_view(pic, 0);
  if(!WpeSubmenuFits(xe, ye))
    return(NULL);
  return(WpeSubmenuOpenBox(xa, ya, xe, ye, n, fopt, f));
}

/* What the dropdown returns when it closed without a choice (Esc). */
#define WPE_SUBMENU_CLOSED 255

/* sub menu box; @prefix is the key that opened a dropdown hanging from no
   bar menu (0 for the bar menus) */
static int WpeHandleSubmenu_run(int xa, int ya, int xe, int ye, int nm, int prefix,
                                OPTK * fopt, FENSTER * f)
{
#if MOUSE
  extern struct mouse e_mouse;
  extern int      e_mn_men;
#endif
  PIC            *pic;
  int             count = ye - ya - 1;      /* rows in the box, dividers included */
  int             i, n, nold = 1, c = 0;
  extern OPT      opt[];

  /* the highlight opens on the first row the user can pick */
  n = WpeSubmenuFirstRow(fopt, count);
  if(nold == n)
    nold = n + 1;

  /* save whatever will be behind the submenu, then draw the box with no
     row highlighted yet */
  pic = WpeSubmenuOpenBox(xa, ya, xe, ye, WPE_SUBMENU_NO_HIGHLIGHT, fopt, f);
  if(pic == NULL)
  {
    e_error(e_msg[ERR_LOWMEM], 0, f->fb);
    return(WPE_ESC);
  }

#if MOUSE
  /* show the actual changes */
  e_refresh();
  /* until mouse button released */
  while(e_mshit() != 0)
  {
    c = -1;
    /* mouse is in the range of the submenu */
    if(e_mouse.y > ya && e_mouse.y < ye)
    {
      i = e_mouse.y - ya - 1;
      /* a divider under the pointer is not a row: the highlight stays where
         it is (nold may still be the open's "nothing drawn yet" marker) */
      if(WpeSubmenuRowSelectable(fopt, i))
        n = i;
      /* if a new item is selected */
      if(nold != n)
      {
        /* check whether nold is in the range of the available items and unselect it */
        if(nold < ye - ya - 1 && nold >= 0)
          e_pr_str_scan(xa + 3, nold + ya + 1, fopt[nold].t, f->fb->mt.fb, fopt[nold].x,
                        1, f->fb->ms.fb, xa + 2, xe - 2);
        /* draw the newly selected item */
        e_pr_str_scan(xa + 3, n + ya + 1, fopt[n].t, f->fb->mz.fb, fopt[n].x, 1,
                      f->fb->mz.fb, xa + 2, xe - 2);
        /* save the selection */
        nold = n;
        e_refresh();
      }
    }
    /* mouse is in the main menu area */
    else if(e_mouse.y == 0)
    {
      /* search for the selected main menu option */
      for(i = 1; i < MENOPT; i++)
        if(e_mouse.x < opt[i].x - e_mn_men)
          break;
      /* check whether a new menu option has been selected */
      if(i != nm + 1)
      {
        /* if yes, restore what was behind */
        e_close_view(pic, 1);
        return(i - 1);
      }
    }
  }

  /* there was a selection and mouse is within the submenu !!! */
  if(c < 0 && e_mouse.y > ya && e_mouse.y < ye && e_mouse.x > xa &&
     e_mouse.x < xe)
    c = MBKEY;
#endif

  while(c != WPE_ESC)
  {
    if(nold != n)
    {
      /* check whether nold is in the range of the available items and unselect it */
      if(nold < ye - ya - 1 && nold >= 0)
        e_pr_str_scan(xa + 3, nold + ya + 1, fopt[nold].t, f->fb->mt.fb, fopt[nold].x,
                      1, f->fb->ms.fb, xa + 2, xe - 2);
      /* draw the newly selected item */
      e_pr_str_scan(xa + 3, n + ya + 1, fopt[n].t, f->fb->mz.fb, fopt[n].x, 1,
                    f->fb->mz.fb, xa + 2, xe - 2);
      /* save the selection */
      nold = n;
    }
#if MOUSE
    if(c != MBKEY)
    {
      if((c = e_toupper(WpeMenuGetch())) == -1)
        c = e_m2_mouse(xa, ya, xe, ye, fopt);
    }
    else
      c = WPE_CR;  /* mouse released at a proper place, submenu item accepted */
#else
    c = e_toupper(WpeMenuGetch());
#endif

    if(c == WPE_RESIZE)
    {
      pic = WpeSubmenuAfterResize(pic, xa, ya, xe, ye, n, fopt, f);
      if(pic == NULL)
        return(WPE_SUBMENU_CLOSED);
      nold = n;
      continue;
    }

    /* check main menu shortcut keys */
    for(i = 0; i < MENOPT; i++)
      if(c == opt[i].as)
      {
        e_close_view(pic, 1);
        return(i);
      }

    /* check submenu items' shortcut keys.  Match the plain hotkey letter
       (e.g. 'g' for "Go to Line") and, in a bar menu, its Alt-<letter> form,
       so the shortcut the item advertises (e.g. "Go to Line  Alt G") works
       with the menu open, not only the bare letter.  A dropdown with no bar
       menu advertises a prefix and a letter instead, so there the Alt form
       is no shortcut (WpeSubmenuRowAltShortcut returns 0 for it). */
    for(i = 0; i < count; i++)
    {
      int hk = fopt[i].o;
      int alt = WpeSubmenuRowAltShortcut(nm, hk);
      if(!WpeSubmenuRowSelectable(fopt, i))
        continue;
      if(c == hk || (alt && c == alt))
      {
        e_close_view(pic, 1);
        fopt[i].fkt(f);
        return(WPE_ESC);
      }
    }

    if(c == Alt0)
      c = WPE_ESC;

    if(i > ye - ya)
      c = WPE_ESC;
    else if(c == WPE_CR)     /* submenu item accepted */
    {
      if(!WpeSubmenuRowSelectable(fopt, n))
        continue;                       /* a divider runs nothing */
      e_close_view(pic, 1);
      fopt[n].fkt(f);
      return(WPE_ESC);
    }
    else if(c == CUP || c == CtrlP)     /* going up in the submenu */
      n = WpeSubmenuStepRow(fopt, count, n, -1);
    else if(c == CDO || c == CtrlN)     /* going down in the submenu */
      n = WpeSubmenuStepRow(fopt, count, n, 1);
    else if(c == CLE || c == CtrlB)     /* going left in the main menu */
    {
      if(!WpeSubmenuHangsFromBar(nm))
        continue;                       /* no neighbouring menu: stay */
      c = nm > 0 ? nm - 1 : MENOPT - 1;
      break;
    }
    else if(c == CRI || c == CtrlF)     /* going right in the main menu */
    {
      if(!WpeSubmenuHangsFromBar(nm))
        continue;                       /* no neighbouring menu: stay */
      c = nm < MENOPT - 1 ? nm + 1 : 0;
      break;
    }
    else if(c == POS1 || c == CtrlA)    /* to the top in the submenu */
      n = WpeSubmenuFirstRow(fopt, count);
    else if(c == ENDE || c == CtrlE)    /* to the bottom in the submenu */
      n = WpeSubmenuLastRow(fopt, count);
    else if(WpeSubmenuKeyIsIgnored(nm, prefix, fopt, count, c))
      continue;                         /* the dropdown stays as it is */
    else
    {
      /* this is the anything else case, ESC, keys which are not listed in
         the submenu */
      e_close_view(pic, 1);
      /* Fall back to the SAME global-shortcut dispatch the editor uses, so a
         Run/Compile/Debug shortcut (Alt-U Run, Alt-M Make, Ctrl-F9, ...) works
         with the menu open too -- e_tst_dfkt alone does not know the prog
         shortcuts (those live in e_prog_switch).  Mirrors we_edit.c. */
      if(c != WPE_ESC && (e_tst_dfkt(f, c) == 0
#ifdef PROG
                          || ((WpeIsProg()) && e_prog_switch(f, c) == 0)
#endif
                         ))
        return(WPE_ESC);
      /* the key was not the editor's either: the dropdown comes back */
      pic = WpeSubmenuOpenBox(xa, ya, xe, ye, n, fopt, f);
      if(pic == NULL)
        return(WPE_SUBMENU_CLOSED);
    }
  }
  e_close_view(pic, 1);
  return(c == WPE_ESC ? WPE_SUBMENU_CLOSED : c);
}

/* WpeHandleSubmenu - public entry to the dropdown engine (top File/Edit menus and
   the Alt-Q language-server menu).  The dropdown owns the screen for its whole
   lifetime, so output, a stop or a server update arriving while a menu is open
   waits for it to close instead of drawing under the box.  The box pushes no
   window, so it tells the screen's owner (we_paint.c) instead. */
int WpeHandleSubmenu(int xa, int ya, int xe, int ye, int nm, OPTK * fopt, FENSTER * f)
{
  int ret;
  wpe_paint_modal_enter("a dropdown");
  ret = WpeHandleSubmenu_run(xa, ya, xe, ye, nm, 0, fopt, f);
  wpe_paint_modal_leave();
  return(ret);
}

/**
 * WpeHandleSubmenuFromPrefix - Run a dropdown opened by a key prefix.
 * @xa, @ya, @xe, @ye: the box; @prefix: the key that opened it (AltG, AltQ);
 * @fopt: the rows; @f: the current window.
 * Return: a menu-bar index when the user pressed that menu's Alt letter
 * (the caller then opens that menu), anything else when a row ran or the
 * box was dismissed.
 *
 * Used by the assistant's Alt-G menu and the language server's Alt-Q menu.
 * Their rows advertise "<prefix> <letter>", so pressing the prefix again,
 * or the Alt form of a row letter, leaves the dropdown as it is, and Left
 * and Right have no neighbouring menu to move to.
 */
int WpeHandleSubmenuFromPrefix(int xa, int ya, int xe, int ye, int prefix,
                               OPTK * fopt, FENSTER * f)
{
  int ret;
  wpe_paint_modal_enter("a prefix's dropdown");
  ret = WpeHandleSubmenu_run(xa, ya, xe, ye, WPE_SUBMENU_NO_BAR_MENU, prefix, fopt, f);
  wpe_paint_modal_leave();
  return(ret);
}

/*      fill "options" struct */
OPTK WpeFillSubmenuItem(char *t, int x, char o, int(*fkt) ())
{
  OPTK            opt;

  opt.t = t;
  opt.x = x;
  opt.o = o;
  opt.fkt = fkt;
  return(opt);
}

