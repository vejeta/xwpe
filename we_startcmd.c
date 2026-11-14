/* we_startcmd.c -- the programs a project folder names, run only after a yes.
 *
 * See we_startcmd.h.  The owner of one decision: may the command a start-folder xwperc or a
 * project file names be run, or must the user be asked first?  It keeps the
 * texts the user counts as his own, and the answers he gave, one per folder,
 * key and text, in $XDG_STATE_HOME/xwpe/start-commands.  The start plan's
 * owner (we_startplan.c) keeps its answers here too.  The store sits in the
 * user's state folder because anything inside a project, which a clone or
 * the assistant can write, could otherwise approve itself.
 */
#include "we_startcmd.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/* The modes of the state folder and of the store: the user's alone. */
#define STARTCMD_DIR_MODE  0700
#define STARTCMD_FILE_MODE 0600

/* The words a record gives its answer. */
#define STARTCMD_WORD_YES "yes"
#define STARTCMD_WORD_NO  "no"

/* A record has its answer, key, folder and command, split by tabs. */
#define STARTCMD_FIELDS 4

/* The temporary file a write goes through, beside the store. */
#define STARTCMD_TEMP_SUFFIX ".XXXXXX"

/* One remembered answer. */
typedef struct {
 int answer;        /* WPE_STARTCMD_YES or WPE_STARTCMD_NO */
 char *key;
 char *folder;      /* resolved */
 char *command;     /* the whole text, compared byte for byte */
} startcmd_record;

/* The store, as read. */
typedef struct {
 startcmd_record *rec;
 int n;
} startcmd_store;

/* A text the user counts as his own for a key. */
typedef struct {
 char *key;
 char *text;
} startcmd_own;

static startcmd_own *startcmd_owns;
static int startcmd_nown;
static int startcmd_store_state = WPE_STARTCMD_STORE_OK;

/** startcmd_dup - A malloc'd copy of a text, or NULL out of memory. */
static char *startcmd_dup(const char *s)
{
 size_t n = strlen(s) + 1;
 char *p = malloc(n);
 if (p) memcpy(p, s, n);
 return p;
}

/**
 * wpe_startcmd_reset - Forget every text noted as the user's own.
 *
 * Used when the options are read again, so a setting the user dropped from
 * his own file no longer lets a folder's command run unasked.
 */
void wpe_startcmd_reset(void)
{
 int i;
 for (i = 0; i < startcmd_nown; i++) {
  free(startcmd_owns[i].key);
  free(startcmd_owns[i].text);
 }
 free(startcmd_owns);
 startcmd_owns = NULL;
 startcmd_nown = 0;
}

/**
 * wpe_startcmd_is_own - Is this the user's own command for the key?
 * @key: the key; @text: the command as it would run.
 * Return: 1 when the text was noted as his own, the built-in default
 * included; such a command runs without a question.
 */
int wpe_startcmd_is_own(const char *key, const char *text)
{
 int i;
 if (!key || !text) return 0;
 for (i = 0; i < startcmd_nown; i++)
  if (strcmp(startcmd_owns[i].key, key) == 0 && strcmp(startcmd_owns[i].text, text) == 0)
   return 1;
 return 0;
}

/**
 * wpe_startcmd_note_own - Count a command as the user's own for a key.
 * @key: the key; @text: the command.
 *
 * Called for the built-in default of every program key and for the value
 * the user's own config gives it, and when the user types a command in
 * Options: a folder whose command is one of these is not asked about.
 * A key may have several such texts.
 */
void wpe_startcmd_note_own(const char *key, const char *text)
{
 startcmd_own *grown;
 char *k, *t;
 if (!key || !text || wpe_startcmd_is_own(key, text)) return;
 k = startcmd_dup(key);
 t = startcmd_dup(text);
 grown = realloc(startcmd_owns, (size_t)(startcmd_nown + 1) * sizeof *grown);
 if (!k || !t || !grown) {
  free(k);
  free(t);
  if (grown) startcmd_owns = grown;
  return;
 }
 startcmd_owns = grown;
 startcmd_owns[startcmd_nown].key = k;
 startcmd_owns[startcmd_nown].text = t;
 startcmd_nown++;
}

/**
 * wpe_startcmd_resolve_folder - The folder as the store names it.
 * @folder: the folder that holds the xwperc or the project file;
 * @out/@sz: receive its resolved path.
 *
 * Links are followed, so a second path to the same clone is the same
 * folder and does not dodge the question.
 * Return: 0, or -1 when the folder cannot be followed.
 */
int wpe_startcmd_resolve_folder(const char *folder, char *out, size_t sz)
{
 char real[PATH_MAX];
 if (!folder || !*folder || sz == 0) return -1;
 if (!realpath(folder, real)) return -1;
 if (strlen(real) >= sz) return -1;
 memcpy(out, real, strlen(real) + 1);
 return 0;
}

/**
 * startcmd_state_dir - The folder that holds the store.
 * @out/@sz: receive it.
 * Return: 0, or -1 when neither $XDG_STATE_HOME nor $HOME is set.
 */
static int startcmd_state_dir(char *out, size_t sz)
{
 const char *xdg = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
 int n;
 if (xdg && *xdg)
  n = snprintf(out, sz, "%s/%s", xdg, WPE_STARTCMD_STATE_SUBDIR);
 else if (home && *home)
  n = snprintf(out, sz, "%s/%s/%s", home, WPE_STARTCMD_STATE_DEFAULT, WPE_STARTCMD_STATE_SUBDIR);
 else
  return -1;
 if (n < 0 || (size_t)n >= sz) return -1;
 return 0;
}

/**
 * startcmd_path_of - The path of one of the stores.
 * @name: the store's file name; @out/@sz: receive it.
 * Return: 0, or -1 when there is nowhere to keep it.
 */
static int startcmd_path_of(const char *name, char *out, size_t sz)
{
 char dir[WPE_STARTCMD_FOLDER_MAX];
 int n;
 if (startcmd_state_dir(dir, sizeof dir) < 0) return -1;
 n = snprintf(out, sz, "%s/%s", dir, name);
 if (n < 0 || (size_t)n >= sz) return -1;
 return 0;
}

