/* we_bars.c                                              */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

/* The bottom bars, one list each, filled for the key style in use.  See
   we_bars.h. */

#include "edit.h"

#define STYLE_B WPE_STYLE_BIT(WPE_STYLE_BORLAND)
#define STYLE_C WPE_STYLE_BIT(WPE_STYLE_CUA)
#define STYLE_BOTH WPE_STYLES_BOTH

/** One entry of a bar, in the styles of @styles.  An entry bound to an action
    takes its key and the code a click sends from the key table; a fixed entry
    (a mnemonic such as "Edit", the Alt-Q and Alt-G doors, the Help viewer's
    Backspace and Enter) carries its own highlight and click code.  The words
    and the column may differ by style. */
typedef struct {
 int action;                       /* WpeAction, WPE_ACT_NONE for a fixed entry */
 unsigned styles;                  /* the styles that have this entry */
 const char *text[WPE_STYLE_COUNT];/* the words after the key, or a fixed entry's label */
 int x[WPE_STYLE_COUNT];           /* the column the entry starts at */
 int hi;                           /* fixed entry: index of the highlighted letter */
 int n;                            /* fixed entry: how many letters are highlighted */
 int code;                         /* fixed entry: the key a click sends */
} WpeBarSpec;

/* An action in both styles with the same words and column. */
#define BAR(action, stem, x) \
 { WPE_ACT_##action, STYLE_BOTH, { (stem), (stem) }, { (x), (x) }, 0, 0, 0 }
/* An action in both styles, with the words and column of each. */
#define BAR2(action, stem_b, x_b, stem_c, x_c) \
 { WPE_ACT_##action, STYLE_BOTH, { (stem_b), (stem_c) }, { (x_b), (x_c) }, 0, 0, 0 }
/* An action in the Borland style only. */
#define BAR_B(action, stem, x) \
 { WPE_ACT_##action, STYLE_B, { (stem), (stem) }, { (x), (x) }, 0, 0, 0 }
/* A fixed entry in both styles with the same label and column. */
#define FIX(text, x, hi, n, code) \
 { WPE_ACT_NONE, STYLE_BOTH, { (text), (text) }, { (x), (x) }, (hi), (n), (code) }
/* A fixed entry in both styles, the column of each. */
#define FIX2(text, x_b, x_c, hi, n, code) \
 { WPE_ACT_NONE, STYLE_BOTH, { (text), (text) }, { (x_b), (x_c) }, (hi), (n), (code) }
/* A fixed entry of one style. */
#define FIX_B(text, x, hi, n, code) \
 { WPE_ACT_NONE, STYLE_B, { (text), (text) }, { (x), (x) }, (hi), (n), (code) }
#define FIX_C(text, x, hi, n, code) \
 { WPE_ACT_NONE, STYLE_C, { (text), (text) }, { (x), (x) }, (hi), (n), (code) }

#define E_SPEC_LEN(spec) ((int)(sizeof(spec) / sizeof((spec)[0])))

/* The editor bar. */
static const WpeBarSpec eblst_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), BAR(SAVE, "Save", 9),
 BAR2(FILE_MANAGER, "Files", 18, "Files", 22),
 BAR2(CLOSE_WINDOW, "Close W.", 28, "Close ", 32),
 BAR2(FIND, "Search", 45, "Srch", 44),
 BAR2(SEARCH_AGAIN, "S.Again", 56, "S.Ag.", 57),
 BAR2(QUIT, "Quit", 68, "Quit", 67)
};

/* The editor bar of the programming modes (wpe, xwpe): F9 builds, as Turbo
   C's bar said, so the bar names it.  "Close" and "Again" are shortened to
   keep every key on an 80-column row; the plain editor (we) has no F9 and
   keeps the bar above.  The CUA bars have no room for one more entry, so
   the CUA style uses the bar above in the programming modes too.  The
   Messages bar is the one that offers Compile and Make by name. */
static const WpeBarSpec eblst_prog_spec[] = {
 BAR_B(HELP_EDITOR, "Help", 0), BAR_B(SAVE, "Save", 9), BAR_B(FILE_MANAGER, "Files", 18),
 BAR_B(CLOSE_WINDOW, "Close", 28), BAR_B(FIND, "Search", 37),
 BAR_B(SEARCH_AGAIN, "Again", 48), BAR_B(MAKE, "Build", 58), BAR_B(QUIT, "Quit", 68)
};

/* Editor bottom bar shown ONLY while an LSP-supported file is the active window
   (chosen per-window in e_edit) -- it swaps the "Search Again" hint for an
   "Alt-Q ? Metals" entry.  The highlighted "Alt-Q ?" (n=7) tells the user how to
   OPEN the action menu from the keyboard, the way every other entry on this bar
   shows its key in red (cf. "Alt-X Quit", "^W Close W.").  "Metals" names the
   server (when clangd/pyright is wired, give it its own variant).  The actions
   themselves run with Alt-Q+letter directly (no menu, no flicker -- the Borland
   Ctrl-K-for-blocks idea); Alt-Q ?, F1, an unknown key, or a click on this entry
   open the menu for discovery.  Alt-Q ("Query the language server") is the only
   collision-free mnemonic: M=Make, L=Install, A=exec-Make are taken and the sole
   free Alt-letter (K) has no tie to a language server.  Positions are packed a
   little tighter than the plain editor bar to fit the longer entry.  The entry
   at index 5 is the one e_lsp_bar_label renames.  The CUA style has no room
   for the Build entry. */
static const WpeBarSpec eblst_lsp_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), BAR(SAVE, "Save", 8),
 BAR2(FILE_MANAGER, "Files", 16, "Files", 20),
 BAR2(CLOSE_WINDOW, "Close", 25, "Close ", 29),
 BAR2(FIND, "Search", 34, "Srch", 40),
 FIX2("Alt-Q ? Metals", 44, 52, 0, 7, WPE_LSP_MENU),
 BAR_B(MAKE, "Build", 59),
 BAR2(QUIT, "Quit", 68, "Quit", 67)
};

