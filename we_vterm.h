/* we_vterm.h - Embedded VT terminal for the xwpe Alt-F5 "User Screen".
 *
 * The console backend (wpe) drops out of ncurses and hands the real terminal
 * back to the program, so a program that PAINTS (cursor addressing, ANSI
 * colour, box drawing) shows exactly as it left the screen (e_t_user_screen,
 * we_term.c).  The X11 backend (xwpe) has no real terminal to drop to: instead
 * it INTERPRETS the captured output with libvterm and paints the resulting
 * cell grid into xwpe's own SCREENCELL buffer (schirm[]), so the same painting
 * program is reproduced faithfully inside the xwpe window.
 *
 * Built only when HAVE_VTERM is defined (configure detects libvterm and gates
 * it on X11).  Without it, Alt-F5 in xwpe falls back to focusing the Messages
 * panel (the 1.6.3 behaviour); see e_user_screen() in we_prog.c.
 */
#ifndef WE_VTERM_H
#define WE_VTERM_H

#include "edit.h"

#ifdef HAVE_VTERM
/* e_x_vterm_user_screen - X11 Alt-F5: render the captured program output with
 * an embedded VT terminal, full-window, then wait for a key and restore the
 * editor.  Mirrors e_t_user_screen() (console) but for xwpe.  Returns 0. */
int e_x_vterm_user_screen(FENSTER *f);
#endif

#endif /* WE_VTERM_H */
