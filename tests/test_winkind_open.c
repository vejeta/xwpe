/* test_winkind_open.c -- unit test for the one finder of windows
 * (e_window_find in we_winkind.c).  Always runs; run it under ASan+UBSan too.
 *
 * What the user sees behind each group:
 *  - a file the user names like one of the editor's own windows (Messages,
 *    Watches, Stack, Help, Buffer, AI, a report, a manual page) is the
 *    user's file: asking for the file never answers with the editor's
 *    window, and asking for the editor's window never answers with the
 *    user's file;
 *  - the clipboard is never a file the user opened, and a new unnamed file
 *    is always a window of its own;
 *  - a label stays whole, with a '/' in it, so a manual page asked for as
 *    "a/b" is found again by "a/b";
 *  - every question about a file's name is answered no for a pane.
 *
 * The table is walked whole, not sampled: every kind of window, against a
 * desk with nothing open, with a file of its title, with its own window,
 * with both, and with a list window of its title.  The stand-ins are never
 * written through: the owner stores and compares them.
 */
#include <stdio.h>
#include "linebuf.h"
#include <stdlib.h>
#include <string.h>

#include "../edit.h"
#include "../we_winkind.h"

static int fails;

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

/* One stand-in text and view state, shared: only addresses matter. */
static BUFFER stand_in_text;
static SCHIRM stand_in_screen;
static FLWND stand_in_list;

#define FOLDER "/home/user/work/"
#define OTHER_FOLDER "/home/user/other/"
#define DESK_SLOTS 8

static FENSTER windows[DESK_SLOTS];
static ECNT desk;

/* How a desk is filled: the kinds, in the order of the enum, each with the
   title the editor gives its window and how an ask for it names it. */
typedef struct {
 WpePane pane;
 const char *title;     /* the window's title, and the label asked for */
 int labelled;          /* the ask carries the title as its label */
 int never_found;       /* a request is always made anew */
} Kind;

static const Kind kinds[] = {
 { WPE_PANE_FILE,      "Messages",  0, 0 },
 { WPE_PANE_MESSAGES,  "Messages",  0, 0 },
 { WPE_PANE_WATCHES,   "Watches",   0, 0 },
 { WPE_PANE_STACK,     "Stack",     0, 0 },
 { WPE_PANE_HELP,      "Help",      0, 0 },
 { WPE_PANE_CLIPBOARD, "Buffer",    0, 0 },
 { WPE_PANE_ASSISTANT, "AI",        0, 0 },
 { WPE_PANE_REPORT,    "Metals Doctor", 1, 0 },
 { WPE_PANE_COMPOSER,  "Prompt",    1, 1 },
 { WPE_PANE_MANPAGE,   "printf",    1, 0 }
};
#define KINDS ((int)(sizeof kinds / sizeof kinds[0]))

static void desk_clear(void)
{
 int i;

 for (i = 0; i < DESK_SLOTS; i++)
 {
  e_window_drop(&windows[i]);
  memset(&windows[i], 0, sizeof windows[i]);
  desk.f[i] = NULL;
 }
 memset(&desk, 0, sizeof desk);
 desk.mxedt = -1;
}

/* Put a window holding text, of that kind, with that title, on the desk. */
static FENSTER *put_text_window(int slot, WpePane pane, const char *title)
{
 FENSTER *w = &windows[slot];

 memset(w, 0, sizeof *w);
 w->datnam = (char *)title;
 w->dirct = (char *)FOLDER;
 w->ed = &desk;
 (void)e_window_hold_text(w, &stand_in_text, &stand_in_screen);
 e_window_set_pane(w, pane);
 desk.f[slot] = w;
 if (slot > desk.mxedt)
  desk.mxedt = slot;
 return w;
}

/* A list window (the window list, the project) carries a title too. */
static FENSTER *put_list_window(int slot, const char *title)
{
 FENSTER *w = &windows[slot];

 memset(w, 0, sizeof *w);
 w->datnam = (char *)title;
 w->dirct = (char *)FOLDER;
 w->ed = &desk;
 (void)e_window_hold_list(w, &stand_in_list, WPE_LISTKIND_WINDOWS);
 desk.f[slot] = w;
 if (slot > desk.mxedt)
  desk.mxedt = slot;
 return w;
}

/* The ask a request for kind @k makes: a file by its name and folder, a
   labelled view by its label, any other pane by nothing. */
