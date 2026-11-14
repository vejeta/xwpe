#ifndef WE_BINDINGS_H
#define WE_BINDINGS_H

/* we_bindings.h -- which key does what, once.

   we_keymap.c turns what a display reports into one key code.  This module
   turns a key code into an action, from one table, for both key styles.  The
   dispatchers switch over actions; the bottom bars, the menus, the generated
   key reference and the keymap check read the same rows, so a binding is one
   line and cannot disagree with the text that names it.

   No editor header and no display library is needed to use it: the unit test
   and tools/keymap-texi link we_bindings.c alone. */

#include <stddef.h>

/** The key styles.  Options > Editor > Keys picks one; it is the bit
    ED_CUA_STYLE of the editor options. */
typedef enum {
 WPE_STYLE_BORLAND = 0,
 WPE_STYLE_MODERN,
 WPE_STYLE_COUNT
} WpeStyle;

#define WPE_STYLE_BIT(s) (1u << (s))
#define WPE_STYLES_BOTH (WPE_STYLE_BIT(WPE_STYLE_BORLAND) | WPE_STYLE_BIT(WPE_STYLE_MODERN))

/** The places that read a key and choose what it does.  The four editor
    contexts are consulted one after the other, in this order, for a key in an
    editor window: a key that one of them binds never reaches the next. */
typedef enum {
 WPE_CTX_WINDOW = 0,    /* windows, menus, help, quit: any window */
 WPE_CTX_PROGRAM,       /* build, run, errors, the language server and assistant doors */
 WPE_CTX_DEBUG,         /* the debugger's own keys */
 WPE_CTX_EDIT,          /* editing: undo, clipboard, search, the two prefixes */
 WPE_CTX_PREFIX_K,      /* the letter after Ctrl-K: blocks and markers */
 WPE_CTX_PREFIX_O,      /* the letter after Ctrl-O: editing commands */
 WPE_CTX_PREFIX_DEBUG,  /* the letter after Ctrl-G: the debugger */
 WPE_CTX_PREFIX_LSP,    /* the letter after Alt-Q: the language server */
 WPE_CTX_PREFIX_AI,     /* the letter after Alt-G: the assistant */
 WPE_CTX_WATCHES,       /* the Watches window's own keys */
 WPE_CTX_HELP,          /* the Help viewer's page keys */
 WPE_CTX_LIST,          /* the file manager's list windows: the key that closes one */
 WPE_CTX_DIALOG_MOVE,   /* a dialog or drop-down: the key that starts the move mode */
 WPE_CTX_CURSOR,        /* the control letters that move the cursor or delete a word, a line */
 WPE_CTX_BOX,           /* a dialog, menu or message box: binds no key, so every distinct key folds */
 WPE_CTX_COUNT
} WpeContext;

#define WPE_CTX_BIT(c) (1u << (c))
/** The keys that act on their own, and the letters that follow a prefix. */
#define WPE_CTXM_DIRECT (WPE_CTX_BIT(WPE_CTX_WINDOW) | WPE_CTX_BIT(WPE_CTX_PROGRAM) | \
                         WPE_CTX_BIT(WPE_CTX_DEBUG) | WPE_CTX_BIT(WPE_CTX_EDIT))
#define WPE_CTXM_PREFIX (WPE_CTX_BIT(WPE_CTX_PREFIX_K) | WPE_CTX_BIT(WPE_CTX_PREFIX_O) | \
                         WPE_CTX_BIT(WPE_CTX_PREFIX_DEBUG) | WPE_CTX_BIT(WPE_CTX_PREFIX_LSP) | \
                         WPE_CTX_BIT(WPE_CTX_PREFIX_AI))
#define WPE_CTXM_MENU   (WPE_CTXM_DIRECT | WPE_CTXM_PREFIX)
#define WPE_CTXM_HELP   WPE_CTX_BIT(WPE_CTX_HELP)

