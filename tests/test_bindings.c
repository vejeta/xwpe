/* test_bindings.c -- unit test for the key bindings (we_bindings.c) and the
 * bottom bars built from them (we_bars.c).
 *
 * Links those two files only: no editor, no display, always runs.
 *
 * The first part proves the table does what the dispatchers did before the
 * keys became data.  The old dispatchers are kept here as functions that
 * return the name of what they ran: oracle_editor() is e_tst_dfkt, e_prog_switch,
 * e_debug_switch and e_tst_fkt in the order the editor consults them, and the
 * others are the letter prefixes and the two single-key questions.  Every key
 * code from -2 to 2100 is put to both, in both styles, with and without the
 * programming layer, and the answers must be equal.  The second part does the
 * same for every bottom bar: the label, the column, the highlight and the
 * code a click sends.  The third part checks the rules of the table itself.
 */
#include <stdio.h>
#include <string.h>

#include "../edit.h"
#include "../we_bindings.h"

static int fails;

static void fail(const char *what, int code, const char *got, const char *want)
{
	if (fails < 60)
		printf("FAIL: %s, code %d: got %s want %s\n", what, code, got, want);
	fails++;
}

/* ------------------------------------------------------------------ */
/* the dispatchers as they were                                        */

static const char *oracle_window(int c, int cua)
{
	static char buf[32];

	if (c >= Alt1 && c <= Alt9) {
		snprintf(buf, sizeof buf, "e_switch_window(%d)", c - Alt1 + 1);
		return buf;
	}
	if (c >= 1024 && c <= 1049) {
		snprintf(buf, sizeof buf, "e_switch_window(%d)", c - 1014);
		return buf;
	}
	switch (c) {
	case F1:
	case HELP:
		return "e_help_loc";
	case CF1:
		return "e_topic_search";
	case AF1:
		return "e_funct_in";
	case WPE_ESC:
		return "esc";
	case F10:
	case AltBl:
		return "menu";
	case Alt0:
		return "e_list_all_win";
	case AF5:
		return "e_user_screen";
	default:
		break;
	}
	if (cua) {
		switch (c) {
		case CtrlL:
			return "e_size_move";
		case OPEN:
		case F2:
			return "WpeManager";
		case SF2:
			return "WpeManagerFirst";
		case CF4:
		case AltX:
		case CtrlW:
			return "e_close_window";
		case FRONT:
		case AltZ:
		case SF4:
			return "e_ed_cascade";
		case SF5:
			return "e_ed_tile";
		case SF6:
			return "e_ed_zoom";
		case CF6:
		case AltN:
			return "e_ed_next";
		case STOP:
		case AF4:
			return "e_quit";
		default:
			return NULL;
		}
	}
	switch (c) {
	case CF5:
	case AF2:
		return "e_size_move";
	case OPEN:
	case F3:
		return "WpeManager";
	case CF3:
		return "WpeManagerFirst";
	case CtrlW:
		return "e_close_window";
	case FRONT:
	case AltZ:
	case F5:
		return "e_ed_zoom";
	case F6:
	case AltN:
		return "e_ed_next";
	case STOP:
	case AltX:
		return "e_quit";
	default:
		return NULL;
	}
}

static const char *oracle_debug(int c, int cua)
{
	switch (c) {
	case F7:
		return "e_deb_trace";
	case F8:
		return "e_deb_next";
	case CF10:
		return "e_deb_run";
	case CF2:
		return "e_d_q_quit";
	case CtrlBrk:
		return "e_d_pause";
	default:
		break;
	}
	if (cua) {
		switch (c) {
		case F5:
			return "e_breakpoint";
		case CF5:
			return "e_make_watches";
		case CF3:
			return "e_deb_stack";
		default:
			return NULL;
		}
	}
	switch (c) {
	case F4:
		return "e_d_goto_cursor";
	case CF8:
		return "e_breakpoint";
	case CF7:
		return "e_make_watches";
	case CF6:
		return "e_deb_stack";
	default:
		return NULL;
	}
}

static const char *oracle_program(int c, int cua)
{
	switch (c) {
	case AltU:
	case CF9:
		return "e_run";
	case AltQ:
		return "e_lsp_ui_key";
	case WPE_LSP_MENU:
		return "e_lsp_ui_menu";
	case AltM:
	case F9:
		return "e_p_make";
	case AltC:
	case AF9:
		return "e_compile";
	case AltL:
		return "e_install";
	case AltA:
		return "e_exec_make";
#ifdef WPE_AI
	case AltG:
		return "e_ai_ui_key";
	case WPE_AI_MENU:
		return "e_ai_ui_key";
#endif
	case AltT:
	case AF8:
		return "e_next_error";
	case AltV:
	case AF7:
		return "e_previous_error";
	case CtrlG:
		return "e_deb_inp";
	default:
		return oracle_debug(c, cua);
	}
}

static const char *oracle_edit(int c, int cua)
{
	switch (c) {
	case CtrlK:
		return "e_ctrl_k";
	case CtrlO:
		return "e_ctrl_o";
	case AltG:
		return "e_goto_line";
	case CF10:
	case AltY:
		return "e_show_clipboard";
	case CtrlDel:
		return "e_blck_del";
	case UNDO:
	case AltBS:
	case CtrlU:
		return "e_make_undo";
	case AGAIN:
	case SABS:
	case CtrlR:
		return "e_make_redo";
	case CUT:
	case ShiftDel:
	case CtrlX:
		return "e_edt_del";
	case PASTE:
	case ShiftEin:
	case AltEin:
	case CtrlV:
		return "e_edt_einf";
	case CtrlEin:
	case AltDel:
	case COPY:
	case CtrlC:
		return "e_edt_copy";
	default:
		break;
	}
	if (cua) {
		switch (c) {
		case AF2:
			return "e_m_save";
		case FID:
		case AF3:
			return "e_find";
		case SF3:
			return "e_replace";
		case F3:
			return "e_rep_search";
		default:
			return NULL;
		}
	}
	switch (c) {
	case F2:
		return "e_m_save";
	case FID:
	case F4:
		return "e_find";
	case AF4:
		return "e_replace";
	case CtrlL:
	case CF4:
		return "e_rep_search";
	default:
		return NULL;
	}
}

/** What the editor ran for a key: the general keys first, then, in the
    programming layer, the programming and debugger keys, then the editing
    keys. */
static const char *oracle_editor(int c, int cua, int prog)
{
	const char *r = oracle_window(c, cua);

	if (r)
		return r;
	if (prog && (r = oracle_program(c, cua)))
		return r;
	return oracle_edit(c, cua);
}

static const char *oracle_ctrl_k(int c)
{
	switch (c) {
	case 'A': return "cursor_from_begin";
	case 'B': return "e_blck_begin";
	case 'C': return "e_blck_copy";
	case 'D': return "e_changecase_dialog";
	case 'F': return "e_mk_beauty";
	case 'H': return "e_blck_hide";
	case 'I': return "e_blck_to_right";
	case 'K': return "mark_end";
	case 'L': return "block_line";
	case 'R': return "e_blck_read";
	case 'U': return "e_blck_to_left";
	case 'V': return "e_blck_move";
	case 'W': return "e_blck_write";
	case 'X': return "block_all";
	case 'Y': return "e_blck_del";
	case 'Z': return "cursor_from_end";
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
		return "marker_set";
	default: return NULL;
	}
}

static const char *oracle_ctrl_o(int c)
{
	switch (c) {
	case 'Y': return "delete_to_eol";
	case 'T': return "delete_word_left";
	case 'F': return "e_find";
	case 'A': return "e_replace";
	case 'S': return "e_sh_def";
	case 'N': return "e_sh_nxt_def";
	case 'K': return "e_nxt_brk";
	case 'B': return "e_p_beautify";
	case 'U': return "help_mark_button";
	case 'M': return "help_mark_line";
	case 'H': return "help_mark_header";
	case 'E': return "help_mark_end";
	case 'L': return "help_marks_delete";
	case 'C': return "e_help_comp";
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
		return "marker_goto";
	default: return NULL;
	}
}

