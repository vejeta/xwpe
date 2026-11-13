#ifndef WE_BARS_H
#define WE_BARS_H

/* we_bars.h -- the bottom bars (the row of clickable key hints under a window).

   Every bar is one list of entries, each an action of the key table
   (we_bindings.c) or a fixed mnemonic.  The list is turned into the entries a
   window draws and clicks when a key style is chosen: the key a bar names and
   the key a click sends come from the table for that style, so a bar cannot
   name a key the style does not bind.  A bar is one array that every window
   points at; changing the key style fills it again.  Needs edit.h for WOPT. */

extern WOPT eblst_bar[], eblst_prog_bar[];
extern WOPT eblst_lsp_bar[];
#ifdef WPE_AI
extern WOPT eblst_ai_bar[], eblst_lspai_bar[];
#endif
extern WOPT fblst_bar[], mblst_bar[], dblst_bar[];
extern WOPT dblst_src_bar[], dblst_run_bar[];
extern WOPT xblst_bar[], wblst_bar[], rblst_bar[];
extern WOPT ablst_bar[], sblst_bar[], hblst_bar[];
extern WOPT gblst_bar[], oblst_bar[];

/**
 * e_bars_use_style - Fill every bottom bar for a key style.
 * @style: WPE_STYLE_*.
 *
 * The bars keep their address; their entries and their length become the
 * style's.  Asking for the style already in use changes nothing, so the
 * label a language server's bar was given stays.
 */
void e_bars_use_style(int style);

/**
 * e_bar_length - How many entries a bar has in the style in use.
 * @bar: any bottom bar.
 * Return: the number of entries, 0 for an array that is not a bar.
 */
int e_bar_length(WOPT *bar);

#endif /* WE_BARS_H */
