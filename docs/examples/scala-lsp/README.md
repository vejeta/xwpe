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
build, which takes roughly a minute on a cold cache.  When the Messages window
shows `LSP: no problems.` it is ready.

## What to try

Each line in the two files carries an inline comment telling you which action to
try right there -- just read down the code.  For reference:

| Key       | Action            | Where to try it (see the inline comment) |
|-----------|-------------------|------------------------------------------|
| `Alt-Q E` | Diagnostics       | anywhere (also marks problems inline)    |
| `Alt-Q D` | Definition        | on `Shape` in `main.scala`               |
| `Alt-Q I` | Implementation    | on `area` in `shapes.scala`              |
| `Alt-Q T` | Type              | on `area` in `main`                      |
| `Alt-Q H` | Hover             | on `Pi` (`shapes.scala`), any identifier |
| `Alt-Q C` | Complete          | after `shapes.`                          |
| `Alt-Q R` | References        | on `Shape` / on `total`                  |
| `Alt-Q U` | Uses (highlight)  | on `name` in `shapes.scala`              |
| `Alt-Q Y` | inlaY hints       | toggle: the un-annotated `val`s get `: T`|
| `Alt-Q O` | Outline           | either file                              |
| `Alt-Q L` | code Lenses       | above `main`                             |
| `Alt-Q W` | Workspace symbol  | type `Shape` or `Color`                  |
| `Alt-Q A` | code Actions      | on `names`                               |
| `Alt-Q S` | Signature help    | inside `describe(...)`                    |
| `Alt-Q N` | reName            | on a local `val` (e.g. `names`)          |
| `Alt-Q F` | Format            | the whole file                           |

`Alt-Q` opens the action menu; `Alt-Q <letter>` runs one directly.  To actually
*run* the program, start the debugger with `Ctrl-G T`.
