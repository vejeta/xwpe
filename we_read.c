/* we_read.c -- the one owner of reading a file or a folder by a name the
   editor did not choose (see we_read.h).

   The shape is the one the write owner and the names owner already use: look
   at the name, open without waiting and without taking a terminal, judge the
   descriptor before one byte is read, refuse what was not asked for, clear
   the non-blocking flag on a regular file.  A device node, a socket or a
   terminal is refused by its kind from the look, so it is never opened; only
   when the name changes between the look and the open is such an object
   opened, and then without waiting and without a controlling terminal, and
   refused before a byte is read.

   This file holds no sentence the user reads (we_path.c owns the words) and
   decides nothing about a refusal from an errno of one system: a link at the
   name is told by looking at the name again. */
#include "we_read.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "we_platform.h"

#define READ_FLAGS_BASE (O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC)
#define READ_CHUNK      ((size_t)65536)   /* what one read asks for */

/**
 * read_note_system - Record a refusal that came from the system.
 * @o: the outcome; @err: the errno.
 */
static void read_note_system(WpeReadOutcome *o, int err)
{
 o->why = WPE_READ_REFUSED_BY_SYSTEM;
 o->err = err;
}

/**
 * read_note_not_a_file - Record that something other than what was asked is there.
 * @o: the outcome; @kind: S_IFMT of what was found; @tty: a descriptor was
 * opened and it is a terminal; @folder: a folder was asked for.
 */
static void read_note_not_a_file(WpeReadOutcome *o, mode_t kind, int tty, int folder)
{
 o->why = WPE_READ_NOT_A_FILE;
 o->kind = kind & S_IFMT;
 o->tty = tty;
 o->want_folder = folder;
}

/**
 * read_note_stat - Keep what a look or an fstat said about the object.
 * @o: the outcome; @st: the stat.
 */
static void read_note_stat(WpeReadOutcome *o, const struct stat *st)
{
 o->size = st->st_size;
 o->mtime = st->st_mtime;
 o->mode = st->st_mode;
 o->uid = st->st_uid;
}

/**
 * read_kind_is_asked - Is this kind of object one the caller asked for?
 * @ask: the ask; @kind: S_IFMT of the object; @folder: the folder face.
 * Return: 1 when it may be read.
 */
static int read_kind_is_asked(const WpeReadAsk *ask, mode_t kind, int folder)
{
 if (folder)
  return S_ISDIR(kind);
 if (S_ISREG(kind))
  return (ask->kinds & WPE_READ_KIND_FILE) != 0;
 if (S_ISFIFO(kind))
  return (ask->kinds & WPE_READ_KIND_PIPE) != 0;
 return 0;
}

/**
 * read_bound_refuses - Does the bound refuse an object of this size?
 * @ask: the ask; @st: the stat of a regular file.
 * Return: 1 when the file is over a bound that refuses.
 */
static int read_bound_refuses(const WpeReadAsk *ask, const struct stat *st)
{
 if (ask->bound == WPE_READ_UNBOUNDED || ask->over != WPE_READ_WHOLE_OR_REFUSE)
  return 0;
 if (!S_ISREG(st->st_mode) || st->st_size < 0)
  return 0;
 return (unsigned long long)st->st_size > (unsigned long long)ask->bound;
}

/**
 * read_judge_stat - Decide from a stat whether the object may be read.
 * @ask: the ask; @st: the stat of the name or of the descriptor; @tty: the
 * descriptor is a terminal (0 for a look by name); @folder: the folder face;
 * @o: the outcome.
 * Return: 0 to go on, -1 with the outcome set.
 */
static int read_judge_stat(const WpeReadAsk *ask, const struct stat *st, int tty,
                           int folder, WpeReadOutcome *o)
{
 if (S_ISLNK(st->st_mode))
 {
  o->why = WPE_READ_IS_A_LINK;
  return -1;
 }
 if (!read_kind_is_asked(ask, st->st_mode, folder))
 {
  read_note_not_a_file(o, st->st_mode, tty, folder);
  return -1;
 }
 read_note_stat(o, st);
 if (!folder && read_bound_refuses(ask, st))
 {
  o->why = WPE_READ_TOO_LARGE;
  o->bound = ask->bound;
  return -1;
 }
 return 0;
}