static const char *oracle_ctrl_g(int c)
{
	switch (c) {
	case 'p': case ('p' - 'a' + 1): return "e_deb_out";
	case 'o': case ('o' - 'a' + 1): return "e_deb_options";
	case 'b': case ('b' - 'a' + 1): return "e_breakpoint";
	case 'm': case ('m' - 'a' + 1): return "e_remove_breakpoints";
	case 'a': case 1: return "e_remove_all_watches";
	case 'd': case ('d' - 'a' + 1): return "e_delete_watches";
	case 'w': case ('w' - 'a' + 1): return "e_make_watches";
	case 'e': case ('e' - 'a' + 1): return "e_edit_watches";
	case 'k': case ('k' - 'a' + 1): return "e_deb_stack";
	case 't': case ('t' - 'a' + 1): return "e_deb_trace";
	case 's': case ('s' - 'a' + 1): return "e_deb_next";
	case 'r': case 'c': case ('r' - 'a' + 1): case ('c' - 'a' + 1): return "e_deb_run";
	case 'q': case ('q' - 'a' + 1): return "e_d_quit";
	case 'u': case ('u' - 'a' + 1): return "e_d_goto_cursor";
	case 'f': case ('f' - 'a' + 1): return "e_d_finish_func";
	case 'i': case ('i' - 'a' + 1): return "e_d_pause";
	default: return NULL;
	}
}

/** The help viewer's page keys, which e_tst_cur ran for a help window. */
static const char *oracle_help(int c)
{
	switch (c) {
	case AF7: case AltV: return "e_help_next(0)";
	case AF8: case AltT: return "e_help_next(1)";
	default: return NULL;
	}
}

/** The letters after Alt-Q: e_lsp_ui_dispatch as it was. */
static const char *oracle_prefix_q(int c)
{
	switch (c) {
	case 'd': case ('d' - 'a' + 1): return "definition";
	case 'i': case ('i' - 'a' + 1): return "implementation";
	case 't': case ('t' - 'a' + 1): return "type_definition";
	case 'h': case ('h' - 'a' + 1): return "hover";
	case 'c': case ('c' - 'a' + 1): return "complete";
	case 'r': case ('r' - 'a' + 1): return "references";
	case 'b': case ('b' - 'a' + 1): return "callers";
	case 'g': case ('g' - 'a' + 1): return "callees";
	case 'k': case ('k' - 'a' + 1): return "supertypes";
	case 'j': case ('j' - 'a' + 1): return "subtypes";
	case 'v': case ('v' - 'a' + 1): return "expand_selection";
	case 'u': case ('u' - 'a' + 1): return "highlight";
	case 'y': case ('y' - 'a' + 1): return "inlay";
	case 'm': case ('m' - 'a' + 1): return "semantic";
	case 'l': case ('l' - 'a' + 1): return "codelens";
	case 's': case ('s' - 'a' + 1): return "signature";
	case 'w': case ('w' - 'a' + 1): return "workspace_symbols";
	case 'a': case ('a' - 'a' + 1): return "code_actions";
	case 'f': case ('f' - 'a' + 1): return "format";
	case 'n': case ('n' - 'a' + 1): return "rename";
	case 'o': case ('o' - 'a' + 1): return "outline";
	case 'e': case ('e' - 'a' + 1): return "diagnostics";
	case '.': return "next_problem";
	case ',': return "prev_problem";
	case WPE_ESC: return "cancel";
	default: return NULL;
	}
}

/** The letters of the assistant's menu after upper-casing: its rows' keys. */
static const char *oracle_prefix_ai(int c)
{
	switch (c) {
	case 'A': return "chat";
	case 'E': return "edit";
	case 'C': return "complete";
	case 'F': return "plan";
	case 'G': return "agent";
	case 'B': return "fix_build";
	case 'Y': return "policy";
	case 'R': return "read_folders";
	case 'N': return "new_session";
	case 'S': return "settings";
	default: return NULL;
	}
}

/** The Watches window's own keys (e_d_is_watch). */
static const char *oracle_watches(int c)
{
	if (c == EINFG)
		return "make_watches";
	if (c == ENTF)
		return "delete_watches";
	return NULL;
}

static const char *table_prefix_q(int c, int style)
{
	switch (wpe_binding_action(c, style, WPE_CTX_PREFIX_LSP)) {
	case WPE_ACT_LSP_DEFINITION: return "definition";
	case WPE_ACT_LSP_IMPLEMENTATION: return "implementation";
	case WPE_ACT_LSP_TYPE_DEFINITION: return "type_definition";
	case WPE_ACT_LSP_HOVER: return "hover";
	case WPE_ACT_LSP_COMPLETE: return "complete";
	case WPE_ACT_LSP_REFERENCES: return "references";
	case WPE_ACT_LSP_CALLERS: return "callers";
	case WPE_ACT_LSP_CALLEES: return "callees";
	case WPE_ACT_LSP_SUPERTYPES: return "supertypes";
	case WPE_ACT_LSP_SUBTYPES: return "subtypes";
	case WPE_ACT_LSP_EXPAND_SELECTION: return "expand_selection";
	case WPE_ACT_LSP_HIGHLIGHT: return "highlight";
	case WPE_ACT_LSP_INLAY: return "inlay";
	case WPE_ACT_LSP_SEMANTIC: return "semantic";
	case WPE_ACT_LSP_CODELENS: return "codelens";
	case WPE_ACT_LSP_SIGNATURE: return "signature";
	case WPE_ACT_LSP_WORKSPACE_SYMBOLS: return "workspace_symbols";
	case WPE_ACT_LSP_CODE_ACTIONS: return "code_actions";
	case WPE_ACT_LSP_FORMAT: return "format";
	case WPE_ACT_LSP_RENAME: return "rename";
	case WPE_ACT_LSP_OUTLINE: return "outline";
	case WPE_ACT_LSP_DIAGNOSTICS: return "diagnostics";
	case WPE_ACT_LSP_NEXT_PROBLEM: return "next_problem";
	case WPE_ACT_LSP_PREV_PROBLEM: return "prev_problem";
	case WPE_ACT_LSP_CANCEL: return "cancel";
	default: return NULL;
	}
}

static const char *table_prefix_ai(int c, int style)
{
	switch (wpe_binding_action(c, style, WPE_CTX_PREFIX_AI)) {
	case WPE_ACT_AI_ASK: return "chat";
	case WPE_ACT_AI_EDIT: return "edit";
	case WPE_ACT_AI_COMPLETE: return "complete";
	case WPE_ACT_AI_PLAN: return "plan";
	case WPE_ACT_AI_AGENT: return "agent";
	case WPE_ACT_AI_FIX_BUILD: return "fix_build";
	case WPE_ACT_AI_POLICY: return "policy";
	case WPE_ACT_AI_READ_FOLDERS: return "read_folders";
	case WPE_ACT_AI_NEW_SESSION: return "new_session";
	case WPE_ACT_AI_SETTINGS: return "settings";
	default: return NULL;
	}
}

static const char *table_watches(int c, int style)
{
	switch (wpe_binding_action(c, style, WPE_CTX_WATCHES)) {
	case WPE_ACT_DEBUG_WATCH: return "make_watches";
	case WPE_ACT_DEBUG_DELETE_WATCH: return "delete_watches";
	default: return NULL;
	}
}

static int oracle_list_close(int c, int cua)
{
	return cua ? c == CF4 : c == AF3;
}

static int oracle_dialog_move(int c, int cua)
{
	return cua ? c == CtrlL : c == AF2;
}

