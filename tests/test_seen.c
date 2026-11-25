/* test_seen.c -- unit test for the owner of "is this the object I saw"
 * (we_seen.c).  Links we_seen.c only; always runs.
 *
 * What goes wrong if the owner breaks: a window's Save writes into a file that
 * only happens to carry the number of the one the window read (the file system
 * gave the number back after the old file was gone), the assistant's revert
 * writes into a new folder that took its workspace's number, or a touched file
 * is taken for a new one and the user is asked about every save.
 *
 * Two kinds of row.  The table row builds the records by hand and goes through
 * every line of the judge, both birth columns, with no file system involved.
 * The disk rows (the look routes, a touched file, a file or a link that took
 * the number, the held folder) run on a disk file system, because tmpfs never
 * gives a number back; each prints which file system it ran on.  A file system
 * that never gives a number back makes those rows SKIP, with that reason; a
 * file system that does, but where the number could not be reached, is a FAIL.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../we_seen.h"
#include "seen_rows.h"
#include "interpose.h"

#define BASE_DEV   7
#define BASE_INO   100
#define BASE_UID   1000
#define BASE_GID   1000
#define BASE_SIZE  10
#define BASE_MTIME 50
#define BASE_CTIME 60
#define BASE_NSEC  5
#define BIRTH_A    10
#define BIRTH_B    20
#define BIRTH_C    30
#define BIRTH_KERNEL_SET WPE_SEEN_BIRTH_KERNEL
#define BIRTH_BACK_SET   WPE_SEEN_BIRTH_BACK_ONLY
#define BIRTH_USER_SET   WPE_SEEN_BIRTH_ANY_WAY
#define HELD_ROUNDS      64

static int fails;
static int skips;

static void chk_int(const char *what, long got, long want)
{
 if (got != want)
 {
  printf("FAIL: %s: got %ld want %ld\n", what, got, want);
  fails++;
 }
}

/** The word of an answer, for a failure line. */
static const char *is_word(WpeSeenIs is)
{
 if (is == WPE_SEEN_SAME)
  return("SAME");
 if (is == WPE_SEEN_OTHER)
  return("OTHER");
 if (is == WPE_SEEN_UNSURE)
  return("UNSURE");
 return("?");
}

static void chk_is(const char *what, WpeSeenIs got, WpeSeenIs want)
{
 if (got != want)
 {
  printf("FAIL: %s: got %s want %s\n", what, is_word(got), is_word(want));
  fails++;
 }
}

static void skip(const char *why)
{
 printf("SKIP: %s\n", why);
 skips++;
}

/* ---- the system calls the owner makes, played by the test -------------- */

#if WPE_PLATFORM_STATX_BTIME
/*
 * The test program defines statx, readlink and fstat itself, so the owner's
 * calls (linked into this program) reach these and not libc's.  Each passes
 * the call through to the kernel and then, when a row asks, changes the
 * answer: a birth the mask does not vouch for, a birth of zero or -1, a
 * statx that is not there (ENOSYS, EPERM) or that fails about the object, a
 * link read one byte short, a descriptor whose fstat fails but still fills the
 * buffer with the folder's real numbers.  Nothing is preloaded or built.
 */
#define SHIM_PASS          0
#define SHIM_NO_MASK       1   /* a birth is filled in but the mask does not name it */
#define SHIM_BIRTH_ZERO    2
#define SHIM_BIRTH_MINUS1  3
#define SHIM_BIRTH_REAL    4
#define SHIM_ENOSYS        5
#define SHIM_EPERM         6
#define SHIM_ENOENT        7
#define SHIM_EACCES        8
#define SHIM_EBADF         9
#define SHIM_ELOOP         10
#define SHIM_ENOTDIR       11
#define SHIM_REAL_BIRTH_SEC  1000
#define SHIM_REAL_BIRTH_NSEC 5
#define SHIM_NO_FD         (-2)

static int shim_statx_mode = SHIM_PASS;
static int shim_readlink_short;
static int shim_fstat_lie_fd = SHIM_NO_FD;

/** The errno a failing statx mode plays, or 0 when the mode is not a failure. */
static int shim_errno_of(int mode)
{
 switch (mode)
 {
 case SHIM_ENOSYS:  return(ENOSYS);
 case SHIM_EPERM:   return(EPERM);
 case SHIM_ENOENT:  return(ENOENT);
 case SHIM_EACCES:  return(EACCES);
 case SHIM_EBADF:   return(EBADF);
 case SHIM_ELOOP:   return(ELOOP);
 case SHIM_ENOTDIR: return(ENOTDIR);
 default:           return(0);
 }
}

/** Change a good statx answer as the mode says: the birth the row wants the owner to meet. */
static void shim_bend_birth(struct statx *sx)
{
 switch (shim_statx_mode)
 {
 case SHIM_NO_MASK:
  sx->stx_btime.tv_sec = SHIM_REAL_BIRTH_SEC;
  sx->stx_btime.tv_nsec = SHIM_REAL_BIRTH_NSEC;
  sx->stx_mask &= ~(unsigned)STATX_BTIME;
  break;
 case SHIM_BIRTH_ZERO:
  sx->stx_btime.tv_sec = 0;
  sx->stx_btime.tv_nsec = 0;
  sx->stx_mask |= STATX_BTIME;
  break;
 case SHIM_BIRTH_MINUS1:
  sx->stx_btime.tv_sec = -1;
  sx->stx_btime.tv_nsec = 0;
  sx->stx_mask |= STATX_BTIME;
  break;
 case SHIM_BIRTH_REAL:
  sx->stx_btime.tv_sec = SHIM_REAL_BIRTH_SEC;
  sx->stx_btime.tv_nsec = SHIM_REAL_BIRTH_NSEC;
  sx->stx_mask |= STATX_BTIME;
  break;
 default:
  break;
 }
}

int statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx *sx)
{
 int err = shim_errno_of(shim_statx_mode);
 int rc;

 if (err != 0)
 {
  errno = err;
  return(-1);
 }
 rc = (int)syscall(SYS_statx, dirfd, path, flags, mask, sx);
 if (rc == 0)
  shim_bend_birth(sx);
 return(rc);
}

ssize_t readlink(const char *path, char *buf, size_t size)
{
 ssize_t got = (ssize_t)syscall(SYS_readlinkat, AT_FDCWD, path, buf, size);

 if (got > 0 && shim_readlink_short)
  got--;
 return(got);
}

WPE_HOOK_TWIN_READLINK

int fstat(int fd, struct stat *st)
{
 int rc = fstatat(fd, "", st, AT_EMPTY_PATH);

 if (rc == 0 && fd == shim_fstat_lie_fd)
 {
  errno = ESTALE;
  return(-1);
 }
 return(rc);
}
#endif

/* ---- row 1: the table, built by hand ---------------------------------- */

/**
 * rec_base - A record of a regular file with known numbers, made by hand.
 *
 * Built with wpe_seen_from_stat (no birth), then the stamps are set on the
 * record so the row does not depend on how this system spells st_ctim.
 */
static void rec_base(WpeSeen *s)
{
 struct stat st;

 memset(&st, 0, sizeof st);
 st.st_dev = BASE_DEV;
 st.st_ino = BASE_INO;
 st.st_mode = S_IFREG | 0644;
 st.st_uid = BASE_UID;
 st.st_gid = BASE_GID;
 st.st_size = BASE_SIZE;
 wpe_seen_from_stat(s, &st);
 s->seen_ctime.tv_sec = BASE_CTIME;
 s->seen_ctime.tv_nsec = BASE_NSEC;
 s->seen_mtime.tv_sec = BASE_MTIME;
 s->seen_mtime.tv_nsec = BASE_NSEC;
}

