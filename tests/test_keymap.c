/* test_keymap.c -- unit test for the key table (we_keymap.c).
 *
 * Links we_keymap.c only: no editor, no display library, always runs.  The
 * three displays learn a key in different words (a curses KEY_ code, an X
 * keysym, an escape sequence); each must end at the same editor code, and
 * what is not bound must give 0, never a letter.
 *
 * Headers of the real libraries are used for the numbers when the machine
 * has them (the table spells the X keysyms itself so that it needs no
 * library); the test is what proves those numbers right.
 */
#include <stdio.h>
#include <string.h>

#include "../we_keymap.h"
#include "../we_bindings.h"
#include "../keys.h"
#include "../unixkeys.h"

/* The fold the editor does once where it takes a key (the key table's). */
#define wpe_key_fold_legacy(code) wpe_binding_fold((code), WPE_STYLE_BORLAND, WPE_CTX_EDIT)

#ifdef NCURSES
#include <curses.h>
#endif

#if defined(__has_include)
# if __has_include(<X11/keysym.h>)
#  include <X11/keysym.h>
#  define HAVE_XK 1
# endif
# if __has_include(<xkbcommon/xkbcommon-keysyms.h>)
#  include <xkbcommon/xkbcommon-keysyms.h>
#  define HAVE_XKB 1
# endif
#endif

#define NFKEYS 10
#define CSI_MAX 16

static int fails;

static void chk(const char *what, long got, long want)
{
	if (got != want) {
		printf("FAIL: %s: got %ld want %ld\n", what, got, want);
		fails++;
	}
}

static const int plain_f[NFKEYS] = { F1, F2, F3, F4, F5, F6, F7, F8, F9, F10 };
static const int shift_f[NFKEYS] = { SF1, SF2, SF3, SF4, SF5, SF6, SF7, SF8, SF9, SF10 };
static const int ctrl_f[NFKEYS]  = { CF1, CF2, CF3, CF4, CF5, CF6, CF7, CF8, CF9, CF10 };
static const int alt_f[NFKEYS]   = { AF1, AF2, AF3, AF4, AF5, AF6, AF7, AF8, AF9, AF10 };

/* One curses key code through the whole decode: the code the editor gets. */
static int via_curses(int ch, unsigned console_mods)
{
#ifdef NCURSES
	WpeBaseKey base;
	unsigned mods;

	if (!wpe_key_from_curses(ch, console_mods, &base, &mods))
		return 0;
	return wpe_key_code(base, mods);
#else
	(void)ch; (void)console_mods;
	return -1;
#endif
}

/* An escape sequence tail after "ESC [" through the whole decode. */
static int via_csi(const char *tail)
{
	WpeBaseKey base;
	unsigned mods;

	if (!wpe_key_from_csi('[', tail, strlen(tail), &base, &mods))
		return 0;
	return wpe_key_code(base, mods);
}

/* A whole sequence, ESC included, as keybound() gives it, through the
   whole decode. */
static int via_seq(const char *seq)
{
	WpeBaseKey base;
	unsigned mods;

	if (!wpe_key_from_sequence(seq, strlen(seq), &base, &mods))
		return 0;
	return wpe_key_code(base, mods);
}

/* A keysym and the modifiers the display reported, through the whole decode. */
static int via_keysym(unsigned long keysym, unsigned mods)
{
	WpeBaseKey base;

	if (!wpe_key_from_keysym(keysym, &mods, &base))
		return 0;
	return wpe_key_code(base, mods);
}

static void test_every_bound_pair_has_exactly_one_row(void)
{
	const WpeKeyRow *a, *b;
	int rows = 0;

	for (a = wpe_key_table; a->base != WPE_KEY_NONE; a++) {
		rows++;
		chk("a row names a code", a->code != 0, 1);
		chk("a row's base is a key", a->base < WPE_KEY_BASE_MAX, 1);
		for (b = a + 1; b->base != WPE_KEY_NONE; b++)
			if (a->base == b->base && a->mods == b->mods) {
				printf("FAIL: pair (%d, %u) has two rows\n", a->base, a->mods);
				fails++;
			}
	}
	chk("the table is not empty", rows > 0, 1);
}

static void test_curses_f_key_blocks_give_plain_shift_ctrl_alt(void)
{
#ifdef NCURSES
	int n;

	for (n = 1; n <= NFKEYS; n++) {
		chk("KEY_F(n) is Fn", via_curses(KEY_F(n), 0), plain_f[n - 1]);
		chk("KEY_F(12+n) is Shift-Fn", via_curses(KEY_F(12 + n), 0), shift_f[n - 1]);
		chk("KEY_F(24+n) is Ctrl-Fn", via_curses(KEY_F(24 + n), 0), ctrl_f[n - 1]);
		chk("KEY_F(36+n) is Ctrl-Shift-Fn", via_curses(KEY_F(36 + n), 0), ctrl_f[n - 1] + WPE_KEY_SHIFT_OFFSET);
		chk("KEY_F(48+n) is Alt-Fn", via_curses(KEY_F(48 + n), 0), alt_f[n - 1]);
	}
	/* terminfo of xterm-256color: kf26 is \E[1;5Q, Ctrl-F2 (it used to be Shift-F10) */
	chk("KEY_F(26) is Ctrl-F2", via_curses(KEY_F(26), 0), CF2);
	chk("KEY_F(17) is Shift-F5, not Shift-F1", via_curses(KEY_F(17), 0), SF5);
	chk("KEY_F(64) is KEY_DL, not a function key", via_curses(KEY_F(64), 0), 0);
	chk("KEY_F(1000) is nothing", via_curses(KEY_F(1000), 0), 0);
#endif
}

#if defined(HAVE_XK) || defined(HAVE_XKB)
/* The table's own copy of a keysym number must equal the library's. */
static void same_keysym(const char *name, unsigned long lib, WpeBaseKey base)
{
	WpeBaseKey got;
	unsigned mods = 0;

	if (!wpe_key_from_keysym(lib, &mods, &got) || got != base) {
		printf("FAIL: keysym %s (0x%lx) is not the key the table says\n", name, lib);
		fails++;
	}
}
#endif

