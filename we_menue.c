/* we_menue.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include "we_menurows.h"
#include "we_testdump.h"

/**
 * WpeMenuGetch - Read one key for the menu bar or a dropdown.
 *
 * A test watching the editor's state learns from here that the key it
 * sends next picks a menu item, not text.
 * Return: the key, as wpe_paint_modal_getch gives it.
 */
static int WpeMenuGetch(void)
{
  int c;

  wpe_test_ctx_enter(WPE_TEST_CTX_MENU, NULL);
  c = wpe_paint_modal_getch();
  wpe_test_ctx_leave();
  return c;
}

/**
 * WpeMainmenuReadKey - Read the next key, or click, for the menu bar.
 *
 * While the bar waits for the user it owns the screen: a stop, a build's
 * end or a reply that comes meanwhile is shown once the bar has closed, so
 * the command picked from it acts on the editor the user was looking at.
 * Return: the key; a click on the bar is turned into the key it stands for.
 */
static int WpeMainmenuReadKey(void)
{
  int c = WpeMenuGetch();

#if MOUSE
  if (c == -1)
    c = e_m1_mouse();
#endif
  return c;
}

/* main menu control bar */
int WpeHandleMainmenu(int n, FENSTER *f)
{
  extern int      e_mn_men;
  int             i, c = 255, nold = n <= 0 ? 1 : n - 1;
  extern OPT      opt[];
  extern char    *e_hlp;
  ECNT           *cn = f->ed;
  int             count;
  MENU           *mainmenu;

  mainmenu = e_menu_build(f, &count);
  fk_cursor(0);
  if(n < 0)
    n = 0;
  else
    c = WPE_CR;

  /* check for valid n */
  if(n < 0 || n > MENOPT - 1)
    n = 0;

  /* go until the user leaves the main control bar */
  while(c != WPE_ESC)
  {

    f = cn->f[cn->mxedt];
    if(e_menu_key_to_editor(f, c) == 0)
    {
      c = 0;
      break;
    }

    /* check for a menu shortcut */
    for(i = 0; i < MENOPT; i++)
      if(c == opt[i].s || c == opt[i].as)
      {
        n = i;
        c = WPE_CR;
      }

    /* if the selection is not the same as before do some drawing */
    if(nold != n)
    {
      /* paint unselected the option */
      e_pr_str_wsd(opt[nold].x, 0, opt[nold].t, f->fb->mt.fb, 0, 1,
                   f->fb->ms.fb, (nold == 0 ? 0 : opt[nold].x - e_mn_men),
       (nold == MENOPT - 1) ? MAXSCOL - 1 : opt[nold + 1].x - e_mn_men - 1);

      /* paint selected the option */
      e_pr_str_wsd(opt[n].x, 0, opt[n].t, f->fb->mz.fb, 0, 1,
                   f->fb->mz.fb, (n == 0 ? 0 : opt[n].x - e_mn_men),
             (n == MENOPT - 1) ? MAXSCOL - 1 : opt[n + 1].x - e_mn_men - 1);

      /* store the selected menu option for later use */
      nold = n;
    }

    if(c == WPE_CR)
    {
      e_hlp = e_menu_help_line(n);
      /* Handle user interaction with the submenu.
         If the user moved to another main option, the function returns. */
      if(mainmenu[n].width != 0)
        c = WpeHandleSubmenu(opt[n].x + mainmenu[n].position, 1,
                             opt[n].x + mainmenu[n].width + mainmenu[n].position, 
                             mainmenu[n].no_of_items + 2, n, mainmenu[n].menuitems, f);
      if(c < MENOPT)
      {
        n = c;
        c = WPE_CR;
      }
      else if(c != WPE_ESC)
        c = 0;
    }
    else
    {
      e_hlp = e_menu_help_line(E_MENU_HELP_BAR);
      c = e_toupper(WpeMainmenuReadKey());
    }
    if(c == CDO || c == CtrlN)        /* down -> submenu open */
      c = WPE_CR;
    else if(c == CLE || c == CtrlB)   /* going left in the main menu */
      n--;
    else if(c == CRI || c == CtrlF)   /* going right in the main menu */
      n++;
    else if(c == POS1 || c == CtrlA)  /* most left in the main menu */
      n = 0;
    else if(c == ENDE || c == CtrlE)  /* most right in the main menu */
      n = MENOPT - 1;
    else if(c == AltX)                /* quit the whole business */
      c = e_quit(f);

    /* adjust the selected main menu */
    if(n < 0)
      n = MENOPT - 1;
    else if(n >= MENOPT)
      n = 0;
  }

  /* paint unselected the last selected main menu option */
  f = cn->f[cn->mxedt];
  e_pr_str_wsd(opt[nold].x, 0, opt[nold].t, f->fb->mt.fb, 0, 1,
               f->fb->ms.fb, (nold == 0 ? 0 : opt[nold].x - e_mn_men),
       (nold == MENOPT - 1) ? MAXSCOL - 1 : opt[nold + 1].x - e_mn_men - 1);

  /* free up the submenu structure */
  e_menu_free(mainmenu, count);

  fk_cursor(1);
  return(c);
}