/**
 * wpe_startcmd_store_path - Where the remembered answers are kept.
 * @out/@sz: receive $XDG_STATE_HOME/xwpe/start-commands, or the same below
 * ~/.local/state.
 * Return: 0, or -1 when there is nowhere to keep them.
 */
int wpe_startcmd_store_path(char *out, size_t sz)
{
 return startcmd_path_of(WPE_STARTCMD_STORE_NAME, out, sz);
}

/**
 * startcmd_mine_alone - Is a file or folder the user's, and not writable by
 * group or others?
 * @st: its stat.
 *
 * What a store and its folder must be before the editor believes a word of
 * it: anyone who could write there could answer for the user.
 */
static int startcmd_mine_alone(const struct stat *st)
{
 return st->st_uid == geteuid() && !(st->st_mode & (S_IWGRP | S_IWOTH));
}

/**
 * startcmd_dir_trusted - May the state folder be believed?
 * @dir: the folder.
 *
 * A folder of another user is refused.  A folder of the user's that group or
 * others may write is made the user's alone again, as every save already
 * does: older editors made it with the umask (0775 under umask 002), and
 * refusing it would lose every answer the user gave.  That is safe because
 * the folder only holds the stores, and each store is still believed only
 * when it is the user's alone (startcmd_stream_of): what a group member could
 * do there is remove or rename the user's own files, never write one.
 * Return: WPE_STARTCMD_STORE_OK, _MISSING (no folder yet), or _BAD (not a
 * folder, not the user's, or a mode that could not be tightened).
 */
static int startcmd_dir_trusted(const char *dir)
{
 struct stat st;
 int fd, ok;
 fd = open(dir, O_RDONLY | O_DIRECTORY);
 if (fd < 0) return errno == ENOENT ? WPE_STARTCMD_STORE_MISSING : WPE_STARTCMD_STORE_BAD;
 ok = fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid();
 if (ok && !startcmd_mine_alone(&st)) ok = fchmod(fd, STARTCMD_DIR_MODE) == 0;
 close(fd);
 return ok ? WPE_STARTCMD_STORE_OK : WPE_STARTCMD_STORE_BAD;
}

/** startcmd_store_free - Free every record of a store. */
static void startcmd_store_free(startcmd_store *s)
{
 int i;
 for (i = 0; i < s->n; i++) {
  free(s->rec[i].key);
  free(s->rec[i].folder);
  free(s->rec[i].command);
 }
 free(s->rec);
 s->rec = NULL;
 s->n = 0;
}

/**
 * startcmd_unescape - Turn a field of the store back into its text.
 * @field: the field, changed in place.
 * Return: 0, or -1 for a backslash the format does not know.
 */
static int startcmd_unescape(char *field)
{
 char *r = field, *w = field;
 while (*r) {
  if (*r != '\\') { *w++ = *r++; continue; }
  r++;
  if (*r == '\\') *w++ = '\\';
  else if (*r == 't') *w++ = '\t';
  else if (*r == 'n') *w++ = '\n';
  else if (*r == 'r') *w++ = '\r';
  else return -1;
  r++;
 }
 *w = '\0';
 return 0;
}

/**
 * startcmd_split_fields - Cut a record line at its tabs.
 * @line: the line, changed in place; @field: receives the four fields.
 * Return: 0 when the line has exactly four fields, else -1.
 */
static int startcmd_split_fields(char *line, char *field[STARTCMD_FIELDS])
{
 int i;
 char *p = line, *tab;
 for (i = 0; i < STARTCMD_FIELDS; i++) {
  field[i] = p;
  tab = strchr(p, '\t');
  if (i == STARTCMD_FIELDS - 1) return tab ? -1 : 0;
  if (!tab) return -1;
  *tab = '\0';
  p = tab + 1;
 }
 return -1;
}

/**
 * startcmd_store_add - Append a record, copying its texts.
 * Return: 0, or -1 out of memory.
 */
static int startcmd_store_add(startcmd_store *s, int answer, const char *key,
                              const char *folder, const char *command)
{
 startcmd_record *grown = realloc(s->rec, (size_t)(s->n + 1) * sizeof *grown);
 startcmd_record *r;
 if (!grown) return -1;
 s->rec = grown;
 r = &s->rec[s->n];
 r->answer = answer;
 r->key = startcmd_dup(key);
 r->folder = startcmd_dup(folder);
 r->command = startcmd_dup(command);
 if (!r->key || !r->folder || !r->command) {
  free(r->key);
  free(r->folder);
  free(r->command);
  return -1;
 }
 s->n++;
 return 0;
}

/**
 * startcmd_parse_record - Take one record line of the store.
 * @s: the store; @line: the line without its newline, changed in place.
 * Return: 0, or -1 for a line that does not parse.
 */
static int startcmd_parse_record(startcmd_store *s, char *line)
{
 char *field[STARTCMD_FIELDS];
 int answer, i;
 if (startcmd_split_fields(line, field) < 0) return -1;
 if (strcmp(field[0], STARTCMD_WORD_YES) == 0) answer = WPE_STARTCMD_YES;
 else if (strcmp(field[0], STARTCMD_WORD_NO) == 0) answer = WPE_STARTCMD_NO;
 else return -1;
 for (i = 1; i < STARTCMD_FIELDS; i++)
  if (startcmd_unescape(field[i]) < 0 || !field[i][0]) return -1;
 return startcmd_store_add(s, answer, field[1], field[2], field[3]);
}

/** startcmd_chop - Cut the newline that ends a line read with getline. */
static void startcmd_chop(char *line)
{
 size_t n = strlen(line);
 if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
}

/**
 * startcmd_parse_stream - Read the store's lines.
 * @fp: the open store; @s: receives its records.
 *
 * A version-1 store holds answers about single commands, which version 2
 * no longer asks about: it is read as an empty store, so each folder asks
 * once more and the next answer writes the store anew as version 2.  Any
 * other first line is a store this version does not know, and fails closed.
 * Return: WPE_STARTCMD_STORE_OK, or WPE_STARTCMD_STORE_BAD when the first
 * line is neither version's or any line does not parse.
 */