static void test_keysym_numbers_serve_x11_and_wayland_alike(void)
{
#ifdef HAVE_XK
	same_keysym("XK_F1", XK_F1, WPE_KEY_F1);
	same_keysym("XK_F10", XK_F10, WPE_KEY_F10);
	same_keysym("XK_Left", XK_Left, WPE_KEY_LEFT);
	same_keysym("XK_Right", XK_Right, WPE_KEY_RIGHT);
	same_keysym("XK_Up", XK_Up, WPE_KEY_UP);
	same_keysym("XK_Down", XK_Down, WPE_KEY_DOWN);
	same_keysym("XK_Home", XK_Home, WPE_KEY_HOME);
	same_keysym("XK_End", XK_End, WPE_KEY_END);
	same_keysym("XK_Prior", XK_Prior, WPE_KEY_PGUP);
	same_keysym("XK_Next", XK_Next, WPE_KEY_PGDN);
	same_keysym("XK_Insert", XK_Insert, WPE_KEY_INSERT);
	same_keysym("XK_Delete", XK_Delete, WPE_KEY_DELETE);
	same_keysym("XK_BackSpace", XK_BackSpace, WPE_KEY_BACKSPACE);
	same_keysym("XK_Help", XK_Help, WPE_KEY_HELP);
	same_keysym("XK_Break", XK_Break, WPE_KEY_BREAK);
	same_keysym("XK_ISO_Left_Tab", XK_ISO_Left_Tab, WPE_KEY_TAB);
	same_keysym("XK_F11", XK_F11, WPE_KEY_F11);
	same_keysym("XK_F12", XK_F12, WPE_KEY_F12);
	same_keysym("XK_L1", XK_L1, WPE_KEY_F11);
	same_keysym("XK_L3", XK_L3, WPE_KEY_L3);
	same_keysym("XK_L10", XK_L10, WPE_KEY_L10);
#endif
#ifdef HAVE_XKB
	same_keysym("XKB_KEY_F1", XKB_KEY_F1, WPE_KEY_F1);
	same_keysym("XKB_KEY_F10", XKB_KEY_F10, WPE_KEY_F10);
	same_keysym("XKB_KEY_Left", XKB_KEY_Left, WPE_KEY_LEFT);
	same_keysym("XKB_KEY_Right", XKB_KEY_Right, WPE_KEY_RIGHT);
	same_keysym("XKB_KEY_Up", XKB_KEY_Up, WPE_KEY_UP);
	same_keysym("XKB_KEY_Down", XKB_KEY_Down, WPE_KEY_DOWN);
	same_keysym("XKB_KEY_Home", XKB_KEY_Home, WPE_KEY_HOME);
	same_keysym("XKB_KEY_End", XKB_KEY_End, WPE_KEY_END);
	same_keysym("XKB_KEY_Prior", XKB_KEY_Prior, WPE_KEY_PGUP);
	same_keysym("XKB_KEY_Next", XKB_KEY_Next, WPE_KEY_PGDN);
	same_keysym("XKB_KEY_Insert", XKB_KEY_Insert, WPE_KEY_INSERT);
	same_keysym("XKB_KEY_Delete", XKB_KEY_Delete, WPE_KEY_DELETE);
	same_keysym("XKB_KEY_BackSpace", XKB_KEY_BackSpace, WPE_KEY_BACKSPACE);
	same_keysym("XKB_KEY_Help", XKB_KEY_Help, WPE_KEY_HELP);
	same_keysym("XKB_KEY_Break", XKB_KEY_Break, WPE_KEY_BREAK);
	same_keysym("XKB_KEY_ISO_Left_Tab", XKB_KEY_ISO_Left_Tab, WPE_KEY_TAB);
	same_keysym("XKB_KEY_F11", XKB_KEY_F11, WPE_KEY_F11);
	same_keysym("XKB_KEY_F12", XKB_KEY_F12, WPE_KEY_F12);
	same_keysym("XKB_KEY_L1", XKB_KEY_L1, WPE_KEY_F11);
	same_keysym("XKB_KEY_L3", XKB_KEY_L3, WPE_KEY_L3);
	same_keysym("XKB_KEY_L10", XKB_KEY_L10, WPE_KEY_L10);
#endif
#if !defined(HAVE_XK) && !defined(HAVE_XKB)
	printf("note: no X11 or xkbcommon header here, the keysym numbers are not cross-checked\n");
#endif
}

/* The chords the Debug menu and the bars show: Ctrl-F2 Quit, Ctrl-F3 and
 * Ctrl-F6 Show stack, Ctrl-F5 and Ctrl-F7 Make Watch, Ctrl-F8 Toggle
 * Breakpoint, Ctrl-F9 Run, Ctrl-F10 Run/Continue, Ctrl-F4 Close. */
static void test_the_ctrl_f_chords_the_debug_menu_shows_are_decoded(void)
{
	static const char *const raw[NFKEYS] = {
		"1;5P", "1;5Q", "1;5R", "1;5S", "15;5~",
		"17;5~", "18;5~", "19;5~", "20;5~", "21;5~"
	};
	static const unsigned long ksym[NFKEYS] = {
		0xffbe, 0xffbf, 0xffc0, 0xffc1, 0xffc2,
		0xffc3, 0xffc4, 0xffc5, 0xffc6, 0xffc7
	};
	int n;

	for (n = 1; n <= NFKEYS; n++) {
		chk("Ctrl-Fn from KEY_F(24+n)", via_curses(KEY_F(24 + n), 0), ctrl_f[n - 1]);
		chk("Ctrl-Fn from a raw tail", via_csi(raw[n - 1]), ctrl_f[n - 1]);
		chk("Ctrl-Fn from a keysym", via_keysym(ksym[n - 1], WPE_MOD_CTRL), ctrl_f[n - 1]);
	}
	chk("Ctrl-F9 is Run", via_csi("20;5~"), CF9);
	chk("Ctrl-F8 is Toggle Breakpoint", via_curses(KEY_F(32), 0), CF8);
	chk("Ctrl-F2 is Quit", via_csi("1;5Q"), CF2);
}

