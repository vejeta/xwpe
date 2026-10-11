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

It has six main actions, all streaming and non-blocking (the editor stays live while
the model works):

- **Ask** (`Alt-G a`) -- a **chat** pane docked at the bottom. It can read files
  in the workspace to answer, and keeps context across the conversation.
- **Edit** (`Alt-G e`) -- rewrite code from an instruction. It sends a marked
  block when one is active, the whole file when it fits the context limit, or a
  consecutive window around the cursor when the file is larger. For that large
  file case, the pane reports the sent range, for example
  `[AI edit] lines 431-770 of 1200 sent`, and the request includes that same
  range header. The reply replaces exactly the sent text and leaves the rest of
  the file untouched. A reply that could not be the new file -- prose, blank, an
  empty fence pair, or so much shorter than the file that it reads as a
  truncation -- is questioned first in a box naming both line counts
  (`reply has 1 line, file has 3: review anyway?`); No or Esc ends the edit with
  buffer and file untouched. Otherwise the proposed change appears in a
  **colored diff popup** next to the changed lines, below when there is room and
  above otherwise, one hunk at a time. Review keys are `y` or Enter to apply,
  `n` to skip, `a` to apply the rest (it stops at a hunk that deletes more lines
  than it adds and shows that one instead), `q` or Esc to cancel the review and
  keep the file exactly as it was before the review -- hunks already accepted
  are dropped too, and the pane says how many, so press `n` on the remaining
  hunks when you want to keep them -- and PgUp or PgDn to scroll a hunk taller
  than the popup. Accepted hunks apply as **one** edit that a single
  **`Ctrl-U`** undoes.
- **Multi-file edit** (`Alt-G f`) -- studies the workspace read-only, then
  proposes one coordinated change across several files. Its status lines in the
  assistant pane are prefixed `[plan]`.
- **Agent (tools)** (`Alt-G g`) -- a tool-using agent: it lists/reads/greps files
  and proposes `write_file` / `run_command` steps **one at a time**, each shown
  for approval (a write is previewed as a diff); an unattended run takes a
  checkpoint first and offers a reviewable changeset at the end. A write to a
  file you have open reloads it as one **`Ctrl-U`** step, hook reformatting
  included; the pane says `Ctrl-U to undo (buffer only)` because the disk keeps
  the agent's text until you save or revert it in the changeset. When it finishes
  you can type a **follow-up** right in the pane.  (Multi-file edit vs Agent:
  Multi-file proposes one coordinated change and asks once; the Agent works
  step-by-step, reading and acting as it goes.)
- **Build & fix (agent)** (`Alt-G b`) -- a specialized agent: runs the build
  (`make` when a makefile is present, otherwise a syntax-only compile of the
  current file) and, if it fails, loops -- read the errors, edit the sources,
  re-build -- until it passes. Same tools and permission level as the agent.

- **Complete at cursor** (`Alt-G c`) -- fills in code where the cursor is. It
  sends the nearby context, the last 2000 characters before the cursor and the
  first 800 after it. The reply is shown first, in a one-hunk **Proposed
  completion** box with the same colours and keys as the Edit review: `y` or
  Enter inserts it, `n`, `q` or Esc leave the file untouched (the pane says
  `[AI complete] discarded`). An accepted suggestion goes in exactly as
  returned, cleaned only of markdown fences and with no line break added, so
  `foo(` completed with `bar)` reads `foo(bar)`; it is one edit that a single
  **`Ctrl-U`** undoes.

**While a request is out**, the menus, the cursor keys and your other windows
work as usual, and `Esc` cancels. The text of the window that is waiting is
locked, and its status row says so: the reply is written back over the lines
that were sent, so a keystroke accepted meanwhile would be offered back to you
as a change to revert. Asking for a second action while one runs does not
refuse it: it is queued, the pane says so, and it starts by itself when the
running one ends.

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
approval dialog**. **Enter/A** applies all, **F** reviews file-by-file (each in
the diff box), and **Q/Esc** cancels. Nothing is written until you approve. (This
is what some tools call a "workspace edit"; it is *not* an agent plan mode.)
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

Whatever the dial says, a command the assistant runs **cannot reach your
screen**: it runs in a session of its own with no terminal attached, so its
output comes back only as text in the assistant pane. A command that writes to
`/dev/tty` used to paint straight over the editor -- the screen went black and
the command's lines scrolled across the file until the next repaint. The same
holds for the external agent CLI behind the *Claude Code* engine and for the
post-edit hook.

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

**Post-edit hook.** `AIEditHook` in the `[Programming]` section of `xwperc`
(e.g. `clang-format -i`) runs on every file the assistant wrote, patched or
edited, with the path appended and quoted. After an agent write it runs on the
file on disk; after an accepted Edit, completion or multi-file change it runs
on a temporary copy beside the file, so nothing reaches the disk before you
save. Either way its rewrite joins the change's undo step: one `Ctrl-U` reverts
the change and the reformatting together. A hook that exits non-zero is
reported in the pane with its exit status, and the buffer is left as the
assistant left it.

## Privacy and safety

- Off by default and absent from a non-`--enable-ai` build.
- **Local-first**: Ollama and localhost OpenAI-compatible servers keep your code
  on your machine; no key, no network beyond localhost.
- HTTPS backends **verify the server certificate** (chain + hostname); there is
  no insecure bypass.
- Every AI edit is preview-then-apply and undoable; agent writes/commands are
  gated by the permission dial, and unattended runs are checkpointed and
  reviewable.
