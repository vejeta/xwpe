/* test_winkind.c -- unit test for the owner of "which kind of window is
 * this, and what does it hold" (we_winkind.c).  Always runs; run it under
 * ASan+UBSan+LSan too.
 *
 * What the user sees behind each group:
 *  - a command asked of a window that holds no text is refused, and the
 *    refusal names the command;
 *  - a window's payload is only ever what its constructor gave it, so a
 *    list's rows are never read as a file's text and the other way round;
 *  - a window the editor opened itself is known by the intent that opened
 *    it, so a build finds its own pane and never a file the user happened
 *    to name after it;
 *  - a window that is closed leaves nothing behind, so a long session does
 *    not slowly run out of memory.
 *
 * The stand-ins below are never written through: the owner only stores and
 * returns their addresses, so an address is enough to tell one payload from
 * another.
 */
#include <stdio.h>
#include "linebuf.h"
#include <stdlib.h>
#include <string.h>

#include "../edit.h"
#include "../we_winkind.h"

static int fails;

/* The refusal's voice is a hook the editor gives the owner at start, so this
   test gives its own and records what it was asked to say: a check can then
   assert both that a window without text was refused and which command the
   refusal named, without linking the closed-window owner. */
static int said_count;
static char said_action[64];

static void record_refusal(FENSTER *f, const char *action)
{
 (void)f;
 said_count++;
 snprintf(said_action, sizeof(said_action), "%s", action ? action : "(none)");
}

static void chk_int(const char *what, long got, long want)
{
 if (got != want)
 {
  printf("FAIL: %s: got %ld want %ld\n", what, got, want);
  fails++;
 }
}

static void chk_ptr(const char *what, const void *got, const void *want)
{
 if (got != want)
 {
  printf("FAIL: %s: got %p want %p\n", what, got, want);
  fails++;
 }
}

/* Stand-ins for the four payloads.  Only their addresses matter. */
static BUFFER stand_in_text;
static SCHIRM stand_in_screen;
static FLBFFR stand_in_files;
static FLWND stand_in_list;
static BUFFER other_text;
static BUFFER pane_text;

/* An insert mode no kind of list has: a typing mode of text. */
#define INSERT_MODE_BEFORE_HOLD 5

static FENSTER win;
static ECNT desk;

static void window_reset(void)
{
 /* A check that leaves a block behind frees it here, so the sanitizer run
    sees the owner's own accounting and not the test's leftovers. */
 e_window_drop(&win);
 memset(&win, 0, sizeof(win));
 win.ed = &desk;
 win.datnam = (char *)"a-file.c";
 /* What a constructor may have written before it holds: a list's kind used
    to be stored here, and the owner must leave a window without text at 0. */
 win.ins = INSERT_MODE_BEFORE_HOLD;
}

static void desk_reset(void)
{
 memset(&desk, 0, sizeof(desk));
 desk.mxedt = -1;
}

/* Every accessor's answer for a window that holds no payload at all. */
static void chk_holds_nothing_of(const char *what, const FENSTER *f)
{
 chk_int(what, e_window_holds(f), WPE_HOLDS_NOTHING);
 chk_int("nothing is text", e_window_is_text(f), 0);
 chk_ptr("nothing has no text", e_window_text(f), NULL);
 chk_ptr("nothing has no view state", e_window_screen(f), NULL);
 chk_ptr("nothing has no file lists", e_window_files(f), NULL);
 chk_ptr("nothing has no rows", e_window_list(f), NULL);
 chk_int("nothing is no list kind", e_window_list_kind(f), WPE_LISTKIND_NONE);
}

static void chk_holds_nothing(const char *what)
{
 chk_holds_nothing_of(what, &win);
}

/* A text window answers with its own text and view state, and with nothing
   of the other three kinds. */
static void chk_only_text(const char *what)
{
 chk_int(what, e_window_holds(&win), WPE_HOLDS_TEXT);
 chk_int("text is text", e_window_is_text(&win), 1);
 chk_ptr("text answers its own buffer", e_window_text(&win), &stand_in_text);
 chk_ptr("text answers its own view state", e_window_screen(&win),
         &stand_in_screen);
 chk_ptr("text has no file lists", e_window_files(&win), NULL);
 chk_ptr("text has no rows", e_window_list(&win), NULL);
 chk_int("text is no list kind", e_window_list_kind(&win), WPE_LISTKIND_NONE);
}