/* A row whose hotkey is this is a divider: a blank or dashed line that
   separates two groups of rows and runs nothing. */
#define WPE_SUBMENU_NO_HOTKEY 0

/* What e_m2_mouse returns for a click on a divider row (that row's hotkey). */
#define WPE_SUBMENU_NO_KEY 0

/**
 * WpeSubmenuHangsFromBar - Does this dropdown belong to a menu-bar entry?
 * @nm: the bar index the dropdown was opened with.
 *
 * The top File/Edit/... menus do: Left and Right move to the neighbouring
 * menu and the Alt form of a row letter is the row's advertised shortcut
 * ("Go to Line  Alt G").  The assistant's Alt-G dropdown does not, and the
 * engine must not borrow either behaviour for it.
 */
static int WpeSubmenuHangsFromBar(int nm)
{
  return nm != WPE_SUBMENU_NO_BAR_MENU;
}

/**
 * WpeSubmenuRowSelectable - Can the highlight rest on row @i?
 * @fopt: the rows; @i: a row index.
 *
 * A divider is drawn like a row but is not one the user can pick, so the
 * arrow keys step over it and Enter and the mouse never run it.
 */
static int WpeSubmenuRowSelectable(OPTK *fopt, int i)
{
  return fopt[i].o != WPE_SUBMENU_NO_HOTKEY;
}

/* The stepping rule itself lives in we_menurows.c, where it is unit tested;
   this adapter answers its question from the engine's own rows. */
static int WpeSubmenuPick(const void *rows, int i)
{
  return WpeSubmenuRowSelectable((OPTK *)rows, i);
}

/**
 * WpeSubmenuStepRow - The next selectable row from @from in direction @step.
 * @fopt: the rows; @count: how many; @from: the current row; @step: +1 or -1.
 *
 * Wraps at both ends and steps over a divider (wpe_menu_step_row).  With no
 * selectable row at all the highlight stays where it is.
 */
static int WpeSubmenuStepRow(OPTK *fopt, int count, int from, int step)
{
  return wpe_menu_step_row(fopt, count, from, step, WpeSubmenuPick);
}

/**
 * WpeSubmenuFirstRow - The first selectable row (where Home and the open go).
 * @fopt: the rows; @count: how many.
 */
static int WpeSubmenuFirstRow(OPTK *fopt, int count)
{
  return wpe_menu_first_row(fopt, count, WpeSubmenuPick);
}

/**
 * WpeSubmenuLastRow - The last selectable row (where End goes).
 * @fopt: the rows; @count: how many.
 */
static int WpeSubmenuLastRow(OPTK *fopt, int count)
{
  return wpe_menu_last_row(fopt, count, WpeSubmenuPick);
}

/**
 * WpeSubmenuAltFormOf - The Alt-letter key a row's hotkey stands for.
 * @hk: the row's hotkey letter.
 * Return: the Alt code (AltG for 'g' or 'G'), or 0 when the hotkey is not a letter.
 */