/**
 * read_look_before - Look at the name before anything is opened.
 * @path: the name; @ask: the ask; @folder: the folder face; @o: the outcome.
 *
 * A pipe, a socket, a device and a terminal are told from this look and are
 * never opened, so nothing waits and no device sees an open.
 * Return: 0, or -1 with the outcome set.
 */
static int read_look_before(const char *path, const WpeReadAsk *ask, int folder,
                            WpeReadOutcome *o)
{
 struct stat st;
 int flags = ask->how == WPE_READ_NAMED_NOFOLLOW ? AT_SYMLINK_NOFOLLOW : 0;

 if (fstatat(ask->dirfd, path, &st, flags) != 0)
 {
  read_note_system(o, errno);
  return -1;
 }
 return read_judge_stat(ask, &st, 0, folder, o);
}

/**
 * read_target_open_fd - The one open of a file or a folder by name.
 * @path: the name; @ask: the ask; @folder: the folder face.
 *
 * Without waiting for a writer or a carrier, without becoming the
 * controlling terminal, closed on exec; not following a link at the name when
 * the ask says so.
 * Return: the descriptor, or -1 with errno set.
 */
static int read_target_open_fd(const char *path, const WpeReadAsk *ask, int folder)
{
 int flags = READ_FLAGS_BASE;

 if (ask->how == WPE_READ_NAMED_NOFOLLOW)
  flags |= O_NOFOLLOW;
 if (folder)
  flags |= O_DIRECTORY;
 return openat(ask->dirfd, path, flags);
}

/**
 * read_open_failure - Say why an open failed.
 * @path: the name; @ask: the ask; @err: the errno of the open; @o: the outcome.
 *
 * Another file at the name is decided from the name, never from the errno,
 * because each system has its own word for a link met by an open: when links
 * are not followed the name is looked at again and a link is named as one.
 */
static void read_open_failure(const char *path, const WpeReadAsk *ask, int err,
                              WpeReadOutcome *o)
{
 struct stat st;

 if (ask->how == WPE_READ_NAMED_NOFOLLOW &&
     fstatat(ask->dirfd, path, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode))
 {
  o->why = WPE_READ_IS_A_LINK;
  return;
 }
 read_note_system(o, err);
}

/**
 * read_target_expected - Is the object held the one the caller looked at?
 * @ask: the ask; @o: the outcome, whose seen is the object held.
 * Return: 0 when nothing was expected or it is the same, -1 with the outcome set.
 */
static int read_target_expected(const WpeReadAsk *ask, WpeReadOutcome *o)
{
 WpeSeenIs is;

 if (ask->expect == NULL)
  return 0;
 is = wpe_seen_judge(ask->expect, &o->seen);
 if (is == WPE_SEEN_SAME)
  return 0;
 o->why = WPE_READ_OTHER_FILE;
 o->unsure = is == WPE_SEEN_UNSURE;
 return -1;
}

/**
 * read_target_hold - Judge the object a descriptor holds, before a byte is read.
 * @fd: the descriptor; @ask: the ask; @folder: the folder face; @o: the outcome.
 * Return: 0, or -1 with the outcome set; the descriptor is not closed here.
 */
static int read_target_hold(int fd, const WpeReadAsk *ask, int folder, WpeReadOutcome *o)
{
 struct stat st;
 int tty;

 if (fstat(fd, &st) != 0)
 {
  read_note_system(o, errno);
  return -1;
 }
 tty = S_ISCHR(st.st_mode) && isatty(fd);
 if (read_judge_stat(ask, &st, tty, folder, o) != 0)
  return -1;
 if (wpe_seen_look_fd(fd, &o->seen) != 0)
 {
  read_note_system(o, errno);
  return -1;
 }
 return read_target_expected(ask, o);
}

/**
 * read_clear_nonblock - Back to blocking reads on a regular file.
 * @fd: the descriptor; @ask: the ask; @o: the outcome (what was found).
 *
 * A pipe asked for by kind stays non-blocking: its reader waits in an event
 * loop, not in read.
 */