/* A File Manager answers with its lists and with no text at all: this is the
   pair of answers that used to be one field read two ways. */
static void chk_only_files(const char *what)
{
 chk_int(what, e_window_holds(&win), WPE_HOLDS_FILES);
 chk_int("the File Manager is not text", e_window_is_text(&win), 0);
 chk_ptr("the File Manager has no text", e_window_text(&win), NULL);
 chk_ptr("the File Manager has no view state", e_window_screen(&win), NULL);
 chk_ptr("the File Manager answers its lists", e_window_files(&win),
         &stand_in_files);
 chk_ptr("the File Manager has no rows", e_window_list(&win), NULL);
}

static void chk_only_list(const char *what, WpeListKind kind)
{
 chk_int(what, e_window_holds(&win), WPE_HOLDS_LIST);
 chk_int("a list is not text", e_window_is_text(&win), 0);
 chk_ptr("a list has no text", e_window_text(&win), NULL);
 chk_ptr("a list has no view state", e_window_screen(&win), NULL);
 chk_ptr("a list has no file lists", e_window_files(&win), NULL);
 chk_ptr("a list answers its rows", e_window_list(&win), &stand_in_list);
 chk_int("a list answers which list it is", e_window_list_kind(&win), kind);
}

static void chk_only_dropdown(const char *what)
{
 chk_int(what, e_window_holds(&win), WPE_HOLDS_DROPDOWN);
 chk_int("a dropdown is not text", e_window_is_text(&win), 0);
 chk_ptr("a dropdown has no text", e_window_text(&win), NULL);
 chk_ptr("a dropdown answers its rows", e_window_list(&win), &stand_in_list);
 chk_int("a dropdown is no list kind", e_window_list_kind(&win),
         WPE_LISTKIND_NONE);
}

/* The agreement the state dump prints and the painters rely on: the owner's
   answer about text is the window's own mode character's answer, the view
   state is set exactly for a text window, and the list kind exactly for a
   list window.  The insert-mode clause: a window that is not text has insert
   mode 0, whatever its constructor wrote before it held its payload. */
static void chk_agreement(const char *what)
{
 int says_text = DTMD_ISTEXT(win.dtmd) ? 1 : 0;
 int kind_is_set = (e_window_list_kind(&win) != WPE_LISTKIND_NONE);
 int is_a_list = (e_window_holds(&win) == WPE_HOLDS_LIST);

 chk_int(what, e_window_is_text(&win), says_text);
 if (says_text)
 {
  chk_int("a text window has a view state", e_window_screen(&win) != NULL, 1);
 }
 else
 {
  chk_ptr("a window without text has no view state", e_window_screen(&win),
          NULL);
  chk_int("a window without text has insert mode 0", win.ins, 0);
 }
 /* The list kind is set exactly for a list window: a dropdown shows rows
    too, but it is no list of the editor's own, and a file window is neither. */
 chk_int("the list kind is set exactly for a list window", kind_is_set,
         is_a_list);
}

/* ---- what each constructor leaves behind ------------------------------ */

static void check_a_fresh_window_holds_nothing(void)
{
 window_reset();
 chk_holds_nothing("a window built and not yet given its contents");
}

static void check_hold_text(void)
{
 window_reset();
 win.dtmd = DTMD_NORMAL;
 chk_int("a text window is held",
         e_window_hold_text(&win, &stand_in_text, &stand_in_screen), 1);
 chk_only_text("after holding a file's text");
 chk_agreement("a text window agrees with its mode character");
}

static void check_hold_files(void)
{
 window_reset();
 win.dtmd = DTMD_FILEMANAGER;
 chk_int("a File Manager is held", e_window_hold_files(&win, &stand_in_files), 1);
 chk_only_files("after holding the File Manager's lists");
 chk_agreement("the File Manager agrees with its mode character");
}

