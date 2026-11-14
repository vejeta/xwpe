/* we_progname.h -- the one rule that names what a build makes from a source
   or a project, and where it is put.

   The program F9 links, the object it compiles, the program Run starts, the
   one the debugger loads and the binary a compile-for-debug writes are all
   named from the same source.  They are named here and nowhere else, so
   that every command the editor runs or shows names the same file: a
   program is never the result of cutting a path that holds a folder, and
   what the user typed (Executable, EXENAME) is never replaced by a placed
   path.

   Strings in, strings out: no editor state.  The only join of a folder and
   a name is the start plan's (wpe_startplan_operand), so every placed path
   keeps its rule ("./" before a name that starts with '-' or '@') and the
   module links with we_path.c and we_startplan.c.  A result that does not
   fit is refused (-1, @out empty), never cut, like we_path.h. */
#ifndef WE_PROGNAME_H
#define WE_PROGNAME_H

#include <stddef.h>

/* The suffixes a build gives its outputs: defined here and nowhere else. */
#define WPE_PROGRAM_SUFFIX          ".e"
#define WPE_OBJECT_SUFFIX           ".o"
#define WPE_CLASS_SUFFIX            ".class"
#define WPE_DEBUG_SUFFIX            ".dbg"
#define WPE_DEBUG_ERRORS_SUFFIX     ".dbgerr"
#define WPE_PROJECT_DEFAULT_PROGRAM "a.out"

/* A source as the build sees it.  Every field may be NULL except name. */
typedef struct {
	const char *name;       /* the source's file name (f->datnam), or a FILES word */
	const char *exedir;     /* Exedir; NULL or "" = the build folder */
	const char *executable; /* the language's Executable, or NULL or "" */
	int self_linked;        /* the compiler writes the program itself (fpc, javac, rustc, a68g) */
	int class_object;       /* the object is a .class (kept apart from self_linked) */
} WpeProgramSource;

/* ".c" of "a.b.c" or "src/x.c": from the last dot of the name (the text after
   the last '/') to its end, wherever that dot is (".c" alone is ".c").  A
   name with no dot gives "", a pointer to the terminator.  Points into
   @path. */
const char *wpe_progname_extension(const char *path);

/* The name of @path without its folder and its extension: "a.b" for
   "src/a.b.c", "" for ".c", "a" for "a".  0, or -1 and @out empty when it
   does not fit. */
int wpe_progname_stem(char *out, size_t n, const char *path);

/* What the build calls the program, not placed: the language's Executable as
   typed when set; else the stem plus ".e", or the stem alone for a compiler
   that writes the program itself.  The stem alone is also the class jdb takes
   as its main class. */
int wpe_progname_program_name(char *out, size_t n, const WpeProgramSource *s);

/* The program as a command names it: @program_name under Exedir, through the
   start plan's operand.  A self-linked program is where the compiler ran, the
   build folder, and is never placed.  An absolute Executable under an Exedir
   is joined like any name (".//abs"), as the link has always done. */
int wpe_progname_program(char *out, size_t n, const WpeProgramSource *s);

/* The object of the source: stem plus ".o", or ".class" when class_object;
   placed under Exedir. */
int wpe_progname_object(char *out, size_t n, const WpeProgramSource *s);

/* The binary a compile-for-debug writes (stem plus ".dbg") and its error
   file (".dbgerr"); placed under Exedir. */
int wpe_progname_debug_binary(char *out, size_t n, const WpeProgramSource *s);
int wpe_progname_debug_errors(char *out, size_t n, const WpeProgramSource *s);

/* A project's program: its EXENAME word, or "a.out" when it has none, placed
   under @exedir. */
int wpe_progname_project_program(char *out, size_t n, const char *exename_word,
                                 const char *exedir);

/* The EXENAME New Project writes for a project path: the stem
   ("/p/my.app.prj" gives "my.app"). */
int wpe_progname_project_default(char *out, size_t n, const char *project_path);

#endif