static WpeWindowAsk ask_of(const Kind *k)
{
 WpeWindowAsk ask;

 memset(&ask, 0, sizeof ask);
 ask.pane = k->pane;
 if (k->pane == WPE_PANE_FILE)
 {
  ask.name = k->title;
  ask.dir = FOLDER;
 }
 else if (k->labelled)
  ask.name = k->title;
 return ask;
}

static WpeWindowAsk file_ask_named(const char *name)
{
 WpeWindowAsk ask;

 memset(&ask, 0, sizeof ask);
 ask.pane = WPE_PANE_FILE;
 ask.name = name;
 ask.dir = FOLDER;
 return ask;
}

/* Slot 0 holds the clipboard in a running editor; slots 1 up the stack. */
static const int FIRST_STACK_SLOT = 1;

/* The desk of one cell: which of the three windows are open.  Returns the
   user's file (or NULL), the kind's own window (or NULL) and the list window
   (or NULL) through the pointers. */
typedef struct {
 FENSTER *file;
 FENSTER *own;
 FENSTER *list;
} Cell;

static Cell cell_desk(const Kind *k, int with_file, int with_own, int with_list)
{
 Cell c = { NULL, NULL, NULL };
 int slot = FIRST_STACK_SLOT;

 desk_clear();
 if (with_own && k->pane == WPE_PANE_CLIPBOARD)
  c.own = put_text_window(0, k->pane, k->title);
 if (with_file)
  c.file = put_text_window(slot++, WPE_PANE_FILE, k->title);
 if (with_own && k->pane != WPE_PANE_FILE && k->pane != WPE_PANE_CLIPBOARD)
  c.own = put_text_window(slot++, k->pane, k->title);
 if (with_list)
  c.list = put_list_window(slot++, k->title);
 return c;
}

/* What a request for kind @k must answer on a cell: O1.  A pane request is
   answered by that pane's own window or by nothing, never by a file or a list;
   a file request by the user's file or by nothing, never by a pane, a list or
   slot 0. */
static FENSTER *expected_for_kind(const Kind *k, const Cell *c)
{
 if (k->never_found)
  return NULL;
 if (k->pane == WPE_PANE_FILE)
  return c->file;
 return c->own;
}

static void chk_cell(const Kind *k, int with_file, int with_own, int with_list)
{
 char what[128];
 Cell c = cell_desk(k, with_file, with_own, with_list);
 WpeWindowAsk ask = ask_of(k);
 WpeWindowAsk file_ask = file_ask_named(k->title);

 snprintf(what, sizeof what, "kind %d (%s) file=%d own=%d list=%d: its ask",
          (int)k->pane, k->title, with_file, with_own, with_list);
 chk_ptr(what, e_window_find(&desk, &ask), expected_for_kind(k, &c));

 snprintf(what, sizeof what, "kind %d (%s) file=%d own=%d list=%d: a file ask",
          (int)k->pane, k->title, with_file, with_own, with_list);
 chk_ptr(what, e_window_find(&desk, &file_ask), c.file);
}

/* O1 for every kind, against every desk of the table. */
static void check_every_kind_against_every_desk(void)
{
 int i, file, own, list;

 for (i = 0; i < KINDS; i++)
  for (file = 0; file <= 1; file++)
   for (own = 0; own <= 1; own++)
    for (list = 0; list <= 1; list++)
     chk_cell(&kinds[i], file, own, list);
}

/* A composer is a window per request, whatever is open: even an open
   composer of the same label is not found. */
static void check_a_composer_is_never_found(void)
{
 WpeWindowAsk ask;

 desk_clear();
 put_text_window(1, WPE_PANE_COMPOSER, "Prompt");
 memset(&ask, 0, sizeof ask);
 ask.pane = WPE_PANE_COMPOSER;
 ask.name = "Prompt";
 chk_ptr("an open composer is not found for a new request",
         e_window_find(&desk, &ask), NULL);
}

/* The empty name is a new unnamed file: never found, even when a window
   titled "" or "Noname" is open; a request for the name Noname still finds
   the user's file of that name. */
static void check_the_unnamed_file_is_always_new(void)
{
 WpeWindowAsk empty = file_ask_named("");
 WpeWindowAsk noname = file_ask_named("Noname");
 FENSTER *unnamed;

 desk_clear();
 unnamed = put_text_window(1, WPE_PANE_FILE, "Noname");
 chk_ptr("the empty name is never found", e_window_find(&desk, &empty), NULL);
 chk_ptr("a file named Noname is found", e_window_find(&desk, &noname), unnamed);
 empty.name = NULL;
 chk_ptr("no name is never found", e_window_find(&desk, &empty), NULL);
}

