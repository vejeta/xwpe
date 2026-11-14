/* test_progname.c -- unit test for the one rule that names what a build
 * makes from a source (we_progname.c).  Links we_progname.c with the path
 * owner and the start plan whose operand it joins through; no editor, always
 * runs.
 *
 * A table of sources, each with the program, the object and the debug
 * binary it must give: the extension and stem of a name with several dots,
 * a leading dot, no dot, a dot in the folder, and a name that looks like an
 * option; Exedir unset, empty, plain, with a trailing separator and an
 * option-like one; the language's Executable relative, absolute, and empty;
 * a compiler that writes the program itself; a Java class object.  Then the
 * edges: a name that fits to the byte and one that does not (refused whole,
 * the buffer left empty), a join of two parts that fit separately, a
 * source with no name, and the project's two names.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../we_path.h"
#include "../we_progname.h"

#define LONG_STEM     250       /* a long file name that every file system takes */
#define TINY_ROOM     5         /* "ab.o" and its terminator, to the byte */
#define A_OUT_ROOM    6         /* "a.out" and its terminator, to the byte */
#define EXEDIR_LONG   100

static int fails;

static void chk_int(const char *what, long got, long want)
{
	if (got != want) {
		printf("FAIL: %s: got %ld want %ld\n", what, got, want);
		fails++;
	}
}

static void chk_str(const char *what, const char *got, const char *want)
{
	if (!got || strcmp(got, want) != 0) {
		printf("FAIL: %s: got \"%s\" want \"%s\"\n", what,
		       got ? got : "(null)", want);
		fails++;
	}
}

/* @len copies of @c, as a heap string. */
static char *run(char c, size_t len)
{
	char *s = malloc(len + 1);

	if (!s)
		exit(2);
	memset(s, c, len);
	s[len] = '\0';
	return s;
}

typedef struct {
	const char *label;
	const char *name, *exedir, *executable;
	int self_linked, class_object;
	const char *program_name, *program, *object, *debug, *errors;
} Row;