/* X(id, "name", "what the user gets").  The name and the description are what
   the generated reference prints. */
#define WPE_ACTIONS(X) \
 X(NONE, "None", "No action") \
 X(SWITCH_WINDOW, "Switch window", "Bring window 1 to 35 to the front") \
 X(HELP_EDITOR, "Help", "Open the help for the editor") \
 X(TOPIC_SEARCH, "Topic search", "Search the help for the word at the cursor") \
 X(FUNCTION_INDEX, "Function index", "Open the manual page index") \
 X(ESCAPE, "Escape", "Leave the Messages list; nothing in a file window") \
 X(MENU_BAR, "Menu bar", "Open the menu bar") \
 X(LIST_WINDOWS, "List all windows", "Open the window list") \
 X(USER_SCREEN, "User screen", "Show the screen of the program that was run") \
 X(SIZE_MOVE, "Size/Move", "Move or resize the window with the cursor keys") \
 X(FILE_MANAGER, "File manager", "Open the file manager") \
 X(FILE_MANAGER_FIRST, "File manager (home)", "Open the file manager at the first directory") \
 X(CLOSE_WINDOW, "Close window", "Close the window") \
 X(CASCADE, "Cascade", "Stack the windows") \
 X(TILE, "Tile", "Tile the windows") \
 X(ZOOM, "Zoom", "Enlarge the window or restore it") \
 X(NEXT_WINDOW, "Next window", "Bring the next window to the front") \
 X(QUIT, "Quit", "Leave the editor") \
 X(RUN, "Run", "Run the program") \
 X(LSP_PREFIX, "Language server", "Alt-Q starts a language-server command") \
 X(LSP_MENU, "Language server menu", "Open the language-server menu (bottom bar)") \
 X(MAKE, "Make", "Run make") \
 X(COMPILE, "Compile", "Compile the file") \
 X(INSTALL, "Install", "Run make install") \
 X(EXEC_MAKE, "Execute make", "Run a make target") \
 X(AI_PREFIX, "Assistant", "Alt-G opens the assistant menu") \
 X(AI_MENU, "Assistant menu", "Open the assistant menu (bottom bar)") \
 X(NEXT_ERROR, "Next error", "Go to the next error") \
 X(PREV_ERROR, "Previous error", "Go to the previous error") \
 X(DEBUG_PREFIX, "Debugger", "Ctrl-G starts a debugger command") \
 X(DEBUG_TRACE, "Trace", "Step into the next line") \
 X(DEBUG_STEP, "Step", "Step over the next line") \
 X(DEBUG_RUN, "Run/Continue", "Run the program or continue it") \
 X(DEBUG_QUIT, "Quit debugger", "End the debug session") \
 X(DEBUG_PAUSE, "Interrupt", "Pause the running program") \
 X(DEBUG_BREAKPOINT, "Toggle breakpoint", "Set or clear a breakpoint on the line") \
 X(DEBUG_WATCH, "Make watch", "Watch the expression under the cursor") \
 X(DEBUG_STACK, "Show stack", "Show the call stack") \
 X(DEBUG_GOTO_CURSOR, "Go to cursor", "Run to the cursor") \
 X(DEBUG_OUTPUT, "Program output", "Show the program's output") \
 X(DEBUG_OPTIONS, "Debugger options", "Open the debugger options") \
 X(DEBUG_REMOVE_BREAKPOINTS, "Remove all breakpoints", "Clear every breakpoint") \
 X(DEBUG_REMOVE_ALL_WATCHES, "Remove all watches", "Delete every watch") \
 X(DEBUG_DELETE_WATCH, "Delete watch", "Delete one watch") \
 X(DEBUG_EDIT_WATCH, "Edit watch", "Change a watch") \
 X(DEBUG_FINISH, "Finish function", "Run until the function returns") \
 X(K_PREFIX, "Block commands", "Ctrl-K starts a block command") \
 X(O_PREFIX, "Editing commands", "Ctrl-O starts an editing command") \
 X(GOTO_LINE, "Go to line", "Jump to a line number") \
 X(SHOW_CLIPBOARD, "Show buffer", "Show the paste buffer") \
 X(BLOCK_DELETE, "Delete block", "Delete the marked block") \
 X(EDIT_UNDO, "Undo", "Undo the last change") \
 X(EDIT_REDO, "Redo", "Redo the change that was undone") \
 X(EDIT_CUT, "Cut", "Cut the block to the clipboard") \
 X(EDIT_PASTE, "Paste", "Paste the clipboard") \
 X(EDIT_COPY, "Copy", "Copy the block to the clipboard") \
 X(SAVE, "Save", "Write the file") \
 X(FIND, "Find", "Search for text") \
 X(REPLACE, "Replace", "Search and replace text") \
 X(SEARCH_AGAIN, "Search again", "Repeat the last search") \
 X(BLOCK_BEGIN, "Begin mark", "Mark the start of a block") \
 X(BLOCK_END, "End mark", "Mark the end of a block") \
 X(BLOCK_ALL, "Mark whole text", "Mark the whole text as a block") \
 X(BLOCK_LINE, "Mark line", "Mark the cursor's line as a block") \
 X(BLOCK_GOTO_BEGIN, "Go to block start", "Move the cursor to the start of the block") \
 X(BLOCK_GOTO_END, "Go to block end", "Move the cursor to the end of the block") \
 X(BLOCK_COPY, "Copy block", "Copy the block to the cursor") \
 X(BLOCK_MOVE, "Move block", "Move the block to the cursor") \
 X(BLOCK_HIDE, "Hide block", "Hide the block marks") \
 X(BLOCK_READ, "Read block", "Read a file into the text") \
 X(BLOCK_WRITE, "Write block", "Write the block to a file") \
 X(BLOCK_INDENT, "Move block right", "Indent the block") \
 X(BLOCK_UNINDENT, "Move block left", "Unindent the block") \
 X(BLOCK_CASE, "Change case", "Change the case of the block") \
 X(BLOCK_BEAUTIFY, "Beautify block", "Format the block") \
 X(MARKER_SET, "Set marker", "Set marker 0 to 9 at the cursor") \
 X(MARKER_GOTO, "Go to marker", "Jump to marker 0 to 9") \
 X(DELETE_TO_EOL, "Delete to end of line", "Delete from the cursor to the end of the line") \
 X(DELETE_WORD_LEFT, "Delete word left", "Delete from the cursor back to the start of the word") \
 X(SHOW_DEFINITION, "Show definition", "Go to the definition of the word") \
 X(NEXT_DEFINITION, "Show next definition", "Go to the next definition of the word") \
 X(MATCH_BRACKET, "Matching bracket", "Go to the matching bracket") \
 X(BEAUTIFY, "Beautify", "Format the text") \
 X(HELP_MARK_BUTTON, "Help button", "Make the block a help button") \
 X(HELP_MARK_LINE, "Help highlight", "Make the block a help highlight") \
 X(HELP_MARK_HEADER, "Help header", "Make the block a help header") \
 X(HELP_MARK_END, "Help page end", "Put a help page end above the line") \
 X(HELP_MARKS_DELETE, "Delete help marks", "Delete the help marks from the line") \
 X(HELP_CHECK, "Check help file", "Check the help file") \
 X(HELP_NEXT, "Help: next link", "Go to the next link in the help page") \
 X(HELP_PREV, "Help: previous link", "Go to the previous link in the help page") \
 X(LSP_DEFINITION, "Go to definition", "Jump to the definition of the symbol") \
 X(LSP_IMPLEMENTATION, "Go to implementation", "Jump to the implementation of the symbol") \
 X(LSP_TYPE_DEFINITION, "Go to type", "Jump to the type of the symbol") \
 X(LSP_HOVER, "Hover", "Show the type and documentation of the symbol") \
 X(LSP_COMPLETE, "Complete", "Complete the word at the cursor") \
 X(LSP_REFERENCES, "References", "List the places that use the symbol") \
 X(LSP_CALLERS, "Callers", "List the functions that call this one") \
 X(LSP_CALLEES, "Callees", "List the functions this one calls") \
 X(LSP_SUPERTYPES, "Supertypes", "List the supertypes of the symbol") \
 X(LSP_SUBTYPES, "Subtypes", "List the subtypes of the symbol") \
 X(LSP_EXPAND_SELECTION, "Expand selection", "Grow the selection to the enclosing construct") \
 X(LSP_HIGHLIGHT, "Highlight uses", "Mark the uses of the symbol in the file") \
 X(LSP_INLAY, "Inlay hints", "Show or hide the inlay hints") \
 X(LSP_SEMANTIC, "Semantic colours", "Show or hide the semantic colours") \
 X(LSP_CODELENS, "Code lens", "Show the code lenses") \
 X(LSP_SIGNATURE, "Signature help", "Show the signature of the call") \
 X(LSP_WORKSPACE_SYMBOLS, "Workspace symbols", "Search the symbols of the project") \
 X(LSP_CODE_ACTIONS, "Code actions", "Offer the code actions at the cursor") \
 X(LSP_FORMAT, "Format", "Format the file") \
 X(LSP_RENAME, "Rename", "Rename the symbol") \
 X(LSP_OUTLINE, "Outline", "List the symbols of the file") \
 X(LSP_DIAGNOSTICS, "Diagnostics", "Restart the server and show the diagnostics") \
 X(LSP_NEXT_PROBLEM, "Next problem", "Go to the next problem") \
 X(LSP_PREV_PROBLEM, "Previous problem", "Go to the previous problem") \
 X(LSP_CANCEL, "Cancel", "Leave the language-server prefix") \
 X(AI_ASK, "Ask (chat)", "Ask the assistant") \
 X(AI_EDIT, "Edit current file", "Have the assistant edit the file") \
 X(AI_COMPLETE, "Complete at cursor", "Have the assistant complete at the cursor") \
 X(AI_PLAN, "Multi-file edit", "Plan an edit over several files") \
 X(AI_AGENT, "Agent (tools)", "Let the assistant use its tools") \
 X(AI_FIX_BUILD, "Build and fix (agent)", "Let the assistant build and fix the errors") \
 X(AI_POLICY, "Permissions", "Change what the assistant may do") \
 X(AI_READ_FOLDERS, "Read folders", "Withdraw a folder the assistant may read") \
 X(AI_NEW_SESSION, "Clear conversation", "Start a new conversation") \
 X(AI_SETTINGS, "AI settings", "Open the assistant's settings") \
 X(CLOSE_LIST, "Close list", "Close the file manager's list window") \
 X(CURSOR_UP, "Cursor up", "Move the cursor up one line") \
 X(CURSOR_DOWN, "Cursor down", "Move the cursor down one line") \
 X(CURSOR_LEFT, "Cursor left", "Move the cursor left one character") \
 X(CURSOR_RIGHT, "Cursor right", "Move the cursor right one character") \
 X(LINE_START, "Start of line", "Move the cursor to the start of the line") \
 X(LINE_END, "End of line", "Move the cursor to the end of the line") \
 X(DELETE_WORD_RIGHT, "Delete word right", "Delete from the cursor to the start of the next word") \
 X(DELETE_LINE, "Delete line", "Delete the cursor's line") \
 X(SELECT_WORD, "Select word", "Select the word at the cursor") \
 X(DELETE_CHAR, "Delete character", "Delete the character at the cursor")

