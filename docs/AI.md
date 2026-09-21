# The AI assistant in xwpe (experimental)

xwpe can host a local-first **AI assistant** behind the **`Alt-G`** prefix, the
same way the `Alt-Q` prefix drives the [language server](LSP.md). It is
**opt-in and off by default** -- a stock build contains none of it, so a distro
(or anyone who does not want AI in their editor) gets the exact classic editor.

## Quick start (local, private, free -- with Ollama)

No API key, no account, nothing leaves your machine:

```sh
# 1. a local model server + a code-tuned model
ollama serve &                 # if not already running
ollama pull qwen2.5-coder      # or deepseek-coder, codellama, ...

# 2. an AI-enabled xwpe
./configure --enable-ai && make

# 3. run it, enable the assistant once (Options > AI, or Options > Editor),
#    then use the Alt-G prefix
./wpe
```

That is all: with Ollama as the backend and no model chosen, xwpe auto-selects a
code model (preferring a larger one you have installed), so the first `Alt-G a`
just works. Point it elsewhere with `XWPE_AI_BACKEND` / `XWPE_AI_ENDPOINT` /
`XWPE_AI_MODEL` or in **Options > AI** (Alt-M lists the backend's models). Every
mode is handed the files you have open as context, so you never have to tell it
what you are working on.

It has three modes, all streaming and non-blocking (you keep editing while the
model works):

- **Ask** (`Alt-G a`) -- a **chat** pane docked at the bottom. It can read files
  in the workspace to answer, and keeps context across the conversation.
- **Edit** (`Alt-G e`) -- rewrite the current file from an instruction. If a
  **block is marked**, only that region is sent and changed -- the rest of the
  file is untouched; with no selection it edits the whole file. The proposed
  change is shown as a **colored diff over the affected lines** (red = removed,
  green = added, with context). Each change is an independent hunk with its own
  `y`/`n`/`a`/`q`, so you can accept some and reject others; the accepted hunks
  apply as **one** edit that a single **`Ctrl-U`** undoes.
- **Agent (tools)** (`Alt-G g`) -- a tool-using agent: it lists/reads/greps files
  and proposes `write_file` / `run_command` steps **one at a time**, each shown
  for approval (a write is previewed as a diff); an unattended run takes a
  checkpoint first and offers a reviewable changeset at the end. When it finishes
  you can type a **follow-up** right in the pane.  (Multi-file edit vs Agent:
  Multi-file proposes one coordinated change and asks once; the Agent works
  step-by-step, reading and acting as it goes.)
- **Build & fix (agent)** (`Alt-G b`) -- a specialized agent: runs the build
  (`make` when a makefile is present, otherwise a syntax-only compile of the
  current file) and, if it fails, loops -- read the errors, edit the sources,
  re-build -- until it passes. Same tools and permission level as the agent.

The `Alt-G` menu groups the **actions** above and, below a divider, the quick
**settings**: *Permissions* (shows and cycles ask/edits/auto), *Clear
conversation*, and *AI settings...* which opens **Options > AI** (the model, the
enable toggle and the full configuration live there).
The **Agent** can run on one of two **engines** (choose in Options > AI, only in
a `--enable-ai-agent-host` build): *Built-in* (the editor's own tool loop, works
with any backend) or *Claude Code* (runs the real `claude` CLI as a persistent
session using **its own** tools, with xwpe as the front-end). With the Claude
Code engine, `Alt-G g` streams the agent's answer and tool activity into the
pane, reloads any file it edits into that file's window (one `Ctrl-U` reverts),
and keeps the session live across turns -- `Alt-G g` feeds the next turn, an
empty prompt (or `Esc`) ends it and offers a reviewable/revertible changeset. The
permission dial gates its tools **per tool**: *Ask* pops a `y`/`n` for each gated
tool call, *Edits* auto-accepts file edits but asks before commands, *Auto* runs
unattended (a checkpoint is taken either way). It is built as a generic "host an
agent CLI that speaks stream-json" (Claude Code first; others as adapters later).

`Alt-G f` runs a **Multi-file edit**: it studies the workspace read-only, then
proposes a coordinated change across several files and shows it as a **modal
approval dialog** -- **Enter/A** applies all, **F** reviews file-by-file (each in
the diff box), **Q/Esc** cancels; nothing is written until you approve. (This is
what some tools call a "workspace edit"; it is *not* an agent "plan mode.")
`Alt-G m` opens the scrollable **model picker**.

The pane keeps its `> ` prompt docked at the bottom while you type. To read back a
long conversation, **PgUp/PgDn** page through the transcript and the **arrow keys**
scroll it a line at a time; typing any character returns the view to the prompt so
the next key lands in the input.

When a [language server](LSP.md) is running, the errors and warnings it reports
for the current file (the ones you see underlined) are folded into the prompt for
Ask, Edit, and the agent -- so "fix this" works without pasting the message.

Nothing about the model ships inside xwpe: Ollama and OpenAI-compatible servers
are plain HTTP, and the Claude CLI is a subprocess using your own login. The
editor binary stays a few megabytes.

---

## Build

The assistant is a configure-time option, **off by default**:

```sh
./configure --enable-ai            # Chat/Edit/Agent, local + Claude-CLI backends
./configure --enable-ai --enable-ai-tls   # + HTTPS backends (Claude API, remote OpenAI)
./configure --enable-ai --enable-ai-agent-host  # + host the Claude Code CLI (Alt-G h)
```

`--enable-ai-tls` links libtls (LibreSSL) or OpenSSL and is only needed to reach
**HTTPS** endpoints; Ollama and a localhost OpenAI-compatible server are plain
HTTP and need none of it. A plain `./configure` produces the classic editor with
zero AI code.

## Turn it on

Enable it at runtime in **Options > AI** (tick "Enable AI assistant"), or set
`XWPE_AI_ENABLE=1`. The same dialog picks the **backend**, the **model** (via the
scrollable picker), and the **permission policy**. "Save Options" persists them.

## Backends

| Backend | What it is | Needs |
|---------|------------|-------|
| **Ollama** (default) | a local model server at `http://localhost:11434` | `ollama serve` + a pulled model |
| **OpenAI-compatible** | llama.cpp / LM Studio / vLLM / OpenAI | an endpoint (and a key for OpenAI) |
| **Claude API** | Anthropic Messages API | `--enable-ai-tls` + `XWPE_AI_KEY` |
| **Claude CLI** | the `claude` command as a subprocess | a Claude Code login (`claude`, no key/TLS) |

The model list is queried live where possible (Ollama `/api/tags`, OpenAI
`/v1/models`) and shown in the picker; the Claude CLI offers its aliases
(default/sonnet/opus/haiku).

## Permission policy

The dial governs what the assistant may do without asking:

- **Ask each action** -- every file write and every command needs a `y`/`n`.
- **Auto-accept edits** -- writes apply automatically; commands still ask.
- **Auto** -- runs unattended; a checkpoint is taken first and every change is
  listed in the end-of-run changeset (keep all / revert all / file-by-file /
  commit). Nothing is ever written blind -- an AI edit is always previewed and
  is one `Ctrl-U` away from undone.

## Configuration / environment

All of these mirror the Options > AI dialog and are handy for scripting or a
fixed setup:

| Variable | Meaning |
|----------|---------|
| `XWPE_AI_ENABLE=1` | turn the assistant on |
| `XWPE_AI_BACKEND` | `ollama` / `openai` / `claude` / `claudecli` / `mock` |
| `XWPE_AI_ENDPOINT` | backend URL (e.g. `http://localhost:11434`) |
| `XWPE_AI_MODEL` | model name/alias |
| `XWPE_AI_KEY` | API key for the OpenAI/Claude HTTP backends |
| `XWPE_AI_POLICY` | `ask` / `edits` / `auto` |
| `XWPE_AI_TRACE=path` | append a diagnostic trace (prompt built, tool run, ...) |

## Privacy and safety

- Off by default and absent from a non-`--enable-ai` build.
- **Local-first**: Ollama and localhost OpenAI-compatible servers keep your code
  on your machine; no key, no network beyond localhost.
- HTTPS backends **verify the server certificate** (chain + hostname); there is
  no insecure bypass.
- Every AI edit is preview-then-apply and undoable; agent writes/commands are
  gated by the permission dial, and unattended runs are checkpointed and
  reviewable.