static void check_hold_list(void)
{
 window_reset();
 win.dtmd = DTMD_DATA;
 chk_int("a project list is held",
         e_window_hold_list(&win, &stand_in_list,
                            WPE_LISTKIND_PROJECT_FILES), 1);
 chk_only_list("after holding the project's file list",
               WPE_LISTKIND_PROJECT_FILES);
 chk_agreement("a project list agrees with its mode character");
}

static void check_hold_every_list_kind(void)
{
 static const WpeListKind kinds[] = {
  WPE_LISTKIND_FUNCTIONS, WPE_LISTKIND_GREP, WPE_LISTKIND_FOUND,
  WPE_LISTKIND_PROJECT_FILES, WPE_LISTKIND_PROJECT_VARIABLES,
  WPE_LISTKIND_PROJECT_INSTALL, WPE_LISTKIND_WINDOWS
 };
 size_t i;

 for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
 {
  window_reset();
  win.dtmd = DTMD_DATA;
  e_window_hold_list(&win, &stand_in_list, kinds[i]);
  chk_int("each list kind is the one held", e_window_list_kind(&win), kinds[i]);
 }
}

static void check_hold_dropdown(void)
{
 window_reset();
 win.dtmd = DTMD_FILEDROPDOWN;
 win.winnum = -1;
 chk_int("a dropdown is held",
         e_window_hold_dropdown(&win, &stand_in_list), 1);
 chk_only_dropdown("after holding a dropdown's rows");
 chk_agreement("a dropdown agrees with its mode character");
}

/* The help and the info viewer take the pane's own buffer over the one just
   allocated: the old text goes back to its owner and the view state stays. */
static void check_replace_text_keeps_the_view(void)
{
 window_reset();
 win.dtmd = DTMD_HELP;
 e_window_hold_text(&win, &stand_in_text, &stand_in_screen);
 chk_int("the viewer's buffer replaces the allocated one",
         e_window_replace_text(&win, &other_text), 1);
 chk_ptr("the text is the pane's own now", e_window_text(&win), &other_text);
 chk_ptr("the view state is the one the reader was given",
         e_window_screen(&win), &stand_in_screen);
}

/* ---- the eight places that give a window its contents ------------------ */

/* One check per constructor of the editor's, named for the file that holds
   it and reproducing what that constructor does.  The owner's answer is only
   right for the pattern the site really uses, and three of the eight are
   patterns a plain "hold a text" check does not cover: a window built with
   memset and no desktop at all, a viewer the editor opened itself and tagged
   as its own, and two viewers that take a fresh buffer over the one the
   window was allocated with. */

/* we_edit.c, e_edit_open: a file's window is allocated, told it holds
   nothing yet, then given its text and its view state, with the pane decided
   by the intent that opened it and never by the title it ends up carrying. */
static void check_the_file_window_constructor(void)
{
 window_reset();
 win.dtmd = DTMD_NORMAL;
 e_window_held_forget(&win);
 chk_int("the file's window holds its text",
         e_window_hold_text(&win, &stand_in_text, &stand_in_screen), 1);
 e_window_set_pane(&win, WPE_PANE_FILE);
 chk_only_text("the window of a file the user opened");
 chk_int("a file the user opened answers for no pane", e_window_pane(&win),
         WPE_PANE_FILE);
 chk_agreement("the file's window agrees with its mode character");
}

/* we_fl_unix.c, the File Manager: one window holds the directory lists, and
   nothing about it answers as text. */
static void check_the_file_manager_constructor(void)
{
 window_reset();
 win.dtmd = DTMD_FILEMANAGER;
 e_window_held_forget(&win);
 chk_int("the File Manager holds its lists",
         e_window_hold_files(&win, &stand_in_files), 1);
 chk_only_files("the File Manager's window");
 chk_agreement("the File Manager agrees with its mode character");
}

/* we_fl_unix.c, a list window: the rows and which list they are, the kind
   handed in by the caller's own switch rather than read back from the rows. */
static void check_the_list_window_constructor(void)
{
 window_reset();
 win.dtmd = DTMD_DATA;
 e_window_held_forget(&win);
 chk_int("the window list holds its rows",
         e_window_hold_list(&win, &stand_in_list, WPE_LISTKIND_WINDOWS), 1);
 chk_only_list("the list of the open windows", WPE_LISTKIND_WINDOWS);
 chk_agreement("a list window agrees with its mode character");
}