static int WpeSubmenuAltFormOf(int hk)
{
  if(hk >= 'A' && hk <= 'Z')
    return e_tast_sim(hk - 'A' + 'a');
  if(hk >= 'a' && hk <= 'z')
    return e_tast_sim(hk);
  return 0;
}

/**
 * WpeSubmenuRowAltShortcut - The Alt form of a row letter, when it is a shortcut here.
 * @nm: the bar index the dropdown was opened with; @hk: the row's hotkey.
 *
 * A bar menu's row advertises its Alt form ("Go to Line  Alt G"), so Alt-G
 * runs it with the menu open.  A dropdown with no bar menu advertises a
 * prefix and a letter ("Alt-G g"), so its Alt form is no shortcut: 0 here.
 */
static int WpeSubmenuRowAltShortcut(int nm, int hk)
{
  if(WpeSubmenuHangsFromBar(nm))
    return WpeSubmenuAltFormOf(hk);
  return 0;
}

/**
 * WpeSubmenuKeyIsRowAltForm - Is @c the Alt form of one of this dropdown's letters?
 * @fopt: the rows; @count: how many; @c: the key read.
 *
 * Used for a dropdown with no bar menu: such a key (Alt-G while the Alt-G
 * menu is open, Alt-A, Alt-C, ...) is neither a row's shortcut nor an
 * editor command the user meant, so the dropdown keeps the keyboard and
 * stays as it is instead of running the prefix handler a second time.
 */
static int WpeSubmenuKeyIsRowAltForm(OPTK *fopt, int count, int c)
{
  int i;
  for(i = 0; i < count; i++)
    if(WpeSubmenuRowSelectable(fopt, i) && c == WpeSubmenuAltFormOf(fopt[i].o))
      return 1;
  return 0;
}

/**
 * WpeSubmenuKeyIsIgnored - Should the dropdown stay as it is on key @c?
 * @nm: the bar index the dropdown was opened with; @prefix: the key that
 * opened it (0 when none); @fopt, @count: the rows; @c: the key read.
 *
 * True for a click on a divider (it carries no key), and, in a dropdown with
 * no bar menu, for the prefix pressed again (Alt-Q with the Alt-Q menu open)
 * and for the Alt form of a row letter.  The user sees the dropdown
 * unchanged, the highlight where it was.
 */
static int WpeSubmenuKeyIsIgnored(int nm, int prefix, OPTK *fopt, int count, int c)
{
  if(c == WPE_SUBMENU_NO_KEY)
    return 1;
  if(WpeSubmenuHangsFromBar(nm))
    return 0;
  if(prefix && c == prefix)
    return 1;                   /* the prefix that opened this dropdown, again */
  return WpeSubmenuKeyIsRowAltForm(fopt, count, c);
}

/* What WpeSubmenuOpenBox highlights when the dropdown opens: no row yet,
   the loop draws the first highlight itself. */
#define WPE_SUBMENU_NO_HIGHLIGHT (-1)

/**
 * WpeSubmenuOpenBox - Draw the dropdown over the desk.
 * @xa, @ya, @xe, @ye: the box; @n: the row drawn highlighted, or
 * WPE_SUBMENU_NO_HIGHLIGHT; @fopt: the rows; @f: the current window.
 *
 * Saves what is behind the box first, so closing it gives the user back
 * the windows as they were.  Used when the dropdown opens, when a key it
 * passed on to the editor has run, and after a resize.
 * Return: the saved background, or NULL when there is no memory for it.
 */
static PIC *WpeSubmenuOpenBox(int xa, int ya, int xe, int ye, int n,
                              OPTK * fopt, FENSTER * f)
{
  PIC *pic;
  int  i;
  int  inset = e_menu_box_inset();

  pic = e_open_view(xa + inset, ya, xe - inset, ye, f->fb->mt.fb, 1);
  if(pic == NULL)
    return(NULL);
  e_std_rahmen(xa + 1, ya, xe - 1, ye, NULL, 0, f->fb->mr.fb, 0);
  for(i = ya + 1; i < ye; i++)
    e_pr_str_scan(xa + 3, i, fopt[i - ya - 1].t, f->fb->mt.fb, fopt[i - ya - 1].x, 1,
                  f->fb->ms.fb, xa + 2, xe - 2);
  if(n != WPE_SUBMENU_NO_HIGHLIGHT)
    e_pr_str_scan(xa + 3, n + ya + 1, fopt[n].t, f->fb->mz.fb, fopt[n].x, 1,
                  f->fb->mz.fb, xa + 2, xe - 2);
  return(pic);
}

