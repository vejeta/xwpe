/* test_xcells.c -- a loss found in the middle of a painted row stops the row.
 *
 * Links we_xterm.c (with we_grid.c, we_path.c and we_fmt.c) and replaces the
 * renderer, the back buffer and Xlib with a recorder: no display, no server,
 * always runs.  The owner of the connection lives in WeXterm.c, which this
 * test does not link either, so the test is the server: it raises the loss the
 * owner's predicates read.
 *
 * What the user would see go wrong if a rule broke: the X server ends while
 * the editor is painting a screen full of cells.  Xlib reports the loss from
 * inside the one call that flushed or waited for a reply, and that call then
 * returns, so the loop goes on: every cell after it sends a request of its own
 * to a server that is gone.  On an X stack that keeps sending them the first
 * of those requests into libXrender ends the editor with SIGSEGV, so the .ESV
 * save never runs.  A check once a row leaves a whole row of them behind, so
 * the loops ask the owner a cell.
 *
 * Both routes of a refresh are counted: the Xft one (e_x_render_dirty_cells,
 * which draws a cell through the renderer's table) and the core X one (the
 * XDrawImageString loop of e_x_refresh, which sends a run of cells and the two
 * colour changes of the next one).
 *
 * The cases, on each route:
 *   painted      every dirty cell of the grid sends its requests, and the
 *                flush follows them
 *   lost mid-row the request of cell (LOSS_ROW, LOSS_COL) is the one inside
 *                which Xlib finds the loss: no request follows it, and the
 *                flush does not either
 *   lost before  a refresh asked for after the loss sends nothing at all
 */
#include <stdio.h>
#include "linebuf.h"
#include <string.h>
#include <X11/Xlib.h>

#include "../edit.h"
#include "../we_render.h"
#include "../we_fdloop.h"
#include "../we_clip.h"
#include "../we_keymap.h"
#include "../we_layout.h"
#include "../we_startplan.h"
#include "../WeXterm.h"
#include "../we_xbackbuf.h"

int e_x_refresh(void);

/* The grid the row paints on, and the cell of it whose request finds the
   loss.  Both are above WPE_GRID_MIN_COLS and WPE_GRID_MIN_ROWS, so a take
   gives exactly this size and the counts below are the grid's own. */
#define GRID_COLS  40
#define GRID_ROWS  8
#define GRID_CELLS (GRID_COLS * GRID_ROWS)
#define LOSS_ROW   3
#define LOSS_COL   7
/* The cell whose request finds the loss, counted from 1. */
#define LOSS_CELL  (LOSS_ROW * GRID_COLS + LOSS_COL + 1)
/* The core route sends three requests a dirty cell: the run of cells before
   it, and the two colour changes of the run it starts. */
#define CORE_A_CELL 3

static int fails;

static void chk(const char *what, int got, int want)
{
	if (got != want) {
		printf("FAIL: %s: got %d want %d\n", what, got, want);
		fails++;
	}
}

/* ---- the recorder ---- */

static int requests;        /* drawing requests counted */
static int flushes;         /* XFlush calls counted */
static int armed_at;        /* the request that finds the loss, 0 for none */
static int lost_at;         /* the request that did find it, 0 for none */
static int bb_exists = 1;   /* which route of e_x_refresh runs */

/* One request went out.  The armed one is the call inside which Xlib finds
   the connection broken, so it sets the flag the owner of the connection
   reads -- after the request, as Xlib does. */
static void note_request(void)
{
	requests++;
	if (armed_at && requests == armed_at) {
		lost_at = requests;
		WpeXInfo.lost = 1;
	}
}

/* The owner of the connection lives in WeXterm.c, which this test does not
   link: these are the predicates it holds, and the test is the server. */
int e_x_conn_usable(const char *what)
{
	(void)what;
	return !WpeXInfo.lost;
}

int e_x_conn_step_usable(void)
{
	return !WpeXInfo.lost;
}

/* ---- the renderer's table, as a recorder ---- */

static void rec_draw_text(int x, int y, const char *utf8, int u8len,
                          int cell_width, int fg_idx, int bg_idx)
{
	(void)x; (void)y; (void)utf8; (void)u8len; (void)cell_width;
	(void)fg_idx; (void)bg_idx;
	note_request();
}

static void rec_draw_rect(int x, int y, int w, int h, int color_idx)
{
	(void)x; (void)y; (void)w; (void)h; (void)color_idx;
	note_request();
}

static void rec_draw_acs(int sc, int px, int py, int fg_idx, int bg_idx)
{
	(void)sc; (void)px; (void)py; (void)fg_idx; (void)bg_idx;
	note_request();
}