/* ------------------------------------------------------------------ */
/* the same questions put to the table                                 */

/** The name of what a dispatcher runs for an action: what the oracle's
    functions return. */
static const char *run_name(int act)
{
	switch (act) {
	case WPE_ACT_HELP_EDITOR: return "e_help_loc";
	case WPE_ACT_TOPIC_SEARCH: return "e_topic_search";
	case WPE_ACT_FUNCTION_INDEX: return "e_funct_in";
	case WPE_ACT_ESCAPE: return "esc";
	case WPE_ACT_MENU_BAR: return "menu";
	case WPE_ACT_LIST_WINDOWS: return "e_list_all_win";
	case WPE_ACT_USER_SCREEN: return "e_user_screen";
	case WPE_ACT_SIZE_MOVE: return "e_size_move";
	case WPE_ACT_FILE_MANAGER: return "WpeManager";
	case WPE_ACT_FILE_MANAGER_FIRST: return "WpeManagerFirst";
	case WPE_ACT_CLOSE_WINDOW: return "e_close_window";
	case WPE_ACT_CASCADE: return "e_ed_cascade";
	case WPE_ACT_TILE: return "e_ed_tile";
	case WPE_ACT_ZOOM: return "e_ed_zoom";
	case WPE_ACT_NEXT_WINDOW: return "e_ed_next";
	case WPE_ACT_QUIT: return "e_quit";
	case WPE_ACT_RUN: return "e_run";
	case WPE_ACT_LSP_PREFIX: return "e_lsp_ui_key";
	case WPE_ACT_LSP_MENU: return "e_lsp_ui_menu";
	case WPE_ACT_MAKE: return "e_p_make";
	case WPE_ACT_COMPILE: return "e_compile";
	case WPE_ACT_INSTALL: return "e_install";
	case WPE_ACT_EXEC_MAKE: return "e_exec_make";
#ifdef WPE_AI
	case WPE_ACT_AI_PREFIX: return "e_ai_ui_key";
	case WPE_ACT_AI_MENU: return "e_ai_ui_key";
#endif
	case WPE_ACT_NEXT_ERROR: return "e_next_error";
	case WPE_ACT_PREV_ERROR: return "e_previous_error";
	case WPE_ACT_DEBUG_PREFIX: return "e_deb_inp";
	case WPE_ACT_DEBUG_TRACE: return "e_deb_trace";
	case WPE_ACT_DEBUG_STEP: return "e_deb_next";
	case WPE_ACT_DEBUG_RUN: return "e_deb_run";
	case WPE_ACT_DEBUG_QUIT: return "e_d_q_quit";
	case WPE_ACT_DEBUG_PAUSE: return "e_d_pause";
	case WPE_ACT_DEBUG_BREAKPOINT: return "e_breakpoint";
	case WPE_ACT_DEBUG_WATCH: return "e_make_watches";
	case WPE_ACT_DEBUG_STACK: return "e_deb_stack";
	case WPE_ACT_DEBUG_GOTO_CURSOR: return "e_d_goto_cursor";
	case WPE_ACT_K_PREFIX: return "e_ctrl_k";
	case WPE_ACT_O_PREFIX: return "e_ctrl_o";
	case WPE_ACT_GOTO_LINE: return "e_goto_line";
	case WPE_ACT_SHOW_CLIPBOARD: return "e_show_clipboard";
	case WPE_ACT_BLOCK_DELETE: return "e_blck_del";
	case WPE_ACT_EDIT_UNDO: return "e_make_undo";
	case WPE_ACT_EDIT_REDO: return "e_make_redo";
	case WPE_ACT_EDIT_CUT: return "e_edt_del";
	case WPE_ACT_EDIT_PASTE: return "e_edt_einf";
	case WPE_ACT_EDIT_COPY: return "e_edt_copy";
	case WPE_ACT_SAVE: return "e_m_save";
	case WPE_ACT_FIND: return "e_find";
	case WPE_ACT_REPLACE: return "e_replace";
	case WPE_ACT_SEARCH_AGAIN: return "e_rep_search";
	default: return NULL;
	}
}

static const char *table_editor(int c, int style, int prog)
{
	static char buf[32];
	int n = wpe_binding_window_number(c);
	const char *r;

	if (n > 0) {
		snprintf(buf, sizeof buf, "e_switch_window(%d)", n);
		return buf;
	}
	if ((r = run_name(wpe_binding_action(c, style, WPE_CTX_WINDOW))))
		return r;
	if (prog) {
		if ((r = run_name(wpe_binding_action(c, style, WPE_CTX_PROGRAM))))
			return r;
		if ((r = run_name(wpe_binding_action(c, style, WPE_CTX_DEBUG))))
			return r;
	}
	return run_name(wpe_binding_action(c, style, WPE_CTX_EDIT));
}

static const char *table_ctrl_k(int c, int style)
{
	switch (wpe_binding_action(c, style, WPE_CTX_PREFIX_K)) {
	case WPE_ACT_BLOCK_GOTO_BEGIN: return "cursor_from_begin";
	case WPE_ACT_BLOCK_BEGIN: return "e_blck_begin";
	case WPE_ACT_BLOCK_COPY: return "e_blck_copy";
	case WPE_ACT_BLOCK_CASE: return "e_changecase_dialog";
	case WPE_ACT_BLOCK_BEAUTIFY: return "e_mk_beauty";
	case WPE_ACT_BLOCK_HIDE: return "e_blck_hide";
	case WPE_ACT_BLOCK_INDENT: return "e_blck_to_right";
	case WPE_ACT_BLOCK_END: return "mark_end";
	case WPE_ACT_BLOCK_LINE: return "block_line";
	case WPE_ACT_BLOCK_READ: return "e_blck_read";
	case WPE_ACT_BLOCK_UNINDENT: return "e_blck_to_left";
	case WPE_ACT_BLOCK_MOVE: return "e_blck_move";
	case WPE_ACT_BLOCK_WRITE: return "e_blck_write";
	case WPE_ACT_BLOCK_ALL: return "block_all";
	case WPE_ACT_BLOCK_DELETE: return "e_blck_del";
	case WPE_ACT_BLOCK_GOTO_END: return "cursor_from_end";
	case WPE_ACT_MARKER_SET: return "marker_set";
	default: return NULL;
	}
}

static const char *table_ctrl_o(int c, int style)
{
	switch (wpe_binding_action(c, style, WPE_CTX_PREFIX_O)) {
	case WPE_ACT_DELETE_TO_EOL: return "delete_to_eol";
	case WPE_ACT_DELETE_WORD_LEFT: return "delete_word_left";
	case WPE_ACT_FIND: return "e_find";
	case WPE_ACT_REPLACE: return "e_replace";
	case WPE_ACT_SHOW_DEFINITION: return "e_sh_def";
	case WPE_ACT_NEXT_DEFINITION: return "e_sh_nxt_def";
	case WPE_ACT_MATCH_BRACKET: return "e_nxt_brk";
	case WPE_ACT_BEAUTIFY: return "e_p_beautify";
	case WPE_ACT_HELP_MARK_BUTTON: return "help_mark_button";
	case WPE_ACT_HELP_MARK_LINE: return "help_mark_line";
	case WPE_ACT_HELP_MARK_HEADER: return "help_mark_header";
	case WPE_ACT_HELP_MARK_END: return "help_mark_end";
	case WPE_ACT_HELP_MARKS_DELETE: return "help_marks_delete";
	case WPE_ACT_HELP_CHECK: return "e_help_comp";
	case WPE_ACT_MARKER_GOTO: return "marker_goto";
	default: return NULL;
	}
}

