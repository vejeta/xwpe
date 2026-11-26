/* test_read_owner.c -- unit test for the owner of reading a file or a folder
 * by a name the editor did not choose (we_read.c).  Links we_read.c,
 * we_seen.c and we_path.c; always runs.
 *
 * What goes wrong if the owner breaks: a project or an options file that is a
 * pipe makes the editor wait for ever, one that is a device fills the memory,
 * an include that names the editor's own terminal takes the user's keys, and
 * a file over the bound is read cut short and then saved short.
 *
 * The matrix is every kind of object that can sit under a name (18), with the
 * link policy (2), the four faces (4) and the three bounds (3): 432 cells.  A
 * list of cases is how a kind gets forgotten, so each cell asks the reference
 * function below what it must give, and the reference is written from the
 * design, not from the owner: why it refused, what it found, whether the
 * object was opened at all (a device, a pipe or a terminal never is, for a
 * name that does not change), how many bytes came back, and whether the read
 * was cut.  Beside the matrix: the identity of what was read, a name that
 * changes between the look and the open, the memory stream, a file that grows
 * while it is read, and the folder face.
 *
 * The load face (wpe_read_load_open) has its own rows after those: every
 * object asked by its name and by a descriptor name (/dev/fd/N), a pipe made
 * before the start noted and one made after it, the same two on a system
 * where every pipe has device 0 and number 0 (played by a test hook), a
 * descriptor open for writing only, the same on a system that does not tell
 * the writing end of a pipe (played by a second hook), a pipe's two ends
 * stood at 0, 1 and 2, descriptors 0 to 2 refilled with the null device, a
 * descriptor name the system does not serve, /dev/stdin after the terminal
 * backend kept the data apart, a FIFO the editor made, and a name swapped for
 * a link to an unnamed pipe between the look and the open.
 *
 * Each cell runs under test_watchdog_case, so a wait is a FAIL that names
 * the cell, never a hang.
 */
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <utime.h>

#include "test_watchdog.h"
#include "interpose.h"
#include "testdir.h"
#include "../we_platform.h"
#include "../we_read.h"

#define BOUND          64        /* the bound the bounded cells use */
#define CELL_SECS      5
#define WRITER_NAME    "fifo_w"
#define TEXT_REG       "hello\n"
#define TEXT_NUL       "a\0b\nc\0d\n"
#define LEN_REG        6
#define LEN_NUL        8

static int fails;
static int skips;
static int cells;
static char dir[TEST_DIR_PATH_SIZE];

/* ---- the open, watched --------------------------------------------------- */

typedef int (*openat_fn)(int, const char *, int, ...);

static const char *watch_name;      /* opens of this name are counted */
static int  watch_opens;
static int  all_opens;              /* every open of this program, for the folder rows */
static char swap_at[TEST_DIR_PATH_SIZE];    /* this name is replaced just before its next open ... */
static char swap_from[TEST_DIR_PATH_SIZE];  /* ... by the object at this name, renamed over it */
static char fail_at[TEST_DIR_PATH_SIZE];    /* the next open of this name fails ... */
static int  fail_with;                      /* ... with this errno, and opens nothing */

/**
 * openat_hooked - The one open of this program, with the swap and the failure.
 * @dirfd, @path, @flags, @mode: as openat.
 *
 * The swap is the moment between the owner's look at a name and its open of
 * it, which a user of the folder can use; the failure plays another system's
 * word for a link met by an open.
 */
static int openat_hooked(int dirfd, const char *path, int flags, mode_t mode)
{
 static openat_fn real;
 WPE_HOOK_GUARD(openat);

 if (real == NULL)
  real = WPE_REAL(openat);
 all_opens++;
 if (watch_name != NULL && strcmp(path, watch_name) == 0)
  watch_opens++;
 if (swap_at[0] != '\0' && strcmp(path, swap_at) == 0)
 {
  swap_at[0] = '\0';
  if (rename(swap_from, path) != 0)
   perror("swap");
 }
 if (fail_at[0] != '\0' && strcmp(path, fail_at) == 0)
 {
  fail_at[0] = '\0';
  errno = fail_with;
  return(-1);
 }
 return(real(dirfd, path, flags, mode));
}

int openat(int dirfd, const char *path, int flags, ...)
{
 mode_t mode = 0;
 va_list ap;

 if (flags & O_CREAT)
 {
  va_start(ap, flags);
  mode = (mode_t)va_arg(ap, int);
  va_end(ap);
 }
 return(openat_hooked(dirfd, path, flags, mode));
}

WPE_HOOK_TWIN_OPENAT

/* read, watched: the first read after it is armed makes the file longer first,
   the moment a file that grows while it is read can use. */
typedef ssize_t (*read_fn)(int, void *, size_t);

static char grow_path[TEST_DIR_PATH_SIZE];

ssize_t read(int fd, void *buf, size_t n)
{
 static read_fn real;
 WPE_HOOK_GUARD(read);

 if (real == NULL)
  real = WPE_REAL(read);
 if (grow_path[0] != '\0')
 {
  FILE *more = fopen(grow_path, "a");

  grow_path[0] = '\0';
  if (more != NULL)
  {
   char fill[BOUND * 2];

   memset(fill, 'g', sizeof fill);
   fwrite(fill, 1, sizeof fill, more);
   fclose(more);
  }
 }
 return(real(fd, buf, n));
}

WPE_HOOK_TWIN_READ

/* ---- the objects ---------------------------------------------------------- */

typedef enum {
 OB_REG, OB_EMPTY, OB_NUL, OB_EXACT, OB_OVER, OB_FIFO_IDLE, OB_FIFO_WRITER, OB_SOCKET,
 OB_ZERO, OB_NULL, OB_PTY, OB_FOLDER, OB_MISSING, OB_DENIED, OB_LINK_REG, OB_LINK_FIFO,
 OB_LINK_ZERO, OB_LOOP, OB_COUNT
} Obj;

typedef enum { FACE_DESC, FACE_STREAM, FACE_TEXT, FACE_FOLDER, FACE_COUNT } Face;
typedef enum { BOUND_NONE, BOUND_REFUSE, BOUND_CUT, BOUND_COUNT } BoundMode;

static const char *const obj_label[OB_COUNT] = {
 "regular", "empty", "nul-bytes", "exact-bound", "bound+1", "fifo-idle", "fifo-writer", "socket",
 "/dev/zero", "/dev/null", "pty-slave", "folder", "missing", "write-only", "link-regular",
 "link-fifo", "link-zero", "link-loop"
};
static const char *const face_label[FACE_COUNT] = { "descriptor", "stream", "text", "folder" };
static const char *const bound_label[BOUND_COUNT] = { "unbounded", "refuse", "cut" };

static char obj_path[OB_COUNT][TEST_DIR_PATH_SIZE];
static int  obj_keep[OB_COUNT];      /* a descriptor the setup holds (a writer, a pty master) */
static int  denied_holds;            /* 1 when this user is kept out of a 0200 file */

/**
 * put_text - Make a regular file with these bytes.
 * @name: the file in the test folder; @bytes/@len: the content.
 * Return: 0, or -1 (counted).
 */
static int put_text(const char *name, const char *bytes, size_t len)
{
 char path[TEST_DIR_PATH_SIZE];
 FILE *fp;

 if (test_path_join(path, sizeof path, dir, name) != 0)
  return(-1);
 fp = fopen(path, "w");
 if (fp == NULL || (len > 0 && fwrite(bytes, 1, len, fp) != len))
 {
  test_dir_fail("FAIL: setup %s: %s\n", name, strerror(errno));
  if (fp != NULL)
   fclose(fp);
  return(-1);
 }
 fclose(fp);
 return(0);
}

/**
 * put_fill - Make a regular file of one repeated byte.
 * @name: the file; @len: its length.
 */
static int put_fill(const char *name, size_t len)
{
 char fill[BOUND * 4];

 memset(fill, 'x', sizeof fill);
 return(put_text(name, fill, len));
}

/**
 * put_link - Make a link in the test folder.
 * @name: the link; @target: what it leads to.
 */
static int put_link(const char *name, const char *target)
{
 char path[TEST_DIR_PATH_SIZE];

 if (test_path_join(path, sizeof path, dir, name) != 0)
  return(-1);
 if (symlink(target, path) != 0)
 {
  test_dir_fail("FAIL: setup link %s: %s\n", name, strerror(errno));
  return(-1);
 }
 return(0);
}

/**
 * put_fifo - Make a named pipe in the test folder.
 * @name: the pipe; @writer: keep a writer on it.
 */
static int put_fifo(const char *name, int writer, int *kept)
{
 char path[TEST_DIR_PATH_SIZE];

 if (test_path_join(path, sizeof path, dir, name) != 0)
  return(-1);
 if (mkfifo(path, 0600) != 0)
 {
  test_dir_fail("FAIL: setup fifo %s: %s\n", name, strerror(errno));
  return(-1);
 }
 if (writer)
 {
  *kept = open(path, O_RDWR | O_NONBLOCK);
  if (*kept < 0)
  {
   test_dir_fail("FAIL: setup fifo writer %s: %s\n", name, strerror(errno));
   return(-1);
  }
 }
 return(0);
}

/**
 * put_pty - Open a pseudo-terminal and name its slave.
 * @out/@n: the slave's name; @master: keeps the master open.
 * Return: 0, or -1 (counted).
 */
static int put_pty(char *out, size_t n, int *master)
{
 const char *slave;

 *master = posix_openpt(O_RDWR | O_NOCTTY);
 if (*master < 0 || grantpt(*master) != 0 || unlockpt(*master) != 0 ||
     (slave = ptsname(*master)) == NULL || strlen(slave) >= n)
 {
  test_dir_fail("FAIL: setup pty: %s\n", strerror(errno));
  return(-1);
 }
 strcpy(out, slave);
 return(0);
}

/**
 * path_of - The path of a name in the test folder.
 * @which: the object; @name: its name in the folder.
 */
static void path_of(Obj which, const char *name)
{
 (void)test_path_join(obj_path[which], sizeof obj_path[which], dir, name);
}

/**
 * make_objects - Make every kind of object the matrix reads, once.
 * Return: 0, or -1 (counted).
 */
static int make_objects(void)
{
 char sock[TEST_DIR_PATH_SIZE], sub[TEST_DIR_PATH_SIZE];
 int joined;

 path_of(OB_REG, "reg");
 path_of(OB_EMPTY, "empty");
 path_of(OB_NUL, "nul");
 path_of(OB_EXACT, "exact");
 path_of(OB_OVER, "over");
 path_of(OB_FIFO_IDLE, "fifo_idle");
 path_of(OB_FIFO_WRITER, WRITER_NAME);
 path_of(OB_SOCKET, "sock");
 path_of(OB_FOLDER, "folder");
 path_of(OB_MISSING, "missing");
 path_of(OB_DENIED, "denied");
 path_of(OB_LINK_REG, "link_reg");
 path_of(OB_LINK_FIFO, "link_fifo");
 path_of(OB_LINK_ZERO, "link_zero");
 path_of(OB_LOOP, "loop");
 snprintf(obj_path[OB_ZERO], sizeof obj_path[OB_ZERO], "/dev/zero");
 snprintf(obj_path[OB_NULL], sizeof obj_path[OB_NULL], "/dev/null");
 if (put_text("reg", TEXT_REG, LEN_REG) != 0 || put_text("empty", "", 0) != 0 ||
     put_text("nul", TEXT_NUL, LEN_NUL) != 0 || put_fill("exact", BOUND) != 0 ||
     put_fill("over", BOUND + 1) != 0)
  return(-1);
 if (put_fifo("fifo_idle", 0, NULL) != 0 ||
     put_fifo(WRITER_NAME, 1, &obj_keep[OB_FIFO_WRITER]) != 0)
  return(-1);
 joined = test_path_join(sock, sizeof sock, dir, "sock") == 0 &&
          test_path_join(sub, sizeof sub, dir, "folder") == 0;
 if (!joined || test_plant_socket(sock) != 0)
  return(-1);
 if (put_pty(obj_path[OB_PTY], sizeof obj_path[OB_PTY], &obj_keep[OB_PTY]) != 0)
  return(-1);
 if (mkdir(sub, 0700) != 0)
 {
  test_dir_fail("FAIL: setup folder: %s\n", strerror(errno));
  return(-1);
 }
 if (put_link("link_reg", "reg") != 0 || put_link("link_fifo", "fifo_idle") != 0 ||
     put_link("link_zero", "/dev/zero") != 0 || put_link("loop", "loop") != 0 ||
     put_link("link_folder", "folder") != 0)
  return(-1);
 denied_holds = test_premise_file_0200_is_unreadable(dir);
 if (test_put_mode(dir, "denied", "secret\n", 0200) != 0)
  return(-1);
 return(0);
}

