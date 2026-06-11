# Scala / Metals language-server demo for xwpe

A tiny, deliberately-commented Scala 3 project that exercises **every** xwpe
language-server action.  It is the fastest way to see (and test) the LSP support
end to end.

## Requirements

- `scala-cli`  -- the build tool (`cs install scala-cli`)
- `metals`     -- the Scala language server (`cs install metals`)
- A JDK that the Scala 3 compiler supports (an **LTS**: 17 or 21).  The system
  default may be too new; `project.scala` pins a Temurin 21 for the build, and
  xwpe auto-pins `JAVA_HOME` for Metals when the default JDK is too new.

See @ref{Language Server} in the manual for the full setup notes.

## Run it

```sh
cd docs/examples/scala-lsp
wpe main.scala            # programming mode (xwpe in X11; wpe in a terminal)
```

The **first** language-server action starts Metals: a JVM boots and imports the
build, which takes roughly a minute on a cold cache.  It is ready once the
inline marks settle.  A couple of *unused import* warnings are expected -- they
are deliberate (see `actions.scala`) so Alt-Q E has something to mark and
Alt-Q A has an "Organize imports" action to offer.

## What to try

Each line in the three files (`main.scala`, `shapes.scala`, `actions.scala`)
carries an inline comment telling you which action to try right there -- just
read down the code.  For reference:

| Key       | Action            | Where to try it (see the inline comment) |
|-----------|-------------------|------------------------------------------|
| `Alt-Q E` | Diagnostics       | anywhere (also marks problems inline)    |
| `Alt-Q D` | Definition        | on `Shape` in `main.scala`               |
| `Alt-Q I` | Implementation    | on `area` in `shapes.scala`              |
| `Alt-Q T` | Type              | on `area` in `main`                      |
| `Alt-Q H` | Hover             | on `Pi` (`shapes.scala`), any identifier |
| `Alt-Q C` | Complete          | after `shapes.`                          |
| `Alt-Q R` | References        | on `Shape` / on `total`                  |
| `Alt-Q B` | Incoming calls    | on `total` in `main.scala`               |
| `Alt-Q G` | Outgoing calls    | on `describe` in `main.scala`            |
| `Alt-Q K` | Supertypes        | on `Circle` in `shapes.scala`            |
| `Alt-Q J` | Subtypes          | on `Shape` in `shapes.scala`             |
| `Alt-Q U` | Uses (highlight)  | on `name` in `shapes.scala`              |
| `Alt-Q V` | Expand selection  | on a token in `main` (press again widens)|
| `Alt-Q Y` | Inlay hints       | toggle: the un-annotated `val`s get `: T`|
| `Alt-Q M` | Semantic colours  | toggle, anywhere (server-driven highlight)|
| `Alt-Q O` | Outline           | either file                              |
| `Alt-Q L` | code Lenses       | above `main`                             |
| `Alt-Q W` | Workspace symbol  | type `Shape` or `Color`                  |
| `Alt-Q A` | code Actions      | `actions.scala` -- see "Code actions" below |
| `Alt-Q S` | Signature help    | inside `describe(...)`                    |
| `Alt-Q N` | reName            | on a local `val` (e.g. `names`)          |
| `Alt-Q F` | Format            | the whole file                           |

`Alt-Q` opens the action menu; `Alt-Q <letter>` runs one directly.  To actually
*run* the program, start the debugger with `Ctrl-G T`.

## Code actions (Alt-Q A)

`actions.scala` is a dedicated playground for refactors and quick-fixes.  Put the
cursor on a marked spot, run `Alt-Q A`, pick an entry from the popup; the buffer
is rewritten in place (`F2` saves).  It exercises the three ways a server
delivers an action -- xwpe applies all of them:

- a **direct edit** -- e.g. `Alt-Q A` on the `"hi " + name` string offers
  "Convert to interpolation string" -> `s"hi $name"`.
- a **server command** -- `Alt-Q A` on an unused `import` offers "Organize
  imports" / "Remove unused" (run via `workspace/executeCommand`, the result
  applied via `workspace/applyEdit`).
- an **unresolved action** -- `Alt-Q A` on the call `greet("Ada", 42)` offers
  "Convert to named arguments" -> `greet(name = "Ada", age = 42)`.  Metals
  ships its refactors with only a `data` field, so xwpe runs a
  `codeAction/resolve` round-trip to fetch the edit before applying it.