static void test_console_shift_state_adds_the_shift_row(void)
{
#ifdef NCURSES
	int n;

	chk("console Shift on Up", via_curses(KEY_UP, WPE_MOD_SHIFT), CUP + WPE_KEY_SHIFT_OFFSET);
	chk("console Ctrl on Left", via_curses(KEY_LEFT, WPE_MOD_CTRL), CCLE);
	chk("console Shift on Delete", via_curses(KEY_DC, WPE_MOD_SHIFT), ShiftDel);
	chk("console Shift on F1", via_curses(KEY_F(1), WPE_MOD_SHIFT), SF1);
	/* Esc then Backspace with the console's Shift held is Shift-Alt-Backspace: redo */
	chk("console Shift on Alt-Backspace is redo",
	    wpe_key_code(WPE_KEY_BACKSPACE, WPE_MOD_ALT | WPE_MOD_SHIFT), SABS);
	chk("Alt-Backspace is undo", wpe_key_code(WPE_KEY_BACKSPACE, WPE_MOD_ALT), AltBS);
	/* the console sends Shift-F1..F10 as KEY_F(11..20) with its shift state set */
	for (n = 1; n <= NFKEYS; n++)
		chk("console Shift-Fn is KEY_F(10+n)", via_curses(KEY_F(10 + n), WPE_MOD_SHIFT), shift_f[n - 1]);
	/* the same numbers on a terminal with no console shift state are xterm's */
	chk("KEY_F(11) alone is F11", via_curses(KEY_F(11), 0), F11);
	chk("KEY_F(15) alone is Shift-F3", via_curses(KEY_F(15), 0), SF3);
#endif
	chk("console Shift is the shift offset", wpe_key_code(WPE_KEY_UP, WPE_MOD_SHIFT), CUP + WPE_KEY_SHIFT_OFFSET);
	chk("Shift-Insert is the editor's ShiftEin", wpe_key_code(WPE_KEY_INSERT, WPE_MOD_SHIFT), ShiftEin);
}

static void test_an_unbound_key_gives_zero_not_a_letter(void)
{
	chk("F11 is a key", wpe_key_code(WPE_KEY_F11, WPE_MOD_NONE), F11);
	chk("Ctrl-Up has no binding", wpe_key_code(WPE_KEY_UP, WPE_MOD_CTRL), 0);
	chk("Shift-Backspace has no binding", wpe_key_code(WPE_KEY_BACKSPACE, WPE_MOD_SHIFT), 0);
	chk("Ctrl-Shift-Tab has no binding", wpe_key_code(WPE_KEY_TAB, WPE_MOD_CTRL | WPE_MOD_SHIFT), 0);
	chk("the none key", wpe_key_code(WPE_KEY_NONE, WPE_MOD_NONE), 0);
	chk("a letter is not a key", via_keysym('a', 0), 0);
	chk("Pause alone is not Break", via_keysym(0xff13, 0), 0);
	chk("Ctrl-Pause is Break", via_keysym(0xff13, WPE_MOD_CTRL), CtrlBrk);
	chk("Break is Break", via_keysym(0xff6b, 0), CtrlBrk);
	chk("a tail that is no key", via_csi("9~"), 0);
	chk("letters are not a tail", via_csi("x"), 0);
	chk("an empty tail", via_csi(""), 0);
	chk("a tail with junk before its end", via_csi("1x5P"), 0);
	chk("a run of digits no key sends", via_csi("999999999999999~"), 0);
	chk("a huge modifier is no key", via_csi("1;99999999999999P"), 0);
	chk("Ctrl and Alt together are Ctrl", wpe_key_code(WPE_KEY_F1, WPE_MOD_CTRL | WPE_MOD_ALT), CF1);
}

#define NSUNKEYS 8

static void test_sun_keys_are_the_same_on_every_backend(void)
{
	static const int sun[NSUNKEYS] = { PROPS, UNDO, FRONT, COPY, OPEN, PASTE, FID, CUT };
	int n;

	/* the keysym numbers are shared by Xlib and xkbcommon, so one decode
	   serves X11 and Wayland: L3 (0xffca) to L10 */
	for (n = 0; n < NSUNKEYS; n++)
		chk("Sun key from its keysym", via_keysym(0xffcaUL + n, 0), sun[n]);
	/* L1 and L2 are F11 and F12's symbols: a PC keyboard's F11 must not
	   quit the editor (Stop) nor F12 redo (Again), so they are read as
	   F11 and F12, which the Borland key set gives no action */
	chk("F11 (Sun Stop's symbol) is F11", via_keysym(0xffc8UL, 0), F11);
	chk("F12 (Sun Again's symbol) is F12", via_keysym(0xffc9UL, 0), F12);
	chk("Shift-F11 is SF11", via_keysym(0xffc8UL, WPE_MOD_SHIFT), SF11);
#ifdef NCURSES
	chk("the terminal's F11 is F11", via_curses(KEY_F(11), 0), F11);
	chk("the terminal's F12 is F12", via_curses(KEY_F(12), 0), F12);
#endif
#ifdef HAVE_XK
	chk("XK_L1 is not STOP", via_keysym(XK_L1, 0), F11);
	chk("XK_L10 is CUT", via_keysym(XK_L10, 0), CUT);
#endif
#ifdef HAVE_XKB
	chk("XKB_KEY_L1 is not STOP", via_keysym(XKB_KEY_L1, 0), F11);
	chk("XKB_KEY_L10 is CUT", via_keysym(XKB_KEY_L10, 0), CUT);
#endif
}

static void test_shift_tab_is_btab_from_every_delivery(void)
{
	/* the keysym that comes with no modifier reported */
	chk("ISO_Left_Tab alone", via_keysym(0xfe20, 0), WPE_BTAB);
	chk("ISO_Left_Tab with Shift", via_keysym(0xfe20, WPE_MOD_SHIFT), WPE_BTAB);
	/* the displays always gave back-tab for it whatever else was held */
	chk("ISO_Left_Tab with Ctrl and Shift", via_keysym(0xfe20, WPE_MOD_CTRL | WPE_MOD_SHIFT), WPE_BTAB);
	chk("ISO_Left_Tab with Alt and Shift", via_keysym(0xfe20, WPE_MOD_ALT | WPE_MOD_SHIFT), WPE_BTAB);
	chk("Shift and a Tab character", wpe_key_code(WPE_KEY_TAB, WPE_MOD_SHIFT), WPE_BTAB);
	chk("a Tab character", wpe_key_code(WPE_KEY_TAB, WPE_MOD_NONE), WPE_TAB);
	chk("CSI Z", via_csi("Z"), WPE_BTAB);
#ifdef NCURSES
	chk("KEY_BTAB", via_curses(KEY_BTAB, 0), WPE_BTAB);
	chk("the console's Shift and a Tab", wpe_key_code(WPE_KEY_TAB, WPE_MOD_SHIFT), WPE_BTAB);
#endif
}

