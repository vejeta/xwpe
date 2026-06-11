# xwpe LSP demos

Short terminal recordings of xwpe's Metals/LSP features, for the README, the
user-facing [`docs/LSP.md`](../LSP.md) guide and the tutorial chapter.  Each GIF
is generated from a `.tape` script with
[VHS](https://github.com/charmbracelet/vhs), so they are reproducible rather
than hand-captured (see *How these are made* below).

## Watch

### The Metals action menu (discoverable, Borland-style)
`Alt-Q ?` (or click `Metals` on the status bar) unfolds the full action list
upward from the bar -- every command with its `Alt-Q` accelerator.

![Metals menu](gifs/menu.gif)

### Go to definition
With the cursor on a `describe(...)` call, `Alt-Q D` leaps it up to the
`def describe` that defines it -- the iconic IDE jump, over LSP.

![Go to definition](gifs/definition.gif)

### Semantic colours (server-driven highlighting)
`Alt-Q M` repaints the file from Metals' semantic tokens: types, function
calls, parameters and variables get their own colours -- distinctions the
regex lexer cannot make.  Toggle it off to return to the classic colours.

![Semantic colours](gifs/semantic.gif)

### Code action + Undo/Redo
`Alt-Q A` on the positional call `greet("Ada", 42)` offers "Convert to named
arguments".  Metals ships that refactor *unresolved* (only a `data` field), so
xwpe does a `codeAction/resolve` round-trip to fetch the edit, then rewrites the
call to `greet(name = "Ada", age = 42)`.  The point of the clip: the server's
rewrite is an ordinary edit -- `Ctrl-U` undoes it, `Ctrl-R` redoes it.

![Code action with Undo/Redo](gifs/codeaction.gif)

## How these are made

The GIFs are **reproducible**, not hand-captured.  Each one is a short
[VHS](https://github.com/charmbracelet/vhs) `.tape` script -- a little program
that types keys and waits -- which VHS plays back against a headless terminal
([`ttyd`](https://github.com/tsl0922/ttyd)) and renders to a GIF (encoded with
`ffmpeg`).  Because every keystroke is scripted, re-recording after a UI change
is one command and the clips never silently drift from how the editor actually
behaves.

The neat trick for the server-backed clips: the ~1-minute Metals cold start
(JVM boot, build import, first index) runs inside a `Hide` ... `Show` block, so
the recording *does* the real work but the GIF jumps straight to the action.
A tape reads like the demo itself -- here is the spine of `codeaction.tape`:

```
Hide                                  # everything until Show is run but not filmed
Type "cd ${DEMO} && ${WPE} actions.scala"
Enter
Alt+q                                 # Alt-Q E starts Metals (diagnostics)...
Type "e"
Enter
Sleep 95s                             # ...let it index, off-camera
Show                                  # filming starts here
Down@60ms 27                          # cursor down to the greet(...) call
Right@45ms 10
Alt+q                                 # Alt-Q A -> code-action popup
Type "a"
Down@350ms 3                          # select "Convert to named arguments"
Enter                                 # apply (codeAction/resolve + rewrite)
Ctrl+u                                # undo the server's rewrite
Ctrl+r                                # redo it
```

## Regenerate

```sh
# from the repo root, with the editor built (make) and the demo tools on PATH
WPE=./wpe DEMO=../scala-demo docs/demos/record.sh          # all tapes
WPE=./wpe DEMO=../scala-demo docs/demos/record.sh menu     # just one
```

`record.sh` lists its requirements (vhs, ttyd, ffmpeg; metals + scala-cli and
an indexed Scala workspace for the server-backed tapes).  The Metals tapes
`Hide` the ~1 min cold-start/indexing wait so the GIF jumps to the action.

## Tapes

| Tape | Shows | Needs Metals |
|------|-------|--------------|
| `tapes/menu.tape`       | the action menu / discoverability  | no  |
| `tapes/definition.tape` | go to definition (cursor jump)     | yes |
| `tapes/semantic.tape`   | semantic colours before/after      | yes |
| `tapes/codeaction.tape` | code action + Undo/Redo            | yes |

See `SCENARIOS.md` for the full key-sequence scripts of every headline
feature (the source material for additional tapes and the reel).