/**
 * WpeSubmenuFits - Does the dropdown still fit on the screen?
 * @xe, @ye: its right and bottom edges.
 *
 * After the terminal or the window shrank, a dropdown hanging from the bar
 * may reach past the status line; it is then closed rather than drawn
 * over cells the screen no longer has.
 */
static int WpeSubmenuFits(int xe, int ye)
{
  return(xe < MAXSCOL && ye < MAXSLNS - 1);
}

/**
 * WpeSubmenuAfterResize - Lay the dropdown out again for the resized screen.
 * @pic: the background saved before the resize; the other arguments as
 * WpeSubmenuOpenBox.
 *
 * The display has drawn the desk again at the new size before the dropdown
 * hears of the resize, so the cells saved behind the box are the old
 * screen's: they are dropped, never put back, and the box is drawn again
 * over the new desk with the highlight where it was.
 * Return: the new background, or NULL when the dropdown no longer fits (it
 * is then closed).
 */
static PIC *WpeSubmenuAfterResize(PIC *pic, int xa, int ya, int xe, int ye,
                                  int n, OPTK * fopt, FENSTER * f)
{
  e_close_view(pic, 0);
  if(!WpeSubmenuFits(xe, ye))
    return(NULL);
  return(WpeSubmenuOpenBox(xa, ya, xe, ye, n, fopt, f));
}

/* What the dropdown returns when it closed without a choice (Esc). */
#define WPE_SUBMENU_CLOSED 255

/* sub menu box; @prefix is the key that opened a dropdown hanging from no
   bar menu (0 for the bar menus) */