static void rec_draw_lock(int px, int py, int cw, int fg_idx, int bg_idx)
{
	(void)px; (void)py; (void)cw; (void)fg_idx; (void)bg_idx;
	note_request();
}

/* ---- Xlib, as a recorder ---- */

int XFlush(Display *display)
{
	(void)display;
	flushes++;
	return 1;
}

int XDrawImageString(Display *display, Drawable d, GC gc, int x, int y,
                     const char *string, int length)
{
	(void)display; (void)d; (void)gc; (void)x; (void)y; (void)string;
	(void)length;
	note_request();
	return 0;
}

int XSetForeground(Display *display, GC gc, unsigned long foreground)
{
	(void)display; (void)gc; (void)foreground;
	note_request();
	return 0;
}

int XSetBackground(Display *display, GC gc, unsigned long background)
{
	(void)display; (void)gc; (void)background;
	note_request();
	return 0;
}

/* ---- the back buffer, whose owner this test does not link ---- */

int e_x_backbuf_exists(void)
{
	return bb_exists;
}

Drawable e_x_backbuf_target(void)
{
	return (Drawable)0;
}

void e_x_backbuf_remake(int pixel_w, int pixel_h, int keep_picture,
                        unsigned long fill_pixel)
{
	(void)pixel_w; (void)pixel_h; (void)keep_picture; (void)fill_pixel;
}

void e_x_backbuf_present(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
}

void e_x_backbuf_restore(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
}

void e_x_backbuf_restore_done(void)
{
}

void e_x_backbuf_scroll(int sx, int sy, int dx, int dy, int w, int h)
{
	(void)sx; (void)sy; (void)dx; (void)dy; (void)w; (void)h;
}

void e_x_backbuf_clear_outside(int win_w, int win_h, int grid_w, int grid_h)
{
	(void)win_w; (void)win_h; (void)grid_w; (void)grid_h;
}

/* ---- the rest of the editor, which this test does not link ---- */

WpeXStruct WpeXInfo;
WpeRenderBackend WpeRender;
ECNT *WpeEditor;
/* The desk the refresh clears the frame lines of: no window in it, so the
   walk over the windows has nothing to walk and the counts below are the
   cells alone. */
static ECNT stub_editor;
struct mouse e_mouse;
char *ctree[5];
char *e_tmp_dir;
PIC *e_X_l_pic;
int cur_x, cur_y;
int e_input_was_char;
int e_d_async_pending;
char MCI, MCA, RD1, RD2, RD3, RD4, RD5, RD6;
char RE1, RE2, RE3, RE4, RE5, RE6, WBT;

int (*fk_u_locate)(int x, int y);
int (*fk_u_cursor)(int x);
int (*fk_u_putchar)(int c);
int (*u_bioskey)(void);
int (*e_frb_u_menue)(int sw, int xa, int ya, FENSTER *f, int md);
COLOR (*e_s_u_clr)(int f, int b);
COLOR (*e_n_u_clr)(int fb);
void (*e_pr_u_col_kasten)(int xa, int ya, int x, int y, FENSTER *f, int sw);
int (*fk_mouse)(int g[]);
int (*fk_u_grab_pointer)(int on);
int (*fk_u_drag_next)(int *px, int *py);
int (*e_u_refresh)(void);
int (*e_u_getch)(void);
int (*e_u_sys_ini)(void);
int (*e_u_sys_end)(void);
int (*e_u_d_switch_out)(int sw);
int (*e_u_switch_screen)(int sw);
int (*e_u_deb_out)(FENSTER *f);
int (*e_u_palette_rgb)(int idx, unsigned char rgb[3]);
int (*e_u_system)(struct WpeStartPlan *plan);
int (*e_u_key_take)(int want);
void (*WpeMouseChangeShape)(WpeMouseShape new_shape);
void (*WpeMouseRestoreShape)(void);
void (*e_clip_os_set)(const char *utf8, int len);
char *(*e_clip_os_get)(int *len);
void (*e_u_serve_display)(void);
void (*e_u_focus_editor)(void);
void (*e_u_raise_editor)(void);
void (*e_u_setlastpic)(PIC *pic);
void (*e_u_repaint_resized)(void);
void (*e_u_screen_reset)(void);
void (*WpeDisplayEnd)(void);
void (*WpeDisplayReleaseFromHandler)(void);

/* The cursor the refresh hides and shows again.  A refresh counts the cells
   alone when the cursor is off, which is what the cases want. */
static int stub_cursor(int on)
{
	return (cur_on = on);
}

