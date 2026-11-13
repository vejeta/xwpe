/* we_layout_desk.c -- the editor's side of the box layout owner.
   See we_layout.h.

   The owner (we_layout.c) knows the desk only through the hooks below; this
   file gives them their meaning: the screen is MAXSCOL x MAXSLNS, the
   windows are laid out by e_relayout_windows, and the repaint after a
   resize is the display's own (e_u_repaint_resized). */

#include "messages.h"
#include "edit.h"
#include "we_layout.h"

/**
 * e_layout_screen - The screen as it is now.
 */
static void e_layout_screen(int *scol, int *slns)
{
 *scol = MAXSCOL;
 *slns = MAXSLNS;
}

/**
 * e_layout_relayout_windows - Scale the windows from the old grid to the
 * new one.  The windows' owner stays e_relayout_windows.
 */
static void e_layout_relayout_windows(int old_scol, int old_slns)
{
 if (WpeEditor != NULL)
  e_relayout_windows(WpeEditor, old_scol, old_slns);
}

/**
 * e_layout_forget_box_view - Stop a graphical display's desk repaint from
 * keeping a box over the new desk.
 *
 * On X11 and Wayland the desk's repaint keeps the last view opened (a
 * dialog, a dropdown) on top of the windows it draws, so an expose does not
 * wipe the box.  After a resize that view's cells describe the old screen:
 * kept, they would stay as a ghost at the old place.  So, unless the last
 * view is a window's own, it is forgotten before the repaint; the owner
 * then draws each box again over the new desk.  The terminal repaints no
 * box and has nothing to forget.
 */
static void e_layout_forget_box_view(void)
{
#ifndef NO_XWINDOWS
 extern PIC *e_X_l_pic;
 int i;

 if (!WpeIsXwin() || WpeEditor == NULL)
  return;
 for (i = 0; i <= WpeEditor->mxedt; i++)
  if (e_X_l_pic == WpeEditor->f[i]->pic)
   return;
 (*e_u_setlastpic)(NULL);
#endif
}

/**
 * e_layout_repaint_desk - The display's repaint at the new size.
 *
 * The one thing that differs by display, and why: the terminal frees every
 * window's saved view and repaints (its repaint does not rebuild them); X11
 * repaints through its own routine, which frees the views and reallocates
 * the grid and the back buffer; Wayland does the same and then forces one
 * full repaint of every cell.
 */
static void e_layout_repaint_desk(void)
{
 if (e_u_repaint_resized != NULL)
  (*e_u_repaint_resized)();
}

/**
 * e_layout_drop_pic - Free a box's saved background, restoring nothing.
 */
static void e_layout_drop_pic(void *pic)
{
 PIC *p = pic;

 e_free_view(&p);
}

/**
 * e_layout_say_no_memory - Say that a box could not be drawn again.
 */
static void e_layout_say_no_memory(const char *what)
{
 (void)what;
 if (WpeEditor != NULL && WpeEditor->mxedt >= 1)
  e_error(e_msg[ERR_LOWMEM], 0, WpeEditor->f[WpeEditor->mxedt]->fb);
}

/**
 * e_layout_desk_ready - Tell the layout owner how to see the editor's
 * desk.  Called once, before anything is drawn.
 */
void e_layout_desk_ready(void)
{
 static const WpeLayoutDesk desk = {
  e_layout_screen, e_layout_relayout_windows, e_layout_forget_box_view,
  e_layout_repaint_desk, e_layout_drop_pic, e_layout_say_no_memory
 };

 wpe_layout_desk_set(&desk);
}

/**
 * wpe_box_frame - Draw a box's frame at the place the owner gave it, and keep
 * the background it covered.
 * @box: an open box.
 * @title: the frame's title, or NULL.
 * @fr: @ft: @fes: the frame, text and shadow colours.
 *
 * The one way a box's frame is drawn, so the frame is always where the
 * owner placed the box, never at coordinates the box kept.  The saved
 * background is stamped with the screen's size at this moment; the owner
 * drops it on a resize and the loop's close puts back only a current one.
 * Return: 0, or -1 when there was no memory for the background.
 */
int wpe_box_frame(WpeBox *box, char *title, int fr, int ft, int fes)
{
 PIC *pic = e_std_kst(box->at.bx_left, box->at.bx_top, box->at.bx_right,
                      box->at.bx_bottom, title, 1, fr, ft, fes);

 if (pic == NULL)
  return -1;
 box->pic = pic;
 return 0;
}