static void test_raw_tails_give_the_cursor_and_editing_keys(void)
{
	chk("CSI A", via_csi("A"), CUP);
	chk("CSI B", via_csi("B"), CDO);
	chk("CSI C", via_csi("C"), CRI);
	chk("CSI D", via_csi("D"), CLE);
	chk("CSI H", via_csi("H"), POS1);
	chk("CSI F", via_csi("F"), ENDE);
	chk("CSI 1~", via_csi("1~"), POS1);
	chk("CSI 2~", via_csi("2~"), EINFG);
	chk("CSI 3~", via_csi("3~"), ENTF);
	chk("CSI 4~", via_csi("4~"), ENDE);
	chk("CSI 5~", via_csi("5~"), BUP);
	chk("CSI 6~", via_csi("6~"), BDO);
	chk("CSI 1;5D is Ctrl-Left", via_csi("1;5D"), CCLE);
	chk("CSI 1;2A is Shift-Up", via_csi("1;2A"), CUP + WPE_KEY_SHIFT_OFFSET);
	chk("CSI 1;3P is Alt-F1", via_csi("1;3P"), AF1);
	chk("CSI 15;3~ is Alt-F5", via_csi("15;3~"), AF5);
	chk("CSI 3;2~ is Shift-Delete", via_csi("3;2~"), ShiftDel);
	chk("CSI 11~ is F1", via_csi("11~"), F1);
	chk("CSI 21~ is F10", via_csi("21~"), F10);
}

/* A terminfo name through the whole decode. */
static int via_name(const char *name)
{
	WpeBaseKey base;
	unsigned mods;

	if (!wpe_key_from_terminfo_name(name, 0, &base, &mods))
		return 0;
	return wpe_key_code(base, mods);
}

/* xterm's Shift cursor keys, which curses folds into KEY_SRIGHT and the
   rest, mark a block like the raw "ESC [ 1;2C" does. */
static void test_shifted_curses_cursor_keys_mark_a_block(void)
{
#ifdef NCURSES
	chk("KEY_SRIGHT", via_curses(KEY_SRIGHT, 0), CRI + WPE_KEY_SHIFT_OFFSET);
	chk("KEY_SLEFT", via_curses(KEY_SLEFT, 0), CLE + WPE_KEY_SHIFT_OFFSET);
	chk("KEY_SR is Shift-Up", via_curses(KEY_SR, 0), CUP + WPE_KEY_SHIFT_OFFSET);
	chk("KEY_SF is Shift-Down", via_curses(KEY_SF, 0), CDO + WPE_KEY_SHIFT_OFFSET);
	chk("KEY_SHOME", via_curses(KEY_SHOME, 0), POS1 + WPE_KEY_SHIFT_OFFSET);
	chk("KEY_SEND", via_curses(KEY_SEND, 0), ENDE + WPE_KEY_SHIFT_OFFSET);
	chk("KEY_SPREVIOUS", via_curses(KEY_SPREVIOUS, 0), BUP + WPE_KEY_SHIFT_OFFSET);
	chk("KEY_SNEXT", via_curses(KEY_SNEXT, 0), BDO + WPE_KEY_SHIFT_OFFSET);
	chk("KEY_SDC is Shift-Delete", via_curses(KEY_SDC, 0), ShiftDel);
	chk("KEY_SIC", via_curses(KEY_SIC, 0), EINFG + WPE_KEY_SHIFT_OFFSET);
	chk("KEY_SRIGHT and its raw tail agree", via_curses(KEY_SRIGHT, 0), via_csi("1;2C"));
#endif
}

/* The Ctrl and Alt cursor keys ncurses knows only by name give the same
   key as their raw sequences. */
static void test_terminfo_names_give_the_modified_cursor_keys(void)
{
	chk("kRIT5 is Ctrl-Right", via_name("kRIT5"), CCRI);
	chk("kLFT5 is Ctrl-Left", via_name("kLFT5"), CCLE);
	chk("kHOM5 is Ctrl-Home", via_name("kHOM5"), CPS1);
	chk("kEND5 is Ctrl-End", via_name("kEND5"), CEND);
	chk("kPRV5 is Ctrl-PgUp", via_name("kPRV5"), CBUP);
	chk("kNXT5 is Ctrl-PgDn", via_name("kNXT5"), CBDO);
	chk("kDC5 is Ctrl-Delete", via_name("kDC5"), CENTF);
	chk("kIC5 is Ctrl-Insert", via_name("kIC5"), CEINFG);
	chk("kRIT3 is Alt-Right", via_name("kRIT3"), CCRI);
	chk("kRIT6 is Ctrl-Shift-Right", via_name("kRIT6"), CCRI + WPE_KEY_SHIFT_OFFSET);
	chk("kUP is Shift-Up", via_name("kUP"), CUP + WPE_KEY_SHIFT_OFFSET);
	chk("kNXT5 and its raw tail agree", via_name("kNXT5"), via_csi("6;5~"));
	chk("kEND5 and its raw tail agree", via_name("kEND5"), via_csi("1;5F"));
	chk("kUP5 is unbound, not a letter", via_name("kUP5"), 0);
	chk("kxIN is not a key", via_name("kxIN"), 0);
	chk("kRIT1 is not a modifier", via_name("kRIT1"), 0);
	chk("kRITx is not a key", via_name("kRITx"), 0);
	chk("kRIT99 is past the modifiers", via_name("kRIT99"), 0);
	chk("an empty name", via_name(""), 0);
	chk("no name", wpe_key_from_terminfo_name(NULL, 0, NULL, NULL), 0);
}

/* The bytes st's and rxvt's terminfo bind to standard capabilities that
   name other keys (ked, kil1, kclr, kel) give the key that sends them. */