static int startcmd_parse_stream(FILE *fp, startcmd_store *s)
{
 char *line = NULL;
 size_t room = 0;
 int first = 1, state = WPE_STARTCMD_STORE_OK;
 while (state == WPE_STARTCMD_STORE_OK && getline(&line, &room, fp) >= 0) {
  startcmd_chop(line);
  if (first) {
   first = 0;
   if (strcmp(line, WPE_STARTCMD_STORE_MAGIC_V1) == 0) break;   /* read as empty */
   if (strcmp(line, WPE_STARTCMD_STORE_MAGIC) != 0) state = WPE_STARTCMD_STORE_BAD;
   continue;
  }
  if (!line[0]) continue;
  if (startcmd_parse_record(s, line) < 0) state = WPE_STARTCMD_STORE_BAD;
 }
 if (first || ferror(fp)) state = WPE_STARTCMD_STORE_BAD;
 free(line);
 return state;
}

/**
 * startcmd_stream_of - The stream of a regular file already open as @fd.
 * @fd: the descriptor (taken: closed on failure); @mode: the stream's mode.
 *
 * The stream works through the very descriptor that was opened without
 * following a link or made by mkstemp, never through the name again, and
 * anything but a regular file (a FIFO, a device planted at the store's
 * name) is refused before a byte is read or written.
 * Return: the stream, or NULL.
 */
static FILE *startcmd_stream_of(int fd, const char *mode)
{
 struct stat st;
 int flags;
 FILE *fp;
 if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || !startcmd_mine_alone(&st)) {
  close(fd);                      /* not a regular file of the user's alone */
  return NULL;
 }
 flags = fcntl(fd, F_GETFL);
 if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
 fp = fdopen(fd, mode);
 if (!fp) close(fd);
 return fp;
}

/**
 * startcmd_open_store - Open one of the stores for reading, if it can be
 * believed.
 * @name: the store's file name; @out: receives the stream.
 *
 * The folder must be the user's alone, and the file is opened without
 * following a link and checked through its descriptor (a regular file, the
 * user's, not writable by group or others), so a store another user could
 * have written is a store that cannot be read.
 * Return: WPE_STARTCMD_STORE_OK, _MISSING or _BAD.
 */
static int startcmd_open_store(const char *name, FILE **out)
{
 char dir[WPE_STARTCMD_FOLDER_MAX], path[WPE_STARTCMD_FOLDER_MAX];
 int fd, state;
 *out = NULL;
 if (startcmd_state_dir(dir, sizeof dir) < 0 || startcmd_path_of(name, path, sizeof path) < 0)
  return WPE_STARTCMD_STORE_BAD;
 state = startcmd_dir_trusted(dir);
 if (state != WPE_STARTCMD_STORE_OK) return state;
 fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
 if (fd < 0) return errno == ENOENT ? WPE_STARTCMD_STORE_MISSING : WPE_STARTCMD_STORE_BAD;
 *out = startcmd_stream_of(fd, "r");
 return *out ? WPE_STARTCMD_STORE_OK : WPE_STARTCMD_STORE_BAD;
}

/**
 * startcmd_store_load - Read the remembered answers.
 * @s: receives them (empty unless the result is WPE_STARTCMD_STORE_OK).
 *
 * The file is opened without following a link and without waiting, so a
 * link planted where the store should be is a store that cannot be read,
 * never someone else's file, and a FIFO there cannot hold the editor.
 * Return: WPE_STARTCMD_STORE_OK, _MISSING or _BAD.
 */
static int startcmd_store_load(startcmd_store *s)
{
 FILE *fp;
 int state;
 s->rec = NULL;
 s->n = 0;
 state = startcmd_open_store(WPE_STARTCMD_STORE_NAME, &fp);
 if (state != WPE_STARTCMD_STORE_OK) return state;
 state = startcmd_parse_stream(fp, s);
 fclose(fp);
 if (state != WPE_STARTCMD_STORE_OK) startcmd_store_free(s);
 return state;
}

/**
 * startcmd_store_find - The record for a folder, key and text.
 * Return: its index, or -1.
 */
static int startcmd_store_find(const startcmd_store *s, const char *folder, const char *key,
                               const char *text)
{
 int i;
 for (i = 0; i < s->n; i++)
  if (strcmp(s->rec[i].folder, folder) == 0 && strcmp(s->rec[i].key, key) == 0 &&
      strcmp(s->rec[i].command, text) == 0)
   return i;
 return -1;
}

/**
 * wpe_startcmd_check - May a folder's command run, or must the user be asked?
 * @folder: the folder that holds the file naming the command; @key: the
 * key; @text: the command as it would run.
 *
 * The user's own command, or the built-in default, runs.  Otherwise the
 * answer remembered for this folder, key and this very text decides; a
 * changed command, a missing store, or one that cannot be read asks.
 * Return: WPE_STARTCMD_RUN, WPE_STARTCMD_USE_OWN or WPE_STARTCMD_ASK.
 */
int wpe_startcmd_check(const char *folder, const char *key, const char *text)
{
 char real[WPE_STARTCMD_FOLDER_MAX];
 startcmd_store s;
 int i, verdict = WPE_STARTCMD_ASK;
 startcmd_store_state = WPE_STARTCMD_STORE_OK;
 if (!key || !text) return WPE_STARTCMD_ASK;
 if (wpe_startcmd_is_own(key, text)) return WPE_STARTCMD_RUN;
 if (wpe_startcmd_resolve_folder(folder, real, sizeof real) < 0) return WPE_STARTCMD_ASK;
 startcmd_store_state = startcmd_store_load(&s);
 if (startcmd_store_state != WPE_STARTCMD_STORE_OK) return WPE_STARTCMD_ASK;
 i = startcmd_store_find(&s, real, key, text);
 if (i >= 0) {
  if (s.rec[i].answer == WPE_STARTCMD_YES) verdict = WPE_STARTCMD_RUN;
  else verdict = WPE_STARTCMD_USE_OWN;
 }
 startcmd_store_free(&s);
 return verdict;
}

