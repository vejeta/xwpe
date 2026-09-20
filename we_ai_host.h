/* we_ai_host.h                                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey             */
/* This is free software; you can redistribute it and/or */
/* modify it under the terms of the                      */
/* GNU General Public License, see the file COPYING.     */

/* External agent host: run a coding-agent CLI (Claude Code first) as a
   persistent subprocess that speaks stream-json on stdin/stdout, driven on the
   editor's fd-loop.  The transport and stream-json event parsing live here; the
   UI (we_ai_ui.c) supplies callbacks that render events into the pane and reflect
   file edits into open buffers.  Adding another agent CLI is a new adapter, not a
   fork of this control flow. */

#ifndef WE_AI_HOST_H
#define WE_AI_HOST_H

#ifdef WPE_AI_AGENT_HOST

#include <stddef.h>

/* Opaque handle to one hosted agent session. */
typedef struct wpe_host wpe_host;

/* Callbacks invoked by wpe_host_pump as stream-json events arrive.  `ud` is
   passed through opaquely.  Any callback may be NULL. */
typedef struct {
 void (*on_text)(const char *text, void *ud);            /* assistant answer text */
 void (*on_tool)(const char *tool, const char *arg, void *ud); /* a tool ran */
 void (*on_file_edit)(const char *path, void *ud);       /* a tool wrote this path */
 void (*on_notice)(const char *text, void *ud);          /* system/init/error line */
 void (*on_result)(const char *summary, int is_error, void *ud); /* end of a turn */
} wpe_host_events;

/* Spawn the agent CLI.  `model` and `resume` may be NULL or "".  `policy` is a
   WPE_AI_POLICY_* value (maps to the CLI's permission flags).  Returns a handle,
   or NULL with `err` filled. */
wpe_host   *wpe_host_start(const char *model, const char *resume, int policy,
                           char *err, size_t errsz);

/* The child's stdout fd, to register with wpe_fd_add. */
int         wpe_host_fd(wpe_host *h);

/* Feed one user turn to the live session (written as a stream-json user
   message).  Returns 0 on success, -1 on write error. */
int         wpe_host_send(wpe_host *h, const char *user_text);

/* Drain readable stdout, parse the complete stream-json lines available, and
   invoke `ev`'s callbacks.  Sets *turn_done non-zero when a result event ended a
   turn; sets *hup non-zero when the child closed stdout / exited. */
void        wpe_host_pump(wpe_host *h, const wpe_host_events *ev, void *ud,
                          int *turn_done, int *hup);

/* The session id the CLI reported (for --resume), or "" if none yet. */
const char *wpe_host_session_id(wpe_host *h);

/* Terminate the child (if alive), reap it without blocking the editor, and free
   the handle. */
void        wpe_host_free(wpe_host *h);

#endif /* WPE_AI_AGENT_HOST */
#endif /* WE_AI_HOST_H */
