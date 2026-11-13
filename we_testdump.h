/* we_testdump.h -- the editor's state, written for a test each time the
   editor waits for a key.
   Copyright (C) 2026 Juan Manuel Méndez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later.

   A test that drives the editor through a terminal sees only the screen,
   and the screen is drawn when the editor gets to it.  With
   $XWPE_TEST_DUMP_FD naming a descriptor open for writing (a pipe the test
   reads), the editor writes there, just before it blocks for a key, what it
   holds: how many keys it has taken, where the keys are read (the text, a
   prefix, the menu bar, a dialog, a message, the File Manager), every text
   window's lines, cursor, marks, modified count and undo depth, and the
   search strings.  The test then knows the editor has finished with the
   keys it sent, without waiting a fixed time, and reads the state without
   saving the file.

   Unset, nothing is installed: the variable is read once at start and the
   key reader is the display's own.

   The format, one record per line, every field on one line:
     S <seq> ctx=<edit|prefix|menu|dialog:<title>|message|filemanager|window|box|wait> top=<n> autosaved=<0|1>
     W <n> <name> ro=<0|1> mx=<width> y=<n> x=<n> mb=<y>,<x> me=<y>,<x> ks=<y>,<x>
       ins=<n> flg=<n> save=<n> view=<c.y>,<c.x>,<a.y>,<a.x>,<e.y>,<e.x>
       pt=<x>,<y>/<x>,<y>/... brp=<row>,<row>|- ud=<steps> rd=<steps>
       rows=<first row wider than the margin or not ended as a row ends, -1>
       pos=<-1, or the first position that is not a place: 0 cursor, 1 block
            begin, 2 block end, 3 find start, 4 find end, 5 anchor>
     L <n> <H|S> <bytes>          (one per line of window n; H ends with a
                                   line break, S is a soft row)
     F <search> <replace> <sw>
     E <seq>
   Bytes outside printable ASCII, and the backslash, are written \xHH; in
   the W name and the F fields a space is written \x20 as well. */

#ifndef WE_TESTDUMP_H
#define WE_TESTDUMP_H

/* Where the key the editor waits for is read, as the dump names it. */
typedef enum
{
 WPE_TEST_CTX_PREFIX,     /* the letter after Ctrl-K or Ctrl-O */
 WPE_TEST_CTX_MENU,       /* the menu bar or one of its dropdowns */
 WPE_TEST_CTX_DIALOG,     /* a dialog box: its title is named */
 WPE_TEST_CTX_MESSAGE     /* a message or error box */
} WpeTestCtx;

void wpe_test_dump_from_env(void);
int  wpe_test_dump_on(void);
void wpe_test_dump_idle(void);
void wpe_test_ctx_enter(WpeTestCtx kind, const char *title);
void wpe_test_ctx_leave(void);
void wpe_test_dump_note_autosave(void);

#endif /* WE_TESTDUMP_H */