/* ---- the reference: what each cell must give ----------------------------- */

typedef struct {
 WpeReadWhy why;
 int        err;
 mode_t     kind;
 int        want_folder;
 int        opens;       /* opens of the object the owner may make: 0 or 1 */
 long       bytes;       /* what the reader gets back (-1: nothing is read) */
 int        cut;
 int        skip;        /* the premise of the cell does not hold here */
} Expect;

/**
 * object_length - How many bytes a regular object holds.
 * @which: the object.
 */
static long object_length(Obj which)
{
 switch (which)
 {
 case OB_REG: case OB_LINK_REG: return(LEN_REG);
 case OB_NUL: return(LEN_NUL);
 case OB_EXACT: return(BOUND);
 case OB_OVER: return(BOUND + 1);
 case OB_DENIED: return(7);
 default: return(0);
 }
}

/**
 * object_is_link - Is the object a link?
 * @which: the object.
 */
static int object_is_link(Obj which)
{
 return(which == OB_LINK_REG || which == OB_LINK_FIFO || which == OB_LINK_ZERO || which == OB_LOOP);
}

/**
 * object_kind_followed - The kind of what the name leads to.
 * @which: the object.  Return: S_IFMT, or 0 for a name that leads to nothing.
 */
static mode_t object_kind_followed(Obj which)
{
 switch (which)
 {
 case OB_FIFO_IDLE: case OB_FIFO_WRITER: case OB_LINK_FIFO: return(S_IFIFO);
 case OB_SOCKET: return(S_IFSOCK);
 case OB_ZERO: case OB_NULL: case OB_PTY: case OB_LINK_ZERO: return(S_IFCHR);
 case OB_FOLDER: return(S_IFDIR);
 case OB_MISSING: case OB_LOOP: return(0);
 default: return(S_IFREG);
 }
}

/**
 * reference_regular - What a regular file gives, past the kind.
 * @which: the object; @face, @bm: the cell; @e: filled.
 */
static void reference_regular(Obj which, Face face, BoundMode bm, Expect *e)
{
 long n = object_length(which);

 if (bm == BOUND_REFUSE && n > BOUND)
 {
  e->why = WPE_READ_TOO_LARGE;
  return;
 }
 e->opens = 1;
 if (which == OB_DENIED)
 {
  e->why = WPE_READ_REFUSED_BY_SYSTEM;
  e->err = EACCES;
  e->skip = !denied_holds;
  return;
 }
 e->why = WPE_READ_DONE;
 e->bytes = n;
 if (face != FACE_DESC && bm == BOUND_CUT && n > BOUND)
 {
  e->bytes = BOUND;
  e->cut = 1;
 }
}

/**
 * reference - What a cell must give, from the design and not from the owner.
 * @which: the object; @nofollow: links are not followed; @face, @bm: the cell.
 * Return: the expectation.
 */
static Expect reference(Obj which, int nofollow, Face face, BoundMode bm)
{
 Expect e;
 mode_t kind = object_kind_followed(which);

 memset(&e, 0, sizeof e);
 e.bytes = -1;
 if (which == OB_MISSING)
 {
  e.why = WPE_READ_REFUSED_BY_SYSTEM;
  e.err = ENOENT;
 }
 else if (object_is_link(which) && nofollow)
  e.why = WPE_READ_IS_A_LINK;
 else if (which == OB_LOOP)
 {
  e.why = WPE_READ_REFUSED_BY_SYSTEM;
  e.err = ELOOP;
 }
 else if (face == FACE_FOLDER && kind != S_IFDIR)
 {
  e.why = WPE_READ_NOT_A_FILE;
  e.kind = kind;
  e.want_folder = 1;
 }
 else if (face == FACE_FOLDER)
 {
  e.why = WPE_READ_DONE;
  e.opens = 1;
  e.bytes = 0;
 }
 else if (kind != S_IFREG)
 {
  e.why = WPE_READ_NOT_A_FILE;
  e.kind = kind;
 }
 else
  reference_regular(which, face, bm, &e);
 return(e);
}

/* ---- running one cell ---------------------------------------------------- */

/**
 * ask_of - The request a cell makes.
 * @nofollow: links are not followed; @bm: the bound.
 */
static WpeReadAsk ask_of(int nofollow, BoundMode bm)
{
 WpeReadAsk a = nofollow ? WPE_READ_ASK_FOLDER_CONFIG("object") : WPE_READ_ASK_SCAN("object");

 if (bm == BOUND_NONE)
  a.bound = WPE_READ_UNBOUNDED;
 else
  a.bound = BOUND;
 a.over = bm == BOUND_CUT ? WPE_READ_CUT : WPE_READ_WHOLE_OR_REFUSE;
 return(a);
}

/**
 * drain_fd - Read a descriptor to its end.
 * @fd: the descriptor.  Return: the bytes read.
 */
static long drain_fd(int fd)
{
 char buf[256];
 long total = 0;
 ssize_t got;

 while ((got = read(fd, buf, sizeof buf)) > 0)
  total += got;
 return(total);
}

/**
 * drain_stream - Read a stream to its end.
 * @fp: the stream.  Return: the bytes read.
 */
static long drain_stream(FILE *fp)
{
 char buf[256];
 long total = 0;
 size_t got;

 while ((got = fread(buf, 1, sizeof buf, fp)) > 0)
  total += (long)got;
 return(total);
}

/**
 * take_cell - Run one face and give back the outcome and the bytes.
 * @path: the object's name; @ask: the request; @face: which face; @o: the outcome.
 * Return: the bytes got, or -1 when nothing was read.
 */
static long take_cell(const char *path, const WpeReadAsk *ask, Face face, WpeReadOutcome *o)
{
 long bytes = -1;

 if (face == FACE_DESC)
 {
  int fd = wpe_read_open(path, ask, o);

  if (fd >= 0)
  {
   bytes = drain_fd(fd);
   close(fd);
  }
 }
 else if (face == FACE_STREAM)
 {
  FILE *fp = wpe_read_stream_open(path, ask, o);

  if (fp != NULL)
  {
   bytes = drain_stream(fp);
   fclose(fp);
  }
 }
 else if (face == FACE_TEXT)
 {
  size_t len = 0;
  char *text = wpe_read_text(path, ask, &len, o);

  if (text != NULL)
  {
   bytes = (long)len;
   if (text[len] != '\0')
    test_dir_fail("FAIL: text face: the bytes are not NUL-ended\n");
   free(text);
  }
 }
 else
 {
  DIR *d = wpe_read_folder(path, ask, o);

  if (d != NULL)
  {
   bytes = 0;
   closedir(d);
  }
 }
 return(bytes);
}

/**
 * cell_label - A name for a cell, for its failure lines.
 */
static void cell_label(char *out, size_t n, Obj which, int nofollow, Face face, BoundMode bm)
{
 snprintf(out, n, "%s / %s / %s / %s", obj_label[which], nofollow ? "nofollow" : "named",
          face_label[face], bound_label[bm]);
}

/**
 * check_cell_outcome - Compare an outcome with the reference.
 * @label: the cell; @e: expected; @o: got; @bytes: what came back.
 */
static void check_cell_outcome(const char *label, const Expect *e, const WpeReadOutcome *o, long bytes)
{
 int ok = o->why == e->why;
 long want_bytes = e->why == WPE_READ_DONE ? e->bytes : -1L;

 if (e->why == WPE_READ_REFUSED_BY_SYSTEM)
  ok = ok && o->err == e->err;
 if (e->why == WPE_READ_NOT_A_FILE)
  ok = ok && o->kind == e->kind && o->want_folder == e->want_folder && o->tty == 0;
 if (e->why == WPE_READ_TOO_LARGE)
  ok = ok && o->bound == BOUND;
 if (e->why == WPE_READ_DONE)
  ok = ok && wpe_seen_known(&o->seen) == (e->opens == 1) && o->cut == e->cut;
 if (!ok)
 {
  printf("FAIL: cell %s: why %d err %d kind 0%o folder %d cut %d, want why %d err %d kind 0%o folder %d cut %d\n",
         label, o->why, o->err, (unsigned)o->kind, o->want_folder, o->cut,
         e->why, e->err, (unsigned)e->kind, e->want_folder, e->cut);
  fails++;
 }
 if (bytes != want_bytes)
 {
  printf("FAIL: cell %s: %ld bytes came back, want %ld\n", label, bytes, want_bytes);
  fails++;
 }
}

/**
 * run_cell - One cell of the matrix.
 */
static void run_cell(Obj which, int nofollow, Face face, BoundMode bm)
{
 char label[256];
 Expect e = reference(which, nofollow, face, bm);
 WpeReadAsk ask = ask_of(nofollow, bm);
 WpeReadOutcome o;
 long bytes;

 cell_label(label, sizeof label, which, nofollow, face, bm);
 cells++;
 if (e.skip)
 {
  skips++;
  return;
 }
 test_watchdog_case(label, CELL_SECS);
 watch_name = obj_path[which];
 watch_opens = 0;
 bytes = take_cell(obj_path[which], &ask, face, &o);
 watch_name = NULL;
 check_cell_outcome(label, &e, &o, bytes);
 if (watch_opens != e.opens)
 {
  printf("FAIL: cell %s: the object was opened %d time(s), want %d\n", label, watch_opens, e.opens);
  fails++;
 }
}

/**
 * run_matrix - Every object, link policy, face and bound.
 */
static void run_matrix(void)
{
 int ob, nofollow, face, bm;

 (void)test_premise(denied_holds, "the write-only cells", "this user reads a 0200 file");
 for (ob = 0; ob < OB_COUNT; ob++)
  for (nofollow = 0; nofollow < 2; nofollow++)
   for (face = 0; face < FACE_COUNT; face++)
    for (bm = 0; bm < BOUND_COUNT; bm++)
     run_cell((Obj)ob, nofollow, (Face)face, (BoundMode)bm);
}

/* ---- beside the matrix ---------------------------------------------------- */

static void chk_int(const char *what, long got, long want)
{
 if (got != want)
 {
  printf("FAIL: %s: got %ld want %ld\n", what, got, want);
  fails++;
 }
}

static void chk_str(const char *what, const char *got, const char *want)
{
 if (strcmp(got, want) != 0)
 {
  printf("FAIL: %s: got \"%s\" want \"%s\"\n", what, got, want);
  fails++;
 }
}

/**
 * make_other_file - Make a second file to put under another file's name.
 * @name: its name; @text: what it holds.
 */
static int make_other_file(const char *name, const char *text)
{
 return(put_text(name, text, strlen(text)));
}

/**
 * row_expect_same - A file read against the look of itself is read.
 */