static const char *table_ctrl_g(int c, int style)
{
	switch (wpe_binding_action(c, style, WPE_CTX_PREFIX_DEBUG)) {
	case WPE_ACT_DEBUG_OUTPUT: return "e_deb_out";
	case WPE_ACT_DEBUG_OPTIONS: return "e_deb_options";
	case WPE_ACT_DEBUG_BREAKPOINT: return "e_breakpoint";
	case WPE_ACT_DEBUG_REMOVE_BREAKPOINTS: return "e_remove_breakpoints";
	case WPE_ACT_DEBUG_REMOVE_ALL_WATCHES: return "e_remove_all_watches";
	case WPE_ACT_DEBUG_DELETE_WATCH: return "e_delete_watches";
	case WPE_ACT_DEBUG_WATCH: return "e_make_watches";
	case WPE_ACT_DEBUG_EDIT_WATCH: return "e_edit_watches";
	case WPE_ACT_DEBUG_STACK: return "e_deb_stack";
	case WPE_ACT_DEBUG_TRACE: return "e_deb_trace";
	case WPE_ACT_DEBUG_STEP: return "e_deb_next";
	case WPE_ACT_DEBUG_RUN: return "e_deb_run";
	case WPE_ACT_DEBUG_QUIT: return "e_d_quit";
	case WPE_ACT_DEBUG_GOTO_CURSOR: return "e_d_goto_cursor";
	case WPE_ACT_DEBUG_FINISH: return "e_d_finish_func";
	case WPE_ACT_DEBUG_PAUSE: return "e_d_pause";
	default: return NULL;
	}
}

static const char *table_help(int c, int style)
{
	switch (wpe_binding_action(c, style, WPE_CTX_HELP)) {
	case WPE_ACT_HELP_PREV: return "e_help_next(0)";
	case WPE_ACT_HELP_NEXT: return "e_help_next(1)";
	default: return NULL;
	}
}

static void same(const char *what, int c, const char *got, const char *want)
{
	if (got == want)
		return;
	if (got && want && strcmp(got, want) == 0)
		return;
	fail(what, c, got ? got : "(nothing)", want ? want : "(nothing)");
}

#define CODE_LOW (-2)
#define CODE_HIGH 2100

static void test_dispatch(void)
{
	int style, prog, c;
	char what[64];

	/* The oracle is the Borland dispatchers as they were before the keys became
	   data, and the Borland style must still be exactly that.  The second style
	   changed on purpose when it became Modern: test_modern_table() lists what
	   it binds now. */
	for (style = 0; style < WPE_STYLE_COUNT; style++) {
		if (style == WPE_STYLE_MODERN)
			continue;
		for (prog = 0; prog <= 1; prog++) {
			snprintf(what, sizeof what, "editor style %d prog %d", style, prog);
			for (c = CODE_LOW; c < CODE_HIGH; c++)
				same(what, c, table_editor(c, style, prog),
				     oracle_editor(c, 0, prog));
		}
		snprintf(what, sizeof what, "Ctrl-K style %d", style);
		for (c = CODE_LOW; c < CODE_HIGH; c++)
			same(what, c, table_ctrl_k(c, style), oracle_ctrl_k(c));
		snprintf(what, sizeof what, "Ctrl-O style %d", style);
		for (c = CODE_LOW; c < CODE_HIGH; c++)
			same(what, c, table_ctrl_o(c, style), oracle_ctrl_o(c));
		snprintf(what, sizeof what, "Ctrl-G style %d", style);
		for (c = CODE_LOW; c < CODE_HIGH; c++)
			same(what, c, table_ctrl_g(c, style), oracle_ctrl_g(c));
		snprintf(what, sizeof what, "Alt-Q style %d", style);
		for (c = CODE_LOW; c < CODE_HIGH; c++)
			same(what, c, table_prefix_q(c, style), oracle_prefix_q(c));
		snprintf(what, sizeof what, "Alt-G style %d", style);
		for (c = CODE_LOW; c < CODE_HIGH; c++)
			same(what, c, table_prefix_ai(c, style), oracle_prefix_ai(c));
		snprintf(what, sizeof what, "Watches style %d", style);
		for (c = CODE_LOW; c < CODE_HIGH; c++)
			same(what, c, table_watches(c, style), oracle_watches(c));
		snprintf(what, sizeof what, "help style %d", style);
		for (c = CODE_LOW; c < CODE_HIGH; c++)
			same(what, c, table_help(c, style), oracle_help(c));
		for (c = CODE_LOW; c < CODE_HIGH; c++) {
			int list = wpe_binding_action(c, style, WPE_CTX_LIST) == WPE_ACT_CLOSE_LIST;
			int move = wpe_binding_action(c, style, WPE_CTX_DIALOG_MOVE) == WPE_ACT_SIZE_MOVE;

			if (list != oracle_list_close(c, 0))
				fail("list close", c, list ? "yes" : "no", "the other");
			if (move != oracle_dialog_move(c, 0))
				fail("dialog move", c, move ? "yes" : "no", "the other");
		}
	}
	/* The key that closes a list is the one the first-code reader names. */
	if (wpe_binding_first_code(WPE_ACT_CLOSE_LIST, WPE_STYLE_BORLAND, WPE_CTX_LIST) != AF3)
		fail("list close code", 0, "?", "Alt-F3");
	if (wpe_binding_first_code(WPE_ACT_CLOSE_LIST, WPE_STYLE_MODERN, WPE_CTX_LIST) != CF4)
		fail("list close code", 0, "?", "Ctrl-F4");
}

/* ------------------------------------------------------------------ */
/* the Modern style's own rows                                         */

typedef struct {
	int code;
	int context;
	int action;
	int session;        /* 0 no session, 1 a session is open, -1 either */
} ModernRow;

static int session_open;
static int probe_session(void) { return session_open; }

/* Every key of the Modern table of the design (3.2) that has an action in this
   build, in the context the editor reads it, and what it must do. */
