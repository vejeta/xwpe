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

/* What this editor built, in this folder: a second store beside the answers,
   for the same reason (a clone could ship it).  A program, an object or a
   library counts as current only when it is the very file the editor wrote
   here (device, inode, modification and change times in nanoseconds, size),
   so a built file a clone ships never makes a build skip. */
#define WPE_BUILT_STORE_NAME  "built-outputs"
#define WPE_BUILT_STORE_MAGIC "xwpe-built-outputs 1"
/* The most records the store keeps, in all; the oldest go first.  There is no
   cap per folder: a project of many files must keep its own records. */
#define WPE_BUILT_MAX_RECORDS 8192

struct stat;

/* One output the editor built: its real path and what identifies the file. */
typedef struct WpeBuiltRec {
 char *folder;               /* the real build folder                     */
 char *output;               /* the output's real path                    */
 unsigned long long dev, ino, size;
 long long mtime_s, mtime_ns, ctime_s, ctime_ns;
} WpeBuiltRec;

/* The records of one build folder, loaded once per build and held. */
typedef struct {
 char folder[WPE_STARTCMD_FOLDER_MAX];  /* the real build folder, "" if none */
 WpeBuiltRec *rec;
 int n;
 int state;                  /* WPE_STARTCMD_STORE_*: BAD means nothing counts */
 int dirty;                  /* a record was noted since the load or save      */
} WpeBuiltSet;

void wpe_startcmd_reset(void);
void wpe_startcmd_note_own(const char *key, const char *text);
int wpe_startcmd_is_own(const char *key, const char *text);
int wpe_startcmd_resolve_folder(const char *folder, char *out, size_t sz);
int wpe_startcmd_store_path(char *out, size_t sz);
int wpe_startcmd_check(const char *folder, const char *key, const char *text);
int wpe_startcmd_last_store_state(void);
int wpe_startcmd_remember(const char *folder, const char *key, const char *text, int answer);
size_t wpe_startcmd_printable(const char *text, char *out, size_t sz);

/* The records of one build folder, loaded once at the plan.  Returns the
   store's state; a set whose state is BAD makes built_here answer 0 and
   note_built fail. */
int  wpe_startcmd_built_load(const char *folder, WpeBuiltSet *set);
/* After a step the editor ran succeeded: @output (absolute, or relative to
   the set's folder, never to the process's), with the stat the caller made,
   is now this editor's.  Kept in memory until wpe_startcmd_built_save.
   Return: 0, or -1 for a refused store or an output that cannot be resolved. */
int  wpe_startcmd_note_built(WpeBuiltSet *set, const char *output, const struct stat *st);
/* Did this editor build this very file in this folder?  @st is the stat the
   caller already made of it (NULL: made here).  0 for no record, another
   identity, another real path, or a refused store. */
int  wpe_startcmd_built_here(const WpeBuiltSet *set, const char *output, const struct stat *st);
/* Write the set's records into the store at most once per step: the store is
   read again, this folder's records are replaced, records whose file is gone
   or changed are dropped and the oldest go past WPE_BUILT_MAX_RECORDS.
   Return: 0 (also when nothing was noted), or -1 when it could not be written. */
int  wpe_startcmd_built_save(WpeBuiltSet *set);
void wpe_startcmd_built_free(WpeBuiltSet *set);
/* Is a file, as it is now, the file it was before a step?  All of device,
   inode, size, and the times of modification and change in nanoseconds. */
int  wpe_startcmd_same_file(const struct stat *before, const struct stat *now);

#endif
