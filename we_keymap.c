/* we_keymap.c -- what a key means, once.  See we_keymap.h.

   The three display backends (we_term.c, we_xterm.c, we_wayland.c) say which
   key and which modifiers their platform delivered; the table below says what
   the editor does with that pair.  Key names of the platforms (curses
   KEY_*, X keysyms, escape sequence tails) are spelled here and nowhere else
   in the display code. */

#include <string.h>

#include "we_keymap.h"
#include "keys.h"
#include "unixkeys.h"

#ifdef NCURSES
#include <curses.h>
#endif

/* ------------------------------------------------------------------------ */
/* The table                                                                 */
/* ------------------------------------------------------------------------ */

#define NONE WPE_MOD_NONE
#define CTRL WPE_MOD_CTRL
#define ALT  WPE_MOD_ALT
#define SHFT WPE_MOD_SHIFT

/* The terminal's "Alt-<cursor key>" rows (ESC then the key) keep the codes
   the console always gave them: Alt-Left and Alt-Right move by word like
   Ctrl-Left and Ctrl-Right, Alt-Up and Alt-Down page, Alt-Home and Alt-End
   jump like Ctrl-Home and Ctrl-End.  They are rows here, so the graphical
   displays give the same answer. */
const WpeKeyRow wpe_key_table[] = {
 { WPE_KEY_F1,  NONE, F1  }, { WPE_KEY_F1,  CTRL, CF1  }, { WPE_KEY_F1,  ALT, AF1  },
 { WPE_KEY_F2,  NONE, F2  }, { WPE_KEY_F2,  CTRL, CF2  }, { WPE_KEY_F2,  ALT, AF2  },
 { WPE_KEY_F3,  NONE, F3  }, { WPE_KEY_F3,  CTRL, CF3  }, { WPE_KEY_F3,  ALT, AF3  },
 { WPE_KEY_F4,  NONE, F4  }, { WPE_KEY_F4,  CTRL, CF4  }, { WPE_KEY_F4,  ALT, AF4  },
 { WPE_KEY_F5,  NONE, F5  }, { WPE_KEY_F5,  CTRL, CF5  }, { WPE_KEY_F5,  ALT, AF5  },
 { WPE_KEY_F6,  NONE, F6  }, { WPE_KEY_F6,  CTRL, CF6  }, { WPE_KEY_F6,  ALT, AF6  },
 { WPE_KEY_F7,  NONE, F7  }, { WPE_KEY_F7,  CTRL, CF7  }, { WPE_KEY_F7,  ALT, AF7  },
 { WPE_KEY_F8,  NONE, F8  }, { WPE_KEY_F8,  CTRL, CF8  }, { WPE_KEY_F8,  ALT, AF8  },
 { WPE_KEY_F9,  NONE, F9  }, { WPE_KEY_F9,  CTRL, CF9  }, { WPE_KEY_F9,  ALT, AF9  },
 { WPE_KEY_F10, NONE, F10 }, { WPE_KEY_F10, CTRL, CF10 }, { WPE_KEY_F10, ALT, AF10 },
 { WPE_KEY_F11, NONE, F11 }, { WPE_KEY_F11, CTRL, CF11 }, { WPE_KEY_F11, ALT, AF11 },
 { WPE_KEY_F12, NONE, F12 }, { WPE_KEY_F12, CTRL, CF12 }, { WPE_KEY_F12, ALT, AF12 },

 { WPE_KEY_UP,    NONE, CUP  }, { WPE_KEY_UP,    ALT, BUP  },
 { WPE_KEY_DOWN,  NONE, CDO  }, { WPE_KEY_DOWN,  ALT, BDO  },
 { WPE_KEY_LEFT,  NONE, CLE  }, { WPE_KEY_LEFT,  CTRL, CCLE }, { WPE_KEY_LEFT,  ALT, CCLE },
 { WPE_KEY_RIGHT, NONE, CRI  }, { WPE_KEY_RIGHT, CTRL, CCRI }, { WPE_KEY_RIGHT, ALT, CCRI },
 { WPE_KEY_HOME,  NONE, POS1 }, { WPE_KEY_HOME,  CTRL, CPS1 }, { WPE_KEY_HOME,  ALT, CPS1 },
 { WPE_KEY_END,   NONE, ENDE }, { WPE_KEY_END,   CTRL, CEND }, { WPE_KEY_END,   ALT, CEND },
 { WPE_KEY_PGUP,  NONE, BUP  }, { WPE_KEY_PGUP,  CTRL, CBUP }, { WPE_KEY_PGUP,  ALT, CBUP },
 { WPE_KEY_PGDN,  NONE, BDO  }, { WPE_KEY_PGDN,  CTRL, CBDO }, { WPE_KEY_PGDN,  ALT, CBDO },

 /* Alt-Insert and Alt-Delete copy to and paste from the X selection buffer
    (the manual's reference chapter); Shift-Insert and Shift-Delete are
    Insert and Delete plus the shift offset.  Delete with two modifiers
    keeps the order the displays always used: Control, then Shift, then Alt. */
 { WPE_KEY_INSERT, NONE, EINFG }, { WPE_KEY_INSERT, CTRL, CEINFG }, { WPE_KEY_INSERT, ALT, AltEin },
 { WPE_KEY_DELETE, NONE, ENTF  }, { WPE_KEY_DELETE, CTRL, CENTF  }, { WPE_KEY_DELETE, ALT, AltDel },
 { WPE_KEY_DELETE, CTRL | SHFT, CENTF }, { WPE_KEY_DELETE, ALT | SHFT, ShiftDel },

 /* Alt-Backspace undoes and Shift-Alt-Backspace redoes, as in Turbo C. */
 { WPE_KEY_BACKSPACE, NONE, CtrlH }, { WPE_KEY_BACKSPACE, ALT, AltBS },
 { WPE_KEY_BACKSPACE, ALT | SHFT, SABS },

 { WPE_KEY_TAB, NONE, WPE_TAB }, { WPE_KEY_TAB, SHFT, WPE_BTAB },

 { WPE_KEY_HELP, NONE, HELP }, { WPE_KEY_HELP, ALT, AF1 },
 { WPE_KEY_BREAK, NONE, CtrlBrk },

 /* F11 and F12 are keys, but the Borland key set gives them no action: their
    symbols are also the Sun Stop and Again keys, which quit the editor and
    redo, a surprise on a PC keyboard. */
 { WPE_KEY_L3, NONE, PROPS }, { WPE_KEY_L4, NONE, UNDO  }, { WPE_KEY_L5, NONE, FRONT },
 { WPE_KEY_L6, NONE, COPY  }, { WPE_KEY_L7, NONE, OPEN  }, { WPE_KEY_L8, NONE, PASTE },
 { WPE_KEY_L9, NONE, FID   }, { WPE_KEY_L10, NONE, CUT  },

 { WPE_KEY_NONE, NONE, 0 }
};

#undef NONE
#undef CTRL
#undef ALT
#undef SHFT

/**
 * wpe_key_row_code - The code of the row for exactly this pair.
 * @base: the key.
 * @mods: the modifiers, matched exactly.
 *
 * Return: the row's code, or 0 when the table has no row for the pair.
 */
static int wpe_key_row_code(WpeBaseKey base, unsigned mods)
{
 const WpeKeyRow *row;

 for (row = wpe_key_table; row->base != WPE_KEY_NONE; row++)
  if (row->base == base && row->mods == mods)
   return(row->code);
 return(0);
}

/**
 * wpe_key_shift_adds_offset - Does Shift on this key mean the plain code
 * plus WPE_KEY_SHIFT_OFFSET?
 *
 * True for the function keys, the cursor block and the Sun keys, where
 * keys.h keeps a shifted code (SF1, ShiftDel, ...) at that distance.  Not
 * for Backspace, Tab (Shift-Tab has its own row) or Break, whose shifted
 * forms are not keys.
 */
