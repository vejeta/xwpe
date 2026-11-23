/* test_ai_read_waits.c -- a folder the user opened cannot stop the editor by
 * putting a pipe where the assistant's layer reads a file.
 *
 * A repository that was cloned chooses the names the editor reads when it
 * judges a path or starts a build server: the git config and "commondir" of
 * its .git folder, and the connection files of its .bsp folder.  A plain fopen
 * of a pipe there waits for ever for a writer.  These rows plant a pipe at
 * each of those names and ask for the answer under the watchdog, so a read
 * that waits is a FAIL that names the row, never a hang.
 *
 * Links we_ai_path.c and we_bsp.c with the read owner (we_read.c).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "test_watchdog.h"
#include "testdir.h"
#include "../we_ai_path.h"
#include "../we_bsp.h"

#define ROW_SECS         5
#define PATH_ROOM        TEST_DIR_PATH_SIZE
#define FOLDER_PREFIX    "ai-reads"
#define CONNECTION_JSON  "{\"argv\":[\"build-server\",\"bsp\"]}\n"
#define FILE_ROOM        256

static int failures;
static char base[TEST_DIR_PATH_SIZE];   /* the folder of the row in progress */

static void fail(const char *row, const char *why)
{
 printf("FAIL %s: %s\n", row, why);
 failures++;
}

/* The absolute path "<base>/<rel>". */
static void under_base(char *dst, size_t sz, const char *rel)
{
 (void)test_path_join(dst, sz, base, rel);
}

static void make_dir(const char *rel)
{
 char p[PATH_ROOM];
 under_base(p, sizeof p, rel);
 if (mkdir(p, 0700) != 0) { perror(p); exit(2); }
}

static void make_pipe(const char *rel)
{
 char p[PATH_ROOM];
 under_base(p, sizeof p, rel);
 if (mkfifo(p, 0600) != 0) { perror(p); exit(2); }
}

static void make_text(const char *rel, const char *text)
{
 char p[PATH_ROOM];
 FILE *fp;
 under_base(p, sizeof p, rel);
 fp = fopen(p, "w");
 if (!fp) { perror(p); exit(2); }
 fputs(text, fp);
 fclose(fp);
}

static void drop(const char *rel)
{
 char p[PATH_ROOM];
 under_base(p, sizeof p, rel);
 unlink(p);
}

/**
 * start_row - A fresh folder for one row, named under the watchdog.
 * @row: the row's name.
 */
static void start_row(const char *row)
{
 if (test_dir_make(base, sizeof base, FOLDER_PREFIX) != 0) exit(2);
 test_watchdog_case(row, ROW_SECS);
}

/**
 * finish_row - Remove the folder a row made, which the row has emptied.
 *
 * Each name is removed by its exact path; nothing is removed by a pattern.
 */
static void finish_row(void)
{
 char p[PATH_ROOM];
 under_base(p, sizeof p, ".git");
 rmdir(p);
 under_base(p, sizeof p, ".bsp");
 rmdir(p);
 rmdir(base);
}

/**
 * path_protected - The answer of the scope's judge for a file in the workspace.
 *
 * It reads the repository's git folder and config to find the folder
 * core.hooksPath moves the hooks to, which is where a pipe at the config stops
 * the editor.
 * Return: what wpe_path_is_protected says for base/x.c.
 */
static int path_protected(void)
{
 char file[PATH_ROOM];
 under_base(file, sizeof file, "x.c");
 return wpe_path_is_protected(base, file);
}

/* A pipe where the repository's git config belongs: the judge answers and goes on. */
static void row_git_config_is_a_pipe(void)
{
 start_row("a pipe at .git/config");
 make_dir(".git");
 make_pipe(".git/config");
 if (path_protected() != 0)
  fail("a pipe at .git/config", "x.c was judged protected with no hooks folder named");
 drop(".git/config");
 finish_row();
}

/* A pipe where a worktree's "commondir" belongs. */
static void row_git_commondir_is_a_pipe(void)
{
 start_row("a pipe at .git/commondir");
 make_dir(".git");
 make_pipe(".git/commondir");
 if (path_protected() != 0)
  fail("a pipe at .git/commondir", "x.c was judged protected with no hooks folder named");
 drop(".git/commondir");
 finish_row();
}