/* we_wind.c, a dropdown: local to the box that opened it, winnum -1, never
   pushed on the window stack, so no key loop is ever chosen for it. */
static void check_the_dropdown_constructor(void)
{
 window_reset();
 win.dtmd = DTMD_FILEDROPDOWN;
 win.winnum = -1;
 e_window_held_forget(&win);
 chk_int("the dropdown holds its rows",
         e_window_hold_dropdown(&win, &stand_in_list), 1);
 chk_only_dropdown("the dropdown of a box");
 chk_int("a dropdown is no window of the stack", win.winnum, -1);
 chk_agreement("a dropdown agrees with its mode character");
}

/* we_edit.c, the clipboard's viewer: cn->f[0] is a text window the editor
   opened itself and tagged as its clipboard pane.  Show Buffer puts it on
   the stack and takes it off again, so the tag and not the place is what
   says what it is. */
static void check_the_clipboard_constructor(void)
{
 window_reset();
 win.dtmd = DTMD_NORMAL;
 e_window_held_forget(&win);
 chk_int("the clipboard's window holds its text",
         e_window_hold_text(&win, &stand_in_text, &stand_in_screen), 1);
 e_window_set_pane(&win, WPE_PANE_CLIPBOARD);
 chk_only_text("the paste buffer shown as a window");
 chk_int("the clipboard answers as the pane the editor opened",
         e_window_pane(&win), WPE_PANE_CLIPBOARD);
 chk_int("the clipboard is not the build's own pane",
         e_window_pane(&win) == WPE_PANE_MESSAGES, 0);
}

/* we_buffer.c, a block delete's shell: a window built with memset, holding
   the lines a delete moved out of the text, with no desktop above it at all.
   The owner must answer from the window alone and never walk up to a desktop
   the shell does not have. */
static void check_the_shell_window_constructor(void)
{
 FENSTER shell;
 SCHIRM shell_screen;

 memset(&shell, 0, sizeof(shell));
 memset(&shell_screen, 0, sizeof(shell_screen));
 shell.ed = NULL;
 shell.dtmd = DTMD_NORMAL;
 chk_int("the shell's window holds its text",
         e_window_hold_text(&shell, &stand_in_text, &shell_screen), 1);
 chk_int("the shell is a text window", e_window_is_text(&shell), 1);
 chk_ptr("the shell answers its own text", e_window_text(&shell),
         &stand_in_text);
 chk_ptr("the shell answers its own view state", e_window_screen(&shell),
         &shell_screen);
 chk_ptr("the shell has no rows", e_window_list(&shell), NULL);
 chk_int("the shell answers for no pane", e_window_pane(&shell),
         WPE_PANE_FILE);
 e_window_drop(&shell);
 chk_holds_nothing_of("the shell after its block was freed", &shell);
}

/* we_fl_fkt.c, e_read_help: the help viewer closes the buffer the window was
   allocated with and takes a fresh one over it, keeping the view state the
   reader was given, so the window still holds text and the readers that still
   ask the legacy field read the buffer taken over it. */
static void check_the_help_viewer_constructor(void)
{
 window_reset();
 win.dtmd = DTMD_HELP;
 e_window_held_forget(&win);
 e_window_hold_text(&win, &stand_in_text, &stand_in_screen);
 e_window_set_pane(&win, WPE_PANE_HELP);
 chk_int("the help buffer replaces the allocated one",
         e_window_replace_text(&win, &pane_text), 1);
 chk_ptr("the help viewer shows its own buffer", e_window_text(&win),
         &pane_text);
 chk_ptr("the help viewer keeps the view state it was given",
         e_window_screen(&win), &stand_in_screen);
 chk_int("the help viewer is still a text window", e_window_is_text(&win), 1);
 chk_ptr("the help viewer has no rows", e_window_list(&win), NULL);
 chk_int("the help viewer is the pane the editor opened",
         e_window_pane(&win), WPE_PANE_HELP);
 chk_agreement("the help viewer agrees with its mode character");
}

/* we_fl_fkt.c, e_read_info: the info viewer does the same with its own
   buffer, over as many nodes as the user walks. */