static int wpe_key_shift_adds_offset(WpeBaseKey base)
{
 return(base != WPE_KEY_BACKSPACE && base != WPE_KEY_TAB &&
        base != WPE_KEY_BREAK);
}

int wpe_key_code(WpeBaseKey base, unsigned mods)
{
 int code, plain;

 if (mods & WPE_MOD_CTRL)
  mods &= ~WPE_MOD_ALT;          /* Control wins when both are held */
 code = wpe_key_row_code(base, mods);
 if (code != 0 || !(mods & WPE_MOD_SHIFT) || !wpe_key_shift_adds_offset(base))
  return(code);
 plain = wpe_key_row_code(base, mods & ~WPE_MOD_SHIFT);
 if (plain == 0)
  return(0);
 return(plain + WPE_KEY_SHIFT_OFFSET);
}

/* ------------------------------------------------------------------------ */
/* Curses                                                                    */
/* ------------------------------------------------------------------------ */

#ifdef NCURSES

/* terminfo's layout of KEY_F(n), from xterm and the entries derived from it
   (xterm-256color, screen, tmux-256color, xterm-direct): twelve keys a block,
   plain, then Shift, Ctrl, Ctrl+Shift, Alt, Alt+Shift. */
#define WPE_CURSES_F_PER_BLOCK 12
#define WPE_CURSES_F_BLOCKS 6
/* The last function key curses names: KEY_F(64) is KEY_DL, a different key. */
#define WPE_CURSES_F_LAST 63

/* The Linux console has no Ctrl or Alt function keys and sends Shift-F1 to
   Shift-F10 as KEY_F(11) to KEY_F(20) with its shift state set (an ioctl the
   editor reads).  Those numbers mean Shift-F(n-10) only then: xterm sends the
   same numbers for F11, F12 and Shift-F1..F8. */
#define WPE_CONSOLE_SHIFT_F_FIRST 11
#define WPE_CONSOLE_SHIFT_F_LAST  20
#define WPE_CONSOLE_SHIFT_F_SKIP  10

/**
 * wpe_curses_function_key - A KEY_F(n) as a key and its modifiers.
 * @n: the function key number terminfo gave it, 1 and up.
 * @console_mods: WPE_MOD_* the Linux console reports.
 * @base: out; @mods: out.
 *
 * Return: 1 for a key, 0 for a number past the layout.
 */
static int wpe_curses_function_key(int n, unsigned console_mods,
                                   WpeBaseKey *base, unsigned *mods)
{
 static const unsigned block_mods[WPE_CURSES_F_BLOCKS] = {
  WPE_MOD_NONE, WPE_MOD_SHIFT, WPE_MOD_CTRL,
  WPE_MOD_CTRL | WPE_MOD_SHIFT, WPE_MOD_ALT, WPE_MOD_ALT | WPE_MOD_SHIFT
 };
 int block;

 if ((console_mods & WPE_MOD_SHIFT) && n >= WPE_CONSOLE_SHIFT_F_FIRST &&
     n <= WPE_CONSOLE_SHIFT_F_LAST)
 {
  *base = (WpeBaseKey)(WPE_KEY_F1 + n - WPE_CONSOLE_SHIFT_F_SKIP - 1);
  *mods = console_mods;
  return(1);
 }
 block = (n - 1) / WPE_CURSES_F_PER_BLOCK;
 if (block >= WPE_CURSES_F_BLOCKS)
  return(0);
 *base = (WpeBaseKey)(WPE_KEY_F1 + (n - 1) % WPE_CURSES_F_PER_BLOCK);
 *mods = block_mods[block] | console_mods;
 return(1);
}

/** One curses key code: the key it names and the modifiers it carries. */
typedef struct {
 int ch;
 WpeBaseKey base;
 unsigned mods;
} WpeCursesRow;

/* The cursor and editing keys curses names, plain and shifted.  terminfo's
   kLFT, kRIT, kri, kind, kHOM, kEND, kPRV, kNXT, kDC and kIC are the
   shifted keys (xterm sends "ESC [ 1;2C" for Shift-Right, which curses
   folds into KEY_SRIGHT), so Shift and a cursor key marks a block whatever
   the terminfo entry folds. */
static const WpeCursesRow wpe_curses_rows[] = {
 { KEY_UP,        WPE_KEY_UP,        WPE_MOD_NONE  },
 { KEY_DOWN,      WPE_KEY_DOWN,      WPE_MOD_NONE  },
 { KEY_LEFT,      WPE_KEY_LEFT,      WPE_MOD_NONE  },
 { KEY_RIGHT,     WPE_KEY_RIGHT,     WPE_MOD_NONE  },
 { KEY_HOME,      WPE_KEY_HOME,      WPE_MOD_NONE  },
 { KEY_END,       WPE_KEY_END,       WPE_MOD_NONE  },
 { KEY_LL,        WPE_KEY_END,       WPE_MOD_NONE  },
 { KEY_PPAGE,     WPE_KEY_PGUP,      WPE_MOD_NONE  },
 { KEY_NPAGE,     WPE_KEY_PGDN,      WPE_MOD_NONE  },
 { KEY_IC,        WPE_KEY_INSERT,    WPE_MOD_NONE  },
 { KEY_DC,        WPE_KEY_DELETE,    WPE_MOD_NONE  },
 { KEY_BACKSPACE, WPE_KEY_BACKSPACE, WPE_MOD_NONE  },
 { KEY_HELP,      WPE_KEY_HELP,      WPE_MOD_NONE  },
 { KEY_BTAB,      WPE_KEY_TAB,       WPE_MOD_SHIFT },
 { KEY_PREVIOUS,  WPE_KEY_UP,        WPE_MOD_SHIFT },
 { KEY_NEXT,      WPE_KEY_DOWN,      WPE_MOD_SHIFT },
 { KEY_SR,        WPE_KEY_UP,        WPE_MOD_SHIFT },
 { KEY_SF,        WPE_KEY_DOWN,      WPE_MOD_SHIFT },
 { KEY_SLEFT,     WPE_KEY_LEFT,      WPE_MOD_SHIFT },
 { KEY_SRIGHT,    WPE_KEY_RIGHT,     WPE_MOD_SHIFT },
 { KEY_SHOME,     WPE_KEY_HOME,      WPE_MOD_SHIFT },
 { KEY_SEND,      WPE_KEY_END,       WPE_MOD_SHIFT },
 { KEY_SPREVIOUS, WPE_KEY_PGUP,      WPE_MOD_SHIFT },
 { KEY_SNEXT,     WPE_KEY_PGDN,      WPE_MOD_SHIFT },
 { KEY_SIC,       WPE_KEY_INSERT,    WPE_MOD_SHIFT },
 { KEY_SDC,       WPE_KEY_DELETE,    WPE_MOD_SHIFT },
#ifdef KEY_BREAK
 { KEY_BREAK,     WPE_KEY_BREAK,     WPE_MOD_NONE  },   /* where a terminal sends it */
#endif
 { 0,             WPE_KEY_NONE,      WPE_MOD_NONE  }
};

/**
 * wpe_curses_named_key - A curses cursor or editing key as a key.
 * @ch: the KEY_* code.
 * @base: out; @mods: out, the modifiers the code itself carries.
 *
 * Return: 1 when @ch is one of them, 0 otherwise.
 */
static int wpe_curses_named_key(int ch, WpeBaseKey *base, unsigned *mods)
{
 const WpeCursesRow *row;

 for (row = wpe_curses_rows; row->base != WPE_KEY_NONE; row++)
  if (row->ch == ch)
  {
   *base = row->base;
   *mods = row->mods;
   return(1);
  }
 return(0);
}

