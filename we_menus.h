#ifndef WE_MENUS_H
#define WE_MENUS_H

/* we_menus.h -- the menus of the menu bar, as data.

   One table (wpe_menu_rows) names every item of every dropdown with the
   words, the hot letter, what it runs, which key text it shows and in which
   key style, block mode and editor mode it is there.  The dropdown builder,
   the manual's generator and the palette read the same rows, so a menu is a
   line and cannot disagree with the text that describes it.  The bar itself
   (the titles, their columns and Alt letters) is wpe_menu_heads.

   The dropdown engine (we_menue.c) is blind to the mode: it draws and runs
   the rows it is given. */

#include <stddef.h>

#include "we_bindings.h"

/** The menus, in the order of the widest bar. */
typedef enum {
 WPE_MENU_SYSTEM = 0,
 WPE_MENU_FILE,
 WPE_MENU_EDIT,
 WPE_MENU_SEARCH,
 WPE_MENU_BLOCK,
 WPE_MENU_RUN,
 WPE_MENU_DEBUG,
 WPE_MENU_PROJECT,
 WPE_MENU_OPTIONS,
 WPE_MENU_WINDOW,
 WPE_MENU_HELP,
 WPE_MENU_COUNT
} WpeMenuId;

/** The three bars the editor shows: the plain editor, the programming editor
    built without the debugger, and the programming editor with it. */
typedef enum {
 WPE_BAR_PLAIN = 0,
 WPE_BAR_PROG,
 WPE_BAR_PROG_DEBUG,
 WPE_BAR_COUNT
} WpeBarComposition;

/** The block modes, as bits, for the rows that depend on one. */
#define WPE_BLOCKS_ANCHOR    0x01u
#define WPE_BLOCKS_WORDSTAR  0x02u
#define WPE_BLOCKS_SELECTION 0x04u

/** Where a row is: 0 everywhere, else the modes it needs. */
#define WPE_WHERE_PROG     0x01u  /* the programming editor only */
#define WPE_WHERE_TERMINAL 0x02u  /* not on a graphical display */
#define WPE_WHERE_AI       0x04u  /* only when the assistant is built in */

/** One item of one dropdown.
    A row is shown for a key style in @styles, and also for a block mode in
    @blocks: Mark Word is the Modern style's and the Selection mode's. */
typedef struct {
 unsigned char menu;      /* WPE_MENU_*: the dropdown it is in */
 const char *stem;        /* the words: "Mark WhOle" */
 signed char hot;         /* index of the hot letter in the stem */
 char letter;             /* the letter typed with the menu open */
 unsigned short action;   /* the action whose keys are shown; NONE: words alone */
 unsigned contexts;       /* WPE_CTXM_*: the contexts whose keys are shown */
 unsigned char styles;    /* WPE_STYLE_BIT(): the styles that always have the row */
 unsigned char blocks;    /* WPE_BLOCKS_*: the block modes that have it in the others */
 unsigned char where;     /* WPE_WHERE_* */
 const char *sep;         /* between the keys, " / " when NULL */
 int (*fkt)();            /* what it runs */
} WpeMenuRow;

/** One entry of the bar, with its dropdown's geometry. */
typedef struct {
 const char *title;
 char letter;                 /* the plain letter that opens it */
 int alt;                     /* the key that opens it */
 int x[WPE_BAR_COUNT];        /* its column in each bar; 0: not in that bar */
 int width;                   /* the dropdown's width */
 int position_terminal;       /* the dropdown's offset from the column */
 int position_graphical;      /* the same on a graphical display */
 int item_width;              /* the width of an item's words and keys */
 int help;                    /* the row of the help line it shows */
} WpeMenuHead;

/** The facts a menu is a function of. */
typedef struct {
 int style;     /* WPE_STYLE_* */
 int blocks;    /* WPE_BLOCKS_* (one bit) */
 int prog;      /* 1 in the programming editor */
 int xwin;      /* 1 on a graphical display */
 int assistant; /* 1 when the assistant is built in */
} WpeMenuFacts;

const WpeMenuRow *wpe_menu_rows(size_t *n);
const WpeMenuHead *wpe_menu_heads(size_t *n);

/**
 * wpe_menu_row_shown - Is this row in the menu for these facts?
 * @r: a row; @f: the key style, block mode, editor mode and display.
 *
 * The one filter of the menus: no code branch decides a row.
 */
int wpe_menu_row_shown(const WpeMenuRow *r, const WpeMenuFacts *f);

/**
 * wpe_menu_bar_composition - Which bar the editor in this mode shows.
 * @prog: 1 in the programming editor.
 */
int wpe_menu_bar_composition(int prog);

/**
 * wpe_menu_head_shown - Is this head in the bar of @composition?
 */
int wpe_menu_head_shown(const WpeMenuHead *h, int composition);

/**
 * wpe_block_mode - The block mode the editor options select.
 * @edopt: the editor's option word.
 * Return: WPE_BLOCKS_ANCHOR, WPE_BLOCKS_WORDSTAR or WPE_BLOCKS_SELECTION.
 * Defined in we_opt.c, which owns the block mode bits.
 */
int wpe_block_mode(int edopt);

#endif /* WE_MENUS_H */
