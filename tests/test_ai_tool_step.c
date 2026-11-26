/* test_ai_tool_step.c -- how a tool step of the assistant ended (we_ai_step.h).
 *
 * A tool step answers an outcome, and its text travels apart.  The rows build
 * the call shape the assistant has (a leaf that always answers, a wrapper
 * with a second answer, a dispatcher the compiler cannot see through, a
 * consumer that frees what it owns) on the owner's three constructors and
 * check what each consumer is told: a step that waits is a step that waits,
 * with no text to free; a result is freed once; "no tool" is not a result; a
 * result with no text (memory ran out) is still a result.
 *
 * The test asks for -O2, so at the distribution and CI flags the shape is the
 * optimised one; flags the builder puts after it (a -O0 debug build) win.  It
 * tests the owner's contract, not the compiler.
 */
#include <stdio.h>
#include "linebuf.h"
#include <stdlib.h>
#include <string.h>

#include "../we_ai_step.h"

#define MARK_TEXT "(a static text)"

static int fail;
static int freed_count;          /* texts the consumer freed                */
static int freed_static;         /* times it was asked to free the static   */
static char static_mark[] = MARK_TEXT;

static void check(int cond, const char *msg)
{
 if (!cond) { printf("FAIL: %s\n", msg); fail = 1; }
 else printf("ok: %s\n", msg);
}

/* The consumer's free: counts, and notes a free of storage it never owned. */
static void owned_free(char *p)
{
 if (p == static_mark)
  freed_static++;
 else if (p)
  freed_count++;
 if (p != static_mark)
  free(p);
}

/* A leaf that always answers now, with fresh heap. */
static char *leaf_deny(void)
{
 return strdup("(denied)");
}

/* What the wrapper's page does: starts (waits) or cannot begin (result). */
static enum ai_tool_step fetch_as_text(int can_start, char **text)
{
 if (can_start)
  return ai_step_waits(text);
 return ai_step_result(text, strdup("TOOL ERROR: cannot connect"));
}

/* A wrapper with a second answer of fresh heap. */
static enum ai_tool_step web_search(int refuse, int can_start, char **text)
{
 if (refuse)
  return ai_step_result(text, leaf_deny());
 return fetch_as_text(can_start, text);
}

/* The dispatcher the compiler cannot see through. */
typedef enum ai_tool_step (*tool_fn)(int, int, char **);
static tool_fn volatile dispatch = web_search;

static enum ai_tool_step run_tool(int refuse, int can_start, char **text)
{
 return dispatch(refuse, can_start, text);
}

/* The consumer: waits means nothing to free; a result is freed once. */
static int consume(enum ai_tool_step step, char *text)
{
 if (step == AI_STEP_WAITS)
  return 0;
 if (step == AI_STEP_NO_TOOL)
  return 2;
 owned_free(text);
 return 1;
}

int main(void)
{
 char *text;
 enum ai_tool_step step;
 int taken;

 /* a step that waits is seen as waits, with no text and nothing freed */
 text = static_mark;
 freed_count = freed_static = 0;
 step = run_tool(0, 1, &text);
 check(step == AI_STEP_WAITS, "a started page: the step waits");
 check(text == NULL, "a started page: its text is NULL, never a stale pointer");
 taken = consume(step, text);
 check(taken == 0 && freed_count == 0 && freed_static == 0,
       "a started page: the consumer frees nothing");

 /* a result is a result, freed once, by its owner */
 freed_count = freed_static = 0;
 step = run_tool(1, 1, &text);
 check(step == AI_STEP_RESULT, "a refusal answers at once: a result");
 check(text && !strcmp(text, "(denied)"), "a refusal: the text is the denial");
 taken = consume(step, text);
 check(taken == 1 && freed_count == 1 && freed_static == 0,
       "a refusal: freed once, and not the static");

 /* a fetch that cannot begin is a result with an error text */
 freed_count = freed_static = 0;
 step = run_tool(0, 0, &text);
 check(step == AI_STEP_RESULT, "a fetch that cannot begin: a result");
 check(text && !strncmp(text, "TOOL ERROR", 10), "a fetch that cannot begin: the error text");
 taken = consume(step, text);
 check(freed_count == 1 && freed_static == 0, "a fetch that cannot begin: freed once");

 /* no tool is not a result, and has no text */
 text = static_mark;
 step = ai_step_no_tool(&text);
 check(step == AI_STEP_NO_TOOL, "no tool line: the outcome is no tool");
 check(text == NULL, "no tool line: the text is NULL");
 check(step != AI_STEP_RESULT, "no tool line: it is not a result");
 freed_count = freed_static = 0;
 taken = consume(step, text);
 check(taken == 2 && freed_count == 0, "no tool line: the consumer frees nothing");

 /* out of memory is a result with no text, never "no tool" */
 text = static_mark;
 step = ai_step_result(&text, NULL);
 check(step == AI_STEP_RESULT, "out of memory: still a result");
 check(step != AI_STEP_NO_TOOL, "out of memory: not the chat's answer");
 check(text == NULL, "out of memory: the text is NULL");

 /* the message that carries a text to the model */
 text = ai_step_turn_text("HEAD:\n", "body");
 check(text && !strcmp(text, "HEAD:\nbody"), "turn text: the head, then the text");
 free(text);
 check(ai_step_turn_text("HEAD:\n", NULL) == NULL, "turn text: no text gives no message");
 text = ai_step_turn_text("HEAD:\n", "");
 check(text && !strcmp(text, "HEAD:\n"), "turn text: an empty text is the head alone");
 free(text);

 printf(fail ? "SOME TESTS FAILED\n" : "ALL OK\n");
 return fail;
}