int wpe_key_from_curses(int ch, unsigned console_mods, WpeBaseKey *base,
                        unsigned *mods)
{
 unsigned own;

 if (ch >= KEY_F(1) && ch <= KEY_F(WPE_CURSES_F_LAST))
  return(wpe_curses_function_key(ch - KEY_F(0), console_mods, base, mods));
 if (!wpe_curses_named_key(ch, base, &own))
  return(0);
 *mods = own | console_mods;
 return(1);
}

#else  /* !NCURSES */

int wpe_key_from_curses(int ch, unsigned console_mods, WpeBaseKey *base,
                        unsigned *mods)
{
 (void)ch; (void)console_mods; (void)base; (void)mods;
 return(0);
}

#endif /* NCURSES */

/* ------------------------------------------------------------------------ */
/* X and Wayland keysyms                                                     */
/* ------------------------------------------------------------------------ */

/* The X keysym values.  Xlib's XK_* and xkbcommon's XKB_KEY_* are the same
   numbers (the protocol fixed them), so a display library is not needed
   here; the unit test checks every value against both headers. */
#define WPE_KSYM_BACKSPACE   0xff08UL
#define WPE_KSYM_ISO_LEFT_TAB 0xfe20UL
#define WPE_KSYM_PAUSE       0xff13UL
#define WPE_KSYM_HOME        0xff50UL
#define WPE_KSYM_LEFT        0xff51UL
#define WPE_KSYM_UP          0xff52UL
#define WPE_KSYM_RIGHT       0xff53UL
#define WPE_KSYM_DOWN        0xff54UL
#define WPE_KSYM_PRIOR       0xff55UL
#define WPE_KSYM_NEXT        0xff56UL
#define WPE_KSYM_END         0xff57UL
#define WPE_KSYM_HELP        0xff6aUL
#define WPE_KSYM_BREAK       0xff6bUL
#define WPE_KSYM_INSERT      0xff63UL
#define WPE_KSYM_DELETE      0xffffUL
#define WPE_KSYM_F1          0xffbeUL
/* F11 to F20 are also the Sun keys L1 to L10: one set of symbols.  F11 and
   F12 are read as the PC keyboard's keys, F13 to F20 as Sun's L3 to L10. */
#define WPE_KSYM_F12         0xffc9UL
#define WPE_KSYM_L3          0xffcaUL
#define WPE_KSYM_L10         0xffd1UL

/**
 * wpe_keysym_range_key - A keysym in a run of consecutive symbols as a key.
 * @keysym: the symbol; @first, @last: the run's ends; @first_key: the key
 * the run starts at.
 * Return: the key, or WPE_KEY_NONE outside the run.
 */
static WpeBaseKey wpe_keysym_range_key(unsigned long keysym, unsigned long first,
                                       unsigned long last, WpeBaseKey first_key)
{
 if (keysym < first || keysym > last)
  return(WPE_KEY_NONE);
 return((WpeBaseKey)(first_key + (int)(keysym - first)));
}

/**
 * wpe_keysym_named_key - A cursor, editing or special keysym as a key.
 * @keysym: the symbol.
 * Return: the key, or WPE_KEY_NONE.
 */
static WpeBaseKey wpe_keysym_named_key(unsigned long keysym)
{
 switch (keysym)
 {
  case WPE_KSYM_LEFT:      return(WPE_KEY_LEFT);
  case WPE_KSYM_RIGHT:     return(WPE_KEY_RIGHT);
  case WPE_KSYM_UP:        return(WPE_KEY_UP);
  case WPE_KSYM_DOWN:      return(WPE_KEY_DOWN);
  case WPE_KSYM_HOME:      return(WPE_KEY_HOME);
  case WPE_KSYM_END:       return(WPE_KEY_END);
  case WPE_KSYM_PRIOR:     return(WPE_KEY_PGUP);
  case WPE_KSYM_NEXT:      return(WPE_KEY_PGDN);
  case WPE_KSYM_INSERT:    return(WPE_KEY_INSERT);
  case WPE_KSYM_DELETE:    return(WPE_KEY_DELETE);
  case WPE_KSYM_BACKSPACE: return(WPE_KEY_BACKSPACE);
  case WPE_KSYM_HELP:      return(WPE_KEY_HELP);
  default:                 return(WPE_KEY_NONE);
 }
}

/* Tab's keysym, and the printable ASCII keysyms (their own character). */
#define WPE_KSYM_TAB         0xff09UL
#define WPE_KSYM_TAB_CODEPOINT 9
#define WPE_KSYM_ASCII_FIRST 0x20UL
#define WPE_KSYM_ASCII_LAST  0x7eUL

int wpe_keysym_chord_codepoint(unsigned long keysym)
{
 if (keysym == WPE_KSYM_TAB)
  return(WPE_KSYM_TAB_CODEPOINT);
 if (keysym >= 'A' && keysym <= 'Z')
  return((int)(keysym - 'A' + 'a'));
 if (keysym >= WPE_KSYM_ASCII_FIRST && keysym <= WPE_KSYM_ASCII_LAST)
  return((int)keysym);
 return(0);
}

int wpe_keysym_is_chord_letter(unsigned long keysym)
{
 int cp = wpe_keysym_chord_codepoint(keysym);

 return(cp >= 'a' && cp <= 'z');
}

int wpe_key_from_keysym(unsigned long keysym, unsigned *mods, WpeBaseKey *base)
{
 WpeBaseKey key;

 if (keysym == WPE_KSYM_ISO_LEFT_TAB)
 {
  *base = WPE_KEY_TAB;               /* back-tab, with Ctrl or Alt held too */
  *mods = WPE_MOD_SHIFT;
  return(1);
 }
 if (keysym == WPE_KSYM_BREAK || (keysym == WPE_KSYM_PAUSE && (*mods & WPE_MOD_CTRL)))
 {
  *base = WPE_KEY_BREAK;             /* Ctrl+Pause is Break; Pause alone is not */
  *mods = WPE_MOD_NONE;
  return(1);
 }
 key = wpe_keysym_range_key(keysym, WPE_KSYM_F1, WPE_KSYM_F12, WPE_KEY_F1);
 if (key == WPE_KEY_NONE)
  key = wpe_keysym_range_key(keysym, WPE_KSYM_L3, WPE_KSYM_L10, WPE_KEY_L3);
 if (key == WPE_KEY_NONE)
  key = wpe_keysym_named_key(keysym);
 if (key == WPE_KEY_NONE)
  return(0);
 *base = key;
 return(1);
}

/* ------------------------------------------------------------------------ */
/* Escape sequence tails                                                     */
/* ------------------------------------------------------------------------ */

/* The xterm modifier parameter is 1 plus a bit set (8 is Meta, which the
   editor does not read); a larger number is not a modifier. */
#define WPE_CSI_MOD_SHIFT 1
#define WPE_CSI_MOD_ALT   2
#define WPE_CSI_MOD_CTRL  4
#define WPE_CSI_MOD_PARAM_MAX 16

/**
 * wpe_csi_modifier_bits - The modifiers an xterm parameter stands for.
 * @param: the number after the ';' (2 is Shift, 5 is Ctrl, 3 is Alt, ...);
 *   0 when there was none.
 * Return: WPE_MOD_* bits.
 */
static unsigned wpe_csi_modifier_bits(int param)
{
 unsigned mods = WPE_MOD_NONE;

 if (param < 2)
  return(WPE_MOD_NONE);
 param -= 1;
 if (param & WPE_CSI_MOD_SHIFT)
  mods |= WPE_MOD_SHIFT;
 if (param & WPE_CSI_MOD_ALT)
  mods |= WPE_MOD_ALT;
 if (param & WPE_CSI_MOD_CTRL)
  mods |= WPE_MOD_CTRL;
 return(mods);
}

/**
 * wpe_csi_letter_key - The key a final letter names.
 * @letter: A..D, H, F (cursor block), P..S (F1..F4), Z (Shift-Tab).
 * @base: out; @mods: out, set only for Z.
 * Return: 1 when the letter names a key.
 */