static void row_expect_same(void)
{
 WpeSeen seen;
 WpeReadAsk ask = WPE_READ_ASK_SCAN("file");
 WpeReadOutcome o;
 int fd;

 test_watchdog_case("expect same", CELL_SECS);
 chk_int("look", wpe_seen_look_name(obj_path[OB_REG], 1, &seen), 0);
 ask = wpe_read_ask_seen(ask, &seen);
 fd = wpe_read_open(obj_path[OB_REG], &ask, &o);
 chk_int("same file is read", fd >= 0, 1);
 if (fd >= 0)
  close(fd);
 chk_int("same file: why", o.why, WPE_READ_DONE);
}

/**
 * row_expect_swapped - A file that took the name between the look and the open is refused.
 *
 * The open hook renames another file over the name just before the owner's
 * open, so the descriptor holds the newcomer and the look held the first.
 */
static void row_expect_swapped(void)
{
 WpeSeen seen, newcomer;
 WpeReadAsk ask = WPE_READ_ASK_SCAN("file");
 WpeReadOutcome o;
 int fd;
 char path[TEST_DIR_PATH_SIZE], from[TEST_DIR_PATH_SIZE];

 test_watchdog_case("expect swapped", CELL_SECS);
 (void)test_path_join(path, sizeof path, dir, "swap_a");
 (void)test_path_join(from, sizeof from, dir, "swap_b");
 if (make_other_file("swap_a", "first\n") != 0 || make_other_file("swap_b", "second\n") != 0)
  return;
 chk_int("look a", wpe_seen_look_name(path, 1, &seen), 0);
 chk_int("look b", wpe_seen_look_name(from, 1, &newcomer), 0);
 ask = wpe_read_ask_seen(ask, &seen);
 snprintf(swap_at, sizeof swap_at, "%s", path);
 snprintf(swap_from, sizeof swap_from, "%s", from);
 fd = wpe_read_open(path, &ask, &o);
 chk_int("swapped file is refused", fd, -1);
 chk_int("swapped file: why", o.why, WPE_READ_OTHER_FILE);
 chk_int("swapped file: unsure as the judge says",
         o.unsure, wpe_seen_judge(&seen, &newcomer) == WPE_SEEN_UNSURE);
 if (fd >= 0)
  close(fd);
}

/**
 * row_expect_touched_no_birth - A touched file is not proven the one seen, on a system with no birth time.
 *
 * The hook plays a system that keeps none (OpenBSD, ext3, most FUSE), so a
 * file touched after the look is UNSURE and a read that asked to be sure
 * refuses it with "it changed or was replaced".  The row sets the variable
 * itself and puts it back.
 */
static void row_expect_touched_no_birth(void)
{
 WpeSeen seen;
 WpeReadAsk ask = WPE_READ_ASK_SCAN("file");
 WpeReadOutcome o;
 char path[TEST_DIR_PATH_SIZE];
 struct utimbuf times = { 1000000000, 1000000000 };
 int fd;

 test_watchdog_case("expect touched, no birth", CELL_SECS);
 (void)test_path_join(path, sizeof path, dir, "touched");
 if (make_other_file("touched", "once\n") != 0)
  return;
 chk_int("hook set", setenv(WPE_SEEN_TEST_FAIL_ENV, WPE_SEEN_TEST_FAIL_NO_BIRTH, 1), 0);
 chk_int("look", wpe_seen_look_name(path, 1, &seen), 0);
 chk_int("touch", utime(path, &times), 0);
 ask = wpe_read_ask_seen(ask, &seen);
 fd = wpe_read_open(path, &ask, &o);
 chk_int("touched file is refused", fd, -1);
 chk_int("touched file: why", o.why, WPE_READ_OTHER_FILE);
 chk_int("touched file: unsure", o.unsure, 1);
 if (fd >= 0)
  close(fd);
 unsetenv(WPE_SEEN_TEST_FAIL_ENV);
}

/**
 * row_swapped_to_fifo - A pipe that takes a file's name after the look is not waited on.
 *
 * The look sees a regular file, so the owner opens it; the hook makes the
 * name a pipe with no writer first.  Opened without O_NONBLOCK that would
 * wait for ever (a named FAIL from the watchdog); opened as designed it is
 * held, found to be a pipe and refused before a byte is read.
 */
static void row_swapped_to_fifo(void)
{
 WpeReadAsk ask = WPE_READ_ASK_SCAN("file");
 WpeReadOutcome o;
 char path[TEST_DIR_PATH_SIZE], from[TEST_DIR_PATH_SIZE];
 int fd;

 test_watchdog_case("a pipe takes the name after the look", CELL_SECS);
 (void)test_path_join(path, sizeof path, dir, "race_file");
 (void)test_path_join(from, sizeof from, dir, "race_pipe");
 if (make_other_file("race_file", "plain\n") != 0 || mkfifo(from, 0600) != 0)
 {
  test_dir_fail("FAIL: setup race pipe: %s\n", strerror(errno));
  return;
 }
 snprintf(swap_at, sizeof swap_at, "%s", path);
 snprintf(swap_from, sizeof swap_from, "%s", from);
 fd = wpe_read_open(path, &ask, &o);
 chk_int("a pipe that took the name is refused", fd, -1);
 chk_int("a pipe that took the name: why", o.why, WPE_READ_NOT_A_FILE);
 chk_int("a pipe that took the name: kind", (long)o.kind, (long)S_IFIFO);
 if (fd >= 0)
  close(fd);
}

/**
 * row_link_by_name_not_errno - A link that took the name is told by looking at the name.
 *
 * Each system has its own errno for a link met by an open (ELOOP, EMLINK,
 * EFTYPE, EOPNOTSUPP).  The hook makes the open fail with each of them and the
 * name is a link each time: the answer is "a link", never a guess from the
 * number.  A name that is not a link and fails the same way is the system's.
 */
static void row_link_by_name_not_errno(void)
{
 static const int errnos[] = { ELOOP, EMLINK, EOPNOTSUPP, ENXIO, EIO };
 WpeReadAsk ask = WPE_READ_ASK_FOLDER_CONFIG("file");
 WpeReadOutcome o;
 char path[TEST_DIR_PATH_SIZE], from[TEST_DIR_PATH_SIZE];
 size_t i;

 test_watchdog_case("a link by name, not by errno", CELL_SECS);
 (void)test_path_join(path, sizeof path, dir, "link_race");
 (void)test_path_join(from, sizeof from, dir, "link_race_to");
 for (i = 0; i < sizeof errnos / sizeof errnos[0]; i++)
 {
  /* the look sees a file, the swap makes it a link, the open fails with this errno */
  (void)unlink(path);
  (void)unlink(from);
  if (make_other_file("link_race", "plain\n") != 0 || put_link("link_race_to", "reg") != 0)
   return;
  snprintf(swap_at, sizeof swap_at, "%s", path);
  snprintf(swap_from, sizeof swap_from, "%s", from);
  snprintf(fail_at, sizeof fail_at, "%s", path);
  fail_with = errnos[i];
  chk_int("a link that took the name is refused", wpe_read_open(path, &ask, &o), -1);
  chk_int("a link that took the name: why", o.why, WPE_READ_IS_A_LINK);
 }
 /* a link that took the name and the open itself is not made to fail: the
    open must refuse it by not following (O_NOFOLLOW), not only the look */
 (void)unlink(path);
 (void)unlink(from);
 if (make_other_file("link_race", "plain\n") != 0 || put_link("link_race_to", "reg") != 0)
  return;
 snprintf(swap_at, sizeof swap_at, "%s", path);
 snprintf(swap_from, sizeof swap_from, "%s", from);
 chk_int("a link swapped in is refused by the open", wpe_read_open(path, &ask, &o), -1);
 chk_int("a link swapped in: why", o.why, WPE_READ_IS_A_LINK);
 /* a plain file that fails the same way is the system's refusal */
 (void)unlink(path);
 if (make_other_file("link_race", "plain\n") != 0)
  return;
 snprintf(fail_at, sizeof fail_at, "%s", path);
 fail_with = EIO;
 chk_int("a plain refusal is refused", wpe_read_open(path, &ask, &o), -1);
 chk_int("a plain refusal: why", o.why, WPE_READ_REFUSED_BY_SYSTEM);
 chk_int("a plain refusal: err", o.err, EIO);
}

/**
 * row_memory_stream - The stream over bytes behaves as a file does.
 *
 * fgets, getline, fseek and ftell over a text with NUL bytes and over an empty
 * text; fclose frees the bytes.  A stream of either callback family has no
 * descriptor of its own, so fileno is -1.  On a system with neither family the
 * bytes live in a temporary file, which has one, so this row asks the platform
 * which of the two it is looking at instead of naming a descriptor as the
 * failure of a system that has no memory stream call.
 */
static void row_memory_stream(void)
{
 WpeReadAsk ask = WPE_READ_ASK_CONFIG("file");
 WpeReadOutcome o;
 FILE *fp;
 char line[64], *buf = NULL;
 size_t room = 0;
 ssize_t got;

 test_watchdog_case("memory stream", CELL_SECS);
 fp = wpe_read_stream_open(obj_path[OB_NUL], &ask, &o);
 chk_int("nul text opens", fp != NULL, 1);
 if (fp == NULL)
  return;
#if WPE_PLATFORM_MEMORY_STREAM == WPE_PLATFORM_MEMORY_STREAM_NONE
 chk_int("the temporary file's descriptor", fileno(fp) >= 0, 1);
#else
 chk_int("no descriptor", fileno(fp), -1);
#endif
 chk_str("first line", fgets(line, sizeof line, fp) ? line : "(none)", "a");
 chk_int("position after the first line", ftell(fp), 4);
 chk_int("seek", fseek(fp, 0, SEEK_SET), 0);
 got = getline(&buf, &room, fp);
 chk_int("getline counts the NUL bytes", got, 4);
 if (got >= 4)
  chk_int("a NUL byte is delivered", buf[1], 0);
 chk_int("seek to the end", fseek(fp, 0, SEEK_END), 0);
 chk_int("ftell at the end", ftell(fp), LEN_NUL);
 free(buf);
 fclose(fp);
 fp = wpe_read_stream_open(obj_path[OB_EMPTY], &ask, &o);
 chk_int("empty text opens", fp != NULL, 1);
 if (fp == NULL)
  return;
 chk_int("empty: fgets", fgets(line, sizeof line, fp) == NULL, 1);
 chk_int("empty: feof", feof(fp) != 0, 1);
 fclose(fp);
}

/**
 * check_stream_end - Read a bounded stream to its end and compare it with the file.
 * @what: the name the failure lines carry; @path: the file; @text/@len: its bytes.
 *
 * A bounded stream is a stream over the bytes the owner read, and where a
 * reader meets its end is a fact of the library: one stops at the bytes
 * written, one goes on to the end of the buffer the owner asked for.  What a
 * caller gets is the file's own bytes and then the end of the stream.
 */
static void check_stream_end(const char *what, const char *path, const char *text, size_t len)
{
 WpeReadAsk ask = ask_of(0, BOUND_REFUSE);
 WpeReadOutcome o;
 char label[128], back[BOUND + 2];
 FILE *fp;
 size_t got;

 fp = wpe_read_stream_open(path, &ask, &o);
 snprintf(label, sizeof label, "%s: a bounded stream opens", what);
 chk_int(label, fp != NULL, 1);
 if (fp == NULL)
  return;
 got = fread(back, 1, sizeof back, fp);
 snprintf(label, sizeof label, "%s: the bytes a reader is given", what);
 chk_int(label, (long)got, (long)len);
 snprintf(label, sizeof label, "%s: the reader is at the end", what);
 chk_int(label, feof(fp) != 0, 1);
 snprintf(label, sizeof label, "%s: the bytes are the file's own", what);
 chk_int(label, got == len && (len == 0 || memcmp(back, text, len) == 0), 1);
 fclose(fp);
}