typedef enum {
#define WPE_ACT_ENUM(id, name, text) WPE_ACT_##id,
 WPE_ACTIONS(WPE_ACT_ENUM)
#undef WPE_ACT_ENUM
 WPE_ACT_COUNT
} WpeAction;

/** What a row's key text is for.  The bottom bar and the menus name the keys
    in different spellings (Alt-F2 and Alt F2); the reference spells them out. */
typedef enum {
 WPE_NAME_DOC,   /* Ctrl-F4, Alt-F2, Ctrl-K B */
 WPE_NAME_BAR,   /* ^F4, Alt-F2, ^K B */
 WPE_NAME_MENU   /* ^F4, Alt F2, ^K B */
} WpeNameForm;

/** Row flags. */
#define WPE_ROW_BAR    0x01u  /* the key a bottom bar names, and sends when clicked */
#define WPE_ROW_MENU   0x02u  /* a key the menus show beside the item */
#define WPE_ROW_HIDDEN 0x04u  /* a code no key sends (a click on a bottom-bar entry) */
#define WPE_ROW_CTRL   0x08u  /* a letter row: the letter's control code also matches */
#define WPE_ROW_NO_SESSION 0x10u  /* only while no debugger session is open (F10 opens the menu) */
#define WPE_ROW_IN_SESSION 0x20u  /* only while a debugger session is open (F10 steps over) */