static int WpeHandleSubmenu_run(int xa, int ya, int xe, int ye, int nm, int prefix,
                                OPTK * fopt, FENSTER * f)
{
#if MOUSE
  extern struct mouse e_mouse;
  extern int      e_mn_men;
#endif
  PIC            *pic;
  int             count = ye - ya - 1;      /* rows in the box, dividers included */
  int             i, n, nold = 1, c = 0;
  extern OPT      opt[];

  /* the highlight opens on the first row the user can pick */
  n = WpeSubmenuFirstRow(fopt, count);
  if(nold == n)
    nold = n + 1;

  /* save whatever will be behind the submenu, then draw the box with no
     row highlighted yet */
  pic = WpeSubmenuOpenBox(xa, ya, xe, ye, WPE_SUBMENU_NO_HIGHLIGHT, fopt, f);
  if(pic == NULL)
  {
    e_error(e_msg[ERR_LOWMEM], 0, f->fb);
    return(WPE_ESC);
  }

#if MOUSE
  /* show the actual changes */
  e_refresh();
  /* until mouse button released */
  while(e_mshit() != 0)
  {
    c = -1;
    /* mouse is in the range of the submenu */
    if(e_mouse.y > ya && e_mouse.y < ye)
    {
      i = e_mouse.y - ya - 1;
      /* a divider under the pointer is not a row: the highlight stays where
         it is (nold may still be the open's "nothing drawn yet" marker) */
      if(WpeSubmenuRowSelectable(fopt, i))
        n = i;
      /* if a new item is selected */
      if(nold != n)
      {
        /* check whether nold is in the range of the available items and unselect it */
        if(nold < ye - ya - 1 && nold >= 0)
          e_pr_str_scan(xa + 3, nold + ya + 1, fopt[nold].t, f->fb->mt.fb, fopt[nold].x,
                        1, f->fb->ms.fb, xa + 2, xe - 2);
        /* draw the newly selected item */
        e_pr_str_scan(xa + 3, n + ya + 1, fopt[n].t, f->fb->mz.fb, fopt[n].x, 1,
                      f->fb->mz.fb, xa + 2, xe - 2);
        /* save the selection */
        nold = n;
        e_refresh();
      }
    }
    /* mouse is in the main menu area */
    else if(e_mouse.y == 0)
    {
      /* search for the selected main menu option */
      for(i = 1; i < MENOPT; i++)
        if(e_mouse.x < opt[i].x - e_mn_men)
          break;
      /* check whether a new menu option has been selected */
      if(i != nm + 1)
      {
        /* if yes, restore what was behind */
        e_close_view(pic, 1);
        return(i - 1);
      }
    }
  }

  /* there was a selection and mouse is within the submenu !!! */
  if(c < 0 && e_mouse.y > ya && e_mouse.y < ye && e_mouse.x > xa &&
     e_mouse.x < xe)
    c = MBKEY;
#endif

  while(c != WPE_ESC)
  {
    if(nold != n)
    {
      /* check whether nold is in the range of the available items and unselect it */
      if(nold < ye - ya - 1 && nold >= 0)
        e_pr_str_scan(xa + 3, nold + ya + 1, fopt[nold].t, f->fb->mt.fb, fopt[nold].x,
                      1, f->fb->ms.fb, xa + 2, xe - 2);
      /* draw the newly selected item */
      e_pr_str_scan(xa + 3, n + ya + 1, fopt[n].t, f->fb->mz.fb, fopt[n].x, 1,
                    f->fb->mz.fb, xa + 2, xe - 2);
      /* save the selection */
      nold = n;
    }
#if MOUSE
    if(c != MBKEY)
    {
      if((c = e_toupper(WpeMenuGetch())) == -1)
        c = e_m2_mouse(xa, ya, xe, ye, fopt);
    }
    else
      c = WPE_CR;  /* mouse released at a proper place, submenu item accepted */
#else
    c = e_toupper(WpeMenuGetch());
#endif

    if(c == WPE_RESIZE)
    {
      pic = WpeSubmenuAfterResize(pic, xa, ya, xe, ye, n, fopt, f);
      if(pic == NULL)
        return(WPE_SUBMENU_CLOSED);
      nold = n;
      continue;
    }

    /* check main menu shortcut keys */
    for(i = 0; i < MENOPT; i++)
      if(c == opt[i].as)
      {
        e_close_view(pic, 1);
        return(i);
      }

    /* check submenu items' shortcut keys.  Match the plain hotkey letter
       (e.g. 'g' for "Go to Line") and, in a bar menu, its Alt-<letter> form,
       so the shortcut the item advertises (e.g. "Go to Line  Alt G") works
       with the menu open, not only the bare letter.  A dropdown with no bar
       menu advertises a prefix and a letter instead, so there the Alt form
       is no shortcut (WpeSubmenuRowAltShortcut returns 0 for it). */
    for(i = 0; i < count; i++)
    {
      int hk = fopt[i].o;
      int alt = WpeSubmenuRowAltShortcut(nm, hk);
      if(!WpeSubmenuRowSelectable(fopt, i))
        continue;
      if(c == hk || (alt && c == alt))
      {
        e_close_view(pic, 1);
        fopt[i].fkt(f);
        return(WPE_ESC);
      }
    }

    if(c == Alt0)
      c = WPE_ESC;

    if(i > ye - ya)
      c = WPE_ESC;
    else if(c == WPE_CR)     /* submenu item accepted */
    {
      if(!WpeSubmenuRowSelectable(fopt, n))
        continue;                       /* a divider runs nothing */
      e_close_view(pic, 1);
      fopt[n].fkt(f);
      return(WPE_ESC);
    }
    else if(c == CUP || c == CtrlP)     /* going up in the submenu */
      n = WpeSubmenuStepRow(fopt, count, n, -1);
    else if(c == CDO || c == CtrlN)     /* going down in the submenu */
      n = WpeSubmenuStepRow(fopt, count, n, 1);
    else if(c == CLE || c == CtrlB)     /* going left in the main menu */
    {
      if(!WpeSubmenuHangsFromBar(nm))
        continue;                       /* no neighbouring menu: stay */
      c = nm > 0 ? nm - 1 : MENOPT - 1;
      break;
    }
    else if(c == CRI || c == CtrlF)     /* going right in the main menu */
    {
      if(!WpeSubmenuHangsFromBar(nm))
        continue;                       /* no neighbouring menu: stay */
      c = nm < MENOPT - 1 ? nm + 1 : 0;
      break;
    }
    else if(c == POS1 || c == CtrlA)    /* to the top in the submenu */
      n = WpeSubmenuFirstRow(fopt, count);
    else if(c == ENDE || c == CtrlE)    /* to the bottom in the submenu */
      n = WpeSubmenuLastRow(fopt, count);
    else if(WpeSubmenuKeyIsIgnored(nm, prefix, fopt, count, c))
      continue;                         /* the dropdown stays as it is */
    else
    {
      /* this is the anything else case, ESC, keys which are not listed in
         the submenu */
      e_close_view(pic, 1);
      /* Fall back to the SAME global-shortcut dispatch the editor uses, so a
         Run/Compile/Debug shortcut (Alt-U Run, Alt-M Make, Ctrl-F9, ...) works
         with the menu open too -- e_tst_dfkt alone does not know the prog
         shortcuts (those live in e_prog_switch).  Mirrors we_edit.c. */
      if(c != WPE_ESC && e_menu_key_to_editor(f, c) == 0)
        return(WPE_ESC);
      /* the key was not the editor's either: the dropdown comes back */
      pic = WpeSubmenuOpenBox(xa, ya, xe, ye, n, fopt, f);
      if(pic == NULL)
        return(WPE_SUBMENU_CLOSED);
    }
  }
  e_close_view(pic, 1);
  return(c == WPE_ESC ? WPE_SUBMENU_CLOSED : c);
}

