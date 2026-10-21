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

# 3. run it, enable the assistant once (Options > AI, "Enable AI assistant"),
#    then use the Alt-G prefix
./wpe
```

That is all: with Ollama as the backend and no model chosen, xwpe auto-selects a
code model (preferring a larger one you have installed), so the first `Alt-G a`
just works. Point it elsewhere with `XWPE_AI_BACKEND` / `XWPE_AI_ENDPOINT` /
`XWPE_AI_MODEL` or in **Options > AI** (Alt-M lists the backend's models). Every
mode is handed the files you have open as context, so you never have to tell it
what you are working on.

`Alt-G` and the `Alt-G AI` entry on the bottom bar belong to programming mode
(`wpe`, `xwpe`): under `we` Alt-G is Go to Line and the bar keeps its classic
slot. Options > AI exists in both modes. The first time you open the menu, by
key or by a click on the bar entry, a one-time notice states the trust model;
Enter or `y` accepts, `n` or Esc declines, and a menu letter both accepts and
runs that action. The acceptance is kept in `$XDG_CONFIG_HOME/xwpe/ai_consented`
(else `~/.config/xwpe/ai_consented`); `XWPE_AI_ENABLE` or `XWPE_AI_CONSENTED=1`
skips the notice. With the menu open, a second `Alt-G` changes nothing, a main
menu's own letter (`Alt-F`, `Alt-E`, ...) opens that menu, and the arrow keys
step over the divider.

It has six main actions, all streaming and non-blocking (the editor stays live while
the model works):

- **Ask** (`Alt-G a`) -- a **chat** pane docked at the bottom. It can read files
  in the workspace to answer, and keeps context across the conversation.
  The pane takes the bottom half under the file windows; when the Messages
  window is open too (a build, a changeset review) the two share the bottom
  strip from the same row, Messages on the left and the pane on the right,
  whichever opened first. Window > Tile and Cascade arrange the file windows
  above the strip and leave the pane docked, as they leave Messages. Whatever
  width the pane has, its transcript is wrapped again to it (Messages opening
  beside it or closing, a resize), so a long reply reads whole over more rows
  instead of ending cut at the border, and a reply that wraps over several
  rows copies as the one line it is.
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
  buffer and file untouched. When the model wraps the file in markdown fences,
  only the content of the outermost fence pair becomes the file (language tag
  dropped, exactly one final newline); the prose around it, and any second
  fenced block, is shown in the pane under
  `[AI edit] the model's commentary (not part of the file):` instead of landing
  in the source. A reply with CRLF line endings is read as LF before the
  comparison, so only the real changes show and no carriage return reaches the
  buffer (a CRLF file itself is handled by the editor as always). Otherwise the
  proposed change appears in a **colored diff popup** inside the file window,
  one hunk at a time: the file first scrolls to the hunk's first line and puts
  the cursor on it (column kept), as Search and Goto Line do, and the popup
  opens one row below that line, above it when there is no room below, never on
  the window's status row or over the assistant pane. A row wider than the popup
  ends in `>` at the clip column, so a change past the edge is never two rows
  that read the same. Review keys are `y` or Enter to apply,
  `n` to skip, `a` to apply the rest (it stops at a hunk that deletes more lines
  than it adds and shows that one instead), `q` or Esc to cancel the review and
  keep the file exactly as it was before the review -- hunks already accepted
  are dropped too, and the pane says how many, so press `n` on the remaining
  hunks when you want to keep them -- and PgUp or PgDn to scroll a hunk taller
  than the popup. The close glyph on the popup's title row cancels like `q`.
  After Esc the keyboard is the editor's again: a letter typed next goes into
  the file as text, not to the review. Resizing the terminal while the popup is
  open lays it out again for the new size, next to the same hunk. Accepted
  hunks apply as **one** edit that a single **`Ctrl-U`** undoes, and the cursor
  rests on the first accepted hunk so it is on screen.
- **Multi-file edit** (`Alt-G f`) -- studies the workspace read-only, then
  proposes one coordinated change across several files. It has one name
  wherever you meet it: the menu row, the prompt (`Multi-file edit: task`),
  its status lines in the assistant pane, prefixed `[Multi-file edit]`, and the
  approval dialog's title.
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
  current C or C++ file) and, if it fails, loops -- read the errors, edit the sources,
  re-build -- until it passes. Same tools and permission level as the agent.

