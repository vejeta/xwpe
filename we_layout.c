/* we_layout.c -- boxes have one layout path.  See we_layout.h.

   Three things live here and nowhere else: the function that turns a box's
   placement rule into a place on the screen, the stack of open boxes (kept
   on the screen owner's depth, so there is one stack and not two), and the
   walk that lays every box out again once a display has taken a new size.
   The generation counter, which the saved backgrounds are stamped with, is
   bumped when the grid changes, so a background saved at the old size can
   be recognised and never put back. */

#include <stddef.h>

#include "we_layout.h"
#include "we_paint.h"

/* Boxes open at once; the screen owner's own bound is the same. */
#define WPE_LAYOUT_STACK_MAX  WPE_PAINT_DEPTH_MAX

static WpeLayoutDesk layout_desk;
static int           layout_desk_known;
static WpeBox       *layout_stack[WPE_LAYOUT_STACK_MAX];
static int           layout_open;       /* boxes in the stack */
static unsigned      layout_generation = 1;
static int           layout_pending;
static int           layout_old_scol, layout_old_slns;
static int           layout_walking;
static const char   *layout_no_memory;
static int           layout_in_draw;    /* draw hooks running (they may nest through a box that opens a box) */
static unsigned      layout_outside;    /* frames opened outside the path */

/**
 * wpe_layout_desk_set - Tell the owner how to see the editor.
 * @desk: the hooks, copied.
 *
 * Called once, before anything is drawn.
 */
void wpe_layout_desk_set(const WpeLayoutDesk *desk)
{
 layout_desk = *desk;
 layout_desk_known = 1;
}

static void wpe_box_set(WpeBoxRect *at, int left, int top, int right, int bottom)
{
 at->bx_left = left;
 at->bx_top = top;
 at->bx_right = right;
 at->bx_bottom = bottom;
}

/**
 * wpe_box_place_centred - Centre a box in the rows between the bar and the
 * status row, or across them from @top_row down.
 *
 * A box that is bigger than the room is cut to the room and says so (@fits
 * is 0).  Nothing is ever placed on the bar row or the status row (nor on
 * the p->inset rows beside them), so a small screen shows the box whole or
 * clipped, never over the status line.  A centred box that does not fit is
 * drawn clipped at the top left of the room, where its first lines are.
 * The error box and the Ctrl-C box keep the corners their authors gave
 * them: p->centre_w and p->centre_h say what size they were centred as, and
 * the box is then cut at the right and bottom edges instead of moved.
 */
static void wpe_box_place_centred(const WpeBoxPlace *p, int scol, int slns,
                                  int top_row, int centre_rows, WpeBoxRect *at,
                                  int *fits)
{
 int first = WPE_BOX_FIRST_ROW + p->inset;
 int last = slns - 1 - WPE_BOX_STATUS_ROWS - p->inset;
 int avail_h, w = p->w, h = p->h, cw, ch, left, top;
 int own_corners = (p->centre_w > 0 || p->centre_h > 0);

 if (top_row < first)
  top_row = first;
 if (top_row > last)
  top_row = last;                 /* a hint below the last row: the last row */
 avail_h = last - top_row + 1;

 if (w > scol) w = scol;
 if (h > avail_h) h = avail_h;
 cw = p->centre_w > 0 ? p->centre_w : w;
 ch = p->centre_h > 0 ? p->centre_h : h;
 left = (scol - cw) / 2;
 if (left < 0) left = 0;
 if (left + w > scol) w = scol - left;
 top = centre_rows ? top_row + (avail_h - ch) / 2 : top_row;
 if (top < top_row) top = top_row;
 if (top + h - 1 > last) h = last - top + 1;
 *fits = (w == p->w && h == p->h);
 if (!*fits && centre_rows && !own_corners)
 {
  left = 0;
  top = top_row;
 }
 wpe_box_set(at, left, top, left + w - 1, top + h - 1);
}

/**
 * wpe_box_place_dialog - A dialog's rule: centred on the window under it.
 *
 * The rule the dialog engine has always used, moved here unchanged: a dock
 * pane (the assistant's, the messages) is a thin strip, so a dialog over it
 * is centred on the whole screen; the dialog is never started on the bar
 * row, is cut at the right and bottom edges, and is put back in the corner
 * when the cut left it too small to be a frame.
 */