static void check_the_info_viewer_constructor(void)
{
 SCHIRM *view = NULL;

 window_reset();
 win.dtmd = DTMD_HELP;
 e_window_held_forget(&win);
 e_window_hold_text(&win, &stand_in_text, &stand_in_screen);
 chk_int("the first node's buffer replaces the allocated one",
         e_window_replace_text(&win, &pane_text), 1);
 chk_int("the next node's buffer replaces that one",
         e_window_replace_text(&win, &other_text), 1);
 chk_ptr("the info viewer shows the node it walked to",
         e_window_text(&win), &other_text);
 chk_ptr("walking nodes keeps the one view state",
         e_window_release_text(&win, &view), &other_text);
 chk_ptr("the view state outlives every buffer taken over it", view,
         &stand_in_screen);
 chk_holds_nothing("after the viewer's last buffer was taken back");
}

/* ---- taking the payload back ------------------------------------------ */
static void check_release_text(void)
{
 SCHIRM *screen = NULL;
 BUFFER *text;

 window_reset();
 win.dtmd = DTMD_NORMAL;
 e_window_hold_text(&win, &stand_in_text, &stand_in_screen);
 text = e_window_release_text(&win, &screen);
 chk_ptr("a close gets the text back to free", text, &stand_in_text);
 chk_ptr("a close gets the view state back to free", screen, &stand_in_screen);
 chk_holds_nothing("after the text was taken back");
}

static void check_release_files_and_list(void)
{
 window_reset();
 win.dtmd = DTMD_FILEMANAGER;
 e_window_hold_files(&win, &stand_in_files);
 chk_ptr("a close gets the lists back to free",
         e_window_release_files(&win), &stand_in_files);
 chk_holds_nothing("after the lists were taken back");

 window_reset();
 win.dtmd = DTMD_DATA;
 e_window_hold_list(&win, &stand_in_list, WPE_LISTKIND_WINDOWS);
 chk_ptr("a close gets the rows back to free",
         e_window_release_list(&win), &stand_in_list);
 chk_holds_nothing("after the rows were taken back");
}

/* A release asked of the wrong kind gives nothing back, so a caller cannot
   free a payload it does not own. */
static void check_release_asks_the_kind(void)
{
 SCHIRM *screen = &stand_in_screen;

 window_reset();
 win.dtmd = DTMD_NORMAL;
 e_window_hold_text(&win, &stand_in_text, &stand_in_screen);
 chk_ptr("a list release of a text window gives nothing",
         e_window_release_list(&win), NULL);
 chk_ptr("a File Manager release of a text window gives nothing",
         e_window_release_files(&win), NULL);
 chk_ptr("the text is still held", e_window_text(&win), &stand_in_text);
 chk_ptr("a text release leaves the view state where it was told",
         e_window_release_text(&win, &screen), &stand_in_text);
 chk_ptr("and hands the view state over", screen, &stand_in_screen);
}

/* ---- the pane the editor opened itself -------------------------------- */

static void check_the_pane_is_the_intent(void)
{
 window_reset();
 win.dtmd = DTMD_NORMAL;
 e_window_hold_text(&win, &stand_in_text, &stand_in_screen);
 chk_int("a file the user opened is no pane", e_window_pane(&win),
         WPE_PANE_FILE);
 /* The window's title says "Messages" but the intent that opened it did not:
    a file of the user's own keeps its own text and stays writable. */
 win.datnam = (char *)"Messages";
 chk_int("a file named after a pane is still the user's file",
         e_window_pane(&win), WPE_PANE_FILE);
 e_window_set_pane(&win, WPE_PANE_MESSAGES);
 chk_int("the pane the editor opened is the one it says",
         e_window_pane(&win), WPE_PANE_MESSAGES);
}

/* A dialog is centred on the screen over a dock strip and on the window over
   any other: the read-only flag cannot say which, so the owner does. */