- **Complete at cursor** (`Alt-G c`) -- fills in code where the cursor is, at
  once: it opens no prompt. It
  sends the nearby context, the last 2000 characters before the cursor and the
  first 800 after it. The reply is shown first, in a one-hunk **Proposed
  completion** box with the same colours and keys as the Edit review: `y` or
  Enter inserts it, `n`, `q` or Esc leave the file untouched (the pane says
  `[AI complete] discarded`). An accepted suggestion goes in exactly as
  returned, cleaned only of markdown fences and with no line break added, so
  `foo(` completed with `bar)` reads `foo(bar)`; it is one edit that a single
  **`Ctrl-U`** undoes.

**While a request is out**, the menus, the cursor keys and your other windows
work as usual, and `Esc` cancels. This holds for a chat reply as it does for
an Edit or an agent run: the waiting line `AI: (gathering the answer...)`
carries the same spinner, seconds count and `(Esc cancels)`, and Esc while the
reply streams cancels that turn. Esc leaves the pane armed: the pane says
`[AI] cancelled`, its input line is ready for a follow-up, and a second Esc
returns to the file window. The text of the window that is waiting is
locked, and its status row says so: the reply is written back over the lines
that were sent, so a keystroke accepted meanwhile would be offered back to you
as a change to revert. Asking for a second action while one runs does not
refuse it: it is queued, the pane says so, and it starts by itself when the
running one ends. Edit, Complete and Multi-file edit work on a file window
only: asked from the pane itself, from Messages or from a read-only viewer they
send nothing and the pane says
`[AI] no file here - Edit, Complete and Multi-file work on a file window`.

**Pasting into the chat** is literal: a terminal paste of several lines goes
into the input row as it was copied, every line break a line break (a CR LF
ending counts as one), and nothing is sent until you press Enter after the
paste; `Ctrl-J` breaks the line by hand. The input holds 32768 bytes. A key
past that is refused and the status row reads `Input at its limit (32768 bytes)`
until the next key; a paste that crosses the limit keeps what fits and shows
the same line. A long paste is painted once, when it ends.

**Saving the transcript**: the pane is read-only, but `F2` or *File > Save*
with the pane focused saves its transcript. The first time it asks a file name
in the Save As dialog, titled `Save transcript`; the file is UTF-8 text, each
line as the pane shows it (`You: `, `AI: `, the bracketed machinery lines), one
per line and not wrapped. The status row says `saved <name>`, and a later `F2`
saves there again without asking until the pane is closed. *File > Save As*
asks every time; Esc in the dialog writes nothing. In the CUA key set the same
save is `Alt-F2`; in the default key set `Alt-F2` stays Window > Size/Move.

A pane line with less room than its text (the agent's or a Multi-file run's
task line, the echo of a task sent to the hosted engine) ends with `...`; the
request carries the whole text.

The `Alt-G` menu groups the **actions** above and, below a divider, the quick
**settings**: *Permissions* (shows and cycles ask/edits/auto), *Clear
conversation* (refused while a reply streams or a task runs: the bar says to
press Esc first), and *AI settings...* which opens **Options > AI** (the model,
the enable toggle and the full configuration live there). Edit, Multi-file and
the Agent ask for their instruction in a prompt box whose key hint sits above
the field: Enter or `Alt-S` sends, `Alt-M` opens the multi-line composer (a
scratch window titled with the task and `Esc = done, then Y sends, N cancels`,
which opens showing what you had already typed in the field),
`Alt-C` or Esc cancels, `Alt-P` brings the focus back to the field.
The field holds one line: a paste with line breaks opens the composer with the
pasted text in it. The field and the composer hold 32768 bytes, like the chat
input; the byte past that is refused with `Input at its limit (32768 bytes)`,
and the composer sends exactly the text it shows. A block pasted whole (Ctrl-V,
Ctrl-K C) can take the composer past the limit; Esc then asks, with the
composer still open, whether to go back and shorten it. Y, Esc and Cancel go back
to the text; only N gives it up. Nothing is sent cut.
The **Agent** can run on one of two **engines** (choose in Options > AI, only in
a `--enable-ai-agent-host` build): *Built-in* (the editor's own tool loop, works
with any backend) or *Claude Code* (runs the real `claude` CLI as a persistent
session using **its own** tools, with xwpe as the front-end). With the Claude
Code engine, `Alt-G g` streams the agent's answer and tool activity into the
pane, reloads any file it edits into that file's window (one `Ctrl-U` reverts),
and keeps the session live across turns -- `Alt-G g` feeds the next turn, an
empty prompt (or `Esc` at that prompt) ends it and offers a reviewable/revertible
changeset. `Esc` while a turn is streaming interrupts that turn instead, as it
cancels a built-in run: the pane says `[agent] interrupting ...`, the engine gets
its interrupt request, and the turn ends with `[agent] turn interrupted` while the
session stays alive; an engine that has not ended the turn within `AIToolTimeout`
seconds is ended as if it had exited. The permission dial gates its tools **per
tool**, classed like the editor's own: *Ask* pops a `y`/`n` for each gated tool
call; *Edits* auto-accepts the file-editing tools (`Write`, `Edit`, `MultiEdit`,
`NotebookEdit`) and asks for `Bash`, `WebFetch`, `WebSearch` and any other tool;
*Auto* runs unattended (a checkpoint is taken either way, announced with the same
`[agent] checkpoint taken` line as the built-in loop). A refusal shows
`[agent] denied: <tool> <arg>` and the CLI reads "Denied by the user". It is
built as a generic "host an agent CLI that speaks stream-json" (Claude Code
first; others as adapters later).

