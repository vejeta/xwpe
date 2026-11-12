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
#include "../keys.h"
#include "../unixkeys.h"

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

/* An escape sequence tail through the whole decode. */
static int via_csi(const char *tail)
{
	WpeBaseKey base;
	unsigned mods;

	if (!wpe_key_from_csi(tail, strlen(tail), &base, &mods))
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
	chk("KEY_F(11) alone is F11, which nothing binds", via_curses(KEY_F(11), 0), 0);
	chk("KEY_F(15) alone is Shift-F3", via_curses(KEY_F(15), 0), SF3);
#endif
	chk("console Shift is the shift offset", wpe_key_code(WPE_KEY_UP, WPE_MOD_SHIFT), CUP + WPE_KEY_SHIFT_OFFSET);
	chk("Shift-Insert is the editor's ShiftEin", wpe_key_code(WPE_KEY_INSERT, WPE_MOD_SHIFT), ShiftEin);
}

static void test_an_unbound_key_gives_zero_not_a_letter(void)
{
	chk("F11 has no binding", wpe_key_code(WPE_KEY_F11, WPE_MOD_NONE), 0);
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
	   quit the editor (Stop) nor F12 redo (Again), and the terminal never
	   bound them either */
	chk("F11 (Sun Stop's symbol) does nothing", via_keysym(0xffc8UL, 0), 0);
	chk("F12 (Sun Again's symbol) does nothing", via_keysym(0xffc9UL, 0), 0);
	chk("Shift-F11 does nothing", via_keysym(0xffc8UL, WPE_MOD_SHIFT), 0);
#ifdef NCURSES
	chk("the terminal's F11 does nothing", via_curses(KEY_F(11), 0), 0);
	chk("the terminal's F12 does nothing", via_curses(KEY_F(12), 0), 0);
#endif
#ifdef HAVE_XK
	chk("XK_L1 is not STOP", via_keysym(XK_L1, 0), 0);
	chk("XK_L10 is CUT", via_keysym(XK_L10, 0), CUT);
#endif
#ifdef HAVE_XKB
	chk("XKB_KEY_L1 is not STOP", via_keysym(XKB_KEY_L1, 0), 0);
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

int main(void)
{
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