static void check_the_dock_panes(void)
{
 static const WpePane docks[] = { WPE_PANE_MESSAGES, WPE_PANE_WATCHES,
                                  WPE_PANE_STACK, WPE_PANE_ASSISTANT };
 static const WpePane others[] = { WPE_PANE_FILE, WPE_PANE_HELP,
                                   WPE_PANE_CLIPBOARD };
 size_t i;

 window_reset();
 win.dtmd = DTMD_NORMAL;
 e_window_hold_text(&win, &stand_in_text, &stand_in_screen);
 win.ins = WPE_INS_READONLY;
 chk_int("a locked file is no dock pane", e_window_is_dock_pane(&win), 0);
 for (i = 0; i < sizeof docks / sizeof docks[0]; i++)
 {
  e_window_set_pane(&win, docks[i]);
  win.ins = 0;
  chk_int("a dock pane is one whatever its read-only flag",
          e_window_is_dock_pane(&win), 1);
 }
 for (i = 0; i < sizeof others / sizeof others[0]; i++)
 {
  e_window_set_pane(&win, others[i]);
  win.ins = WPE_INS_READONLY;
  chk_int("a read-only window that is no dock pane is not one",
          e_window_is_dock_pane(&win), 0);
 }
}

/* ---- windows chosen by kind ------------------------------------------- */

static FENSTER stack_file;
static FENSTER stack_messages;
static FENSTER stack_project;

static void desk_of_three(void)
{
 desk_reset();
 e_window_drop(&stack_file);
 e_window_drop(&stack_messages);
 e_window_drop(&stack_project);
 memset(&stack_file, 0, sizeof(stack_file));
 memset(&stack_messages, 0, sizeof(stack_messages));
 memset(&stack_project, 0, sizeof(stack_project));
 stack_file.ed = &desk;
 stack_messages.ed = &desk;
 stack_project.ed = &desk;
 stack_file.dtmd = DTMD_NORMAL;
 stack_messages.dtmd = DTMD_NORMAL;
 stack_project.dtmd = DTMD_DATA;
 e_window_hold_text(&stack_file, &stand_in_text, &stand_in_screen);
 e_window_hold_text(&stack_messages, &other_text, &stand_in_screen);
 e_window_hold_list(&stack_project, &stand_in_list, WPE_LISTKIND_PROJECT_FILES);
 e_window_set_pane(&stack_messages, WPE_PANE_MESSAGES);
 desk.mxedt = 3;
 desk.f[1] = &stack_file;
 desk.f[2] = &stack_project;
 desk.f[3] = &stack_messages;
 desk.edt[1] = 1;
 desk.edt[2] = 2;
 desk.edt[3] = 3;
}

static void check_windows_chosen_by_kind(void)
{
 desk_of_three();
 chk_ptr("the top window is the one on top", e_window_top(&desk),
         &stack_messages);
 chk_ptr("the build steps over its own pane to the window the user was in",
         e_window_top_except_pane(&desk, WPE_PANE_MESSAGES), &stack_project);
 chk_ptr("the pane is found by what it is", 
         e_window_pane_find(&desk, WPE_PANE_MESSAGES), &stack_messages);
 chk_ptr("a pane that is not open is not found",
         e_window_pane_find(&desk, WPE_PANE_WATCHES), NULL);
 chk_ptr("the topmost window with text is found past a list",
         e_window_text_under(&desk), &stack_messages);

 /* The project list on top: the build still gives the keys back to it, and
    a search for text steps over it to the file below. */
 desk.f[3] = &stack_project;
 desk.f[2] = &stack_messages;
 chk_ptr("a build from the project list returns to the project list",
         e_window_top_except_pane(&desk, WPE_PANE_MESSAGES), &stack_project);
 chk_ptr("text is found below the project list", e_window_text_under(&desk),
         &stack_messages);

 /* Nothing but the pane open: the build has no window of the user's to
    return to. */
 desk.mxedt = 1;
 desk.f[1] = &stack_messages;
 chk_ptr("with only the pane open there is no window to return to",
         e_window_top_except_pane(&desk, WPE_PANE_MESSAGES), NULL);
 desk.mxedt = -1;
 chk_ptr("with no window open there is no top", e_window_top(&desk), NULL);
 chk_ptr("with no window open there is no text", e_window_text_under(&desk),
         NULL);
}