static int wpe_csi_letter_key(int letter, WpeBaseKey *base, unsigned *mods)
{
 *mods = WPE_MOD_NONE;
 switch (letter)
 {
  case 'A': *base = WPE_KEY_UP; return(1);
  case 'B': *base = WPE_KEY_DOWN; return(1);
  case 'C': *base = WPE_KEY_RIGHT; return(1);
  case 'D': *base = WPE_KEY_LEFT; return(1);
  case 'H': *base = WPE_KEY_HOME; return(1);
  case 'F': *base = WPE_KEY_END; return(1);
  case 'P': *base = WPE_KEY_F1; return(1);
  case 'Q': *base = WPE_KEY_F2; return(1);
  case 'R': *base = WPE_KEY_F3; return(1);
  case 'S': *base = WPE_KEY_F4; return(1);
  case 'Z': *base = WPE_KEY_TAB; *mods = WPE_MOD_SHIFT; return(1);
  default:  return(0);
 }
}

/**
 * wpe_csi_number_key - The key "<n>~" names.
 * @number: the parameter before the '~' (vt220 numbering: 1 and 7 Home,
 *   2 Insert, 3 Delete, 4 and 8 End, 5 and 6 the pages, 11..15 F1..F5,
 *   17..21 F6..F10, 23 and 24 F11 and F12).
 * @base: out.
 * Return: 1 when the number names a key.
 */
static int wpe_csi_number_key(int number, WpeBaseKey *base)
{
 switch (number)
 {
  case 1: case 7:  *base = WPE_KEY_HOME; return(1);
  case 2:          *base = WPE_KEY_INSERT; return(1);
  case 3:          *base = WPE_KEY_DELETE; return(1);
  case 4: case 8:  *base = WPE_KEY_END; return(1);
  case 5:          *base = WPE_KEY_PGUP; return(1);
  case 6:          *base = WPE_KEY_PGDN; return(1);
  case 11: case 12: case 13: case 14: case 15:
   *base = (WpeBaseKey)(WPE_KEY_F1 + number - 11);
   return(1);
  case 17: case 18: case 19: case 20: case 21:
   *base = (WpeBaseKey)(WPE_KEY_F6 + number - 17);
   return(1);
  case 23: case 24:
   *base = (WpeBaseKey)(WPE_KEY_F11 + number - 23);
   return(1);
  default: return(0);
 }
}

/* No key's parameter comes near this; a longer run of digits (line noise, a
   sequence of another kind) stays at it instead of overflowing. */
#define WPE_CSI_NUMBER_MAX 9999

/**
 * wpe_csi_add_digit - One more decimal digit of a sequence's parameter.
 * @value: the number so far; @digit: 0..9.
 *
 * Keeps a long run of digits from overflowing, so a stray sequence is just
 * a number no key has.
 * Return: the new number, at most WPE_CSI_NUMBER_MAX.
 */
static int wpe_csi_add_digit_max(int value, int digit, int max)
{
 if (value > (max - digit) / 10)
  return(max);
 return(value * 10 + digit);
}

static int wpe_csi_add_digit(int value, int digit)
{
 return(wpe_csi_add_digit_max(value, digit, WPE_CSI_NUMBER_MAX));
}

/**
 * wpe_csi_parse_numbers - Read "<a>" or "<a>;<b>" from the start of a tail.
 * @tail: the tail; @n: its length.
 * @first: out, the first number (0 when there is none); @second: out, the
 *   second (0 when there is none).
 * Return: the index of the final byte, or -1 when the tail holds anything
 * but digits and one ';' before it.
 */
static int wpe_csi_parse_numbers(const char *tail, size_t n, int *first,
                                 int *second)
{
 size_t i;
 int *into = first;

 *first = 0;
 *second = 0;
 for (i = 0; i + 1 < n; i++)
 {
  if (tail[i] >= '0' && tail[i] <= '9')
   *into = wpe_csi_add_digit(*into, tail[i] - '0');
  else if (tail[i] == ';' && into == first)
   into = second;
  else
   return(-1);
 }
 return((int)i);
}

/** A final byte that ends a numbered key and the modifiers it stands for. */
typedef struct {
 int final_byte;
 unsigned mods;
} WpeCsiNumberEnding;

/* xterm ends every numbered key ("ESC [ 8 ~") with '~' and puts the
   modifiers in a parameter; rxvt says them with the final byte instead:
   "ESC [ 8 $" is Shift-End, "ESC [ 8 ^" Ctrl-End, "ESC [ 8 @" both. */
static const WpeCsiNumberEnding wpe_csi_number_endings[] = {
 { '~', WPE_MOD_NONE },
 { '$', WPE_MOD_SHIFT },
 { '^', WPE_MOD_CTRL },
 { '@', WPE_MOD_CTRL | WPE_MOD_SHIFT },
 { 0,   WPE_MOD_NONE }
};

/**
 * wpe_csi_number_ending - Does this final byte end a numbered key?
 * @final_byte: the sequence's last byte.
 * @mods: out, the modifiers the byte itself says (rxvt), set only on a
 *   match.
 *
 * Lets rxvt's Shift, Ctrl and Ctrl-Shift Home, End, page, Insert and
 * Delete keys reach the same table rows as xterm's.
 * Return: 1 when it ends one, 0 otherwise.
 */
static int wpe_csi_number_ending(int final_byte, unsigned *mods)
{
 const WpeCsiNumberEnding *row;

 for (row = wpe_csi_number_endings; row->final_byte != 0; row++)
  if (row->final_byte == final_byte)
  {
   *mods = row->mods;
   return(1);
  }
 return(0);
}

/** One of rxvt's arrows: the introducer and lower-case letter it sends. */
typedef struct {
 int intro;
 int letter;
 WpeBaseKey base;
 unsigned mods;
} WpeCsiRxvtArrow;

/* rxvt sends a modified arrow as a lower-case final letter with no
   parameter: after '[' it is Shift, after 'O' Ctrl. */
static const WpeCsiRxvtArrow wpe_csi_rxvt_arrows[] = {
 { '[', 'a', WPE_KEY_UP,    WPE_MOD_SHIFT },
 { '[', 'b', WPE_KEY_DOWN,  WPE_MOD_SHIFT },
 { '[', 'c', WPE_KEY_RIGHT, WPE_MOD_SHIFT },
 { '[', 'd', WPE_KEY_LEFT,  WPE_MOD_SHIFT },
 { 'O', 'a', WPE_KEY_UP,    WPE_MOD_CTRL  },
 { 'O', 'b', WPE_KEY_DOWN,  WPE_MOD_CTRL  },
 { 'O', 'c', WPE_KEY_RIGHT, WPE_MOD_CTRL  },
 { 'O', 'd', WPE_KEY_LEFT,  WPE_MOD_CTRL  },
 { 0,   0,   WPE_KEY_NONE,  WPE_MOD_NONE  }
};

/**
 * wpe_csi_rxvt_arrow_key - The arrow an rxvt lower-case letter names.
 * @intro: '[' or 'O'; @letter: the final byte.
 * @base: out; @mods: out, set only on a match.
 *
 * Shift and Ctrl with the arrows mark a block and move by a word under
 * rxvt too, also where its bytes reach the editor unfolded.
 * Return: 1 when the pair is one of rxvt's arrows, 0 otherwise.
 */
static int wpe_csi_rxvt_arrow_key(int intro, int letter, WpeBaseKey *base,
                                  unsigned *mods)
{
 const WpeCsiRxvtArrow *row;

 for (row = wpe_csi_rxvt_arrows; row->base != WPE_KEY_NONE; row++)
  if (row->intro == intro && row->letter == letter)
  {
   *base = row->base;
   *mods = row->mods;
   return(1);
  }
 return(0);
}

