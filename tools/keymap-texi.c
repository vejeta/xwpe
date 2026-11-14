/* keymap-texi.c -- print the key bindings (we_bindings.c) as the key reference.
 *
 *   keymap-texi            the Texinfo chapter, on standard output
 *   keymap-texi --dump S   one line per binding of style S (borland or modern):
 *                          context, key code, key, action, separated by " | "
 *
 * The reference chapter (docs/chapters/keys-generated.texi) is this program's
 * output, kept in the tree; make check fails when the two differ, so a binding
 * cannot change without the reference changing with it.  The dump of the CUA
 * style is kept as docs/chapters/keys-cua-1.6.txt, so the Modern style's
 * differences can be listed against it.
 *
 * Links we_bindings.c only: no editor, no display. */
#include <stdio.h>
#include <string.h>

#include "../we_bindings.h"

#define DOC_NAME_LEN 64

static const char *const style_titles[WPE_STYLE_COUNT] = {
	"Borland style", "Modern style"
};

/** Print @s with the three characters Texinfo reads as markup escaped. */
static void put_texi(FILE *out, const char *s)
{
	for (; *s; s++) {
		if (*s == '@' || *s == '{' || *s == '}')
			fputc('@', out);
		fputc(*s, out);
	}
}

/** Print the keys of @action in @style and @context as @kbd{...}, joined by
    commas, after the cell separator; an empty cell is the separator alone. */
static void put_cell(FILE *out, const WpeBinding *t, size_t n, int action, int style, int context)
{
	size_t i;
	int first = 1;
	char name[DOC_NAME_LEN];

	fputs(" @tab", out);
	for (i = 0; i < n; i++) {
		if (t[i].action != action || t[i].context != context ||
		    !(t[i].styles & WPE_STYLE_BIT(style)) || (t[i].flags & WPE_ROW_HIDDEN))
			continue;
		wpe_binding_row_name(&t[i], WPE_NAME_DOC, name, sizeof name);
		if (first)
			fputs(" @kbd{", out);
		else
			fputs(", @kbd{", out);
		put_texi(out, name);
		fputc('}', out);
		first = 0;
	}
}

/** Print the fourth column of a context table: what to press where a terminal
    keeps a Modern key, one phrase per Modern row that names one.  A row that
    needs a keyboard protocol and names nothing says so. */
static void put_fallback_cell(FILE *out, const WpeBinding *t, size_t n, int action, int context)
{
	size_t i;
	int first = 1;

	fputs(" @tab", out);
	for (i = 0; i < n; i++) {
		if (t[i].action != action || t[i].context != context ||
		    !(t[i].styles & WPE_STYLE_BIT(WPE_STYLE_MODERN)) || !t[i].fallback)
			continue;
		fputs(first ? " " : "; ", out);
		put_texi(out, t[i].fallback);
		first = 0;
	}
}

/** Is @action the first row of its context that is not hidden? Rows of one
    action are printed together, at the place of its first row. */
static int first_row_of_action(const WpeBinding *t, size_t i)
{
	size_t j;

	for (j = 0; j < i; j++)
		if (t[j].action == t[i].action && t[j].context == t[i].context)
			return 0;
	return 1;
}

static int context_has_visible_rows(const WpeBinding *t, size_t n, int context)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (t[i].context == context && !(t[i].flags & WPE_ROW_HIDDEN))
			return 1;
	return 0;
}

/** What a reader needs to know about when a context's keys are read, or NULL. */
static const char *context_note(int context)
{
	switch (context) {
	case WPE_CTX_PROGRAM:
	case WPE_CTX_DEBUG:
	case WPE_CTX_PREFIX_DEBUG:
	case WPE_CTX_PREFIX_LSP:
	case WPE_CTX_PREFIX_AI:
		return("These keys work in wpe and xwpe, the programming modes, and not in we.");
	case WPE_CTX_WATCHES:
		return("In the Watches window these keys are read before the tables above, and win.");
	case WPE_CTX_HELP:
		return("In a Help window these keys are read before the tables above, and win.");
	case WPE_CTX_LIST:
	case WPE_CTX_DIALOG_MOVE:
		return("These keys are read by the window or dialog named, not by an editor window.");
	default:
		return(NULL);
	}
}