static void test_bound_sequences_give_the_key_that_sends_them(void)
{
	chk("st ked is Ctrl-End", via_seq("\033[1;5F"), CEND);
	chk("st kil1 is Ctrl-Insert", via_seq("\033[2;5~"), CEINFG);
	chk("st kclr is Ctrl-Delete", via_seq("\033[3;5~"), CENTF);
	chk("st kel is Shift-End", via_seq("\033[1;2F"), ENDE + WPE_KEY_SHIFT_OFFSET);
	chk("st krmir is Shift-Insert", via_seq("\033[2;2~"), EINFG + WPE_KEY_SHIFT_OFFSET);
	chk("st ka1 is Home", via_seq("\033[1~"), POS1);
	chk("rxvt kel is Ctrl-End", via_seq("\033[8^"), CEND);
	chk("rxvt kEND is Shift-End", via_seq("\033[8$"), ENDE + WPE_KEY_SHIFT_OFFSET);
	chk("rxvt kEND6 is Ctrl-Shift-End", via_seq("\033[8@"), CEND + WPE_KEY_SHIFT_OFFSET);
	chk("rxvt kDC5 is Ctrl-Delete", via_seq("\033[3^"), CENTF);
	chk("rxvt kIC5 is Ctrl-Insert", via_seq("\033[2^"), CEINFG);
	chk("rxvt kNXT5 is Ctrl-PgDn", via_seq("\033[6^"), CBDO);
	chk("rxvt kHOM5 is Ctrl-Home", via_seq("\033[7^"), CPS1);
	chk("rxvt kUP is Shift-Up", via_seq("\033[a"), CUP + WPE_KEY_SHIFT_OFFSET);
	chk("rxvt kDN is Shift-Down", via_seq("\033[b"), CDO + WPE_KEY_SHIFT_OFFSET);
	chk("rxvt kRIT is Shift-Right", via_seq("\033[c"), CRI + WPE_KEY_SHIFT_OFFSET);
	chk("rxvt kLFT is Shift-Left", via_seq("\033[d"), CLE + WPE_KEY_SHIFT_OFFSET);
	chk("rxvt kRIT5 is Ctrl-Right", via_seq("\033Oc"), CCRI);
	chk("rxvt kLFT5 is Ctrl-Left", via_seq("\033Od"), CCLE);
	chk("rxvt kUP5 is unbound, not a letter", via_seq("\033Oa"), 0);
	chk("rxvt and xterm Ctrl-End agree", via_seq("\033[8^"), via_name("kEND5"));
	chk("rxvt and xterm Shift-Right agree", via_seq("\033[c"), via_csi("1;2C"));
	chk("an SS3 arrow is plain", via_seq("\033OC"), CRI);
	chk("a lower-case letter with a parameter is no key", via_seq("\033[1;5c"), 0);
	chk("a lower-case letter past the arrows is no key", via_seq("\033[e"), 0);
	chk("rxvt's ending on a number no key has", via_seq("\033[9^"), 0);
	chk("the mouse report is no key", via_seq("\033[M"), 0);
	chk("the keypad centre is no key", via_seq("\033Ou"), 0);
	chk("no ESC is no key", via_seq("[1;5F"), 0);
	chk("another introducer is no key", via_seq("\033X1;5F"), 0);
	chk("ESC and the introducer alone", via_seq("\033["), 0);
	chk("ESC alone", via_seq("\033"), 0);
	chk("an empty sequence", via_seq(""), 0);
	chk("no sequence", wpe_key_from_sequence(NULL, 0, NULL, NULL), 0);
}

/* A report of either protocol through the owner, before the fold to the
   legacy keys, so a distinct code can be told from the plain key. */
static int via_report(const char *tail)
{
	WpeBaseKey base;
	unsigned mods;
	int cp;

	if (!wpe_key_from_csi_cp('[', tail, strlen(tail), &base, &mods, &cp))
		return 0;
	if (base == WPE_KEY_CHAR)
		return wpe_key_code_char(cp, mods);
	return wpe_key_code(base, mods);
}

/* The same through the whole decode the terminal calls, then the fold. */
static int via_decode(const char *tail)
{
	int is_text;

	return wpe_key_fold_legacy(wpe_key_decode_csi('[', tail, strlen(tail), &is_text));
}

static int text_of(const char *tail)
{
	int is_text;

	wpe_key_decode_csi('[', tail, strlen(tail), &is_text);
	return is_text;
}

/* "<cp>;<param>u" and "27;<param>;<cp>~" for a letter and a modifier
   parameter: the two protocols' spellings of one chord. */
static void spellings(char *kitty, char *mok, size_t size, int cp, int param)
{
	snprintf(kitty, size, "%d;%du", cp, param);
	snprintf(mok, size, "27;%d;%d~", param, cp);
}

#define PARAM_SHIFT 2
#define PARAM_ALT   3
#define PARAM_CTRL  5
#define PARAM_CTRL_SHIFT 6
#define PARAM_CTRL_ALT   7
#define PARAM_ALT_SHIFT  4
#define SPELL_MAX 32
#define LETTERS 26
#define ALT_SHIFT_FIRST 1024

static void test_f11_and_f12_are_keys_from_every_delivery(void)
{
	chk("CSI 23~ is F11", via_csi("23~"), F11);
	chk("CSI 24~ is F12", via_csi("24~"), F12);
	chk("CSI 23;2~ is Shift-F11", via_csi("23;2~"), SF11);
	chk("CSI 24;2~ is Shift-F12", via_csi("24;2~"), SF12);
	chk("CSI 23;5~ is Ctrl-F11", via_csi("23;5~"), CF11);
	chk("CSI 24;5~ is Ctrl-F12", via_csi("24;5~"), CF12);
	chk("CSI 23;3~ is Alt-F11", via_csi("23;3~"), AF11);
	chk("CSI 24;3~ is Alt-F12", via_csi("24;3~"), AF12);
	chk("F11 is not an existing code", F11 != F10 && F11 != POS1 && F12 != CUP, 1);
	chk("SF11 is F11 plus the shift offset", SF11, F11 + WPE_KEY_SHIFT_OFFSET);
	chk("SF12 is F12 plus the shift offset", SF12, F12 + WPE_KEY_SHIFT_OFFSET);
	chk("kitty F11", via_report("23~"), F11);
	chk("kitty Shift-F3 is CSI 13;2~, not a private code", via_report("13;2~"), SF3);
}