/**
 * wpe_csi_numbered_key - A numbered key with its final byte's modifiers.
 * @number: the parameter before the final byte; @param: the xterm
 *   modifier parameter after the ';' (0 when there was none).
 * @ending_mods: what the final byte said (rxvt).
 * @base: out; @mods: out.
 * Return: 1 when the number names a key, 0 otherwise.
 */
static int wpe_csi_numbered_key(int number, int param, unsigned ending_mods,
                                WpeBaseKey *base, unsigned *mods)
{
 if (!wpe_csi_number_key(number, base))
  return(0);
 *mods = ending_mods | wpe_csi_modifier_bits(param);
 return(1);
}

/* The final byte is the whole tail: no parameter came before it. */
#define WPE_CSI_FINAL_ONLY 0

/**
 * wpe_csi_legacy_key - A key in xterm's or rxvt's own escape forms.
 * @intro: '[' or 'O'; @tail: the tail; @n: its length.
 * @base: out; @mods: out.
 *
 * The forms with at most two numbers: "A", "1;5P", "3~", "15;2~", rxvt's
 * "8^" and "c", and "Z" for Shift-Tab.
 * Return: 1 for a key, 0 otherwise.
 */
static int wpe_csi_legacy_key(int intro, const char *tail, size_t n,
                              WpeBaseKey *base, unsigned *mods)
{
 int first, second, last;
 int final_byte;
 unsigned ending_mods;

 if (n == 0)
  return(0);
 last = wpe_csi_parse_numbers(tail, n, &first, &second);
 if (last < 0 || second > WPE_CSI_MOD_PARAM_MAX)
  return(0);
 final_byte = (unsigned char)tail[last];
 if (wpe_csi_number_ending(final_byte, &ending_mods))
  return(wpe_csi_numbered_key(first, second, ending_mods, base, mods));
 if (last == WPE_CSI_FINAL_ONLY &&
     wpe_csi_rxvt_arrow_key(intro, final_byte, base, mods))
  return(1);
 if (!wpe_csi_letter_key(final_byte, base, mods))
  return(0);
 *mods |= wpe_csi_modifier_bits(second);
 return(1);
}

/* ------------------------------------------------------------------------ */
/* Character keys                                                            */
/* ------------------------------------------------------------------------ */

/* The codepoints a protocol reports for the keys that have a byte of their
   own, and the first one that is text. */
#define WPE_CP_BACKSPACE_CTRL   8
#define WPE_CP_TAB              9
#define WPE_CP_ENTER            13
#define WPE_CP_ESCAPE           27
#define WPE_CP_TEXT_FIRST       32
#define WPE_CP_BACKSPACE        127
#define WPE_CP_MAX              0x10ffff
/* Alt with a capital letter, and Alt-Shift with a letter, is this plus the
   letter's index: the window-switch codes (Alt-Shift-S is window 28). */
#define WPE_ALT_SHIFT_FIRST     1024
#define WPE_LETTERS             26

/**
 * wpe_letter_index - Which letter a codepoint is.
 * @cp: a codepoint; @upper: out, 1 when it is a capital.
 * Return: 0 for A, up to 25 for Z, or -1 when it is no ASCII letter.
 */
static int wpe_letter_index(int cp, int *upper)
{
 *upper = 0;
 if (cp >= 'a' && cp <= 'z')
  return(cp - 'a');
 if (cp >= 'A' && cp <= 'Z')
 {
  *upper = 1;
  return(cp - 'A');
 }
 return(-1);
}

int wpe_alt_key_code(int c)
{
 if (c >= 'A' && c <= 'Z')
  return(c + WPE_ALT_SHIFT_FIRST - 'A');
 switch(c)
 {
  case 'a':  return(AltA);
  case 'b':  return(AltB);
  case 'c':  return(AltC);
  case 'd':  return(AltD);
  case 'e':  return(AltE);
  case 'f':  return(AltF);
  case 'g':  return(AltG);
  case 'h':  return(AltH);
  case 'i':  return(AltI);
  case 'j':  return(AltJ);
  case 'k':  return(AltK);
  case 'l':  return(AltL);
  case 'm':  return(AltM);
  case 'n':  return(AltN);
  case 'o':  return(AltO);
  case 'p':  return(AltP);
  case 'q':  return(AltQ);
  case 'r':  return(AltR);
  case 's':  return(AltS);
  case 't':  return(AltT);
  case 'u':  return(AltU);
  case 'v':  return(AltV);
  case 'w':  return(AltW);
  case 'x':  return(AltX);
  case 'y':  return(AltY);
  case 'z':  return(AltZ);
  case '1':  return(Alt1);
  case '2':  return(Alt2);
  case '3':  return(Alt3);
  case '4':  return(Alt4);
  case '5':  return(Alt5);
  case '6':  return(Alt6);
  case '7':  return(Alt7);
  case '8':  return(Alt8);
  case '9':  return(Alt9);
  case '0':  return(Alt0);
  case ' ':  return(AltBl);
  case '#':  return(AltSYS);
  case CtrlA:  return(CBUP);
  case CtrlE:  return(CBDO);
  case CtrlB:  return(CCLE);
  case CtrlF:  return(CCRI);
  case CtrlP:  return(CPS1);
  case CtrlN:  return(CEND);
  case CtrlH:  return(AltBS);
  default:  return(0);
 }
}

/** A control key that has no byte of its own, and its code. */
typedef struct {
 int cp;
 int code;
} WpeCtrlPunct;

static const WpeCtrlPunct wpe_ctrl_punct[] = {
 { '[',  WPE_CTRL_LBRACKET }, { ']', WPE_CTRL_RBRACKET },
 { '/',  WPE_CTRL_SLASH },    { '`', WPE_CTRL_BACKQUOTE },
 { ' ',  WPE_CTRL_SPACE },    { '\\', WPE_CTRL_BACKSLASH },
 { ',',  WPE_CTRL_COMMA },    { '.', WPE_CTRL_PERIOD },
 { '=',  WPE_CTRL_EQUAL },    { '-', WPE_CTRL_MINUS },
 { 0,    0 }
};

/**
 * wpe_ctrl_punct_code - The code of Ctrl with a punctuation key.
 * @cp: the key's character.
 * Return: the WPE_CTRL_* code, or 0.
 */
static int wpe_ctrl_punct_code(int cp)
{
 const WpeCtrlPunct *row;

 for (row = wpe_ctrl_punct; row->cp != 0; row++)
  if (row->cp == cp)
   return(row->code);
 return(0);
}

/**
 * wpe_ctrl_letter_code - The code of Ctrl with a letter.
 * @index: 0 for A to 25 for Z.
 *
 * The letter's control byte, except for the four whose byte another key
 * already sends (H Backspace, I Tab, M Enter, and [ Esc): those are
 * distinct codes, so a keyboard that says "Ctrl plus the letter" can be
 * told from the plain key.
 * Return: the code.
 */
static int wpe_ctrl_letter_code(int index)
{
 switch (index)
 {
  case 'h' - 'a': return(WPE_CTRL_H);
  case 'i' - 'a': return(WPE_CTRL_I);
  case 'm' - 'a': return(WPE_CTRL_M);
  default:        return(index + 1);
 }
}

/**
 * wpe_ctrl_alt_code - The code of Ctrl and Alt with a letter.
 * @index: 0 for A to 25 for Z.
 *
 * ESC followed by a control byte keeps the codes it always had (the page and
 * word jumps); every other letter is a WPE_CA_* code, bound to nothing.
 * Return: the code.
 */
static int wpe_ctrl_alt_code(int index)
{
 int legacy = wpe_alt_key_code(index + 1);

 if (legacy != 0)
  return(legacy);
 return(WPE_CA_FIRST + index);
}