#ifdef WPE_AI
/* Editor bottom bar shown while the AI assistant is enabled and the active
   window holds a plain file that did NOT already get the contextual LSP bar.
   It swaps the "Search Again" hint for an "Alt-G AI" entry: the highlighted
   "Alt-G" (n=5) shows the keyboard prefix, and a click on the entry opens the
   AI action menu (handled as WPE_AI_MENU in e_prog_switch).  "Alt-G AI" keeps
   the column of the "Search Again" slot it replaces in the plain editor bar;
   "F9 Build" sits right before it (the CUA style has no room for it). */
static const WpeBarSpec eblst_ai_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), BAR(SAVE, "Save", 9),
 BAR2(FILE_MANAGER, "Files", 18, "Files", 22),
 BAR2(CLOSE_WINDOW, "Close", 28, "Close ", 32),
 BAR2(FIND, "Search", 37, "Srch", 44),
 BAR_B(MAKE, "Build", 47),
 FIX2("Alt-G AI", 56, 57, 0, 5, WPE_AI_MENU),
 BAR2(QUIT, "Quit", 66, "Quit", 67)
};

/* Combined editor bottom bar for a file that has BOTH a language server AND the
   AI assistant enabled: it must show BOTH prefixes so neither feature loses its
   discoverability -- "Alt-Q ? LSP" opens the language-server action menu (the
   "Alt-Q ?" in red shows the keyboard prefix) and "Alt-G AI" opens the AI menu.
   To fit both in 80 columns the "Search Again" and "Search" hints are dropped
   (F4/^L still work); the server name is shown generically as "LSP" since the
   slot is shared.  Alt-Q and Alt-G both keep working regardless -- this only
   restores their on-screen hints.  The entry at index 4 is the one
   e_lsp_bar_label renames. */