static void wpe_box_place_dialog(const WpeBoxPlace *p, int scol, int slns,
                                 WpeBoxRect *at, int *fits)
{
 int dlg_w = p->w - 1, dlg_h = p->h - 1;
 int bx = 0, by = WPE_BOX_FIRST_ROW, bex = scol - 1, bey = slns - 2;
 int xa, ya, xe, ye;

 if (p->has_within && !p->within_is_pane)
 {
  bx = p->within.bx_left;
  by = p->within.bx_top;
  bex = p->within.bx_right;
  bey = p->within.bx_bottom;
 }
 xa = bx + (bex - bx - dlg_w) / 2;
 ya = by + (bey - by - dlg_h) / 2;
 if (xa < 0) xa = 0;
 if (ya < WPE_BOX_FIRST_ROW) ya = WPE_BOX_FIRST_ROW;
 xe = xa + dlg_w;
 ye = ya + dlg_h;
 if (xe >= scol) xe = scol - 1;
 if (ye >= slns - 1) ye = slns - 2;
 if (xa >= xe - 4) xa = 0;
 if (ya >= ye - 2) ya = WPE_BOX_FIRST_ROW;
 wpe_box_set(at, xa, ya, xe, ye);
 *fits = (xe - xa == dlg_w && ye - ya == dlg_h);
}

/**
 * wpe_box_place_from_bar - A dropdown: from row 1 at the bar entry's column.
 *
 * Moved left, not closed, when it would pass the right edge.  When its rows
 * do not all fit between the bar and the status row it is cut and @fits is
 * 0: the dropdown then answers "gone", because a menu with hidden rows
 * would run what the user cannot see.
 */
static void wpe_box_place_from_bar(const WpeBoxPlace *p, int scol, int slns,
                                   WpeBoxRect *at, int *fits)
{
 int w = p->w, h = p->h, left = p->x_hint, bottom;

 if (w > scol) w = scol;
 if (left + w > scol) left = scol - w;
 if (left < 0) left = 0;
 bottom = WPE_BOX_FIRST_ROW + h - 1;
 if (bottom > slns - 1 - WPE_BOX_STATUS_ROWS) bottom = slns - 1 - WPE_BOX_STATUS_ROWS;
 wpe_box_set(at, left, WPE_BOX_FIRST_ROW, left + w - 1, bottom);
 *fits = (w == p->w && bottom - WPE_BOX_FIRST_ROW + 1 == h);
}

/**
 * wpe_box_place_at_text - A popup beside a text cell: below it, else above.
 *
 * The popup the language server's hover and the code actions use: one row
 * under the cell, moved up above it when the rows do not fit before the
 * status row or the end of the window under it, and kept on the screen
 * across.
 */
static void wpe_box_place_at_text(const WpeBoxPlace *p, int scol, int slns,
                                  WpeBoxRect *at, int *fits)
{
 int cx = p->x_hint, cy = p->y_hint;
 int xa = cx - 1, xe = xa + p->w - 1, ya = cy + 1, ye = ya + p->h - 1;

 if (xe > scol - 1)
 {
  xe = scol - 1;
  xa = xe - (p->w - 1);
 }
 if (xa < 0)
 {
  xa = 0;
  xe = p->w - 1;
  if (xe > scol - 1) xe = scol - 1;
 }
 if (ye > slns - 2 || (p->has_within && ya >= p->within.bx_bottom))
 {
  ye = cy - 1;
  ya = ye - (p->h - 1);
 }
 if (ya < WPE_BOX_FIRST_ROW)
 {
  ya = WPE_BOX_FIRST_ROW;
  ye = ya + p->h - 1;
 }
 *fits = (xe - xa + 1 == p->w && ye <= slns - 2);
 if (ye > slns - 2)
  ye = slns - 2;                  /* taller than the room: cut, never over the status row */
 wpe_box_set(at, xa, ya, xe, ye);
}

/**
 * wpe_box_place_hinted - A box at its caller's corner, moved in to fit.
 */
static void wpe_box_place_hinted(const WpeBoxPlace *p, int scol, int slns,
                                 WpeBoxRect *at, int *fits)
{
 int w = p->w, h = p->h, left = p->x_hint, top = p->y_hint;
 int last_row = slns - 1 - WPE_BOX_STATUS_ROWS;

 if (w > scol) w = scol;
 if (h > last_row - WPE_BOX_FIRST_ROW + 1) h = last_row - WPE_BOX_FIRST_ROW + 1;
 if (left + w > scol) left = scol - w;
 if (left < 0) left = 0;
 if (top + h - 1 > last_row) top = last_row - h + 1;
 if (top < WPE_BOX_FIRST_ROW) top = WPE_BOX_FIRST_ROW;
 wpe_box_set(at, left, top, left + w - 1, top + h - 1);
 *fits = (w == p->w && h == p->h);
}