/**
 * wpe_key_code_ctrl - Ctrl (with or without Shift) and one character.
 * @cp: the character; @mods: the modifiers, Ctrl among them.
 * Return: the code, or 0.
 */
static int wpe_key_code_ctrl(int cp, unsigned mods)
{
 int upper;
 int index = wpe_letter_index(cp, &upper);

 if (index >= 0)
 {
  if (mods & WPE_MOD_ALT)
   return(wpe_ctrl_alt_code(index));
  if ((mods & WPE_MOD_SHIFT) || upper)
   return(WPE_CS_FIRST + index);
  return(wpe_ctrl_letter_code(index));
 }
 if (mods & (WPE_MOD_SHIFT | WPE_MOD_ALT))
  return(0);
 return(wpe_ctrl_punct_code(cp));
}

/**
 * wpe_key_code_alt - Alt (with or without Shift) and one character.
 * @cp: the character; @mods: the modifiers, Alt among them, Ctrl not.
 * Return: the code, or 0.
 */
static int wpe_key_code_alt(int cp, unsigned mods)
{
 int upper;
 int index = wpe_letter_index(cp, &upper);

 if (index >= 0)
 {
  if ((mods & WPE_MOD_SHIFT) || upper)
   return(wpe_alt_key_code('A' + index));
  return(wpe_alt_key_code('a' + index));
 }
 if (mods & WPE_MOD_SHIFT)
  return(0);
 return(wpe_alt_key_code(cp));
}

/**
 * wpe_key_code_tab - Tab with the modifiers held.
 * @mods: the modifiers.
 * Return: Tab, Shift-Tab or Ctrl-Tab; 0 for any other chord.
 */
static int wpe_key_code_tab(unsigned mods)
{
 if (mods == WPE_MOD_NONE)
  return(WPE_TAB);
 if (mods == WPE_MOD_SHIFT)
  return(WPE_BTAB);
 if (mods == WPE_MOD_CTRL)
  return(WPE_CTRL_TAB);
 return(0);
}

/**
 * wpe_key_code_backspace - Backspace with the modifiers held.
 * @mods: the modifiers.
 *
 * A legacy terminal sends Backspace's own byte for Shift-Backspace and
 * Ctrl-Backspace, so in a terminal that reports them as chords they still
 * erase, as they always did; the Alt forms have codes of their own.
 * Return: the code.
 */
static int wpe_key_code_backspace(unsigned mods)
{
 int code = wpe_key_code(WPE_KEY_BACKSPACE, mods);

 if (code == 0 && !(mods & WPE_MOD_ALT))
  return(wpe_key_code(WPE_KEY_BACKSPACE, WPE_MOD_NONE));
 return(code);
}

int wpe_key_code_char(int cp, unsigned mods)
{
 if (cp == WPE_CP_ESCAPE)
  return(WPE_ESC);
 if (cp == WPE_CP_ENTER)
  return((mods & WPE_MOD_ALT) ? 0 : WPE_CR);
 if (cp == WPE_CP_TAB)
  return(wpe_key_code_tab(mods));
 if (cp == WPE_CP_BACKSPACE || cp == WPE_CP_BACKSPACE_CTRL)
  return(wpe_key_code_backspace(mods));
 if (cp < WPE_CP_TEXT_FIRST || cp > WPE_CP_MAX)
  return(0);
 if (mods & WPE_MOD_CTRL)
  return(wpe_key_code_ctrl(cp, mods));
 if (mods & WPE_MOD_ALT)
  return(wpe_key_code_alt(cp, mods));
 return(cp);
}

int wpe_key_fold_legacy(int code)
{
 switch (code)
 {
  case WPE_CTRL_H:        return(CtrlH);
  case WPE_CTRL_I:        return(CtrlI);
  case WPE_CTRL_M:        return(CtrlM);
  case WPE_CTRL_LBRACKET: return(WPE_ESC);
  case WPE_CTRL_TAB:      return(WPE_TAB);   /* a legacy terminal sends Tab */
  default:                break;
 }
 if (code >= WPE_CS_FIRST && code <= WPE_CS_LAST)
  return(code - WPE_CS_FIRST + CtrlA);   /* Ctrl-Shift-letter: Ctrl-letter */
 return(code);
}

/* ------------------------------------------------------------------------ */
/* modifyOtherKeys and the kitty keyboard protocol                           */
/* ------------------------------------------------------------------------ */

/* Both protocols report a key as numbers: "27;5;112~" (xterm's
   modifyOtherKeys: 27, the modifier, the character) or "112:80;6u" (kitty:
   the character with optional shifted and base-layout forms after ':', the
   modifier with an optional event type, optional text). */
/* A codepoint is at most 0x10ffff; a longer run of digits stays above it. */
#define WPE_REPORT_NUMBER_MAX 9999999
#define WPE_CSI_FIELDS_MAX 3
#define WPE_CSI_SUBS_MAX   3
/* Which sub-field is which. */
#define WPE_CSI_SUB_MAIN    0
#define WPE_CSI_SUB_SHIFTED 1
#define WPE_CSI_SUB_BASE_LAYOUT 2
#define WPE_CSI_SUB_EVENT   1
/* Which field is which. */
#define WPE_CSI_FIELD_KEY   0
#define WPE_CSI_FIELD_MODS  1
#define WPE_CSI_FIELD_TEXT  2
/* modifyOtherKeys' first number, and its field count. */
#define WPE_MOK_NUMBER 27
#define WPE_MOK_FIELDS 3
/* The kitty event types: 1 press, 2 repeat, 3 release. */
#define WPE_KITTY_EVENT_RELEASE 3
/* The modifier parameter is 1 plus a bit set; Super, Hyper and Meta have no
   place in the editor, so a chord that holds one is not read as the same key
   without it. */
#define WPE_KITTY_MOD_UNUSED 0x38
#define WPE_KITTY_MOD_PARAM_MAX 256
/* The Private Use Area numbers kitty gives the keys that have no character. */
#define WPE_KITTY_PUA_FIRST        57344
#define WPE_KITTY_KP_DIGIT_FIRST   57399
#define WPE_KITTY_KP_DIGIT_LAST    57408
#define WPE_KITTY_KP_DECIMAL       57409
#define WPE_KITTY_KP_ENTER         57414
#define WPE_KITTY_KP_SEPARATOR     57416
#define WPE_KITTY_KP_CURSOR_FIRST  57417
#define WPE_KITTY_KP_CURSOR_LAST   57426

/** The numbers of one escape sequence: fields split by ';', each in parts
    split by ':'. */
typedef struct {
 int nfields;
 int nsubs;
 int v[WPE_CSI_FIELDS_MAX][WPE_CSI_SUBS_MAX];
} WpeCsiFields;

/**
 * wpe_csi_parse_fields - Read the numbers of a tail up to its final byte.
 * @tail: the tail; @n: its length.
 * @f: out, the numbers (0 where a field or part is empty).
 * Return: the index of the final byte, or -1 when anything but digits, ';'
 * and ':' comes before it, or there are more fields or parts than a key
 * report has.
 */
static int wpe_csi_parse_fields(const char *tail, size_t n, WpeCsiFields *f)
{
 size_t i;
 int field = 0, sub = 0;

 memset(f, 0, sizeof *f);
 f->nfields = 1;
 for (i = 0; i + 1 < n; i++)
 {
  if (tail[i] >= '0' && tail[i] <= '9')
   f->v[field][sub] = wpe_csi_add_digit_max(f->v[field][sub], tail[i] - '0',
                                            WPE_REPORT_NUMBER_MAX);
  else if (tail[i] == ':')
  {
   if (++sub >= WPE_CSI_SUBS_MAX)
    return(-1);
   if (sub + 1 > f->nsubs)
    f->nsubs = sub + 1;
  }
  else if (tail[i] == ';')
  {
   if (++field >= WPE_CSI_FIELDS_MAX)
    return(-1);
   sub = 0;
   f->nfields = field + 1;
  }
  else
   return(-1);
 }
 return((int)i);
}

