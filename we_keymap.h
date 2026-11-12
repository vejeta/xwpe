#ifndef WE_KEYMAP_H
#define WE_KEYMAP_H

/* we_keymap.h -- what a key means, once.

   The terminal, X11 and Wayland each learn a key in their own words: a curses
   KEY_ code, an X keysym, a raw escape sequence.  The editor acts on one
   number from keys.h.  This module is the only place that turns the first
   into the second, from a (base key, modifiers) pair, so the same key gives
   the same answer on every display and a new binding is one table row.

   No editor header and no display library is needed to use it: the unit test
   links this file alone. */

#include <stddef.h>

/** The key itself, without the modifiers that were held. */
typedef enum {
 WPE_KEY_NONE = 0,
 WPE_KEY_F1, WPE_KEY_F2, WPE_KEY_F3, WPE_KEY_F4, WPE_KEY_F5, WPE_KEY_F6,
 WPE_KEY_F7, WPE_KEY_F8, WPE_KEY_F9, WPE_KEY_F10, WPE_KEY_F11, WPE_KEY_F12,
 WPE_KEY_UP, WPE_KEY_DOWN, WPE_KEY_LEFT, WPE_KEY_RIGHT,
 WPE_KEY_HOME, WPE_KEY_END, WPE_KEY_PGUP, WPE_KEY_PGDN,
 WPE_KEY_INSERT, WPE_KEY_DELETE, WPE_KEY_BACKSPACE, WPE_KEY_TAB,
 WPE_KEY_HELP, WPE_KEY_BREAK,
 /* The Sun keys L3 (Props) to L10 (Cut).  L1 (Stop) and L2 (Again) send
    the same symbols as a PC keyboard's F11 and F12 and are read as those. */
 WPE_KEY_L3, WPE_KEY_L4, WPE_KEY_L5,
 WPE_KEY_L6, WPE_KEY_L7, WPE_KEY_L8, WPE_KEY_L9, WPE_KEY_L10,
 WPE_KEY_BASE_MAX
} WpeBaseKey;

/** The modifiers held with a key, as a bit set. */
#define WPE_MOD_NONE   0u
#define WPE_MOD_SHIFT  1u
#define WPE_MOD_CTRL   2u
#define WPE_MOD_ALT    4u

/** keys.h's convention for a shifted key: the plain code plus this (F1 is
    314, Shift-F1 is 826). */
#define WPE_KEY_SHIFT_OFFSET 512

/** One binding: the editor code for a base key with exactly these modifiers. */
typedef struct {
 WpeBaseKey base;
 unsigned mods;
 int code;
} WpeKeyRow;

/** Every (base, modifiers) pair bound by a row, ended by a row whose base is
    WPE_KEY_NONE.  A pair appears once.  Shift on top of a bound pair needs no
    row: wpe_key_code() adds WPE_KEY_SHIFT_OFFSET. */
extern const WpeKeyRow wpe_key_table[];

/**
 * wpe_key_code - The editor's code for a key and the modifiers held.
 * @base: the key.
 * @mods: WPE_MOD_* bits held with it.
 *
 * Use this at the end of every display's key decoder.  A key with no binding
 * gives 0, never a letter, so an unknown chord does nothing instead of
 * typing into the file.
 * Return: the keys.h code, or 0 when nothing is bound.
 */
int wpe_key_code(WpeBaseKey base, unsigned mods);

/**
 * wpe_key_from_curses - A curses key code as a key and its modifiers.
 * @ch: the value getch() returned (above KEY_CODE_YES).
 * @console_mods: the shift and control the Linux console reports through
 *   an ioctl (WPE_MOD_*), 0 on any other terminal.
 * @base: out, the key.
 * @mods: out, the modifiers: the ones terminfo's layout puts in the key
 *   code (KEY_F(13..) is Shift-F1.., KEY_F(25..) Ctrl-F1.., KEY_F(49..)
 *   Alt-F1..) joined with @console_mods.
 *
 * Return: 1 when @ch is a key the table knows, 0 otherwise (mouse and
 * resize are not keys and stay with the caller).
 */
