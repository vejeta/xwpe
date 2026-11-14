/* we_progname.c -- the one rule that names what a build makes from a source
   (see we_progname.h). */
#include "we_progname.h"

#include <string.h>

#include "we_path.h"
#include "we_startplan.h"

#define PROGNAME_DIR_SEP  '/'
#define PROGNAME_EXT_SEP  '.'

/**
 * progname_refuse - Leave @out empty: the result did not fit or had no source.
 * @out/@n: the buffer and its size.
 *
 * Every function of this file ends a refusal here, so a caller that reads
 * the result of nothing still never gets a prefix of a name.
 * Return: -1, always.
 */
static int progname_refuse(char *out, size_t n)
{
 if (out && n)
  out[0] = '\0';
 return -1;
}

/**
 * progname_name_of - The file name of a path: the text after its last '/'.
 * @path: a file name, a FILES word or a project path.
 *
 * Return: a pointer into @path.
 */
static const char *progname_name_of(const char *path)
{
 const char *slash = strrchr(path, PROGNAME_DIR_SEP);

 if (slash)
  return slash + 1;
 return path;
}

/**
 * progname_source_ok - Does @s name a source?
 * @s: the source as the build sees it.
 *
 * Return: 1 when it has a name to take the stem from.
 */
static int progname_source_ok(const WpeProgramSource *s)
{
 return s && s->name;
}

/**
 * progname_has - Is a language's Executable or an EXENAME set?
 * @word: the text as typed, or NULL.
 *
 * Return: 1 for a word that holds a character; NULL and "" are not set.
 */
static int progname_has(const char *word)
{
 return word && word[0] != '\0';
}

/**
 * progname_place - Put a name under Exedir, the way a command names a path.
 * @out/@n: receive the path; @exedir: Exedir, NULL or "" for the build
 * folder; @name: the name to place.
 *
 * The only join is the start plan's operand, so a placed path that starts
 * with '-' or '@' gets "./" and is never an option or a response file.
 * Return: 0, or -1 with @out empty when it does not fit.
 */
static int progname_place(char *out, size_t n, const char *exedir,
                          const char *name)
{
 if (wpe_startplan_operand(out, n, exedir, name))
  return progname_refuse(out, n);
 return 0;
}

/**
 * progname_stem_with - The stem of a source followed by a suffix, not placed.
 * @name/@size: receive "stem" + @suffix; @s: the source; @suffix: ".o", ".dbg"
 * and the like.
 *
 * Return: 0, or -1 with @name empty when it does not fit or @s has no name.
 */
static int progname_stem_with(char *name, size_t size, const WpeProgramSource *s,
                              const char *suffix)
{
 if (!progname_source_ok(s))
  return progname_refuse(name, size);
 if (wpe_progname_stem(name, size, s->name))
  return -1;
 return wpe_path_cat(name, size, suffix);
}

/**
 * progname_placed_stem_with - The stem of a source and a suffix, under Exedir.
 * @out/@n: receive the path; @s: the source; @suffix: what the output ends in.
 *
 * Return: 0, or -1 with @out empty.
 */
static int progname_placed_stem_with(char *out, size_t n,
                                     const WpeProgramSource *s,
                                     const char *suffix)
{
 char name[WPE_PATH_SIZE];

 if (progname_stem_with(name, sizeof name, s, suffix))
  return progname_refuse(out, n);
 return progname_place(out, n, s->exedir, name);
}

/**
 * wpe_progname_extension - The extension of a file name, with its dot.
 * @path: a file name or a path.
 *
 * "a.b.c" gives ".c", ".hidden.c" gives ".c", ".c" alone gives ".c": the last
 * dot of the name, wherever it is.  A dot in a folder never counts.  Lets the
 * compiler entry and the debugger's adapter be picked by the same reading of
 * a file as the program's name is made from.
 * Return: a pointer into @path, or to its terminator when the name has no dot;
 * "" for a NULL @path.
 */
const char *wpe_progname_extension(const char *path)
{
 const char *name, *dot;

 if (!path)
  return "";
 name = progname_name_of(path);
 dot = strrchr(name, PROGNAME_EXT_SEP);
 if (dot)
  return dot;
 return name + strlen(name);
}

/**
 * wpe_progname_stem - A file name without its folder and its extension.
 * @out/@n: receive the stem; @path: a file name, a FILES word or a project
 * path.
 *
 * "src/a.b.c" gives "a.b", so `a.b.c` makes `a.b.e` as it always has, and a
 * folder with a dot in it never changes a program's name.
 * Return: 0, or -1 with @out empty when the name does not fit.
 */