/**
 * row_stream_end_is_the_last_byte - A bounded stream ends at the file's last byte.
 *
 * What goes wrong if it does not: a caller that loops on fgets or getline runs
 * once too often over a byte the file does not hold, and a caller that counts
 * what it was given counts a byte it never had.  NetBSD reads a memory stream
 * to the end of the buffer the owner asked for, so it gives that byte back when
 * the owner asks for one more than the text; FreeBSD and macOS stop at the
 * bytes written.  The owner asks for what the text needs on the library it runs
 * on.
 */
static void row_stream_end_is_the_last_byte(void)
{
 char exact[BOUND];

 test_watchdog_case("the end of a bounded stream", CELL_SECS);
 memset(exact, 'x', sizeof exact);
 check_stream_end("a text", obj_path[OB_REG], TEXT_REG, LEN_REG);
 check_stream_end("a text with NUL bytes", obj_path[OB_NUL], TEXT_NUL, LEN_NUL);
 check_stream_end("a text of exactly the bound", obj_path[OB_EXACT], exact, sizeof exact);
 check_stream_end("an empty text", obj_path[OB_EMPTY], "", 0);
}

/**
 * check_stream_full_buffer - A file that fills its stream exactly reads back whole.
 * @len: how many bytes the file holds.
 *
 * The last byte is one of its own, so a library that keeps a NUL terminator
 * inside the buffer gives itself away at once: the reader gets the file's
 * length but not the file's last byte.
 */
static void check_stream_full_buffer(size_t len)
{
 WpeReadAsk ask = ask_of(0, BOUND_REFUSE);
 WpeReadOutcome o;
 char name[32], path[TEST_DIR_PATH_SIZE], label[128];
 char text[BOUND], back[BOUND + 2];
 FILE *fp;
 size_t i, got;

 snprintf(name, sizeof name, "full_%u", (unsigned)len);
 if (test_path_join(path, sizeof path, dir, name) != 0)
  return;
 for (i = 0; i < len; i++)
  text[i] = (char)('a' + (int)(i % 26));
 if (len > 0)
  text[len - 1] = 'Z';
 if (put_text(name, text, len) != 0)
  return;
 fp = wpe_read_stream_open(path, &ask, &o);
 snprintf(label, sizeof label, "a file of %u bytes opens", (unsigned)len);
 chk_int(label, fp != NULL, 1);
 if (fp == NULL)
  return;
 got = fread(back, 1, sizeof back, fp);
 snprintf(label, sizeof label, "a file of %u bytes reads back whole", (unsigned)len);
 chk_int(label, (long)got, (long)len);
 if (got == len && len > 0)
 {
  snprintf(label, sizeof label, "a file of %u bytes keeps its last byte", (unsigned)len);
  chk_int(label, back[len - 1], text[len - 1]);
 }
 fclose(fp);
}

/**
 * row_stream_keeps_a_full_buffer - A memory stream of exactly the text keeps the text.
 *
 * What goes wrong if it does not: the last byte of every file the owner reads
 * whole for a bounded reader is a NUL, so an options or a project file loses
 * its last character and a last line that had no newline of its own gains one.
 * A library that keeps its terminator inside the buffer needs a byte more than
 * the text, and a byte more is a byte a reader of the other libraries is
 * given, so the room is measured and not guessed (row above).
 */
static void row_stream_keeps_a_full_buffer(void)
{
 static const size_t lens[] = { 1, 2, 3, 7, 8, BOUND - 1, BOUND };
 size_t i;

 test_watchdog_case("a buffer exactly the text", CELL_SECS);
 for (i = 0; i < sizeof lens / sizeof lens[0]; i++)
  check_stream_full_buffer(lens[i]);
}

/**
 * row_grows_while_read - A file that grows while it is read is never handed over past the bound.
 *
 * The hook appends to the file just before the first read.  The look and the
 * judgement saw a file under the bound; the bytes really read are counted, so
 * the stream of a bound that refuses is refused whole, and the text face the
 * same.  Given the descriptor instead, the reader would be handed the growth.
 */
static void row_grows_while_read(void)
{
 WpeReadAsk ask = ask_of(0, BOUND_REFUSE);
 WpeReadOutcome o;
 FILE *fp;
 size_t len;
 char *text;

 test_watchdog_case("a file that grows", CELL_SECS);
 if (put_fill("grower", BOUND - 10) != 0)
  return;
 (void)test_path_join(grow_path, sizeof grow_path, dir, "grower");
 fp = wpe_read_stream_open(grow_path, &ask, &o);
 chk_int("a grown file is refused by the stream", fp == NULL, 1);
 chk_int("a grown file: why", o.why, WPE_READ_TOO_LARGE);
 if (fp != NULL)
  fclose(fp);
 grow_path[0] = '\0';
 if (put_fill("grower2", BOUND - 10) != 0)
  return;
 (void)test_path_join(grow_path, sizeof grow_path, dir, "grower2");
 text = wpe_read_text(grow_path, &ask, &len, &o);
 chk_int("a grown file is refused by the text", text == NULL, 1);
 chk_int("a grown file, text: why", o.why, WPE_READ_TOO_LARGE);
 free(text);
 grow_path[0] = '\0';
}

/**
 * row_outcome_facts - The outcome says what was read.
 *
 * The size, the modification time and the mode of the object read come from
 * the descriptor, so the header check needs no second stat by name.
 */
static void row_outcome_facts(void)
{
 WpeReadAsk ask = WPE_READ_ASK_SCAN("file");
 WpeReadOutcome o;
 struct utimbuf times = { 1100000000, 1100000000 };
 int fd;

 test_watchdog_case("outcome facts", CELL_SECS);
 chk_int("set the time", utime(obj_path[OB_REG], &times), 0);
 fd = wpe_read_open(obj_path[OB_REG], &ask, &o);
 chk_int("opens", fd >= 0, 1);
 if (fd >= 0)
  close(fd);
 chk_int("size", o.size, LEN_REG);
 chk_int("mtime", (long)o.mtime, 1100000000L);
 chk_int("regular mode", S_ISREG(o.mode) != 0, 1);
 chk_int("identity is known", wpe_seen_known(&o.seen), 1);
}

/**
 * row_name_ok - The name face judges without opening.
 *
 * A pipe is refused by its kind and the open hook sees no open; a regular file
 * passes; a file over the bound that refuses is refused.
 */
static void row_name_ok(void)
{
 WpeReadAsk ask = ask_of(0, BOUND_REFUSE);
 WpeReadOutcome o;
 int before;

 test_watchdog_case("name face", CELL_SECS);
 before = all_opens;
 chk_int("a pipe is not ok", wpe_read_name_ok(obj_path[OB_FIFO_WRITER], &ask, &o), -1);
 chk_int("a pipe: why", o.why, WPE_READ_NOT_A_FILE);
 chk_int("a regular file is ok", wpe_read_name_ok(obj_path[OB_REG], &ask, &o), 0);
 chk_int("a file over the bound is not ok", wpe_read_name_ok(obj_path[OB_OVER], &ask, &o), -1);
 chk_int("a file over the bound: why", o.why, WPE_READ_TOO_LARGE);
 chk_int("nothing was opened", all_opens, before);
}

/**
 * row_folder_entries - The entries of a folder are told by kind and none is opened.
 *
 * A folder that holds a pipe, a link and a socket is listed; each entry is
 * asked of the folder's descriptor, a link is reported as a link, and the
 * open hook sees no open of any entry.
 */
static void row_folder_entries(void)
{
 WpeReadAsk ask = WPE_READ_ASK_SCAN("folder");
 WpeReadOutcome o;
 DIR *d;
 struct dirent *de;
 WpeSeen s;
 int seen_pipe = 0, seen_link = 0, seen_socket = 0, before;

 test_watchdog_case("folder entries", CELL_SECS);
 d = wpe_read_folder(dir, &ask, &o);
 chk_int("the test folder opens as a folder", d != NULL, 1);
 if (d == NULL)
  return;
 before = all_opens;
 while ((de = readdir(d)) != NULL)
 {
  if (wpe_read_entry_look(d, de->d_name, &s) != 0)
   continue;
  if (strcmp(de->d_name, "fifo_idle") == 0)
   seen_pipe = wpe_seen_kind(&s) == S_IFIFO;
  else if (strcmp(de->d_name, "link_reg") == 0)
   seen_link = wpe_seen_kind(&s) == S_IFLNK;
  else if (strcmp(de->d_name, "sock") == 0)
   seen_socket = wpe_seen_kind(&s) == S_IFSOCK;
 }
 chk_int("a pipe entry is a pipe", seen_pipe, 1);
 chk_int("a link entry is a link", seen_link, 1);
 chk_int("a socket entry is a socket", seen_socket, 1);
 chk_int("no entry was opened", all_opens, before);
 closedir(d);
}

/**
 * row_folder_entry_is_not_followed - A folder met while walking another is not entered through a link.
 *
 * A link to a folder is refused by the form for a folder met in a walk
 * (WPE_READ_ASK_FOLDER_ENTRY) and named as a link; the form for a folder the
 * user named follows it, as the user's own folder may be one.
 */
static void row_folder_entry_is_not_followed(void)
{
 WpeReadAsk entry = WPE_READ_ASK_FOLDER_ENTRY("folder");
 WpeReadAsk named = WPE_READ_ASK_FOLDER("folder");
 WpeReadOutcome o;
 char path[TEST_DIR_PATH_SIZE];
 DIR *d;

 test_watchdog_case("folder entry link", CELL_SECS);
 if (test_path_join(path, sizeof path, dir, "link_folder") != 0)
  return;
 d = wpe_read_folder(path, &entry, &o);
 chk_int("a link to a folder is not entered by the walk's form", d == NULL, 1);
 chk_int("a link to a folder: why", o.why, WPE_READ_IS_A_LINK);
 if (d != NULL)
  closedir(d);
 d = wpe_read_folder(path, &named, &o);
 chk_int("a link to a folder is followed for a folder the user named", d != NULL, 1);
 if (d != NULL)
  closedir(d);
}

/**
 * row_sentences - Each refusal says one sentence through the words owner.
 *
 * The sentence is we_path.c's; this pins the mapping from the outcome to it.
 */
