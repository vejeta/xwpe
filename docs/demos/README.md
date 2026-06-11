# xwpe LSP demos

Short terminal recordings of xwpe's Metals/LSP features, for the README and
the tutorial chapter.  Each GIF is generated from a `.tape` script with
[VHS](https://github.com/charmbracelet/vhs), so they are reproducible rather
than hand-captured.

## Watch

### The Metals action menu (discoverable, Borland-style)
`Alt-Q ?` (or click `Metals` on the status bar) unfolds the full action list
upward from the bar -- every command with its `Alt-Q` accelerator.

![Metals menu](gifs/menu.gif)

### Semantic colours (server-driven highlighting)
`Alt-Q M` repaints the file from Metals' semantic tokens: types, function
calls, parameters and variables get their own colours -- distinctions the
regex lexer cannot make.  Toggle it off to return to the classic colours.

![Semantic colours](gifs/semantic.gif)

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
| `tapes/menu.tape`     | the action menu / discoverability | no  |
| `tapes/semantic.tape` | semantic colours before/after      | yes |

See `SCENARIOS.md` for the full key-sequence scripts of every headline
feature (the source material for additional tapes and the reel).