static void test_a_codepoint_with_modifiers_is_one_code(void)
{
	int i;

	chk("text",wpe_key_code_char('a', WPE_MOD_NONE), 'a');
	chk("text beyond ASCII", wpe_key_code_char(0x20ac, WPE_MOD_NONE), 0x20ac);
	chk("Esc", wpe_key_code_char(27, WPE_MOD_NONE), WPE_ESC);
	chk("Enter", wpe_key_code_char(13, WPE_MOD_NONE), WPE_CR);
	chk("Tab", wpe_key_code_char(9, WPE_MOD_NONE), WPE_TAB);
	chk("Shift-Tab", wpe_key_code_char(9, WPE_MOD_SHIFT), WPE_BTAB);
	chk("Ctrl-Tab", wpe_key_code_char(9, WPE_MOD_CTRL), WPE_CTRL_TAB);
	chk("Backspace", wpe_key_code_char(127, WPE_MOD_NONE), CtrlH);
	chk("Alt-Backspace", wpe_key_code_char(127, WPE_MOD_ALT), AltBS);
	for (i = 0; i < LETTERS; i++) {
		int cp = 'a' + i;
		int ctrl = i + 1;

		if (cp == 'h') ctrl = WPE_CTRL_H;
		if (cp == 'i') ctrl = WPE_CTRL_I;
		if (cp == 'm') ctrl = WPE_CTRL_M;
		chk("Ctrl-letter", wpe_key_code_char(cp, WPE_MOD_CTRL), ctrl);
		chk("Ctrl-capital is Ctrl-Shift", wpe_key_code_char('A' + i, WPE_MOD_CTRL), WPE_CS_FIRST + i);
		chk("Ctrl-Shift-letter", wpe_key_code_char(cp, WPE_MOD_CTRL | WPE_MOD_SHIFT), WPE_CS_FIRST + i);
		chk("Alt-Shift-letter is the window code", wpe_key_code_char(cp, WPE_MOD_ALT | WPE_MOD_SHIFT), ALT_SHIFT_FIRST + i);
		chk("Alt-capital is the window code", wpe_key_code_char('A' + i, WPE_MOD_ALT), ALT_SHIFT_FIRST + i);
		chk("Alt-letter is the Alt code", wpe_key_code_char(cp, WPE_MOD_ALT), wpe_alt_key_code(cp));
		chk("Alt-letter is bound", wpe_key_code_char(cp, WPE_MOD_ALT) != 0, 1);
		chk("Ctrl-Alt-letter is bound or reserved",
		    wpe_key_code_char(cp, WPE_MOD_CTRL | WPE_MOD_ALT) != 0, 1);
		chk("Ctrl-letter is never a Ctrl-Shift code",
		    wpe_key_code_char(cp, WPE_MOD_CTRL) != WPE_CS_FIRST + i, 1);
	}
	chk("Alt-Shift-S is window 28's code", wpe_key_code_char('s', WPE_MOD_ALT | WPE_MOD_SHIFT), ALT_SHIFT_FIRST + 'S' - 'A');
	chk("Alt-A", wpe_key_code_char('a', WPE_MOD_ALT), AltA);
	chk("Alt-X", wpe_key_code_char('x', WPE_MOD_ALT), AltX);
	chk("Alt-1", wpe_key_code_char('1', WPE_MOD_ALT), Alt1);
	chk("Alt-0", wpe_key_code_char('0', WPE_MOD_ALT), Alt0);
	chk("Alt-Space", wpe_key_code_char(' ', WPE_MOD_ALT), AltBl);
	chk("Alt-#", wpe_key_code_char('#', WPE_MOD_ALT), AltSYS);
	chk("Alt-comma is not bound", wpe_key_code_char(',', WPE_MOD_ALT), 0);
	chk("Ctrl-Alt-A keeps ESC Ctrl-A's code", wpe_key_code_char('a', WPE_MOD_CTRL | WPE_MOD_ALT), CBUP);
	chk("Ctrl-Alt-Q is reserved", wpe_key_code_char('q', WPE_MOD_CTRL | WPE_MOD_ALT), WPE_CA_FIRST + 'q' - 'a');
	chk("Ctrl-Space", wpe_key_code_char(' ', WPE_MOD_CTRL), WPE_CTRL_SPACE);
	chk("Ctrl-/", wpe_key_code_char('/', WPE_MOD_CTRL), WPE_CTRL_SLASH);
	chk("Ctrl-backquote", wpe_key_code_char('`', WPE_MOD_CTRL), WPE_CTRL_BACKQUOTE);
	chk("Ctrl-backslash is not Shift-Tab", wpe_key_code_char('\\', WPE_MOD_CTRL), WPE_CTRL_BACKSLASH);
	chk("Ctrl-]", wpe_key_code_char(']', WPE_MOD_CTRL), WPE_CTRL_RBRACKET);
	chk("Ctrl-[", wpe_key_code_char('[', WPE_MOD_CTRL), WPE_CTRL_LBRACKET);
	chk("Ctrl-comma", wpe_key_code_char(',', WPE_MOD_CTRL), WPE_CTRL_COMMA);
	chk("Ctrl-period", wpe_key_code_char('.', WPE_MOD_CTRL), WPE_CTRL_PERIOD);
	chk("Ctrl-=", wpe_key_code_char('=', WPE_MOD_CTRL), WPE_CTRL_EQUAL);
	chk("Ctrl--", wpe_key_code_char('-', WPE_MOD_CTRL), WPE_CTRL_MINUS);
	chk("Ctrl-3 is not bound", wpe_key_code_char('3', WPE_MOD_CTRL), 0);
	chk("Ctrl-Shift-/ is not bound", wpe_key_code_char('/', WPE_MOD_CTRL | WPE_MOD_SHIFT), 0);
	chk("a control character is not a key", wpe_key_code_char(1, WPE_MOD_NONE), 0);
	chk("Ctrl and a beyond-ASCII letter is not bound", wpe_key_code_char(0x20ac, WPE_MOD_CTRL), 0);
	chk("the codes are distinct from the control bytes",
	    WPE_CS_FIRST > 255 && WPE_CA_FIRST > 255 && WPE_CTRL_H > 255 && WPE_BTAB > 255, 1);
	chk("Ctrl-Shift and Ctrl-Alt ranges do not meet",
	    WPE_CS_LAST < WPE_CTRL_H && WPE_CTRL_MINUS < WPE_CA_FIRST, 1);
	chk("BTAB is not a control byte", WPE_BTAB != 28, 1);
}