/** One binding.  A key code, or the range @code..@code_last, in the styles of
    @styles, in @context, does @action. */
typedef struct {
 unsigned short action;
 int code;
 int code_last;
 unsigned char styles;
 unsigned char context;
 unsigned char flags;
 const char *name;   /* the key, Ctrl-F4 style; NULL for a letter after a prefix */
 const char *fallback; /* what to press where a terminal keeps this key; NULL when it always arrives */
} WpeBinding;

/**
 * wpe_binding_set_session_probe - Tell the table how to ask whether a debugger
 * session is open.
 * @probe: returns non-zero while a session is open; NULL (the default) means
 *   none is ever open, as in the unit test and the reference generator.
 *
 * The rows flagged WPE_ROW_NO_SESSION and WPE_ROW_IN_SESSION apply only in
 * the state they name, so one key (F10, F8) can open the menu or step over
 * according to what the user is doing, with no state test in a dispatcher.
 */
void wpe_binding_set_session_probe(int (*probe)(void));

/**
 * wpe_binding_set_trace - Have every dispatch of a key to an action reported.
 * @hook: called with the key, the style, the context and the action each time
 *   a dispatcher gets an action from the table; NULL (the default) reports
 *   nothing.
 *
 * For tests: a row can send a key and read which action the table gave, so
 * "every key of the Modern table drives its action" is checked on the running
 * editor, one row per table line.  The queries the fold makes to ask whether
 * a key is bound are not dispatches and are not reported.
 */