int wpe_progname_stem(char *out, size_t n, const char *path)
{
 const char *name, *ext;

 if (!out || !n || !path)
  return progname_refuse(out, n);
 name = progname_name_of(path);
 if (wpe_path_copy(out, n, name))
  return -1;
 ext = wpe_progname_extension(name);
 out[ext - name] = '\0';
 return 0;
}

/**
 * wpe_progname_program_name - What the build calls the program, not placed.
 * @out/@n: receive the name; @s: the source.
 *
 * The user's Executable as typed when set, else "a.e" for a source `a.c`
 * the editor links, or "a" for a compiler that writes the program itself.
 * The last is also the class a Java debugger starts.
 * Return: 0, or -1 with @out empty.
 */
int wpe_progname_program_name(char *out, size_t n, const WpeProgramSource *s)
{
 if (!progname_source_ok(s))
  return progname_refuse(out, n);
 if (progname_has(s->executable)) {
  if (wpe_path_copy(out, n, s->executable))
   return -1;
  return 0;
 }
 if (s->self_linked)
  return wpe_progname_stem(out, n, s->name);
 return progname_stem_with(out, n, s, WPE_PROGRAM_SUFFIX);
}

/**
 * wpe_progname_program - The program as a command names it.
 * @out/@n: receive the path; @s: the source.
 *
 * F9's link, Run and the debugger name the program through this one call,
 * so they start the file the link wrote, under Exedir when it is set.  A
 * compiler that writes the program itself leaves it where it ran, so it is
 * not placed.
 * Return: 0, or -1 with @out empty when it does not fit.
 */
int wpe_progname_program(char *out, size_t n, const WpeProgramSource *s)
{
 char name[WPE_PATH_SIZE];

 if (wpe_progname_program_name(name, sizeof name, s))
  return progname_refuse(out, n);
 if (s->self_linked) {
  if (wpe_path_copy(out, n, name))
   return -1;
  return 0;
 }
 return progname_place(out, n, s->exedir, name);
}

/**
 * wpe_progname_object - The object file of a source, under Exedir.
 * @out/@n: receive the path; @s: the source.
 *
 * Return: 0, or -1 with @out empty when it does not fit.
 */
int wpe_progname_object(char *out, size_t n, const WpeProgramSource *s)
{
 if (progname_source_ok(s) && s->class_object)
  return progname_placed_stem_with(out, n, s, WPE_CLASS_SUFFIX);
 return progname_placed_stem_with(out, n, s, WPE_OBJECT_SUFFIX);
}

/**
 * wpe_progname_debug_binary - The binary a compile-for-debug writes.
 * @out/@n: receive the path; @s: the source.
 *
 * Return: 0, or -1 with @out empty when it does not fit.
 */
int wpe_progname_debug_binary(char *out, size_t n, const WpeProgramSource *s)
{
 return progname_placed_stem_with(out, n, s, WPE_DEBUG_SUFFIX);
}

/**
 * wpe_progname_debug_errors - The file a compile-for-debug writes its
 * messages to.
 * @out/@n: receive the path; @s: the source.
 *
 * Return: 0, or -1 with @out empty when it does not fit.
 */
int wpe_progname_debug_errors(char *out, size_t n, const WpeProgramSource *s)
{
 return progname_placed_stem_with(out, n, s, WPE_DEBUG_ERRORS_SUFFIX);
}

/**
 * wpe_progname_project_program - A project's program, under Exedir.
 * @out/@n: receive the path; @exename_word: the project's EXENAME word, or
 * NULL or "" when it has none; @exedir: Exedir, NULL or "" for the build
 * folder.
 *
 * A project with no EXENAME links "a.out", as the project chapter says.
 * Return: 0, or -1 with @out empty when it does not fit.
 */
int wpe_progname_project_program(char *out, size_t n, const char *exename_word,
                                 const char *exedir)
{
 if (progname_has(exename_word))
  return progname_place(out, n, exedir, exename_word);
 return progname_place(out, n, exedir, WPE_PROJECT_DEFAULT_PROGRAM);
}

/**
 * wpe_progname_project_default - The program a new project is given.
 * @out/@n: receive the EXENAME; @project_path: the project file's path.
 *
 * "/p/my.app.prj" gives "my.app": the same stem rule as for a source.
 * Return: 0, or -1 with @out empty when it does not fit.
 */
int wpe_progname_project_default(char *out, size_t n, const char *project_path)
{
 return wpe_progname_stem(out, n, project_path);
}
