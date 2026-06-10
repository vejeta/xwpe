# LSP demo scenarios

Key-sequence scripts for the headline xwpe LSP/Metals features, captured at
implementation time so the demo reel (VHS .tape rendering -> GIFs for the
README and tutorials) is just an assembly pass.  See task #211.

All scenarios run in the bundled example project (open it from that dir):

    cd docs/examples/scala-lsp && wpe main.scala

The project pins the LTS Scala 3.3 PC + Temurin 21 (`project.scala`), so hover/
completion/inlay are stable.  Metals must be installed (`cs install metals`).

Notation: `Alt-Q X` = press Alt-Q then X.  `^K` = Ctrl-K.  Waits marked `(~)`.

---

## 1. Async cold start (the headliner -- show the editor stays alive)
The most impressive single demo: prove the editor never freezes while Metals
boots.  Open `main.scala`, press `Alt-Q E`, then IMMEDIATELY keep typing /
arrow around / open the File menu while "Starting language server (the editor
stays responsive)..." shows and `LSP: Indexing` / `LSP: Compiling` stream in
Messages.  When `Language server ready` appears, `Alt-Q E` again -> diagnostics.
- Keys: `Alt-Q E`, then arrows/typing for ~30s, then `Alt-Q E`.
- Show: cursor moving + menus opening DURING the boot; status streaming live.

## 2. Hover (type + docs)
Cursor on `describe` (or any symbol) -> `Alt-Q H` -> popup with the type/docs.

## 3. Go to definition / implementation / type
- `Alt-Q D` on `Shape` in `describe(s: Shape)` -> jumps to shapes.scala.
- `Alt-Q I` on `area` -> an implementation.  `Alt-Q T` on a typed val -> its type.

## 4. Completion
Type `shapes.` then `Alt-Q C` -> member list -> arrows + Enter inserts.

## 5. References / document highlight
- `Alt-Q R` on `total` -> every use listed in Messages.
- `Alt-Q U` on a symbol -> all occurrences highlighted in the file.

## 6. Call hierarchy (vim-mnemonic pair)
- `Alt-Q B` on `total` -> incoming calls (who calls it).
- `Alt-Q G` on `main` -> outgoing calls (what it calls).

## 7. Type hierarchy (vi K=up / J=down)
- `Alt-Q K` on `Hello` -> supertypes (Greeter).
- `Alt-Q J` on `Greeter` -> subtypes (Hello).

## 8. Expand selection (structural smart-select)
Cursor on a token -> `Alt-Q V` selects it; press `Alt-Q V` again and again to
widen: token -> expression -> argument list -> call -> statement -> block.

## 9. Range formatting (context-sensitive)
Mark a block (`Alt-Q V` a few times, or `^K B` / `^K K`) -> `Alt-Q F` formats
JUST that range.  With nothing marked, `Alt-Q F` formats the whole file.

## 10. Inlay hints toggle
`Alt-Q Y` -> inferred types appear dim at end-of-line on un-annotated vals;
`Alt-Q Y` again hides them.

## 11. Live diagnostics as you type
Introduce a type error (e.g. `val x: Int = "no"`); the `LSP: N error(s)` count
updates live in Messages and the offending span is recolored inline.

## 12. The Metals menu (discoverable, Borland-style)
Click the `Metals` entry on the bottom bar (or `Alt-Q ?`) -> the action dropdown
unfolds upward listing every command with its `Alt-Q X` accelerator.

---

## Reel order suggestion (README GIF)
1 (async start) -> 12 (menu) -> 2/3 (hover+definition) -> 6/7 (hierarchies) ->
8 (expand) -> 10 (inlay) -> 11 (live diagnostics).  ~60-90s total.