/** The birth setter of the table row: a record with a birth time of its own. */
static void rec_birth(WpeSeen *s, long sec, WpeSeenBirthTrust trust)
{
 s->seen_has_birth = 1;
 s->seen_birth_trust = trust;
 s->seen_birth.tv_sec = sec;
 s->seen_birth.tv_nsec = BASE_NSEC;
}

static void rec_touch(WpeSeen *s)
{
 s->seen_ctime.tv_sec += 5;
}

/** Step A: another device, inode or kind is another object, whatever the stamps say. */
static void table_step_a(void)
{
 WpeSeen then, now;

 rec_base(&then);
 rec_base(&now);
 now.seen_ino = BASE_INO + 1;
 chk_is("A: another inode", wpe_seen_judge(&then, &now), WPE_SEEN_OTHER);
 rec_base(&now);
 now.seen_dev = BASE_DEV + 1;
 chk_is("A: another device", wpe_seen_judge(&then, &now), WPE_SEEN_OTHER);
 rec_base(&now);
 now.seen_kind = S_IFLNK;
 chk_is("A: another kind", wpe_seen_judge(&then, &now), WPE_SEEN_OTHER);
 rec_birth(&then, BIRTH_A, BIRTH_KERNEL_SET);
 rec_base(&now);
 rec_birth(&now, BIRTH_A, BIRTH_KERNEL_SET);
 now.seen_ino = BASE_INO + 1;
 chk_is("A: another inode with equal births", wpe_seen_judge(&then, &now), WPE_SEEN_OTHER);
}