static void check_the_project_list_slot(void)
{
 chk_int("the project's files are the first remembered list",
         e_project_list_slot(WPE_LISTKIND_PROJECT_FILES), 0);
 chk_int("the project's variables are the second",
         e_project_list_slot(WPE_LISTKIND_PROJECT_VARIABLES), 1);
 chk_int("the project's install rows are the third",
         e_project_list_slot(WPE_LISTKIND_PROJECT_INSTALL), 2);
 chk_int("the window list has no remembered slot",
         e_project_list_slot(WPE_LISTKIND_WINDOWS), -1);
 chk_int("a search result list has no remembered slot",
         e_project_list_slot(WPE_LISTKIND_FOUND), -1);
 chk_int("no list at all has no remembered slot",
         e_project_list_slot(WPE_LISTKIND_NONE), -1);
}

/* ---- the refusal a command gets --------------------------------------- */

static void check_a_command_refused_names_the_command(void)
{
 desk_of_three();
 said_count = 0;
 said_action[0] = '\0';
 desk.mxedt = 2;                    /* the project list on top */
 chk_ptr("a text command asked of a list is refused",
         e_action_text_window(&stack_file, "Highlight uses"), NULL);
 chk_int("the refusal is said once", said_count, 1);
 chk_int("the refusal names the command",
         strcmp(said_action, "Highlight uses") == 0, 1);

 said_count = 0;
 desk.mxedt = 1;                    /* a file window on top */
 chk_ptr("a text command asked of a file window goes ahead",
         e_action_text_window(&stack_file, "Highlight uses"), &stack_file);
 chk_int("a command that goes ahead says nothing", said_count, 0);

 said_count = 0;
 desk.mxedt = -1;                   /* no window at all */
 chk_ptr("a text command with no window open is refused",
         e_action_text_window(&stack_file, NULL), NULL);
 chk_int("the refusal is said once", said_count, 1);
 chk_int("a command without a name of its own is still refused",
         strcmp(said_action, "(none)") == 0, 1);
}

/* ---- the count a key loop waits on ------------------------------------ */

static void check_the_top_count_moves(void)
{
 unsigned long before = e_window_top_count();

 chk_int("the count stands still until the stack is told",
         e_window_top_count() == before, 1);
 e_window_stack_changed();
 chk_int("a window pushed, popped or raised moves the count",
         e_window_top_count() == before + 1, 1);
 e_window_stack_changed();
 chk_int("a second change moves it again",
         e_window_top_count() == before + 2, 1);
}

/* ---- a window that is closed leaves nothing behind --------------------- */

static void check_drop_leaves_nothing_behind(void)
{
 window_reset();
 win.dtmd = DTMD_NORMAL;
 e_window_hold_text(&win, &stand_in_text, &stand_in_screen);
 e_window_drop(&win);
 chk_holds_nothing("after the window's block was freed");
 /* A second drop, and a drop of a window that never held anything, are both
    safe: a close path that runs twice must not free twice. */
 e_window_drop(&win);
 window_reset();
 e_window_drop(&win);
}

int main(void)
{
 e_window_refusal_voice_set(record_refusal);

 check_a_fresh_window_holds_nothing();
 check_hold_text();
 check_hold_files();
 check_hold_list();
 check_hold_every_list_kind();
 check_hold_dropdown();
 check_replace_text_keeps_the_view();
 check_the_file_window_constructor();
 check_the_file_manager_constructor();
 check_the_list_window_constructor();
 check_the_dropdown_constructor();
 check_the_clipboard_constructor();
 check_the_shell_window_constructor();
 check_the_help_viewer_constructor();
 check_the_info_viewer_constructor();
 check_release_text();
 check_release_files_and_list();
 check_release_asks_the_kind();
 check_the_pane_is_the_intent();
 check_the_dock_panes();
 check_windows_chosen_by_kind();
 check_the_project_list_slot();
 check_a_command_refused_names_the_command();
 check_the_top_count_moves();
 check_drop_leaves_nothing_behind();

 /* Nothing the checks built outlives them: the sanitizer run reports a leak
    in the owner, not one the test left behind. */
 window_reset();
 e_window_drop(&stack_file);
 e_window_drop(&stack_messages);
 e_window_drop(&stack_project);

 if (fails == 0)
  printf("test_winkind: all checks passed\n");
 else
  printf("test_winkind: %d check(s) failed\n", fails);
 return(fails == 0 ? 0 : 1);
}