/**
 * wpe_box_place - The one function that turns a placement rule into a place.
 * @place: the rule and the box's size, frame included.
 * @scol: @slns: the screen as it is now.
 * @at: receives the cells the box covers.
 * @fits: receives 0 when the box had to be cut below the size it asked for.
 *
 * No other code computes where a box goes.  The user sees a box that is
 * centred again when the window is made bigger or smaller, a dropdown that
 * keeps its place under its menu entry, and never a box over the bar or the
 * status row.
 */
void wpe_box_place(const WpeBoxPlace *place, int scol, int slns,
                   WpeBoxRect *at, int *fits)
{
 switch (place->anchor)
 {
  case WPE_BOX_CENTRED_SCREEN:
   wpe_box_place_centred(place, scol, slns, WPE_BOX_FIRST_ROW, 1, at, fits);
   break;
  case WPE_BOX_TOP_CENTRED:
   wpe_box_place_centred(place, scol, slns,
     place->y_hint < WPE_BOX_FIRST_ROW ? WPE_BOX_FIRST_ROW : place->y_hint,
     0, at, fits);
   break;
  case WPE_BOX_CENTRED_WINDOW:
   wpe_box_place_dialog(place, scol, slns, at, fits);
   break;
  case WPE_BOX_FROM_BAR:
   wpe_box_place_from_bar(place, scol, slns, at, fits);
   break;
  case WPE_BOX_AT_TEXT:
   wpe_box_place_at_text(place, scol, slns, at, fits);
   break;
  case WPE_BOX_HINTED:
   wpe_box_place_hinted(place, scol, slns, at, fits);
   break;
  case WPE_BOX_BAR_ENTRY:
  {
   int right = place->x_hint + place->w - 1;

   if (right > scol - 1) right = scol - 1;
   wpe_box_set(at, place->x_hint, 0, right, 0);
   *fits = (right - place->x_hint + 1 == place->w);
   break;
  }
  case WPE_BOX_FULL_SCREEN:
  default:
   wpe_box_set(at, 0, 0, scol - 1, slns - 1);
   *fits = 1;
   break;
 }
}

/**
 * wpe_box_screen - The screen as it is now, through the editor's hook.
 */
static void wpe_box_screen(int *scol, int *slns)
{
 *scol = 80;
 *slns = 24;
 if (layout_desk_known && layout_desk.screen)
  layout_desk.screen(scol, slns);
}

/**
 * wpe_box_put - Compute where a box goes now and say so in the trace.
 */
static void wpe_box_put(WpeBox *box)
{
 int scol, slns;

 if (box->refresh != NULL)
  box->refresh(box);
 wpe_box_screen(&scol, &slns);
 wpe_box_place(&box->place, scol, slns, &box->at, &box->fits);
 wpe_ui_trace("layout placed %s at %d,%d %dx%d%s", box->what ? box->what : "a box",
              box->at.bx_left, box->at.bx_top,
              box->at.bx_right - box->at.bx_left + 1,
              box->at.bx_bottom - box->at.bx_top + 1,
              box->fits ? "" : " clipped");
}

/**
 * wpe_box_draw - Run a box's draw hook and keep its verdict.
 *
 * A draw reads no key and opens no box: when it is out of memory it says
 * so by its return value and the owner reports it once the walk is over,
 * because an error box opened now would land on the stack being walked.
 */
static WpeBoxDrawn wpe_box_draw(WpeBox *box)
{
 WpeBoxDrawn verdict = WPE_BOX_DRAWN;

 layout_in_draw++;
 if (box->draw != NULL)
  verdict = box->draw(box);
 layout_in_draw--;
 if (verdict == WPE_BOX_NO_MEMORY && layout_no_memory == NULL)
  layout_no_memory = box->what ? box->what : "a box";
 return verdict;
}

/**
 * wpe_box_open - A box takes the screen: place it, register it, draw it.
 * @box: the box, with its rule, its draw hook and its state.
 *
 * Registering is the screen owner's modal enter, so paints wait for the
 * box as they always did, and the box is on the stack the walk lays out
 * again after a resize.
 * Return: what the draw said; on WPE_BOX_NO_MEMORY the box is not open.
 */