/**
 * wpe_report_modifiers - The modifiers of a protocol report's parameter.
 * @param: the number (1 plus a bit set; 0 or 1 is none).
 * @mods: out.
 * Return: 1 for modifiers the editor reads; 0 for a chord with Super, Hyper
 * or Meta held, or a parameter no terminal sends.  Caps and Num lock are
 * not modifiers of the chord.
 */
static int wpe_report_modifiers(int param, unsigned *mods)
{
 if (param > WPE_KITTY_MOD_PARAM_MAX)
  return(0);
 if (param >= 2 && ((param - 1) & WPE_KITTY_MOD_UNUSED))
  return(0);
 *mods = wpe_csi_modifier_bits(param);
 return(1);
}

/**
 * wpe_kitty_pua_text - The character a keypad key of the kitty protocol
 * types.
 * @cp: a Private Use Area number.
 * Return: the character (13 for the keypad's Enter), or 0 for a key that
 * types none.
 */
static int wpe_kitty_pua_text(int cp)
{
 if (cp >= WPE_KITTY_KP_DIGIT_FIRST && cp <= WPE_KITTY_KP_DIGIT_LAST)
  return('0' + cp - WPE_KITTY_KP_DIGIT_FIRST);
 switch (cp)
 {
  case WPE_KITTY_KP_DECIMAL:   return('.');
  case WPE_KITTY_KP_DECIMAL + 1: return('/');
  case WPE_KITTY_KP_DECIMAL + 2: return('*');
  case WPE_KITTY_KP_DECIMAL + 3: return('-');
  case WPE_KITTY_KP_DECIMAL + 4: return('+');
  case WPE_KITTY_KP_ENTER:     return(WPE_CP_ENTER);
  case WPE_KITTY_KP_ENTER + 1: return('=');
  case WPE_KITTY_KP_SEPARATOR: return(',');
  default:                     return(0);
 }
}

/** The keypad's cursor keys of the kitty protocol, in their numbering. */
static const WpeBaseKey wpe_kitty_kp_cursor[] = {
 WPE_KEY_LEFT, WPE_KEY_RIGHT, WPE_KEY_UP, WPE_KEY_DOWN, WPE_KEY_PGUP,
 WPE_KEY_PGDN, WPE_KEY_HOME, WPE_KEY_END, WPE_KEY_INSERT, WPE_KEY_DELETE
};

/**
 * wpe_pua_key - A kitty key that has no character, as a key.
 * @cp: its Private Use Area number.
 * @base: out; @codepoint: out, set when the key types a character.
 *
 * The keypad gives the keys and characters the keypad of a legacy terminal
 * gives; F13 and up, the lock keys, Print Screen, Pause, Menu, the media
 * keys and the modifier keys are no keys of the editor.
 * Return: 1 for a key, 0 for one the editor has no use for.
 */
static int wpe_pua_key(int cp, WpeBaseKey *base, int *codepoint)
{
 int text = wpe_kitty_pua_text(cp);

 if (text != 0)
 {
  *base = WPE_KEY_CHAR;
  *codepoint = text;
  return(1);
 }
 if (cp >= WPE_KITTY_KP_CURSOR_FIRST && cp <= WPE_KITTY_KP_CURSOR_LAST)
 {
  *base = wpe_kitty_kp_cursor[cp - WPE_KITTY_KP_CURSOR_FIRST];
  return(1);
 }
 return(0);
}

/**
 * wpe_report_text - The character a report types, as shifted by its mods.
 * @cp: the key's unshifted character; @shifted: the shifted form the
 *   protocol gave, or 0; @text: the text it gave, or 0.
 * @mods: in and out: Shift is dropped when it only chose the character.
 *
 * With Ctrl or Alt held the key stands for itself (Alt-Shift-S is the
 * window switch, with the capital's index); without them Shift made the
 * character, from the report where it says so, from the letter otherwise.
 * Return: the character.
 */
static int wpe_report_text(int cp, int shifted, int text, unsigned *mods)
{
 int upper;

 if (*mods & WPE_MOD_CTRL)
  return(cp);
 if (*mods & WPE_MOD_ALT)
 {
  if ((*mods & WPE_MOD_SHIFT) && shifted != 0 &&
      wpe_letter_index(cp, &upper) < 0)
  {
   *mods &= ~WPE_MOD_SHIFT;
   return(shifted);
  }
  return(cp);
 }
 if (text >= WPE_CP_TEXT_FIRST)
 {
  *mods &= ~WPE_MOD_SHIFT;
  return(text);
 }
 if ((*mods & WPE_MOD_SHIFT) && shifted != 0)
 {
  *mods &= ~WPE_MOD_SHIFT;
  return(shifted);
 }
 if ((*mods & WPE_MOD_SHIFT) && cp >= 'a' && cp <= 'z')
 {
  *mods &= ~WPE_MOD_SHIFT;
  return(cp - 'a' + 'A');
 }
 *mods &= ~WPE_MOD_SHIFT;
 return(cp);
}

/**
 * wpe_report_key - One protocol report as a key.
 * @cp: the key's number; @shifted: its shifted form or 0; @base_layout: the
 *   same key on a standard US layout, or 0; @text: its text or 0.
 * @base: out; @mods: in and out; @codepoint: out.
 *
 * A chord with Ctrl or Alt on a key whose own character is not ASCII (a
 * Cyrillic or Greek letter) is read by the key's place on the base layout
 * when the terminal says it, so Ctrl-S stays Save there.  A key that types
 * an ASCII character keeps it, as the X11 and Wayland decoders do, so
 * Ctrl-Z on a German or a Dvorak keyboard is the Ctrl-Z its user typed.
 * Return: 1 for a key, 0 for a number that is none.
 */
static int wpe_report_key(int cp, int shifted, int base_layout, int text,
                          WpeBaseKey *base, unsigned *mods, int *codepoint)
{
 if ((*mods & (WPE_MOD_CTRL | WPE_MOD_ALT)) && cp > WPE_CP_BACKSPACE &&
     cp < WPE_KITTY_PUA_FIRST &&
     base_layout >= WPE_CP_TEXT_FIRST && base_layout < WPE_CP_BACKSPACE)
  cp = base_layout;            /* Ctrl-S stays Save on a Cyrillic layout */
 if (cp >= WPE_KITTY_PUA_FIRST)
 {
  if (!wpe_pua_key(cp, base, codepoint))
   return(0);
  if (*base != WPE_KEY_CHAR)
   return(1);
  cp = *codepoint;
 }
 else if (cp < WPE_CP_TEXT_FIRST &&
          cp != WPE_CP_ESCAPE && cp != WPE_CP_ENTER && cp != WPE_CP_TAB &&
          cp != WPE_CP_BACKSPACE_CTRL)
  return(0);
 else if (cp > WPE_CP_MAX)
  return(0);
 *base = WPE_KEY_CHAR;
 if (cp >= WPE_CP_TEXT_FIRST && cp != WPE_CP_BACKSPACE)
  cp = wpe_report_text(cp, shifted, text, mods);
 *codepoint = cp;
 return(1);
}

/**
 * wpe_kitty_report_key - The key of "c[:shifted[:base]];m[:event][;text]u".
 * @f: the parsed numbers.
 * @base: out; @mods: out; @codepoint: out.
 * Return: 1 for a key, 0 for a release or anything else that is none.
 */
