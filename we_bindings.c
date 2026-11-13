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
#define CU WPE_STYLE_BIT(WPE_STYLE_CUA)
#define BOTH WPE_STYLES_BOTH
#define B WPE_ROW_BAR
#define M WPE_ROW_MENU
#define H WPE_ROW_HIDDEN
#define CT WPE_ROW_CTRL

#define RANGE(act, lo, hi, st, ctx, fl, nm) \
 { WPE_ACT_##act, (lo), (hi), (st), (ctx), (fl), (nm) }
#define KEY(act, code, st, ctx, fl, nm) RANGE(act, code, code, st, ctx, fl, nm)
#define LETTER(act, ch, ctx, fl) KEY(act, ch, BOTH, ctx, fl, NULL)

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
 KEY(MENU_BAR, F10, BOTH, WPE_CTX_WINDOW, 0, "F10"),
 KEY(MENU_BAR, AltBl, BOTH, WPE_CTX_WINDOW, 0, "Alt-Space"),
 KEY(LIST_WINDOWS, Alt0, BOTH, WPE_CTX_WINDOW, M, "Alt-0"),
 KEY(USER_SCREEN, AF5, BOTH, WPE_CTX_WINDOW, M, "Alt-F5"),
 /* the CUA style */
 KEY(SIZE_MOVE, CtrlL, CU, WPE_CTX_WINDOW, M, "Ctrl-L"),
 KEY(FILE_MANAGER, F2, CU, WPE_CTX_WINDOW, B | M, "F2"),
 KEY(FILE_MANAGER, OPEN, CU, WPE_CTX_WINDOW, 0, "Sun Open"),
 KEY(FILE_MANAGER_FIRST, SF2, CU, WPE_CTX_WINDOW, 0, "Shift-F2"),
 KEY(CLOSE_WINDOW, CtrlW, CU, WPE_CTX_WINDOW, M, "Ctrl-W"),
 KEY(CLOSE_WINDOW, CF4, CU, WPE_CTX_WINDOW, B | M, "Ctrl-F4"),
 KEY(CLOSE_WINDOW, AltX, CU, WPE_CTX_WINDOW, 0, "Alt-X"),
 KEY(CASCADE, FRONT, CU, WPE_CTX_WINDOW, 0, "Sun Front"),
 KEY(CASCADE, AltZ, CU, WPE_CTX_WINDOW, 0, "Alt-Z"),
 KEY(CASCADE, SF4, CU, WPE_CTX_WINDOW, M, "Shift-F4"),
 KEY(TILE, SF5, CU, WPE_CTX_WINDOW, M, "Shift-F5"),
 KEY(ZOOM, SF6, CU, WPE_CTX_WINDOW, M, "Shift-F6"),
 KEY(NEXT_WINDOW, CF6, CU, WPE_CTX_WINDOW, M, "Ctrl-F6"),
 KEY(NEXT_WINDOW, AltN, CU, WPE_CTX_WINDOW, M, "Alt-N"),
 KEY(QUIT, STOP, CU, WPE_CTX_WINDOW, 0, "Sun Stop"),
 KEY(QUIT, AF4, CU, WPE_CTX_WINDOW, B | M, "Alt-F4"),
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
 KEY(RUN, CF9, BOTH, WPE_CTX_PROGRAM, M, "Ctrl-F9"),
 KEY(RUN, AltU, BOTH, WPE_CTX_PROGRAM, M, "Alt-U"),
 KEY(LSP_PREFIX, AltQ, BOTH, WPE_CTX_PROGRAM, 0, "Alt-Q"),
 KEY(LSP_MENU, WPE_LSP_MENU, BOTH, WPE_CTX_PROGRAM, H, NULL),
 KEY(MAKE, F9, BOTH, WPE_CTX_PROGRAM, B | M, "F9"),
 KEY(MAKE, AltM, BOTH, WPE_CTX_PROGRAM, M, "Alt-M"),
 KEY(COMPILE, AltC, BOTH, WPE_CTX_PROGRAM, M, "Alt-C"),
 KEY(COMPILE, AF9, BOTH, WPE_CTX_PROGRAM, 0, "Alt-F9"),
 KEY(INSTALL, AltL, BOTH, WPE_CTX_PROGRAM, M, "Alt-L"),
 KEY(EXEC_MAKE, AltA, BOTH, WPE_CTX_PROGRAM, M, "Alt-A"),
 KEY(AI_PREFIX, AltG, BOTH, WPE_CTX_PROGRAM, 0, "Alt-G"),
 KEY(AI_MENU, WPE_AI_MENU, BOTH, WPE_CTX_PROGRAM, H, NULL),
 KEY(NEXT_ERROR, AltT, BOTH, WPE_CTX_PROGRAM, M, "Alt-T"),
 KEY(NEXT_ERROR, AF8, BOTH, WPE_CTX_PROGRAM, 0, "Alt-F8"),
 KEY(PREV_ERROR, AltV, BOTH, WPE_CTX_PROGRAM, M, "Alt-V"),
 KEY(PREV_ERROR, AF7, BOTH, WPE_CTX_PROGRAM, 0, "Alt-F7"),
 KEY(DEBUG_PREFIX, CtrlG, BOTH, WPE_CTX_PROGRAM, 0, "Ctrl-G"),

 /* ---- WPE_CTX_DEBUG ---- */
 KEY(DEBUG_TRACE, F7, BOTH, WPE_CTX_DEBUG, B | M, "F7"),
 KEY(DEBUG_STEP, F8, BOTH, WPE_CTX_DEBUG, B | M, "F8"),
 KEY(DEBUG_RUN, CF10, BOTH, WPE_CTX_DEBUG, B | M, "Ctrl-F10"),
 KEY(DEBUG_QUIT, CF2, BOTH, WPE_CTX_DEBUG, B | M, "Ctrl-F2"),
 KEY(DEBUG_PAUSE, CtrlBrk, BOTH, WPE_CTX_DEBUG, B | M, "Ctrl-Brk"),
 KEY(DEBUG_BREAKPOINT, F5, CU, WPE_CTX_DEBUG, M, "F5"),
 KEY(DEBUG_WATCH, CF5, CU, WPE_CTX_DEBUG, B | M, "Ctrl-F5"),
 KEY(DEBUG_STACK, CF3, CU, WPE_CTX_DEBUG, M, "Ctrl-F3"),
 KEY(DEBUG_GOTO_CURSOR, F4, BO, WPE_CTX_DEBUG, B, "F4"),
 KEY(DEBUG_BREAKPOINT, CF8, BO, WPE_CTX_DEBUG, M, "Ctrl-F8"),
 KEY(DEBUG_WATCH, CF7, BO, WPE_CTX_DEBUG, B | M, "Ctrl-F7"),
 KEY(DEBUG_STACK, CF6, BO, WPE_CTX_DEBUG, M, "Ctrl-F6"),

 /* ---- WPE_CTX_EDIT ---- */
 KEY(K_PREFIX, CtrlK, BOTH, WPE_CTX_EDIT, 0, "Ctrl-K"),
 KEY(O_PREFIX, CtrlO, BOTH, WPE_CTX_EDIT, 0, "Ctrl-O"),
 KEY(GOTO_LINE, AltG, BOTH, WPE_CTX_EDIT, M, "Alt-G"),
 KEY(SHOW_CLIPBOARD, CF10, BOTH, WPE_CTX_EDIT, 0, "Ctrl-F10"),
 KEY(SHOW_CLIPBOARD, AltY, BOTH, WPE_CTX_EDIT, M, "Alt-Y"),
 KEY(BLOCK_DELETE, CtrlDel, BOTH, WPE_CTX_EDIT, M, "Ctrl-Del"),
 KEY(EDIT_UNDO, UNDO, BOTH, WPE_CTX_EDIT, 0, "Sun Undo"),
 KEY(EDIT_UNDO, AltBS, BOTH, WPE_CTX_EDIT, 0, "Alt-Backspace"),
 KEY(EDIT_UNDO, CtrlU, BOTH, WPE_CTX_EDIT, M, "Ctrl-U"),
 KEY(EDIT_REDO, AGAIN, BOTH, WPE_CTX_EDIT, 0, "Sun Again"),
 KEY(EDIT_REDO, SABS, BOTH, WPE_CTX_EDIT, 0, "Alt-Shift-Backspace"),
 KEY(EDIT_REDO, CtrlR, BOTH, WPE_CTX_EDIT, M, "Ctrl-R"),
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
 /* the CUA style */
 KEY(SAVE, AF2, CU, WPE_CTX_EDIT, B | M, "Alt-F2"),
 KEY(FIND, FID, CU, WPE_CTX_EDIT, 0, "Sun Find"),
 KEY(FIND, AF3, CU, WPE_CTX_EDIT, B | M, "Alt-F3"),
 KEY(REPLACE, SF3, CU, WPE_CTX_EDIT, M, "Shift-F3"),
 KEY(SEARCH_AGAIN, F3, CU, WPE_CTX_EDIT, B | M, "F3"),
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

 /* ---- WPE_CTX_PREFIX_O: the letter after Ctrl-O ---- */
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
 RANGE(MARKER_GOTO, '0', '9', BOTH, WPE_CTX_PREFIX_O, M, NULL),

 /* ---- WPE_CTX_PREFIX_DEBUG: the letter after Ctrl-G, lower case or control ---- */
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
 KEY(CLOSE_LIST, CF4, CU, WPE_CTX_LIST, 0, "Ctrl-F4"),

 /* ---- WPE_CTX_DIALOG_MOVE: a dialog's move mode ---- */
 KEY(SIZE_MOVE, AF2, BO, WPE_CTX_DIALOG_MOVE, 0, "Alt-F2"),
 KEY(SIZE_MOVE, CtrlL, CU, WPE_CTX_DIALOG_MOVE, 0, "Ctrl-L")
};