WpeBoxDrawn wpe_box_open(WpeBox *box)
{
 int slot = wpe_paint_modal_depth();
 WpeBoxDrawn verdict;

 wpe_paint_modal_enter(box->what);
 box->slot = slot < WPE_LAYOUT_STACK_MAX ? slot : WPE_LAYOUT_STACK_MAX - 1;
 box->pic = NULL;
 box->moved = 0;
 box->open = 1;
 layout_stack[box->slot] = box;
 layout_open++;
 wpe_box_put(box);
 verdict = wpe_box_draw(box);
 if (verdict != WPE_BOX_DRAWN)
 {
  wpe_box_close(box);
  layout_no_memory = NULL;
 }
 return verdict;
}

/**
 * wpe_box_close - The box is closed: leave the stack and the screen owner.
 * @box: a box wpe_box_open opened; a box that was never opened is ignored.
 *
 * The background the box saved is put back by its own loop with
 * e_close_view, which restores only a background saved at the current
 * size (see wpe_layout_generation).
 */
void wpe_box_close(WpeBox *box)
{
 if (box->open == 0)
  return;
 if (layout_stack[box->slot] == box)
 {
  layout_stack[box->slot] = NULL;
  layout_open--;
 }
 box->open = 0;
 wpe_paint_modal_leave();
}

/**
 * wpe_box_getch - Read a key for an open box.
 * @box: the box.
 *
 * The one way a box reads a key.  When the screen was resized the owner has
 * already laid the box out again; the key reported is the editor's resize
 * sentinel, and box->moved tells the loop to refocus.
 * Return: the key.
 */
int wpe_box_getch(WpeBox *box)
{
 int c = wpe_paint_modal_getch();

 if (box->moved)
  box->moved = 0;
 return c;
}

/**
 * wpe_box_depth - How many boxes the owner has open.
 */
int wpe_box_depth(void)
{
 return layout_open;
}

/**
 * wpe_box_move - The user dragged or keyed a box to another place.
 * @box: an open box.
 * @dx: @dy: the cells it moved by.
 *
 * The box keeps the place the user gave it for as long as the screen keeps
 * its size, so a picker closing over the dialog redraws the dialog where
 * the user left it.  The next resize places it by its rule again.
 */
void wpe_box_move(WpeBox *box, int dx, int dy)
{
 box->at.bx_left += dx;
 box->at.bx_right += dx;
 box->at.bx_top += dy;
 box->at.bx_bottom += dy;
}

/**
 * wpe_box_redraw - Draw an open box again where it stands.
 * @box: an open box.
 *
 * Used when something floated over the box and went away without leaving
 * the box whole: the old background is dropped, the box is not placed
 * anew, and its draw hook paints it at box->at.
 * Return: what the draw said.
 */
WpeBoxDrawn wpe_box_redraw(WpeBox *box)
{
 if (box->pic != NULL && layout_desk_known && layout_desk.drop_pic)
  layout_desk.drop_pic(box->pic);
 box->pic = NULL;
 return wpe_box_draw(box);
}

/**
 * wpe_box_within - Say which window a box must sit on.
 * @place: the box's rule.
 * @left: @top: @right: @bottom: the window's cells.
 * @is_pane: the window is a docked tool pane, a strip a dialog is not
 *   centred on.
 */
void wpe_box_within(WpeBoxPlace *place, int left, int top, int right,
                    int bottom, int is_pane)
{
 place->has_within = 1;
 place->within_is_pane = is_pane;
 wpe_box_set(&place->within, left, top, right, bottom);
}

/**
 * wpe_layout_frame_outside - Check a frame the editor just opened.
 * @left: @top: @right: @bottom: the cells it covers.
 * @view_kind: e_open_view's kind of view.
 *
 * Every box frame is opened by a box's draw hook.  A frame opened at any
 * other time is a box that bypasses the layout owner, and the trace says so,
 * so the rows of the tests can assert that nothing went around it.  Exempt,
 * by name: the views of kind WPE_LAYOUT_PLAIN_VIEW (a window's frame, the
 * desk's keep-view, a drag) and the whole screen handed to a command.
 * Return: 1 when the frame is outside the path.
 */
int wpe_layout_frame_outside(int left, int top, int right, int bottom,
                             int view_kind)
{
 int scol, slns;

 if (layout_in_draw > 0 || view_kind == WPE_LAYOUT_PLAIN_VIEW)
  return 0;
 wpe_box_screen(&scol, &slns);
 if (left == 0 && top == 0 && right == scol - 1 && bottom == slns - 1)
  return 0;
 layout_outside++;
 wpe_ui_trace("layout frame outside the path %d,%d-%d,%d", left, top, right, bottom);
 return 1;
}

/**
 * wpe_layout_frames_outside - How many frames were opened outside the path.
 */
unsigned wpe_layout_frames_outside(void)
{
 return layout_outside;
}

