/* we_menus.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

/* The menus of the menu bar as data, and the builder that turns them into
   the dropdowns the engine in we_menue.c draws.  See we_menus.h.

   The table assumes the UNIX build, where the menus have always been built:
   the System menu has its six rows and the Window menu its User Screen. */

#include "messages.h"
#include "edit.h"
#include "we_menus.h"

int             e_p_show_messages(FENSTER *f);
int             e_p_show_watches(FENSTER *f);
int             e_blck_gt_beg(FENSTER *f);
int             e_blck_gt_end(FENSTER *f);
int             e_blck_mrk_line(FENSTER *f);
int             e_blck_mrk_all(FENSTER *f);
int             e_blck_mrk_word(FENSTER *f);
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

/* The key reference generator and the unit test read the rows without the
   editor: built with WPE_MENU_TABLE_ONLY, a row names no function and the
   builder is left out, so they link with no editor symbol and need no
   linker option to leave symbols unresolved. */
#ifdef WPE_MENU_TABLE_ONLY
#define MENU_FKT(fkt) NULL
#else
#define MENU_FKT(fkt) (fkt)

/* Debug > Program output: the display sets how the output is shown, see below. */
static int e_menu_program_output(FENSTER *f);
#endif

/* The bar the editor shows, and what the mouse reads of it. */
OPT opt[WPE_MENU_COUNT];
int MENOPT = 8;
int e_mn_men = 3;

/* The width of the item text of each menu: the keys end at this column. */
#define E_MENU_WIDTH_FILE   19
#define E_MENU_WIDTH_EDIT   22
#define E_MENU_WIDTH_SEARCH 23
#define E_MENU_WIDTH_BLOCK  20
#define E_MENU_WIDTH_RUN    30
#define E_MENU_WIDTH_DEBUG  28
#define E_MENU_WIDTH_WINDOW 23
#define E_MENU_WIDTH_HELP   22
#define E_MENU_WIDTH_NONE    0

/* The help line of a menu is a row of help.key after the six lines of the
   bar's own states; the row is the menu's place in the widest bar, less the
   Debug menu when the build has no debugger. */
#define E_MENU_HELP_FIRST 6
#ifdef DEBUGGER
#define E_MENU_HELP_ROW(id) (E_MENU_HELP_FIRST + (id))
#else
#define E_MENU_HELP_ROW(id) (E_MENU_HELP_FIRST + (id) - ((id) > WPE_MENU_DEBUG))
#endif

/* The help line shown while the bar itself has the keyboard. */
#define E_MENU_HELP_ROW_BAR 24

/* The columns of a menu that no bar of the build has. */
#define NO_COLUMN 0

/* The bar: the plain editor, the programming editor without the debugger,
   and with it.  The columns are what the three bars have always drawn. */
static const WpeMenuHead wpe_heads[WPE_MENU_COUNT] = {
 { "#",       '#', AltSYS, {  3,  2,  2 }, 23, -2, -3, E_MENU_WIDTH_NONE,
   E_MENU_HELP_ROW(WPE_MENU_SYSTEM) },
 { "File",    'F', AltF,   { 10,  7,  6 }, 24, -3, -3, E_MENU_WIDTH_FILE,
   E_MENU_HELP_ROW(WPE_MENU_FILE) },
 { "Edit",    'E', AltE,   { 19, 14, 12 }, 27, -3, -3, E_MENU_WIDTH_EDIT,
   E_MENU_HELP_ROW(WPE_MENU_EDIT) },
 { "Search",  'S', AltS,   { 28, 21, 18 }, 28, -3, -3, E_MENU_WIDTH_SEARCH,
   E_MENU_HELP_ROW(WPE_MENU_SEARCH) },
 { "Block",   'B', AltB,   { 39, 30, 26 }, 25, -3, -3, E_MENU_WIDTH_BLOCK,
   E_MENU_HELP_ROW(WPE_MENU_BLOCK) },
 { "Run",     'R', AltR,   { NO_COLUMN, 38, 34 }, 35, -3, -3, E_MENU_WIDTH_RUN,
   E_MENU_HELP_ROW(WPE_MENU_RUN) },
 { "Debug",   'D', AltD,   { NO_COLUMN, NO_COLUMN, 40 }, 33, -3, -3, E_MENU_WIDTH_DEBUG,
   E_MENU_HELP_ROW(WPE_MENU_DEBUG) },
 { "Project", 'P', AltP,   { NO_COLUMN, 45, 47 }, 23, -3, -3, E_MENU_WIDTH_NONE,
   E_MENU_HELP_ROW(WPE_MENU_PROJECT) },
 { "Options", 'O', AltO,   { 49, 55, 56 }, 22, -4, -4, E_MENU_WIDTH_NONE,
   E_MENU_HELP_ROW(WPE_MENU_OPTIONS) },
 { "Window",  'W', AltW,   { 61, 65, 65 }, 28, -14, -13, E_MENU_WIDTH_WINDOW,
   E_MENU_HELP_ROW(WPE_MENU_WINDOW) },
 { "Help",    'H', AltH,   { 72, 74, 74 }, 27, -22, -21, E_MENU_WIDTH_HELP,
   E_MENU_HELP_ROW(WPE_MENU_HELP) }
};