/**
 * wpe_startcmd_last_store_state - How the last lookup found the store.
 * Return: WPE_STARTCMD_STORE_OK, _MISSING or _BAD; the editor says once
 * that it could not read the remembered commands when it is _BAD.
 */
int wpe_startcmd_last_store_state(void)
{
 return startcmd_store_state;
}

/**
 * startcmd_put_escaped - Write a text as one field of the store.
 * @fp: the file; @text: the text.
 */
static void startcmd_put_escaped(FILE *fp, const char *text)
{
 for (; *text; text++) {
  if (*text == '\\') fputs("\\\\", fp);
  else if (*text == '\t') fputs("\\t", fp);
  else if (*text == '\n') fputs("\\n", fp);
  else if (*text == '\r') fputs("\\r", fp);
  else fputc(*text, fp);
 }
}

/**
 * startcmd_write_records - Write the store's text to an open file.
 * Return: 0, or -1 when a write failed.
 */
static int startcmd_write_records(FILE *fp, const startcmd_store *s)
{
 int i;
 fprintf(fp, "%s\n", WPE_STARTCMD_STORE_MAGIC);
 for (i = 0; i < s->n; i++) {
  if (s->rec[i].answer == WPE_STARTCMD_YES) fputs(STARTCMD_WORD_YES, fp);
  else fputs(STARTCMD_WORD_NO, fp);
  fputc('\t', fp);
  startcmd_put_escaped(fp, s->rec[i].key);
  fputc('\t', fp);
  startcmd_put_escaped(fp, s->rec[i].folder);
  fputc('\t', fp);
  startcmd_put_escaped(fp, s->rec[i].command);
  fputc('\n', fp);
 }
 if (fflush(fp) != 0 || ferror(fp)) return -1;
 return 0;
}

/**
 * startcmd_make_dirs - Make the state folder, and the folders above it below
 * the base, each the user's alone.
 * @dir: the state folder.
 * Return: 0 when it exists as a folder afterwards, else -1.
 */
static int startcmd_make_dirs(const char *dir)
{
 char path[WPE_STARTCMD_FOLDER_MAX];
 struct stat st;
 size_t i, n = strlen(dir);
 if (n == 0 || n >= sizeof path) return -1;
 memcpy(path, dir, n + 1);
 for (i = 1; i <= n; i++) {
  if (path[i] != '/' && path[i] != '\0') continue;
  path[i] = '\0';
  if (mkdir(path, STARTCMD_DIR_MODE) != 0 && errno != EEXIST) return -1;
  if (i < n) path[i] = '/';
 }
 if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
 if ((st.st_mode & 0777) != STARTCMD_DIR_MODE && chmod(dir, STARTCMD_DIR_MODE) != 0)
  return -1;                           /* an existing one is made the user's alone */
 return 0;
}

/**
 * startcmd_store_save - Put the records in place of the store.
 * @s: the records.
 *
 * They go to a new file beside the store, made without following any link
 * and readable by the user alone, which then takes the store's name in one
 * step: a reader never sees half a store, and a link planted at the store's
 * name is replaced, never followed.
 * Return: 0, or -1 when it could not be written.
 */
static int startcmd_save_file(const char *name, int (*write_fn)(FILE *, const void *),
                              const void *ctx)
{
 char dir[WPE_STARTCMD_FOLDER_MAX], path[WPE_STARTCMD_FOLDER_MAX];
 char temp[WPE_STARTCMD_FOLDER_MAX + sizeof STARTCMD_TEMP_SUFFIX];
 FILE *fp;
 int fd, rc;
 if (startcmd_state_dir(dir, sizeof dir) < 0 || startcmd_path_of(name, path, sizeof path) < 0)
  return -1;
 if (startcmd_make_dirs(dir) < 0) return -1;
 snprintf(temp, sizeof temp, "%s%s", path, STARTCMD_TEMP_SUFFIX);
 fd = mkstemp(temp);
 if (fd < 0) return -1;
 fchmod(fd, STARTCMD_FILE_MODE);
 fp = startcmd_stream_of(fd, "w");
 if (!fp) { unlink(temp); return -1; }
 rc = write_fn(fp, ctx);
 if (fclose(fp) != 0) rc = -1;
 if (rc == 0 && rename(temp, path) != 0) rc = -1;
 if (rc != 0) unlink(temp);
 return rc;
}

/** startcmd_write_answers - The writer of the answers' store, for startcmd_save_file. */
static int startcmd_write_answers(FILE *fp, const void *ctx)
{
 return startcmd_write_records(fp, (const startcmd_store *)ctx);
}

/** startcmd_store_save - Put the answers in place of the store; see startcmd_save_file. */
static int startcmd_store_save(const startcmd_store *s)
{
 return startcmd_save_file(WPE_STARTCMD_STORE_NAME, startcmd_write_answers, s);
}

/** startcmd_store_drop - Take one record out of the store. */
static void startcmd_store_drop(startcmd_store *s, int i)
{
 free(s->rec[i].key);
 free(s->rec[i].folder);
 free(s->rec[i].command);
 memmove(&s->rec[i], &s->rec[i + 1], (size_t)(s->n - i - 1) * sizeof s->rec[0]);
 s->n--;
}

/**
 * startcmd_folder_oldest - The oldest record of a folder, when it has too many.
 * Return: its index, or -1 while the folder has at most
 * WPE_STARTCMD_MAX_PER_FOLDER records.
 */
static int startcmd_folder_oldest(const startcmd_store *s, const char *folder)
{
 int i, n = 0, first = -1;
 for (i = 0; i < s->n; i++) {
  if (strcmp(s->rec[i].folder, folder) != 0) continue;
  if (first < 0) first = i;
  n++;
 }
 if (n > WPE_STARTCMD_MAX_PER_FOLDER) return first;
 return -1;
}

/**
 * startcmd_store_put - Set the answer for a folder, key and text, newest last.
 * @s: the store; the rest as wpe_startcmd_remember.
 *
 * One record per folder, key and text: an answer for the same text replaces
 * the old one, and another text of the same key is a record of its own, so
 * two plans answered for in one folder (two programs debugged, files in two
 * subfolders) both stay answered and never push each other out.  Past
 * WPE_STARTCMD_MAX_PER_FOLDER records in one folder, or
 * WPE_STARTCMD_MAX_RECORDS in all, the oldest answers are dropped.
 * Return: 0, or -1 out of memory.
 */
