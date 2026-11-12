/* we_startcmd.h -- the programs a project folder names, run only after a yes.
 *
 * An xwperc in the folder the editor starts from, or a project file, may
 * name the print command, a language's compiler and linker line, or the
 * debugger.  The first time such a command differs from the user's own
 * setting, the editor shows it and asks; the answer is kept per folder, key
 * and command text in the user's state folder, never in the project.
 *
 * Pure C, no display header: the C tests link we_startcmd.c alone.
 */
#ifndef WE_STARTCMD_H
#define WE_STARTCMD_H

#include <stddef.h>

/* What a lookup or the question answers. */
#define WPE_STARTCMD_RUN     0   /* run the folder's command                   */
#define WPE_STARTCMD_USE_OWN 1   /* run the user's own command instead         */
#define WPE_STARTCMD_ASK     2   /* nothing is known: the user must be asked   */
#define WPE_STARTCMD_CANCEL  3   /* the user said neither: run nothing         */

/* The answers a record keeps. */
#define WPE_STARTCMD_NO  0
#define WPE_STARTCMD_YES 1

/* The most answers the store keeps, in all and for one folder; the oldest
   go first. */
#define WPE_STARTCMD_MAX_RECORDS    512
#define WPE_STARTCMD_MAX_PER_FOLDER 64

/* The store, below the state folder, and its first line.  Version 2 keeps
   one answer per folder, key and text: two plans answered for in one
   folder both stay answered. */
#define WPE_STARTCMD_STATE_SUBDIR "xwpe"
#define WPE_STARTCMD_STORE_NAME   "start-commands"
#define WPE_STARTCMD_STORE_MAGIC  "xwpe-start-commands 2"
/* The first line of a version-1 store, read as an empty store. */
#define WPE_STARTCMD_STORE_MAGIC_V1 "xwpe-start-commands 1"

/* The state folder when $XDG_STATE_HOME is not set, below $HOME. */
#define WPE_STARTCMD_STATE_DEFAULT ".local/state"

/* The keys, as the store names them. */
#define WPE_STARTCMD_KEY_PRINT    "PrintCmd"
#define WPE_STARTCMD_KEY_DEBUGGER "Debugger"
#define WPE_STARTCMD_KEY_COMPILER "Compiler/"   /* + the language's name     */
#define WPE_STARTCMD_KEY_PROJECT  "Project/"    /* + the project file's name */

/* Room for a key, a resolved folder and a store path. */
#define WPE_STARTCMD_KEY_MAX    256
#define WPE_STARTCMD_FOLDER_MAX 4096

/* How a load of the store went. */
#define WPE_STARTCMD_STORE_OK      0
#define WPE_STARTCMD_STORE_MISSING 1   /* no file yet: the first answer makes it */
#define WPE_STARTCMD_STORE_BAD     2   /* unreadable, another version, a bad line */

void wpe_startcmd_reset(void);
void wpe_startcmd_note_own(const char *key, const char *text);
int wpe_startcmd_is_own(const char *key, const char *text);
int wpe_startcmd_resolve_folder(const char *folder, char *out, size_t sz);
int wpe_startcmd_store_path(char *out, size_t sz);
int wpe_startcmd_check(const char *folder, const char *key, const char *text);
int wpe_startcmd_last_store_state(void);
int wpe_startcmd_remember(const char *folder, const char *key, const char *text, int answer);
size_t wpe_startcmd_printable(const char *text, char *out, size_t sz);

#endif
