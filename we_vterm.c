/* we_vterm.c - Embedded VT terminal for the xwpe Alt-F5 "User Screen"
 * (X11 and Wayland).
 *
 * See we_vterm.h for the rationale.  In short: the console backend hands the
 * real terminal back to the program (e_t_user_screen, we_term.c); the
 * graphical backends cannot, so they feed the captured program output to
 * libvterm and paint the resulting cell grid into xwpe's own SCREENCELL
 * buffer.
 *
 * Phase A (this file): a STATIC replay.  Alt-F5 takes the bytes already
 * captured in e_d_prog_output (filled during Ctrl-F9 Run / a debug session),
 * interprets them once, paints the final screen the program left behind,
 * waits for a key, and restores the editor.  A live, interactive terminal
 * (feeding keystrokes back to a running child) is a later phase; the parsing
 * and rendering core here is shared by both.
 */

#include "edit.h"
#include "we_vterm.h"

#ifdef HAVE_VTERM

#include <vterm.h>

extern char *e_d_prog_output;
extern int e_d_prog_output_len;
extern void e_d_pty_drain(void);

/* xwpe's attribute byte packs both colours into one int: attr = bg*16 + fg,
   each index in 0..15 into the editor's 16-colour palette (the renderer reads
   fg = attr%16, bg = attr/16 -- see we_xterm.c).  These two indices are the
   classic terminal defaults: light grey on black. */
#define VT_FG_DEFAULT  7   /* Light Gray */
#define VT_BG_DEFAULT  0   /* Black */
#define VT_ATTR(bg, fg) ((bg) * 16 + (fg))

/* Entries of the editor's palette. */
#define VT_PALETTE_SIZE 16

/* ------------------------------------------------------------------ colours */

/* The 16 xwpe palette entries are not in ANSI order, so we cannot index them
   directly with an SGR number.  Instead we ask the display for the RGB it
   paints each entry with (e_palette_rgb) and map every VT colour to the
   NEAREST palette entry by Euclidean distance.  That is robust to whatever
   the display resolves its colour names to and matches what the renderer
   will actually show. */
static unsigned char vt_pal[VT_PALETTE_SIZE][3];
static int vt_pal_ready = 0;

/* e_vt_load_palette - Snapshot the display's 16 RGB colours, once, so the
   program's colours on the User Screen match the editor's own on whichever
   display (X11 or Wayland) the user runs. */
static void e_vt_load_palette(void)
{
 int i;

 if (vt_pal_ready)
  return;
 for (i = 0; i < VT_PALETTE_SIZE; i++)
  e_palette_rgb(i, vt_pal[i]);
 vt_pal_ready = 1;
}

/* e_vt_nearest - Index of the palette entry closest to (r,g,b). */
static int e_vt_nearest(int r, int g, int b)
{
 int best = 0, best_d = 1 << 30, i;

 for (i = 0; i < VT_PALETTE_SIZE; i++)
 {
  int dr = r - vt_pal[i][0];
  int dg = g - vt_pal[i][1];
  int db = b - vt_pal[i][2];
  int d = dr * dr + dg * dg + db * db;
  if (d < best_d) { best_d = d; best = i; }
 }
 return best;
}

/* e_vt_fg - Foreground palette index for a cell colour.  A default
   foreground maps to light grey rather than the nearest match so unstyled
   output reads like a normal terminal. */
static int e_vt_fg(VTermScreen *vs, VTermColor col)
{
 if (VTERM_COLOR_IS_DEFAULT_FG(&col))
  return VT_FG_DEFAULT;
 vterm_screen_convert_color_to_rgb(vs, &col);
 return e_vt_nearest(col.rgb.red, col.rgb.green, col.rgb.blue);
}

/* e_vt_bg - Background palette index for a cell colour.  A default
   background maps to black. */
static int e_vt_bg(VTermScreen *vs, VTermColor col)
{
 if (VTERM_COLOR_IS_DEFAULT_BG(&col))
  return VT_BG_DEFAULT;
 vterm_screen_convert_color_to_rgb(vs, &col);
 return e_vt_nearest(col.rgb.red, col.rgb.green, col.rgb.blue);
}

/* e_vt_cell_attr - Pack a cell's colours into an xwpe attribute byte,
   honouring SGR reverse (swap fg/bg) and bold (brighten the foreground: the
   palette's bright variants live at index+8). */