static void read_clear_nonblock(int fd, const WpeReadAsk *ask, const WpeReadOutcome *o)
{
 int fl;

 if (S_ISFIFO(o->mode) && (ask->kinds & WPE_READ_KIND_PIPE))
  return;
 fl = fcntl(fd, F_GETFL);
 if (fl >= 0)
  (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
}

/**
 * read_begin - Open, judge and release a name for reading.
 * @path: the name; @ask: the ask; @folder: the folder face; @o: the outcome.
 * Return: the descriptor, or -1 with the outcome set.
 */
static int read_begin(const char *path, const WpeReadAsk *ask, int folder, WpeReadOutcome *o)
{
 int fd;

 memset(o, 0, sizeof *o);
 o->why = WPE_READ_DONE;
 if (read_look_before(path, ask, folder, o) != 0)
  return -1;
 fd = read_target_open_fd(path, ask, folder);
 if (fd < 0)
 {
  read_open_failure(path, ask, errno, o);
  return -1;
 }
 if (read_target_hold(fd, ask, folder, o) != 0)
 {
  close(fd);
  return -1;
 }
 if (!folder)
  read_clear_nonblock(fd, ask, o);
 return fd;
}

/**
 * wpe_read_open - Open a file for a reader that has its own loop.
 * @path: the name; @ask: what the caller asks for; @o: the outcome.
 *
 * Lets a copy, a load or a search read a file the user or a folder named
 * without ever waiting on a pipe or taking the user's keys from a terminal.
 * Return: the descriptor, judged and no longer non-blocking, or -1.
 */
int wpe_read_open(const char *path, const WpeReadAsk *ask, WpeReadOutcome *o)
{
 return read_begin(path, ask, 0, o);
}

/**
 * wpe_read_name_ok - Judge a name without opening it.
 * @path: the name; @ask: what the caller asks for; @o: the outcome.
 *
 * Lets a caller refuse before it changes anything: a project about to
 * replace the open one is checked first, so a pipe or an oversized file
 * leaves the open project as it was.
 * Return: 0 when the name leads to a file that may be read, else -1.
 */
int wpe_read_name_ok(const char *path, const WpeReadAsk *ask, WpeReadOutcome *o)
{
 memset(o, 0, sizeof *o);
 o->why = WPE_READ_DONE;
 return read_look_before(path, ask, 0, o);
}

/**
 * read_buffer_grow - Double the buffer a read is filling.
 * @buf: the buffer; @cap: its size, doubled on success; @o: the outcome.
 *
 * Lets a large file be read whole without the caller guessing its size
 * (a file under /proc says 0).  When memory runs out the user gets the
 * usual out-of-memory answer, never a half-read file.
 * Return: the grown buffer, or NULL after freeing @buf with the outcome set.
 */
static char *read_buffer_grow(char *buf, size_t *cap, WpeReadOutcome *o)
{
 size_t want = *cap * 2;
 char *grown = realloc(buf, want);

 if (grown == NULL)
 {
  free(buf);
  o->why = WPE_READ_NO_MEMORY;
  return NULL;
 }
 *cap = want;
 return grown;
}

/**
 * read_all_bounded - Read a descriptor to its end, never past the bound.
 * @fd: the descriptor; @ask: the ask; @len: receives the byte count; @o: the outcome.
 *
 * The count is of the bytes really read: a file under /proc or /sys reports
 * a size of 0 and gives more, so the size is only an early refusal.  One
 * byte over the bound is TOO_LARGE, or the cut when the ask says cut.
 * Return: the bytes (NUL-ended, malloc'd), or NULL with the outcome set.
 */
static char *read_all_bounded(int fd, const WpeReadAsk *ask, size_t *len, WpeReadOutcome *o)
{
 size_t cap = READ_CHUNK, used = 0, want;
 char *buf = malloc(cap);
 ssize_t got;

 if (buf == NULL)
 {
  o->why = WPE_READ_NO_MEMORY;
  return NULL;
 }
 for (;;)
 {
  if (ask->bound != WPE_READ_UNBOUNDED && used > ask->bound)
   break;
  if (used + READ_CHUNK + 1 > cap)
  {
   buf = read_buffer_grow(buf, &cap, o);
   if (buf == NULL)
    return NULL;
  }
  want = cap - used - 1;
  if (ask->bound != WPE_READ_UNBOUNDED && want > ask->bound + 1 - used)
   want = ask->bound + 1 - used;
  got = read(fd, buf + used, want);
  if (got < 0 && errno == EINTR)
   continue;
  if (got < 0)
  {
   read_note_system(o, errno);
   free(buf);
   return NULL;
  }
  if (got == 0)
   break;
  used += (size_t)got;
 }
 if (ask->bound != WPE_READ_UNBOUNDED && used > ask->bound)
 {
  if (ask->over == WPE_READ_WHOLE_OR_REFUSE)
  {
   o->why = WPE_READ_TOO_LARGE;
   o->bound = ask->bound;
   free(buf);
   return NULL;
  }
  used = ask->bound;
  o->cut = 1;
  o->bound = ask->bound;
 }
 buf[used] = '\0';
 *len = used;
 return buf;
}

/**
 * wpe_read_text - Read a whole file into memory.
 * @path: the name; @ask: what the caller asks for; @len: receives the byte
 * count (the bytes may hold NUL); @o: the outcome.
 *
 * Lets a caller that writes the file back (options, a project, a before
 * image) hold it whole or not at all: a file over the bound is refused with
 * the bound named, never cut short and saved short.
 * Return: the bytes, NUL-ended, to be freed; NULL with the outcome set.
 */
char *wpe_read_text(const char *path, const WpeReadAsk *ask, size_t *len, WpeReadOutcome *o)
{
 char *text;
 int fd = read_begin(path, ask, 0, o);

 if (fd < 0)
  return NULL;
 text = read_all_bounded(fd, ask, len, o);
 close(fd);
 return text;
}

/**
 * ReadStreamHold - The bytes a memory stream gives a reader, and where it is.
 * @bytes: the copy the stream owns, freed when the caller closes the stream.
 * @len: how many bytes a reader may get.
 * @at: where the next read starts.
 */
typedef struct {
 char *bytes;
 size_t len;
 size_t at;
} ReadStreamHold;

/**
 * read_stream_release - Free a hold's bytes, and the hold itself.
 * @hold: the bytes the stream was given.
 *
 * Both callback families take this as the close of the stream, so fclose frees
 * the bytes and no caller keeps a second pointer to release.  The temporary
 * file a system with neither family falls back to calls it itself, once the
 * file holds the bytes.
 * Return: 0, the answer both families of a memory stream take.
 */
static int read_stream_release(void *hold)
{
 ReadStreamHold *h = hold;

 free(h->bytes);
 free(h);
 return 0;
}

/* A callback family reads and seeks through functions of its own; a temporary
   file needs neither, so the helpers both families share live under them. */
#if WPE_PLATFORM_MEMORY_STREAM != WPE_PLATFORM_MEMORY_STREAM_NONE

/**
 * read_stream_slice - How many bytes a read of this much gives from where the stream is.
 * @h: the stream's own bytes; @want: what the reader asked for.
 * Return: the bytes to give, 0 at the end of them.
 */
static size_t read_stream_slice(const ReadStreamHold *h, size_t want)
{
 if (h->at >= h->len)
  return 0;
 if (want > h->len - h->at)
  return h->len - h->at;
 return want;
}

/**
 * read_stream_take - Give a reader the bytes at the stream's place and move past them.
 * @hold: the stream's own bytes; @buf/@want: the reader's buffer and how many
 * bytes it asked for.
 *
 * A want larger than what is left gives what is left, so a reader is never
 * given a byte the file does not hold.  The funopen family counts in an int and
 * its wrapper refuses a negative count before it reaches here, so what comes in
 * is always a count the reader asked for.
 * Return: the bytes given, 0 at the end of them (which sets the stream's end).
 */
static ssize_t read_stream_take(ReadStreamHold *hold, char *buf, size_t want)
{
 size_t take = read_stream_slice(hold, want);

 memcpy(buf, hold->bytes + hold->at, take);
 hold->at += take;
 return (ssize_t)take;
}

/**
 * read_stream_move_to - Land a seek of the stream, and say where it landed.
 * @hold: the stream's own bytes; @by: the place asked for; @whence: what it
 * counts from.
 *
 * What the user gets: a place inside the file's own bytes, so ftell never
 * reports a stream longer than the file it came from.  A seek past the end is
 * refused, where a file would allow it and leave the gap to read as the end of
 * the file: this stream's length is the file's, and a place past it is a place
 * no byte of the file answers to.
 * Return: the place, or -1 with errno set.
 */
static long long read_stream_move_to(ReadStreamHold *hold, long long by, int whence)
{
 long long at = (long long)hold->at;

 if (whence == SEEK_SET)
  at = by;
 else if (whence == SEEK_CUR)
  at += by;
 else if (whence == SEEK_END)
  at = (long long)hold->len + by;
 else
 {
  errno = EINVAL;
  return -1;
 }
 if (at < 0 || (unsigned long long)at > (unsigned long long)hold->len)
 {
  errno = EINVAL;
  return -1;
 }
 hold->at = (size_t)at;
 return at;
}

#endif

/* The three constructions of the stream, one per system, each named
   read_stream_of_hold: the two callback families differ only in the shape the
   library asks for, and the third holds the bytes in a temporary file.  All
   three leave the hold to the caller when they fail and take it when they
   succeed, and each says in @o the cause it alone knows. */
#if WPE_PLATFORM_MEMORY_STREAM == WPE_PLATFORM_MEMORY_STREAM_COOKIE

/**
 * read_stream_cookie_read - The read the cookie family calls.
 * @hold: the stream's own bytes; @buf/@want: the reader's buffer and how many
 * bytes it asked for.
 *
 * What the user gets: the file's own bytes from where the reader is, and then
 * the end of the stream, so a line loop stops where the file does and a reader
 * that counts is given the count the file has.
 * Return: the bytes given, 0 at the end of them.
 */
static ssize_t read_stream_cookie_read(void *hold, char *buf, size_t want)
{
 return read_stream_take(hold, buf, want);
}

/**
 * read_stream_cookie_seek - The seek the cookie family calls.
 * @hold: the stream's own bytes; @pos: the place asked for, which this lands;
 * @whence: what the place counts from.
 *
 * What the user gets: an fseek that lands inside the file's own bytes and an
 * ftell that never reports a longer file.  The place is the type this library's
 * own prototype asks for, which is glibc's wider one and off_t elsewhere.
 * Return: 0 when the place landed, -1 with errno set when it did not.
 */
static int read_stream_cookie_seek(void *hold, WpePlatformStreamOffset *pos, int whence)
{
 long long at = read_stream_move_to(hold, (long long)*pos, whence);

 if (at < 0)
  return -1;
 *pos = (WpePlatformStreamOffset)at;
 return 0;
}

/**
 * read_stream_of_hold - A stream of the cookie family over bytes it owns.
 * @hold: the bytes, which the stream frees when the caller closes it;
 * @o: the outcome, which says why no stream could be had.
 *
 * What the user gets: a stream over the bytes the read owner took from the
 * file, which the caller reads and closes and does nothing else for.
 * Return: the stream, or NULL when the library could not make one.
 */
static FILE *read_stream_of_hold(ReadStreamHold *hold, WpeReadOutcome *o)
{
 cookie_io_functions_t calls;
 FILE *fp;

 calls.read = read_stream_cookie_read;
 calls.write = NULL;
 calls.seek = read_stream_cookie_seek;
 calls.close = read_stream_release;
 fp = fopencookie(hold, "r", calls);
 if (fp == NULL)
  o->why = WPE_READ_NO_MEMORY;   /* the one thing that stops this call */
 return fp;
}

#elif WPE_PLATFORM_MEMORY_STREAM == WPE_PLATFORM_MEMORY_STREAM_FUNOPEN

/**
 * read_stream_fun_read - The read the funopen family calls.
 * @hold: the stream's own bytes; @buf/@want: the reader's buffer and how many
 * bytes it asked for, in the int this family counts in.
 *
 * What the user gets: the file's own bytes from where the reader is, and then
 * the end of the stream.  A count the family cannot hold, an int that is
 * negative, is answered as an error instead of read as a size, so no reader is
 * given bytes from anywhere but the file.
 * Return: the bytes given, 0 at the end of them, -1 with errno set on a count
 * that is not one.
 */
static int read_stream_fun_read(void *hold, char *buf, int want)
{
 if (want < 0)
 {
  errno = EINVAL;
  return -1;
 }
 return (int)read_stream_take(hold, buf, (size_t)want);
}

/**
 * read_stream_fun_seek - The seek the funopen family calls.
 * @hold: the stream's own bytes; @pos: the place asked for; @whence: what it
 * counts from.
 *
 * What the user gets: an fseek that lands inside the file's own bytes and an
 * ftell that never reports a longer file.
 * Return: the place it landed, or -1 with errno set when it did not.
 */
static off_t read_stream_fun_seek(void *hold, off_t pos, int whence)
{
 return (off_t)read_stream_move_to(hold, (long long)pos, whence);
}

/**
 * read_stream_of_hold - A stream of the funopen family over bytes it owns.
 * @hold: the bytes, which the stream frees when the caller closes it;
 * @o: the outcome, which says why no stream could be had.
 *
 * What the user gets: a stream over the bytes the read owner took from the
 * file, which the caller reads and closes and does nothing else for.
 * Return: the stream, or NULL when the library could not make one.
 */
static FILE *read_stream_of_hold(ReadStreamHold *hold, WpeReadOutcome *o)
{
 FILE *fp = funopen(hold, read_stream_fun_read, NULL, read_stream_fun_seek,
                    read_stream_release);

 if (fp == NULL)
  o->why = WPE_READ_NO_MEMORY;   /* the one thing that stops this call */
 return fp;
}

#else

/**
 * read_stream_of_hold - A stream over a temporary file, where no family exists.
 * @hold: the bytes, which the file takes a copy of and this then frees;
 * @o: the outcome, which says why no stream could be had.
 *
 * What the user gets: the same stream the two families give, on a system that
 * has neither (illumos, AIX).  A reader is given the file's own bytes and then
 * the end of the stream, and fclose is the whole of the cleanup.  What it costs
 * is a file in the temp folder for as long as the stream is open, a descriptor
 * of its own, and a stream a caller could write to, where neither family gives
 * one: the callers of this face read, and a write would change only the copy.
 * Return: the stream, or NULL when no temporary file could be had or filled.
 */
static FILE *read_stream_of_hold(ReadStreamHold *hold, WpeReadOutcome *o)
{
 size_t len = hold->len;
 size_t put;
 FILE *fp = tmpfile();

 if (fp == NULL)
 {
  read_note_system(o, errno);   /* a full descriptor table, a temp folder that refuses */
  return NULL;
 }
 put = len == 0 ? 0 : fwrite(hold->bytes, 1, len, fp);
 if (put != len)
 {
  read_note_system(o, errno);   /* the temp folder ran out of room, or the file failed */
  fclose(fp);
  return NULL;
 }
 rewind(fp);
 read_stream_release(hold);   /* the file holds the bytes now */
 return fp;
}

#endif

/**
 * read_stream_of_bytes - A stream over bytes the stream then owns.
 * @bytes/@len: the bytes (malloc'd) and how many of them a reader may get;
 * @o: the outcome, which says why no stream could be had.
 *
 * What the user gets: a reader of a file under a bound reads that file's bytes
 * and then the end of the stream, so a line loop stops where the file does and
 * a reader that counts is given the count the file has, on every system.  The
 * stream owns the bytes and fclose frees them, so no caller keeps a second
 * pointer to release.  A library's own memory stream does neither on every
 * system: with a buffer of the caller's it leaks when the caller only closes
 * the stream, and with none of its own it hands a reader a byte the file does
 * not hold wherever its read runs to the end of the buffer it was given.  An
 * empty text is a stream already at its end.
 * Return: the stream, or NULL with the outcome set (the bytes are freed).
 */
static FILE *read_stream_of_bytes(char *bytes, size_t len, WpeReadOutcome *o)
{
 ReadStreamHold *hold = malloc(sizeof *hold);
 FILE *fp;

 if (hold == NULL)
 {
  free(bytes);
  o->why = WPE_READ_NO_MEMORY;
  return NULL;
 }
 hold->bytes = bytes;
 hold->len = len;
 hold->at = 0;
 fp = read_stream_of_hold(hold, o);
 if (fp == NULL)
  read_stream_release(hold);
 return fp;
}

/**
 * read_stream_of_descriptor - A stream on a judged descriptor.
 * @fd: the descriptor; @o: the outcome.
 * Return: the stream (fclose closes the descriptor), or NULL.
 */
static FILE *read_stream_of_descriptor(int fd, WpeReadOutcome *o)
{
 FILE *fp = fdopen(fd, "r");

 if (fp == NULL)
 {
  o->why = WPE_READ_NO_MEMORY;
  close(fd);
 }
 return fp;
}

/**
 * wpe_read_stream_open - Open a file for a reader that takes a line at a time.
 * @path: the name; @ask: what the caller asks for; @o: the outcome.
 *
 * Lets the header check, the include search, the option and project readers
 * keep their fgets and getline loops over a name a user or a folder chose.
 * A bound that refuses gives a stream over the bytes read whole, so the
 * reader never sees more than the bound; no bound gives a stream on the
 * descriptor, which ends when the regular file does.  Close it with fclose.
 * Return: the stream, or NULL with the outcome set.
 */
FILE *wpe_read_stream_open(const char *path, const WpeReadAsk *ask, WpeReadOutcome *o)
{
 char *text;
 size_t len;
 int fd = read_begin(path, ask, 0, o);

 if (fd < 0)
  return NULL;
 if (ask->bound == WPE_READ_UNBOUNDED)
  return read_stream_of_descriptor(fd, o);
 text = read_all_bounded(fd, ask, &len, o);
 close(fd);
 if (text == NULL)
  return NULL;
 return read_stream_of_bytes(text, len, o);   /* the stream owns the bytes now */
}

/**
 * wpe_read_folder - Open a folder to list it.
 * @path: the name; @ask: what the caller asks for; @o: the outcome.
 *
 * The one way to list a folder: opened as a folder and judged on the
 * descriptor, so a name that is a pipe is refused by its kind before it is
 * opened.
 * Return: the folder, to be closed with closedir, or NULL with the outcome set.
 */
DIR *wpe_read_folder(const char *path, const WpeReadAsk *ask, WpeReadOutcome *o)
{
 DIR *d;
 int fd = read_begin(path, ask, 1, o);

 if (fd < 0)
  return NULL;
 d = fdopendir(fd);
 if (d == NULL)
 {
  read_note_system(o, errno);
  close(fd);
 }
 return d;
}

/**
 * wpe_read_entry_look - What an entry of an open folder is.
 * @d: the folder; @name: the entry's name; @s: receives the look.
 *
 * Asked of the folder's descriptor and the name, never of a joined path, so a
 * folder swapped for a link in between is not followed.  A link is reported
 * as a link.
 * Return: 0, or -1 with errno set.
 */
int wpe_read_entry_look(DIR *d, const char *name, WpeSeen *s)
{
 return wpe_seen_look_at(dirfd(d), name, 0, s);
}

/**
 * wpe_read_entry_look_target - What an entry of an open folder leads to.
 * @d: the folder; @name: the entry's name; @s: receives the look.
 *
 * Lets a listing show a link as the file it points to (its size, its date,
 * whether it is a folder), asked of the folder's descriptor and the name like
 * wpe_read_entry_look.  A link whose target is gone has no target: the call
 * fails and the caller asks wpe_read_entry_look for the link itself.
 * Return: 0, or -1 with errno set.
 */
int wpe_read_entry_look_target(DIR *d, const char *name, WpeSeen *s)
{
 return wpe_seen_look_at(dirfd(d), name, 1, s);
}

/**
 * wpe_read_entry_kind - What kind of thing an entry of an open folder is.
 * @d: the folder; @name: the entry's name.
 *
 * Lets a listing offer only what can be read: a folder, a pipe or a socket
 * called t.c is told from a file without opening it.  A link is reported as
 * a link.
 * Return: the S_IFMT bits, 0 when the entry cannot be looked at.
 */
mode_t wpe_read_entry_kind(DIR *d, const char *name)
{
 WpeSeen s;

 if (wpe_read_entry_look(d, name, &s) != 0)
  return 0;
 return wpe_seen_kind(&s);
}

/**
 * wpe_read_outcome_errno - An outcome for a refusal the system gave.
 * @o: the outcome; @err: the errno.
 */
void wpe_read_outcome_errno(WpeReadOutcome *o, int err)
{
 memset(o, 0, sizeof *o);
 read_note_system(o, err);
}

/**
 * wpe_read_outcome_missing - Was the name refused only because it does not exist?
 * @o: the outcome.
 */
int wpe_read_outcome_missing(const WpeReadOutcome *o)
{
 return o->why == WPE_READ_REFUSED_BY_SYSTEM && o->err == ENOENT;
}

/**
 * read_reason_words - The fixed words of a refusal that is not a size.
 * Return: the words, never NULL; "" for no refusal.
 */
static const char *read_reason_words(const WpeReadOutcome *o)
{
 switch (o->why)
 {
 case WPE_READ_DONE:
  return "";
 case WPE_READ_REFUSED_BY_SYSTEM:
  return wpe_path_refused_reason(o->err);
 case WPE_READ_NOT_A_FILE:
  if (o->want_folder)
   return wpe_path_reason_of(WPE_PATH_REASON_NOT_FOLDER);
  return wpe_path_kind_reason(o->kind, o->tty);
 case WPE_READ_IS_A_LINK:
  return wpe_path_reason_of(WPE_PATH_REASON_LINK);
 case WPE_READ_OTHER_FILE:
  return wpe_path_reason_of(o->unsure ? WPE_PATH_REASON_CHANGED : WPE_PATH_REASON_OTHER);
 case WPE_READ_NO_WRITER:
  return wpe_path_reason_of(WPE_PATH_REASON_NO_WRITER);
 case WPE_READ_NO_MEMORY:
  return strerror(ENOMEM);
 case WPE_READ_TOO_LARGE:
  break;
 }
 return strerror(EIO);
}

/**
 * wpe_read_reason - The words of a refusal, as the words owner spells them.
 * @out/@n: the buffer; @o: the outcome.
 * Return: the length written, without the terminator; 0 for no refusal.
 */
int wpe_read_reason(char *out, size_t n, const WpeReadOutcome *o)
{
 if (o->why == WPE_READ_TOO_LARGE)
  return wpe_path_too_large_reason(out, n, o->bound);
 (void)wpe_fmt_shown(out, n, "%s", read_reason_words(o));
 return n > 0 ? (int)strlen(out) : 0;
}

/**
 * wpe_read_say - The sentence a refusal gets.
 * @out/@n: the buffer; @what: the noun, ASCII lowercase; @path: the name;
 * @o: the outcome.
 *
 * A caller with no box (the assistant's pane, the start plan's text) says
 * this in its own sink.  A sentence longer than the buffer ends in "...";
 * wpe_read_say_dup gives it at its real length.
 * Return: the length written, without the terminator.
 */
int wpe_read_say(char *out, size_t n, const char *what, const char *path,
                 const WpeReadOutcome *o)
{
 char reason[WPE_PATH_MSG_MAX];

 if (o->why == WPE_READ_REFUSED_BY_SYSTEM)
  return wpe_path_say_refused(out, n, what, path, o->err);
 (void)wpe_read_reason(reason, sizeof reason, o);
 return wpe_path_say_refused_because(out, n, what, path, reason);
}

/**
 * wpe_read_say_dup - The sentence a refusal gets, at its real length.
 * Used by a box that shows a long path with the reason after it, so the
 * reason is not cut off.
 * Return: the sentence (free it), or NULL when out of memory.
 */
char *wpe_read_say_dup(const char *what, const char *path, const WpeReadOutcome *o)
{
 char reason[WPE_PATH_MSG_MAX];

 if (o->why == WPE_READ_REFUSED_BY_SYSTEM)
  return wpe_path_say_refused_dup(what, path, o->err);
 (void)wpe_read_reason(reason, sizeof reason, o);
 return wpe_path_say_refused_because_dup(what, path, reason);
}