#undef BO
#undef CU
#undef BOTH
#undef B
#undef M
#undef H
#undef CT
#undef KEY
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
 "Dialog move"
};

static const char *const wpe_context_prefixes[WPE_CTX_COUNT] = {
 NULL, NULL, NULL, NULL, "Ctrl-K", "Ctrl-O", "Ctrl-G", "Alt-Q", "Alt-G",
 NULL, NULL, NULL, NULL
};

/** The control code of a lower-case letter: 'p' gives 16. */
static int wpe_control_of(int letter)
{
 return(letter - 'a' + 1);
}

/** Does the row bind @code in @style and @context? */
static int wpe_row_matches(const WpeBinding *row, int code, int style, int context)
{
 if (row->context != context || !(row->styles & WPE_STYLE_BIT(style)))
  return(0);
 if (code >= row->code && code <= row->code_last)
  return(1);
 if ((row->flags & WPE_ROW_CTRL) && code == wpe_control_of(row->code))
  return(1);
 return(0);
}

int wpe_binding_style(int edopt)
{
 return((edopt & ED_CUA_STYLE) ? WPE_STYLE_CUA : WPE_STYLE_BORLAND);
}

WpeAction wpe_binding_action(int code, int style, int context)
{
 size_t i;

 for (i = 0; i < WPE_BINDING_COUNT; i++)
  if (wpe_row_matches(&wpe_bindings[i], code, style, context))
   return((WpeAction)wpe_bindings[i].action);
 return(WPE_ACT_NONE);
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

int wpe_binding_fold(int code, int style, int context)
{
 if (wpe_binding_action(code, style, context) != WPE_ACT_NONE)
  return(code);
 return(wpe_legacy_key(code));
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