static const WpeBarSpec eblst_lspai_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), BAR(SAVE, "Save", 8),
 BAR2(FILE_MANAGER, "Files", 16, "Files", 20),
 BAR2(CLOSE_WINDOW, "Close", 25, "Close", 30),
 FIX2("Alt-Q ? LSP", 34, 40, 0, 7, WPE_LSP_MENU),
 FIX2("Alt-G AI", 46, 54, 0, 5, WPE_AI_MENU),
 BAR_B(MAKE, "Build", 55),
 BAR2(QUIT, "Quit", 64, "Quit", 67)
};
#endif

/* The File-Manager bar. */
static const WpeBarSpec fblst_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), FIX2("Edit", 10, 9, 0, 1, AltE),
 FIX_B("Attributes", 17, 0, 1, AltA), FIX_C("RePlace", 15, 2, 1, AltP),
 FIX2("Move", 29, 24, 0, 1, AltM),
 FIX_B("COpy", 35, 1, 1, AltO), FIX_C("DUplicate", 30, 1, 1, AltU),
 FIX("Remove", 41, 0, 1, AltR),
 BAR(CLOSE_WINDOW, "Close W.", 49), BAR2(QUIT, "Quit", 66, "Quit", 63)
};

/* The Messages bar. */
static const WpeBarSpec mblst_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), FIX("PreV. Err.", 9, 3, 1, AltV),
 FIX("NexT Err.", 21, 3, 1, AltT), FIX("Compile", 32, 0, 1, AltC),
 FIX("Make", 41, 0, 1, AltM), FIX("RUn", 47, 1, 1, AltU),
 BAR(CLOSE_WINDOW, "Close", 52), BAR2(QUIT, "Quit", 66, "Quit", 63)
};

/* The Watches bar. */
static const WpeBarSpec dblst_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), BAR(DEBUG_WATCH, "Make Watch", 9),
 BAR(DEBUG_TRACE, "Trace", 25), BAR(DEBUG_STEP, "Step", 34),
 BAR(DEBUG_RUN, "Run", 43), BAR(CLOSE_WINDOW, "Close", 53),
 BAR2(QUIT, "Quit", 67, "Quit", 64)
};

/* The bar a source window shows while a debug session runs: the keys that
   drive the session, the way Turbo C's bar named them, so the user does not
   have to remember them.  A click sends the key the label names, so "^F10
   Run" would continue and "^F2 Quit" ends the debugger; "F4 Go to cursor" is
   the Borland style's, which is the only style that has one.  The CUA style
   keeps its editing bar: its debugger keys are not decided yet, and a bar
   that names a key doing nothing is worse than one that names none. */
static const WpeBarSpec dblst_src_spec[] = {
 BAR_B(HELP_EDITOR, "Help", 0), BAR_B(DEBUG_TRACE, "Trace", 8),
 BAR_B(DEBUG_STEP, "Step", 17), BAR_B(DEBUG_GOTO_CURSOR, "Go to cursor", 25),
 BAR_B(DEBUG_WATCH, "Make Watch", 41), BAR_B(DEBUG_QUIT, "Quit", 56),
 BAR_B(QUIT, "Quit", 66)
};

/* The bar a source window shows while the program runs under the debugger:
   only keys that work then.  F7, F8, F4 and the breakpoint and watch keys are
   refused until the program stops, so they are not named (they come back
   here when breakpoints and watches can change during a run); a click on
   "^Brk Pause" sends Ctrl-Break, so a terminal that never sends that key can
   still pause the program. */
static const WpeBarSpec dblst_run_spec[] = {
 BAR_B(HELP_EDITOR, "Help", 0), BAR_B(DEBUG_PAUSE, "Pause", 9),
 BAR_B(DEBUG_QUIT, "Quit", 21)
};