/* `-pm build.log` asks for the build's Messages window and names the file it
   shows: a request for the pane, never answered by a file of the user's that
   is called Messages, and answered by the pane once that is open. */
static void check_a_messages_ask_with_a_source_is_a_pane_request(void)
{
 WpeWindowAsk ask;
 FENSTER *pane;

 memset(&ask, 0, sizeof ask);
 ask.pane = WPE_PANE_MESSAGES;
 ask.source = "build.log";
 desk_clear();
 put_text_window(1, WPE_PANE_FILE, "Messages");
 chk_ptr("a file called Messages is not the -pm pane", e_window_find(&desk, &ask), NULL);
 pane = put_text_window(2, WPE_PANE_MESSAGES, "Messages");
 chk_ptr("the -pm pane is the Messages pane", e_window_find(&desk, &ask), pane);
}

/* A file is told by its folder as well as its name. */
static void check_a_file_is_told_by_its_folder(void)
{
 WpeWindowAsk ask = file_ask_named("main.c");
 FENSTER *file;

 desk_clear();
 file = put_text_window(1, WPE_PANE_FILE, "main.c");
 chk_ptr("the same name in the same folder", e_window_find(&desk, &ask), file);
 ask.dir = OTHER_FOLDER;
 chk_ptr("the same name in another folder", e_window_find(&desk, &ask), NULL);
 ask.dir = NULL;
 chk_ptr("a file ask without a folder finds nothing", e_window_find(&desk, &ask), NULL);
}

/* A file window in slot 0 is not a window the user opened: O1's slot clause. */
static void check_a_file_never_answers_from_slot_zero(void)
{
 WpeWindowAsk ask = file_ask_named("main.c");

 desk_clear();
 put_text_window(0, WPE_PANE_FILE, "main.c");
 chk_ptr("slot 0 never answers a file ask", e_window_find(&desk, &ask), NULL);
}

/* The clipboard is slot 0 and is found there, and nowhere before it exists. */
static void check_the_clipboard_is_found_in_slot_zero(void)
{
 WpeWindowAsk ask;
 FENSTER *clip;

 memset(&ask, 0, sizeof ask);
 ask.pane = WPE_PANE_CLIPBOARD;
 desk_clear();
 chk_ptr("no clipboard before the first window", e_window_find(&desk, &ask), NULL);
 clip = put_text_window(0, WPE_PANE_CLIPBOARD, "Buffer");
 desk.mxedt = 0;
 chk_ptr("the clipboard in slot 0", e_window_find(&desk, &ask), clip);
}

/* A label is kept whole: a '/' in it stays, a label longer than a file name
   is cut once and found by the same cut, and a window with another label is
   not found. */
static void check_a_label_is_kept_whole(void)
{
 static const WpePane views[] = { WPE_PANE_REPORT, WPE_PANE_MANPAGE };
 char long_label[WPE_NAME_SIZE + 40];
 char *title, *cut;
 FENSTER *w;
 WpeWindowAsk ask;
 int i;

 title = e_window_label_title("a/b");
 chk_int("a label keeps its slash", title && !strcmp(title, "a/b"), 1);
 memset(long_label, 'x', sizeof long_label - 1);
 long_label[sizeof long_label - 1] = '\0';
 cut = e_window_label_title(long_label);
 chk_int("a long label is cut once", cut ? (long)strlen(cut) : -1, WPE_NAME_SIZE - 1);

 for (i = 0; i < 2; i++)
 {
  desk_clear();
  w = put_text_window(1, views[i], title);
  memset(&ask, 0, sizeof ask);
  ask.pane = views[i];
  ask.name = "a/b";
  chk_ptr("a label with a slash is found by the same label", e_window_find(&desk, &ask), w);
  ask.name = "b";
  chk_ptr("the end of the path is not the label", e_window_find(&desk, &ask), NULL);
  ask.name = "a_b";
  chk_ptr("a label with the slash turned is another label", e_window_find(&desk, &ask), NULL);
  desk_clear();
  w = put_text_window(1, views[i], cut);
  ask.name = long_label;
  chk_ptr("a long label is found by its cut", e_window_find(&desk, &ask), w);
 }
 desk_clear();
 free(title);
 free(cut);
}

/* O4: a question about a file's name is answered no for every pane, and yes
   only for the user's file. */
