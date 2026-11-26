/* we_ai_step.h -- how a tool step of the assistant ended.
 *
 * A tool step is one run of a tool the model asked for: read a file, run a
 * command, fetch a page.  Some answer at once; a web page and a command are
 * read on the editor's loop, so the editor keeps taking keys and the result
 * comes later.  What a step answers is an outcome, never the identity of a
 * pointer: the text travels apart, through an out-parameter, so a compiler
 * that knows a function returns fresh memory cannot fold the outcome away.
 * GCC did exactly that to the old pointer compare (GCC PR 129066,
 * https://gcc.gnu.org/bugzilla/show_bug.cgi?id=129066).
 *
 * Pure and header-only, with no editor types, so a unit test links nothing
 * else.  The only writer of the outcome and of the text of a step is this
 * header.
 */
#ifndef WE_AI_STEP_H
#define WE_AI_STEP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** How a tool step ended. */
enum ai_tool_step {
 AI_STEP_NO_TOOL = 0,   /* the reply asked for no tool: it is the answer      */
 AI_STEP_RESULT,        /* the text is ready now; the caller owns it           */
 AI_STEP_WAITS          /* the text comes later, once, through the task's      */
};                      /* continuation (ai_page_done_fn)                      */

/**
 * ai_step_result - The step has its text now.
 * @text: receives the result the model reads next; @owned: that text,
 * malloc'd, or NULL when memory ran out.
 *
 * The model reads what the tool printed on its next turn and the user sees the
 * run go on.  The caller owns *text and frees it once; NULL is still a
 * result (the feed turns it into an explicit note), never "no tool".
 * Return: AI_STEP_RESULT.
 */
static inline enum ai_tool_step ai_step_result(char **text, char *owned)
{
 *text = owned;
 return AI_STEP_RESULT;
}

/**
 * ai_step_waits - A page or a command was started on the loop.
 * @text: receives NULL, there is nothing to free.
 *
 * The editor goes on taking keys while the page is read; the task's
 * continuation hands the model the result once, when it is done, or nobody
 * receives it when the user ends the task first.
 * Return: AI_STEP_WAITS.
 */
static inline enum ai_tool_step ai_step_waits(char **text)
{
 *text = NULL;
 return AI_STEP_WAITS;
}

/**
 * ai_step_no_tool - There was nothing to run.
 * @text: receives NULL.
 *
 * The chat shows the reply as the answer instead of acting on it.
 * Return: AI_STEP_NO_TOOL.
 */
static inline enum ai_tool_step ai_step_no_tool(char **text)
{
 *text = NULL;
 return AI_STEP_NO_TOOL;
}

/**
 * ai_step_turn_text - The message that carries a tool's text to the model.
 * @head: the words that open the message ("TOOL RESULT:" and a newline);
 * @text: what the tool answered, or NULL.
 *
 * The model reads the head and then the text, as the next turn's input.  A
 * step that had no text (memory ran out before it was made) gives no message,
 * so no caller reads the length of nothing.
 * Return: the malloc'd message, NULL when @text is NULL or memory ran out.
 */
static inline char *ai_step_turn_text(const char *head, const char *text)
{
 size_t hl, tl;
 char *msg;
 if (!text)
  return NULL;
 hl = strlen(head);
 tl = strlen(text);
 msg = malloc(hl + tl + 1);
 if (!msg)
  return NULL;
 memcpy(msg, head, hl);
 memcpy(msg + hl, text, tl + 1);
 return msg;
}

#endif /* WE_AI_STEP_H */