void wpe_binding_set_trace(void (*hook)(int code, int style, int context, int action));

/**
 * wpe_binding_style - The key style an editor's options select.
 * @edopt: the editor's option word.
 * Return: a WpeStyle.  The only reader of the style bit outside the dialog.
 */
int wpe_binding_style(int edopt);

/**
 * wpe_binding_action - What a key does in a context.
 * @code: a keys.h code, or the uppercase letter after Ctrl-K or Ctrl-O, or the
 *   letter or control code after Ctrl-G.
 * @style: WPE_STYLE_*; @context: WPE_CTX_*.
 * Return: the action, WPE_ACT_NONE when the key is not bound there.
 */
WpeAction wpe_binding_action(int code, int style, int context);

/**
 * wpe_binding_prefix_folds_control - Does Ctrl-K B read as Ctrl-K Ctrl-B?
 * @style: WPE_STYLE_*.
 * Return: 1 in the Borland style, where a control letter after Ctrl-K is the
 * letter (WordStar's Ctrl-K Ctrl-B); 0 in the Modern style, where it is a
 * chord of its own (Ctrl-K Ctrl-C), so the letter commands and the chords
 * cannot take each other's keys.
 */
int wpe_binding_prefix_folds_control(int style);

/**
 * wpe_binding_first_code - The first key code that does an action in a context.
 * @action, @style, @context: what, in which style, read where.
 * Return: the code, 0 when the style binds none there.  For a caller that
 * must act as if the key had been pressed (the Esc that closes a list).
 */