/* The Execute, Write, Read, Add and Show bars. */
static const WpeBarSpec xblst_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), FIX("Execute", 19, 0, 1, AltE),
 BAR(CLOSE_WINDOW, "Close W.", 38), BAR2(QUIT, "Quit", 66, "Quit", 63)
};
static const WpeBarSpec wblst_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), FIX("Write", 19, 0, 1, AltW),
 BAR(CLOSE_WINDOW, "Close W.", 38), BAR2(QUIT, "Quit", 66, "Quit", 63)
};
static const WpeBarSpec rblst_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), FIX("Read", 19, 0, 1, AltR),
 BAR(CLOSE_WINDOW, "Close W.", 38), BAR2(QUIT, "Quit", 66, "Quit", 63)
};
static const WpeBarSpec ablst_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), FIX("Add", 19, 0, 1, AltA),
 BAR(CLOSE_WINDOW, "Close W.", 38), BAR2(QUIT, "Quit", 66, "Quit", 63)
};
static const WpeBarSpec sblst_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), FIX("Save", 14, 0, 1, AltS),
 FIX("Change Dir", 25, 0, 1, AltC), BAR(CLOSE_WINDOW, "Close W.", 42),
 BAR(QUIT, "Quit", 66)
};
static const WpeBarSpec gblst_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), FIX("Show", 19, 0, 1, AltS),
 BAR(CLOSE_WINDOW, "Close W.", 38), BAR2(QUIT, "Quit", 66, "Quit", 63)
};
static const WpeBarSpec oblst_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), FIX("Add", 14, 0, 1, AltA),
 FIX("Delete", 25, 0, 1, AltD), BAR(CLOSE_WINDOW, "Close W.", 42),
 BAR2(QUIT, "Quit", 66, "Quit", 63)
};

/* The Help viewer's bar: Backspace and Enter are keys of the viewer, not
   bindings. */
static const WpeBarSpec hblst_spec[] = {
 BAR(HELP_EDITOR, "Help", 0), FIX("<BSP> Back", 9, 0, 5, WPE_DC),
 FIX("<CR> Goto", 21, 0, 4, WPE_CR), FIX(" PreVious", 32, 4, 1, AltV),
 FIX(" NexT", 45, 4, 1, AltT), BAR(CLOSE_WINDOW, "Close", 54),
 BAR2(QUIT, "Quit", 68, "Quit", 65)
};

/* The bars themselves: one array each, as long as its list, filled for the
   style in use. */
WOPT eblst_bar[E_SPEC_LEN(eblst_spec)], eblst_prog_bar[E_SPEC_LEN(eblst_prog_spec)];
WOPT eblst_lsp_bar[E_SPEC_LEN(eblst_lsp_spec)];
#ifdef WPE_AI
WOPT eblst_ai_bar[E_SPEC_LEN(eblst_ai_spec)];
WOPT eblst_lspai_bar[E_SPEC_LEN(eblst_lspai_spec)];
#endif
WOPT fblst_bar[E_SPEC_LEN(fblst_spec)], mblst_bar[E_SPEC_LEN(mblst_spec)];
WOPT dblst_bar[E_SPEC_LEN(dblst_spec)];
WOPT dblst_src_bar[E_SPEC_LEN(dblst_src_spec)], dblst_run_bar[E_SPEC_LEN(dblst_run_spec)];
WOPT xblst_bar[E_SPEC_LEN(xblst_spec)], wblst_bar[E_SPEC_LEN(wblst_spec)];
WOPT rblst_bar[E_SPEC_LEN(rblst_spec)], ablst_bar[E_SPEC_LEN(ablst_spec)];
WOPT sblst_bar[E_SPEC_LEN(sblst_spec)], hblst_bar[E_SPEC_LEN(hblst_spec)];
WOPT gblst_bar[E_SPEC_LEN(gblst_spec)], oblst_bar[E_SPEC_LEN(oblst_spec)];

/* Each bar with its list and, once filled, how many entries the style in use
   gave it. */