/** Steps B and B': the status-change times are equal. */
static void table_step_b(void)
{
 WpeSeen then, now;

 rec_base(&then);
 rec_base(&now);
 chk_is("B: nothing changed", wpe_seen_judge(&then, &now), WPE_SEEN_SAME);
 now.seen_uid = BASE_UID + 1;
 chk_is("B': another owner", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
 rec_base(&now);
 now.seen_gid = BASE_GID + 1;
 chk_is("B': another group", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
 rec_base(&now);
 now.seen_mode = 0600;
 chk_is("B': another mode", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
 rec_base(&now);
 now.seen_size = BASE_SIZE + 1;
 chk_is("B': another size", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
 rec_base(&now);
 now.seen_mtime.tv_nsec = BASE_NSEC + 1;
 chk_is("B': another modification time", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
 rec_base(&now);
 rec_birth(&now, BIRTH_A, BIRTH_KERNEL_SET);
 chk_is("B: a birth on one side only is not compared", wpe_seen_judge(&then, &now), WPE_SEEN_SAME);
 rec_birth(&then, BIRTH_A, BIRTH_KERNEL_SET);
 chk_is("B: equal births", wpe_seen_judge(&then, &now), WPE_SEEN_SAME);
 rec_birth(&now, BIRTH_B, BIRTH_KERNEL_SET);
 chk_is("B': equal stamps, different births", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
}

/** Steps C and C': the object was touched since, both sides have a birth. */
static void table_step_c(void)
{
 WpeSeen then, now;

 rec_base(&then);
 rec_birth(&then, BIRTH_A, BIRTH_KERNEL_SET);
 rec_base(&now);
 rec_birth(&now, BIRTH_A, BIRTH_KERNEL_SET);
 rec_touch(&now);
 now.seen_size = BASE_SIZE + 5;
 chk_is("C: touched since, same birth", wpe_seen_judge(&then, &now), WPE_SEEN_SAME);
 now.seen_uid = BASE_UID + 1;
 chk_is("C: touched since, same birth, another owner", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
 rec_base(&now);
 rec_touch(&now);
 rec_birth(&now, BIRTH_B, BIRTH_KERNEL_SET);
 chk_is("C': another birth, set only by the kernel", wpe_seen_judge(&then, &now), WPE_SEEN_OTHER);
 rec_birth(&now, BIRTH_B, BIRTH_USER_SET);
 chk_is("C': another birth, the new side can be set by a user", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
 rec_birth(&now, BIRTH_B, BIRTH_KERNEL_SET);
 rec_birth(&then, BIRTH_A, BIRTH_USER_SET);
 chk_is("C': another birth, the old side can be set by a user", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
}

/** Step D: the object was touched and there is no usable birth. */
static void table_step_d(void)
{
 WpeSeen then, now;

 rec_base(&then);
 rec_base(&now);
 rec_touch(&now);
 chk_is("D: touched, no birth on either side", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
 rec_birth(&then, BIRTH_A, BIRTH_KERNEL_SET);
 chk_is("D: touched, a birth on the old side only", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
 rec_base(&then);
 rec_birth(&now, BIRTH_A, BIRTH_KERNEL_SET);
 chk_is("D: touched, a birth on the new side only", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
}

/** A record that is not known gives UNSURE, never SAME, whichever side it is on. */
static void table_unknown(void)
{
 WpeSeen known, never;

 rec_base(&known);
 wpe_seen_forget(&never);
 chk_is("unknown old side", wpe_seen_judge(&never, &known), WPE_SEEN_UNSURE);
 chk_is("unknown new side", wpe_seen_judge(&known, &never), WPE_SEEN_UNSURE);
 chk_is("both unknown", wpe_seen_judge(&never, &never), WPE_SEEN_UNSURE);
 chk_int("a forgotten record is not known", wpe_seen_known(&never), 0);
 chk_int("a taken record is known", wpe_seen_known(&known), 1);
 chk_int("the kind of a taken record", wpe_seen_kind(&known) == S_IFREG, 1);
}

/** A record built from a stat by hand has no birth, so a touched file is not proven (R18). */
static void table_from_stat_has_no_birth(void)
{
 WpeSeen s;

 rec_base(&s);
 chk_int("from_stat gives no birth", s.seen_has_birth, 0);
}

/** A link record built by hand: the target text of the link, as a name look makes it. */
static void name_make(WpeSeenName *n, ino_t ino, const char *text)
{
 rec_base(&n->seen);
 n->seen.seen_ino = ino;
 n->seen.seen_kind = S_IFLNK;
 n->seen.seen_size = (off_t)strlen(text);
 n->has_text = 1;
 n->len = strlen(text);
 strcpy(n->text, text);
}

/** The name judge words a sentence: a link is the same link by its text, never by an action. */
static void table_names(void)
{
 WpeSeenName then, now, plain;

 name_make(&then, BASE_INO, "target-a");
 name_make(&now, BASE_INO, "target-a");
 chk_is("a link with the same text", wpe_seen_name_judge(&then, &now), WPE_SEEN_SAME);
 now.seen.seen_ctime.tv_sec += 7;
 chk_is("the same link, ctime moved", wpe_seen_name_judge(&then, &now), WPE_SEEN_SAME);
 name_make(&now, BASE_INO, "target-b");
 chk_is("a link with another text", wpe_seen_name_judge(&then, &now), WPE_SEEN_OTHER);
 name_make(&now, BASE_INO + 1, "target-a");
 chk_is("a link with another number", wpe_seen_name_judge(&then, &now), WPE_SEEN_OTHER);
 name_make(&now, BASE_INO, "target-a");
 now.has_text = 0;
 chk_is("a link without its text goes to the stamps: same stamps", wpe_seen_name_judge(&then, &now), WPE_SEEN_SAME);
 now.seen.seen_ctime.tv_sec += 7;
 chk_is("a link without its text goes to the stamps: touched", wpe_seen_name_judge(&then, &now), WPE_SEEN_UNSURE);
 name_make(&plain, BASE_INO, "x");
 plain.has_text = 0;
 plain.seen.seen_kind = S_IFREG;
 now = plain;
 chk_is("a file is judged by the stamps", wpe_seen_name_judge(&plain, &now), WPE_SEEN_SAME);
 wpe_seen_forget(&now.seen);
 chk_is("an unknown name", wpe_seen_name_judge(&then, &now), WPE_SEEN_UNSURE);
}

/** The two looks of one moment: device, inode and kind. */
static void table_same_now(void)
{
 struct stat a, b;

 memset(&a, 0, sizeof a);
 a.st_dev = BASE_DEV;
 a.st_ino = BASE_INO;
 a.st_mode = S_IFREG | 0644;
 b = a;
 chk_int("same_now: one object", wpe_seen_same_now(&a, &b), 1);
 b.st_ino++;
 chk_int("same_now: another inode", wpe_seen_same_now(&a, &b), 0);
 b = a;
 b.st_dev++;
 chk_int("same_now: another device", wpe_seen_same_now(&a, &b), 0);
 b = a;
 b.st_mode = S_IFDIR | 0755;
 chk_int("same_now: another kind", wpe_seen_same_now(&a, &b), 0);
}

/** after_mode_change - The look after the editor's own chmod: later ctime, other mode. */
static void after_mode_change(WpeSeen *now)
{
 rec_base(now);
 rec_touch(now);
 now->seen_mode = 0600;
}

/* @scenario SC-name-identity-21: the judge of the look after the editor's own
   change of mode.  Only the status-change time and the mode may move; a birth
   decides where both sides have one. */
static void table_after_own_change(void)
{
 WpeSeen then, now;

 rec_base(&then);
 after_mode_change(&now);
 chk_is("own: no birth, only ctime and mode moved", wpe_seen_judge_after_own_change(&then, &now), WPE_SEEN_SAME);
 now.seen_uid = BASE_UID + 1;
 chk_is("own: no birth, another owner", wpe_seen_judge_after_own_change(&then, &now), WPE_SEEN_UNSURE);
 after_mode_change(&now);
 now.seen_gid = BASE_GID + 1;
 chk_is("own: no birth, another group", wpe_seen_judge_after_own_change(&then, &now), WPE_SEEN_UNSURE);
 after_mode_change(&now);
 now.seen_size = BASE_SIZE + 1;
 chk_is("own: no birth, another size", wpe_seen_judge_after_own_change(&then, &now), WPE_SEEN_UNSURE);
 after_mode_change(&now);
 now.seen_mtime.tv_nsec = BASE_NSEC + 1;
 chk_is("own: no birth, another modification time", wpe_seen_judge_after_own_change(&then, &now), WPE_SEEN_UNSURE);
 after_mode_change(&now);
 now.seen_ctime.tv_sec = BASE_CTIME - 1;
 chk_is("own: no birth, ctime went back", wpe_seen_judge_after_own_change(&then, &now), WPE_SEEN_UNSURE);
 after_mode_change(&now);
 now.seen_ctime.tv_sec = BASE_CTIME;
 now.seen_ctime.tv_nsec = BASE_NSEC - 1;
 chk_is("own: no birth, ctime went back inside the second", wpe_seen_judge_after_own_change(&then, &now), WPE_SEEN_UNSURE);
 after_mode_change(&now);
 now.seen_ino = BASE_INO + 1;
 chk_is("own: another inode", wpe_seen_judge_after_own_change(&then, &now), WPE_SEEN_OTHER);
 after_mode_change(&now);
 wpe_seen_forget(&now);
 chk_is("own: an unknown look", wpe_seen_judge_after_own_change(&then, &now), WPE_SEEN_UNSURE);
 rec_birth(&then, BIRTH_A, BIRTH_KERNEL_SET);
 after_mode_change(&now);
 rec_birth(&now, BIRTH_A, BIRTH_KERNEL_SET);
 chk_is("own: equal kernel births", wpe_seen_judge_after_own_change(&then, &now), WPE_SEEN_SAME);
 rec_birth(&now, BIRTH_B, BIRTH_KERNEL_SET);
 chk_is("own: another kernel birth", wpe_seen_judge_after_own_change(&then, &now), WPE_SEEN_OTHER);
 rec_birth(&then, BIRTH_A, BIRTH_USER_SET);
 rec_birth(&now, BIRTH_B, BIRTH_USER_SET);
 chk_is("own: another birth its owner can set", wpe_seen_judge_after_own_change(&then, &now), WPE_SEEN_UNSURE);
 rec_birth(&now, BIRTH_A, BIRTH_KERNEL_SET);
 now.seen_mtime.tv_nsec = BASE_NSEC + 1;
 chk_is("own: equal births decide over the stamps", wpe_seen_judge_after_own_change(&then, &now), WPE_SEEN_SAME);
}

/** The judge that answers only what a birth time proves: a number the file
    system gave back to a new object agrees with the old one on every field a
    system without a birth time reports (measured on OpenBSD 8.0, where one ffs
    timestamp covered a removal and the eleven files made after it). */
static void table_judge_proved(void)
{
 WpeSeen then, now;

 rec_base(&then);
 rec_base(&now);
 chk_is("proved: equal stamps, no birth on either side", wpe_seen_judge_proved(&then, &now), WPE_SEEN_UNSURE);
 chk_is("proved: the plain judge still says same", wpe_seen_judge(&then, &now), WPE_SEEN_SAME);
 rec_birth(&now, BIRTH_A, BIRTH_KERNEL_SET);
 chk_is("proved: a birth on one side only", wpe_seen_judge_proved(&then, &now), WPE_SEEN_UNSURE);
 rec_birth(&then, BIRTH_A, BIRTH_KERNEL_SET);
 chk_is("proved: equal births", wpe_seen_judge_proved(&then, &now), WPE_SEEN_SAME);
 rec_birth(&now, BIRTH_B, BIRTH_KERNEL_SET);
 chk_is("proved: equal stamps, another birth", wpe_seen_judge_proved(&then, &now), WPE_SEEN_UNSURE);
 rec_base(&then);
 rec_base(&now);
 rec_birth(&then, BIRTH_A, BIRTH_USER_SET);
 rec_birth(&now, BIRTH_A, BIRTH_USER_SET);
 rec_touch(&now);
 chk_is("proved: touched, one equal birth its owner can set", wpe_seen_judge_proved(&then, &now), WPE_SEEN_SAME);
 rec_base(&then);
 rec_base(&now);
 now.seen_ino = BASE_INO + 1;
 chk_is("proved: another inode", wpe_seen_judge_proved(&then, &now), WPE_SEEN_OTHER);
 wpe_seen_forget(&now);
 chk_is("proved: an unknown look", wpe_seen_judge_proved(&then, &now), WPE_SEEN_UNSURE);
}

/** A pair of records whose births its owner can move back and no call moves forward. */
static void rec_back_only_pair(WpeSeen *then, WpeSeen *now, long then_sec, long now_sec)
{
 rec_base(then);
 rec_base(now);
 rec_birth(then, then_sec, BIRTH_BACK_SET);
 rec_birth(now, now_sec, BIRTH_BACK_SET);
}

/** The pairs that agree, or that a birth cannot contradict: the question never moves them. */
static void table_contradicted_leaves(void)
{
 WpeSeen then, now;

 rec_back_only_pair(&then, &now, BIRTH_A, BIRTH_A);
 chk_is("contradicted: equal births", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_SAME);
 rec_touch(&now);
 chk_is("contradicted: touched, equal births", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_SAME);
 rec_back_only_pair(&then, &now, BIRTH_B, BIRTH_A);
 chk_is("contradicted: a birth that went back proves nothing", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_UNSURE);
 rec_back_only_pair(&then, &now, BIRTH_A, BIRTH_B);
 then.seen_birth_trust = WPE_SEEN_BIRTH_ANY_WAY;
 now.seen_birth_trust = WPE_SEEN_BIRTH_ANY_WAY;
 chk_is("contradicted: a later birth its owner can set either way", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_UNSURE);
 now.seen_birth_trust = WPE_SEEN_BIRTH_BACK_ONLY;
 chk_is("contradicted: a later birth, one side either way", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_UNSURE);
 rec_back_only_pair(&then, &now, BIRTH_A, BIRTH_B);
 wpe_seen_forget(&now);
 chk_is("contradicted: an unknown look", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_UNSURE);
}

/** The pairs where one side has no birth, or the numbers differ, or the kernel sets the birth. */
static void table_contradicted_unreadable(void)
{
 WpeSeen then, now;

 rec_back_only_pair(&then, &now, BIRTH_A, BIRTH_B);
 now.seen_has_birth = 0;
 chk_is("contradicted: no birth on one side, nothing differs", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_SAME);
 rec_touch(&now);
 chk_is("contradicted: no birth on one side, touched", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_UNSURE);
 rec_back_only_pair(&then, &now, BIRTH_A, BIRTH_B);
 now.seen_ino = BASE_INO + 1;
 chk_is("contradicted: another inode", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_OTHER);
 rec_base(&then);
 rec_base(&now);
 rec_birth(&then, BIRTH_A, BIRTH_KERNEL_SET);
 rec_birth(&now, BIRTH_B, BIRTH_KERNEL_SET);
 rec_touch(&now);
 chk_is("contradicted: a later kernel birth, the plain judge answers OTHER too", wpe_seen_judge(&then, &now), WPE_SEEN_OTHER);
 chk_is("contradicted: a later kernel birth", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_OTHER);
}

/**
 * The question of a caller that is about to say a name is gone: a birth LATER
 * than the one remembered, where no call moves a birth forward, is another
 * object, though the plain judge says UNSURE.  It never changes a SAME.
 */
static void table_judge_contradicted(void)
{
 WpeSeen then, now;

 table_contradicted_leaves();
 table_contradicted_unreadable();
 rec_back_only_pair(&then, &now, BIRTH_A, BIRTH_B);
 chk_is("contradicted: the plain judge says UNSURE", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
 chk_is("contradicted: a later back-only birth", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_OTHER);
 rec_touch(&now);
 chk_is("contradicted: touched, a later back-only birth", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_OTHER);
 rec_back_only_pair(&then, &now, BIRTH_A, BIRTH_A);
 now.seen_birth.tv_nsec = then.seen_birth.tv_nsec + 1;
 chk_is("contradicted: later by one nanosecond", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_OTHER);
 now.seen_birth.tv_nsec = then.seen_birth.tv_nsec - 1;
 chk_is("contradicted: earlier by one nanosecond", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_UNSURE);
 rec_back_only_pair(&then, &now, BIRTH_B, BIRTH_C);
 now.seen_birth.tv_sec = BIRTH_A;
 chk_is("contradicted: then later than now", wpe_seen_judge_contradicted(&then, &now), WPE_SEEN_UNSURE);
}

/** Row 1: every line of the judge, with no file system involved. */
static void test_the_table(void)
{
 table_step_a();
 table_step_b();
 table_step_c();
 table_step_d();
 table_after_own_change();
 table_judge_proved();
 table_judge_contradicted();
 table_unknown();
 table_from_stat_has_no_birth();
 table_names();
 table_same_now();
}

/* ---- the disk rows ---------------------------------------------------- */

/** Make the subfolder of one row: a fresh folder, so no filler of another row is in it. */
static int row_folder(const char *base, const char *name, char *out, size_t n)
{
 if (test_path_join(out, n, base, name) != 0)
  return(-1);
 if (mkdir(out, 0700) != 0)
 {
  printf("FAIL: row folder %s: %s\n", out, strerror(errno));
  fails++;
  return(-1);
 }
 return(0);
}

static int put_file(const char *path, const char *text)
{
 int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
 size_t len = strlen(text);

 if (fd < 0)
  return(-1);
 if (write(fd, text, len) != (ssize_t)len)
 {
  close(fd);
  return(-1);
 }
 return(close(fd));
}

static int append_to(const char *path, const char *text)
{
 int fd = open(path, O_WRONLY | O_APPEND);
 size_t len = strlen(text);

 if (fd < 0)
  return(-1);
 if (write(fd, text, len) != (ssize_t)len)
 {
  close(fd);
  return(-1);
 }
 return(close(fd));
}

/**
 * reuse_row - Make a reuse row's number come again, and count how it ended.
 * @dir: the row's folder.  @row: its name.  @name: where the objects are made.  @pc: the case.
 *
 * The call of every reuse row here: the one owner of the SKIP-or-FAIL decision
 * (seen_rows_reuse_or_skip) says what to print, and this counts it.  The walk
 * runs first and the probe only after the walk missed, so a row that reaches
 * the number is never skipped.
 * Return: 1 when the new object holds the old number and the row goes on, 0
 * after a SKIP or a FAIL.
 */
static int reuse_row(const char *dir, const char *row, const char *name, const SeenRowsCase *pc)
{
 SeenRowsOutcome out = seen_rows_reuse_or_skip(dir, row, name, pc);

 if (out == SEEN_ROWS_SKIP)
  skips++;
 if (out == SEEN_ROWS_FAIL)
  fails++;
 return(out == SEEN_ROWS_RAN);
}

/**
 * Look routes: the same file by descriptor, by name and by a folder held open
 * is one record, a missing name is a failed look that leaves the record unknown,
 * and a link is looked at itself or at what it leads to as asked.
 */
static void test_the_look_routes(const char *base)
{
 char dir[TEST_DIR_PATH_SIZE], file[TEST_DIR_PATH_SIZE], link[TEST_DIR_PATH_SIZE];
 WpeSeen by_fd, by_name, by_at, through, itself, gone;
 int fd, dfd;
 struct stat st;

 if (row_folder(base, "routes", dir, sizeof dir) != 0)
  return;
 test_path_join(file, sizeof file, dir, "t");
 test_path_join(link, sizeof link, dir, "l");
 chk_int("make the file", put_file(file, "text\n"), 0);
 chk_int("make the link", symlink("t", link), 0);
 fd = open(file, O_RDONLY);
 dfd = open(dir, O_RDONLY | O_DIRECTORY);
 chk_int("look by descriptor", wpe_seen_look_fd(fd, &by_fd), 0);
 chk_int("look by name", wpe_seen_look_name(file, 1, &by_name), 0);
 chk_int("look by a folder and a leaf", wpe_seen_look_at(dfd, "t", 1, &by_at), 0);
 chk_is("descriptor against name", wpe_seen_judge(&by_fd, &by_name), WPE_SEEN_SAME);
 chk_is("descriptor against leaf", wpe_seen_judge(&by_fd, &by_at), WPE_SEEN_SAME);
 chk_int("the three routes agree on a birth", by_fd.seen_has_birth == by_name.seen_has_birth &&
                                              by_name.seen_has_birth == by_at.seen_has_birth, 1);
 printf("note: a birth time is %s here\n", by_fd.seen_has_birth ? "reported" : "not reported");
 chk_int("a record of a regular file", wpe_seen_kind(&by_fd) == S_IFREG, 1);
 chk_int("the record is the file's number", stat(file, &st) == 0 && st.st_ino == by_fd.seen_ino, 1);
 chk_int("every route tells the names the file has", wpe_seen_nlink(&by_fd) == (nlink_t)st.st_nlink &&
         wpe_seen_nlink(&by_name) == (nlink_t)st.st_nlink && wpe_seen_nlink(&by_at) == (nlink_t)st.st_nlink, 1);
 chk_int("the record carries the file's bits", wpe_seen_mode(&by_fd) == (st.st_mode & 07777), 1);
 chk_int("look at the link itself", wpe_seen_look_name(link, 0, &itself), 0);
 chk_int("look through the link", wpe_seen_look_name(link, 1, &through), 0);
 chk_int("the link itself is a link", wpe_seen_kind(&itself) == S_IFLNK, 1);
 chk_int("through the link is the file", wpe_seen_kind(&through) == S_IFREG, 1);
 chk_is("through the link is the file's record", wpe_seen_judge(&by_name, &through), WPE_SEEN_SAME);
 chk_is("the link itself is not the file", wpe_seen_judge(&by_name, &itself), WPE_SEEN_OTHER);
 errno = 0;
 chk_int("a missing name: the look fails", wpe_seen_look_name("no-such-file-here", 1, &gone), -1);
 chk_int("a missing name: ENOENT", errno == ENOENT, 1);
 chk_int("a failed look leaves the record unknown", wpe_seen_known(&gone), 0);
 chk_is("a failed look is never SAME", wpe_seen_judge(&gone, &by_name), WPE_SEEN_UNSURE);
 close(fd);
 close(dfd);
}

/**
 * A file touched in place is the window's file while the system tells births
 * apart, and not proven under the hook that plays a system with no birth time.
 * The hook acts in every route (descriptor, name, folder and leaf).
 */
static void test_a_touched_file_and_the_no_birth_hook(const char *base)
{
 char dir[TEST_DIR_PATH_SIZE], file[TEST_DIR_PATH_SIZE];
 WpeSeen then, now, h_fd, h_name, h_at;
 int fd, dfd;

 if (row_folder(base, "touched", dir, sizeof dir) != 0)
  return;
 test_path_join(file, sizeof file, dir, "t");
 chk_int("make the file", put_file(file, "text\n"), 0);
 chk_int("look before", wpe_seen_look_name(file, 1, &then), 0);
 chk_int("append in place", append_to(file, "more\n"), 0);
 chk_int("look after", wpe_seen_look_name(file, 1, &now), 0);
 if (then.seen_has_birth && now.seen_has_birth)
  chk_is("a touched file is the same file (a birth time)", wpe_seen_judge(&then, &now), WPE_SEEN_SAME);
 else
 {
  chk_is("a touched file is not proven (no birth time here)", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
  skip("the touched-file row that needs a birth time: this file system reports none");
 }
 chk_int("the hook is set", setenv(WPE_SEEN_TEST_FAIL_ENV, "restore," WPE_SEEN_TEST_FAIL_NO_BIRTH, 1), 0);
 chk_int("look before, hook", wpe_seen_look_name(file, 1, &then), 0);
 chk_int("append in place again", append_to(file, "more\n"), 0);
 chk_int("look after, hook", wpe_seen_look_name(file, 1, &now), 0);
 chk_int("the hook drops the birth of a name look", then.seen_has_birth + now.seen_has_birth, 0);
 chk_is("a touched file is UNSURE without a birth time", wpe_seen_judge(&then, &now), WPE_SEEN_UNSURE);
 fd = open(file, O_RDONLY);
 dfd = open(dir, O_RDONLY | O_DIRECTORY);
 chk_int("look by descriptor, hook", wpe_seen_look_fd(fd, &h_fd), 0);
 chk_int("look by name, hook", wpe_seen_look_name(file, 1, &h_name), 0);
 chk_int("look by leaf, hook", wpe_seen_look_at(dfd, "t", 1, &h_at), 0);
 chk_int("the hook drops the birth in every route", h_fd.seen_has_birth + h_name.seen_has_birth + h_at.seen_has_birth, 0);
 chk_is("without a birth, an untouched file is still the same", wpe_seen_judge(&h_fd, &h_name), WPE_SEEN_SAME);
 unsetenv(WPE_SEEN_TEST_FAIL_ENV);
 close(fd);
 close(dfd);
}

/** The look of the old file, taken by a reuse case while it still exists (ctx is a WpeSeen). */
static void look_file_name(const char *path, void *ctx)
{
 wpe_seen_look_name(path, 1, (WpeSeen *)ctx);
}

/** The same for a link: its name and its target text (ctx is a WpeSeenName). */
static void look_link_name(const char *path, void *ctx)
{
 wpe_seen_name_look(path, (WpeSeenName *)ctx);
}

/** Do two records carry different birth times? */
static int births_differ(const WpeSeen *a, const WpeSeen *b)
{
 return(a->seen_birth.tv_sec != b->seen_birth.tv_sec || a->seen_birth.tv_nsec != b->seen_birth.tv_nsec);
}

/**
 * Another file with the window's file's number, made after the window's file
 * was removed.  Never SAME.  With a birth time on both sides and the births
 * apart it is OTHER; under the hook that plays a system without a birth time
 * it is UNSURE, and that is exact.  Needs a file system that gives a number
 * back.  The new file is given other text, so a twin made inside one clock
 * tick is told by its size and is UNSURE, not SAME.
 */
static void file_took_the_number(const char *base, const char *row, const char *hook)
{
 char dir[TEST_DIR_PATH_SIZE], file[TEST_DIR_PATH_SIZE];
 WpeSeen then, now;
 WpeSeenIs verdict;
 SeenRowsCase pc;

 if (row_folder(base, row, dir, sizeof dir) != 0)
  return;
 if (hook != NULL)
  chk_int("the hook is set", setenv(WPE_SEEN_TEST_FAIL_ENV, hook, 1), 0);
 test_path_join(file, sizeof file, dir, "t");
 wpe_seen_forget(&then);
 pc.make_old = seen_rows_make_file_text;
 pc.old_ctx = (void *)"old text\n";
 pc.look_old = look_file_name;
 pc.look_ctx = &then;
 pc.make_new = seen_rows_make_file;
 pc.new_ctx = NULL;
 if (reuse_row(dir, "a file that took the number", "t", &pc))
 {
  chk_int("the look before was taken", wpe_seen_known(&then), 1);
  chk_int("give the new file other text", append_to(file, "other, and longer\n"), 0);
  chk_int("look after", wpe_seen_look_name(file, 1, &now), 0);
  chk_int("the new file has the old number", now.seen_ino == then.seen_ino, 1);
  verdict = wpe_seen_judge(&then, &now);
  chk_int("another file with the number is not SAME", verdict != WPE_SEEN_SAME, 1);
  if (hook != NULL)
   chk_is("another file with the number, no birth time", verdict, WPE_SEEN_UNSURE);
  else if (then.seen_has_birth && now.seen_has_birth && births_differ(&then, &now))
   chk_is("another file with the number, the births differ", verdict,
          seen_rows_expect("another file with the number", dir, SEEN_BIRTHS_DIFFER));
  else
   printf("note: the new file's birth time equals the old one's or is not reported; only 'not SAME' was asserted\n");
 }
 if (hook != NULL)
  unsetenv(WPE_SEEN_TEST_FAIL_ENV);
}

static void test_a_file_that_took_the_number(const char *base)
{
 file_took_the_number(base, "reuse", NULL);
 file_took_the_number(base, "reuse-nobirth", WPE_SEEN_TEST_FAIL_NO_BIRTH);
}

/**
 * A link made again at the same number: with the same target text it is the
 * same link by the name judge, with another text it is another link.  The name
 * judge only words a sentence.  The text is the whole target, and its length
 * is the size the look reported.
 */
static void link_took_the_number(const char *base, const char *row, const char *old_text,
                                 const char *new_text, WpeSeenIs want_is)
{
 char dir[TEST_DIR_PATH_SIZE], link[TEST_DIR_PATH_SIZE];
 WpeSeenName then, now;
 SeenRowsCase pc;

 if (row_folder(base, row, dir, sizeof dir) != 0)
  return;
 test_path_join(link, sizeof link, dir, "l");
 wpe_seen_forget(&then.seen);
 pc.make_old = seen_rows_make_link;
 pc.old_ctx = (void *)old_text;
 pc.look_old = look_link_name;
 pc.look_ctx = &then;
 pc.make_new = seen_rows_make_link;
 pc.new_ctx = (void *)new_text;
 if (!reuse_row(dir, row, "l", &pc))
  return;
 chk_int("the name look keeps the target text", then.has_text, 1);
 chk_int("the text is the whole target", then.len == strlen(old_text) && strcmp(then.text, old_text) == 0, 1);
 wpe_seen_name_look(link, &now);
 chk_int("the new link has the old number", now.seen.seen_ino == then.seen.seen_ino, 1);
 chk_is(row, wpe_seen_name_judge(&then, &now), want_is);
}

static void test_a_link_that_took_the_number(const char *base)
{
 link_took_the_number(base, "link-same-text", "target-a", "target-a", WPE_SEEN_SAME);
 link_took_the_number(base, "link-other-text", "target-a", "target-b", WPE_SEEN_OTHER);
}

/**
 * Row 10: a folder held open never shares its number.  The folder is removed
 * and made again 64 times; each new folder is not the held one, and none has
 * its number.  Control: once the hold is released the number does come again
 * (on a file system that gives numbers back), so the loop above could have
 * seen a reuse had the hold not been there.
 */
static void test_a_held_folder_never_shares_its_number(const char *base)
{
 char dir[TEST_DIR_PATH_SIZE], ws[TEST_DIR_PATH_SIZE];
 WpeSeenHeld held = WPE_SEEN_HELD_NONE;
 struct stat st;
 SeenRowsCase pc;
 int round;

 if (row_folder(base, "held", dir, sizeof dir) != 0)
  return;
 test_path_join(ws, sizeof ws, dir, "ws");
 chk_int("make the workspace", mkdir(ws, 0700), 0);
 chk_int("hold it", wpe_seen_hold_dir(ws, &held), 0);
 chk_int("the held folder is the path", wpe_seen_held_is(&held, ws), 1);
 for (round = 0; round < HELD_ROUNDS; round++)
 {
  if (rmdir(ws) != 0 || mkdir(ws, 0700) != 0 || stat(ws, &st) != 0)
  {
   printf("FAIL: round %d: cannot remove and make the folder again: %s\n", round, strerror(errno));
   fails++;
   break;
  }
  if (wpe_seen_held_is(&held, ws) != 0)
  {
   printf("FAIL: round %d: a new folder was taken for the held one\n", round);
   fails++;
   break;
  }
  if (!other_object_while_held(held.fd, &st))
  {
   printf("FAIL: round %d: a new folder got the held folder's number\n", round);
   fails++;
   break;
  }
 }
 wpe_seen_release(&held);
 chk_int("a released hold holds nothing", held.fd, -1);
 chk_int("a released hold is the path of nothing", wpe_seen_held_is(&held, ws), 0);
 wpe_seen_release(&held);
 chk_int("a second release is harmless", held.fd, -1);
 chk_int("remove the workspace", rmdir(ws), 0);
 pc.make_old = seen_rows_make_folder;
 pc.old_ctx = NULL;
 pc.look_old = NULL;
 pc.look_ctx = NULL;
 pc.make_new = seen_rows_make_folder;
 pc.new_ctx = NULL;
 (void)reuse_row(dir, "a folder that took the number once the hold is released", "ctl", &pc);
}

/** The kinds the probe is asked about, one after the other; index order of the arrays below. */
#define PROBE_KINDS 4

/**
 * case_of_kind - The case a reuse row of that kind walks with (old and new object the same kind).
 * @pc: receives it.  @kind: 0 file, 1 link, 2 folder, 3 FIFO.
 */
static void case_of_kind(SeenRowsCase *pc, int kind)
{
 memset(pc, 0, sizeof *pc);
 if (kind == 0)
  pc->make_old = pc->make_new = seen_rows_make_file;
 else if (kind == 1)
  pc->make_old = pc->make_new = seen_rows_make_link;
 else if (kind == 2)
  pc->make_old = pc->make_new = seen_rows_make_folder;
 else
  pc->make_old = pc->make_new = seen_rows_make_fifo;
 pc->old_ctx = pc->new_ctx = (void *)"target-a";
}

#define KIND_LINK 1

/** The S_IFMT the objects of a kind index have. */
static mode_t kind_mode(int kind)
{
 static const mode_t modes[PROBE_KINDS] = { S_IFREG, S_IFLNK, S_IFDIR, S_IFIFO };

 return(modes[kind]);
}

/** The word a kind index is called in a row's name. */
static const char *kind_index_word(int kind)
{
 static const char *const words[PROBE_KINDS] = { "file", "link", "folder", "FIFO" };

 return(words[kind]);
}

/**
 * A walk that reaches the number is RAN, whatever the probe would have said:
 * the probe is asked only after a walk missed.  The hook says "never gives
 * back" for every kind, and the file walk on a file system that does give a
 * number back still runs.  Only asked where the file system is known to give
 * the number back (ext2/3/4), so a miss there is a fault of the row.
 */
static void test_a_reached_walk_is_never_skipped(const char *dir)
{
 SeenRowsCase pc;

 case_of_kind(&pc, 0);
 chk_int("the hook is set",
         setenv(SEEN_ROWS_PROBE_ENV, "never:file,never:link,never:folder,never:fifo", 1), 0);
 chk_int("a walk that reached the number ran, the hook's answer unasked",
         seen_rows_reuse_or_skip(dir, "a reached walk under the hook", "hook", &pc), SEEN_ROWS_RAN);
 chk_int("the hook is unset", unsetenv(SEEN_ROWS_PROBE_ENV), 0);
}

/**
 * Where the file system is ext2/3/4 (a disk file system of Linux that gives a
 * freed number back), the probe gives back for every kind a row walks with:
 * file, link, folder and FIFO.  So no reuse row is skipped there.  Measured on
 * ext4 before this row was written: 20 trials in 20 for each kind, with no
 * flush, as the rows make them.  On any other file system the row prints what
 * the probe said and asserts nothing: each system is its own fact.
 */
static void test_ext4_gives_every_kind_back(const char *base)
{
 char dir[TEST_DIR_PATH_SIZE], fs[SEEN_ROWS_NAME_SIZE], what[SEEN_ROWS_NAME_SIZE + 64];
 SeenRowsCase pc;
 int kind, never;

 if (row_folder(base, "ext4", dir, sizeof dir) != 0)
  return;
 seen_rows_fs_name(dir, fs, sizeof fs);
 for (kind = 0; kind < PROBE_KINDS; kind++)
 {
  case_of_kind(&pc, kind);
  never = seen_rows_never_gives_back(dir, &pc);
  if (strcmp(fs, "ext2/3/4") == 0)
  {
   snprintf(what, sizeof what, "ext2/3/4 gives a freed %s number back", kind_index_word(kind));
   chk_int(what, never, 0);
  }
  else
   printf("note: %s: a freed %s number %s on this file system\n", fs, kind_index_word(kind),
          never ? "was not given back" : "was given back");
 }
 if (strcmp(fs, "ext2/3/4") == 0)
  test_a_reached_walk_is_never_skipped(dir);
}

/**
 * The probe is asked about the kind the row walks with.  The decision of a
 * missed walk is played with the hook that answers per kind
 * (XWPE_TEST_REUSE_PROBE): with "never:link" and every other kind giving
 * back, a missed link walk is a SKIP and a missed file, folder or FIFO walk
 * is a FAIL.  A probe that ignored the case and always made a file would give
 * a link the file's answer, and the first check would turn red.
 */
static void test_the_probe_asks_about_the_kind(const char *base)
{
 char dir[TEST_DIR_PATH_SIZE];
 SeenRowsProbe pr;
 SeenRowsCase pc;
 int kind;

 if (row_folder(base, "kinds", dir, sizeof dir) != 0)
  return;
 chk_int("the hook is set",
         setenv(SEEN_ROWS_PROBE_ENV, "never:link,gives:file,gives:folder,gives:fifo", 1), 0);
 for (kind = 0; kind < PROBE_KINDS; kind++)
 {
  case_of_kind(&pc, kind);
  if (kind == KIND_LINK)
   chk_int("a missed link walk is a SKIP", seen_rows_decide_miss(dir, &pc, &pr), SEEN_ROWS_SKIP);
  else
   chk_int("a missed walk of another kind is a FAIL", seen_rows_decide_miss(dir, &pc, &pr), SEEN_ROWS_FAIL);
  chk_int("the answer was the hook's", pr.hooked, 1);
  chk_int("the probe made the kind of the case", pr.kind == kind_mode(kind), 1);
 }
 chk_int("the hook is unset", unsetenv(SEEN_ROWS_PROBE_ENV), 0);
}

/** The hold on things that are not a folder to keep, and what a hold does across exec. */
static void test_the_hold_refuses_what_is_not_a_folder(const char *base)
{
 char dir[TEST_DIR_PATH_SIZE], file[TEST_DIR_PATH_SIZE], link[TEST_DIR_PATH_SIZE];
 WpeSeenHeld h = WPE_SEEN_HELD_NONE;
 int flags;

 if (row_folder(base, "hold-misc", dir, sizeof dir) != 0)
  return;
 test_path_join(file, sizeof file, dir, "f");
 test_path_join(link, sizeof link, dir, "l");
 chk_int("make a file", put_file(file, "x"), 0);
 chk_int("make a link to the folder", symlink(".", link), 0);
 chk_int("a missing folder is not held", wpe_seen_hold_dir("no-such-folder-here", &h), -1);
 chk_int("a missing folder: nothing held", h.fd, -1);
 chk_int("a file is not held", wpe_seen_hold_dir(file, &h), -1);
 chk_int("a link to a folder is not followed", wpe_seen_hold_dir(link, &h), -1);
 chk_int("a folder is held", wpe_seen_hold_dir(dir, &h), 0);
 flags = fcntl(h.fd, F_GETFD);
 chk_int("the hold is closed on exec", flags >= 0 && (flags & FD_CLOEXEC) != 0, 1);
 chk_int("another path is not the held folder", wpe_seen_held_is(&h, file), 0);
 chk_int("a missing path is not the held folder", wpe_seen_held_is(&h, "no-such-folder-here"), 0);
 wpe_seen_release(&h);
}

/** Two looks of one moment by name: a file and its other name, and two files. */
static void test_two_names_of_one_file(const char *base)
{
 char dir[TEST_DIR_PATH_SIZE], a[TEST_DIR_PATH_SIZE], b[TEST_DIR_PATH_SIZE], c[TEST_DIR_PATH_SIZE];

 if (row_folder(base, "names", dir, sizeof dir) != 0)
  return;
 test_path_join(a, sizeof a, dir, "a");
 test_path_join(b, sizeof b, dir, "b");
 test_path_join(c, sizeof c, dir, "c");
 chk_int("make a", put_file(a, "x"), 0);
 chk_int("make c", put_file(c, "x"), 0);
 chk_int("a second name for a", link(a, b), 0);
 chk_int("two names of one file", wpe_seen_names_same_now(a, b), 1);
 chk_int("two files", wpe_seen_names_same_now(a, c), 0);
 chk_int("a missing name", wpe_seen_names_same_now(a, "no-such-file-here"), 0);
}

static int row_folder(const char *base, const char *name, char *out, size_t n);
static int put_file(const char *path, const char *text);

#if WPE_PLATFORM_STATX_BTIME

/** What one statx mode must make each look say. */
typedef struct {
 int mode;
 const char *name;
 int want_rc;       /* 0, or -1 for a look that fails */
 int want_errno;    /* with -1 */
 int want_birth;    /* a birth time kept */
 long want_sec;     /* its seconds, when kept and not 0 */
} ShimRow;

static const ShimRow shim_rows[] = {
 { SHIM_NO_MASK,      "a birth the mask does not name is not kept",  0, 0, 0, 0 },
 { SHIM_BIRTH_ZERO,   "a birth of zero is not kept",                 0, 0, 0, 0 },
 { SHIM_BIRTH_MINUS1, "a birth of -1 is not kept",                   0, 0, 0, 0 },
 { SHIM_BIRTH_REAL,   "a birth the mask names is kept",              0, 0, 1, SHIM_REAL_BIRTH_SEC },
 { SHIM_ENOSYS,       "no statx (ENOSYS): the look goes on without a birth", 0, 0, 0, 0 },
 { SHIM_EPERM,        "statx refused (EPERM): the look goes on without a birth", 0, 0, 0, 0 },
 { SHIM_ENOENT,       "statx ENOENT is the object's own: the look fails", -1, ENOENT, 0, 0 },
 { SHIM_EACCES,       "statx EACCES is the object's own: the look fails", -1, EACCES, 0, 0 },
 { SHIM_EBADF,        "statx EBADF is the object's own: the look fails", -1, EBADF, 0, 0 },
 { SHIM_ELOOP,        "statx ELOOP is the object's own: the look fails", -1, ELOOP, 0, 0 },
 { SHIM_ENOTDIR,      "statx ENOTDIR is the object's own: the look fails", -1, ENOTDIR, 0, 0 },
};

#define SHIM_ROUTE_FD   0
#define SHIM_ROUTE_NAME 1
#define SHIM_ROUTE_AT   2
#define SHIM_ROUTES     3

static const char *const shim_route_names[SHIM_ROUTES] = { "descriptor", "name", "folder and leaf" };

/** Look at the file by one of the three routes. */
static int shim_look(int route, const char *file, int fd, int dfd, WpeSeen *s)
{
 if (route == SHIM_ROUTE_FD)
  return(wpe_seen_look_fd(fd, s));
 if (route == SHIM_ROUTE_NAME)
  return(wpe_seen_look_name(file, 1, s));
 return(wpe_seen_look_at(dfd, "t", 1, s));
}

/** One mode on one route: the return, the errno, the record's known, birth and (when kept) its seconds. */
static void shim_check_route(const ShimRow *row, int route, const char *file, int fd, int dfd)
{
 char what[200];
 WpeSeen s;
 int rc;

 shim_statx_mode = row->mode;
 errno = 0;
 rc = shim_look(route, file, fd, dfd, &s);
 shim_statx_mode = SHIM_PASS;
 snprintf(what, sizeof what, "%s (%s): the return", row->name, shim_route_names[route]);
 chk_int(what, rc, row->want_rc);
 if (row->want_rc != 0)
 {
  snprintf(what, sizeof what, "%s (%s): errno", row->name, shim_route_names[route]);
  chk_int(what, errno, row->want_errno);
  snprintf(what, sizeof what, "%s (%s): the record is unknown", row->name, shim_route_names[route]);
  chk_int(what, wpe_seen_known(&s), 0);
  return;
 }
 snprintf(what, sizeof what, "%s (%s): the record is known", row->name, shim_route_names[route]);
 chk_int(what, wpe_seen_known(&s), 1);
 snprintf(what, sizeof what, "%s (%s): a birth is kept", row->name, shim_route_names[route]);
 chk_int(what, s.seen_has_birth, row->want_birth);
 if (row->want_birth)
 {
  snprintf(what, sizeof what, "%s (%s): its seconds", row->name, shim_route_names[route]);
  chk_int(what, (long)s.seen_birth.tv_sec, row->want_sec);
  chk_int("a kernel birth cannot be set by a user", s.seen_birth_trust == WPE_SEEN_BIRTH_KERNEL, 1);
 }
}

/** A fallback look is the same object a statx look finds. */
static void shim_check_fallback_is_the_same_object(const char *file)
{
 WpeSeen normal, fallback;

 chk_int("look with statx", wpe_seen_look_name(file, 1, &normal), 0);
 shim_statx_mode = SHIM_ENOSYS;
 chk_int("look by fstatat", wpe_seen_look_name(file, 1, &fallback), 0);
 shim_statx_mode = SHIM_PASS;
 chk_int("the fallback has the same inode", fallback.seen_ino == normal.seen_ino, 1);
 chk_int("the fallback has the same device", fallback.seen_dev == normal.seen_dev, 1);
 chk_int("the fallback has the same size", fallback.seen_size == normal.seen_size, 1);
}

/**
 * statx, played: the birth the mask vouches for is kept and nothing else, a
 * statx that is not there (ENOSYS, EPERM) falls back to fstat with no birth,
 * and an error about the object (ENOENT, EACCES, EBADF, ELOOP, ENOTDIR) is the
 * answer and is never retried with fstat, which would find the file.
 */
static void test_statx_answers(const char *base)
{
 char dir[TEST_DIR_PATH_SIZE], file[TEST_DIR_PATH_SIZE];
 size_t i;
 int route, fd, dfd;

 if (row_folder(base, "shim", dir, sizeof dir) != 0)
  return;
 test_path_join(file, sizeof file, dir, "t");
 chk_int("make the file", put_file(file, "text\n"), 0);
 fd = open(file, O_RDONLY);
 dfd = open(dir, O_RDONLY | O_DIRECTORY);
 for (i = 0; i < sizeof shim_rows / sizeof shim_rows[0]; i++)
  for (route = 0; route < SHIM_ROUTES; route++)
   shim_check_route(&shim_rows[i], route, file, fd, dfd);
 shim_check_fallback_is_the_same_object(file);
 close(fd);
 close(dfd);
}

/**
 * A link whose readlink comes back a byte shorter than the size the look
 * reported (swapped between the two calls) has no text, and the judge falls
 * to the stamps; the same link read whole has its text.
 */
static void test_a_link_read_short_has_no_text(const char *base)
{
 char dir[TEST_DIR_PATH_SIZE], link[TEST_DIR_PATH_SIZE];
 WpeSeenName whole, cut;

 if (row_folder(base, "linkshort", dir, sizeof dir) != 0)
  return;
 test_path_join(link, sizeof link, dir, "l");
 chk_int("make the link", symlink("target-a", link), 0);
 wpe_seen_name_look(link, &whole);
 chk_int("a link read whole has its text", whole.has_text, 1);
 shim_readlink_short = 1;
 wpe_seen_name_look(link, &cut);
 shim_readlink_short = 0;
 chk_int("a link read short has no text", cut.has_text, 0);
 chk_int("a link read short is still known", wpe_seen_known(&cut.seen), 1);
}

/**
 * A held descriptor whose fstat fails proves nothing, even when the buffer was
 * filled with the folder's real numbers (a stale network handle can answer
 * from its cache and still report the error).  A closed descriptor does too.
 */
static void test_a_held_folder_that_does_not_answer(const char *base)
{
 char dir[TEST_DIR_PATH_SIZE];
 WpeSeenHeld h = WPE_SEEN_HELD_NONE;
 WpeSeenHeld closed = WPE_SEEN_HELD_NONE;

 if (row_folder(base, "stale", dir, sizeof dir) != 0)
  return;
 chk_int("hold the folder", wpe_seen_hold_dir(dir, &h), 0);
 chk_int("it answers", wpe_seen_held_is(&h, dir), 1);
 shim_fstat_lie_fd = h.fd;
 chk_int("an fstat that fails, with the real numbers, is not the folder", wpe_seen_held_is(&h, dir), 0);
 shim_fstat_lie_fd = SHIM_NO_FD;
 chk_int("it answers again", wpe_seen_held_is(&h, dir), 1);
 chk_int("hold the folder again", wpe_seen_hold_dir(dir, &closed), 0);
 close(closed.fd);
 chk_int("a closed descriptor is not the folder", wpe_seen_held_is(&closed, dir), 0);
 wpe_seen_release(&h);
}

#endif

/**
 * The twin of the back probe: a system the build calls back-only moves no
 * birth forward by any call it documents.  A measurement of the system, never
 * of the judge.  It fails when the build claims back-only and a call moved the
 * birth forward; it prints what it found on the others, which is the evidence
 * that promotes one (NetBSD) or keeps one out (macOS).  Skips by name where the
 * build reads no birth from struct stat, which is Linux.
 */
static void row_back_only_birth_is_not_forgeable_forward(const char *base)
{
 char dir[TEST_DIR_PATH_SIZE];
 int moved;

 if (row_folder(base, "forward", dir, sizeof dir) != 0)
  return;
 moved = seen_rows_birth_moves_forward(dir);
 if (moved < 0)
 {
  skip("the forward birth probe needs a birth time in struct stat; this build reads none from it");
  return;
 }
 printf("note: a call moved the birth time forward here: %s\n", moved ? "yes" : "no");
 chk_int("a system the build calls back-only moves no birth forward", WPE_PLATFORM_BIRTH_BACK_ONLY && moved, 0);
}

/** The rows that play the system calls (needs statx; elsewhere they say so). */
static void test_the_played_calls(const char *base)
{
#if WPE_PLATFORM_STATX_BTIME
 test_statx_answers(base);
 test_a_link_read_short_has_no_text(base);
 test_a_held_folder_that_does_not_answer(base);
#else
 (void)base;
 skip("the played statx, readlink and fstat rows need statx and a birth time; this system has none");
#endif
}

/** The disk rows, in one folder made on a disk file system. */
static void test_the_disk_rows(void)
{
 char base[TEST_DIR_PATH_SIZE];

 if (seen_rows_dir(base, sizeof base, "test_seen") != 0)
  return;
 test_the_look_routes(base);
 test_a_touched_file_and_the_no_birth_hook(base);
 test_a_file_that_took_the_number(base);
 test_a_link_that_took_the_number(base);
 test_a_held_folder_never_shares_its_number(base);
 test_the_probe_asks_about_the_kind(base);
 test_ext4_gives_every_kind_back(base);
 test_the_hold_refuses_what_is_not_a_folder(base);
 test_two_names_of_one_file(base);
 row_back_only_birth_is_not_forgeable_forward(base);
 test_the_played_calls(base);
 seen_rows_remove(base);
}

int main(void)
{
 test_the_table();
 test_the_disk_rows();
 printf("test_seen: %d failure(s), %d skip(s)\n", fails + test_dir_failures, skips);
 return(fails + test_dir_failures > 0 ? 1 : 0);
}