static const Row rows[] = {
	{ "a.c", "a.c", NULL, NULL, 0, 0,
	  "a.e", "a.e", "a.o", "a.dbg", "a.dbgerr" },
	{ "a.b.c cuts at the last dot", "a.b.c", NULL, NULL, 0, 0,
	  "a.b.e", "a.b.e", "a.b.o", "a.b.dbg", "a.b.dbgerr" },
	{ "a has no extension", "a", NULL, NULL, 0, 0,
	  "a.e", "a.e", "a.o", "a.dbg", "a.dbgerr" },
	{ ".hidden.c", ".hidden.c", NULL, NULL, 0, 0,
	  ".hidden.e", ".hidden.e", ".hidden.o", ".hidden.dbg", ".hidden.dbgerr" },
	{ ".c alone: today's stem is empty", ".c", NULL, NULL, 0, 0,
	  ".e", ".e", ".o", ".dbg", ".dbgerr" },
	{ "a dot in the folder is not the name's", "x.d/a", NULL, NULL, 0, 0,
	  "a.e", "a.e", "a.o", "a.dbg", "a.dbgerr" },
	{ "src/x.c", "src/x.c", NULL, NULL, 0, 0,
	  "x.e", "x.e", "x.o", "x.dbg", "x.dbgerr" },
	{ "a.b.rs for the debug binary", "a.b.rs", NULL, NULL, 0, 0,
	  "a.b.e", "a.b.e", "a.b.o", "a.b.dbg", "a.b.dbgerr" },
	{ "-x.c is never an option", "-x.c", NULL, NULL, 0, 0,
	  "-x.e", "./-x.e", "./-x.o", "./-x.dbg", "./-x.dbgerr" },
	{ "Exedir out.d", "a.c", "out.d", NULL, 0, 0,
	  "a.e", "out.d/a.e", "out.d/a.o", "out.d/a.dbg", "out.d/a.dbgerr" },
	{ "Exedir with a trailing separator", "a.c", "out/", NULL, 0, 0,
	  "a.e", "out/a.e", "out/a.o", "out/a.dbg", "out/a.dbgerr" },
	{ "Exedir .", "a.c", ".", NULL, 0, 0,
	  "a.e", "./a.e", "./a.o", "./a.dbg", "./a.dbgerr" },
	{ "Exedir absolute", "a.c", "/o/out", NULL, 0, 0,
	  "a.e", "/o/out/a.e", "/o/out/a.o", "/o/out/a.dbg", "/o/out/a.dbgerr" },
	{ "Exedir absolute, self linked: the program stays, the object goes",
	  "a.pas", "/o/out", NULL, 1, 0,
	  "a", "a", "/o/out/a.o", "/o/out/a.dbg", "/o/out/a.dbgerr" },
	{ "Exedir empty is the build folder", "a.c", "", NULL, 0, 0,
	  "a.e", "a.e", "a.o", "a.dbg", "a.dbgerr" },
	{ "Exedir that looks like an option", "a.c", "-d", NULL, 0, 0,
	  "a.e", "./-d/a.e", "./-d/a.o", "./-d/a.dbg", "./-d/a.dbgerr" },
	{ "Executable relative, no Exedir", "a.c", NULL, "out/prog", 0, 0,
	  "out/prog", "out/prog", "a.o", "a.dbg", "a.dbgerr" },
	{ "Executable relative, Exedir out.d", "a.c", "out.d", "out/prog", 0, 0,
	  "out/prog", "out.d/out/prog", "out.d/a.o", "out.d/a.dbg", "out.d/a.dbgerr" },
	{ "Executable absolute, Exedir . (today's join)", "a.c", ".", "/abs/prog", 0, 0,
	  "/abs/prog", ".//abs/prog", "./a.o", "./a.dbg", "./a.dbgerr" },
	{ "Executable absolute, Exedir empty", "a.c", "", "/abs/prog", 0, 0,
	  "/abs/prog", "/abs/prog", "a.o", "a.dbg", "a.dbgerr" },
	{ "Executable empty is unset", "a.c", NULL, "", 0, 0,
	  "a.e", "a.e", "a.o", "a.dbg", "a.dbgerr" },
	{ "self linked, Exedir out.d: the program stays where the compiler ran",
	  "a.pas", "out.d", NULL, 1, 0,
	  "a", "a", "out.d/a.o", "out.d/a.dbg", "out.d/a.dbgerr" },
	{ "self linked with an Executable is not placed", "a.pas", "out.d", "prog", 1, 0,
	  "prog", "prog", "out.d/a.o", "out.d/a.dbg", "out.d/a.dbgerr" },
	{ "rustc with an Executable: the user's word, never placed", "a.rs", NULL, "prog", 1, 0,
	  "prog", "prog", "a.o", "a.dbg", "a.dbgerr" },
	{ "self linked, a stem with a hyphen is what rustc writes", "my-prog.rs", NULL, NULL, 1, 0,
	  "my-prog", "my-prog", "my-prog.o", "my-prog.dbg", "my-prog.dbgerr" },
	{ "class object, linked by the editor", "T.java", "out.d", NULL, 0, 1,
	  "T.e", "out.d/T.e", "out.d/T.class", "out.d/T.dbg", "out.d/T.dbgerr" },
	{ "javac: self linked and a class object", "T.java", "out.d", NULL, 1, 1,
	  "T", "T", "out.d/T.class", "out.d/T.dbg", "out.d/T.dbgerr" },
};

static void chk_op(const char *what, const char *label, int rc, const char *got,
		   const char *want)
{
	char msg[256];

	snprintf(msg, sizeof msg, "%s: %s", label, what);
	chk_int(msg, rc, 0);
	chk_str(msg, got, want);
}

static void test_table(void)
{
	size_t i;
	char out[WPE_PATH_SIZE];

	for (i = 0; i < sizeof rows / sizeof rows[0]; i++) {
		const Row *r = &rows[i];
		WpeProgramSource s;

		s.name = r->name;
		s.exedir = r->exedir;
		s.executable = r->executable;
		s.self_linked = r->self_linked;
		s.class_object = r->class_object;
		chk_op("program_name", r->label,
		       wpe_progname_program_name(out, sizeof out, &s), out, r->program_name);
		chk_op("program", r->label,
		       wpe_progname_program(out, sizeof out, &s), out, r->program);
		chk_op("object", r->label,
		       wpe_progname_object(out, sizeof out, &s), out, r->object);
		chk_op("debug_binary", r->label,
		       wpe_progname_debug_binary(out, sizeof out, &s), out, r->debug);
		chk_op("debug_errors", r->label,
		       wpe_progname_debug_errors(out, sizeof out, &s), out, r->errors);
	}
}