/* WpeHandleSubmenu - public entry to the dropdown engine (top File/Edit menus and
   the Alt-Q language-server menu).  The dropdown owns the screen for its whole
   lifetime, so output, a stop or a server update arriving while a menu is open
   waits for it to close instead of drawing under the box.  The box pushes no
   window, so it tells the screen's owner (we_paint.c) instead. */
int WpeHandleSubmenu(int xa, int ya, int xe, int ye, int nm, OPTK * fopt, FENSTER * f)
{
  int ret;
  wpe_paint_modal_enter("a dropdown");
  wpe_test_menu_enter(nm, fopt, ye - ya - 1);
  ret = WpeHandleSubmenu_run(xa, ya, xe, ye, nm, 0, fopt, f);
  wpe_test_menu_leave();
  wpe_paint_modal_leave();
  return(ret);
}

/**
 * WpeHandleSubmenuFromPrefix - Run a dropdown opened by a key prefix.
 * @xa, @ya, @xe, @ye: the box; @prefix: the key that opened it (AltG, AltQ);
 * @fopt: the rows; @f: the current window.
 * Return: a menu-bar index when the user pressed that menu's Alt letter
 * (the caller then opens that menu), anything else when a row ran or the
 * box was dismissed.
 *
 * Used by the assistant's Alt-G menu and the language server's Alt-Q menu.
 * Their rows advertise "<prefix> <letter>", so pressing the prefix again,
 * or the Alt form of a row letter, leaves the dropdown as it is, and Left
 * and Right have no neighbouring menu to move to.
 */
int WpeHandleSubmenuFromPrefix(int xa, int ya, int xe, int ye, int prefix,
                               OPTK * fopt, FENSTER * f)
{
  int ret;
  wpe_paint_modal_enter("a prefix's dropdown");
  wpe_test_menu_enter(WPE_SUBMENU_NO_BAR_MENU, fopt, ye - ya - 1);
  ret = WpeHandleSubmenu_run(xa, ya, xe, ye, WPE_SUBMENU_NO_BAR_MENU, prefix, fopt, f);
  wpe_test_menu_leave();
  wpe_paint_modal_leave();
  return(ret);
}

/*      fill "options" struct */
OPTK WpeFillSubmenuItem(char *t, int x, char o, int(*fkt) ())
{
  OPTK            opt;

  opt.t = t;
  opt.x = x;
  opt.o = o;
  opt.fkt = fkt;
  return(opt);
}