int e_abs_refr(void) { return 0; }
int e_clip_paste_order(int own_clipboard, int other_clipboard,
                       int other_primary, int order[2])
{
	(void)own_clipboard; (void)other_clipboard; (void)other_primary;
	(void)order;
	return 0;
}
int e_close_view(PIC *pic, int sw) { (void)pic; (void)sw; return 0; }
void e_cls(int frb, int chr) { (void)frb; (void)chr; }
int e_codepoint_to_utf8(int cp, unsigned char *out)
{
	(void)cp; (void)out;
	return 0;
}
void e_cursor(FENSTER *f, int sw) { (void)f; (void)sw; }
void e_d_place_cursor_in_messages(void) { }
void e_fd_key_wait(int keyboard_watched) { (void)keyboard_watched; }
int e_fd_watch(int fd, int events, wpe_fd_callback cb, void *data,
               const char *what)
{
	(void)fd; (void)events; (void)cb; (void)data; (void)what;
	return 0;
}
void e_firstl(FENSTER *f, int sw) { (void)f; (void)sw; }
int e_frb_x_menue(int sw, int xa, int ya, FENSTER *f, int md)
{
	(void)sw; (void)xa; (void)ya; (void)f; (void)md;
	return 0;
}
void e_free_view(PIC **pp) { (void)pp; }
void e_ini_desk(ECNT *cn) { (void)cn; }
int e_message(int sw, char *str, FENSTER *f)
{
	(void)sw; (void)str; (void)f;
	return 0;
}
void end_repaint(void) { }
PIC *e_open_view(int xa, int ya, int xe, int ye, int col, int sw)
{
	(void)xa; (void)ya; (void)xe; (void)ye; (void)col; (void)sw;
	return NULL;
}
void e_pr_x_col_kasten(int xa, int ya, int x, int y, FENSTER *f, int sw)
{
	(void)xa; (void)ya; (void)x; (void)y; (void)f; (void)sw;
}
int e_quit(FENSTER *f) { (void)f; return 0; }
int e_schirm(FENSTER *f, int sw) { (void)f; (void)sw; return 0; }
void e_screen_say_pending(void) { }
WpeGridTake e_screen_take(int cols, int rows, int *old_cols, int *old_rows)
{
	(void)cols; (void)rows; (void)old_cols; (void)old_rows;
	return WPE_GRID_SAME;
}
int e_shell_run(struct WpeStartPlan *plan, char *const argv[])
{
	(void)plan; (void)argv;
	return 0;
}
COLOR e_s_x_clr(int f, int b)
{
	COLOR no_color;
	(void)f; (void)b;
	memset(&no_color, 0, sizeof no_color);
	return no_color;
}
COLOR e_n_x_clr(int fb)
{
	COLOR no_color;
	(void)fb;
	memset(&no_color, 0, sizeof no_color);
	return no_color;
}
int e_tast_sim(int c) { (void)c; return 0; }
int e_t_deb_out(FENSTER *f) { (void)f; return 0; }
void ini_repaint(ECNT *cn) { (void)cn; }
int wpe_key_code(WpeBaseKey base, unsigned mods) { (void)base; (void)mods; return 0; }
int wpe_key_code_char(int codepoint, unsigned mods)
{
	(void)codepoint; (void)mods;
	return 0;
}
int wpe_key_from_keysym(unsigned long keysym, unsigned *mods, WpeBaseKey *base)
{
	(void)keysym; (void)mods; (void)base;
	return 0;
}
int wpe_keysym_chord_codepoint(unsigned long keysym) { (void)keysym; return 0; }
int wpe_keysym_is_chord_letter(unsigned long keysym) { (void)keysym; return 0; }
int wpe_layout_run_pending(void) { return 0; }
void wpe_layout_screen_resized(int old_scol, int old_slns)
{
	(void)old_scol; (void)old_slns;
}
void wpe_render_chrome(void) { }
int wpe_startplan_carry(WpeStartPlan *p, char *const carrier[], int from)
{
	(void)p; (void)carrier; (void)from;
	return 0;
}
void WpeNullFunction(void) { }
int WpeZeroFunction(void) { return 0; }
void WpeXInit(int *argc, char **argv) { (void)argc; (void)argv; }

unsigned long e_x_blank_pixel(void) { return 0; }
Colormap e_x_palette_colormap(void) { return (Colormap)0; }
int e_x_palette_rgb(int idx, unsigned char rgb[3])
{
	(void)idx; (void)rgb;
	return 0;
}
void e_x_grid_of_pixels(int pixel_w, int pixel_h, int *cols, int *rows)
{
	(void)pixel_w; (void)pixel_h; (void)cols; (void)rows;
}
int e_x_refit_font_for_dpi(void) { return 0; }

/* ---- the cases ---- */