static void row_sentences(void)
{
 WpeReadAsk named = WPE_READ_ASK_PROJECT("project file");
 WpeReadAsk folder = WPE_READ_ASK_FOLDER_CONFIG("options file");
 WpeReadOutcome o;
 char said[WPE_PATH_REFUSED_MAX];
 char p[TEST_DIR_PATH_SIZE];
 FILE *fp;

 test_watchdog_case("sentences", CELL_SECS);
 fp = wpe_read_stream_open(obj_path[OB_FIFO_IDLE], &named, &o);
 chk_int("pipe refused", fp == NULL, 1);
 (void)wpe_read_say(said, sizeof said, "project file", obj_path[OB_FIFO_IDLE], &o);
 snprintf(p, sizeof p, "Cannot open project file %s: it is a pipe.", obj_path[OB_FIFO_IDLE]);
 chk_str("pipe sentence", said, p);
 fp = wpe_read_stream_open(obj_path[OB_LINK_ZERO], &folder, &o);
 chk_int("link refused", fp == NULL, 1);
 (void)wpe_read_say(said, sizeof said, "options file", obj_path[OB_LINK_ZERO], &o);
 snprintf(p, sizeof p, "Cannot open options file %s: it is a link.", obj_path[OB_LINK_ZERO]);
 chk_str("link sentence", said, p);
 fp = wpe_read_stream_open(obj_path[OB_ZERO], &named, &o);
 chk_int("device refused", fp == NULL, 1);
 (void)wpe_read_say(said, sizeof said, "project file", obj_path[OB_ZERO], &o);
 chk_str("device sentence", said, "Cannot open project file /dev/zero: it is a device.");
 fp = wpe_read_stream_open(obj_path[OB_MISSING], &named, &o);
 chk_int("missing refused", fp == NULL, 1);
 (void)wpe_read_say(said, sizeof said, "project file", obj_path[OB_MISSING], &o);
 snprintf(p, sizeof p, "Project file not found: %s.", obj_path[OB_MISSING]);
 chk_str("missing sentence", said, p);
 chk_int("missing is the one not-found", wpe_read_outcome_missing(&o), 1);
 named.bound = BOUND;
 fp = wpe_read_stream_open(obj_path[OB_OVER], &named, &o);
 chk_int("oversize refused", fp == NULL, 1);
 (void)wpe_read_say(said, sizeof said, "project file", obj_path[OB_OVER], &o);
 snprintf(p, sizeof p, "Cannot open project file %s: it is larger than 64 bytes.", obj_path[OB_OVER]);
 chk_str("oversize sentence", said, p);
 chk_int("a folder asked as a file", wpe_read_stream_open(obj_path[OB_FOLDER], &named, &o) == NULL, 1);
 (void)wpe_read_say(said, sizeof said, "project file", obj_path[OB_FOLDER], &o);
 snprintf(p, sizeof p, "Cannot open project file %s: it is a folder.", obj_path[OB_FOLDER]);
 chk_str("folder sentence", said, p);
 chk_int("a file asked as a folder", wpe_read_folder(obj_path[OB_REG], &named, &o) == NULL, 1);
 (void)wpe_read_say(said, sizeof said, "folder", obj_path[OB_REG], &o);
 snprintf(p, sizeof p, "Cannot open folder %s: it is not a folder.", obj_path[OB_REG]);
 chk_str("not a folder sentence", said, p);
 {
  /* the length given back is what was written, never what was wanted, and a
     reason that does not fit ends in the mark */
  char tight[8];
  int n = wpe_read_reason(tight, sizeof tight, &o);

  chk_int("a cut reason's length is what was written", n, (int)strlen(tight));
  chk_str("a cut reason ends in the mark", tight, "it i...");
 }
}

/**
 * row_terminal_by_descriptor - A terminal is told from another device only on a descriptor.
 *
 * A look by name can say "device" and no more; only isatty on an opened
 * descriptor says terminal.  The hook makes a regular file's name lead to the
 * pty's slave just before the open, so the descriptor is a terminal and the
 * outcome says so.
 */
static void row_terminal_by_descriptor(void)
{
 WpeReadAsk ask = WPE_READ_ASK_SCAN("header");
 WpeReadOutcome o;
 char path[TEST_DIR_PATH_SIZE], from[TEST_DIR_PATH_SIZE];

 test_watchdog_case("terminal by descriptor", CELL_SECS);
 (void)test_path_join(path, sizeof path, dir, "term_race");
 (void)test_path_join(from, sizeof from, dir, "term_link");
 if (make_other_file("term_race", "plain\n") != 0 || symlink(obj_path[OB_PTY], from) != 0)
 {
  test_dir_fail("FAIL: setup terminal race: %s\n", strerror(errno));
  return;
 }
 snprintf(swap_at, sizeof swap_at, "%s", path);
 snprintf(swap_from, sizeof swap_from, "%s", from);
 chk_int("a terminal is refused", wpe_read_open(path, &ask, &o), -1);
 chk_int("a terminal: why", o.why, WPE_READ_NOT_A_FILE);
 chk_int("a terminal: tty", o.tty, 1);
}

/* ---- the load face --------------------------------------------------------- */

#define LOAD_FD_BASE     120      /* the rows hold the objects at 120 + the object's number */
#define LOAD_PIPE_TEXT   "pipe text\n"
#define LOAD_PIPE_LEN    10
#define LOAD_HIDE_HOOK   "read-no-pipe-identity"
#define LOAD_UNTOLD_HOOK "read-pipe-ends-untold"
#define LOAD_PROBE_FAILS_HOOK "read-probe-pipe-fails"
#define LOAD_PLAY_PROBE_FAILS 2   /* a row's @untold: the ends are untold and the probe fails */
#define LOAD_HOOK_ENV    "XWPE_TEST_FAIL"
#define LOAD_HOOKS_MAX   128
#define LOW_NUMBERS      3        /* descriptors 0, 1 and 2 */
#define LOAD_NO_STAND_IN (-1)     /* no pipe was stood at descriptor 0 */

/**
 * load_hooks_set - Play the systems whose pipes differ from this one's.
 * @hide: pipes have no identity (NetBSD, OpenBSD); @untold: a pipe's writing
 * end is not told from its reading end (FreeBSD, OpenBSD).
 *
 * Sets exactly these hooks, and none when both are 0.  A row calls it before
 * wpe_read_note_start, because the owner learns both facts from the probe it
 * makes there.
 */
static void load_hooks_set(int hide, int untold)
{
 char hooks[LOAD_HOOKS_MAX];

 hooks[0] = '\0';
 if (hide)
  snprintf(hooks, sizeof hooks, "%s", LOAD_HIDE_HOOK);
 if (hide && untold)
  strncat(hooks, ",", sizeof hooks - strlen(hooks) - 1);
 if (untold)
  strncat(hooks, LOAD_UNTOLD_HOOK, sizeof hooks - strlen(hooks) - 1);
 if (hooks[0] == '\0')
  unsetenv(LOAD_HOOK_ENV);
 else
  setenv(LOAD_HOOK_ENV, hooks, 1);
}

/**
 * load_hooks_probe_fails - Play a system that does not tell a pipe's writing end
 * and whose probe pipe cannot be made.
 *
 * Both hooks together, because the default the owner takes when its probe
 * fails (the ends are untold) only matters where the system's own word is
 * two-way.  A row calls it before wpe_read_note_start like load_hooks_set.
 */
static void load_hooks_probe_fails(void)
{
 setenv(LOAD_HOOK_ENV, LOAD_UNTOLD_HOOK "," LOAD_PROBE_FAILS_HOOK, 1);
}

/**
 * load_end_is_told - Does this system say which end of a pipe is the writing one?
 * @ends: a pipe.  Return: 1 when the writing end says O_WRONLY (the system's own word).
 */
static int load_end_is_told(const int ends[2])
{
 return (fcntl(ends[1], F_GETFL) & O_ACCMODE) == O_WRONLY;
}

/**
 * LoadGot - What one load gave: the return, the outcome, and what was done with it.
 * @stream_given: a stream came back (and was read and closed).
 * @bytes: what the stream held, -1 for none.
 * @nonblock, @cloexec: a descriptor came back non-blocking, closed on exec.
 */
typedef struct {
 int            rc;
 int            fd;
 int            stream_given;
 long           bytes;
 int            nonblock;
 int            cloexec;
 WpeReadOutcome o;
} LoadGot;

/**
 * load_take - Ask the load face and look at what came back.
 * @path: the name; @g: receives the answer.
 *
 * A stream is read to its end and closed; a descriptor is left open for the
 * row to close with load_release.
 */
static void load_take(const char *path, LoadGot *g)
{
 FILE *fp = NULL;

 memset(g, 0, sizeof *g);
 g->bytes = -1;
 g->fd = -2;
 g->rc = wpe_read_load_open(path, "file", &fp, &g->fd, &g->o);
 if (fp != NULL)
 {
  g->stream_given = 1;
  g->bytes = drain_stream(fp);
  fclose(fp);
 }
 if (g->fd >= 0)
 {
  g->nonblock = (fcntl(g->fd, F_GETFL) & O_NONBLOCK) != 0;
  g->cloexec = (fcntl(g->fd, F_GETFD) & FD_CLOEXEC) != 0;
 }
}

/**
 * load_release - Close the descriptor a load gave, if it gave one.
 * @g: the answer.
 */
static void load_release(LoadGot *g)
{
 if (g->fd >= 0)
  close(g->fd);
 g->fd = -2;
}

/**
 * check_load_refused - A load was refused, for this reason, and gave nothing.
 * @label: the row; @g: the answer; @why, @err, @kind, @tty: what the refusal must say.
 */
static void check_load_refused(const char *label, const LoadGot *g, WpeReadWhy why, int err,
                               mode_t kind, int tty)
{
 int ok = g->rc == -1 && !g->stream_given && g->fd < 0 && g->o.why == why;

 if (why == WPE_READ_REFUSED_BY_SYSTEM)
  ok = ok && g->o.err == err;
 if (why == WPE_READ_NOT_A_FILE)
  ok = ok && g->o.kind == kind && g->o.tty == tty;
 if (!ok)
 {
  printf("FAIL: %s: rc %d stream %d fd %d why %d err %d kind 0%o tty %d, want a refusal why %d err %d kind 0%o tty %d\n",
         label, g->rc, g->stream_given, g->fd, g->o.why, g->o.err, (unsigned)g->o.kind, g->o.tty,
         why, err, (unsigned)kind, tty);
  fails++;
 }
}

/**
 * check_load_stream - A load gave a stream of these bytes.
 * @label: the row; @g: the answer; @bytes: what the file holds; @stream: o.stream must be this.
 */
static void check_load_stream(const char *label, const LoadGot *g, long bytes, int stream)
{
 if (g->rc != 0 || !g->stream_given || g->fd >= 0 || g->o.why != WPE_READ_DONE ||
     g->bytes != bytes || g->o.stream != stream)
 {
  printf("FAIL: %s: rc %d stream %d fd %d why %d bytes %ld o.stream %d, want a stream of %ld bytes, o.stream %d\n",
         label, g->rc, g->stream_given, g->fd, g->o.why, g->bytes, g->o.stream, bytes, stream);
  fails++;
 }
}

/**
 * check_load_pipe - A load gave a non-blocking descriptor of a pipe, closed on exec.
 * @label: the row; @g: the answer.
 */
static void check_load_pipe(const char *label, const LoadGot *g)
{
 if (g->rc != 0 || g->stream_given || g->fd < 0 || g->o.why != WPE_READ_DONE ||
     g->o.stream != 1 || !g->nonblock || !g->cloexec || !S_ISFIFO(g->o.mode))
 {
  printf("FAIL: %s: rc %d stream %d fd %d why %d o.stream %d nonblock %d cloexec %d, want a descriptor, non-blocking, closed on exec\n",
         label, g->rc, g->stream_given, g->fd, g->o.why, g->o.stream, g->nonblock, g->cloexec);
  fails++;
 }
}

/**
 * descriptor_is_open - Is a descriptor number open?
 * @fd: the number.
 */
static int descriptor_is_open(int fd)
{
 return fcntl(fd, F_GETFD) >= 0;
}

/**
 * run_load_name_cell - One object asked of the load face by its name.
 * @which: the object.
 *
 * A regular file comes back as a stream with no stream mark, a FIFO as a
 * non-blocking descriptor, and everything else is refused as the other faces
 * refuse it; the reference is the matrix's own, with the FIFO now taken.
 */
static void run_load_name_cell(Obj which)
{
 Expect e = reference(which, 0, FACE_DESC, BOUND_NONE);
 int fifo = object_kind_followed(which) == S_IFIFO;
 char label[128];
 LoadGot g;

 snprintf(label, sizeof label, "load by name: %s", obj_label[which]);
 cells++;
 if (e.skip)
 {
  skips++;
  return;
 }
 test_watchdog_case(label, CELL_SECS);
 watch_name = obj_path[which];
 watch_opens = 0;
 load_take(obj_path[which], &g);
 watch_name = NULL;
 if (fifo)
  check_load_pipe(label, &g);
 else if (e.why == WPE_READ_DONE)
  check_load_stream(label, &g, e.bytes, 0);
 else
  check_load_refused(label, &g, e.why, e.err, e.kind, 0);
 chk_int(label, watch_opens, (fifo || e.opens) ? 1 : 0);
 load_release(&g);
}