static int e_vt_cell_attr(VTermScreen *vs, VTermScreenCell *cell)
{
 int fg = e_vt_fg(vs, cell->fg);
 int bg = e_vt_bg(vs, cell->bg);

 if (cell->attrs.bold && fg < 8)
  fg += 8;
 if (cell->attrs.reverse)
 {
  int t = fg; fg = bg; bg = t;
 }
 return VT_ATTR(bg, fg);
}

/* -------------------------------------------------------------- rendering */

/* e_vt_paint_cell - Render one VT cell at (x,y) into schirm and return
   how many extra columns it consumed (1 for a wide/CJK glyph, else 0). */
static int e_vt_paint_cell(VTermScreen *vs, int x, int y)
{
 VTermScreenCell cell;
 VTermPos pos;
 int attr, cp, cw;

 pos.row = y;
 pos.col = x;
 if (!vterm_screen_get_cell(vs, pos, &cell))
 {
  e_pr_char(x, y, ' ', VT_ATTR(VT_BG_DEFAULT, VT_FG_DEFAULT));
  return 0;
 }

 attr = e_vt_cell_attr(vs, &cell);
 cp = (cell.chars[0] != 0) ? (int)cell.chars[0] : ' ';
 cw = (cell.width >= 2) ? 2 : 1;

 if (cw == 2)
  return e_put_wide_cell(x, y, cp, cw, attr, x + 1 < MAXSCOL);

 e_pr_char(x, y, cp, attr);
 return 0;
}

/* e_vt_paint_grid - Paint the whole interpreted screen into schirm. */
static void e_vt_paint_grid(VTermScreen *vs)
{
 int x, y;

 for (y = 0; y < MAXSLNS; y++)
  for (x = 0; x < MAXSCOL; x++)
   x += e_vt_paint_cell(vs, x, y);
}

/* e_vt_paint_prompt - Overlay a discreet reverse-video hint on the
   bottom row so the user knows how to return to the editor. */
static void e_vt_paint_prompt(void)
{
 static const char *msg = " xwpe: press any key to return to the editor ";
 int attr = VT_ATTR(VT_FG_DEFAULT, VT_BG_DEFAULT); /* black on light grey */
 int y = MAXSLNS - 1;
 int x;

 for (x = 0; x < MAXSCOL; x++)
 {
  int c = (x < (int)strlen(msg)) ? msg[x] : ' ';
  e_pr_char(x, y, c, attr);
 }
}

/* -------------------------------------------------------------- lifecycle */

/* e_vt_render_output - Build a VTerm sized to the xwpe screen, feed it
   the captured bytes, and paint the result.  Separated from the modal wait so
   the parsing/rendering core can be reused by a future live terminal. */
static void e_vt_render_output(void)
{
 VTerm *vt;
 VTermScreen *vs;

 vt = vterm_new(MAXSLNS, MAXSCOL);
 vterm_set_utf8(vt, 1);
 vs = vterm_obtain_screen(vt);
 vterm_screen_reset(vs, 1);

 vterm_input_write(vt, e_d_prog_output, (size_t)e_d_prog_output_len);

 e_vt_load_palette();
 e_vt_paint_grid(vs);
 e_vt_paint_prompt();

 vterm_free(vt);
}

/* e_vterm_user_screen - Alt-F5 entry point.  See header. */
int e_vterm_user_screen(FENSTER *f)
{
 int saved_chrome;

 e_d_pty_drain();

 if (!e_d_prog_output || e_d_prog_output_len <= 0)
 {
  e_error("No program output yet -- run with Ctrl-F9 or start a debug session",
    0, f->fb);
  return 0;
 }

 /* The User Screen owns the whole window, so suppress the per-window fluid
    scrollbar overlay (wpe_render_chrome, called by every refresh): otherwise
    the editor's and Messages' scrollbars bleed on top of the program's
    painted screen.  Restored before the editor repaint so its scrollbars
    come back.  Same gate the scrollbar-drag path uses (we_mouse.c). */
 saved_chrome = wpe_paint_chrome_suppressed();
 wpe_paint_chrome_suppress(1);

 e_vt_render_output();
 e_refresh();

 wpe_paint_modal_getch();   /* any key returns to the editor */

 wpe_paint_chrome_suppress(saved_chrome);
 e_repaint_desk(f->ed->f[f->ed->mxedt]);
 return 0;
}

#endif /* HAVE_VTERM */