static const ModernRow modern_rows[] = {
	{ CtrlS, WPE_CTX_EDIT, WPE_ACT_SAVE, -1 },
	{ CtrlZ, WPE_CTX_EDIT, WPE_ACT_EDIT_UNDO, -1 },
	{ CtrlU, WPE_CTX_EDIT, WPE_ACT_EDIT_UNDO, -1 },
	{ CtrlY, WPE_CTX_EDIT, WPE_ACT_EDIT_REDO, -1 },
	{ CtrlX, WPE_CTX_EDIT, WPE_ACT_EDIT_CUT, -1 },
	{ CtrlC, WPE_CTX_EDIT, WPE_ACT_EDIT_COPY, -1 },
	{ CtrlV, WPE_CTX_EDIT, WPE_ACT_EDIT_PASTE, -1 },
	{ CtrlA, WPE_CTX_EDIT, WPE_ACT_BLOCK_ALL, -1 },
	{ CtrlL, WPE_CTX_EDIT, WPE_ACT_BLOCK_LINE, -1 },
	{ CtrlD, WPE_CTX_EDIT, WPE_ACT_SELECT_WORD, -1 },
	{ CtrlF, WPE_CTX_EDIT, WPE_ACT_FIND, -1 },
	{ F4, WPE_CTX_EDIT, WPE_ACT_FIND, -1 },
	{ F3, WPE_CTX_EDIT, WPE_ACT_SEARCH_AGAIN, -1 },
	{ CtrlR, WPE_CTX_EDIT, WPE_ACT_REPLACE, -1 },
	{ WPE_CTRL_H, WPE_CTX_EDIT, WPE_ACT_REPLACE, -1 },
	{ CtrlG, WPE_CTX_EDIT, WPE_ACT_GOTO_LINE, -1 },
	{ CtrlK, WPE_CTX_EDIT, WPE_ACT_K_PREFIX, -1 },
	{ CtrlO, WPE_CTX_WINDOW, WPE_ACT_FILE_MANAGER, -1 },
	{ CtrlW, WPE_CTX_WINDOW, WPE_ACT_CLOSE_WINDOW, -1 },
	{ CF4, WPE_CTX_WINDOW, WPE_ACT_CLOSE_WINDOW, -1 },
	{ CtrlQ, WPE_CTX_WINDOW, WPE_ACT_QUIT, -1 },
	{ AltX, WPE_CTX_WINDOW, WPE_ACT_QUIT, -1 },
	{ F6, WPE_CTX_WINDOW, WPE_ACT_NEXT_WINDOW, -1 },
	{ WPE_CTRL_TAB, WPE_CTX_WINDOW, WPE_ACT_NEXT_WINDOW, -1 },
	{ F10, WPE_CTX_WINDOW, WPE_ACT_MENU_BAR, 0 },
	{ F10, WPE_CTX_DEBUG, WPE_ACT_DEBUG_STEP, 1 },
	{ F5, WPE_CTX_DEBUG, WPE_ACT_DEBUG_RUN, -1 },
	{ SF5, WPE_CTX_DEBUG, WPE_ACT_DEBUG_QUIT, -1 },
	{ F9, WPE_CTX_DEBUG, WPE_ACT_DEBUG_BREAKPOINT, -1 },
	{ F11, WPE_CTX_DEBUG, WPE_ACT_DEBUG_TRACE, -1 },
	{ SF11, WPE_CTX_DEBUG, WPE_ACT_DEBUG_FINISH, -1 },
	{ CF10, WPE_CTX_DEBUG, WPE_ACT_DEBUG_GOTO_CURSOR, -1 },
	{ F8, WPE_CTX_PROGRAM, WPE_ACT_NEXT_ERROR, 0 },
	{ SF8, WPE_CTX_PROGRAM, WPE_ACT_PREV_ERROR, 0 },
	{ F8, WPE_CTX_DEBUG, WPE_ACT_DEBUG_STEP, -1 },
	{ CF5, WPE_CTX_PROGRAM, WPE_ACT_RUN, -1 },
	{ AltM, WPE_CTX_PROGRAM, WPE_ACT_MAKE, -1 },
	{ F12, WPE_CTX_PROGRAM, WPE_ACT_LSP_DEFINITION, -1 },
	{ SF12, WPE_CTX_PROGRAM, WPE_ACT_LSP_REFERENCES, -1 },
	{ F2, WPE_CTX_PROGRAM, WPE_ACT_LSP_RENAME, -1 },
	{ WPE_CTRL_SPACE, WPE_CTX_PROGRAM, WPE_ACT_LSP_COMPLETE, -1 },
	{ AltQ, WPE_CTX_PROGRAM, WPE_ACT_LSP_PREFIX, -1 },
	{ AltG, WPE_CTX_PROGRAM, WPE_ACT_AI_PREFIX, -1 }
};

/** Keys the Modern style gives up, so they are bound to nothing in it. */
static const ModernRow modern_unbound[] = {
	{ CtrlP, WPE_CTX_CURSOR, WPE_ACT_NONE, -1 },
	{ CtrlN, WPE_CTX_CURSOR, WPE_ACT_NONE, -1 },
	{ CtrlB, WPE_CTX_CURSOR, WPE_ACT_NONE, -1 },
	{ CtrlA, WPE_CTX_CURSOR, WPE_ACT_NONE, -1 },
	{ CtrlT, WPE_CTX_CURSOR, WPE_ACT_NONE, -1 },
	{ CtrlZ, WPE_CTX_CURSOR, WPE_ACT_NONE, -1 },
	{ CtrlO, WPE_CTX_EDIT, WPE_ACT_NONE, -1 },
	{ F9, WPE_CTX_PROGRAM, WPE_ACT_NONE, -1 },
	{ F2, WPE_CTX_WINDOW, WPE_ACT_NONE, -1 }
};

static void test_modern_table(void)
{
	size_t i;
	int state;
	char what[64];

	wpe_binding_set_session_probe(probe_session);
	for (i = 0; i < sizeof(modern_rows) / sizeof(modern_rows[0]); i++) {
		const ModernRow *r = &modern_rows[i];

		for (state = 0; state <= 1; state++) {
			session_open = state;
			if (r->session != -1 && r->session != state)
				continue;
			if (wpe_binding_action(r->code, WPE_STYLE_MODERN, r->context) != r->action) {
				snprintf(what, sizeof what, "Modern code in context %d, session %d", r->context, state);
				fail(what, r->code, wpe_action_name(wpe_binding_action(r->code, WPE_STYLE_MODERN, r->context)),
				     wpe_action_name(r->action));
			}
		}
	}
	session_open = 0;
	for (i = 0; i < sizeof(modern_unbound) / sizeof(modern_unbound[0]); i++) {
		const ModernRow *r = &modern_unbound[i];

		if (wpe_binding_action(r->code, WPE_STYLE_MODERN, r->context) != WPE_ACT_NONE)
			fail("Modern key that must be free", r->code, "an action", "none");
	}
	/* a key that is state-bound does the other thing in the other state */
	session_open = 1;
	if (wpe_binding_action(F10, WPE_STYLE_MODERN, WPE_CTX_WINDOW) != WPE_ACT_NONE)
		fail("F10 with a session open still opens the menu", F10, "menu", "none");
	if (wpe_binding_action(F8, WPE_STYLE_MODERN, WPE_CTX_PROGRAM) != WPE_ACT_NONE)
		fail("F8 with a session open still goes to an error", F8, "next error", "none");
	session_open = 0;
	if (wpe_binding_action(F10, WPE_STYLE_MODERN, WPE_CTX_DEBUG) != WPE_ACT_NONE)
		fail("F10 with no session steps over", F10, "step", "none");
	/* the Borland style never sees the state: F10 is the menu either way */
	session_open = 1;
	if (wpe_binding_action(F10, WPE_STYLE_BORLAND, WPE_CTX_WINDOW) != WPE_ACT_MENU_BAR)
		fail("Borland F10 with a session open", F10, "none", "menu");
	session_open = 0;
	wpe_binding_set_session_probe(NULL);
	/* the Ctrl-K letter and chord rule */
	if (!wpe_binding_prefix_folds_control(WPE_STYLE_BORLAND) ||
	    wpe_binding_prefix_folds_control(WPE_STYLE_MODERN))
		fail("Ctrl-K control letter fold", 0, "wrong style", "Borland folds, Modern does not");
	/* the fold: Ctrl-Shift-letter reaches the editor as itself where Modern binds it, and as Ctrl-letter where it does not */
	if (wpe_binding_fold(WPE_CS_FIRST + 'B' - 'A', WPE_STYLE_MODERN, WPE_CTX_EDIT) != WPE_CS_FIRST + 'B' - 'A')
		fail("Modern Ctrl-Shift-B stays", 0, "folded", "kept");
	if (wpe_binding_fold(WPE_CS_FIRST + 'B' - 'A', WPE_STYLE_BORLAND, WPE_CTX_EDIT) != CtrlB)
		fail("Borland Ctrl-Shift-B folds", 0, "kept", "Ctrl-B");
	if (wpe_binding_fold(WPE_CTRL_H, WPE_STYLE_MODERN, WPE_CTX_BOX) != CtrlH)
		fail("Ctrl-H in a box is Backspace", 0, "kept", "Ctrl-H");
	if (wpe_binding_fold(WPE_CTRL_H, WPE_STYLE_MODERN, WPE_CTX_EDIT) != WPE_CTRL_H)
		fail("Modern Ctrl-H in the editor is its own key", 0, "folded", "kept");
	if (wpe_binding_fold(WPE_CTRL_H, WPE_STYLE_BORLAND, WPE_CTX_EDIT) != CtrlH)
		fail("Borland Ctrl-H in the editor is Backspace", 0, "kept", "Ctrl-H");
}

/* ------------------------------------------------------------------ */
/* the delivery table                                                  */