static int startcmd_store_put(startcmd_store *s, const char *folder, const char *key,
                              const char *text, int answer)
{
 int i = startcmd_store_find(s, folder, key, text);
 if (i >= 0) startcmd_store_drop(s, i);
 if (startcmd_store_add(s, answer, key, folder, text) < 0) return -1;
 while ((i = startcmd_folder_oldest(s, folder)) >= 0) startcmd_store_drop(s, i);
 while (s->n > WPE_STARTCMD_MAX_RECORDS) startcmd_store_drop(s, 0);
 return 0;
}

/**
 * wpe_startcmd_remember - Keep the user's answer for a folder, key and command.
 * @folder: the folder that holds the file naming the command; @key: the
 * key; @text: the command as it was shown; @answer: WPE_STARTCMD_YES (Run
 * it) or WPE_STARTCMD_NO (Use mine).
 *
 * The next start in the same folder with the same command does not ask.
 * The store is read, changed and written back whole, so a second editor's
 * answers are kept.  A store that cannot be read is left as it is, for the
 * user to look at: nothing is remembered and the next run asks again.
 * Return: 0, or -1 when the answer could not be kept.
 */
int wpe_startcmd_remember(const char *folder, const char *key, const char *text, int answer)
{
 char real[WPE_STARTCMD_FOLDER_MAX];
 startcmd_store s;
 int state, rc;
 if (!key || !*key || !text || !*text) return -1;
 if (answer != WPE_STARTCMD_YES && answer != WPE_STARTCMD_NO) return -1;
 if (wpe_startcmd_resolve_folder(folder, real, sizeof real) < 0) return -1;
 state = startcmd_store_load(&s);
 if (state == WPE_STARTCMD_STORE_BAD) return -1;
 rc = startcmd_store_put(&s, real, key, text, answer);
 if (rc == 0) rc = startcmd_store_save(&s);
 startcmd_store_free(&s);
 return rc;
}

/* ---- What this editor built: the built-outputs store ---------------------- */

/* A record: folder, output, dev, ino, size, mtime s and ns, ctime s and ns. */
#define BUILT_FIELDS 9

#ifdef __APPLE__
#define BUILT_MTIM(st) ((st)->st_mtimespec)
#define BUILT_CTIM(st) ((st)->st_ctimespec)
#else
#define BUILT_MTIM(st) ((st)->st_mtim)
#define BUILT_CTIM(st) ((st)->st_ctim)
#endif

/** built_rec_free - Free the texts of one record. */
static void built_rec_free(WpeBuiltRec *r)
{
 free(r->folder);
 free(r->output);
 r->folder = NULL;
 r->output = NULL;
}

/** built_recs_free - Free a list of records. */
static void built_recs_free(WpeBuiltRec *rec, int n)
{
 int i;
 for (i = 0; i < n; i++) built_rec_free(&rec[i]);
 free(rec);
}

/** built_rec_identify - Take the identity of a file into a record. */
static void built_rec_identify(WpeBuiltRec *r, const struct stat *st)
{
 r->dev = (unsigned long long)st->st_dev;
 r->ino = (unsigned long long)st->st_ino;
 r->size = (unsigned long long)st->st_size;
 r->mtime_s = (long long)BUILT_MTIM(st).tv_sec;
 r->mtime_ns = (long long)BUILT_MTIM(st).tv_nsec;
 r->ctime_s = (long long)BUILT_CTIM(st).tv_sec;
 r->ctime_ns = (long long)BUILT_CTIM(st).tv_nsec;
}

/**
 * built_rec_is - Is this the very file the record describes?
 * @r: the record; @st: the file's stat.
 *
 * The change time is part of it: the kernel sets it on every create, rename,
 * chmod and utime, and nobody can set it back, so a file put in place of the
 * recorded one, or touched since, never matches.
 */
static int built_rec_is(const WpeBuiltRec *r, const struct stat *st)
{
 WpeBuiltRec now;
 built_rec_identify(&now, st);
 return r->dev == now.dev && r->ino == now.ino && r->size == now.size &&
        r->mtime_s == now.mtime_s && r->mtime_ns == now.mtime_ns &&
        r->ctime_s == now.ctime_s && r->ctime_ns == now.ctime_ns;
}

/**
 * wpe_startcmd_same_file - Is a file, as it is now, the file it was before?
 * @before: the stat taken before a step; @now: the stat taken after it.
 *
 * Device, inode, size and the modification and change times in nanoseconds
 * must all be equal.  A step is said to have written an output only when
 * this is 0, so a file that was there before the step and that the step left
 * alone is never taken for the step's work.
 * Return: 1 when nothing about the file changed, else 0.
 */
int wpe_startcmd_same_file(const struct stat *before, const struct stat *now)
{
 WpeBuiltRec a;
 memset(&a, 0, sizeof a);
 built_rec_identify(&a, before);
 return built_rec_is(&a, now);
}

/**
 * built_recs_add - Append a record, copying its texts.
 * @rec/@n: the list; @folder/@output: the texts; @id: a record that carries
 * the identity.
 * Return: 0, or -1 out of memory.
 */
static int built_recs_add(WpeBuiltRec **rec, int *n, const char *folder, const char *output,
                          const WpeBuiltRec *id)
{
 WpeBuiltRec *grown = realloc(*rec, (size_t)(*n + 1) * sizeof **rec);
 WpeBuiltRec *r;
 if (!grown) return -1;
 *rec = grown;
 r = &grown[*n];
 *r = *id;
 r->folder = startcmd_dup(folder);
 r->output = startcmd_dup(output);
 if (!r->folder || !r->output) {
  built_rec_free(r);
  return -1;
 }
 (*n)++;
 return 0;
}

/** built_recs_drop - Take one record out of a list. */
static void built_recs_drop(WpeBuiltRec *rec, int *n, int i)
{
 built_rec_free(&rec[i]);
 memmove(&rec[i], &rec[i + 1], (size_t)(*n - i - 1) * sizeof rec[0]);
 (*n)--;
}

