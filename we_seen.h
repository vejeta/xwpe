/* we_seen.h -- is this the object the editor saw earlier?

   A file system gives an inode number back after the file is gone, so
   "same device and same inode" proves nothing about a record kept from an
   earlier moment.  This is the one place that records what a look found and
   judges a later look against it.  Every other file keeps a WpeSeen and asks
   wpe_seen_judge; none reads the number, the stamps or the birth time.

   The judge answers with three values.  SAME is proof enough to write over
   the object, UNSURE is "not proven the object seen" and every guard that
   writes treats it as replaced, OTHER is a different object.  The rule: an
   object made after the look has a status-change time at or after the first
   one's, the kernel stamps that time and no call lets a user set it, so a
   record whose stamps and owner still agree is the object seen, and a file
   touched since is told from a new one by its birth time where the system
   has one.  Where the owner of a file can set the birth time, or the file
   system keeps one owner for every file (vfat, exfat, some network mounts), a
   twin made by that owner is not told apart.

   Plain C: <sys/stat.h> and we_path.h only, no curses and no editor type, so
   a unit test links it alone, as it does we_path.c. */
#ifndef WE_SEEN_H
#define WE_SEEN_H

#include <stddef.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "we_path.h"

/** The answer of a judge. */
typedef enum { WPE_SEEN_SAME = 1, WPE_SEEN_OTHER, WPE_SEEN_UNSURE } WpeSeenIs;

/**
 * WpeSeenBirthTrust - Who can move a birth time on this system, and which way.
 *
 * What the user gets: where no system call moves a birth time forward, a file
 * the run made later is told from a file it removed, even when the file system
 * handed the new file the old one's number.  When unsure, the class is the
 * weaker one (ANY_WAY): a wrong strong class tells a real move as a removal.
 */
typedef enum {
 WPE_SEEN_BIRTH_KERNEL = 0,   /* only the kernel sets it (statx on the local file systems) */
 WPE_SEEN_BIRTH_BACK_ONLY,    /* its owner can set it earlier, and no system call sets it later */
 WPE_SEEN_BIRTH_ANY_WAY       /* its owner can set it either way, or the way is not established */
} WpeSeenBirthTrust;

/**
 * WpeSeen - One object as a look found it.
 *
 * The members carry the prefix seen_ so the fitness rule can keep every
 * reader of them inside we_seen.c.  A record with known 0 was never taken,
 * or the look failed: the judge never answers SAME for it.
 */
typedef struct {
 int    known;
 mode_t seen_kind;                  /* S_IFMT of the object */
 dev_t  seen_dev;
 ino_t  seen_ino;
 uid_t  seen_uid;
 gid_t  seen_gid;
 mode_t seen_mode;                  /* the twelve permission and special bits */
 off_t  seen_size;
 nlink_t seen_nlink;                /* how many names the object had; not judged, only told */
 struct timespec seen_ctime;        /* status change: only the kernel sets it */
 struct timespec seen_mtime;
 int    seen_has_birth;             /* the system said when the object was made */
 WpeSeenBirthTrust seen_birth_trust; /* who can move that time on this system, and which way */
 struct timespec seen_birth;
} WpeSeen;

/**
 * WpeSeenName - A name itself, never what it leads to, with a link's target.
 *
 * has_text is 1 only when the whole target was read and its length is the
 * size the look reported, so a link swapped between the two calls does not
 * pass for the first.
 */
typedef struct {
 WpeSeen seen;
 int     has_text;
 size_t  len;
 char    text[WPE_PATH_SIZE];
} WpeSeenName;

/** A folder kept open so its number cannot be given to another folder. */
typedef struct {
 int fd;
} WpeSeenHeld;

/** The value of a WpeSeenHeld that holds nothing. */
#define WPE_SEEN_HELD_NONE { -1 }

/** The variable the test hooks of the owners are read from (as in we_write.c). */
#define WPE_SEEN_TEST_FAIL_ENV "XWPE_TEST_FAIL"

/** What WPE_SEEN_TEST_FAIL_ENV carries to make every look drop the birth time, so a
    Linux run plays a system that has none (OpenBSD, ext3, most FUSE). */
#define WPE_SEEN_TEST_FAIL_NO_BIRTH "seen-no-birth"

/** What WPE_SEEN_TEST_FAIL_ENV carries to make every statx birth BACK_ONLY, so a Linux run
    plays FreeBSD, where the owner can move a birth back and no call moves it forward. */
#define WPE_SEEN_TEST_FAIL_BIRTH_BACK_ONLY "seen-birth-back-only"

int  wpe_seen_look_fd(int fd, WpeSeen *s);
int  wpe_seen_look_name(const char *path, int follow, WpeSeen *s);
int  wpe_seen_look_at(int dirfd, const char *leaf, int follow, WpeSeen *s);
void wpe_seen_name_look(const char *path, WpeSeenName *n);
void wpe_seen_from_stat(WpeSeen *s, const struct stat *st);
void wpe_seen_forget(WpeSeen *s);
int  wpe_seen_known(const WpeSeen *s);
mode_t wpe_seen_kind(const WpeSeen *s);
mode_t wpe_seen_mode(const WpeSeen *s);
nlink_t wpe_seen_nlink(const WpeSeen *s);
off_t wpe_seen_size(const WpeSeen *s);

WpeSeenIs wpe_seen_judge(const WpeSeen *then, const WpeSeen *now);
WpeSeenIs wpe_seen_judge_proved(const WpeSeen *then, const WpeSeen *now);
WpeSeenIs wpe_seen_judge_contradicted(const WpeSeen *then, const WpeSeen *now);
WpeSeenIs wpe_seen_judge_after_own_change(const WpeSeen *before, const WpeSeen *after);
WpeSeenIs wpe_seen_name_judge(const WpeSeenName *then, const WpeSeenName *now);

int  wpe_seen_same_now(const struct stat *a, const struct stat *b);
int  wpe_seen_names_same_now(const char *a, const char *b);

int  wpe_seen_hold_dir(const char *path, WpeSeenHeld *h);
int  wpe_seen_held_is(const WpeSeenHeld *h, const char *path);
void wpe_seen_release(WpeSeenHeld *h);

#endif