static void print_context(FILE *out, const WpeBinding *t, size_t n, int context)
{
	size_t i;
	int style;
	const char *note = context_note(context);

	fprintf(out, "\n@subsection %s\n\n", wpe_context_name(context));
	if (note)
		fprintf(out, "%s\n\n", note);
	fprintf(out, "@multitable @columnfractions .16 .26 .18 .18 .22\n");
	fprintf(out, "@headitem Action @tab What it does");
	for (style = 0; style < WPE_STYLE_COUNT; style++)
		fprintf(out, " @tab %s", style_titles[style]);
	fprintf(out, " @tab Where a terminal keeps the Modern key\n");
	for (i = 0; i < n; i++) {
		if (t[i].context != context || !first_row_of_action(t, i))
			continue;
		if (t[i].flags & WPE_ROW_HIDDEN)
			continue;
		fputs("@item ", out);
		put_texi(out, wpe_action_name(t[i].action));
		fputs(" @tab ", out);
		put_texi(out, wpe_action_text(t[i].action));
		for (style = 0; style < WPE_STYLE_COUNT; style++)
			put_cell(out, t, n, t[i].action, style, context);
		put_fallback_cell(out, t, n, t[i].action, context);
		fputc('\n', out);
	}
	fputs("@end multitable\n", out);
}

/** The contexts an editor window reads for a key that acts on its own. */
static int is_direct_context(int context)
{
	return(context == WPE_CTX_WINDOW || context == WPE_CTX_PROGRAM ||
	       context == WPE_CTX_DEBUG || context == WPE_CTX_EDIT || context == WPE_CTX_CURSOR);
}

/** Does the Modern row need a keyboard protocol, or does some terminal keep it? */
static int row_is_delivery_noted(const WpeBinding *r)
{
	int emu;

	if (wpe_binding_key_class(r->code) == WPE_DELIVER_PROTOCOL)
		return(1);
	for (emu = 0; emu < WPE_EMU_COUNT; emu++)
		if (!wpe_key_reaches(r->code, emu))
			return(1);
	return(0);
}

/** Is row @i the first Modern row of the delivery table with its key?  A key
    bound in two contexts (F10: the menu, or a step in a session) is one line,
    since what a terminal delivers does not depend on what the key does. */
static int row_is_first_noted_with_code(const WpeBinding *t, size_t i)
{
	size_t j;

	for (j = 0; j < i; j++)
		if (t[j].code == t[i].code && (t[j].styles & WPE_STYLE_BIT(WPE_STYLE_MODERN)) &&
		    is_direct_context(t[j].context) && !(t[j].flags & WPE_ROW_HIDDEN))
			return(0);
	return(1);
}

/** One table of what each terminal does with the Modern keys that do not
    arrive everywhere: yes when the editor receives the key from that terminal. */
static void print_delivery(FILE *out, const WpeBinding *t, size_t n)
{
	size_t i;
	int emu;
	char name[DOC_NAME_LEN];

	fputs("\n@subsection Which terminals deliver the Modern keys\n\n", out);
	fputs("A key marked protocol is a key of its own only in a terminal that\n", out);
	fputs("implements xterm's modifyOtherKeys or kitty's keyboard protocol, which\n", out);
	fputs("the editor asks for.  The record of what each terminal was measured to\n", out);
	fputs("send is docs/chapters/keys-delivery.txt in the source.\n\n", out);
	fputs("@multitable @columnfractions .17 .09 .10 .10 .10 .10 .10 .09 .10\n", out);
	fputs("@headitem Key @tab Arrives", out);
	for (emu = 0; emu < WPE_EMU_COUNT; emu++) {
		fputs(" @tab ", out);
		put_texi(out, wpe_emulator_name(emu));
	}
	fputc('\n', out);
	for (i = 0; i < n; i++) {
		if (!(t[i].styles & WPE_STYLE_BIT(WPE_STYLE_MODERN)) || !is_direct_context(t[i].context) ||
		    (t[i].flags & WPE_ROW_HIDDEN) || !row_is_delivery_noted(&t[i]) ||
		    !row_is_first_noted_with_code(t, i))
			continue;
		wpe_binding_row_name(&t[i], WPE_NAME_DOC, name, sizeof name);
		fputs("@item @kbd{", out);
		put_texi(out, name);
		fputs("} @tab ", out);
		fputs(wpe_binding_key_class(t[i].code) == WPE_DELIVER_PROTOCOL ? "protocol" : "always", out);
		for (emu = 0; emu < WPE_EMU_COUNT; emu++)
			fputs(wpe_key_reaches(t[i].code, emu) ? " @tab yes" : " @tab no", out);
		fputc('\n', out);
	}
	fputs("@end multitable\n", out);
}