`Alt-G f` runs a **Multi-file edit**: it studies the workspace read-only, then
proposes a coordinated change across several files and shows it as a **modal
approval dialog** titled `Multi-file edit - N files`. Each file is listed with
its `(+added -removed)` counts taken
against the buffer as it is now, unsaved edits included; `[new file]` marks a
file that does not exist yet; and a `!` mark with the words
`outside the workspace` flags a path the model wants to write beyond the
project's (or the current file's) directory. Such a file is not refused, but you
see it before approving and the pane names it when it is applied. **Enter/A**
applies all, **F** reviews file-by-file (each in the diff box), and **Q/Esc**
cancels. Nothing is written until you approve. (This
is what some tools call a "workspace edit"; it is *not* an agent plan mode.)
After the accept each changed file is open in its own window, and the
`Messages` window lists one row per file with its first changed line
(`src/parser.c:12: [Multi-file edit] changed`). **Alt-T** / **Alt-V** step
through those rows as they step through compiler errors. There is no keep or
revert step: the edits live in the buffers until you save each window, and
**Ctrl-U** in a window takes that file's edit back.
`Alt-G m` opens the scrollable **model picker**.

The pane keeps its `> ` prompt docked at the bottom while you type. To read back a
long conversation, **PgUp/PgDn** page through the transcript, the **arrow keys**
scroll it a line at a time and the **mouse wheel** three lines a step. The view
then stays where you left it while a reply lands, also after you have clicked
into the file window to work there; typing any character in the input row
returns the view to the prompt so the next key lands in the input. The pane's
first lines are a header,
`[AI: <backend> | model: <model> | policy: <dial>]` (`| web` is appended
while web access is on), and two hint lines; when
the backend, the model or the permission dial change while the chat is open, a
new header line with the current values precedes the next turn. Ok in
Options > AI changes only what you touched: on a backend the Backend radio does
not list (one set by `XWPE_AI_BACKEND`), the radio marks no row and Ok keeps
that backend and its model. A question
sent while a reply is still streaming is queued, the pane says so, and it
starts when the running turn ends.

**Web access.** The chat can search the web and read pages with the agent's
`web_search` and `web_fetch`, behind one saved switch, `AIWeb`, off until you
say yes. The first turn that wants the web while the switch was never answered
shows a notice in the pane -- your query goes to the engine set by
`AISearchURL` (named there), the pages it picks are fetched, nothing else
leaves the machine, the model stays where it is -- and the line
`Enter / y = enable web access      n / Esc = not now`. `y` writes
`AIWeb : on` to xwperc at once and the turn goes on; `n` answers without the
web and asks again next time. The permanent off is the **Web access** box on
the first row of Options > AI (`Alt-W`, beside *Enable AI assistant*). The box
shows the state in force, so it tells you what the assistant will do; Ok saves
what the box says, except that a one-session `XWPE_AI_WEB` is not turned into
a saved setting by opening the dialog, and clearing a box you were never asked
about leaves the question still to be asked. With the web on, every search and page goes through the
permission dial like the agent's (`APPROVE?  <query>` for a search and
`APPROVE?  <url>` for a page under *ask* and *edits*, unasked under *auto*;
the hosted engine's own request reads `APPROVE?  WebFetch <url>`), the
transcript shows the query, `[agent] search engine:
<host>` and every URL read, the answer ends with a `sources:` line, and until
the switch says on the chat offers no web tool, names them as the Agent's, and
the agent's web tools refuse with a line naming the switch. The chat can ask
its notice because a question is waiting for an answer; an agent task runs on
its own, so there an unanswered switch counts as off rather than prompting
mid-task. A backend that browses for itself is reached two ways, because the
editor does not dispatch its tools: a deny list on the command line the editor
builds, and the per-tool question that CLI sends back. The Claude CLI backend
gets the list only, since `claude -p` cannot be asked anything once it runs:
`WebSearch` and `WebFetch` are pre-approved while the web is on and denied
while it is off. The hosted `claude-code` engine (`AIAgentEngine : claude-code`,
Alt-G g) gets both: the list while the web is off, and the question at the
*ask* and *edits* dials, so with the web on you still see a box for a fetch.
The *auto* dial sends no question, so there the list is all that holds and the
editor cannot promise the CLI honours it. Move off the *auto* dial if you need
the web demonstrably shut.