static void test_delivery(void)
{
	size_t n, i;
	const WpeDelivery *d = wpe_key_delivery(&n);

	if (n == 0)
		fail("delivery table", 0, "empty", "rows");
	for (i = 0; i < n; i++)
		if (!d[i].why || (strncmp(d[i].why, "measured", 8) && strncmp(d[i].why, "documented", 10)))
			fail("delivery row without a source", d[i].code, d[i].why ? d[i].why : "(none)", "measured or documented");
	/* measured: xterm answers modifyOtherKeys, Konsole and kitty the kitty protocol */
	if (wpe_emulator_protocols(WPE_EMU_XTERM) != WPE_PROTO_MODIFY_OTHER_KEYS)
		fail("xterm protocol", 0, "other", "modifyOtherKeys");
	if (wpe_emulator_protocols(WPE_EMU_KONSOLE) != WPE_PROTO_KITTY)
		fail("Konsole protocol", 0, "other", "kitty");
	if (wpe_emulator_protocols(WPE_EMU_VTE) != 0 || wpe_emulator_protocols(WPE_EMU_SCREEN) != 0)
		fail("VTE and screen implement neither", 0, "one", "none");
	/* Ctrl-Shift-S needs a protocol, so GNOME Terminal (VTE) cannot deliver it */
	if (wpe_binding_key_class(WPE_CS_FIRST + 'S' - 'A') != WPE_DELIVER_PROTOCOL)
		fail("Ctrl-Shift-S class", 0, "always", "protocol");
	if (wpe_key_reaches(WPE_CS_FIRST + 'S' - 'A', WPE_EMU_VTE) || !wpe_key_reaches(WPE_CS_FIRST + 'S' - 'A', WPE_EMU_XTERM))
		fail("Ctrl-Shift-S delivery", 0, "wrong", "xterm yes, VTE no");
	/* Konsole keeps Ctrl-Shift-letter even though it has a protocol */
	if (wpe_key_reaches(WPE_CS_FIRST + 'P' - 'A', WPE_EMU_KONSOLE))
		fail("Konsole Ctrl-Shift-P", 0, "delivered", "taken");
	if (wpe_binding_key_class(CtrlS) != WPE_DELIVER_ALWAYS || !wpe_key_reaches(CtrlS, WPE_EMU_VTE))
		fail("Ctrl-S arrives everywhere", 0, "no", "yes");
	if (wpe_key_reaches(CtrlS, WPE_EMU_SCREEN))
		fail("screen keeps Ctrl-S", 0, "delivered", "taken");
}

/* ------------------------------------------------------------------ */
/* the bottom bars as they were                                        */

typedef struct {
	const char *t;
	int x, s, n, as;
} OldEntry;

#define ROW(t, x, s, n, as) { t, x, s, n, as }

static const OldEntry old_eblst_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("F2 Save", 9, 0, 2, F2), ROW("F3 Files", 18, 0, 2, F3),
	ROW("^W Close W.", 28, 0, 2, CtrlW), ROW("F4 Search", 45, 0, 2, F4),
	/* a click on this entry sends Ctrl-L, which runs the same action as the
	   Ctrl-F4 it sent before */
	ROW("^L S.Again", 56, 0, 2, CtrlL), ROW("Alt-X Quit", 68, 0, 5, AltX) };
static const OldEntry old_eblst_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("^S Save", 9, 0, 2, CtrlS), ROW("^O Open", 18, 0, 2, CtrlO),
	ROW("^W Close", 27, 0, 2, CtrlW), ROW("^F Find", 37, 0, 2, CtrlF),
	ROW("F3 Next", 46, 0, 2, F3), ROW("^Q Quit", 67, 0, 2, CtrlQ) };
static const OldEntry modern_eblst_prog[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("^S Save", 9, 0, 2, CtrlS), ROW("^O Open", 18, 0, 2, CtrlO),
	ROW("^W Close", 27, 0, 2, CtrlW), ROW("^F Find", 37, 0, 2, CtrlF),
	ROW("F3 Next", 46, 0, 2, F3), ROW("Alt-M Make", 55, 0, 5, AltM),
	ROW("^Q Quit", 67, 0, 2, CtrlQ) };
static const OldEntry old_eblst_prog_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("F2 Save", 9, 0, 2, F2), ROW("F3 Files", 18, 0, 2, F3),
	ROW("^W Close", 28, 0, 2, CtrlW), ROW("F4 Search", 37, 0, 2, F4),
	ROW("^L Again", 48, 0, 2, CtrlL), ROW("F9 Build", 58, 0, 2, F9),
	ROW("Alt-X Quit", 68, 0, 5, AltX) };
static const OldEntry old_eblst_lsp_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("F2 Save", 8, 0, 2, F2), ROW("F3 Files", 16, 0, 2, F3),
	ROW("^W Close", 25, 0, 2, CtrlW), ROW("F4 Search", 34, 0, 2, F4),
	ROW("Alt-Q ? Metals", 44, 0, 7, WPE_LSP_MENU), ROW("F9 Build", 59, 0, 2, F9),
	ROW("Alt-X Quit", 68, 0, 5, AltX) };
static const OldEntry old_eblst_lsp_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("^S Save", 8, 0, 2, CtrlS), ROW("^O Open", 20, 0, 2, CtrlO),
	ROW("^W Close", 29, 0, 2, CtrlW), ROW("^F Find", 40, 0, 2, CtrlF),
	ROW("Alt-Q ? Metals", 52, 0, 7, WPE_LSP_MENU), ROW("^Q Quit", 67, 0, 2, CtrlQ) };
#ifdef WPE_AI
static const OldEntry old_eblst_ai_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("F2 Save", 9, 0, 2, F2), ROW("F3 Files", 18, 0, 2, F3),
	ROW("^W Close", 28, 0, 2, CtrlW), ROW("F4 Search", 37, 0, 2, F4), ROW("F9 Build", 47, 0, 2, F9),
	ROW("Alt-G AI", 56, 0, 5, WPE_AI_MENU), ROW("Alt-X Quit", 66, 0, 5, AltX) };
static const OldEntry old_eblst_ai_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("^S Save", 9, 0, 2, CtrlS), ROW("^O Open", 22, 0, 2, CtrlO),
	ROW("^W Close", 32, 0, 2, CtrlW), ROW("^F Find", 44, 0, 2, CtrlF),
	ROW("Alt-G AI", 57, 0, 5, WPE_AI_MENU), ROW("^Q Quit", 67, 0, 2, CtrlQ) };
static const OldEntry old_eblst_lspai_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("F2 Save", 8, 0, 2, F2), ROW("F3 Files", 16, 0, 2, F3),
	ROW("^W Close", 25, 0, 2, CtrlW), ROW("Alt-Q ? LSP", 34, 0, 7, WPE_LSP_MENU),
	ROW("Alt-G AI", 46, 0, 5, WPE_AI_MENU), ROW("F9 Build", 55, 0, 2, F9),
	ROW("Alt-X Quit", 64, 0, 5, AltX) };
static const OldEntry old_eblst_lspai_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("^S Save", 8, 0, 2, CtrlS), ROW("^O Open", 20, 0, 2, CtrlO),
	ROW("^W Close", 30, 0, 2, CtrlW), ROW("Alt-Q ? LSP", 40, 0, 7, WPE_LSP_MENU),
	ROW("Alt-G AI", 54, 0, 5, WPE_AI_MENU), ROW("^Q Quit", 67, 0, 2, CtrlQ) };
#endif
static const OldEntry old_fblst_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Edit", 10, 0, 1, AltE), ROW("Attributes", 17, 0, 1, AltA),
	ROW("Move", 29, 0, 1, AltM), ROW("COpy", 35, 1, 1, AltO), ROW("Remove", 41, 0, 1, AltR),
	ROW("^W Close W.", 49, 0, 2, CtrlW), ROW("Alt-X Quit", 66, 0, 5, AltX) };