/**
 * open_start_descriptor - Open an object the way a program started with it would hold it.
 * @which: the object.
 * Return: the descriptor, or -1 for an object that cannot be held (missing, a loop).
 */
static int open_start_descriptor(Obj which)
{
 switch (which)
 {
 case OB_MISSING: case OB_LOOP:
  return(-1);
 case OB_FIFO_IDLE: case OB_FIFO_WRITER: case OB_LINK_FIFO:
  return(open(obj_path[which], O_RDONLY | O_NONBLOCK));
 case OB_SOCKET:
  return(socket(AF_UNIX, SOCK_STREAM, 0));
 case OB_PTY:
  return(open(obj_path[which], O_RDWR | O_NOCTTY));
 case OB_DENIED:
  return(open(obj_path[which], O_WRONLY));
 default:
  return(open(obj_path[which], O_RDONLY));
 }
}

/**
 * hold_start_descriptors - Put each object on a number of its own, before the start is noted.
 * @held: receives 1 for each object that is held.
 */
static void hold_start_descriptors(int held[OB_COUNT])
{
 int ob;

 for (ob = 0; ob < OB_COUNT; ob++)
 {
  int fd = open_start_descriptor((Obj)ob);

  held[ob] = fd >= 0;
  if (fd < 0)
   continue;
  if (dup2(fd, LOAD_FD_BASE + ob) < 0)
   held[ob] = 0;
  close(fd);
 }
}

/**
 * check_load_descriptor_cell - One object asked of the load face by its descriptor name.
 * @which: the object; @n: its number.
 *
 * A regular file is read as a stream marked "no name"; a FIFO is moved, so the
 * descriptor that comes back is the number itself; a socket, a device, a
 * terminal and a folder are refused with their kind, and a descriptor open for
 * writing only as a bad descriptor.  A refusal leaves the number open.
 */
static void check_load_descriptor_cell(Obj which, int n)
{
 mode_t kind = object_kind_followed(which);
 char label[128], name[64];
 LoadGot g;

 snprintf(label, sizeof label, "load by descriptor: %s", obj_label[which]);
 snprintf(name, sizeof name, "/dev/fd/%d", n);
 test_watchdog_case(label, CELL_SECS);
 load_take(name, &g);
 if (which == OB_DENIED)
 {
  check_load_refused(label, &g, WPE_READ_REFUSED_BY_SYSTEM, EBADF, 0, 0);
  chk_int("a write-only descriptor is left open", descriptor_is_open(n), 1);
 }
 else if (kind == S_IFIFO)
 {
  check_load_pipe(label, &g);
  chk_int("a pipe is moved, not copied: the number comes back", g.fd, n);
 }
 else if (kind == S_IFREG)
 {
  check_load_stream(label, &g, object_length(which), 1);
  chk_int("a regular file's number is closed with its stream", descriptor_is_open(n), 0);
 }
 else
 {
  check_load_refused(label, &g, WPE_READ_NOT_A_FILE, 0, kind, which == OB_PTY);
  chk_int("a refused descriptor is left open", descriptor_is_open(n), 1);
 }
 load_release(&g);
}

/**
 * row_load_taken_is_bad_descriptor - A descriptor read once says what a closed one says.
 * @n: the number a pipe was taken from.
 */
static void row_load_taken_is_bad_descriptor(int n)
{
 char name[64];
 LoadGot g;

 snprintf(name, sizeof name, "/dev/fd/%d", n);
 load_take(name, &g);
 check_load_refused("load: a descriptor taken before", &g, WPE_READ_REFUSED_BY_SYSTEM, EBADF, 0, 0);
}

/**
 * run_load_matrix - Every object by its name, and by its descriptor name.
 */
static void run_load_matrix(void)
{
 int held[OB_COUNT];
 int ob;

 (void)test_premise(denied_holds, "the load rows of a write-only file", "this user reads a 0200 file");
 for (ob = 0; ob < OB_COUNT; ob++)
  run_load_name_cell((Obj)ob);
 hold_start_descriptors(held);
 wpe_read_note_start();
 for (ob = 0; ob < OB_COUNT; ob++)
 {
  if (!held[ob])
   continue;
  cells++;
  check_load_descriptor_cell((Obj)ob, LOAD_FD_BASE + ob);
  if (object_kind_followed((Obj)ob) == S_IFIFO)
   row_load_taken_is_bad_descriptor(LOAD_FD_BASE + ob);
 }
 for (ob = 0; ob < OB_COUNT; ob++)
  if (held[ob] && descriptor_is_open(LOAD_FD_BASE + ob))
   close(LOAD_FD_BASE + ob);
}

/**
 * make_pipe_with_text - A pipe with a line already in it.
 * @ends: receives both ends.  Return: 0, or -1 (counted).
 */
static int make_pipe_with_text(int ends[2])
{
 if (pipe(ends) != 0 || write(ends[1], LOAD_PIPE_TEXT, LOAD_PIPE_LEN) != LOAD_PIPE_LEN)
 {
  test_dir_fail("FAIL: setup pipe: %s\n", strerror(errno));
  return(-1);
 }
 return(0);
}

/**
 * drain_pipe_text - What a non-blocking pipe holds now.
 * @fd: its read end; @out/@n: the buffer.  Return: the bytes.
 */
static long drain_pipe_text(int fd, char *out, size_t n)
{
 ssize_t got = read(fd, out, n - 1);

 if (got < 0)
  got = 0;
 out[got] = '\0';
 return((long)got);
}

/**
 * row_load_start_descriptors - A pipe made before the start is read; one made after is refused.
 * @label: the row; @hide: play a system whose pipes have no identity;
 * @untold: play a system that does not tell a pipe's writing end.
 *
 * What goes wrong if the guard breaks: a name such as /proc/self/fd/9 or
 * /dev/fd/9 that leads to a pipe the editor made for a child or a server is
 * read, and the bytes meant for the other end are taken.  The guard goes by
 * the descriptor number, so it holds where every pipe has device 0 and number
 * 0 too (NetBSD, OpenBSD), which the hook plays here.
 */
static void row_load_start_descriptors(const char *label, int hide, int untold)
{
 int before[2], after[2];
 char name[64], text[64];
 LoadGot g;

 test_watchdog_case(label, CELL_SECS);
 load_hooks_set(hide, untold);
 if (make_pipe_with_text(before) != 0)
  return;
 wpe_read_note_start();
 if (make_pipe_with_text(after) != 0)
  return;
 snprintf(name, sizeof name, "/dev/fd/%d", after[0]);
 load_take(name, &g);
 check_load_refused(label, &g, WPE_READ_NOT_A_FILE, 0, S_IFIFO, 0);
 chk_int("a pipe made after the start is left open", descriptor_is_open(after[0]), 1);
 snprintf(name, sizeof name, "/proc/self/fd/%d", after[0]);
 load_take(name, &g);
 check_load_refused(label, &g, WPE_READ_NOT_A_FILE, 0, S_IFIFO, 0);
 snprintf(name, sizeof name, "/dev/fd/%d", before[0]);
 load_take(name, &g);
 check_load_pipe(label, &g);
 chk_int("the pipe made before the start comes back as itself", g.fd, before[0]);
 (void)drain_pipe_text(g.fd, text, sizeof text);
 chk_str("what the pipe held", text, LOAD_PIPE_TEXT);
 load_release(&g);
 row_load_taken_is_bad_descriptor(before[0]);
 close(after[0]);
 close(after[1]);
 close(before[1]);
 load_hooks_set(0, 0);
}

/**
 * pipe_has_identity - Does the editor know this pipe by a device and a number now?
 * @fd: the pipe's read end.
 * Return: 1 when it does (not on NetBSD and OpenBSD, and not under the hook).
 */
static int pipe_has_identity(int fd)
{
 struct stat st;
 WpeSeenIdent id;

 if (fstat(fd, &st) != 0)
  return 0;
 wpe_seen_ident_of(&st, &id);
 return wpe_seen_ident_known(&id);
}

/**
 * row_load_replaced_descriptor - A number reopened on another pipe after the start.
 * @label: the row; @hide: play a system whose pipes have no identity.
 *
 * A started pipe that has an identity is also compared when asked, so a
 * number the editor closed and something else took is not read.  Where a pipe
 * has none, the number is all there is: the editor never closes a start
 * number but to take it, so the number is believed and the pipe loads.
 */
static void row_load_replaced_descriptor(const char *label, int hide)
{
 int started[2], other[2], known;
 char name[64];
 LoadGot g;

 test_watchdog_case(label, CELL_SECS);
 load_hooks_set(hide, 0);
 if (make_pipe_with_text(started) != 0 || make_pipe_with_text(other) != 0)
  return;
 known = pipe_has_identity(started[0]);
 wpe_read_note_start();
 if (dup2(other[0], started[0]) < 0)
  return;
 snprintf(name, sizeof name, "/dev/fd/%d", started[0]);
 load_take(name, &g);
 if (known)
  check_load_refused("load: a number another pipe took", &g, WPE_READ_NOT_A_FILE, 0, S_IFIFO, 0);
 else
 {
  check_load_pipe("load: no identity: the number is believed", &g);
  chk_int("the believed number comes back as itself", g.fd, started[0]);
 }
 load_release(&g);
 close(started[0]);
 close(started[1]);
 close(other[0]);
 close(other[1]);
 load_hooks_set(0, 0);
}

/**
 * row_load_write_only_pipe - A pipe's write end at 3 or above, where the system tells it.
 * @label: the row; @untold: play a system that does not tell a pipe's writing end.
 *
 * Where the system says O_WRONLY for the writing end (Linux, NetBSD), the end
 * is a bad descriptor and stays as it was.  Where it says two-way (FreeBSD,
 * OpenBSD, and the hook), nothing tells it from a reading end that
 * `<(command)` hands over, so a pipe end at 3 and above is read.  The loaded
 * end is not read here: under the hook it is a write end.
 */
static void row_load_write_only_pipe(const char *label, int untold)
{
 int ends[2], told;
 char name[64];
 LoadGot g;

 test_watchdog_case(label, CELL_SECS);
 load_hooks_set(0, untold);
 if (make_pipe_with_text(ends) != 0)
  return;
 told = load_end_is_told(ends) && !untold;
 wpe_read_note_start();
 snprintf(name, sizeof name, "/dev/fd/%d", ends[1]);
 load_take(name, &g);
 if (told)
 {
  check_load_refused("load: the write end of a pipe", &g, WPE_READ_REFUSED_BY_SYSTEM, EBADF, 0, 0);
  chk_int("the write end is still open", descriptor_is_open(ends[1]), 1);
 }
 else
 {
  check_load_pipe("load: a pipe end the system calls two-way is read", &g);
  chk_int("the two-way end comes back as itself", g.fd, ends[1]);
 }
 load_release(&g);
 close(ends[0]);
 close(ends[1]);
 load_hooks_set(0, 0);
}

/**
 * stand_pipe_end_at - Put one end of a pipe on a low descriptor number.
 * @ends: the pipe; @n: the number; @saved: receives a copy of what was there.
 * Return: 0, or -1 (counted).
 */
static int stand_pipe_end_at(const int ends[2], int n, int *saved)
{
 int end = ends[1];

 if (n == 0)
  end = ends[0];
 fflush(stdout);
 *saved = dup(n);
 if (*saved < 0 || dup2(end, n) < 0)
 {
  test_dir_fail("FAIL: setup pipe end at %d: %s\n", n, strerror(errno));
  return -1;
 }
 return 0;
}