/**
 * built_number - Read an unsigned decimal field whole.
 * Return: 0, or -1 for text that is not one.
 */
static int built_number(const char *text, unsigned long long *out)
{
 char *end;
 if (!*text || *text == '-') return -1;
 errno = 0;
 *out = strtoull(text, &end, 10);
 return (*end || errno) ? -1 : 0;
}

/** built_signed - Read a signed decimal field whole; 0, or -1. */
static int built_signed(const char *text, long long *out)
{
 char *end;
 if (!*text) return -1;
 errno = 0;
 *out = strtoll(text, &end, 10);
 return (*end || errno) ? -1 : 0;
}

/**
 * built_split - Cut a record line at its tabs.
 * @line: changed in place; @field: receives BUILT_FIELDS fields.
 * Return: 0 for exactly that many, else -1.
 */
static int built_split(char *line, char *field[BUILT_FIELDS])
{
 int i;
 char *p = line, *tab;
 for (i = 0; i < BUILT_FIELDS; i++) {
  field[i] = p;
  tab = strchr(p, '\t');
  if (i == BUILT_FIELDS - 1) return tab ? -1 : 0;
  if (!tab) return -1;
  *tab = '\0';
  p = tab + 1;
 }
 return -1;
}

/**
 * built_parse_record - Take one record line of the store.
 * @line: without its newline, changed in place; @only: keep only the
 * records of this folder, or NULL for all; @rec/@n: the list.
 * Return: 0, or -1 for a line that does not parse.
 */
static int built_parse_record(char *line, const char *only, WpeBuiltRec **rec, int *n)
{
 char *f[BUILT_FIELDS];
 WpeBuiltRec id;
 int i;
 memset(&id, 0, sizeof id);
 if (built_split(line, f) < 0) return -1;
 for (i = 0; i < 2; i++)
  if (startcmd_unescape(f[i]) < 0 || !f[i][0]) return -1;
 if (built_number(f[2], &id.dev) < 0 || built_number(f[3], &id.ino) < 0 ||
     built_number(f[4], &id.size) < 0 || built_signed(f[5], &id.mtime_s) < 0 ||
     built_signed(f[6], &id.mtime_ns) < 0 || built_signed(f[7], &id.ctime_s) < 0 ||
     built_signed(f[8], &id.ctime_ns) < 0)
  return -1;
 if (only && strcmp(f[0], only) != 0) return 0;
 return built_recs_add(rec, n, f[0], f[1], &id);
}

/**
 * built_read_stream - Read the store's lines.
 * @fp: the open store; @only: the folder to keep, or NULL; @rec/@n: receive.
 * Return: WPE_STARTCMD_STORE_OK, or _BAD for another first line or a bad line.
 */
static int built_read_stream(FILE *fp, const char *only, WpeBuiltRec **rec, int *n)
{
 char *line = NULL;
 size_t room = 0;
 int first = 1, state = WPE_STARTCMD_STORE_OK;
 while (state == WPE_STARTCMD_STORE_OK && getline(&line, &room, fp) >= 0) {
  startcmd_chop(line);
  if (first) {
   first = 0;
   if (strcmp(line, WPE_BUILT_STORE_MAGIC) != 0) state = WPE_STARTCMD_STORE_BAD;
   continue;
  }
  if (!line[0]) continue;
  if (built_parse_record(line, only, rec, n) < 0) state = WPE_STARTCMD_STORE_BAD;
 }
 if (first || ferror(fp)) state = WPE_STARTCMD_STORE_BAD;
 free(line);
 return state;
}

/**
 * built_read_store - Read the built-outputs store.
 * @only: the folder whose records to keep, or NULL for all; @rec/@n: receive
 * them (empty unless the result is OK).
 * Return: WPE_STARTCMD_STORE_OK, _MISSING or _BAD.
 */
static int built_read_store(const char *only, WpeBuiltRec **rec, int *n)
{
 FILE *fp;
 int state;
 char dir[WPE_STARTCMD_FOLDER_MAX];
 *rec = NULL;
 *n = 0;
 /* No state folder to name (none set, or one too long): there is nothing
    to read, and the first save says it could not write. */
 if (startcmd_state_dir(dir, sizeof dir) < 0) return WPE_STARTCMD_STORE_MISSING;
 state = startcmd_open_store(WPE_BUILT_STORE_NAME, &fp);
 if (state != WPE_STARTCMD_STORE_OK) return state;
 state = built_read_stream(fp, only, rec, n);
 fclose(fp);
 if (state != WPE_STARTCMD_STORE_OK) {
  built_recs_free(*rec, *n);
  *rec = NULL;
  *n = 0;
 }
 return state;
}

/**
 * wpe_startcmd_built_load - The records of one build folder.
 * @folder: the build folder, as the plan runs in it; @set: receives them.
 *
 * Read once at the plan of a build and held in memory, so the cost per output
 * is one lookup, never a parse of the store.  Only this folder's records are
 * kept.
 * Return: the store's state; the set is usable (empty) for _OK and _MISSING,
 * and refuses everything for _BAD.
 */
int wpe_startcmd_built_load(const char *folder, WpeBuiltSet *set)
{
 memset(set, 0, sizeof *set);
 set->state = WPE_STARTCMD_STORE_BAD;
 if (wpe_startcmd_resolve_folder(folder, set->folder, sizeof set->folder) < 0) {
  set->folder[0] = '\0';
  return set->state;
 }
 set->state = built_read_store(set->folder, &set->rec, &set->n);
 return set->state;
}

/** wpe_startcmd_built_free - Free a set's records. */
void wpe_startcmd_built_free(WpeBuiltSet *set)
{
 built_recs_free(set->rec, set->n);
 memset(set, 0, sizeof *set);
 set->state = WPE_STARTCMD_STORE_BAD;
}

/**
 * built_real_path - The real path of an output of a set's folder.
 * @set: the set; @output: absolute, or relative to the set's folder; @out:
 * receives the real path (PATH_MAX).
 *
 * Never relative to the process's folder: the record and the lookup name the
 * same file whatever the editor's working folder is.
 * Return: 0, or -1 when it cannot be resolved (it does not exist).
 */