int wpe_binding_first_code(WpeAction action, int style, int context);

/**
 * wpe_binding_window_number - The window a window-switch key brings up.
 * Return: 1 to 35, 0 when @code is not a window-switch key.
 */
int wpe_binding_window_number(int code);

/**
 * wpe_binding_fold - The code every reader of keys should see.
 * @code: what the display gave; @style, @context: where it is read.
 *
 * The one place that turns a distinct key into the key it stands for when
 * the style binds nothing to the distinct one: Ctrl-H, Ctrl-I, Ctrl-M and
 * Ctrl-[ (a terminal with a keyboard protocol reports them apart from
 * Backspace, Tab, Enter and Esc), Ctrl-Tab (Tab) and Ctrl-Shift-letter
 * (Ctrl-letter).  The display decoders return the distinct code; the editor
 * folds it once, where it takes a key, so a dialog, a menu and the file
 * manager read the same code as the editor.  Ctrl-Alt-letter and the Ctrl
 * punctuation codes have no legacy form and pass through.
 * Return: the code the readers see.
 */
int wpe_binding_fold(int code, int style, int context);

/**
 * wpe_binding_table - Every row, in the order the reference prints them.
 * @n: out, the number of rows.
 */
const WpeBinding *wpe_binding_table(size_t *n);

/** The name and the description of an action. */
const char *wpe_action_name(WpeAction a);
const char *wpe_action_text(WpeAction a);
/** The name of a context, as the reference titles it. */
const char *wpe_context_name(int context);
/** The prefix a context's letters follow ("Ctrl-K"), NULL for a direct one. */
const char *wpe_context_prefix(int context);

/**
 * wpe_binding_row_name - The key of a row, written for @form.
 * @row: a row; @buf, @n: the destination.
 * Return: @buf.
 */
const char *wpe_binding_row_name(const WpeBinding *row, int form, char *buf, size_t n);

/**
 * wpe_binding_key_text - The keys that name an action, joined.
 * @buf, @n: the destination.
 * @action, @style: what and in which style.
 * @contexts: WPE_CTX_BIT() of the contexts to read.
 * @flag: only the rows with this flag (WPE_ROW_BAR or WPE_ROW_MENU).
 * @form: the spelling.  @sep: between keys, " / " when NULL.
 * @prog: 1 in the programming editor, 0 in the plain one.
 * Return: the length written, 0 when no row matches.
 *
 * What a bar or a menu prints beside an item, so the text cannot name a key
 * the table does not bind, nor one that an earlier context binds to another
 * action in this mode: Go to Line's Alt-G is the assistant's key in the
 * programming editor, so its item shows no key there.
 */
int wpe_binding_key_text(char *buf, size_t n, WpeAction action, int style,
                         unsigned contexts, unsigned flag, int form,
                         const char *sep, int prog);

/**
 * wpe_binding_key_name - The key that names an action in a style.
 * @action, @style: what and in which style.
 * Return: the name of the bar row, else the first menu row, else the first row
 *   (Ctrl-F4 style), NULL when the action has no key.
 */
const char *wpe_binding_key_name(WpeAction action, int style);

/**
 * wpe_binding_bar_code - The code a bottom bar sends when its entry is clicked.
 * Return: the bar row's code, 0 when the action has none in the style.
 */
int wpe_binding_bar_code(WpeAction action, int style);

/**
 * wpe_binding_run_key - The key that runs the program, for a message.
 * @edopt: the editor's option word.
 * Return: the key's name in the style @edopt selects, "Run" when the style
 *   binds none.
 */
const char *wpe_binding_run_key(int edopt);