static void test_every_protocol_spelling_gives_the_legacy_code(void)
{
	char kitty[SPELL_MAX], mok[SPELL_MAX];
	int i;

	for (i = 0; i < LETTERS; i++) {
		int cp = 'a' + i;

		spellings(kitty, mok, sizeof kitty, cp, PARAM_CTRL);
		chk("kitty Ctrl-letter", via_report(kitty), wpe_key_code_char(cp, WPE_MOD_CTRL));
		chk("mok Ctrl-letter", via_report(mok), wpe_key_code_char(cp, WPE_MOD_CTRL));
		chk("Ctrl-letter folds to the legacy byte", via_decode(kitty), i + 1);
		chk("Ctrl-letter, mok, folds to the legacy byte", via_decode(mok), i + 1);

		spellings(kitty, mok, sizeof kitty, cp, PARAM_CTRL_SHIFT);
		chk("kitty Ctrl-Shift-letter", via_report(kitty), WPE_CS_FIRST + i);
		chk("mok Ctrl-Shift-letter", via_report(mok), WPE_CS_FIRST + i);
		chk("Ctrl-Shift-letter folds to Ctrl-letter", via_decode(kitty), i + 1);

		spellings(kitty, mok, sizeof kitty, cp, PARAM_ALT);
		chk("kitty Alt-letter", via_report(kitty), wpe_alt_key_code(cp));
		chk("mok Alt-letter", via_report(mok), wpe_alt_key_code(cp));
		chk("kitty Alt-letter is what ESC and the letter give", via_decode(kitty), wpe_alt_key_code(cp));

		spellings(kitty, mok, sizeof kitty, cp, PARAM_ALT_SHIFT);
		chk("kitty Alt-Shift-letter", via_report(kitty), ALT_SHIFT_FIRST + i);
		chk("mok Alt-Shift-letter", via_report(mok), ALT_SHIFT_FIRST + i);

		spellings(kitty, mok, sizeof kitty, cp, PARAM_CTRL_ALT);
		chk("kitty Ctrl-Alt-letter", via_report(kitty), wpe_key_code_char(cp, WPE_MOD_CTRL | WPE_MOD_ALT));
		chk("mok Ctrl-Alt-letter", via_report(mok), wpe_key_code_char(cp, WPE_MOD_CTRL | WPE_MOD_ALT));
	}
	chk("kitty Ctrl-H is its own code", via_report("104;5u"), WPE_CTRL_H);
	chk("mok Ctrl-H is its own code", via_report("27;5;104~"), WPE_CTRL_H);
	chk("kitty Ctrl-I", via_report("105;5u"), WPE_CTRL_I);
	chk("kitty Ctrl-M", via_report("109;5u"), WPE_CTRL_M);
	chk("kitty Ctrl-[", via_report("91;5u"), WPE_CTRL_LBRACKET);
	chk("Ctrl-H folds to Backspace's byte", via_decode("104;5u"), CtrlH);
	chk("Ctrl-I folds to Tab's byte", via_decode("105;5u"), WPE_TAB);
	chk("Ctrl-M folds to Enter's byte", via_decode("109;5u"), WPE_CR);
	chk("Ctrl-[ folds to Esc", via_decode("91;5u"), WPE_ESC);
	chk("kitty Esc", via_report("27u"), WPE_ESC);
	chk("mok Esc", via_report("27;1;27~"), WPE_ESC);
	chk("kitty Ctrl-Space", via_report("32;5u"), WPE_CTRL_SPACE);
	chk("mok Ctrl-Space", via_report("27;5;32~"), WPE_CTRL_SPACE);
	chk("kitty Alt-Space", via_report("32;3u"), AltBl);
	chk("kitty Alt-1", via_report("49;3u"), Alt1);
	chk("kitty Alt-0", via_report("48;3u"), Alt0);
	chk("kitty Alt-#", via_report("35;3u"), AltSYS);
	chk("kitty Alt-Shift-3 with its shifted form is Alt-#", via_report("51:35;4u"), AltSYS);
	chk("kitty Ctrl-Tab", via_report("9;5u"), WPE_CTRL_TAB);
	chk("mok Ctrl-Tab", via_report("27;5;9~"), WPE_CTRL_TAB);
	chk("Ctrl-Tab folds to Tab, as a legacy terminal sends it", via_decode("9;5u"), WPE_TAB);
	chk("mok Ctrl-Tab folds to Tab", via_decode("27;5;9~"), WPE_TAB);
	chk("kitty Shift-Tab", via_report("9;2u"), WPE_BTAB);
	chk("mok Shift-Tab (xterm's own example)", via_report("27;2;9~"), WPE_BTAB);
	chk("kitty Tab", via_report("9u"), WPE_TAB);
	chk("kitty Enter", via_report("13u"), WPE_CR);
	chk("kitty Shift-Enter", via_report("13;2u"), WPE_CR);
	chk("kitty Alt-Enter is not bound", via_report("13;3u"), 0);
	chk("kitty Backspace", via_report("127u"), CtrlH);
	chk("kitty Alt-Backspace", via_report("127;3u"), AltBS);
	chk("kitty Shift-Backspace erases, as its legacy byte did", via_report("127;2u"), CtrlH);
	chk("kitty Ctrl-Backspace erases", via_report("127;5u"), CtrlH);
	chk("kitty Ctrl-Shift-Backspace erases", via_report("127;6u"), CtrlH);
	chk("mok Shift-Backspace erases", via_report("27;2;127~"), CtrlH);
	chk("mok Ctrl-Backspace erases", via_report("27;5;8~"), CtrlH);
	chk("kitty Alt-Shift-Backspace keeps its code", via_report("127;4u"), SABS);
	chk("kitty Ctrl-\\ is not Shift-Tab", via_report("92;5u"), WPE_CTRL_BACKSLASH);
}