static int built_real_path(const WpeBuiltSet *set, const char *output, char *out)
{
 char joined[WPE_STARTCMD_FOLDER_MAX];
 int n;
 if (!output || !*output || !set->folder[0]) return -1;
 if (output[0] == '/')
  n = snprintf(joined, sizeof joined, "%s", output);
 else
  n = snprintf(joined, sizeof joined, "%s/%s", set->folder, output);
 if (n < 0 || (size_t)n >= sizeof joined) return -1;
 return realpath(joined, out) ? 0 : -1;
}

/** built_set_find - The index of the record of a real path in a set, or -1. */
static int built_set_find(const WpeBuiltSet *set, const char *real)
{
 int i;
 for (i = 0; i < set->n; i++)
  if (strcmp(set->rec[i].output, real) == 0) return i;
 return -1;
}

/**
 * wpe_startcmd_built_here - Did this editor build this very file here?
 * @set: the folder's records; @output: the output; @st: the caller's stat of
 * it, or NULL.
 * Return: 1 for a record of this real path whose identity is the file's;
 * else 0, a refused store included.
 */
int wpe_startcmd_built_here(const WpeBuiltSet *set, const char *output, const struct stat *st)
{
 char real[WPE_STARTCMD_FOLDER_MAX];
 struct stat own;
 int i;
 if (set->state == WPE_STARTCMD_STORE_BAD) return 0;
 if (built_real_path(set, output, real) < 0) return 0;
 if (!st) {
  if (stat(real, &own) != 0) return 0;
  st = &own;
 }
 i = built_set_find(set, real);
 return i >= 0 && built_rec_is(&set->rec[i], st);
}

/**
 * wpe_startcmd_note_built - This editor just built @output: it is now its own.
 * @set: the folder's records; @output: the output; @st: the stat the caller
 * made of it after the step.
 *
 * Kept in memory, one record per real path, replaced in place; the store is
 * written by wpe_startcmd_built_save, once per step.
 * Return: 0, or -1 for a refused store, an output that cannot be resolved
 * or out of memory.
 */
int wpe_startcmd_note_built(WpeBuiltSet *set, const char *output, const struct stat *st)
{
 char real[WPE_STARTCMD_FOLDER_MAX];
 WpeBuiltRec id;
 int i;
 if (!st || set->state == WPE_STARTCMD_STORE_BAD) return -1;
 if (built_real_path(set, output, real) < 0) return -1;
 memset(&id, 0, sizeof id);
 built_rec_identify(&id, st);
 i = built_set_find(set, real);
 if (i >= 0) built_recs_drop(set->rec, &set->n, i);
 if (built_recs_add(&set->rec, &set->n, set->folder, real, &id) < 0) return -1;
 set->dirty = 1;
 return 0;
}

/**
 * built_prune - Drop the records whose file is gone or is not the one built.
 * @rec/@n: the list.
 */
static void built_prune(WpeBuiltRec *rec, int *n)
{
 struct stat st;
 int i = 0;
 while (i < *n) {
  if (stat(rec[i].output, &st) != 0 || !built_rec_is(&rec[i], &st))
   built_recs_drop(rec, n, i);
  else
   i++;
 }
}

/* The records a save writes. */
typedef struct {
 WpeBuiltRec *rec;
 int n;
} built_list;

/** built_write_list - Write the store's text for a list of records. */
static int built_write_list(FILE *fp, const void *ctx)
{
 const built_list *l = ctx;
 int i;
 fprintf(fp, "%s\n", WPE_BUILT_STORE_MAGIC);
 for (i = 0; i < l->n; i++) {
  const WpeBuiltRec *r = &l->rec[i];
  startcmd_put_escaped(fp, r->folder);
  fputc('\t', fp);
  startcmd_put_escaped(fp, r->output);
  fprintf(fp, "\t%llu\t%llu\t%llu\t%lld\t%lld\t%lld\t%lld\n", r->dev, r->ino, r->size,
          r->mtime_s, r->mtime_ns, r->ctime_s, r->ctime_ns);
 }
 if (fflush(fp) != 0 || ferror(fp)) return -1;
 return 0;
}

/**
 * built_merge - Replace a folder's records in the whole store's list.
 * @all/@nall: the store's records; @set: the folder's records.
 * Return: 0, or -1 out of memory.
 */
static int built_merge(WpeBuiltRec **all, int *nall, const WpeBuiltSet *set)
{
 int i = 0;
 while (i < *nall) {
  if (strcmp((*all)[i].folder, set->folder) == 0) built_recs_drop(*all, nall, i);
  else i++;
 }
 for (i = 0; i < set->n; i++)
  if (built_recs_add(all, nall, set->rec[i].folder, set->rec[i].output, &set->rec[i]) < 0)
   return -1;
 return 0;
}

/**
 * wpe_startcmd_built_save - Write what a step noted.
 * @set: the folder's records.
 *
 * The store is read again, so another folder's records, and another
 * editor's, are kept; this folder's are replaced; records whose file is gone
 * or changed are dropped; the oldest go past WPE_BUILT_MAX_RECORDS.  Two
 * editors writing at once can lose each other's records, which only means one
 * more build.
 * Return: 0 (also when nothing was noted), or -1 when it could not be written.
 */
int wpe_startcmd_built_save(WpeBuiltSet *set)
{
 WpeBuiltRec *all = NULL;
 int nall = 0, state, rc;
 built_list l;
 if (!set->dirty) return 0;
 if (set->state == WPE_STARTCMD_STORE_BAD || !set->folder[0]) return -1;
 state = built_read_store(NULL, &all, &nall);
 if (state == WPE_STARTCMD_STORE_BAD) return -1;
 if (built_merge(&all, &nall, set) < 0) { built_recs_free(all, nall); return -1; }
 built_prune(all, &nall);
 while (nall > WPE_BUILT_MAX_RECORDS) built_recs_drop(all, &nall, 0);
 l.rec = all;
 l.n = nall;
 rc = startcmd_save_file(WPE_BUILT_STORE_NAME, built_write_list, &l);
 built_recs_free(all, nall);
 if (rc == 0) {
  set->dirty = 0;
  set->state = WPE_STARTCMD_STORE_OK;
 }
 return rc;
}