/* A character a cell, and a colour that alternates a cell so the core route
   breaks its run at every one of them; every cell differs from what the
   display last showed, so every cell of the grid is dirty.  No cell is a box
   corner, so the segment cache stays empty and adds no request of its own. */
static void fill_grid(void)
{
	int n;

	for (n = 0; n < GRID_CELLS; n++) {
		schirm[n].ch = 'A' + (n % 26);
		schirm[n].attr = (n % 2) ? 0x01 : 0x10;
		schirm[n].flags = 0;
		altschirm[n].ch = 0;
		altschirm[n].attr = 0;
		altschirm[n].flags = 0;
		extbyte[n] = 0;
		altextbyte[n] = 0;
	}
}

/* Start a case: a clean recorder, a grid every cell of which is dirty, and
   the loss armed at request ARM (0 for a run that finds no loss). */
static void start_case(int arm)
{
	int i;

	requests = 0;
	flushes = 0;
	lost_at = 0;
	armed_at = arm;
	WpeXInfo.lost = 0;
	WpeXInfo.display = NULL;
	WpeXInfo.window = 0;
	WpeXInfo.gc = NULL;
	WpeXInfo.font_width = 8;
	WpeXInfo.font_height = 16;
	for (i = 0; i < 16; i++)
		WpeXInfo.colors[i] = i + 1;
	cur_on = 0;
	cur_x = 0;
	cur_y = 0;
	fill_grid();
}

#ifdef HAVE_XFT
static void case_xft_paints_every_cell(void)
{
	start_case(0);
	e_x_refresh();
	chk("xft: every dirty cell sends a request of its own", requests,
		GRID_CELLS);
	chk("xft: the flush follows the cells", flushes, 1);
	chk("xft: the server is still there", WpeXInfo.lost, 0);
}

static void case_xft_lost_mid_row(void)
{
	start_case(LOSS_CELL);
	e_x_refresh();
	chk("xft: the armed cell was the one that found the loss", lost_at,
		LOSS_CELL);
	chk("xft: no cell after it sends a request", requests, LOSS_CELL);
	chk("xft: no flush follows the loss", flushes, 0);
}

static void case_xft_lost_before(void)
{
	start_case(0);
	WpeXInfo.lost = 1;
	e_x_refresh();
	chk("xft: a refresh asked for after the loss sends nothing", requests, 0);
	chk("xft: ... and no flush", flushes, 0);
}
#endif /* HAVE_XFT */

static void case_core_paints_every_cell(void)
{
	start_case(0);
	e_x_refresh();
	chk("core: three requests a dirty cell and one for the last run",
		requests, GRID_CELLS * CORE_A_CELL + 1);
	chk("core: the flush follows the cells", flushes, 1);
	chk("core: the server is still there", WpeXInfo.lost, 0);
}

static void case_core_lost_mid_row(void)
{
	start_case(LOSS_CELL * CORE_A_CELL);
	e_x_refresh();
	chk("core: the armed cell was the one that found the loss", lost_at,
		LOSS_CELL * CORE_A_CELL);
	chk("core: no cell after it sends a request", requests,
		LOSS_CELL * CORE_A_CELL);
	chk("core: no flush follows the loss", flushes, 0);
}

static void case_core_lost_before(void)
{
	start_case(0);
	WpeXInfo.lost = 1;
	e_x_refresh();
	chk("core: a refresh asked for after the loss sends nothing", requests, 0);
	chk("core: ... and no flush", flushes, 0);
}

int main(void)
{
	if (wpe_grid_take(GRID_COLS, GRID_ROWS) != WPE_GRID_CHANGED)
		printf("note: the grid took the size it already had\n");
	chk("the grid is the size the counts are made of", MAXSCOL, GRID_COLS);
	chk("the grid is the height the counts are made of", MAXSLNS, GRID_ROWS);

	fk_u_cursor = stub_cursor;
	stub_editor.mxedt = 0;
	WpeEditor = &stub_editor;
	WpeRender.draw_rect = rec_draw_rect;
	WpeRender.draw_text = rec_draw_text;
	WpeRender.draw_acs = rec_draw_acs;
	WpeRender.draw_lock = rec_draw_lock;
	WpeRender.font_width = 8;
	WpeRender.font_height = 16;

#ifdef HAVE_XFT
	bb_exists = 1;                 /* the Xft route: a buffer and a renderer */
	case_xft_paints_every_cell();
	case_xft_lost_mid_row();
	case_xft_lost_before();
#endif
	bb_exists = 0;                 /* the core X route: no buffer to draw on */
	case_core_paints_every_cell();
	case_core_lost_mid_row();
	case_core_lost_before();

	if (fails) {
		printf("test_xcells: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_xcells: ok\n");
	return 0;
}