static const OldEntry old_fblst_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Edit", 9, 0, 1, AltE), ROW("RePlace", 15, 2, 1, AltP),
	ROW("Move", 24, 0, 1, AltM), ROW("DUplicate", 30, 1, 1, AltU), ROW("Remove", 41, 0, 1, AltR),
	ROW("^W Close W.", 49, 0, 2, CtrlW), ROW("^Q Quit", 63, 0, 2, CtrlQ) };
static const OldEntry old_mblst_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("PreV. Err.", 9, 3, 1, AltV), ROW("NexT Err.", 21, 3, 1, AltT),
	ROW("Compile", 32, 0, 1, AltC), ROW("Make", 41, 0, 1, AltM), ROW("RUn", 47, 1, 1, AltU),
	ROW("^W Close", 52, 0, 2, CtrlW), ROW("Alt-X Quit", 66, 0, 5, AltX) };
static const OldEntry old_mblst_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("PreV. Err.", 9, 3, 1, AltV), ROW("NexT Err.", 21, 3, 1, AltT),
	ROW("Compile", 32, 0, 1, AltC), ROW("Make", 41, 0, 1, AltM), ROW("RUn", 47, 1, 1, AltU),
	ROW("^W Close", 52, 0, 2, CtrlW), ROW("^Q Quit", 63, 0, 2, CtrlQ) };
static const OldEntry old_dblst_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("^F7 Make Watch", 9, 0, 3, CF7), ROW("F7 Trace", 25, 0, 2, F7),
	ROW("F8 Step", 34, 0, 2, F8), ROW("^F10 Run", 43, 0, 4, CF10), ROW("^W Close", 53, 0, 2, CtrlW),
	ROW("Alt-X Quit", 67, 0, 5, AltX) };
static const OldEntry old_dblst_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("^F7 Make Watch", 9, 0, 3, CF7), ROW("F11 Trace", 25, 0, 3, F11),
	ROW("F10 Step", 34, 0, 3, F10), ROW("F5 Run", 43, 0, 2, F5), ROW("^W Close", 53, 0, 2, CtrlW),
	ROW("^Q Quit", 64, 0, 2, CtrlQ) };
/* The Modern style's bar for a source window while a session runs. */
static const OldEntry modern_dblst_src[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("F11 Into", 9, 0, 3, F11), ROW("F10 Over", 19, 0, 3, F10),
	ROW("^F10 Go to cursor", 29, 0, 4, CF10), ROW("^F7 Make Watch", 48, 0, 3, CF7),
	ROW("Shift-F5 Stop", 64, 0, 8, SF5) };
static const OldEntry modern_dblst_run[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("^Brk Pause", 9, 0, 4, CtrlBrk),
	ROW("Shift-F5 Stop", 21, 0, 8, SF5) };
static const OldEntry old_dblst_src_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("F7 Trace", 8, 0, 2, F7), ROW("F8 Step", 17, 0, 2, F8),
	ROW("F4 Go to cursor", 25, 0, 2, F4), ROW("^F7 Make Watch", 41, 0, 3, CF7),
	ROW("^F2 Quit", 56, 0, 3, CF2), ROW("Alt-X Quit", 66, 0, 5, AltX) };
static const OldEntry old_dblst_run_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("^Brk Pause", 9, 0, 4, CtrlBrk), ROW("^F2 Quit", 21, 0, 3, CF2) };
static const OldEntry old_xblst_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Execute", 19, 0, 1, AltE), ROW("^W Close W.", 38, 0, 2, CtrlW),
	ROW("Alt-X Quit", 66, 0, 5, AltX) };
static const OldEntry old_xblst_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Execute", 19, 0, 1, AltE), ROW("^W Close W.", 38, 0, 2, CtrlW),
	ROW("^Q Quit", 63, 0, 2, CtrlQ) };
static const OldEntry old_wblst_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Write", 19, 0, 1, AltW), ROW("^W Close W.", 38, 0, 2, CtrlW),
	ROW("Alt-X Quit", 66, 0, 5, AltX) };
static const OldEntry old_wblst_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Write", 19, 0, 1, AltW), ROW("^W Close W.", 38, 0, 2, CtrlW),
	ROW("^Q Quit", 63, 0, 2, CtrlQ) };
static const OldEntry old_rblst_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Read", 19, 0, 1, AltR), ROW("^W Close W.", 38, 0, 2, CtrlW),
	ROW("Alt-X Quit", 66, 0, 5, AltX) };
static const OldEntry old_rblst_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Read", 19, 0, 1, AltR), ROW("^W Close W.", 38, 0, 2, CtrlW),
	ROW("^Q Quit", 63, 0, 2, CtrlQ) };
static const OldEntry old_ablst_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Add", 19, 0, 1, AltA), ROW("^W Close W.", 38, 0, 2, CtrlW),
	ROW("Alt-X Quit", 66, 0, 5, AltX) };
static const OldEntry old_ablst_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Add", 19, 0, 1, AltA), ROW("^W Close W.", 38, 0, 2, CtrlW),
	ROW("^Q Quit", 63, 0, 2, CtrlQ) };
static const OldEntry old_sblst_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Save", 14, 0, 1, AltS), ROW("Change Dir", 25, 0, 1, AltC),
	ROW("^W Close W.", 42, 0, 2, CtrlW), ROW("Alt-X Quit", 66, 0, 5, AltX) };
static const OldEntry old_sblst_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Save", 14, 0, 1, AltS), ROW("Change Dir", 25, 0, 1, AltC),
	ROW("^W Close W.", 42, 0, 2, CtrlW), ROW("^Q Quit", 66, 0, 2, CtrlQ) };
static const OldEntry old_hblst_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("<BSP> Back", 9, 0, 5, WPE_DC), ROW("<CR> Goto", 21, 0, 4, WPE_CR),
	ROW(" PreVious", 32, 4, 1, AltV), ROW(" NexT", 45, 4, 1, AltT), ROW("^W Close", 54, 0, 2, CtrlW),
	ROW("Alt-X Quit", 68, 0, 5, AltX) };
static const OldEntry old_hblst_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("<BSP> Back", 9, 0, 5, WPE_DC), ROW("<CR> Goto", 21, 0, 4, WPE_CR),
	ROW(" PreVious", 32, 4, 1, AltV), ROW(" NexT", 45, 4, 1, AltT), ROW("^W Close", 54, 0, 2, CtrlW),
	ROW("^Q Quit", 65, 0, 2, CtrlQ) };
static const OldEntry old_gblst_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Show", 19, 0, 1, AltS), ROW("^W Close W.", 38, 0, 2, CtrlW),
	ROW("Alt-X Quit", 66, 0, 5, AltX) };
static const OldEntry old_gblst_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Show", 19, 0, 1, AltS), ROW("^W Close W.", 38, 0, 2, CtrlW),
	ROW("^Q Quit", 63, 0, 2, CtrlQ) };
static const OldEntry old_oblst_o[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Add", 14, 0, 1, AltA), ROW("Delete", 25, 0, 1, AltD),
	ROW("^W Close W.", 42, 0, 2, CtrlW), ROW("Alt-X Quit", 66, 0, 5, AltX) };
static const OldEntry old_oblst_u[] = {
	ROW("F1 Help", 0, 0, 2, F1), ROW("Add", 14, 0, 1, AltA), ROW("Delete", 25, 0, 1, AltD),
	ROW("^W Close W.", 42, 0, 2, CtrlW), ROW("^Q Quit", 63, 0, 2, CtrlQ) };

#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
/* A bar in both styles: the old array of each. */
#define BARS(name, bar, ob, oc) { name, bar, { ob, oc }, { LEN(ob), LEN(oc) } }
/* A bar of the Borland style only: the CUA style gives it no entry. */
#define BARB(name, bar, ob) { name, bar, { ob, NULL }, { LEN(ob), 0 } }