One case the switch cannot reach: a hosted engine named in Options
(`AIHostCommand`, for instance `aider`) is run through the shell as you wrote
it, so the editor adds no flags to it. The switch then holds only through the
per-tool question, which arrives only if that CLI asks. At the *auto* dial it
usually does not, and the pane says so when the session opens.
A tool line the chat
cannot run (a web tool with the switch off, an invented name) is answered, not
dropped: the pane says which tool was not run and why, and the answer follows.
`XWPE_AI_WEB=0|1` overrides the switch for one session.

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
./configure --enable-ai --enable-ai-agent-host  # + host the Claude Code CLI (Alt-G g, engine setting)
```

`--enable-ai-tls` links libtls (LibreSSL) or OpenSSL and is only needed to reach
**HTTPS** endpoints; Ollama and a localhost OpenAI-compatible server are plain
HTTP and need none of it. A plain `./configure` produces the classic editor with
zero AI code.

## Turn it on

Enable it at runtime in **Options > AI** (tick "Enable AI assistant"), or set
`XWPE_AI_ENABLE=1`. The same dialog picks the **backend**, the **model** (via the
scrollable picker), and the **permission policy**. "Save Options" persists them.
Pressing Alt-G while the assistant is off writes a line in the AI pane that
names this dialog.

The AI pane colours each line by who wrote it, from the colour scheme: your
own "You:" lines white, the reply in the editor text colour, the machinery
lines ("[agent] ...", "[AI edit] ...", the header) light grey, a line that
waits for a key (">> ...") white on green, an error light red.  The entries
are the "AI pane Colors" section of Options > Adjust Colors (keys `aiu`,
`aim`, `aia`, `aie` in xwperc's colour section).  The chat sends the file it
was opened from (and its marked block) as context, never its own transcript.

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

**Waiting for a server.** Before a request the editor checks that something
listens at the endpoint: a TCP connect bounded at 3 s, the one wait it still
sits through (a server that drops the connection attempt holds it that long;
one that refuses answers at once). From there on it never waits inside a
connection. A chat, Edit, completion or agent turn opens its connection on the editor's
loop: the connect, the TLS handshake and the first byte of the answer share
one deadline, 120 s by default (`XWPE_AI_TIMEOUT`). Meanwhile the waiting line
shows, the editor takes keys and Esc cancels. A server that lets the deadline
pass ends the request with `no answer from host:port within N s`. The model
list (Alt-M) has its own 5 s deadline; a box over the dialog says
`listing models from host:port ... (Esc cancels)`, and Esc abandons it with
`listing cancelled`. Host names are looked up before the deadline starts: a
numeric address or `localhost` resolves at once, other names are bounded by
the system resolver.

**How errors read.** Every failure names host:port and the reason:

- `cannot connect to host:port (Connection refused)`: nothing listens there.
- `TLS handshake/verify failed with host:port: <reason>`: the TLS library's
  reason. Only a certificate that could not be verified adds the advice to set
  the server's CA file (Options > AI, Alt-C).
- `HTTP 401 from host:port: <reason>`: a refused model list, with the reason
  from the JSON body or the status line. With Options > AI open it shows in a
  message box over the dialog; Esc closes it and the dialog stays as it was.
- An error sent after part of a reply keeps the text and adds
  `AI: error: <reason>` in the error colour; the session keeps the partial
  text with a note that an error cut it.
- A model saved for another backend is not sent; one line says
  `[AI] model <x> is not <backend>'s; using <y>`.