/**
 * row_load_pipe_at_low - A pipe on descriptor 0, 1 or 2 where the editor started.
 * @n: the number; @untold: play a system that does not tell a pipe's writing
 * end (1), or one that does not tell it and whose probe pipe cannot be made
 * (LOAD_PLAY_PROBE_FAILS: the owner then takes the ends as untold).
 *
 * What the user gets: `xwpe /dev/fd/1 | cat` and `xwpe /dev/fd/2 2>&1 | cat`
 * keep their output on every system, `cmd | xwpe /dev/stdin` reads the data
 * and `xwpe /dev/fd/2 2<notes.txt` loads.  The writing end of a pipe stood at 1
 * or 2 is a bad descriptor and stays the pipe, never replaced by the null
 * device; the reading end at 0 is data.  The answers are checked once the
 * number is back, because a failure line printed while a pipe holds number 1
 * would go into the pipe.
 */
static void row_load_pipe_at_low(int n, int untold)
{
 int ends[2], saved, still_pipe;
 char name[64], label[96];
 struct stat now;
 LoadGot g;

 if (untold == LOAD_PLAY_PROBE_FAILS)
  snprintf(label, sizeof label, "load pipe at %d, probe fails", n);
 else if (untold)
  snprintf(label, sizeof label, "load pipe at %d, ends untold", n);
 else
  snprintf(label, sizeof label, "load pipe at %d", n);
 test_watchdog_case(label, CELL_SECS);
 if (untold == LOAD_PLAY_PROBE_FAILS)
  load_hooks_probe_fails();
 else
  load_hooks_set(0, untold);
 if (make_pipe_with_text(ends) != 0 || stand_pipe_end_at(ends, n, &saved) != 0)
  return;
 wpe_read_note_start();
 if (n == 0)
  snprintf(name, sizeof name, "/dev/stdin");
 else
  snprintf(name, sizeof name, "/dev/fd/%d", n);
 load_take(name, &g);
 still_pipe = fstat(n, &now) == 0 && S_ISFIFO(now.st_mode);
 dup2(saved, n);
 close(saved);
 if (n == 0)
  check_load_pipe("load: a pipe's reading end at 0 is data", &g);
 else
  check_load_refused("load: a pipe's writing end at 1 or 2", &g, WPE_READ_REFUSED_BY_SYSTEM, EBADF, 0, 0);
 if (n != 0)
  chk_int("the output stays the pipe, not the null device", still_pipe, 1);
 load_release(&g);
 close(ends[0]);
 close(ends[1]);
 load_hooks_set(0, 0);
}

/**
 * row_load_low_descriptor - Descriptor 0, 1 or 2 is refilled with /dev/null once taken.
 * @n: the number; @untold: play a system that does not tell a pipe's writing end.
 *
 * What goes wrong if the refill breaks: the next open of the editor gets the
 * number that standard error or output still writes to, and later lines land
 * in that file.  The stand-in for the number is a regular file, read as any
 * file; the number must then be the null device, and the original restored.
 * A regular file is never an output, whatever the number and the system, so
 * `xwpe /dev/fd/2 2<notes.txt` loads everywhere.
 */
static void row_load_low_descriptor(int n, int untold)
{
 struct stat now, null;
 char name[64];
 int saved, file, still_null;
 LoadGot g;

 test_watchdog_case("load low descriptor", CELL_SECS);
 load_hooks_set(0, untold);
 fflush(stdout);
 saved = dup(n);
 file = open(obj_path[OB_REG], O_RDONLY);
 if (saved < 0 || file < 0 || dup2(file, n) < 0)
 {
  test_dir_fail("FAIL: setup low descriptor %d: %s\n", n, strerror(errno));
  return;
 }
 close(file);
 wpe_read_note_start();
 if (n == 0)
  snprintf(name, sizeof name, "/dev/stdin");
 else
  snprintf(name, sizeof name, "/dev/fd/%d", n);
 load_take(name, &g);
 still_null = fstat(n, &now) == 0 && stat("/dev/null", &null) == 0 &&
              S_ISCHR(now.st_mode) && now.st_rdev == null.st_rdev;
 dup2(saved, n);
 close(saved);
 check_load_stream("load: a low descriptor holding a file", &g, LEN_REG, 1);
 chk_int("a taken low descriptor is the null device", still_null, 1);
 load_hooks_set(0, 0);
}

/**
 * row_load_unserved_name_is_the_descriptor - A descriptor name the system does not serve still means the descriptor.
 *
 * FreeBSD has no /dev/fd/3 without fdescfs and NetBSD has none above 63, yet
 * the descriptor is open and holds the text.  The name is never opened: the
 * hook that fails the open of this very name stays armed, and the pipe loads
 * as itself.
 */
static void row_load_unserved_name_is_the_descriptor(void)
{
 int ends[2];
 char name[64];
 LoadGot g;

 test_watchdog_case("load unserved name", CELL_SECS);
 if (make_pipe_with_text(ends) != 0)
  return;
 wpe_read_note_start();
 snprintf(name, sizeof name, "/dev/fd/%d", ends[0]);
 snprintf(fail_at, sizeof fail_at, "%s", name);
 fail_with = ENOENT;
 watch_name = name;
 watch_opens = 0;
 load_take(name, &g);
 watch_name = NULL;
 chk_int("the unserved name was never opened", fail_at[0] != '\0' && watch_opens == 0, 1);
 fail_at[0] = '\0';
 check_load_pipe("load: a name the system does not serve is the descriptor", &g);
 chk_int("the descriptor comes back as itself", g.fd, ends[0]);
 load_release(&g);
 close(ends[1]);
}

/**
 * row_load_stdin_kept - /dev/stdin is the kept descriptor once the backend moved the data.
 * @untold: play a system that does not tell a pipe's writing end.
 *
 * The terminal backend makes descriptor 0 the keyboard's terminal and keeps
 * the data apart.  The stand-in for the terminal here is the null device,
 * opened write-only: the number 0 then says "an output" while the kept
 * descriptor says "data", so a judgement that looks at the number instead of
 * the descriptor it holds refuses the load.
 */
static void row_load_stdin_kept(int untold)
{
 int ends[2], saved, quiet, kept;
 struct stat now, null;
 LoadGot g;
 int null_stays;

 test_watchdog_case("load stdin kept", CELL_SECS);
 load_hooks_set(0, untold);
 if (make_pipe_with_text(ends) != 0)
  return;
 saved = dup(0);
 if (saved < 0 || dup2(ends[0], 0) < 0)
  return;
 wpe_read_note_start();
 kept = fcntl(0, F_DUPFD_CLOEXEC, 3);
 quiet = open("/dev/null", O_WRONLY);
 if (kept < 0 || quiet < 0 || dup2(quiet, 0) < 0)
 {
  dup2(saved, 0);
  return;
 }
 close(quiet);
 wpe_read_stdin_kept(kept);
 load_take("/dev/stdin", &g);
 null_stays = fstat(0, &now) == 0 && stat("/dev/null", &null) == 0 && S_ISCHR(now.st_mode) &&
              now.st_rdev == null.st_rdev;
 dup2(saved, 0);
 close(saved);
 check_load_pipe("load: /dev/stdin after the data was kept apart", &g);
 chk_int("the kept descriptor is the one taken", g.fd, kept);
 chk_int("descriptor 0 stays the terminal's", null_stays, 1);
 load_release(&g);
 close(ends[0]);
 close(ends[1]);
 load_hooks_set(0, 0);
}

/**
 * row_load_names_stdin - Which names mean the standard input.
 */
static void row_load_names_stdin(void)
{
 test_watchdog_case("load names stdin", CELL_SECS);
 chk_int("/dev/stdin", wpe_read_names_stdin("/dev/stdin"), 1);
 chk_int("/dev/fd/0", wpe_read_names_stdin("/dev/fd/0"), 1);
 chk_int("/proc/self/fd/0", wpe_read_names_stdin("/proc/self/fd/0"), 1);
 chk_int("/dev/fd/3", wpe_read_names_stdin("/dev/fd/3"), 0);
 chk_int("/dev/fd/", wpe_read_names_stdin("/dev/fd/"), 0);
 chk_int("/dev/fd/0x", wpe_read_names_stdin("/dev/fd/0x"), 0);
 chk_int("/dev/stdin/", wpe_read_names_stdin("/dev/stdin/"), 0);
 chk_int("a plain name", wpe_read_names_stdin("notes.txt"), 0);
}

/**
 * row_load_own_fifo - A FIFO the editor made is refused; another in the same folder is read.
 *
 * The debugger's FIFOs sit on a file system like a user's.  The table is
 * keyed by path and judged by what the path leads to when asked, because
 * the debugger makes them again at every start.
 */
static void row_load_own_fifo(void)
{
 char own[TEST_DIR_PATH_SIZE], plain[TEST_DIR_PATH_SIZE], via[TEST_DIR_PATH_SIZE];
 LoadGot g;

 test_watchdog_case("load own fifo", CELL_SECS);
 (void)test_path_join(own, sizeof own, dir, "own_fifo");
 (void)test_path_join(plain, sizeof plain, dir, "plain_fifo");
 (void)test_path_join(via, sizeof via, dir, "own_link");
 if (mkfifo(own, 0600) != 0 || mkfifo(plain, 0600) != 0 || symlink("own_fifo", via) != 0)
 {
  test_dir_fail("FAIL: setup own fifo: %s\n", strerror(errno));
  return;
 }
 wpe_read_note_start();
 wpe_read_note_own_fifo(own);
 wpe_read_note_own_fifo(own);
 load_take(own, &g);
 check_load_refused("load: a FIFO the editor made", &g, WPE_READ_NOT_A_FILE, 0, S_IFIFO, 0);
 load_take(via, &g);
 check_load_refused("load: a link to a FIFO the editor made", &g, WPE_READ_NOT_A_FILE, 0, S_IFIFO, 0);
 load_take(plain, &g);
 check_load_pipe("load: another FIFO in the same folder", &g);
 load_release(&g);
 (void)remove(own);
 if (mkfifo(own, 0600) != 0)
  return;
 load_take(own, &g);
 check_load_refused("load: the FIFO made again at the same path", &g, WPE_READ_NOT_A_FILE, 0, S_IFIFO, 0);
}

/**
 * name_leads_to_pipe - Does this name, opened now, give the pipe with its text still in it?
 * @name: the candidate.  Return: 1 when it does, nothing read.
 *
 * The opened object must be a FIFO that poll shows readable and that holds
 * exactly the text the row put in (FIONREAD), so a name that leads somewhere
 * else, or to a pipe already drained, does not count.  The descriptor of the
 * test is closed again.
 */
static int name_leads_to_pipe(const char *name)
{
 struct pollfd p;
 struct stat st;
 int fd = open(name, O_RDONLY | O_NONBLOCK);
 int queued = -1;
 int counts = 0;

 if (fd < 0)
  return 0;
 p.fd = fd;
 p.events = POLLIN;
 p.revents = 0;
 if (fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode) && poll(&p, 1, 0) == 1 &&
     (p.revents & POLLIN) != 0 && ioctl(fd, FIONREAD, &queued) == 0)
  counts = queued == LOAD_PIPE_LEN;
 close(fd);
 return counts;
}

/**
 * load_name_restore - Give descriptor 0 back after load_name_of_pipe stood a pipe there.
 * @stand_in: what load_name_of_pipe gave; LOAD_NO_STAND_IN does nothing.
 */