#define DUMP_LINE_LEN 256
#define DUMP_FIELDS 4

/** One line of a dump: context, code, key, action. */
typedef struct {
	char context[64];
	char key[64];
	char action[96];
} DumpLine;

/** Split a dump line "context | code | key | action"; 0 when it is not one. */
static int parse_dump_line(char *line, DumpLine *d)
{
	char *field[DUMP_FIELDS];
	int k = 0;
	char *p = line;

	field[k++] = p;
	while (k < DUMP_FIELDS && (p = strstr(p, " | ")) != NULL) {
		*p = '\0';
		p += 3;
		field[k++] = p;
	}
	if (k != DUMP_FIELDS)
		return(0);
	p = field[3] + strlen(field[3]);
	while (p > field[3] && (p[-1] == '\n' || p[-1] == '\r'))
		*--p = '\0';
	snprintf(d->context, sizeof d->context, "%s", field[0]);
	snprintf(d->key, sizeof d->key, "%s", field[2]);
	snprintf(d->action, sizeof d->action, "%s", field[3]);
	return(1);
}

/** The Modern row of the same key name in any context an editor window reads
    directly, NULL when the Modern style leaves the key alone. */
static const WpeBinding *modern_row_of_key(const WpeBinding *t, size_t n, const char *key,
                                           const char *old_action, int *same_action)
{
	size_t i;
	const WpeBinding *first = NULL;
	char name[DOC_NAME_LEN];

	*same_action = 0;
	for (i = 0; i < n; i++) {
		if (!(t[i].styles & WPE_STYLE_BIT(WPE_STYLE_MODERN)) || !is_direct_context(t[i].context) ||
		    (t[i].flags & WPE_ROW_HIDDEN))
			continue;
		wpe_binding_row_name(&t[i], WPE_NAME_DOC, name, sizeof name);
		if (strcmp(name, key) != 0)
			continue;
		if (strcmp(wpe_action_name(t[i].action), old_action) == 0)
			*same_action = 1;
		if (!first)
			first = &t[i];
	}
	return(first);
}

/** Is the context of a dump line one of the direct ones (by its title)? */
static int dump_context_is_direct(const char *title)
{
	int c;

	for (c = 0; c < WPE_CTX_COUNT; c++)
		if (is_direct_context(c) && strcmp(title, wpe_context_name(c)) == 0)
			return(1);
	return(0);
}

/**
 * print_changes - The two lists of what the Modern style did to the keys of
 * the 1.6 CUA style: reassigned (the same key, another command) and lost
 * (the key does nothing now), from the frozen dump of that style.
 * @frozen: the file docs/chapters/keys-cua-1.6.txt.
 *
 * A reassigned key is the worse change, because the hand does something
 * else, so it is listed first.
 */
