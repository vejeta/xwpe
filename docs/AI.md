# The AI assistant in xwpe (experimental)

xwpe can host a local-first **AI assistant** behind the **`Alt-G`** prefix, the
same way the `Alt-Q` prefix drives the [language server](LSP.md). It is
**opt-in and off by default** -- a stock build contains none of it, so a distro
(or anyone who does not want AI in their editor) gets the exact classic editor.

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
- **Agent** (`Alt-G g`) -- a tool-using agent: it lists/reads/greps files and
  proposes `write_file` / `run_command` steps. Every mutating step is shown for
  approval (a write is previewed as a diff); an unattended run takes a checkpoint
  first and offers a reviewable changeset at the end. When it finishes you can
  type a **follow-up** right in the pane.
- **Fix the build** (`Alt-G b`) -- runs the build (`make` when a makefile is
  present, otherwise a syntax-only compile of the current file) and, if it fails,
  loops the agent: read the errors, edit the sources, re-build, repeat until it
  passes. It uses the same tools and permission dial as the agent.

`Alt-G p` runs a multi-file **Plan**, and `Alt-G m` opens the scrollable
**model picker**.

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