/**
 * wpe_layout_generation - The size the screen has had.
 *
 * Every view saved by e_open_view carries the value it had then; a view
 * from before a resize describes a screen that is gone.
 */
unsigned wpe_layout_generation(void)
{
 return layout_generation;
}

/**
 * wpe_layout_note_resize - A display has taken a new grid.
 * @old_scol: @old_slns: the grid before.
 *
 * The generation moves at once, so a view saved before this point can no
 * longer be put back, even by a command that is still running (a window
 * the editor opened for it, an X11 expose).  The oldest size is kept when
 * several resizes arrive before the walk, because the windows scale from
 * the size they were last laid out for.
 */
void wpe_layout_note_resize(int old_scol, int old_slns)
{
 layout_generation++;
 if (!layout_pending)
 {
  layout_old_scol = old_scol;
  layout_old_slns = old_slns;
 }
 layout_pending = 1;
}

/**
 * wpe_layout_resize_pending - Has a display resized with no walk yet?
 */
int wpe_layout_resize_pending(void)
{
 return layout_pending;
}

/**
 * wpe_layout_walk_box - Lay one box out again over the new desk.
 *
 * Its old background is freed without being put back (it saved cells of the
 * old desk), the rule gives the new place, and the box draws itself, which
 * saves the new background over the desk already repainted.  A box whose
 * draw says it is gone leaves the stack; its loop finds out at its next key.
 */
static void wpe_layout_walk_box(WpeBox *box)
{
 if (box->pic != NULL && layout_desk_known && layout_desk.drop_pic)
  layout_desk.drop_pic(box->pic);
 box->pic = NULL;
 wpe_box_put(box);
 box->moved = 1;
 if (wpe_box_draw(box) == WPE_BOX_GONE)
 {
  box->open = 2;
  layout_stack[box->slot] = NULL;
  layout_open--;
 }
}

/**
 * wpe_layout_trace_resized - Say in the trace which size the walk starts for.
 *
 * The rows of the graphical tests read the grid the window took from this
 * line, since a window's size in pixels does not say it.
 */
static void wpe_layout_trace_resized(void)
{
 int scol, slns;

 wpe_box_screen(&scol, &slns);
 wpe_ui_trace("layout resized from %dx%d to %dx%d", layout_old_scol,
              layout_old_slns, scol, slns);
}

/**
 * wpe_layout_screen_resized - Lay the desk and every open box out again.
 * @old_scol: @old_slns: the grid before the display's new size.
 *
 * Called once, at the top of the display's key reader, never from inside a
 * paint: the windows are laid out for the new size, the display repaints
 * the desk, then each box from the outermost in is placed by its rule and
 * drawn, so the background each one saves holds the boxes under it drawn
 * fresh and closing them in order puts the screen back right.  A second
 * call while one runs is left pending and written to the trace.
 */
void wpe_layout_screen_resized(int old_scol, int old_slns)
{
 int i;

 if (layout_walking)
 {
  wpe_ui_trace("layout walk re-entered, left pending");
  wpe_layout_note_resize(old_scol, old_slns);
  return;
 }
 if (!layout_pending)
  wpe_layout_note_resize(old_scol, old_slns);
 layout_walking = 1;
 layout_pending = 0;
 wpe_layout_trace_resized();
 if (layout_desk_known)
 {
  if (layout_desk.forget_box_view)
   layout_desk.forget_box_view();
  if (layout_desk.relayout_windows)
   layout_desk.relayout_windows(layout_old_scol, layout_old_slns);
  if (layout_desk.repaint_desk)
   layout_desk.repaint_desk();
 }
 for (i = 0; i < WPE_LAYOUT_STACK_MAX; i++)
 {
  if (layout_stack[i] != NULL)
   wpe_layout_walk_box(layout_stack[i]);
 }
 layout_walking = 0;
 if (layout_no_memory != NULL)
 {
  const char *what = layout_no_memory;

  layout_no_memory = NULL;
  if (layout_desk_known && layout_desk.say_no_memory)
   layout_desk.say_no_memory(what);
 }
}

/**
 * wpe_layout_run_pending - The walk, when a display left one pending.
 *
 * The key reader calls this first on every pass, so a resize a display
 * noted while it was busy (a command's window, a live resize) is laid out
 * at the one point where nothing is being drawn.
 * Return: non-zero when the walk ran: the reader hands the editor its
 * resize sentinel.
 */
int wpe_layout_run_pending(void)
{
 if (!layout_pending || layout_walking)
  return 0;
 wpe_layout_screen_resized(layout_old_scol, layout_old_slns);
 return 1;
}
