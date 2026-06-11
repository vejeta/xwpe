# Go (gopls) language-server demo for xwpe

A tiny, deliberately-commented Go module that exercises **every** xwpe
language-server action that gopls supports.  It is the fastest way to see (and
try) the IDE features in Go -- gopls needs no JVM, so it is ready quickly.

## Requirements

- `gopls` and `go` -- on Debian/Ubuntu: `sudo apt install gopls golang-go`
  (or `go install golang.org/x/tools/gopls@latest`).

gopls wants a **module**: this directory has a `go.mod`, so it loads with full
features.  A lone `.go` file with no module loads in a degraded ad-hoc mode.

See the user-facing guide [`docs/LSP.md`](../../LSP.md), or the **Language
servers** chapter of the manual (`info xwpe`), for the full setup notes.

## Run it

```sh
cd docs/examples/go-lsp
wpe main.go              # programming mode (xwpe in X11; wpe in a terminal)
```

The **first** language-server action starts gopls; it is ready in a moment.
`Alt-Q ?` opens the action menu, `Alt-Q <letter>` runs one directly.

## What to try

Each line in `main.go`, `shapes.go` and `actions.go` carries an inline comment
telling you which action to try right there.  For reference:

| Key       | Action            | Where to try it                          |
|-----------|-------------------|------------------------------------------|
| `Alt-Q E` | Diagnostics       | anywhere (also marks problems inline)    |
| `Alt-Q D` | Definition        | on `Circle` (main) or `Printf` -> `fmt`  |
| `Alt-Q I` | Implementation    | on `Area` in `shapes.go` -> the implementers |
| `Alt-Q T` | Type              | on `shapes` in `main`                    |
| `Alt-Q H` | Hover             | on `Area`, any identifier                |
| `Alt-Q C` | Complete          | after `fmt.`                             |
| `Alt-Q R` | References        | on `Shape` / on `total`                  |
| `Alt-Q B` | Incoming calls    | on `total` in `main.go`                  |
| `Alt-Q G` | Outgoing calls    | on `describe` in `main.go`               |
| `Alt-Q K` | Supertypes        | on `Circle` -> the `Shape` interface     |
| `Alt-Q J` | Subtypes          | on `Shape` -> Circle / Rectangle / Triangle |
| `Alt-Q U` | Uses (highlight)  | on `Name` in `shapes.go`                 |
| `Alt-Q V` | Expand selection  | on a token in `main` (press again widens)|
| `Alt-Q Y` | Inlay hints       | toggle: the `:=` vars get `: T`, call args get param names |
| `Alt-Q M` | Semantic colours  | toggle, anywhere (server-driven highlight)|
| `Alt-Q O` | Outline           | any file                                 |
| `Alt-Q W` | Workspace symbol  | type `Shape` or `Circle`                 |
| `Alt-Q A` | code Actions      | `actions.go` -- see "Code actions" below |
| `Alt-Q S` | Signature help    | inside `total(...)`                      |
| `Alt-Q N` | reName            | on `describe` (or any symbol)            |
| `Alt-Q F` | Format            | the whole file (gofmt, via gopls)        |

`Alt-Q` opens the action menu; `Alt-Q <letter>` runs one directly.  To compile
and run the program, use `F9` (build) and the debugger (`Ctrl-G T`, Delve over
DAP).

> **`Alt-Q L` (code lenses):** gopls attaches lenses mostly to test files and
> `go.mod` (run test, tidy, upgrade), so on `main.go` this is usually empty --
> expected, not a bug.

## Go to definition into the standard library

`Alt-Q D` on `Printf` (in `describe`, `main.go`) jumps into the `fmt` package
source under the Go toolchain -- which xwpe opens **read-only**, marked by a
padlock at the left of the title bar and a dimmed name.  You can read and copy
the standard-library source, not edit it.

## Code actions (Alt-Q A)

`actions.go` is a playground for gopls's refactors.  Put the cursor on a marked
spot, run `Alt-Q A`, pick an entry; the buffer is rewritten in place (`F2`
saves).  The rewrite is a single `Ctrl-U` (Undo) away, and `Ctrl-R` redoes it.

- **extract to variable** -- `Alt-Q A` on the `strings.Join(...)` expression in
  `greet` pulls it into a named local.
- **extract to function** -- `Alt-Q A` on the marked expression in `banner`
  lifts it into a new function.
- **fill struct** -- `Alt-Q A` inside `Point{X: 0}` in `origin` offers to fill
  the remaining fields (`Y`, `Z`).