static struct {
 WOPT *bar;
 const WpeBarSpec *spec;
 int n;           /* entries in the list */
 int count;       /* entries in the style in use */
} e_bars[] = {
#define E_BUILD(name) { name##_bar, name##_spec, E_SPEC_LEN(name##_spec), 0 }
 E_BUILD(eblst), E_BUILD(eblst_prog), E_BUILD(eblst_lsp),
#ifdef WPE_AI
 E_BUILD(eblst_ai), E_BUILD(eblst_lspai),
#endif
 E_BUILD(fblst), E_BUILD(mblst), E_BUILD(dblst),
 E_BUILD(dblst_src), E_BUILD(dblst_run),
 E_BUILD(xblst), E_BUILD(wblst), E_BUILD(rblst), E_BUILD(ablst),
 E_BUILD(sblst), E_BUILD(hblst), E_BUILD(gblst), E_BUILD(oblst)
#undef E_BUILD
};

#define E_BARS_COUNT ((int)(sizeof(e_bars) / sizeof(e_bars[0])))

/* The labels of the entries bound to an action: one slot each, kept while the
   style lasts because a bar holds a pointer to its label.  A bar whose labels
   do not fit gets the words alone, so the pool can never be overrun. */
#define E_BAR_LABEL_LEN 40
#define E_BAR_LABELS_MAX 256
static char e_bar_labels[E_BAR_LABELS_MAX][E_BAR_LABEL_LEN];
static int e_bar_labels_used;

/**
 * e_bar_label_slot - The next free slot for the label of an entry.
 * Return: a buffer of E_BAR_LABEL_LEN bytes, NULL when the pool is used up.
 */
static char *e_bar_label_slot(void)
{
 if (e_bar_labels_used >= E_BAR_LABELS_MAX)
  return(NULL);
 return(e_bar_labels[e_bar_labels_used++]);
}

/**
 * e_bar_fill_entry - Make one bar entry from its list entry.
 * @dst: the entry to fill; @spec: what it is made from; @style: the key style.
 *
 * An entry bound to an action reads "<key> <words>": the key and the code a
 * click sends are what the key table names for the action in this style, and
 * the key is the part drawn in the hot colour.
 */
static void e_bar_fill_entry(WOPT *dst, const WpeBarSpec *spec, int style)
{
 char *label = NULL;
 int keylen = 0;

 dst->x = spec->x[style];
 if (spec->action == WPE_ACT_NONE)
 {
  dst->t = (char *)spec->text[style];
  dst->s = spec->hi;
  dst->n = spec->n;
  dst->as = spec->code;
  return;
 }
 label = e_bar_label_slot();
 if (label)
  wpe_bar_entry_text(label, E_BAR_LABEL_LEN, (WpeAction)spec->action, style,
                     spec->text[style], &keylen);
 dst->t = label ? label : (char *)spec->text[style];
 dst->s = 0;
 dst->n = keylen;
 dst->as = wpe_binding_bar_code((WpeAction)spec->action, style);
}

/**
 * e_bar_fill - Fill one bar for a style: the entries the style has, in order.
 * @b: a row of e_bars; @style: the key style.
 */
static void e_bar_fill(int b, int style)
{
 int j, used = 0;

 for (j = 0; j < e_bars[b].n; j++)
 {
  if (!(e_bars[b].spec[j].styles & WPE_STYLE_BIT(style)))
   continue;
  e_bar_fill_entry(&e_bars[b].bar[used++], &e_bars[b].spec[j], style);
 }
 e_bars[b].count = used;
}

void e_bars_use_style(int style)
{
 static int built = -1;
 int b;

 if (built == style)
  return;
 built = style;
 e_bar_labels_used = 0;
 for (b = 0; b < E_BARS_COUNT; b++)
  e_bar_fill(b, style);
}

int e_bar_length(WOPT *bar)
{
 int b;

 for (b = 0; b < E_BARS_COUNT; b++)
  if (e_bars[b].bar == bar)
   return(e_bars[b].count);
 return(0);
}