/* A pipe where a build server's connection file belongs: no argv, no wait. */
static void row_bsp_connection_is_a_pipe(void)
{
 char file[FILE_ROOM];
 char **argv;
 start_row("a pipe at .bsp/x.json");
 make_dir(".bsp");
 make_pipe(".bsp/x.json");
 argv = e_bsp_connection_argv(base, file, sizeof file);
 if (argv) {
  fail("a pipe at .bsp/x.json", "a connection command was read from a pipe");
  e_bsp_free_argv(argv);
 }
 drop(".bsp/x.json");
 finish_row();
}

/* A pipe that sorts first does not hide the regular connection file beside it. */
static void row_bsp_connection_beside_a_pipe(void)
{
 char file[FILE_ROOM];
 char **argv;
 start_row("a pipe a.json beside a regular b.json");
 make_dir(".bsp");
 make_pipe(".bsp/a.json");
 make_text(".bsp/b.json", CONNECTION_JSON);
 argv = e_bsp_connection_argv(base, file, sizeof file);
 if (!argv)
  fail("a pipe a.json beside a regular b.json", "the regular connection file was not read");
 else {
  if (strcmp(file, "b.json") != 0)
   fail("a pipe a.json beside a regular b.json", "the file chosen is not the regular one");
  e_bsp_free_argv(argv);
 }
 drop(".bsp/a.json");
 drop(".bsp/b.json");
 finish_row();
}

/* scala-cli's own file name as a pipe is not chosen either. */
static void row_bsp_scala_cli_file_is_a_pipe(void)
{
 char file[FILE_ROOM];
 char **argv;
 start_row("a pipe at .bsp/scala-cli.json beside a regular z.json");
 make_dir(".bsp");
 make_pipe(".bsp/scala-cli.json");
 make_text(".bsp/z.json", CONNECTION_JSON);     /* sorts after the pipe's name */
 argv = e_bsp_connection_argv(base, file, sizeof file);
 if (!argv)
  fail("a pipe at .bsp/scala-cli.json", "the regular connection file was not read");
 else {
  if (strcmp(file, "z.json") != 0)
   fail("a pipe at .bsp/scala-cli.json", "the file chosen is not the regular one");
  e_bsp_free_argv(argv);
 }
 drop(".bsp/scala-cli.json");
 drop(".bsp/z.json");
 finish_row();
}

/* A link at the connection file is refused, never followed to another file. */
static void row_bsp_connection_is_a_link(void)
{
 char file[FILE_ROOM], target[PATH_ROOM], link[PATH_ROOM];
 char **argv;
 start_row("a link at .bsp/x.json");
 make_dir(".bsp");
 make_text("real.json", CONNECTION_JSON);
 under_base(target, sizeof target, "real.json");
 under_base(link, sizeof link, ".bsp/x.json");
 if (symlink(target, link) != 0) { perror(link); exit(2); }
 argv = e_bsp_connection_argv(base, file, sizeof file);
 if (argv) {
  fail("a link at .bsp/x.json", "a link was followed to a connection file");
  e_bsp_free_argv(argv);
 }
 unlink(link);
 drop("real.json");
 finish_row();
}

/* A row runs when no row was named on the command line, or when it is the one named.
   A row that waits ends the program, so naming one shows the others' answers. */
#define ROW(only, fn) do { if (!(only) || !strcmp((only), #fn)) fn(); } while (0)

int main(int argc, char **argv)
{
 const char *only = argc > 1 ? argv[1] : NULL;
 ROW(only, row_git_config_is_a_pipe);
 ROW(only, row_git_commondir_is_a_pipe);
 ROW(only, row_bsp_connection_is_a_pipe);
 ROW(only, row_bsp_connection_beside_a_pipe);
 ROW(only, row_bsp_scala_cli_file_is_a_pipe);
 ROW(only, row_bsp_connection_is_a_link);
 test_watchdog_disarm();
 if (failures) { printf("%d failure(s)\n", failures); return 1; }
 printf("test_ai_read_waits: all rows passed\n");
 return 0;
}