static void chk_stem(const char *path, const char *ext, const char *stem)
{
	char out[WPE_PATH_SIZE];
	char what[WPE_PATH_SIZE + 64];

	snprintf(what, sizeof what, "extension of %.200s", path);
	chk_str(what, wpe_progname_extension(path), ext);
	snprintf(what, sizeof what, "stem of %.200s", path);
	chk_int(what, wpe_progname_stem(out, sizeof out, path), 0);
	chk_str(what, out, stem);
}

static void test_extension_and_stem(void)
{
	chk_stem("a.c", ".c", "a");
	chk_stem("a.b.c", ".c", "a.b");
	chk_stem("a", "", "a");
	chk_stem(".hidden.c", ".c", ".hidden");
	chk_stem(".c", ".c", "");
	chk_stem("x.d/a", "", "a");
	chk_stem("x.d/a.c", ".c", "a");
	chk_stem("src/x.c", ".c", "x");
	chk_stem("nuevo.v2.prj", ".prj", "nuevo.v2");
	chk_stem("a.", ".", "a");
	chk_stem("", "", "");
	chk_stem("dir/", "", "");
}

static void test_long_names(void)
{
	char out[WPE_PATH_SIZE], tiny[TINY_ROOM];
	char *stem = run('n', LONG_STEM);
	char *name = malloc(LONG_STEM + 3);
	char *want = malloc(LONG_STEM + 3);
	char *edir = run('d', EXEDIR_LONG);
	char *big, *placed;
	WpeProgramSource s = { NULL, NULL, NULL, 0, 0 };

	if (!name || !want)
		exit(2);
	snprintf(name, LONG_STEM + 3, "%s.c", stem);
	s.name = name;
	chk_int("250 byte stem: program", wpe_progname_program(out, sizeof out, &s), 0);
	snprintf(want, LONG_STEM + 3, "%s.e", stem);
	chk_str("250 byte stem: program is whole", out, want);
	chk_int("250 byte stem: length", (long)strlen(out), LONG_STEM + 2);
	s.exedir = edir;
	placed = malloc(EXEDIR_LONG + LONG_STEM + 8);
	if (!placed)
		exit(2);
	snprintf(placed, EXEDIR_LONG + LONG_STEM + 8, "%s/%s.o", edir, stem);
	chk_int("250 byte stem under a long Exedir: object",
		wpe_progname_object(out, sizeof out, &s), 0);
	chk_str("250 byte stem under a long Exedir: object is whole", out, placed);
	free(placed);

	/* To the byte: the stem of a name of 4095 bytes fits, of 4096 does not. */
	big = run('n', WPE_PATH_SIZE - 1);
	chk_int("stem of a name of WPE_PATH_SIZE-1 bytes fits",
		wpe_progname_stem(out, sizeof out, big), 0);
	chk_int("stem is whole", (long)strlen(out), WPE_PATH_SIZE - 1);
	free(big);
	big = run('n', WPE_PATH_SIZE);
	memset(out, 'X', sizeof out);
	chk_int("stem of a name of WPE_PATH_SIZE bytes is refused",
		wpe_progname_stem(out, sizeof out, big), -1);
	chk_int("a refused stem leaves the buffer empty", out[0], '\0');
	s.name = big;
	s.exedir = NULL;
	memset(out, 'X', sizeof out);
	chk_int("program of a 4096 byte path is refused",
		wpe_progname_program(out, sizeof out, &s), -1);
	chk_int("a refused program leaves the buffer empty", out[0], '\0');
	memset(out, 'X', sizeof out);
	chk_int("object of a 4096 byte path is refused",
		wpe_progname_object(out, sizeof out, &s), -1);
	chk_int("a refused object leaves the buffer empty", out[0], '\0');
	free(big);

	/* A name that fits, and its suffix that does not. */
	big = run('n', WPE_PATH_SIZE - 3);
	s.name = big;
	chk_int("name of WPE_PATH_SIZE-3 bytes: program fits with its .e",
		wpe_progname_program(out, sizeof out, &s), 0);
	chk_int("program is whole", (long)strlen(out), WPE_PATH_SIZE - 1);
	free(big);
	big = run('n', WPE_PATH_SIZE - 2);
	s.name = big;
	memset(out, 'X', sizeof out);
	chk_int("name of WPE_PATH_SIZE-2 bytes: the .e does not fit",
		wpe_progname_program(out, sizeof out, &s), -1);
	chk_int("and the buffer is empty", out[0], '\0');
	free(big);

	/* Two parts that fit apart and not together. */
	big = run('n', WPE_PATH_SIZE - 10);
	s.name = big;
	s.exedir = edir;
	memset(out, 'X', sizeof out);
	chk_int("a folder and a name that fit apart, joined, are refused",
		wpe_progname_program(out, sizeof out, &s), -1);
	chk_int("the join leaves the buffer empty", out[0], '\0');
	free(big);

	/* A small buffer, to the byte. */
	s.name = "ab.c";
	s.exedir = NULL;
	chk_int("ab.o in exactly its room", wpe_progname_object(tiny, sizeof tiny, &s), 0);
	chk_str("ab.o is whole", tiny, "ab.o");
	memset(tiny, 'X', sizeof tiny);
	chk_int("ab.o one byte short is refused",
		wpe_progname_object(tiny, sizeof tiny - 1, &s), -1);
	chk_int("and leaves the buffer empty", tiny[0], '\0');
	free(stem);
	free(name);
	free(want);
	free(edir);
}