/* How a byte the box cannot show is written: \x and two hex digits. */
#define STARTCMD_HEX_FMT "\\x%02x"
#define STARTCMD_HEX_LEN 4

/* The first code point that is neither a control nor a C1 control. */
#define STARTCMD_FIRST_SHOWN_WIDE 0xa0

/* How a format character is written out: \u and four hex digits. */
#define STARTCMD_FORMAT_FMT "\\u%04lx"
#define STARTCMD_FORMAT_LEN 6

/**
 * startcmd_is_format_char - Is a code point one that reorders or hides text?
 * @cp: the code point.
 *
 * The bidirectional embeddings, overrides and isolates make a command read
 * in another order than it runs; the zero-width characters and the byte
 * order mark hide where one word ends.  Return: 1 for such a character.
 */
static int startcmd_is_format_char(unsigned long cp)
{
 if (cp >= 0x200b && cp <= 0x200f) return 1;   /* zero width, LRM, RLM   */
 if (cp >= 0x2028 && cp <= 0x202e) return 1;   /* separators, embeddings */
 if (cp >= 0x2060 && cp <= 0x2069) return 1;   /* joiner, isolates       */
 if (cp == 0x061c || cp == 0xfeff) return 1;   /* Arabic mark, BOM       */
 return 0;
}

/**
 * startcmd_utf8_decode - The code point of a well-formed UTF-8 sequence
 * that shows a character.
 * @s: the bytes; @out: receives the code point.
 * Return: the sequence's length (2 to 4), or 0 when the bytes are not one
 * or encode a C1 control (which a terminal would act on).
 */
static int startcmd_utf8_decode(const unsigned char *s, unsigned long *out)
{
 unsigned long cp;
 int len, i;
 if (s[0] >= 0xc2 && s[0] <= 0xdf) { len = 2; cp = s[0] & 0x1f; }
 else if (s[0] >= 0xe0 && s[0] <= 0xef) { len = 3; cp = s[0] & 0x0f; }
 else if (s[0] >= 0xf0 && s[0] <= 0xf4) { len = 4; cp = s[0] & 0x07; }
 else return 0;
 for (i = 1; i < len; i++) {
  if ((s[i] & 0xc0) != 0x80) return 0;
  cp = (cp << 6) | (s[i] & 0x3f);
 }
 if (cp < STARTCMD_FIRST_SHOWN_WIDE) return 0;
 if (len == 3 && cp < 0x800) return 0;
 if (len == 4 && (cp < 0x10000 || cp > 0x10ffff)) return 0;
 if (cp >= 0xd800 && cp <= 0xdfff) return 0;
 *out = cp;
 return len;
}

/* Room for the longest written-out form of one character. */
#define STARTCMD_PIECE_MAX (STARTCMD_FORMAT_LEN + 1)

static const char *startcmd_escape_of(unsigned char c);

/**
 * startcmd_piece - How one character of a command is shown.
 * @s: the command at that character; @buf: room for a written-out form;
 * @piece/@plen: receive what is shown.
 * Return: how many bytes of @s it takes.
 */
static int startcmd_piece(const unsigned char *s, char buf[STARTCMD_PIECE_MAX],
                          const char **piece, size_t *plen)
{
 unsigned long cp;
 int len;
 if ((*piece = startcmd_escape_of(*s)) != NULL) { *plen = 2; return 1; }
 if (*s >= 0x20 && *s < 0x7f) { *piece = (const char *)s; *plen = 1; return 1; }
 len = (*s >= 0x80) ? startcmd_utf8_decode(s, &cp) : 0;
 if (len > 0 && startcmd_is_format_char(cp)) {
  snprintf(buf, STARTCMD_PIECE_MAX, STARTCMD_FORMAT_FMT, cp);
  *piece = buf;
  *plen = STARTCMD_FORMAT_LEN;
  return len;
 }
 if (len > 0) { *piece = (const char *)s; *plen = (size_t)len; return len; }
 snprintf(buf, STARTCMD_PIECE_MAX, STARTCMD_HEX_FMT, *s);
 *piece = buf;
 *plen = STARTCMD_HEX_LEN;
 return 1;
}

/**
 * startcmd_escape_of - The two-character form of a control the box names.
 * Return: "\\n", "\\t", "\\r", "\\\\", or NULL for another byte.
 */
static const char *startcmd_escape_of(unsigned char c)
{
 if (c == '\n') return "\\n";
 if (c == '\t') return "\\t";
 if (c == '\r') return "\\r";
 if (c == '\\') return "\\\\";
 return NULL;
}

/**
 * wpe_startcmd_printable - A command as the question shows it.
 * @text: the command; @out/@sz: receive the shown form.
 *
 * Every control byte, escape sequences included, is written out (\x1b,
 * \n, \t), a backslash is doubled, a byte that is not part of a printable
 * UTF-8 character is written as \xNN, and a character that reorders or
 * hides text (bidirectional controls, zero-width characters, the byte
 * order mark) as \uNNNN.  So the command the user reads is the command
 * that would run, in the order it runs, and it cannot clear the screen,
 * move the cursor or paint over the question.
 * Return: the length the whole form needs, which may exceed @sz - 1 (the
 * form is then cut, always on a whole escape).
 */
size_t wpe_startcmd_printable(const char *text, char *out, size_t sz)
{
 const unsigned char *s = (const unsigned char *)text;
 size_t used = 0;
 char buf[STARTCMD_PIECE_MAX];
 const char *piece;
 size_t plen;
 int len;
 if (sz) out[0] = '\0';
 while (*s) {
  len = startcmd_piece(s, buf, &piece, &plen);
  if (sz && used + plen < sz) {
   memcpy(out + used, piece, plen);
   out[used + plen] = '\0';
  } else if (sz) {
   sz = 0;                           /* cut here, on a whole piece */
  }
  used += plen;
  s += len;
 }
 return used;
}