int wpe_key_from_curses(int ch, unsigned console_mods, WpeBaseKey *base,
                        unsigned *mods);

/**
 * wpe_key_from_keysym - An X or Wayland key symbol as a key.
 * @keysym: XK_* from Xlib or XKB_KEY_* from xkbcommon: they are the same
 *   numbers, so one function serves both displays.
 * @mods: in and out, the modifiers the display reported.  Set by the key
 *   where the symbol itself says it (ISO_Left_Tab is Shift-Tab whatever
 *   else is held, Break is a key of its own, Pause only means Break with
 *   Control held).
 * @base: out, the key.
 *
 * Return: 1 when the symbol is a key the table knows, 0 otherwise (a letter,
 * a dead key, a modifier).
 */
int wpe_key_from_keysym(unsigned long keysym, unsigned *mods,
                        WpeBaseKey *base);

/**
 * wpe_key_from_csi - The tail of an escape sequence as a key.
 * @intro: the introducer after ESC, '[' or 'O'.  Only rxvt's lower-case
 *   arrows depend on it: "ESC [ a" is Shift-Up, "ESC O a" Ctrl-Up.
 * @tail: the bytes after ESC and the introducer, up to and including the
 *   final byte: "A", "1;5P", "3~", "15;2~", "Z", rxvt's "8^" and "c".
 * @n: how many bytes.
 * @base: out, the key.
 * @mods: out, the modifiers of the xterm parameter (m-1: shift 1, alt 2,
 *   ctrl 4) or of rxvt's final byte ('$' Shift, '^' Ctrl, '@' both), so a
 *   Ctrl-F9 that arrives raw is read like one curses folded.
 *
 * Return: 1 when the sequence is a key the table knows, 0 otherwise.
 */
int wpe_key_from_csi(int intro, const char *tail, size_t n, WpeBaseKey *base,
                     unsigned *mods);

/**
 * wpe_key_from_sequence - A whole escape sequence as a key.
 * @seq: the bytes a terminal sends for the key, ESC included: what
 *   ncurses' keybound() says a key code stands for ("\033[1;5F").
 * @n: how many bytes.
 * @base: out, the key; @mods: out, its modifiers, as wpe_key_from_csi().
 *
 * Some terminfo entries give a modified key's bytes to a standard
 * capability that names another key (st's Ctrl-End is ked, KEY_EOS;
 * rxvt's is kel, KEY_EOL), so the code curses delivers cannot be trusted
 * for them, but the bytes behind it can.  Without this those keys did
 * nothing in the terminal.
 * Return: 1 when the bytes are a key the table knows, 0 otherwise.
 */
int wpe_key_from_sequence(const char *seq, size_t n, WpeBaseKey *base,
                          unsigned *mods);

/**
 * wpe_key_from_terminfo_name - A key curses knows only by its terminfo name.
 * @name: what curses' keyname() says for a code from KEY_MAX up: "kRIT5"
 *   (Ctrl-Right), "kNXT5" (Ctrl-PgDn), "kHOM6" (Ctrl-Shift-Home).
 * @console_mods: the shift and control the Linux console reports, as for
 *   wpe_key_from_curses().
 * @base: out, the key.
 * @mods: out, the modifiers the name's digit stands for (xterm's
 *   parameter: 2 Shift, 3 Alt, 5 Ctrl, ...), joined with @console_mods.
 *
 * Under TERM=xterm-256color or tmux-256color, Ctrl-Right and Ctrl-PgDn
 * arrive this way; without it they did nothing in the terminal.
 * Return: 1 when the name is a cursor or editing key, 0 otherwise.
 */
int wpe_key_from_terminfo_name(const char *name, unsigned console_mods,
                               WpeBaseKey *base, unsigned *mods);

#endif /* WE_KEYMAP_H */
