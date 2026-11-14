/* we_bindings.c                                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

/* The key bindings of both key styles, as data.  See we_bindings.h. */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "keys.h"
#include "unixkeys.h"
#include "options.h"
#include "we_bindings.h"

#define BO WPE_STYLE_BIT(WPE_STYLE_BORLAND)
#define MO WPE_STYLE_BIT(WPE_STYLE_MODERN)
#define BOTH WPE_STYLES_BOTH
#define B WPE_ROW_BAR
#define M WPE_ROW_MENU
#define H WPE_ROW_HIDDEN
#define CT WPE_ROW_CTRL
#define NOS WPE_ROW_NO_SESSION
#define INS WPE_ROW_IN_SESSION

#define RANGE(act, lo, hi, st, ctx, fl, nm) \
 { WPE_ACT_##act, (lo), (hi), (st), (ctx), (fl), (nm), NULL }
#define KEY(act, code, st, ctx, fl, nm) RANGE(act, code, code, st, ctx, fl, nm)
/* A key with the words to give where a terminal keeps it from the editor. */
#define KEYF(act, code, st, ctx, fl, nm, fb) \
 { WPE_ACT_##act, (code), (code), (st), (ctx), (fl), (nm), (fb) }
#define LETTER(act, ch, ctx, fl) KEY(act, ch, BOTH, ctx, fl, NULL)
/* Ctrl-Shift-letter, a key of its own only under a keyboard protocol. */
#define CS(ch) (WPE_CS_FIRST + ((ch) - 'A'))

/* The rows are in the order the reference prints them, and within one action
   in the order a menu names the keys.  The order of the contexts is the order
   the editor consults them. */
static const WpeBinding wpe_bindings[] = {
 /* ---- WPE_CTX_WINDOW: any window ---- */
 RANGE(SWITCH_WINDOW, Alt1, Alt9, BOTH, WPE_CTX_WINDOW, 0, "Alt-1..9"),
 RANGE(SWITCH_WINDOW, 1024, 1049, BOTH, WPE_CTX_WINDOW, 0, "Alt-Shift-A..Z"),
 KEY(HELP_EDITOR, F1, BOTH, WPE_CTX_WINDOW, B | M, "F1"),
 KEY(HELP_EDITOR, HELP, BOTH, WPE_CTX_WINDOW, 0, "Sun Help"),
 KEY(TOPIC_SEARCH, CF1, BOTH, WPE_CTX_WINDOW, M, "Ctrl-F1"),
 KEY(FUNCTION_INDEX, AF1, BOTH, WPE_CTX_WINDOW, M, "Alt-F1"),
 KEY(ESCAPE, WPE_ESC, BOTH, WPE_CTX_WINDOW, 0, "Esc"),
 KEY(MENU_BAR, F10, BO, WPE_CTX_WINDOW, 0, "F10"),
 KEYF(MENU_BAR, F10, MO, WPE_CTX_WINDOW, NOS, "F10", "Alt-letter opens a menu; Alt-Space"),
 KEY(MENU_BAR, AltBl, BOTH, WPE_CTX_WINDOW, 0, "Alt-Space"),
 KEY(LIST_WINDOWS, Alt0, BOTH, WPE_CTX_WINDOW, M, "Alt-0"),
 KEY(USER_SCREEN, AF5, BOTH, WPE_CTX_WINDOW, M, "Alt-F5"),
 /* the Modern style */
 KEY(SIZE_MOVE, AF2, MO, WPE_CTX_WINDOW, M, "Alt-F2"),
 KEY(FILE_MANAGER, CtrlO, MO, WPE_CTX_WINDOW, B | M, "Ctrl-O"),
 KEY(FILE_MANAGER, OPEN, MO, WPE_CTX_WINDOW, 0, "Sun Open"),
 KEY(CLOSE_WINDOW, CtrlW, MO, WPE_CTX_WINDOW, B | M, "Ctrl-W"),
 KEY(CLOSE_WINDOW, CF4, MO, WPE_CTX_WINDOW, M, "Ctrl-F4"),
 KEY(CASCADE, FRONT, MO, WPE_CTX_WINDOW, 0, "Sun Front"),
 KEY(NEXT_WINDOW, F6, MO, WPE_CTX_WINDOW, M, "F6"),
 KEY(NEXT_WINDOW, AltN, MO, WPE_CTX_WINDOW, M, "Alt-N"),
 KEYF(NEXT_WINDOW, WPE_CTRL_TAB, MO, WPE_CTX_WINDOW, 0, "Ctrl-Tab", "F6 or Alt-N"),
 KEY(QUIT, STOP, MO, WPE_CTX_WINDOW, 0, "Sun Stop"),
 KEY(QUIT, CtrlQ, MO, WPE_CTX_WINDOW, B | M, "Ctrl-Q"),
 KEY(QUIT, AltX, MO, WPE_CTX_WINDOW, M, "Alt-X"),
 KEYF(QUIT, AF4, MO, WPE_CTX_WINDOW, 0, "Alt-F4", "Ctrl-Q; the window manager keeps Alt-F4"),
 /* the Borland style */
 KEY(SIZE_MOVE, AF2, BO, WPE_CTX_WINDOW, M, "Alt-F2"),
 KEY(SIZE_MOVE, CF5, BO, WPE_CTX_WINDOW, 0, "Ctrl-F5"),
 KEY(FILE_MANAGER, F3, BO, WPE_CTX_WINDOW, B | M, "F3"),
 KEY(FILE_MANAGER, OPEN, BO, WPE_CTX_WINDOW, 0, "Sun Open"),
 KEY(FILE_MANAGER_FIRST, CF3, BO, WPE_CTX_WINDOW, 0, "Ctrl-F3"),
 KEY(CLOSE_WINDOW, CtrlW, BO, WPE_CTX_WINDOW, B | M, "Ctrl-W"),
 KEY(ZOOM, F5, BO, WPE_CTX_WINDOW, M, "F5"),
 KEY(ZOOM, FRONT, BO, WPE_CTX_WINDOW, 0, "Sun Front"),
 KEY(ZOOM, AltZ, BO, WPE_CTX_WINDOW, M, "Alt-Z"),
 KEY(NEXT_WINDOW, F6, BO, WPE_CTX_WINDOW, M, "F6"),
 KEY(NEXT_WINDOW, AltN, BO, WPE_CTX_WINDOW, M, "Alt-N"),
 KEY(QUIT, AltX, BO, WPE_CTX_WINDOW, B | M, "Alt-X"),
 KEY(QUIT, STOP, BO, WPE_CTX_WINDOW, 0, "Sun Stop"),

 /* ---- WPE_CTX_PROGRAM: programming modes ---- */
 KEY(RUN, CF5, MO, WPE_CTX_PROGRAM, M, "Ctrl-F5"),
 KEY(RUN, CF9, BOTH, WPE_CTX_PROGRAM, M, "Ctrl-F9"),
 KEY(RUN, AltU, BOTH, WPE_CTX_PROGRAM, M, "Alt-U"),
 KEY(LSP_PREFIX, AltQ, BOTH, WPE_CTX_PROGRAM, 0, "Alt-Q"),
 KEY(LSP_MENU, WPE_LSP_MENU, BOTH, WPE_CTX_PROGRAM, H, NULL),
 KEY(MAKE, F9, BO, WPE_CTX_PROGRAM, B | M, "F9"),
 KEY(MAKE, AltM, BO, WPE_CTX_PROGRAM, M, "Alt-M"),
 KEY(MAKE, AltM, MO, WPE_CTX_PROGRAM, B | M, "Alt-M"),
 KEYF(MAKE, CS('B'), MO, WPE_CTX_PROGRAM, 0, "Ctrl-Shift-B", "Alt-M"),
 KEY(COMPILE, AltC, BOTH, WPE_CTX_PROGRAM, M, "Alt-C"),
 KEY(COMPILE, AF9, BOTH, WPE_CTX_PROGRAM, 0, "Alt-F9"),
 KEY(INSTALL, AltL, BOTH, WPE_CTX_PROGRAM, M, "Alt-L"),
 KEY(EXEC_MAKE, AltA, BOTH, WPE_CTX_PROGRAM, M, "Alt-A"),
 KEY(AI_PREFIX, AltG, BOTH, WPE_CTX_PROGRAM, 0, "Alt-G"),
 KEY(AI_MENU, WPE_AI_MENU, BOTH, WPE_CTX_PROGRAM, H, NULL),
 KEY(NEXT_ERROR, F8, MO, WPE_CTX_PROGRAM, NOS | M, "F8"),
 KEY(NEXT_ERROR, AltT, BOTH, WPE_CTX_PROGRAM, M, "Alt-T"),
 KEY(NEXT_ERROR, AF8, BOTH, WPE_CTX_PROGRAM, 0, "Alt-F8"),
 KEY(PREV_ERROR, SF8, MO, WPE_CTX_PROGRAM, NOS | M, "Shift-F8"),
 KEY(PREV_ERROR, AltV, BOTH, WPE_CTX_PROGRAM, M, "Alt-V"),
 KEY(PREV_ERROR, AF7, BOTH, WPE_CTX_PROGRAM, 0, "Alt-F7"),
 KEY(DEBUG_PREFIX, CtrlG, BO, WPE_CTX_PROGRAM, 0, "Ctrl-G"),
 /* the language server on one key, in the Modern style (Alt-Q and a letter
    stays in both): a key the terminal keeps has its Alt-Q letter as fallback */
 KEYF(LSP_DEFINITION, F12, MO, WPE_CTX_PROGRAM, M, "F12", "Alt-Q d"),
 KEYF(LSP_REFERENCES, SF12, MO, WPE_CTX_PROGRAM, M, "Shift-F12", "Alt-Q r"),
 KEYF(LSP_RENAME, F2, MO, WPE_CTX_PROGRAM, M, "F2", "Alt-Q n"),
 KEYF(LSP_COMPLETE, WPE_CTRL_SPACE, MO, WPE_CTX_PROGRAM, M, "Ctrl-Space", "Alt-Q c"),
 KEYF(LSP_FORMAT, CS('I'), MO, WPE_CTX_PROGRAM, M, "Ctrl-Shift-I", "Alt-Q f"),
 KEYF(LSP_OUTLINE, CS('O'), MO, WPE_CTX_PROGRAM, M, "Ctrl-Shift-O", "Alt-Q o"),
 KEYF(LSP_WORKSPACE_SYMBOLS, CtrlT, MO, WPE_CTX_PROGRAM, M, "Ctrl-T", "Alt-Q w"),

 /* ---- WPE_CTX_DEBUG ---- */
 KEY(DEBUG_TRACE, F7, BO, WPE_CTX_DEBUG, B | M, "F7"),
 KEYF(DEBUG_TRACE, F11, MO, WPE_CTX_DEBUG, B | M, "F11", "F7"),
 KEY(DEBUG_TRACE, F7, MO, WPE_CTX_DEBUG, 0, "F7"),
 KEY(DEBUG_STEP, F8, BO, WPE_CTX_DEBUG, B | M, "F8"),
 KEYF(DEBUG_STEP, F10, MO, WPE_CTX_DEBUG, INS | B | M, "F10", "F8"),
 KEY(DEBUG_STEP, F8, MO, WPE_CTX_DEBUG, 0, "F8"),
 KEY(DEBUG_RUN, CF10, BO, WPE_CTX_DEBUG, B | M, "Ctrl-F10"),
 KEY(DEBUG_RUN, F5, MO, WPE_CTX_DEBUG, B | M, "F5"),
 KEY(DEBUG_QUIT, CF2, BO, WPE_CTX_DEBUG, B | M, "Ctrl-F2"),
 KEY(DEBUG_QUIT, SF5, MO, WPE_CTX_DEBUG, B | M, "Shift-F5"),
 KEY(DEBUG_QUIT, CF2, MO, WPE_CTX_DEBUG, 0, "Ctrl-F2"),
 KEY(DEBUG_PAUSE, CtrlBrk, BOTH, WPE_CTX_DEBUG, B | M, "Ctrl-Brk"),
 KEY(DEBUG_GOTO_CURSOR, F4, BO, WPE_CTX_DEBUG, B, "F4"),
 KEY(DEBUG_GOTO_CURSOR, CF10, MO, WPE_CTX_DEBUG, B | M, "Ctrl-F10"),
 KEY(DEBUG_GOTO_CURSOR, F4, MO, WPE_CTX_DEBUG, 0, "F4"),
 KEY(DEBUG_BREAKPOINT, CF8, BO, WPE_CTX_DEBUG, M, "Ctrl-F8"),
 KEY(DEBUG_BREAKPOINT, F9, MO, WPE_CTX_DEBUG, M, "F9"),
 KEY(DEBUG_WATCH, CF7, BOTH, WPE_CTX_DEBUG, B | M, "Ctrl-F7"),
 KEY(DEBUG_STACK, CF6, BOTH, WPE_CTX_DEBUG, M, "Ctrl-F6"),
 KEYF(DEBUG_FINISH, SF11, MO, WPE_CTX_DEBUG, M, "Shift-F11", "Debug menu"),

 /* ---- WPE_CTX_EDIT ---- */
 KEY(K_PREFIX, CtrlK, BOTH, WPE_CTX_EDIT, 0, "Ctrl-K"),
 KEY(O_PREFIX, CtrlO, BO, WPE_CTX_EDIT, 0, "Ctrl-O"),
 KEY(GOTO_LINE, AltG, BOTH, WPE_CTX_EDIT, M, "Alt-G"),
 KEY(GOTO_LINE, CtrlG, MO, WPE_CTX_EDIT, M, "Ctrl-G"),
 KEY(SHOW_CLIPBOARD, CF10, BOTH, WPE_CTX_EDIT, 0, "Ctrl-F10"),
 KEY(SHOW_CLIPBOARD, AltY, BOTH, WPE_CTX_EDIT, M, "Alt-Y"),
 KEY(BLOCK_DELETE, CtrlDel, BOTH, WPE_CTX_EDIT, M, "Ctrl-Del"),
 KEY(EDIT_UNDO, UNDO, BOTH, WPE_CTX_EDIT, 0, "Sun Undo"),
 KEY(EDIT_UNDO, AltBS, BOTH, WPE_CTX_EDIT, 0, "Alt-Backspace"),
 KEY(EDIT_UNDO, CtrlZ, MO, WPE_CTX_EDIT, M, "Ctrl-Z"),
 KEY(EDIT_UNDO, CtrlU, BOTH, WPE_CTX_EDIT, M, "Ctrl-U"),
 KEY(EDIT_REDO, AGAIN, BOTH, WPE_CTX_EDIT, 0, "Sun Again"),
 KEY(EDIT_REDO, SABS, BOTH, WPE_CTX_EDIT, 0, "Alt-Shift-Backspace"),
 KEY(EDIT_REDO, CtrlY, MO, WPE_CTX_EDIT, M, "Ctrl-Y"),
 KEYF(EDIT_REDO, CS('Z'), MO, WPE_CTX_EDIT, 0, "Ctrl-Shift-Z", "Ctrl-Y"),
 KEY(EDIT_REDO, CtrlR, BO, WPE_CTX_EDIT, M, "Ctrl-R"),
 KEY(EDIT_CUT, CUT, BOTH, WPE_CTX_EDIT, 0, "Sun Cut"),
 KEY(EDIT_CUT, ShiftDel, BOTH, WPE_CTX_EDIT, M, "Shift-Del"),
 KEY(EDIT_CUT, CtrlX, BOTH, WPE_CTX_EDIT, M, "Ctrl-X"),
 KEY(EDIT_PASTE, PASTE, BOTH, WPE_CTX_EDIT, 0, "Sun Paste"),
 KEY(EDIT_PASTE, ShiftEin, BOTH, WPE_CTX_EDIT, M, "Shift-Ins"),
 KEY(EDIT_PASTE, AltEin, BOTH, WPE_CTX_EDIT, 0, "Alt-Ins"),
 KEY(EDIT_PASTE, CtrlV, BOTH, WPE_CTX_EDIT, M, "Ctrl-V"),
 KEY(EDIT_COPY, COPY, BOTH, WPE_CTX_EDIT, 0, "Sun Copy"),
 KEY(EDIT_COPY, CtrlEin, BOTH, WPE_CTX_EDIT, M, "Ctrl-Ins"),
 KEY(EDIT_COPY, AltDel, BOTH, WPE_CTX_EDIT, 0, "Alt-Del"),
 KEY(EDIT_COPY, CtrlC, BOTH, WPE_CTX_EDIT, M, "Ctrl-C"),
 /* the Modern style */
 KEY(SAVE, CtrlS, MO, WPE_CTX_EDIT, B | M, "Ctrl-S"),
 KEY(FIND, FID, MO, WPE_CTX_EDIT, 0, "Sun Find"),
 KEY(FIND, CtrlF, MO, WPE_CTX_EDIT, B | M, "Ctrl-F"),
 KEY(FIND, F4, MO, WPE_CTX_EDIT, 0, "F4"),
 KEY(REPLACE, CtrlR, MO, WPE_CTX_EDIT, M, "Ctrl-R"),
 KEYF(REPLACE, WPE_CTRL_H, MO, WPE_CTX_EDIT, 0, "Ctrl-H", "Ctrl-R"),
 KEY(SEARCH_AGAIN, F3, MO, WPE_CTX_EDIT, B | M, "F3"),
 KEY(BLOCK_ALL, CtrlA, MO, WPE_CTX_EDIT, M, "Ctrl-A"),
 KEY(BLOCK_LINE, CtrlL, MO, WPE_CTX_EDIT, M, "Ctrl-L"),
 KEY(SELECT_WORD, CtrlD, MO, WPE_CTX_EDIT, M, "Ctrl-D"),
 /* the Borland style */
 KEY(SAVE, F2, BO, WPE_CTX_EDIT, B | M, "F2"),
 KEY(FIND, FID, BO, WPE_CTX_EDIT, 0, "Sun Find"),
 KEY(FIND, F4, BO, WPE_CTX_EDIT, B | M, "F4"),
 KEY(REPLACE, AF4, BO, WPE_CTX_EDIT, M, "Alt-F4"),
 KEY(SEARCH_AGAIN, CtrlL, BO, WPE_CTX_EDIT, B | M, "Ctrl-L"),
 KEY(SEARCH_AGAIN, CF4, BO, WPE_CTX_EDIT, 0, "Ctrl-F4"),

 /* ---- WPE_CTX_PREFIX_K: the letter after Ctrl-K ---- */
 LETTER(BLOCK_GOTO_BEGIN, 'A', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_BEGIN, 'B', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_COPY, 'C', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_CASE, 'D', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_BEAUTIFY, 'F', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_HIDE, 'H', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_INDENT, 'I', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_END, 'K', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_LINE, 'L', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_READ, 'R', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_UNINDENT, 'U', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_MOVE, 'V', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_WRITE, 'W', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_ALL, 'X', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_DELETE, 'Y', WPE_CTX_PREFIX_K, M),
 LETTER(BLOCK_GOTO_END, 'Z', WPE_CTX_PREFIX_K, M),
 RANGE(MARKER_SET, '0', '9', BOTH, WPE_CTX_PREFIX_K, M, NULL),

 /* ---- WPE_CTX_PREFIX_O: the letter after Ctrl-O (Borland only: Ctrl-O opens a file in Modern) ---- */
#undef LETTER
#define LETTER(act, ch, ctx, fl) KEY(act, ch, BO, ctx, fl, NULL)
 LETTER(DELETE_TO_EOL, 'Y', WPE_CTX_PREFIX_O, M),
 LETTER(DELETE_WORD_LEFT, 'T', WPE_CTX_PREFIX_O, M),
 LETTER(FIND, 'F', WPE_CTX_PREFIX_O, M),
 LETTER(REPLACE, 'A', WPE_CTX_PREFIX_O, M),
 LETTER(SHOW_DEFINITION, 'S', WPE_CTX_PREFIX_O, M),
 LETTER(NEXT_DEFINITION, 'N', WPE_CTX_PREFIX_O, M),
 LETTER(MATCH_BRACKET, 'K', WPE_CTX_PREFIX_O, M),
 LETTER(BEAUTIFY, 'B', WPE_CTX_PREFIX_O, M),
 LETTER(HELP_MARK_BUTTON, 'U', WPE_CTX_PREFIX_O, M),
 LETTER(HELP_MARK_LINE, 'M', WPE_CTX_PREFIX_O, M),
 LETTER(HELP_MARK_HEADER, 'H', WPE_CTX_PREFIX_O, M),
 LETTER(HELP_MARK_END, 'E', WPE_CTX_PREFIX_O, M),
 LETTER(HELP_MARKS_DELETE, 'L', WPE_CTX_PREFIX_O, M),
 LETTER(HELP_CHECK, 'C', WPE_CTX_PREFIX_O, M),
 RANGE(MARKER_GOTO, '0', '9', BO, WPE_CTX_PREFIX_O, M, NULL),

 /* ---- WPE_CTX_PREFIX_DEBUG: the letter after Ctrl-G, lower case or control (Borland only:
        Ctrl-G is Go to line in Modern, and the Debug menu names every command) ---- */
 LETTER(DEBUG_BREAKPOINT, 'b', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_REMOVE_BREAKPOINTS, 'm', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_WATCH, 'w', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_EDIT_WATCH, 'e', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_DELETE_WATCH, 'd', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_REMOVE_ALL_WATCHES, 'a', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_STACK, 'k', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_GOTO_CURSOR, 'u', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_FINISH, 'f', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_TRACE, 't', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_STEP, 's', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_RUN, 'r', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_RUN, 'c', WPE_CTX_PREFIX_DEBUG, CT),
 LETTER(DEBUG_PAUSE, 'i', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_OUTPUT, 'p', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_OPTIONS, 'o', WPE_CTX_PREFIX_DEBUG, M | CT),
 LETTER(DEBUG_QUIT, 'q', WPE_CTX_PREFIX_DEBUG, M | CT),
#undef LETTER
#define LETTER(act, ch, ctx, fl) KEY(act, ch, BOTH, ctx, fl, NULL)

 /* ---- WPE_CTX_PREFIX_LSP: the letter after Alt-Q, lower case or control ---- */
 LETTER(LSP_DEFINITION, 'd', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_IMPLEMENTATION, 'i', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_TYPE_DEFINITION, 't', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_HOVER, 'h', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_COMPLETE, 'c', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_REFERENCES, 'r', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_CALLERS, 'b', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_CALLEES, 'g', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_SUPERTYPES, 'k', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_SUBTYPES, 'j', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_EXPAND_SELECTION, 'v', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_HIGHLIGHT, 'u', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_INLAY, 'y', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_SEMANTIC, 'm', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_CODELENS, 'l', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_SIGNATURE, 's', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_WORKSPACE_SYMBOLS, 'w', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_CODE_ACTIONS, 'a', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_FORMAT, 'f', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_RENAME, 'n', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_OUTLINE, 'o', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_DIAGNOSTICS, 'e', WPE_CTX_PREFIX_LSP, CT),
 LETTER(LSP_NEXT_PROBLEM, '.', WPE_CTX_PREFIX_LSP, 0),
 LETTER(LSP_PREV_PROBLEM, ',', WPE_CTX_PREFIX_LSP, 0),
 KEY(LSP_CANCEL, WPE_ESC, BOTH, WPE_CTX_PREFIX_LSP, 0, "Alt-Q Esc"),

 /* ---- WPE_CTX_PREFIX_AI: the letter after Alt-G, upper case ---- */
 LETTER(AI_ASK, 'A', WPE_CTX_PREFIX_AI, M),
 LETTER(AI_EDIT, 'E', WPE_CTX_PREFIX_AI, M),
 LETTER(AI_COMPLETE, 'C', WPE_CTX_PREFIX_AI, M),
 LETTER(AI_PLAN, 'F', WPE_CTX_PREFIX_AI, M),
 LETTER(AI_AGENT, 'G', WPE_CTX_PREFIX_AI, M),
 LETTER(AI_FIX_BUILD, 'B', WPE_CTX_PREFIX_AI, M),
 LETTER(AI_POLICY, 'Y', WPE_CTX_PREFIX_AI, M),
 LETTER(AI_READ_FOLDERS, 'R', WPE_CTX_PREFIX_AI, M),
 LETTER(AI_NEW_SESSION, 'N', WPE_CTX_PREFIX_AI, M),
 LETTER(AI_SETTINGS, 'S', WPE_CTX_PREFIX_AI, M),

 /* ---- WPE_CTX_WATCHES: the Watches window ---- */
 KEY(DEBUG_WATCH, EINFG, BOTH, WPE_CTX_WATCHES, 0, "Ins"),
 KEY(DEBUG_DELETE_WATCH, ENTF, BOTH, WPE_CTX_WATCHES, 0, "Del"),

 /* ---- WPE_CTX_HELP: the Help viewer ---- */
 KEY(HELP_NEXT, AF8, BOTH, WPE_CTX_HELP, M, "Alt-F8"),
 KEY(HELP_NEXT, AltT, BOTH, WPE_CTX_HELP, M, "Alt-T"),
 KEY(HELP_PREV, AF7, BOTH, WPE_CTX_HELP, M, "Alt-F7"),
 KEY(HELP_PREV, AltV, BOTH, WPE_CTX_HELP, M, "Alt-V"),

 /* ---- WPE_CTX_LIST: the file manager's list windows ---- */
 KEY(CLOSE_LIST, AF3, BO, WPE_CTX_LIST, 0, "Alt-F3"),
 KEY(CLOSE_LIST, CF4, MO, WPE_CTX_LIST, 0, "Ctrl-F4"),

 /* ---- WPE_CTX_DIALOG_MOVE: a dialog's move mode ---- */
 KEY(SIZE_MOVE, AF2, BOTH, WPE_CTX_DIALOG_MOVE, 0, "Alt-F2"),

 /* ---- WPE_CTX_CURSOR: control letters that move the cursor or delete (Borland only:
        in Modern each of these letters is a VS Code command) ---- */
 KEY(CURSOR_UP, CtrlP, BO, WPE_CTX_CURSOR, 0, "Ctrl-P"),
 KEY(CURSOR_DOWN, CtrlN, BO, WPE_CTX_CURSOR, 0, "Ctrl-N"),
 KEY(CURSOR_LEFT, CtrlB, BO, WPE_CTX_CURSOR, 0, "Ctrl-B"),
 KEY(CURSOR_RIGHT, CtrlF, BO, WPE_CTX_CURSOR, 0, "Ctrl-F"),
 KEY(LINE_START, CtrlA, BO, WPE_CTX_CURSOR, 0, "Ctrl-A"),
 KEY(LINE_END, CtrlE, BO, WPE_CTX_CURSOR, 0, "Ctrl-E"),
 KEY(DELETE_WORD_RIGHT, CtrlT, BO, WPE_CTX_CURSOR, 0, "Ctrl-T"),
 KEY(DELETE_TO_EOL, CtrlZ, BO, WPE_CTX_CURSOR, 0, "Ctrl-Z"),
 KEY(DELETE_LINE, DGZ, BO, WPE_CTX_CURSOR, 0, "Ctrl-Y"),
 KEY(DELETE_CHAR, CtrlD, BO, WPE_CTX_CURSOR, 0, "Ctrl-D")
};

#undef BO
#undef MO
#undef BOTH
#undef B
#undef M
#undef H
#undef CT
#undef NOS
#undef INS
#undef KEY
#undef KEYF
#undef RANGE
#undef LETTER

#define WPE_BINDING_COUNT (sizeof(wpe_bindings) / sizeof(wpe_bindings[0]))

static const char *const wpe_action_names[WPE_ACT_COUNT] = {
#define WPE_ACT_NAME(id, name, text) name,
 WPE_ACTIONS(WPE_ACT_NAME)
#undef WPE_ACT_NAME
};

static const char *const wpe_action_texts[WPE_ACT_COUNT] = {
#define WPE_ACT_TEXT(id, name, text) text,
 WPE_ACTIONS(WPE_ACT_TEXT)
#undef WPE_ACT_TEXT
};

static const char *const wpe_context_names[WPE_CTX_COUNT] = {
 "Windows and menus",
 "Programming",
 "Debugger",
 "Editing",
 "After Ctrl-K",
 "After Ctrl-O",
 "After Ctrl-G",
 "After Alt-Q",
 "After Alt-G",
 "Watches window",
 "Help viewer",
 "File manager lists",
 "Dialog move",
 "Cursor keys",
 "Boxes"
};

static const char *const wpe_context_prefixes[WPE_CTX_COUNT] = {
 NULL, NULL, NULL, NULL, "Ctrl-K", "Ctrl-O", "Ctrl-G", "Alt-Q", "Alt-G",
 NULL, NULL, NULL, NULL, NULL, NULL
};

/** The control code of a lower-case letter: 'p' gives 16. */
static int wpe_control_of(int letter)
{
 return(letter - 'a' + 1);
}

static int (*wpe_session_probe)(void);

void wpe_binding_set_session_probe(int (*probe)(void))
{
 wpe_session_probe = probe;
}

/** Is a debugger session open?  Never, until the editor says how to ask. */
static int wpe_session_runs(void)
{
 return(wpe_session_probe != NULL && wpe_session_probe() != 0);
}

/** Does the state the row names (no session, a session) hold now? */
static int wpe_row_state_holds(const WpeBinding *row)
{
 if ((row->flags & WPE_ROW_NO_SESSION) && wpe_session_runs())
  return(0);
 if ((row->flags & WPE_ROW_IN_SESSION) && !wpe_session_runs())
  return(0);
 return(1);
}

/** Does the row bind @code in @style and @context? */
static int wpe_row_matches(const WpeBinding *row, int code, int style, int context)
{
 if (row->context != context || !(row->styles & WPE_STYLE_BIT(style)))
  return(0);
 if (!wpe_row_state_holds(row))
  return(0);
 if (code >= row->code && code <= row->code_last)
  return(1);
 if ((row->flags & WPE_ROW_CTRL) && code == wpe_control_of(row->code))
  return(1);
 return(0);
}

int wpe_binding_style(int edopt)
{
 return((edopt & ED_CUA_STYLE) ? WPE_STYLE_MODERN : WPE_STYLE_BORLAND);
}

static void (*wpe_trace_hook)(int code, int style, int context, int action);
static int wpe_trace_paused;   /* the fold asks; asking is not dispatching */

void wpe_binding_set_trace(void (*hook)(int code, int style, int context, int action))
{
 wpe_trace_hook = hook;
}

WpeAction wpe_binding_action(int code, int style, int context)
{
 size_t i;

 for (i = 0; i < WPE_BINDING_COUNT; i++)
  if (wpe_row_matches(&wpe_bindings[i], code, style, context)) {
   if (wpe_trace_hook && !wpe_trace_paused)
    wpe_trace_hook(code, style, context, wpe_bindings[i].action);
   return((WpeAction)wpe_bindings[i].action);
  }
 return(WPE_ACT_NONE);
}

int wpe_binding_prefix_folds_control(int style)
{
 return(style == WPE_STYLE_BORLAND);
}

int wpe_binding_first_code(WpeAction action, int style, int context)
{
 size_t i;

 for (i = 0; i < WPE_BINDING_COUNT; i++) {
  const WpeBinding *row = &wpe_bindings[i];

  if (row->action == action && row->context == context &&
      (row->styles & WPE_STYLE_BIT(style)))
   return(row->code);
 }
 return(0);
}

int wpe_binding_window_number(int code)
{
 if (code >= Alt1 && code <= Alt9)
  return(code - Alt1 + 1);
 if (code >= 1024 && code <= 1049)
  return(code - 1014);
 return(0);
}

/** The key a legacy terminal sends where a distinct Ctrl code was reported. */
static int wpe_legacy_key(int code)
{
 switch (code) {
 case WPE_CTRL_H:        return(CtrlH);
 case WPE_CTRL_I:        return(CtrlI);
 case WPE_CTRL_M:        return(CtrlM);
 case WPE_CTRL_LBRACKET: return(WPE_ESC);
 case WPE_CTRL_TAB:      return(WPE_TAB);   /* a legacy terminal sends Tab */
 default:                break;
 }
 if (code >= WPE_CS_FIRST && code <= WPE_CS_LAST)
  return(code - WPE_CS_FIRST + CtrlA);      /* Ctrl-Shift-letter: Ctrl-letter */
 return(code);
}

/** Does any of the contexts an editor window reads bind @code? */
static int wpe_bound_in_editor(int code, int style)
{
 static const int contexts[] = {
  WPE_CTX_WINDOW, WPE_CTX_PROGRAM, WPE_CTX_DEBUG, WPE_CTX_EDIT, WPE_CTX_CURSOR
 };
 size_t i;
 int bound = 0;

 wpe_trace_paused++;
 for (i = 0; i < sizeof(contexts) / sizeof(contexts[0]) && !bound; i++)
  bound = wpe_binding_action(code, style, contexts[i]) != WPE_ACT_NONE;
 wpe_trace_paused--;
 return(bound);
}

int wpe_binding_fold(int code, int style, int context)
{
 int bound;

 if (context == WPE_CTX_EDIT) {
  bound = wpe_bound_in_editor(code, style);
 } else {
  wpe_trace_paused++;
  bound = wpe_binding_action(code, style, context) != WPE_ACT_NONE;
  wpe_trace_paused--;
 }
 return(bound ? code : wpe_legacy_key(code));
}

/* ---- Which terminals deliver which key ----
   Filled from docs/chapters/keys-delivery.txt, measured on 2026-11-14 with a
   raw reader in each terminal.  "measured" says the bytes were seen,
   "documented" that the terminal could not be driven here and the entry is
   what its own documentation says. */

#define EMU(e) (1u << (e))
#define ALL_X_TERMS (EMU(WPE_EMU_XTERM) | EMU(WPE_EMU_VTE) | EMU(WPE_EMU_KONSOLE) | EMU(WPE_EMU_KITTY))

static const WpeDelivery wpe_deliveries[] = {
 /* a key of its own only under a protocol */
 { WPE_CS_FIRST, WPE_CS_LAST, WPE_DELIVER_PROTOCOL, EMU(WPE_EMU_KONSOLE),
   "measured: Konsole keeps every Ctrl-Shift-letter for itself; others send it as Ctrl-letter without a protocol" },
 { CS('C'), CS('C'), WPE_DELIVER_PROTOCOL, EMU(WPE_EMU_VTE) | EMU(WPE_EMU_KITTY),
   "documented: copy in GNOME Terminal and kitty" },
 { CS('V'), CS('V'), WPE_DELIVER_PROTOCOL, EMU(WPE_EMU_VTE) | EMU(WPE_EMU_KITTY),
   "documented: paste in GNOME Terminal and kitty" },
 { CS('T'), CS('T'), WPE_DELIVER_PROTOCOL, EMU(WPE_EMU_VTE) | EMU(WPE_EMU_KITTY),
   "documented: new tab" },
 { CS('W'), CS('W'), WPE_DELIVER_PROTOCOL, EMU(WPE_EMU_VTE) | EMU(WPE_EMU_KITTY),
   "documented: close tab" },
 { CS('N'), CS('N'), WPE_DELIVER_PROTOCOL, EMU(WPE_EMU_VTE) | EMU(WPE_EMU_KITTY),
   "documented: new window" },
 { CS('Q'), CS('Q'), WPE_DELIVER_PROTOCOL, EMU(WPE_EMU_VTE) | EMU(WPE_EMU_KITTY),
   "documented: quit the terminal" },
 { CS('F'), CS('F'), WPE_DELIVER_PROTOCOL, EMU(WPE_EMU_VTE),
   "documented: find in GNOME Terminal" },
 { WPE_CTRL_TAB, WPE_CTRL_TAB, WPE_DELIVER_PROTOCOL, EMU(WPE_EMU_KONSOLE) | EMU(WPE_EMU_KITTY) | EMU(WPE_EMU_VTE),
   "measured in Konsole: next tab; documented in kitty and GNOME Terminal" },
 { WPE_CTRL_BACKQUOTE, WPE_CTRL_SPACE, WPE_DELIVER_PROTOCOL, 0,
   "measured: the legacy bytes are NUL for Ctrl-` and Ctrl-Space, so the distinct key exists only under a protocol" },
 { WPE_CTRL_H, WPE_CTRL_H, WPE_DELIVER_PROTOCOL, 0,
   "measured: the legacy byte is 0x08, Backspace" },
 /* a legacy byte or a plain CSI, but a terminal keeps it */
 { CBUP, CBDO, WPE_DELIVER_ALWAYS, EMU(WPE_EMU_VTE),
   "documented: GNOME Terminal and xfce4-terminal switch tabs with Ctrl-PgUp and Ctrl-PgDn" },
 { F1, F1, WPE_DELIVER_ALWAYS, EMU(WPE_EMU_VTE),
   "documented: GNOME Terminal's help" },
 { F10, F10, WPE_DELIVER_ALWAYS, EMU(WPE_EMU_VTE),
   "documented: GNOME Terminal's menu" },
 { F11, F11, WPE_DELIVER_ALWAYS, EMU(WPE_EMU_VTE),
   "documented: GNOME Terminal's full screen" },
 { Alt1, Alt9, WPE_DELIVER_ALWAYS, EMU(WPE_EMU_VTE),
   "documented: GNOME Terminal switches tabs with Alt-1 to Alt-9" },
 { AF4, AF4, WPE_DELIVER_ALWAYS, ALL_X_TERMS | EMU(WPE_EMU_CONSOLE),
   "documented: the window manager closes the window; the Linux console switches screens" },
 { CtrlB, CtrlB, WPE_DELIVER_ALWAYS, EMU(WPE_EMU_TMUX),
   "documented: tmux's prefix" },
 { CtrlS, CtrlS, WPE_DELIVER_ALWAYS, EMU(WPE_EMU_SCREEN),
   "measured: screen takes Ctrl-S as XOFF, flow control" }
};

#define WPE_DELIVERY_COUNT (sizeof(wpe_deliveries) / sizeof(wpe_deliveries[0]))

static const char *const wpe_emulator_names[WPE_EMU_COUNT] = {
 "xterm", "GNOME Terminal", "Konsole", "kitty", "tmux", "screen", "Linux console"
};

/* Measured: xterm answers modifyOtherKeys, Konsole and kitty answer the kitty
   protocol, GNOME Terminal (VTE), xfce4-terminal and screen answer neither, tmux
   passes the xterm form on with extended-keys on, the console has no protocol. */
static const unsigned wpe_emulator_proto[WPE_EMU_COUNT] = {
 WPE_PROTO_MODIFY_OTHER_KEYS, 0, WPE_PROTO_KITTY, WPE_PROTO_KITTY,
 WPE_PROTO_MODIFY_OTHER_KEYS, 0, 0
};

const WpeDelivery *wpe_key_delivery(size_t *n)
{
 *n = WPE_DELIVERY_COUNT;
 return(wpe_deliveries);
}

const char *wpe_emulator_name(int emu)
{
 return((emu >= 0 && emu < WPE_EMU_COUNT) ? wpe_emulator_names[emu] : "");
}

unsigned wpe_emulator_protocols(int emu)
{
 return((emu >= 0 && emu < WPE_EMU_COUNT) ? wpe_emulator_proto[emu] : 0);
}

int wpe_binding_key_class(int code)
{
 size_t i;

 for (i = 0; i < WPE_DELIVERY_COUNT; i++)
  if (code >= wpe_deliveries[i].code && code <= wpe_deliveries[i].code_last &&
      wpe_deliveries[i].klass == WPE_DELIVER_PROTOCOL)
   return(WPE_DELIVER_PROTOCOL);
 return(WPE_DELIVER_ALWAYS);
}

int wpe_key_reaches(int code, int emu)
{
 size_t i;
 int needs_protocol = wpe_binding_key_class(code) == WPE_DELIVER_PROTOCOL;

 if (emu < 0 || emu >= WPE_EMU_COUNT)
  return(0);
 for (i = 0; i < WPE_DELIVERY_COUNT; i++)
  if (code >= wpe_deliveries[i].code && code <= wpe_deliveries[i].code_last &&
      (wpe_deliveries[i].taken & EMU(emu)))
   return(0);
 if (needs_protocol && wpe_emulator_proto[emu] == 0)
  return(0);
 return(1);
}

const WpeBinding *wpe_binding_table(size_t *n)
{
 *n = WPE_BINDING_COUNT;
 return(wpe_bindings);
}

const char *wpe_action_name(WpeAction a)
{
 return((a >= 0 && a < WPE_ACT_COUNT) ? wpe_action_names[a] : "");
}

const char *wpe_action_text(WpeAction a)
{
 return((a >= 0 && a < WPE_ACT_COUNT) ? wpe_action_texts[a] : "");
}

const char *wpe_context_name(int context)
{
 return((context >= 0 && context < WPE_CTX_COUNT) ? wpe_context_names[context] : "");
}

const char *wpe_context_prefix(int context)
{
 return((context >= 0 && context < WPE_CTX_COUNT) ? wpe_context_prefixes[context] : NULL);
}

/** Write the letter or range of a prefix row: "B", "0..9". */
static void wpe_letter_name(const WpeBinding *row, char *buf, size_t n)
{
 if (row->code == row->code_last)
  snprintf(buf, n, "%c", toupper(row->code));
 else
  snprintf(buf, n, "%c..%c", row->code, row->code_last);
}

/** The key as the reference writes it: Ctrl-K B for a letter after a prefix. */
static void wpe_doc_name(const WpeBinding *row, char *buf, size_t n)
{
 char letter[8];

 if (row->name) {
  snprintf(buf, n, "%s", row->name);
  return;
 }
 if (row->flags & WPE_ROW_HIDDEN) {
  buf[0] = '\0';
  return;
 }
 wpe_letter_name(row, letter, sizeof letter);
 snprintf(buf, n, "%s %s", wpe_context_prefix(row->context), letter);
}

/** Shorten a name for a bar or a menu: Ctrl- is ^, and a menu drops the hyphen
    after Alt and Shift. */
static void wpe_spell_name(char *buf, size_t n, int form)
{
 char tmp[64];

 if (form == WPE_NAME_DOC)
  return;
 if (strncmp(buf, "Ctrl-", 5) == 0) {
  snprintf(tmp, sizeof tmp, "^%s", buf + 5);
  snprintf(buf, n, "%s", tmp);
 } else if (form == WPE_NAME_MENU && strncmp(buf, "Alt-", 4) == 0) {
  snprintf(tmp, sizeof tmp, "Alt %s", buf + 4);
  snprintf(buf, n, "%s", tmp);
 } else if (form == WPE_NAME_MENU && strncmp(buf, "Shift-", 6) == 0) {
  snprintf(tmp, sizeof tmp, "Shift %s", buf + 6);
  snprintf(buf, n, "%s", tmp);
 }
}

const char *wpe_binding_row_name(const WpeBinding *row, int form, char *buf, size_t n)
{
 wpe_doc_name(row, buf, n);
 wpe_spell_name(buf, n, form);
 return(buf);
}

int wpe_binding_key_text(char *buf, size_t n, WpeAction action, int style,
                         unsigned contexts, unsigned flag, int form,
                         const char *sep)
{
 size_t i;
 size_t len = 0;
 char name[64];

 if (!sep)
  sep = " / ";
 if (n > 0)
  buf[0] = '\0';
 for (i = 0; i < WPE_BINDING_COUNT; i++) {
  const WpeBinding *row = &wpe_bindings[i];

  if (row->action != action || !(row->styles & WPE_STYLE_BIT(style)))
   continue;
  if (!(contexts & WPE_CTX_BIT(row->context)) || !(row->flags & flag))
   continue;
  wpe_binding_row_name(row, form, name, sizeof name);
  if (len + strlen(sep) + strlen(name) + 1 > n)
   break;
  if (len > 0) {
   strcpy(buf + len, sep);
   len += strlen(sep);
  }
  strcpy(buf + len, name);
  len += strlen(name);
 }
 return((int)len);
}

/** The first row of an action in a style with @flag, NULL when none. */
static const WpeBinding *wpe_find_row(WpeAction action, int style, unsigned flag)
{
 size_t i;

 for (i = 0; i < WPE_BINDING_COUNT; i++) {
  const WpeBinding *row = &wpe_bindings[i];

  if (row->action == action && (row->styles & WPE_STYLE_BIT(style)) &&
      (flag == 0 || (row->flags & flag)))
   return(row);
 }
 return(NULL);
}

const char *wpe_binding_key_name(WpeAction action, int style)
{
 static char name[64];
 const WpeBinding *row = wpe_find_row(action, style, WPE_ROW_BAR);

 if (!row)
  row = wpe_find_row(action, style, WPE_ROW_MENU);
 if (!row)
  row = wpe_find_row(action, style, 0);
 if (!row)
  return(NULL);
 wpe_doc_name(row, name, sizeof name);
 return(name);
}

const char *wpe_binding_run_key(int edopt)
{
 const char *name = wpe_binding_key_name(WPE_ACT_RUN, wpe_binding_style(edopt));

 if (name)
  return(name);
 return("Run");
}

int wpe_binding_bar_code(WpeAction action, int style)
{
 const WpeBinding *row = wpe_find_row(action, style, WPE_ROW_BAR);

 return(row ? row->code : 0);
}

const char *wpe_bar_entry_text(char *buf, size_t n, WpeAction action, int style,
                               const char *stem, int *keylen)
{
 const WpeBinding *row = wpe_find_row(action, style, WPE_ROW_BAR);
 char key[64];

 if (!row) {
  snprintf(buf, n, "%s", stem);
  if (keylen)
   *keylen = 0;
  return(buf);
 }
 wpe_binding_row_name(row, WPE_NAME_BAR, key, sizeof key);
 snprintf(buf, n, "%s %s", key, stem);
 if (keylen)
  *keylen = (int)strlen(key);
 return(buf);
}

const char *wpe_menu_item_text(char *buf, size_t n, const char *stem,
                               WpeAction action, int style, unsigned contexts,
                               int width, const char *sep)
{
 char keys[96];
 int keylen = wpe_binding_key_text(keys, sizeof keys, action, style, contexts,
                                   WPE_ROW_MENU, WPE_NAME_MENU, sep);
 int pad = width - (int)strlen(stem) - keylen;

 if (keylen == 0) {
  snprintf(buf, n, "%s", stem);
  return(buf);
 }
 if (pad < 1)
  pad = 1;
 snprintf(buf, n, "%s%*s%s", stem, pad, "", keys);
 return(buf);
}