static void test_no_source(void)
{
	char out[WPE_PATH_SIZE];
	WpeProgramSource s = { NULL, "out.d", NULL, 0, 0 };

	memset(out, 'X', sizeof out);
	chk_int("a source with no name: program", wpe_progname_program(out, sizeof out, &s), -1);
	chk_int("a source with no name: buffer empty", out[0], '\0');
	memset(out, 'X', sizeof out);
	chk_int("no source at all: program_name", wpe_progname_program_name(out, sizeof out, NULL), -1);
	chk_int("no source at all: buffer empty", out[0], '\0');
	memset(out, 'X', sizeof out);
	chk_int("a source with no name: object", wpe_progname_object(out, sizeof out, &s), -1);
	chk_int("a source with no name: object buffer empty", out[0], '\0');
	memset(out, 'X', sizeof out);
	chk_int("NULL path: stem", wpe_progname_stem(out, sizeof out, NULL), -1);
	chk_int("NULL path: stem buffer empty", out[0], '\0');
	chk_str("NULL path: extension", wpe_progname_extension(NULL), "");
	chk_int("no buffer: stem",wpe_progname_stem(NULL, 0, "a.c"), -1);
	chk_int("no room: program", wpe_progname_program(out, 0, &s), -1);
}

static void chk_project(const char *what, const char *word, const char *exedir,
			const char *want)
{
	char out[WPE_PATH_SIZE];

	chk_int(what, wpe_progname_project_program(out, sizeof out, word, exedir), 0);
	chk_str(what, out, want);
}

static void chk_default(const char *path, const char *want)
{
	char out[WPE_PATH_SIZE];

	chk_int(path, wpe_progname_project_default(out, sizeof out, path), 0);
	chk_str(path, out, want);
}

static void test_project(void)
{
	char out[A_OUT_ROOM];

	chk_project("EXENAME NULL is a.out", NULL, NULL, "a.out");
	chk_project("EXENAME empty is a.out", "", NULL, "a.out");
	chk_project("EXENAME NULL under Exedir out", NULL, "out", "out/a.out");
	chk_project("EXENAME prog under Exedir .", "prog", ".", "./prog");
	chk_project("EXENAME -x is never an option", "-x", NULL, "./-x");
	chk_project("EXENAME -x under Exedir out", "-x", "out", "out/-x");
	chk_project("EXENAME absolute under Exedir . (today's join)", "/abs/p", ".", ".//abs/p");
	chk_project("EXENAME absolute, Exedir empty", "/abs/p", "", "/abs/p");
	chk_default("nuevo.v2.prj", "nuevo.v2");
	chk_default("/p/q.prj", "q");
	chk_default("/p/my.app.prj", "my.app");
	chk_default("x.d/noext", "noext");
	chk_default(".prj", "");
	memset(out, 'X', sizeof out);
	chk_int("a.out one byte short is refused",
		wpe_progname_project_program(out, sizeof out - 1, NULL, NULL), -1);
	chk_int("and leaves the buffer empty", out[0], '\0');
	memset(out, 'X', sizeof out);
	chk_int("a.out in its room", wpe_progname_project_program(out, sizeof out, NULL, NULL), 0);
	chk_str("a.out whole", out, "a.out");
}

int main(void)
{
	test_table();
	test_extension_and_stem();
	test_long_names();
	test_no_source();
	test_project();
	if (fails) {
		printf("test_progname: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_progname: ok\n");
	return 0;
}