## Permission policy

The dial governs what the assistant may do without asking. Its levels are
**ask**, **edits** and **auto** (the radio buttons of Options > AI spell them
out as "Ask each action", "Auto-accept edits" and "Auto (skip asks)"):

- **ask** -- every file write and every command needs a `y`/`n`.
- **edits** -- writes apply automatically; commands still ask.
- **auto** -- runs unattended; a checkpoint is taken first and every change is
  listed in the end-of-run changeset (keep all / revert all / file-by-file /
  commit). Each row names the file by its path under the workspace root and
  the first line the hunk changed, where `Alt-T` / `Alt-V` then jump; `Esc`
  keeps, at the top of the review and inside the file-by-file questions,
  where it keeps the files not yet asked about; `c` commits the listed files
  and nothing else (the keys are two Messages rows, and `c` is shown only in a
  git workspace), a commit a hook refuses leaves the files staged and the pane
  says so, and a file that could not be put back is named in the
  pane with the reason. An Edit, a completion or a Multi-file change is always previewed
  before it lands. An agent write under *edits* or *auto* is written first and
  reviewed after, in that changeset, and stays one `Ctrl-U` away from undone in
  the window that shows the file.

The dial is read live. Raising it with `Alt-G y` while a run waits for the
model applies to the next tool, and the first write that follows takes the
checkpoint the launch would have taken (the pane shows the same
`[agent] checkpoint taken` line), so a run started under *ask* and raised to
*auto* still ends in the review. A checkpoint that could hold no copy of your
files is not taken; the pane says what failed and then `[AI] no checkpoint:
this run cannot be reverted from the review`.

Whatever the dial, the argument of `list_dir`, `grep` and `glob` is passed to
the shell as a single word: a pattern with spaces is one pattern, and no
character in it is a command. A write to a file outside the working tree (an
absolute path beyond it, or one that climbs out with `..`) always asks, under
*edits* and *auto* too; the write box carries the `!` mark and the words
`outside the workspace`, and the pane names the file again once it is written.

Whatever the dial says, a command the assistant runs **cannot reach your
screen**: it runs in a session of its own with no terminal attached, so its
output comes back only as text in the assistant pane. A command that writes to
`/dev/tty` used to paint straight over the editor -- the screen went black and
the command's lines scrolled across the file until the next repaint. The same
holds for the external agent CLI behind the *Claude Code* engine and for the
post-edit hook.

While a command or a web tool runs the editor reads no keys, so the pane's last
line says `[agent] running run_command ... (result follows)` (or `web_search`,
`web_fetch`) for the whole run. A command gets `AIToolTimeout` seconds (default
60, `XWPE_AI_TOOL_TIMEOUT` for one session); past that it is ended (its whole
process group, TERM then KILL), the pane says `run_command timed out after N s`
and the model reads `TOOL ERROR: timed out after N s`. The bound reaches the
command's process group; a command that detaches itself from it (`setsid`,
`nohup ... &`) is beyond it and runs on. The read-only tools and the post-edit
hook share the limit. A command's output or a file read with
`read_file` is cut at 6000 bytes and then ends with the line
`(output cut at 6000 bytes)`, so the model asks for less instead of taking the
beginning for the whole.

Every denial is visible: answering a request with `n` or `Esc` puts
`[agent] denied: <tool> <arg>` in the pane (and in the trace) for a command, a
web tool and a write alike; the model is told `(denied by user)` and the run
goes on. A write that cannot be made (its directory does not exist) is told the
same way: `[agent] write failed: <path>` in the pane, nothing created, the model
reads `(write failed)` and the run goes on.

A `write_file` body is the file, written verbatim: every line with its newline,
the last one included, so a file the agent writes ends with a newline as a saved
file does, and CRLF content lands as sent. `apply_patch` edits that text in
place and keeps the same ending. A reply with neither a `TOOL` line nor the
closing `DONE` is taken as the answer: shown whole, the run ends and the
follow-up prompt opens under it.