static void load_name_restore(int stand_in)
{
 if (stand_in == LOAD_NO_STAND_IN)
  return;
 dup2(stand_in, 0);
 close(stand_in);
}

/**
 * load_name_of_pipe - Find a name that leads to an unnamed pipe this system has.
 * @fd: the read end of a pipe with its text in it; @out, @size: receive the name;
 * @stand_in: receives the saved descriptor 0 when the pipe had to stand there,
 * else LOAD_NO_STAND_IN (the caller gives it back with load_name_restore).
 *
 * What goes wrong if no name can be found: the rows that plant a link to an
 * unnamed pipe would test nothing.  The candidates, in order, are
 * /proc/self/fd/N, /dev/fd/N, and the pipe stood at descriptor 0 with
 * /dev/fd/0, which a system without fdescfs still serves.  Linux gives the
 * first, the BSDs the second or the third.
 * Return: 1 with the name, or 0 when no name leads to the pipe here.
 */
static int load_name_of_pipe(int fd, char *out, size_t size, int *stand_in)
{
 *stand_in = LOAD_NO_STAND_IN;
 snprintf(out, size, "/proc/self/fd/%d", fd);
 if (name_leads_to_pipe(out))
  return 1;
 snprintf(out, size, "/dev/fd/%d", fd);
 if (name_leads_to_pipe(out))
  return 1;
 *stand_in = dup(0);
 if (*stand_in < 0 || dup2(fd, 0) < 0)
 {
  if (*stand_in >= 0)
   close(*stand_in);
  *stand_in = LOAD_NO_STAND_IN;
  return 0;
 }
 snprintf(out, size, "/dev/fd/0");
 if (name_leads_to_pipe(out))
  return 1;
 load_name_restore(*stand_in);
 *stand_in = LOAD_NO_STAND_IN;
 return 0;
}

/**
 * link_kind - The kind of object a name leads to.
 * @path: the name.  Return: S_IFMT of what stat shows, 0 when it shows nothing.
 */
static mode_t link_kind(const char *path)
{
 struct stat st;

 if (stat(path, &st) != 0)
  return 0;
 return st.st_mode & S_IFMT;
}

/**
 * row_load_pipe_by_a_name - A name that leads to an unnamed pipe is refused, also when it is swapped in.
 *
 * A link to a name of a pipe the program made reads the bytes meant for the
 * other end.  Where the name is a device (the BSDs), the look refuses it by
 * its kind and the link is never opened; where it is a FIFO (Linux), the
 * look refuses it as a pipe nothing named.  The guard also runs on the object
 * held, so a name that was a regular file at the look and became such a link
 * before the open is caught there, on every system.
 */
static void row_load_pipe_by_a_name(void)
{
 int ends[2], stand_in;
 char target[64], plain[TEST_DIR_PATH_SIZE], link_path[TEST_DIR_PATH_SIZE];
 LoadGot g;

 test_watchdog_case("load pipe by a name", CELL_SECS);
 if (make_pipe_with_text(ends) != 0)
  return;
 if (!load_name_of_pipe(ends[0], target, sizeof target, &stand_in))
 {
  printf("SKIP load: no name leads to an unnamed pipe here\n");
  skips++;
  close(ends[0]);
  close(ends[1]);
  return;
 }
 (void)test_path_join(link_path, sizeof link_path, dir, "pipe_link");
 if (symlink(target, link_path) != 0)
 {
  test_dir_fail("FAIL: setup pipe link: %s\n", strerror(errno));
  load_name_restore(stand_in);
  return;
 }
 watch_name = link_path;
 watch_opens = 0;
 load_take(link_path, &g);
 watch_name = NULL;
 check_load_refused("load: a link to an unnamed pipe", &g, WPE_READ_NOT_A_FILE, 0, link_kind(link_path), 0);
 chk_int("the link to an unnamed pipe was never opened", watch_opens, 0);
 (void)test_path_join(plain, sizeof plain, dir, "swap_plain");
 (void)remove(link_path);
 if (make_other_file("swap_plain", "plain\n") != 0 || symlink(target, link_path) != 0)
 {
  load_name_restore(stand_in);
  return;
 }
 snprintf(swap_at, sizeof swap_at, "%s", plain);
 snprintf(swap_from, sizeof swap_from, "%s", link_path);
 load_take(plain, &g);
 check_load_refused("load: a name swapped for a link to an unnamed pipe", &g, WPE_READ_NOT_A_FILE, 0, S_IFIFO, 0);
 chk_int("the pipe behind the link keeps its bytes", (int)drain_pipe_text(ends[0], target, sizeof target), LOAD_PIPE_LEN);
 load_name_restore(stand_in);
 close(ends[0]);
 close(ends[1]);
}

/**
 * row_load_late_regular_file - A file the editor opened after the start is no start descriptor.
 *
 * What the user gets: /dev/fd/N of a file the editor holds for itself says
 * what the system says of a closed descriptor, not that it is "not a regular
 * file", and the descriptor is left alone.
 */
static void row_load_late_regular_file(void)
{
 char name[64];
 int fd;
 LoadGot g;

 test_watchdog_case("load late regular file", CELL_SECS);
 wpe_read_note_start();
 fd = open(obj_path[OB_REG], O_RDONLY);
 if (fd < 0)
  return;
 snprintf(name, sizeof name, "/dev/fd/%d", fd);
 load_take(name, &g);
 check_load_refused("load: a file opened after the start", &g, WPE_READ_REFUSED_BY_SYSTEM, EBADF, 0, 0);
 chk_int("the late file is left open", descriptor_is_open(fd), 1);
 close(fd);
}

/**
 * stat_gives_identity - Does the system give this object a device or a number at all?
 * @st: a raw fstat.  Return: 0 for device 0 and number 0 (a pipe on NetBSD and OpenBSD).
 *
 * Asks whether a number is there, not whether it differs from another's.
 */
static int stat_gives_identity(const struct stat *st)
{
 return (st->st_dev | st->st_ino) != 0;
}

/**
 * row_hook_hides_pipe_identity - The no-identity hook still makes a pipe anonymous.
 *
 * The rows that play NetBSD and OpenBSD mean nothing if the hook stops
 * working, so this asserts it: a pipe has an identity where the system gives
 * one (the device and number of a raw fstat), and under the hook none.
 */
static void row_hook_hides_pipe_identity(void)
{
 int ends[2];
 struct stat st;
 WpeSeenIdent id;

 test_watchdog_case("hook hides pipe identity", CELL_SECS);
 if (pipe(ends) != 0 || fstat(ends[0], &st) != 0)
  return;
 wpe_seen_ident_of(&st, &id);
 chk_int("a pipe has an identity where the system gives one", wpe_seen_ident_known(&id),
         stat_gives_identity(&st));
 load_hooks_set(1, 0);
 wpe_seen_ident_of(&st, &id);
 chk_int("a pipe under the hook has none", wpe_seen_ident_known(&id), 0);
 load_hooks_set(0, 0);
 close(ends[0]);
 close(ends[1]);
}

/**
 * row_load_by_name_under_hook - Under the hook an unnamed pipe by a link is refused, a named FIFO read.
 *
 * Where every pipe has device 0, the unnamed pipe is told from a FIFO on a
 * file system by its device, which the probe learned.
 */
static void row_load_by_name_under_hook(void)
{
 int ends[2], stand_in;
 char target[64], link_path[TEST_DIR_PATH_SIZE], plain[TEST_DIR_PATH_SIZE];
 LoadGot g;

 test_watchdog_case("load by name under hook", CELL_SECS);
 load_hooks_set(1, 0);
 wpe_read_note_start();
 if (make_pipe_with_text(ends) != 0)
  return;
 if (!load_name_of_pipe(ends[0], target, sizeof target, &stand_in))
 {
  printf("SKIP load under hook: no name leads to an unnamed pipe here\n");
  skips++;
  close(ends[0]);
  close(ends[1]);
  load_hooks_set(0, 0);
  return;
 }
 (void)test_path_join(link_path, sizeof link_path, dir, "hook_pipe_link");
 (void)test_path_join(plain, sizeof plain, dir, "hook_fifo");
 if (symlink(target, link_path) != 0 || mkfifo(plain, 0600) != 0)
 {
  load_name_restore(stand_in);
  load_hooks_set(0, 0);
  return;
 }
 load_take(link_path, &g);
 check_load_refused("load under hook: a link to an unnamed pipe", &g, WPE_READ_NOT_A_FILE, 0, link_kind(link_path), 0);
 load_take(plain, &g);
 check_load_pipe("load under hook: a FIFO on a file system", &g);
 load_release(&g);
 load_name_restore(stand_in);
 load_hooks_set(0, 0);
 close(ends[0]);
 close(ends[1]);
}

/**
 * run_load_rows - The load face beside its matrix.
 */
static void run_load_rows(void)
{
 int n;

 run_load_matrix();
 row_load_start_descriptors("load start descriptors", 0, 0);
 row_load_start_descriptors("load start descriptors, no pipe identity", 1, 0);
 row_load_start_descriptors("load start descriptors, ends untold", 0, 1);
 row_load_replaced_descriptor("load replaced descriptor", 0);
 row_load_replaced_descriptor("load replaced descriptor, no pipe identity", 1);
 row_load_write_only_pipe("load write-only pipe", 0);
 row_load_write_only_pipe("load write-only pipe, ends untold", 1);
 for (n = 0; n < LOW_NUMBERS; n++)
 {
  row_load_low_descriptor(n, 0);
  row_load_low_descriptor(n, 1);
  row_load_pipe_at_low(n, 0);
  row_load_pipe_at_low(n, 1);
 }
 row_load_pipe_at_low(1, LOAD_PLAY_PROBE_FAILS);
 row_load_pipe_at_low(2, LOAD_PLAY_PROBE_FAILS);
 row_load_unserved_name_is_the_descriptor();
 row_load_stdin_kept(0);
 row_load_stdin_kept(1);
 row_load_names_stdin();
 row_load_own_fifo();
 row_load_pipe_by_a_name();
 row_load_late_regular_file();
 row_hook_hides_pipe_identity();
 row_load_by_name_under_hook();
}

/**
 * remove_entry - nftw's callback: remove one entry of the test folder.
 */
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
 (void)st;
 (void)flag;
 (void)ftw;
 return(remove(path));
}

/**
 * remove_test_folder - Remove the folder the rows made, by its exact name.
 *
 * The folder is the one mkdtemp made; a link in it is removed, never followed.
 */
static void remove_test_folder(void)
{
 int i;

 for (i = 0; i < OB_COUNT; i++)
  if (obj_keep[i] > 0)
   close(obj_keep[i]);
 (void)chmod(dir, 0700);
 (void)nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

int main(void)
{
 int before_failures;

 if (test_dir_make(dir, sizeof dir, "read_owner") != 0)
 {
  printf("test_read_owner: no test folder\n");
  return 1;
 }
 if (make_objects() != 0)
  return 1;
 run_matrix();
 row_expect_same();
 row_expect_swapped();
 row_expect_touched_no_birth();
 row_swapped_to_fifo();
 row_link_by_name_not_errno();
 row_memory_stream();
 row_stream_end_is_the_last_byte();
 row_stream_keeps_a_full_buffer();
 row_grows_while_read();
 row_outcome_facts();
 row_name_ok();
 row_folder_entries();
 row_folder_entry_is_not_followed();
 row_sentences();
 row_terminal_by_descriptor();
 run_load_rows();
 test_watchdog_disarm();
 remove_test_folder();
 before_failures = fails + test_dir_failures;
 if (before_failures != 0)
 {
  printf("test_read_owner: %d failure(s)\n", before_failures);
  return 1;
 }
 printf("test_read_owner: ok (%d cells, %d skipped)\n", cells, skips);
 return 0;
}
