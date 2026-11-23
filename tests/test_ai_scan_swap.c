/* test_ai_scan_swap.c -- a folder swapped for a link while the scope scan runs
 * is not entered.
 *
 * wpe_path_count_outward_links walks a workspace and counts the links that
 * lead out of it.  It learns what each entry is from the folder, then opens a
 * folder entry to walk it.  A program racing the editor can put a link where
 * that folder was between the two.  The scan opens such folders with the read
 * owner's form for a folder met in a walk (no link followed), so the link is
 * refused and nothing behind it is counted.
 *
 * The swap is made from inside the scan: the linker's --wrap puts a function
 * around wpe_read_entry_kind, which swaps the folder named "sub" for a link
 * to a tree that holds a link leading out of the workspace, right after the
 * entry was told to be a folder.  A scan that follows the link counts that
 * outward link (1); the right answer is 0.  Without a linker that has --wrap
 * (Apple's ld) the row is reported as SKIP.
 *
 * Links we_ai_path.c with the read owner (we_read.c).
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "test_watchdog.h"
#include "testdir.h"
#include "../we_ai_path.h"

#define ROW_SECS       5
#define TEST_SKIP_STATUS 77       /* the exit status automake reads as SKIP */
#define FOLDER_PREFIX  "ai-scan-swap"
#define ROOT_TARGET    "/"        /* what the link beyond the swap leads to: out of any workspace */

static char base[TEST_DIR_PATH_SIZE];
static char ws[TEST_DIR_PATH_SIZE];
static char sub[TEST_DIR_PATH_SIZE];
static char aside[TEST_DIR_PATH_SIZE];
static char beyond[TEST_DIR_PATH_SIZE];
static char esc[TEST_DIR_PATH_SIZE];
static int swapped;

#ifdef WPE_TEST_LD_WRAP
mode_t __real_wpe_read_entry_kind(DIR *d, const char *name);

/**
 * __wrap_wpe_read_entry_kind - Tell the entry's kind, then swap "sub" for a link, once.
 *
 * The scan has just been told that "sub" is a folder when this puts a link in
 * its place, as a program racing the editor would.
 */
mode_t __wrap_wpe_read_entry_kind(DIR *d, const char *name)
{
 mode_t kind = __real_wpe_read_entry_kind(d, name);

 if (kind == S_IFDIR && !swapped && strcmp(name, "sub") == 0)
 {
  swapped = 1;
  if (rename(sub, aside) == 0 && symlink(beyond, sub) != 0)
   (void)rename(aside, sub);
 }
 return kind;
}
#endif

/**
 * make_tree - ws/sub (a folder), beyond/esc (a link leading to "/").
 * Return: 0, or -1 (counted).
 */
static int make_tree(void)
{
 if (test_dir_make(base, sizeof base, FOLDER_PREFIX) != 0)
  return -1;
 if (test_path_join(ws, sizeof ws, base, "ws") != 0 ||
     test_path_join(sub, sizeof sub, ws, "sub") != 0 ||
     test_path_join(aside, sizeof aside, ws, "sub.orig") != 0 ||
     test_path_join(beyond, sizeof beyond, base, "beyond") != 0 ||
     test_path_join(esc, sizeof esc, beyond, "esc") != 0)
  return -1;
 if (mkdir(ws, 0700) != 0 || mkdir(sub, 0700) != 0 || mkdir(beyond, 0700) != 0 ||
     symlink(ROOT_TARGET, esc) != 0)
 {
  test_dir_fail("FAIL: setup: %s\n", strerror(errno));
  return -1;
 }
 return 0;
}

/**
 * remove_tree - Remove what make_tree made, each by its exact name.
 */
static void remove_tree(void)
{
 unlink(sub);          /* the link a swap put there, when it did */
 rmdir(aside);
 rmdir(sub);
 unlink(esc);
 rmdir(beyond);
 rmdir(ws);
 rmdir(base);
}

int main(void)
{
 int truncated = 0, outward, failures = 0;

 if (make_tree() != 0)
 {
  remove_tree();
  return 1;
 }
 test_watchdog_case("a folder swapped for a link during the scan", ROW_SECS);
#ifdef WPE_TEST_LD_WRAP
 outward = wpe_path_count_outward_links(ws, -1, &truncated);
 if (!swapped)
 {
  printf("FAIL: the swap never happened, the row proves nothing\n");
  failures++;
 }
 if (outward != 0)
 {
  printf("FAIL: the scan entered a folder swapped for a link: %d outward link(s) counted\n",
         outward);
  failures++;
 }
#else
 (void)truncated;
 (void)outward;
 printf("SKIP: no linker --wrap, the swap row is not run\n");
 test_watchdog_disarm();
 remove_tree();
 return TEST_SKIP_STATUS;        /* automake counts 77 as a SKIP, never a PASS */
#endif
 test_watchdog_disarm();
 remove_tree();
 failures += test_dir_failures;
 if (failures)
  return 1;
 printf("test_ai_scan_swap: ok\n");
 return 0;
}