static int print_changes(FILE *out, const char *frozen)
{
	size_t n;
	const WpeBinding *t = wpe_binding_table(&n);
	FILE *in = fopen(frozen, "r");
	char line[DUMP_LINE_LEN];
	DumpLine d;
	int pass;

	if (!in) {
		fprintf(stderr, "keymap-texi: cannot read %s\n", frozen);
		return(1);
	}
	fputs("@c Generated by tools/keymap-texi --changes from we_bindings.c and the\n", out);
	fputs("@c frozen dump docs/chapters/keys-cua-1.6.txt.  Do not edit.\n", out);
	for (pass = 0; pass < 2; pass++) {
		fputs(pass == 0 ? "\n@subsection Keys that do something else\n\n" :
		      "\n@subsection Keys that do nothing now\n\n", out);
		fputs(pass == 0 ?
		      "@multitable @columnfractions .20 .38 .42\n@headitem Key @tab 1.6 (the CUA style) @tab Modern\n" :
		      "@multitable @columnfractions .20 .38 .42\n@headitem Key @tab 1.6 (the CUA style) @tab Where it is now\n", out);
		rewind(in);
		while (fgets(line, sizeof line, in)) {
			const WpeBinding *m;
			int same;

			if (!parse_dump_line(line, &d) || !dump_context_is_direct(d.context) ||
			    strcmp(d.key, "(click)") == 0)
				continue;
			m = modern_row_of_key(t, n, d.key, d.action, &same);
			if (pass == 0 && m && !same) {
				fputs("@item @kbd{", out);
				put_texi(out, d.key);
				fputs("} @tab ", out);
				put_texi(out, d.action);
				fputs(" @tab ", out);
				put_texi(out, wpe_action_name(m->action));
				fputc('\n', out);
			} else if (pass == 1 && !m) {
				fputs("@item @kbd{", out);
				put_texi(out, d.key);
				fputs("} @tab ", out);
				put_texi(out, d.action);
				fputs(" @tab its menu entry\n", out);
			}
		}
		fputs("@end multitable\n", out);
	}
	fclose(in);
	return(0);
}

static void print_texi(FILE *out)
{
	size_t n;
	const WpeBinding *t = wpe_binding_table(&n);
	int context;

	fputs("@c Generated by tools/keymap-texi from we_bindings.c.  Do not edit:\n", out);
	fputs("@c change a binding there and run make; make check fails on any difference.\n", out);
	fputs("\nIn an editor window the Windows and menus, Programming, Debugger and\n", out);
	fputs("Editing tables are read in that order: a key that an earlier table\n", out);
	fputs("binds acts there and never reaches a later one.  The Programming and\n", out);
	fputs("Debugger tables apply in wpe and xwpe only.  The Help viewer's and the\n", out);
	fputs("Watches window's own keys are read first in those windows, and win.  A\n", out);
	fputs("cell with no key means the style binds none.\n", out);
	for (context = 0; context < WPE_CTX_COUNT; context++)
		if (context_has_visible_rows(t, n, context))
			print_context(out, t, n, context);
	print_delivery(out, t, n);
}

static void print_dump(FILE *out, int style)
{
	size_t n, i;
	const WpeBinding *t = wpe_binding_table(&n);
	char name[DOC_NAME_LEN];

	for (i = 0; i < n; i++) {
		if (!(t[i].styles & WPE_STYLE_BIT(style)))
			continue;
		wpe_binding_row_name(&t[i], WPE_NAME_DOC, name, sizeof name);
		if (!name[0])
			snprintf(name, sizeof name, "(click)");
		fprintf(out, "%s | %d | %s | %s\n", wpe_context_name(t[i].context),
		        t[i].code, name, wpe_action_name(t[i].action));
	}
}

int main(int argc, char **argv)
{
	/* "-o FILE" first: write there instead of to standard output, so a maintainer
	   regenerates a tracked file without a shell redirection. */
	if (argc >= 3 && strcmp(argv[1], "-o") == 0) {
		if (!freopen(argv[2], "w", stdout)) {
			fprintf(stderr, "keymap-texi: cannot write %s\n", argv[2]);
			return 1;
		}
		argc -= 2;
		argv += 2;
		argv[0] = "keymap-texi";
	}
	if (argc == 1) {
		print_texi(stdout);
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "--dump") == 0) {
		if (strcmp(argv[2], "borland") == 0) {
			print_dump(stdout, WPE_STYLE_BORLAND);
			return 0;
		}
		if (strcmp(argv[2], "modern") == 0) {
			print_dump(stdout, WPE_STYLE_MODERN);
			return 0;
		}
	}
	if (argc == 3 && strcmp(argv[1], "--changes") == 0)
		return(print_changes(stdout, argv[2]));
	fprintf(stderr, "usage: keymap-texi [-o FILE] [--dump borland|modern | --changes FROZEN-DUMP]\n");
	return 2;
}