static void check_a_file_question_is_never_answered_by_a_pane(void)
{
 int i;
 FENSTER *w;
 char what[96];

 for (i = 0; i < KINDS; i++)
 {
  desk_clear();
  w = put_text_window(1, kinds[i].pane, kinds[i].title);
  snprintf(what, sizeof what, "kind %d title_is", (int)kinds[i].pane);
  chk_int(what, e_window_title_is(w, kinds[i].title), kinds[i].pane == WPE_PANE_FILE);
  snprintf(what, sizeof what, "kind %d is_file", (int)kinds[i].pane);
  chk_int(what, e_window_is_file(w, FOLDER, kinds[i].title),
          kinds[i].pane == WPE_PANE_FILE);
  snprintf(what, sizeof what, "kind %d ends_with", (int)kinds[i].pane);
  chk_int(what, e_window_title_ends_with(w, kinds[i].title + 1),
          kinds[i].pane == WPE_PANE_FILE);
  snprintf(what, sizeof what, "kind %d is_path (bare name)", (int)kinds[i].pane);
  chk_int(what, e_window_is_path(w, kinds[i].title, 0), kinds[i].pane == WPE_PANE_FILE);
  snprintf(what, sizeof what, "kind %d is_path (whole path)", (int)kinds[i].pane);
  chk_int(what, e_window_is_path(w, FOLDER "Messages", 1) &&
          !strcmp(kinds[i].title, "Messages"), kinds[i].pane == WPE_PANE_FILE);
  snprintf(what, sizeof what, "kind %d file_name", (int)kinds[i].pane);
  chk_int(what, e_window_file_name(w) != NULL, kinds[i].pane == WPE_PANE_FILE);
 }
 desk_clear();
 w = put_list_window(1, "Messages");
 chk_int("a list window is not a file", e_window_is_file(w, FOLDER, "Messages"), 0);
 chk_int("a list window is not a title", e_window_title_is(w, "Messages"), 0);
 chk_int("a list window does not end a name", e_window_title_ends_with(w, "essages"), 0);
 chk_int("a list window is not a path (bare)", e_window_is_path(w, "Messages", 0), 0);
 chk_int("a list window is not a path (whole)", e_window_is_path(w, FOLDER "Messages", 1), 0);
 chk_int("a list window has no file name", e_window_file_name(w) != NULL, 0);
}

/* The write owner's answer, played by tests/stub_write_window.c. */
extern int stub_window_is_a_file;

/* O4 for a window whose text came from a pipe: it has a title, but no file
   is behind it, so a language, a debugger, a breakpoint and a build are told
   it has no file name, each of the three questions the same.  The finder
   still finds it by its title, so a second open raises the window.  After a
   Save As it is a file's window and all three answer yes. */
static void check_a_window_from_a_pipe_has_no_file_name(void)
{
 FENSTER *w;

 desk_clear();
 w = put_text_window(1, WPE_PANE_FILE, "notes.c");
 stub_window_is_a_file = 0;
 chk_int("from a pipe: no file name", e_window_file_name(w) != NULL, 0);
 chk_int("from a pipe: the title does not end a file name", e_window_title_ends_with(w, ".c"), 0);
 chk_int("from a pipe: the title is no file's", e_window_title_is(w, "notes.c"), 0);
 chk_int("from a pipe: the finder still knows the window", e_window_is_file(w, FOLDER, "notes.c"), 1);
 chk_ptr("from a pipe: a second open finds it", e_window_find(&desk, &(WpeWindowAsk){
   .pane = WPE_PANE_FILE, .name = "notes.c", .dir = FOLDER }), w);
 stub_window_is_a_file = 1;
 chk_int("a file: it has its file name", e_window_file_name(w) != NULL, 1);
 chk_int("a file: the title ends a file name", e_window_title_ends_with(w, ".c"), 1);
 chk_int("a file: the title is its", e_window_title_is(w, "notes.c"), 1);
 desk_clear();
}

int main(void)
{
 check_a_window_from_a_pipe_has_no_file_name();
 check_every_kind_against_every_desk();
 check_a_composer_is_never_found();
 check_a_messages_ask_with_a_source_is_a_pane_request();
 check_the_unnamed_file_is_always_new();
 check_a_file_is_told_by_its_folder();
 check_a_file_never_answers_from_slot_zero();
 check_the_clipboard_is_found_in_slot_zero();
 check_a_label_is_kept_whole();
 check_a_file_question_is_never_answered_by_a_pane();

 desk_clear();
 if (fails == 0)
  printf("test_winkind_open: all checks passed\n");
 else
  printf("test_winkind_open: %d check(s) failed\n", fails);
 return fails ? 1 : 0;
}