static void test_the_spec_examples_of_the_kitty_protocol(void)
{
	chk("shift+a with its text is the text A", via_report("97;2;65u"), 'A');
	chk("... and the decode says it is text", text_of("97;2;65u"), 1);
	chk("a plain a is text", via_report("97u"), 'a');
	chk("Ctrl-a is not text", text_of("97;5u"), 0);
	chk("Ctrl-Shift-I", via_report("105;6u"), WPE_CS_FIRST + 'i' - 'a');
	chk("Ctrl-Shift-3 is no key", via_report("51;6u"), 0);
	chk("Ctrl-Shift-; is no key", via_report("59;6u"), 0);
	chk("Esc", via_report("27u"), WPE_ESC);
	chk("Shift-F3 is 13;2~", via_report("13;2~"), SF3);
	chk("Ctrl-Shift-P with its shifted key", via_report("112:80;6u"), WPE_CS_FIRST + 'p' - 'a');
	chk("Ctrl-Shift-P with shifted and base keys", via_report("112:80:112;6u"), WPE_CS_FIRST + 'p' - 'a');
	chk("Ctrl-Shift with a non-Latin letter and no US key is no key",
	    via_report("1103:1071:1094;6u"), 0);
	chk("Ctrl-S on a Cyrillic layout is read by its US key",
	    via_report("1099::115;5u"), CtrlS);
	chk("Alt-X on a Cyrillic layout is read by its US key",
	    via_report("1095::120;3u"), AltX);
	chk("typed text keeps its own character whatever the US key",
	    via_report("1099::115u"), 1099);
	chk("Ctrl-Z on a German layout stays Ctrl-Z, not its US place",
	    via_report("122::121;5u"), CtrlZ);
	chk("Alt-A on a French layout stays Alt-A, not its US place",
	    via_report("97::113;3u"), AltA);
	chk("Ctrl-S on a Dvorak layout stays Ctrl-S",
	    via_report("115::59;5u"), CtrlS);
	chk("the keypad's Enter", via_report("57414u"), WPE_CR);
	chk("the keypad's Shift-Left", via_report("57417;2u"), CLE + WPE_KEY_SHIFT_OFFSET);
	chk("a modifier key alone is nothing", via_report("57441u"), 0);
	chk("a release is nothing", via_report("97;5:3u"), 0);
	chk("a repeat is a press", via_report("97;5:2u"), CtrlA);
	chk("a press with its event type", via_report("97;5:1u"), CtrlA);
	chk("Super is not Ctrl", via_report("97;9u"), 0);
	chk("Hyper is not Ctrl", via_report("97;17u"), 0);
	chk("Meta is not Ctrl", via_report("97;33u"), 0);
	chk("Caps Lock does not change the chord", via_report("97;69u"), CtrlA);
	chk("Num Lock does not change the chord", via_report("97;133u"), CtrlA);
	chk("the keypad's 0 is the text 0", via_report("57399u"), '0');
	chk("the keypad's 9 is the text 9", via_report("57408u"), '9');
	chk("the keypad's decimal point", via_report("57409u"), '.');
	chk("the keypad's divide", via_report("57410u"), '/');
	chk("the keypad's multiply", via_report("57411u"), '*');
	chk("the keypad's minus", via_report("57412u"), '-');
	chk("the keypad's plus", via_report("57413u"), '+');
	chk("the keypad's equal", via_report("57415u"), '=');
	chk("the keypad's separator", via_report("57416u"), ',');
	chk("the keypad's Left", via_report("57417u"), CLE);
	chk("the keypad's Delete", via_report("57426u"), ENTF);
	chk("the keypad's Home with Ctrl", via_report("57423;5u"), CPS1);
	chk("the keypad's Begin is nothing", via_report("57427u"), 0);
	chk("F13 is nothing", via_report("57376u"), 0);
	chk("F35 is nothing", via_report("57398u"), 0);
	chk("Caps Lock key is nothing", via_report("57358u"), 0);
	chk("Print Screen is nothing", via_report("57361u"), 0);
	chk("Menu is nothing", via_report("57363u"), 0);
	chk("a media key is nothing", via_report("57428u"), 0);
	chk("the last modifier key is nothing", via_report("57454u"), 0);
	chk("a number past Unicode is nothing", via_report("1114112u"), 0);
	chk("an empty report is nothing", via_report("u"), 0);
	chk("a report with four fields is nothing", via_report("97;5;65;1u"), 0);
	chk("a report with four parts is nothing", via_report("97:1:2:3;5u"), 0);
	chk("a control character number is nothing", via_report("1;5u"), 0);
	chk("the kitty functional keys keep xterm's forms", via_report("1;2P"), SF1);
	chk("kitty Insert", via_report("2~"), EINFG);
	chk("kitty F5-F12 numbers", via_report("19~"), F8);
	chk("an introducer other than [ is not kitty", wpe_key_decode_csi('O', "97;5u", 5, &(int){0}), 0);
}

static void test_a_long_or_unrecognised_tail_is_no_key(void)
{
	chk("a junk tail of 1s", via_decode("1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1z"), 0);
	chk("three numbers that are not modifyOtherKeys", via_decode("1;1;1~"), 0);
	chk("27 with two numbers is no key", via_decode("27;5~"), 0);
	chk("a private-marked report is no key", via_decode("?1u"), 0);
	chk("modifyOtherKeys of an unknown number", via_decode("27;5;1~"), 0);
	chk("the colon on a legacy form is no key", via_decode("1;5:3A"), 0);
	chk("a plain legacy key still decodes", via_decode("1;5D"), CCLE);
}

static void test_fold_gives_the_legacy_codes_and_nothing_else(void)
{
	int i;

	chk("Ctrl-H", wpe_key_fold_legacy(WPE_CTRL_H), CtrlH);
	chk("Ctrl-I", wpe_key_fold_legacy(WPE_CTRL_I), CtrlI);
	chk("Ctrl-M", wpe_key_fold_legacy(WPE_CTRL_M), CtrlM);
	chk("Ctrl-[", wpe_key_fold_legacy(WPE_CTRL_LBRACKET), WPE_ESC);
	chk("Ctrl-Tab folds to Tab", wpe_key_fold_legacy(WPE_CTRL_TAB), WPE_TAB);
	for (i = 0; i < LETTERS; i++)
		chk("Ctrl-Shift-letter", wpe_key_fold_legacy(WPE_CS_FIRST + i), i + 1);
	chk("a plain key is unchanged", wpe_key_fold_legacy(F5), F5);
	chk("Ctrl-/ is unchanged", wpe_key_fold_legacy(WPE_CTRL_SLASH), WPE_CTRL_SLASH);
	chk("Ctrl-Alt letter is unchanged", wpe_key_fold_legacy(WPE_CA_FIRST), WPE_CA_FIRST);
}

int main(void)
{
	test_f11_and_f12_are_keys_from_every_delivery();
	test_a_codepoint_with_modifiers_is_one_code();
	test_every_protocol_spelling_gives_the_legacy_code();
	test_the_spec_examples_of_the_kitty_protocol();
	test_a_long_or_unrecognised_tail_is_no_key();
	test_fold_gives_the_legacy_codes_and_nothing_else();
	test_bound_sequences_give_the_key_that_sends_them();
	test_shifted_curses_cursor_keys_mark_a_block();
	test_terminfo_names_give_the_modified_cursor_keys();
	test_every_bound_pair_has_exactly_one_row();
	test_curses_f_key_blocks_give_plain_shift_ctrl_alt();
	test_keysym_numbers_serve_x11_and_wayland_alike();
	test_the_ctrl_f_chords_the_debug_menu_shows_are_decoded();
	test_console_shift_state_adds_the_shift_row();
	test_an_unbound_key_gives_zero_not_a_letter();
	test_sun_keys_are_the_same_on_every_backend();
	test_shift_tab_is_btab_from_every_delivery();
	test_raw_tails_give_the_cursor_and_editing_keys();
	if (fails)
		printf("test_keymap: %d FAILED\n", fails);
	else
		printf("test_keymap: all passed\n");
	return fails ? 1 : 0;
}