/**
 * wpe_bar_entry_text - The label of a bottom-bar entry bound to an action.
 * @buf, @n: the destination.
 * @action, @style, @stem: the action, the style and what follows the key.
 * @keylen: out, the length of the key at the start of the label (the part a
 *   bar draws in the hot colour).
 * Return: the label, "Alt-F2 Save" for (SAVE, CUA, "Save"); the stem alone
 *   when the action has no bar key in the style.
 */
const char *wpe_bar_entry_text(char *buf, size_t n, WpeAction action, int style,
                               const char *stem, int *keylen);

/**
 * wpe_menu_item_text - The label of a menu item: its words, spaces, then its keys.
 * @buf, @n: the destination.
 * @stem: the item's words.
 * @action, @style: what it runs and the style.
 * @contexts: which contexts' keys are shown.
 * @width: the label's width; the keys end at its right edge.  0 for no padding.
 * @sep: between keys, " / " when NULL.
 * @prog: 1 in the programming editor (see wpe_binding_key_text).
 * Return: @buf.  With no key to show the label is @stem alone, without
 *   padding.  At least one space separates the words from the keys, so a
 *   label whose keys do not fit in @width is longer than @width.
 */
const char *wpe_menu_item_text(char *buf, size_t n, const char *stem,
                               WpeAction action, int style, unsigned contexts,
                               int width, const char *sep, int prog);

/* ---- Which terminals deliver which key ---------------------------------
   The fourth column of the generated reference.  Everything here is filled
   from docs/chapters/keys-delivery.txt, which records what each terminal was
   measured to send (or says plainly that a terminal could not be driven). */

/** The terminals the delivery table names. */
typedef enum {
 WPE_EMU_XTERM = 0,
 WPE_EMU_VTE,       /* GNOME Terminal, xfce4-terminal, Tilix, Terminator */
 WPE_EMU_KONSOLE,
 WPE_EMU_KITTY,
 WPE_EMU_TMUX,
 WPE_EMU_SCREEN,
 WPE_EMU_CONSOLE,   /* the Linux console */
 WPE_EMU_COUNT
} WpeEmulator;

/** The two keyboard protocols a terminal may implement. */
#define WPE_PROTO_MODIFY_OTHER_KEYS 0x01u   /* xterm: CSI 27 ; m ; c ~ */
#define WPE_PROTO_KITTY             0x02u   /* kitty: CSI c ; m u */

/** How a key reaches the editor. */
typedef enum {
 WPE_DELIVER_ALWAYS = 0,   /* a legacy byte or plain CSI: every terminal sends it */
 WPE_DELIVER_PROTOCOL      /* a key of its own only where a protocol is implemented */
} WpeDeliveryClass;

/** The key codes @code..@code_last arrive as @klass, and the terminals of
    the mask @taken keep them for themselves.  @why is "measured" or
    "documented", then the reason. */
typedef struct {
 int code;
 int code_last;
 unsigned char klass;
 unsigned char taken;     /* bits: 1 << WpeEmulator */
 const char *why;
} WpeDelivery;

/** The delivery rows, in the order the reference prints them. */
const WpeDelivery *wpe_key_delivery(size_t *n);

/** The name of a terminal, as the reference titles it. */
const char *wpe_emulator_name(int emu);

/** The protocols a terminal implements (WPE_PROTO_* bits), as measured. */
unsigned wpe_emulator_protocols(int emu);

/**
 * wpe_key_reaches - Does the editor receive the key @code from @emu?
 * Return: 1 when it arrives; 0 when the terminal keeps it, or when the key
 * needs a protocol the terminal does not implement.  tmux counts as
 * implementing modifyOtherKeys, which it passes with extended-keys on.
 */
int wpe_key_reaches(int code, int emu);

/**
 * wpe_binding_key_class - How the key of a binding reaches the editor.
 * Return: a WpeDeliveryClass, WPE_DELIVER_PROTOCOL when any delivery row that
 * contains @code says so.
 */
int wpe_binding_key_class(int code);

#endif /* WE_BINDINGS_H */
