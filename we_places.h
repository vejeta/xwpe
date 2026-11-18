/* we_places.h -- where the editor's own places are.

   One owner of "where is this user's home, state folder and configuration
   folder".  A place is an absolute path or there is none: it is never
   relative and never the folder the editor was started in, which may belong
   to a repository the user only cloned.  Every other file asks here instead
   of reading HOME, XDG_STATE_HOME or XDG_CONFIG_HOME itself
   (tests/check-write-owner.sh, rule 25).

   Plain C: <stddef.h> and we_path.h only, so a unit test links it with
   we_path.c and nothing of the editor. */
#ifndef WE_PLACES_H
#define WE_PLACES_H

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* The judge of a folder the editor keeps its own files in: a folder of this
   user that group and others cannot write.  One predicate for the owner of
   the editor's own folders (we_write.c), the start-command store and the
   crash handler, which cannot call more than a signal handler may. */
static inline int wpe_own_dir_mine_alone(const struct stat *st)
{
 return S_ISDIR(st->st_mode) && st->st_uid == geteuid() &&
        (st->st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

/* The test seam: XWPE_TEST_FAIL carrying this word (a list of words
   separated by commas) makes the password-file step answer "none".  It can
   only remove a place, never add one, so it cannot weaken a guard; a row
   that removes HOME needs it so that it never resolves the home of the user
   running the tests.  Under the seam the user-database home is also gone
   from startplan_is_users_startup, which compares a folder's start-up file
   with that home as well as with $HOME. */
#define WPE_PLACES_TEST_FAIL_ENV  "XWPE_TEST_FAIL"
#define WPE_PLACES_NO_PASSWD_HOME "no-passwd-home"

/* The name of the editor's folder below a base folder of the user's. */
#define WPE_PLACES_SUBDIR "xwpe"

/* The note a migration leaves in the old folder (two %s: the new option
   file, then the old folder's name).  Kept here with the other words that
   name the user's places. */
#define WPE_PLACES_MOVED_NOTE_FMT \
 "xwpe now stores its files following the XDG Base Directory specification.\n" \
 "Your settings were copied automatically:\n\n" \
 "  configuration : %s\n" \
 "  runtime state : $XDG_STATE_HOME/xwpe/  (default ~/.local/state/xwpe/)\n\n" \
 "This directory (~/%s) is kept only as a backup and is no longer read.\n" \
 "You can delete it once the new location works for you.\n"

/* The user's home folder: $HOME when it is an absolute path, else the home
   of the password file's entry of geteuid() when that is absolute, else
   none.  0 with @buf filled, -1 when there is none or it does not fit. */
int wpe_user_home(char *buf, size_t n);

/* The home the user database gives for geteuid() when it is absolute, for
   code that compares it with $HOME on purpose (a tool that takes its home
   from the database whatever $HOME says).  0 with @buf filled, or -1. */
int wpe_user_database_home(char *buf, size_t n);

/* <home>/.xwpe, where older editors kept their settings; only the migration
   to the XDG folders reads it.  0, or -1 when there is no home folder. */
int wpe_legacy_dir(char *buf, size_t n);

/* $XDG_STATE_HOME/xwpe when that variable is absolute, else
   <home>/.local/state/xwpe.  Not made here.  0, or -1 when there is no place. */
int wpe_state_dir(char *buf, size_t n);

/* $XDG_CONFIG_HOME/xwpe when that variable is absolute, else
   <home>/.config/xwpe.  Not made here.  0, or -1 when there is no place. */
int wpe_config_dir(char *buf, size_t n);

/* The editor's private folder of this run (made new at every start, 0700,
   and removed at the end).  The main program names it once after it made it;
   nothing resolves it from the environment.  wpe_run_dir_set copies @path;
   wpe_run_dir gives it back: 0, or -1 when none was named. */
void wpe_run_dir_set(const char *path);
int wpe_run_dir(char *buf, size_t n);

#endif