/* How many entries each bar has, and the room the mouse leaves left of a
   title (e_mn_men). */
static const int wpe_bar_count[WPE_BAR_COUNT] = { 8, 10, 11 };
static const int wpe_bar_gap[WPE_BAR_COUNT] = { 3, 2, 1 };

#define BOTH WPE_STYLES_BOTH
#define MODERN WPE_STYLE_BIT(WPE_STYLE_MODERN)
#define NO_BLOCKS 0
#define EVERYWHERE 0

/* A row with words alone, and a row that shows the keys of an action. */
#define WORDS(menu, stem, hot, letter, fkt) \
 { menu, stem, hot, letter, WPE_ACT_NONE, 0, BOTH, NO_BLOCKS, EVERYWHERE, NULL, MENU_FKT(fkt) }
#define WORDS_IF(menu, stem, hot, letter, where, fkt) \
 { menu, stem, hot, letter, WPE_ACT_NONE, 0, BOTH, NO_BLOCKS, where, NULL, MENU_FKT(fkt) }
#define BOUND(menu, stem, hot, letter, fkt, action, ctxs) \
 { menu, stem, hot, letter, WPE_ACT_##action, ctxs, BOTH, NO_BLOCKS, EVERYWHERE, NULL, MENU_FKT(fkt) }
#define BOUND_IF(menu, stem, hot, letter, fkt, action, ctxs, where) \
 { menu, stem, hot, letter, WPE_ACT_##action, ctxs, BOTH, NO_BLOCKS, where, NULL, MENU_FKT(fkt) }

/* The rows, in bar order and then in the order the dropdown shows them. */
static const WpeMenuRow wpe_rows[] = {
 /* the System menu */
 WORDS(WPE_MENU_SYSTEM, "About WE", 0, 'A', e_about_WE),
 WORDS(WPE_MENU_SYSTEM, "Clear Desktop", 0, 'C', e_clear_desk),
 WORDS(WPE_MENU_SYSTEM, "Repaint Desktop", 0, 'R', e_repaint_desk),
 WORDS(WPE_MENU_SYSTEM, "System Info", 0, 'S', e_sys_info),
 WORDS(WPE_MENU_SYSTEM, "Show Wastebasket", 5, 'W', WpeShowWastebasket),
 WORDS(WPE_MENU_SYSTEM, "Delete Wastebasket", 0, 'D', WpeDelWastebasket),
 /* File */
 BOUND(WPE_MENU_FILE, "File-Manager", 0, 'M', WpeManager, FILE_MANAGER, WPE_CTXM_MENU),
 WORDS(WPE_MENU_FILE, "New", 0, 'N', e_new),
 BOUND(WPE_MENU_FILE, "Save", 0, 'S', e_m_save, SAVE, WPE_CTXM_MENU),
 WORDS(WPE_MENU_FILE, "Save As", 5, 'A', WpeSaveAsManager),
 WORDS(WPE_MENU_FILE, "Save aLl", 6, 'L', e_saveall),
 WORDS(WPE_MENU_FILE, "Execute", 0, 'E', WpeExecuteManager),
 WORDS(WPE_MENU_FILE, "SHell", 1, 'H', WpeShell),
 WORDS(WPE_MENU_FILE, "Find", 0, 'F', WpeFindWindow),
 WORDS(WPE_MENU_FILE, "Grep", 0, 'G', WpeGrepWindow),
 WORDS(WPE_MENU_FILE, "Print File", 0, 'P', WpePrintFile),
 BOUND(WPE_MENU_FILE, "Quit", 0, 'Q', e_quit, QUIT, WPE_CTXM_MENU),
 /* Edit: Cut, Copy and Paste talk to the OS clipboard directly (we_clip.c),
    the same in the terminal and on X11 */
 BOUND(WPE_MENU_EDIT, "Cut", 2, 'T', e_edt_del, EDIT_CUT, WPE_CTXM_DIRECT),
 BOUND(WPE_MENU_EDIT, "Copy", 0, 'C', e_edt_copy, EDIT_COPY, WPE_CTXM_DIRECT),
 BOUND(WPE_MENU_EDIT, "Paste", 0, 'P', e_edt_einf, EDIT_PASTE, WPE_CTXM_DIRECT),
 BOUND(WPE_MENU_EDIT, "Show Buffer", 0, 'S', e_show_clipboard, SHOW_CLIPBOARD, WPE_CTXM_DIRECT),
 BOUND(WPE_MENU_EDIT, "Delete", 0, 'D', e_blck_del, BLOCK_DELETE, WPE_CTXM_DIRECT),
 BOUND(WPE_MENU_EDIT, "Undo", 0, 'U', e_make_undo, EDIT_UNDO, WPE_CTXM_DIRECT),
 BOUND(WPE_MENU_EDIT, "Redo", 0, 'R', e_make_redo, EDIT_REDO, WPE_CTXM_DIRECT),
 /* Search.  Go to Line is one row: its Alt-G is the assistant's key in the
    programming editor, and the key table leaves a shadowed key out of the text. */
 BOUND(WPE_MENU_SEARCH, "Find", 0, 'F', e_find, FIND, WPE_CTXM_MENU),
 BOUND(WPE_MENU_SEARCH, "Replace", 0, 'R', e_replace, REPLACE, WPE_CTXM_MENU),
 BOUND(WPE_MENU_SEARCH, "Search again", 0, 'S', e_rep_search, SEARCH_AGAIN, WPE_CTXM_MENU),
 BOUND(WPE_MENU_SEARCH, "Go to Line", 0, 'G', e_goto_line, GOTO_LINE, WPE_CTXM_MENU),
 /* Block.  The three mark rows stand together.  Mark Word is the Modern
    style's (Ctrl-D) and the Selection block mode's. */
 BOUND(WPE_MENU_BLOCK, "Begin Mark", 0, 'B', e_blck_begin, BLOCK_BEGIN, WPE_CTXM_PREFIX),
 BOUND(WPE_MENU_BLOCK, "End Mark", 0, 'E', e_blck_end, BLOCK_END, WPE_CTXM_PREFIX),
 BOUND(WPE_MENU_BLOCK, "Mark WhOle", 7, 'O', e_blck_mrk_all, BLOCK_ALL, WPE_CTXM_MENU),
 BOUND(WPE_MENU_BLOCK, "Mark Line", 5, 'L', e_blck_mrk_line, BLOCK_LINE, WPE_CTXM_MENU),
 { WPE_MENU_BLOCK, "MarK Word", 3, 'K', WPE_ACT_SELECT_WORD, WPE_CTXM_MENU,
   MODERN, WPE_BLOCKS_SELECTION, EVERYWHERE, NULL, MENU_FKT(e_blck_mrk_word) },
 BOUND(WPE_MENU_BLOCK, "Goto Begin", 0, 'G', e_blck_gt_beg, BLOCK_GOTO_BEGIN, WPE_CTXM_PREFIX),
 BOUND(WPE_MENU_BLOCK, "Goto ENd", 6, 'N', e_blck_gt_end, BLOCK_GOTO_END, WPE_CTXM_PREFIX),
 BOUND(WPE_MENU_BLOCK, "Copy", 0, 'C', e_blck_copy, BLOCK_COPY, WPE_CTXM_PREFIX),
 BOUND(WPE_MENU_BLOCK, "Move", 0, 'M', e_blck_move, BLOCK_MOVE, WPE_CTXM_PREFIX),
 BOUND(WPE_MENU_BLOCK, "Delete", 0, 'D', e_blck_del, BLOCK_DELETE, WPE_CTXM_PREFIX),
 BOUND(WPE_MENU_BLOCK, "Hide", 0, 'H', e_blck_hide, BLOCK_HIDE, WPE_CTXM_PREFIX),
 BOUND(WPE_MENU_BLOCK, "Read", 0, 'R', e_blck_read, BLOCK_READ, WPE_CTXM_PREFIX),
 BOUND(WPE_MENU_BLOCK, "Write", 0, 'W', e_blck_write, BLOCK_WRITE, WPE_CTXM_PREFIX),
 BOUND(WPE_MENU_BLOCK, "Move to RIght", 9, 'I', e_blck_to_right, BLOCK_INDENT, WPE_CTXM_PREFIX),
 BOUND(WPE_MENU_BLOCK, "Move to LefT", 11, 'T', e_blck_to_left, BLOCK_UNINDENT, WPE_CTXM_PREFIX),
 BOUND(WPE_MENU_BLOCK, "ChAnge Case", 2, 'A', e_changecase_dialog, BLOCK_CASE, WPE_CTXM_PREFIX),
 /* Run.  Only keys a terminal delivers are shown: Alt-F9, Alt-F8 and Alt-F7
    still compile and walk where they arrive (X11, Wayland, the Linux console)
    but terminal emulators keep them for themselves. */
 BOUND(WPE_MENU_RUN, "Compile", 0, 'C', e_compile, COMPILE, WPE_CTXM_MENU),
 BOUND(WPE_MENU_RUN, "Make", 0, 'M', e_p_make, MAKE, WPE_CTXM_MENU),
 BOUND(WPE_MENU_RUN, "Run", 0, 'R', e_run, RUN, WPE_CTXM_MENU),
 BOUND(WPE_MENU_RUN, "Install", 0, 'I', e_install, INSTALL, WPE_CTXM_MENU),
 BOUND(WPE_MENU_RUN, "Execute Make", 0, 'E', e_exec_make, EXEC_MAKE, WPE_CTXM_MENU),
 BOUND(WPE_MENU_RUN, "Next Error", 0, 'N', e_next_error, NEXT_ERROR, WPE_CTXM_MENU),
 BOUND(WPE_MENU_RUN, "Previous Error", 0, 'P', e_previous_error, PREV_ERROR, WPE_CTXM_MENU),
 BOUND(WPE_MENU_RUN, "Show Definition", 0, 'S', e_sh_def, SHOW_DEFINITION, WPE_CTXM_MENU),
 BOUND(WPE_MENU_RUN, "Show NeXt Definition", 7, 'X', e_sh_nxt_def, NEXT_DEFINITION, WPE_CTXM_MENU),
 BOUND(WPE_MENU_RUN, "Matching BracKet", 13, 'K', e_nxt_brk, MATCH_BRACKET, WPE_CTXM_MENU),
 BOUND(WPE_MENU_RUN, "Beautify", 0, 'B', e_p_beautify, BEAUTIFY, WPE_CTXM_MENU),
 WORDS(WPE_MENU_RUN, "Arguments                     ", 0, 'A', e_arguments),
#ifdef DEBUGGER
 /* Debug */
 BOUND(WPE_MENU_DEBUG, "Toggle Breakpoint", 7, 'B', e_breakpoint, DEBUG_BREAKPOINT, WPE_CTXM_MENU),
 BOUND(WPE_MENU_DEBUG, "ReMove all Breakp.", 2, 'M', e_remove_breakpoints, DEBUG_REMOVE_BREAKPOINTS, WPE_CTXM_MENU),
 BOUND(WPE_MENU_DEBUG, "Make Watch", 5, 'W', e_make_watches, DEBUG_WATCH, WPE_CTXM_MENU),
 BOUND(WPE_MENU_DEBUG, "Edit watch", 0, 'E', e_edit_watches_command, DEBUG_EDIT_WATCH, WPE_CTXM_MENU),
 BOUND(WPE_MENU_DEBUG, "Delete watch", 0, 'D', e_delete_watches_command, DEBUG_DELETE_WATCH, WPE_CTXM_MENU),
 BOUND(WPE_MENU_DEBUG, "Remove All watches", 7, 'A', e_remove_all_watches_command, DEBUG_REMOVE_ALL_WATCHES, WPE_CTXM_MENU),
 BOUND(WPE_MENU_DEBUG, "Show stacK", 9, 'K', e_deb_stack, DEBUG_STACK, WPE_CTXM_MENU),
 BOUND(WPE_MENU_DEBUG, "Goto cursor", 0, 'U', e_d_goto_cursor, DEBUG_GOTO_CURSOR, WPE_CTXM_MENU),
 BOUND(WPE_MENU_DEBUG, "Finish function", 0, 'F', e_d_finish_func, DEBUG_FINISH, WPE_CTXM_MENU),
 BOUND(WPE_MENU_DEBUG, "Trace", 0, 'T', e_deb_trace, DEBUG_TRACE, WPE_CTXM_MENU),
 BOUND(WPE_MENU_DEBUG, "Step", 0, 'S', e_deb_next, DEBUG_STEP, WPE_CTXM_MENU),
 BOUND(WPE_MENU_DEBUG, "Run/Continue", 0, 'R', e_deb_run, DEBUG_RUN, WPE_CTXM_MENU),
 { WPE_MENU_DEBUG, "Interrupt (pause)", 0, 'I', WPE_ACT_DEBUG_PAUSE, WPE_CTXM_MENU,
   BOTH, NO_BLOCKS, EVERYWHERE, "/", MENU_FKT(e_d_pause_command) },
 BOUND(WPE_MENU_DEBUG, "Program output", 0, 'P', e_menu_program_output, DEBUG_OUTPUT, WPE_CTXM_MENU),
 BOUND(WPE_MENU_DEBUG, "debugger Options", 9, 'O', e_deb_options, DEBUG_OPTIONS, WPE_CTXM_MENU),
 BOUND(WPE_MENU_DEBUG, "Quit", 0, 'Q', e_d_quit_entry, DEBUG_QUIT, WPE_CTXM_MENU),
#endif
 /* Project */
 WORDS(WPE_MENU_PROJECT, "New Project", 0, 'N', e_new_project),
 WORDS(WPE_MENU_PROJECT, "Open Project", 5, 'P', e_project),
 WORDS(WPE_MENU_PROJECT, "Close Project", 0, 'C', e_cl_project),
 WORDS(WPE_MENU_PROJECT, "Add Item", 0, 'A', e_p_add_item),
 WORDS(WPE_MENU_PROJECT, "Delete Item", 0, 'D', e_p_del_item),
 WORDS(WPE_MENU_PROJECT, "Options", 0, 'O', e_project_options),
 /* Options */
 WORDS(WPE_MENU_OPTIONS, "Adjust Colors", 0, 'A', e_ad_colors),
 WORDS(WPE_MENU_OPTIONS, "Save Options", 0, 'S', e_opt_save),
 WORDS(WPE_MENU_OPTIONS, "Editor", 0, 'E', e_edt_options),
 WORDS(WPE_MENU_OPTIONS, "File-Manager", 0, 'F', WpeFileManagerOptions),
 WORDS(WPE_MENU_OPTIONS, "Help", 0, 'H', e_help_options),
 WORDS_IF(WPE_MENU_OPTIONS, "ProGramming", 3, 'G', WPE_WHERE_PROG, e_program_opt),
 WORDS_IF(WPE_MENU_OPTIONS, "Compiler", 0, 'C', WPE_WHERE_PROG, e_run_options),
#ifdef DEBUGGER
 WORDS_IF(WPE_MENU_OPTIONS, "Debugger", 0, 'D', WPE_WHERE_PROG, e_deb_options),
#endif
#ifdef WPE_AI
 WORDS_IF(WPE_MENU_OPTIONS, "AI", 1, 'I', WPE_WHERE_AI, e_ai_options),
#endif
 /* Window */
 BOUND(WPE_MENU_WINDOW, "Size/Move", 0, 'S', e_size_move, SIZE_MOVE, WPE_CTXM_MENU),
 BOUND(WPE_MENU_WINDOW, "Zoom", 0, 'Z', e_ed_zoom, ZOOM, WPE_CTXM_MENU),
 BOUND(WPE_MENU_WINDOW, "Tile", 0, 'T', e_ed_tile, TILE, WPE_CTXM_MENU),
 BOUND(WPE_MENU_WINDOW, "Cascade", 1, 'A', e_ed_cascade, CASCADE, WPE_CTXM_MENU),
 BOUND(WPE_MENU_WINDOW, "Next", 2, 'X', e_ed_next, NEXT_WINDOW, WPE_CTXM_MENU),
 BOUND(WPE_MENU_WINDOW, "Close", 0, 'C', e_close_window, CLOSE_WINDOW, WPE_CTXM_MENU),
 BOUND(WPE_MENU_WINDOW, "List All", 0, 'L', e_list_all_win, LIST_WINDOWS, WPE_CTXM_MENU),
 BOUND_IF(WPE_MENU_WINDOW, "User Screen", 0, 'U', e_user_screen, USER_SCREEN, WPE_CTXM_MENU,
          WPE_WHERE_TERMINAL),
#ifdef DEBUGGER
 WORDS_IF(WPE_MENU_WINDOW, "Watches", 0, 'W', WPE_WHERE_PROG, e_p_show_watches),
#endif
 WORDS_IF(WPE_MENU_WINDOW, "Messages", 0, 'M', WPE_WHERE_PROG, e_p_show_messages),
 WORDS_IF(WPE_MENU_WINDOW, "Project", 0, 'P', WPE_WHERE_PROG, e_show_project),
 /* Help */
 BOUND(WPE_MENU_HELP, "Editor", 0, 'E', e_help, HELP_EDITOR, WPE_CTXM_MENU),
 BOUND(WPE_MENU_HELP, "Topic Search", 0, 'T', e_topic_search, TOPIC_SEARCH, WPE_CTXM_MENU),
 BOUND(WPE_MENU_HELP, "FUnction Index", 1, 'U', e_funct_in, FUNCTION_INDEX, WPE_CTXM_MENU),
 WORDS(WPE_MENU_HELP, "Info                  ", 0, 'I', e_info),
 WORDS(WPE_MENU_HELP, "Goto              <CR>", 0, 'G', e_hp_ret),
 WORDS(WPE_MENU_HELP, "Back             <BSP>", 0, 'B', e_hp_back),
 BOUND(WPE_MENU_HELP, "Next", 0, 'G', e_hp_next, HELP_NEXT, WPE_CTXM_HELP),
 BOUND(WPE_MENU_HELP, "Prev.", 0, 'P', e_hp_prev, HELP_PREV, WPE_CTXM_HELP)
};

#define WPE_ROW_COUNT (sizeof wpe_rows / sizeof wpe_rows[0])

#ifndef WPE_MENU_TABLE_ONLY
/**
 * e_menu_program_output - Debug > Program output.
 * @f: the current window.
 *
 * The display (terminal or graphical) decides how the program's output is
 * shown, and says so by setting a function pointer at start, so the row
 * cannot name it: this is the row's function.
 */
static int e_menu_program_output(FENSTER *f)
{
 return(e_u_deb_out(f));
}
#endif

const WpeMenuRow *wpe_menu_rows(size_t *n)
{
 if (n)
  *n = WPE_ROW_COUNT;
 return(wpe_rows);
}

const WpeMenuHead *wpe_menu_heads(size_t *n)
{
 if (n)
  *n = WPE_MENU_COUNT;
 return(wpe_heads);
}

int wpe_menu_row_shown(const WpeMenuRow *r, const WpeMenuFacts *f)
{
 if (!(r->styles & WPE_STYLE_BIT(f->style)) && !(r->blocks & (unsigned)f->blocks))
  return(0);
 if ((r->where & WPE_WHERE_PROG) && !f->prog)
  return(0);
 if ((r->where & WPE_WHERE_TERMINAL) && f->xwin)
  return(0);
 if ((r->where & WPE_WHERE_AI) && !f->assistant)
  return(0);
 return(1);
}

int wpe_menu_bar_composition(int prog)
{
 if (!prog)
  return(WPE_BAR_PLAIN);
#ifdef DEBUGGER
 return(WPE_BAR_PROG_DEBUG);
#else
 return(WPE_BAR_PROG);
#endif
}

int wpe_menu_head_shown(const WpeMenuHead *h, int composition)
{
 return(h->x[composition] != NO_COLUMN);
}

static int e_bar_composition = WPE_BAR_PLAIN;   /* the bar opt[] holds */

#ifndef WPE_MENU_TABLE_ONLY
/**
 * e_menu_help_line - The help line to show for a bar entry.
 * @n: the entry's place in the bar, or E_MENU_HELP_BAR for the bar itself.
 *
 * While a menu is open the status row explains it; the line belongs to the
 * menu, whatever bar the editor shows.
 */
char *e_menu_help_line(int n)
{
 extern char *e_hlp_str[];
 int i, shown = 0;

 if (n == E_MENU_HELP_BAR)
  return(e_hlp_str[E_MENU_HELP_ROW_BAR]);
 for (i = 0; i < WPE_MENU_COUNT; i++)
 {
  if (!wpe_menu_head_shown(&wpe_heads[i], e_bar_composition))
   continue;
  if (shown++ == n)
   return(e_hlp_str[wpe_heads[i].help]);
 }
 return(e_hlp_str[E_MENU_HELP_ROW_BAR]);
}
#endif

/**
 * e_menu_bar_from_heads - Lay the bar of one composition into opt[].
 * @composition: a WpeBarComposition.
 *
 * Done once at start, so the bar drawing, the mouse and the Alt letters read
 * opt[], MENOPT and e_mn_men as they always did, and nothing else writes them.
 */
void e_menu_bar_from_heads(int composition)
{
 int i, shown = 0;

 e_bar_composition = composition;
 for (i = 0; i < WPE_MENU_COUNT; i++)
 {
  const WpeMenuHead *h = &wpe_heads[i];

  if (!wpe_menu_head_shown(h, composition))
   continue;
  opt[shown].t = (char *)h->title;
  opt[shown].x = h->x[composition];
  opt[shown].s = h->letter;
  opt[shown].as = h->alt;
  shown++;
 }
 MENOPT = wpe_bar_count[composition];
 e_mn_men = wpe_bar_gap[composition];
}

#ifndef WPE_MENU_TABLE_ONLY
/**
 * e_menu_box_inset - The columns a dropdown's box leaves at each side.
 *
 * A graphical display draws the box one column narrower on each side than a
 * terminal, so the frame lines up with the bar's title.
 */
int e_menu_box_inset(void)
{
#ifdef NEWSTYLE
 if (WpeIsXwin())
  return(1);
#endif
 return(0);
}

/* The text of the items whose keys come from the key table.  An item holds a
   pointer to its text, so the texts are kept for as long as the menus are
   open; they are written again, with the same content, each time the menu
   bar opens. */
#define E_MENU_TEXT_LEN 64
#define E_MENU_TEXTS_MAX 128
static char e_menu_texts[E_MENU_TEXTS_MAX][E_MENU_TEXT_LEN];
static int e_menu_texts_used;

/**
 * e_menu_facts_of - What the menus of the editor @f are a function of.
 */
static WpeMenuFacts e_menu_facts_of(FENSTER *f)
{
 WpeMenuFacts facts;

 facts.style = wpe_binding_style(f->ed->edopt);
 facts.blocks = wpe_block_mode(f->ed->edopt);
 facts.prog = WpeIsProg() != 0;
 facts.xwin = WpeIsXwin() != 0;
#ifdef WPE_AI
 facts.assistant = 1;
#else
 facts.assistant = 0;
#endif
 return(facts);
}

/**
 * e_menu_item_from_row - The dropdown item of a row.
 * @r: the row; @h: the head of its menu; @facts: style and mode.
 *
 * A row with an action shows the keys the style binds to it, so a menu never
 * advertises a key that does something else; a row without one shows its
 * words alone.
 */
static OPTK e_menu_item_from_row(const WpeMenuRow *r, const WpeMenuHead *h,
                                 const WpeMenuFacts *facts)
{
 char *text;

 /* A menu with more items than the pool holds shows its words alone. */
 if (r->action == WPE_ACT_NONE || e_menu_texts_used >= E_MENU_TEXTS_MAX)
  return(WpeFillSubmenuItem((char *)r->stem, r->hot, r->letter, r->fkt));
 text = e_menu_texts[e_menu_texts_used++];
 wpe_menu_item_text(text, E_MENU_TEXT_LEN, r->stem, (WpeAction)r->action,
                    facts->style, r->contexts, h->item_width, r->sep, facts->prog);
 return(WpeFillSubmenuItem(text, r->hot, r->letter, r->fkt));
}

/**
 * e_menu_count_rows - How many rows the menu @id has for these facts.
 */
static int e_menu_count_rows(int id, const WpeMenuFacts *facts)
{
 size_t i;
 int n = 0;

 for (i = 0; i < WPE_ROW_COUNT; i++)
  if (wpe_rows[i].menu == id && wpe_menu_row_shown(&wpe_rows[i], facts))
   n++;
 return(n);
}

/**
 * e_menu_fill_rows - Fill the items of the menu @id from the table.
 * @items: room for e_menu_count_rows items.
 */
static void e_menu_fill_rows(int id, const WpeMenuHead *h, const WpeMenuFacts *facts,
                             OPTK *items)
{
 size_t i;
 int n = 0;

 for (i = 0; i < WPE_ROW_COUNT; i++)
  if (wpe_rows[i].menu == id && wpe_menu_row_shown(&wpe_rows[i], facts))
   items[n++] = e_menu_item_from_row(&wpe_rows[i], h, facts);
}

/**
 * e_menu_of_head - The dropdown of one entry of the bar.
 * @id: the menu; @h: its head; @facts: style and mode; @f: for a message.
 */
static MENU e_menu_of_head(int id, const WpeMenuHead *h, const WpeMenuFacts *facts,
                           FENSTER *f)
{
 MENU m;

 m.position = facts->xwin ? h->position_graphical : h->position_terminal;
 m.width = h->width;
 m.no_of_items = e_menu_count_rows(id, facts);
 m.menuitems = MALLOC(m.no_of_items * sizeof(OPTK));
 if (m.menuitems == NULL)
 {
  e_error(e_msg[ERR_LOWMEM], 1, f->fb);
  m.width = 0;
  m.no_of_items = 0;
  return(m);
 }
 e_menu_fill_rows(id, h, facts, m.menuitems);
 return(m);
}

/**
 * e_menu_build - Build the dropdowns of the bar for the editor @f.
 * @f: the current window; @count: out, the entries of the bar.
 *
 * What every dropdown shows follows the table and five facts: the key style,
 * the block mode, the editor mode, the display and whether the assistant is
 * built in.  The caller gives the menus back with e_menu_free.
 * Return: one MENU per entry of the bar, in bar order.
 */
MENU *e_menu_build(FENSTER *f, int *count)
{
 WpeMenuFacts facts = e_menu_facts_of(f);
 int composition = wpe_menu_bar_composition(facts.prog);
 MENU *menus = MALLOC(wpe_bar_count[composition] * sizeof(MENU));
 int id, shown = 0;

 e_menu_texts_used = 0;
 for (id = 0; id < WPE_MENU_COUNT; id++)
  if (wpe_menu_head_shown(&wpe_heads[id], composition))
   menus[shown++] = e_menu_of_head(id, &wpe_heads[id], &facts, f);
 *count = shown;
 return(menus);
}

/**
 * e_menu_free - Give back the dropdowns e_menu_build made.
 */
void e_menu_free(MENU *menus, int count)
{
 int i;

 for (i = 0; i < count; i++)
  if (menus[i].width != 0)
   FREE(menus[i].menuitems);
 FREE(menus);
}
#endif /* WPE_MENU_TABLE_ONLY */