A turn that fails is reported as a failure, never as a finished task. A server
that answers an error shows `[agent] working: backend error: <message>` with the
server's own words; the run stops, no tool runs, the session is saved and the
review opens if files were written. A connection that dies (closed with no
bytes, an answer that is not HTTP, a reply cut before its end) shows
`[AI] transport error`: the run stops, nothing is applied, and neither
`[agent] done` nor the follow-up prompt appears. Chat, Edit and Complete report
the same failure under their own prefix.

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
| `XWPE_AI_WEB` | `1` / `0`: web access for one session (`AIWeb`, asked once at first need, off by default). Governs the chat's and the agent's web tools, and what the browsing backends are launched with (see "Web access" above) |
| `XWPE_AI_TOOL_TIMEOUT` | seconds a tool command may run before it is ended (`AIToolTimeout`, default 60) |
| `XWPE_AI_TIMEOUT` | seconds a request waits for the server: connect, TLS handshake and first byte (default 120) |
| `XWPE_AI_PROJECT_INSTRUCTIONS` | where the project instructions file is folded in: `agent` (default), `all` or `off` (`AIProjectInstructions`) |
| `XWPE_AI_PROJECT_INSTRUCTIONS_MAX` | bytes of the project instructions file the prompt carries (`AIProjectInstructionsMax`, default 4095, at most 65536) |
| `XWPE_AI_TRACE=path` | append a diagnostic trace (prompt built, tool run, ...) |

**Post-edit hook.** `AIEditHook` in the `[Programming]` section of `xwperc`
(e.g. `clang-format -i`) runs on every file the assistant wrote, patched or
edited, with the path appended and quoted. After an agent write it runs on the
file on disk; after an accepted Edit, completion or multi-file change it runs
on a temporary copy beside the file, so nothing reaches the disk before you
save. Either way its rewrite joins the change's undo step: one `Ctrl-U` reverts
the change and the reformatting together. A hook that exits non-zero is
reported in the pane with its exit status, and the buffer is left as the
assistant left it. The hook shares the tool time limit (`AIToolTimeout`): one
that hangs is ended after that many seconds and reported as failed.

**Key file.** Every write of a key typed in Options > AI leaves the key file
readable by you alone (mode 0600), also when it already existed with a wider
mode.

**The current file.** A chat sends the current file with the question. A file
longer than the room the prompt has for it is cut at a whole line: the prompt
ends with `(current file cut at line N of M)` and the pane says
`[AI] <file>: N of M lines sent`.

**Build & fix without a build command.** With no makefile and no syntax check
for the file's type (C and C++ have one), Alt-G b says
`[fix-build] no build command for <file>: no makefile here, and only C and C++
have a syntax check` and runs nothing.

**Project instructions.** A `.xwpe-ai.md` (or, when it is absent, an
`AGENTS.md`) in the working directory is folded into the prompt under a
`PROJECT INSTRUCTIONS` heading, so a repository's standing rules are followed
without restating them. The editor's own file wins, so a repository whose
`AGENTS.md` serves other coding agents can still give this editor its own
instructions. Where the block goes is set by `AIProjectInstructions`
(`XWPE_AI_PROJECT_INSTRUCTIONS`): under `agent`, the default, the Agent, the
Multi-file edit and Build & fix carry it, and the chat carries it from the
turn in which it first uses a tool, for the rest of the session; a plain Ask
does not, because the file exists for work that acts on the repository and on
the current file it is context spent for nothing, a tenth of a local model's
window. `all` folds it into every chat turn as well; `off` never folds it.
Edit and Complete never carry it: their prompts ask for file text only. The
pane says so: an agent run shows
`[agent] project instructions: .xwpe-ai.md (N bytes)` right after its task
line, and the chat shows the same line once per session, on the first turn
that carries the block. The prompt carries at most `AIProjectInstructionsMax`
bytes of the file (`XWPE_AI_PROJECT_INSTRUCTIONS_MAX`; default 4095, at most
65536); a longer one is cut and the line names the key:
`(N bytes, cut at M; AIProjectInstructionsMax raises it)`. The instructions
govern how the tools are used, never
which tools exist: the prompt states, right after the block, that the tool list
above stands, so a rule written for other coding agents ("do not open network
connections") does not make the model refuse the editor's own web search.

## Privacy and safety

- Off by default and absent from a non-`--enable-ai` build.
- **Local-first**: Ollama and localhost OpenAI-compatible servers keep your code
  on your machine; no key, no network beyond localhost.
- HTTPS backends **verify the server certificate** (chain + hostname); there is
  no insecure bypass.
- Every AI edit is preview-then-apply and undoable; agent writes/commands are
  gated by the permission dial, and unattended runs are checkpointed and
  reviewable.
