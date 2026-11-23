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
 * Each cell runs under test_watchdog_case, so a wait is a FAIL that names
 * the cell, never a hang.
 */
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <utime.h>

#include "test_watchdog.h"
#include "interpose.h"
#include "testdir.h"
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

#ifdef __USE_FORTIFY_LEVEL
#if __USE_FORTIFY_LEVEL > 0
/** The fortified openat with no mode: the same hook. */
int __openat_2(int dirfd, const char *path, int flags)
{
 return(openat_hooked(dirfd, path, flags, 0));
}
#endif
#endif

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
 * text; the stream has no descriptor (fileno is -1) and fclose frees its
 * bytes.
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
 chk_int("no descriptor", fileno(fp), -1);
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
 row_grows_while_read();
 row_outcome_facts();
 row_name_ok();
 row_folder_entries();
 row_folder_entry_is_not_followed();
 row_sentences();
 row_terminal_by_descriptor();
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