static const struct {
	const char *name;
	WOPT *bar;
	const OldEntry *old[WPE_STYLE_COUNT];
	int n[WPE_STYLE_COUNT];
} bars[] = {
	BARS("eblst", eblst_bar, old_eblst_o, old_eblst_u),
	BARS("eblst_prog", eblst_prog_bar, old_eblst_prog_o, modern_eblst_prog),
	BARS("eblst_lsp", eblst_lsp_bar, old_eblst_lsp_o, old_eblst_lsp_u),
#ifdef WPE_AI
	BARS("eblst_ai", eblst_ai_bar, old_eblst_ai_o, old_eblst_ai_u),
	BARS("eblst_lspai", eblst_lspai_bar, old_eblst_lspai_o, old_eblst_lspai_u),
#endif
	BARS("fblst", fblst_bar, old_fblst_o, old_fblst_u),
	BARS("mblst", mblst_bar, old_mblst_o, old_mblst_u),
	BARS("dblst", dblst_bar, old_dblst_o, old_dblst_u),
	BARS("dblst_src", dblst_src_bar, old_dblst_src_o, modern_dblst_src),
	BARS("dblst_run", dblst_run_bar, old_dblst_run_o, modern_dblst_run),
	BARS("xblst", xblst_bar, old_xblst_o, old_xblst_u),
	BARS("wblst", wblst_bar, old_wblst_o, old_wblst_u),
	BARS("rblst", rblst_bar, old_rblst_o, old_rblst_u),
	BARS("ablst", ablst_bar, old_ablst_o, old_ablst_u),
	BARS("sblst", sblst_bar, old_sblst_o, old_sblst_u),
	BARS("hblst", hblst_bar, old_hblst_o, old_hblst_u),
	BARS("gblst", gblst_bar, old_gblst_o, old_gblst_u),
	BARS("oblst", oblst_bar, old_oblst_o, old_oblst_u)
};

/** Every bar, filled for each style, against the arrays it had in that style;
    and the switch back and forth gives the same bars again. */
static void test_bars(void)
{
	size_t b;
	int i, style, pass;

	for (pass = 0; pass < 2; pass++)
	for (style = 0; style < WPE_STYLE_COUNT; style++) {
		e_bars_use_style(style);
		for (b = 0; b < sizeof(bars) / sizeof(bars[0]); b++) {
			int n = bars[b].n[style];

			if (e_bar_length(bars[b].bar) != n) {
				printf("FAIL: bar %s has %d entries in style %d, was %d\n", bars[b].name,
				       e_bar_length(bars[b].bar), style, n);
				fails++;
				continue;
			}
			for (i = 0; i < n; i++) {
				const WOPT *w = &bars[b].bar[i];
				const OldEntry *o = &bars[b].old[style][i];

				if (strcmp(w->t, o->t) != 0 || w->x != o->x || w->s != o->s ||
				    w->n != o->n || w->as != o->as) {
					printf("FAIL: bar %s style %d entry %d: got {\"%s\", %d, %d, %d, %d} want {\"%s\", %d, %d, %d, %d}\n",
					       bars[b].name, style, i, w->t, w->x, w->s, w->n, w->as,
					       o->t, o->x, o->s, o->n, o->as);
					fails++;
				}
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* the rules of the table                                              */

static void test_table_rules(void)
{
	size_t n, i, j;
	const WpeBinding *t = wpe_binding_table(&n);
	int style;
	int a;

	for (i = 0; i < n; i++) {
		if (t[i].code > t[i].code_last) {
			printf("FAIL: row %zu has an empty range\n", i);
			fails++;
		}
		if (t[i].action <= WPE_ACT_NONE || t[i].action >= WPE_ACT_COUNT) {
			printf("FAIL: row %zu has no action\n", i);
			fails++;
		}
		if (t[i].context >= WPE_CTX_COUNT || !(t[i].styles & WPE_STYLES_BOTH)) {
			printf("FAIL: row %zu has no context or no style\n", i);
			fails++;
		}
	}
	/* no key is bound twice in one style and context */
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++) {
			if (t[i].context != t[j].context || !(t[i].styles & t[j].styles))
				continue;
			if (t[i].code <= t[j].code_last && t[j].code <= t[i].code_last) {
				printf("FAIL: rows %zu and %zu bind the same key twice\n", i, j);
				fails++;
			}
		}
	/* at most one bar row per action and style, at most one in each context */
	for (a = 1; a < WPE_ACT_COUNT; a++)
		for (style = 0; style < WPE_STYLE_COUNT; style++) {
			int count = 0;

			for (i = 0; i < n; i++)
				if (t[i].action == a && (t[i].styles & WPE_STYLE_BIT(style)) &&
				    (t[i].flags & WPE_ROW_BAR))
					count++;
			if (count > 1) {
				printf("FAIL: %s has %d bar keys in style %d\n", wpe_action_name(a), count, style);
				fails++;
			}
		}
	/* every action has a name and a description */
	for (a = 0; a < WPE_ACT_COUNT; a++)
		if (!wpe_action_name(a)[0] || !wpe_action_text(a)[0]) {
			printf("FAIL: action %d has no name or description\n", a);
			fails++;
		}
	/* the spellings */
	{
		char b[64];
		const WpeBinding *r = NULL;

		for (i = 0; i < n; i++)
			if (t[i].code == AF2 && t[i].context == WPE_CTX_WINDOW)
				r = &t[i];
		if (!r || strcmp(wpe_binding_row_name(r, WPE_NAME_DOC, b, sizeof b), "Alt-F2"))
			fail("doc spelling", 0, "?", "Alt-F2");
		if (r && strcmp(wpe_binding_row_name(r, WPE_NAME_MENU, b, sizeof b), "Alt F2"))
			fail("menu spelling", 0, b, "Alt F2");
		if (r && strcmp(wpe_binding_row_name(r, WPE_NAME_BAR, b, sizeof b), "Alt-F2"))
			fail("bar spelling", 0, b, "Alt-F2");
	}
	{
		char b[96];

		wpe_menu_item_text(b, sizeof b, "Save", WPE_ACT_SAVE, WPE_STYLE_MODERN, WPE_CTXM_MENU, 19, NULL);
		same("menu item", 0, b, "Save             ^S");
		wpe_menu_item_text(b, sizeof b, "Save", WPE_ACT_SAVE, WPE_STYLE_BORLAND, WPE_CTXM_MENU, 19, NULL);
		same("Borland menu item", 0, b, "Save             F2");
		wpe_menu_item_text(b, sizeof b, "Tile", WPE_ACT_TILE, WPE_STYLE_BORLAND, WPE_CTXM_MENU, 23, NULL);
		same("menu item without a key", 0, b, "Tile");
		wpe_menu_item_text(b, sizeof b, "Replace", WPE_ACT_REPLACE, WPE_STYLE_BORLAND, WPE_CTXM_MENU, 23, NULL);
		same("menu item with a prefix key", 0, b, "Replace   Alt F4 / ^O A");
	}
	/* an action with no key in a style names none */
	if (wpe_binding_key_name(WPE_ACT_TILE, WPE_STYLE_BORLAND) != NULL)
		fail("Tile has a Borland key", 0, "a key", "none");
	/* the Modern style leaves Tile to the Window menu, as the Borland style does */
	if (wpe_binding_key_name(WPE_ACT_TILE, WPE_STYLE_MODERN) != NULL)
		fail("Tile has a Modern key", 0, "a key", "none");
}

int main(void)
{
	test_dispatch();
	test_modern_table();
	test_delivery();
	test_bars();
	test_table_rules();
	if (fails) {
		printf("test_bindings: %d failures\n", fails);
		return 1;
	}
	printf("test_bindings: ok\n");
	return 0;
}