static int wpe_kitty_report_key(const WpeCsiFields *f, WpeBaseKey *base,
                                unsigned *mods, int *codepoint)
{
 if (f->v[WPE_CSI_FIELD_MODS][WPE_CSI_SUB_EVENT] >= WPE_KITTY_EVENT_RELEASE)
  return(0);
 if (!wpe_report_modifiers(f->v[WPE_CSI_FIELD_MODS][WPE_CSI_SUB_MAIN], mods))
  return(0);
 return(wpe_report_key(f->v[WPE_CSI_FIELD_KEY][WPE_CSI_SUB_MAIN],
                       f->v[WPE_CSI_FIELD_KEY][WPE_CSI_SUB_SHIFTED],
                       f->v[WPE_CSI_FIELD_KEY][WPE_CSI_SUB_BASE_LAYOUT],
                       f->v[WPE_CSI_FIELD_TEXT][WPE_CSI_SUB_MAIN],
                       base, mods, codepoint));
}

/**
 * wpe_mok_report_key - The key of modifyOtherKeys' "27;m;c~".
 * @f: the parsed numbers.
 * @base: out; @mods: out; @codepoint: out.
 * Return: 1 for a key, 0 otherwise.
 */
static int wpe_mok_report_key(const WpeCsiFields *f, WpeBaseKey *base,
                              unsigned *mods, int *codepoint)
{
 if (!wpe_report_modifiers(f->v[WPE_CSI_FIELD_MODS][WPE_CSI_SUB_MAIN], mods))
  return(0);
 return(wpe_report_key(f->v[WPE_CSI_FIELD_TEXT][WPE_CSI_SUB_MAIN], 0, 0, 0,
                       base, mods, codepoint));
}

int wpe_key_from_csi_cp(int intro, const char *tail, size_t n, WpeBaseKey *base,
                        unsigned *mods, int *codepoint)
{
 WpeCsiFields f;
 int last, final_byte;

 *codepoint = 0;
 if (n == 0)
  return(0);
 last = wpe_csi_parse_fields(tail, n, &f);
 if (last < 0)
  return(0);
 final_byte = (unsigned char)tail[last];
 if (final_byte == 'u')
  return(intro == '[' && wpe_kitty_report_key(&f, base, mods, codepoint));
 if (final_byte == '~' && f.nfields == WPE_MOK_FIELDS &&
     f.v[WPE_CSI_FIELD_KEY][WPE_CSI_SUB_MAIN] == WPE_MOK_NUMBER)
  return(wpe_mok_report_key(&f, base, mods, codepoint));
 if (f.nfields > 2 || f.nsubs > 0)
  return(0);
 return(wpe_csi_legacy_key(intro, tail, n, base, mods));
}

int wpe_key_from_csi(int intro, const char *tail, size_t n, WpeBaseKey *base,
                     unsigned *mods)
{
 int codepoint;

 return(wpe_key_from_csi_cp(intro, tail, n, base, mods, &codepoint));
}

int wpe_key_decode_csi(int intro, const char *tail, size_t n, int *is_text)
{
 WpeBaseKey base;
 unsigned mods;
 int codepoint, code;

 *is_text = 0;
 if (!wpe_key_from_csi_cp(intro, tail, n, &base, &mods, &codepoint))
  return(0);
 if (base != WPE_KEY_CHAR)
  return(wpe_key_fold_legacy(wpe_key_code(base, mods)));
 code = wpe_key_code_char(codepoint, mods);
 if (code != 0 && code == codepoint && codepoint >= WPE_CP_TEXT_FIRST &&
     !(mods & (WPE_MOD_CTRL | WPE_MOD_ALT)))
  *is_text = 1;
 return(wpe_key_fold_legacy(code));
}

/* ESC and the introducer come before a sequence's tail. */
#define WPE_SEQ_ESC  0x1b
#define WPE_SEQ_HEAD 2

int wpe_key_from_sequence(const char *seq, size_t n, WpeBaseKey *base,
                          unsigned *mods)
{
 if (seq == NULL || n <= WPE_SEQ_HEAD)
  return(0);
 if (seq[0] != WPE_SEQ_ESC)
  return(0);
 if (seq[1] != '[' && seq[1] != 'O')
  return(0);
 return(wpe_key_from_csi(seq[1], seq + WPE_SEQ_HEAD, n - WPE_SEQ_HEAD, base,
                         mods));
}

/* ------------------------------------------------------------------------ */
/* terminfo's names for modified keys                                        */
/* ------------------------------------------------------------------------ */

/* ncurses gives a key that only an extended terminfo capability names
   (xterm's kRIT5 for Ctrl-Right, kNXT5 for Ctrl-PgDn) a number it picks
   when the terminal is opened, so the number says nothing; the name does.
   The name is a stem for the key and a digit that is xterm's modifier
   parameter (user_caps(5): 2 Shift, 3 Alt, 5 Ctrl, 6 Ctrl+Shift, ...). */

/** A terminfo stem and the key it names. */
typedef struct {
 const char *stem;
 WpeBaseKey base;
} WpeTerminfoStem;

static const WpeTerminfoStem wpe_terminfo_stems[] = {
 { "kUP",  WPE_KEY_UP },     { "kDN",  WPE_KEY_DOWN },
 { "kLFT", WPE_KEY_LEFT },   { "kRIT", WPE_KEY_RIGHT },
 { "kHOM", WPE_KEY_HOME },   { "kEND", WPE_KEY_END },
 { "kPRV", WPE_KEY_PGUP },   { "kNXT", WPE_KEY_PGDN },
 { "kIC",  WPE_KEY_INSERT }, { "kDC",  WPE_KEY_DELETE },
 { NULL,   WPE_KEY_NONE }
};

/* A stem with no digit after it is the shifted key (kUP is Shift-Up). */
#define WPE_TERMINFO_SHIFT_PARAM 2
/* The longest stem in the table above. */
#define WPE_TERMINFO_STEM_MAX 4

/**
 * wpe_terminfo_stem_key - The key a terminfo name's stem names.
 * @name: the whole name ("kRIT5"); @len: how many leading bytes are the
 *   stem (everything before the digits).
 * Return: the key, or WPE_KEY_NONE when the stem is not one of them.
 */
static WpeBaseKey wpe_terminfo_stem_key(const char *name, size_t len)
{
 const WpeTerminfoStem *row;
 size_t i;

 for (row = wpe_terminfo_stems; row->stem != NULL; row++)
 {
  for (i = 0; i < len && row->stem[i] == name[i]; i++)
   ;
  if (i == len && row->stem[len] == '\0')
   return(row->base);
 }
 return(WPE_KEY_NONE);
}

/**
 * wpe_terminfo_digits_param - The modifier parameter a name ends with.
 * @digits: the bytes after the stem.
 * Return: the number, WPE_TERMINFO_SHIFT_PARAM when there are none, or -1
 * when anything but digits follows the stem.
 */
static int wpe_terminfo_digits_param(const char *digits)
{
 int value = 0;

 if (*digits == '\0')
  return(WPE_TERMINFO_SHIFT_PARAM);
 for (; *digits != '\0'; digits++)
 {
  if (*digits < '0' || *digits > '9')
   return(-1);
  value = wpe_csi_add_digit(value, *digits - '0');
 }
 return(value);
}

int wpe_key_from_terminfo_name(const char *name, unsigned console_mods,
                               WpeBaseKey *base, unsigned *mods)
{
 size_t stem_len = 0;
 int param;
 WpeBaseKey key;

 if (name == NULL)
  return(0);
 while (name[stem_len] != '\0' && !(name[stem_len] >= '0' && name[stem_len] <= '9'))
  stem_len++;
 if (stem_len == 0 || stem_len > WPE_TERMINFO_STEM_MAX)
  return(0);
 key = wpe_terminfo_stem_key(name, stem_len);
 if (key == WPE_KEY_NONE)
  return(0);
 param = wpe_terminfo_digits_param(name + stem_len);
 if (param < WPE_TERMINFO_SHIFT_PARAM || param > WPE_CSI_MOD_PARAM_MAX)
  return(0);
 *base = key;
 *mods = wpe_csi_modifier_bits(param) | console_mods;
 return(1);
}
