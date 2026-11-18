/* we_places.c -- where the editor's own places are (see we_places.h).

   Plain C, no editor header: the unit tests link it with we_path.c. */
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "we_path.h"
#include "we_places.h"

/** The folders below a home folder, and the ones below an XDG base. */
#define PLACES_STATE_BELOW_HOME  ".local/state"
#define PLACES_CONFIG_BELOW_HOME ".config"

/** The variables the XDG Base Directory specification names. */
#define PLACES_XDG_STATE_ENV  "XDG_STATE_HOME"
#define PLACES_XDG_CONFIG_ENV "XDG_CONFIG_HOME"

static char places_run_dir[WPE_PATH_SIZE];      /* "" until the main program names it */

/**
 * places_is_absolute - Is a variable's value a path from the root?
 * @s: the value, or NULL.
 *
 * A value that is not absolute names a place that depends on the folder the
 * editor was started in, which may be a repository the user only cloned.
 */
static int places_is_absolute(const char *s)
{
 return s != NULL && s[0] == '/';
}

/**
 * places_test_fail_has - Does XWPE_TEST_FAIL carry this word?
 * @word: one of the seam's words.
 *
 * The variable holds one word or a list separated by commas.
 */
static int places_test_fail_has(const char *word)
{
 const char *list = getenv(WPE_PLACES_TEST_FAIL_ENV);
 size_t len = strlen(word);

 while (list != NULL && *list != '\0')
 {
  size_t part = strcspn(list, ",");

  if (part == len && strncmp(list, word, len) == 0)
   return 1;
  list += part;
  if (*list == ',')
   list++;
 }
 return 0;
}

/**
 * places_password_home - The home folder the password file gives this user.
 * @buf/@n: receive it.
 * Return: 0, or -1 when the entry is missing, its folder is not absolute or
 * the test seam removed the step.
 *
 * Asked at most once per process: the editor never changes its effective user,
 * and it runs at start and from ordinary code, never from a signal handler.
 */
static int places_password_home(char *buf, size_t n)
{
 static char cached[WPE_PATH_SIZE];
 static int asked;

 if (places_test_fail_has(WPE_PLACES_NO_PASSWD_HOME))
  return -1;
 if (!asked)
 {
  struct passwd *pw = getpwuid(geteuid());

  asked = 1;
  cached[0] = '\0';
  if (pw != NULL && places_is_absolute(pw->pw_dir) &&
      wpe_path_copy(cached, sizeof cached, pw->pw_dir) != 0)
   cached[0] = '\0';
 }
 if (cached[0] == '\0')
  return -1;
 return wpe_path_copy(buf, n, cached);
}

/**
 * wpe_user_home - The user's home folder, or none.
 * @buf/@n: receive it.
 * Return: 0 with an absolute path, -1 when there is none.
 *
 * What the user gets: the editor's state, configuration and conversations
 * are kept in the same place whether or not HOME is set (a cron job, a
 * container, a service), and never in the folder the editor was started in.
 */
int wpe_user_home(char *buf, size_t n)
{
 const char *home = getenv("HOME");

 if (places_is_absolute(home))
  return wpe_path_copy(buf, n, home);
 return places_password_home(buf, n);
}

/**
 * places_below_base - One of the editor's folders: the XDG base, else below home.
 * @env: the variable naming the base; @below_home: the folder below home
 * that stands in for it; @buf/@n: receive the path.
 * Return: 0, or -1 when there is no absolute place or it does not fit.
 */
static int places_below_base(const char *env, const char *below_home, char *buf, size_t n)
{
 const char *base = getenv(env);
 char home[WPE_PATH_SIZE];

 if (places_is_absolute(base))
  return wpe_path_join(buf, n, base, WPE_PLACES_SUBDIR);
 if (wpe_user_home(home, sizeof home) != 0)
 {
  if (n > 0)
   buf[0] = '\0';
  return -1;
 }
 if (wpe_path_join(buf, n, home, below_home) != 0)
  return -1;
 return wpe_path_cat(buf, n, "/" WPE_PLACES_SUBDIR);
}

/**
 * wpe_state_dir - The folder for what the editor writes while it runs.
 * @buf: receives the path; @n: its size.
 * Return: 0, or -1 when no absolute place is known or it does not fit.
 *
 * $XDG_STATE_HOME/xwpe, else <home>/.local/state/xwpe.  A variable that is
 * not an absolute path is ignored, so a crash report or a conversation is
 * never put in a folder that depends on where the editor was started.  The
 * folder itself is not made here.
 */
int wpe_state_dir(char *buf, size_t n)
{
 return places_below_base(PLACES_XDG_STATE_ENV, PLACES_STATE_BELOW_HOME, buf, n);
}

/**
 * wpe_config_dir - The folder for the editor's own configuration.
 * @buf: receives the path; @n: its size.
 * Return: 0, or -1 when no absolute place is known or it does not fit.
 *
 * $XDG_CONFIG_HOME/xwpe, else <home>/.config/xwpe, with the same rule as
 * wpe_state_dir: with no home folder the editor reads no personal
 * configuration, and a ".config" in the start folder is never taken for it.
 */
int wpe_config_dir(char *buf, size_t n)
{
 return places_below_base(PLACES_XDG_CONFIG_ENV, PLACES_CONFIG_BELOW_HOME, buf, n);
}

/** wpe_run_dir_set - Name the private folder this run made at its start. */
void wpe_run_dir_set(const char *path)
{
 if (path == NULL || wpe_path_copy(places_run_dir, sizeof places_run_dir, path) != 0)
  places_run_dir[0] = '\0';
}

/**
 * wpe_run_dir - The private folder of this run.
 * @buf/@n: receive it.
 * Return: 0, or -1 when none was named (a test that never made one).
 */
int wpe_run_dir(char *buf, size_t n)
{
 if (places_run_dir[0] == '\0')
  return -1;
 return wpe_path_copy(buf, n, places_run_dir);
}
