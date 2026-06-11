# Rust (rust-analyzer) language-server demo for xwpe

A tiny, deliberately-commented Cargo crate that exercises the xwpe
language-server actions for Rust, through rust-analyzer.

## Requirements

- `rust-analyzer`, plus `cargo`/`rustc` and `rust-src` (for go-to-definition into
  `std`).  On Debian/Ubuntu:

```sh
sudo apt install rust-analyzer cargo rustc rust-src
# or, with rustup:  rustup component add rust-analyzer rust-src
```

rust-analyzer wants a **Cargo workspace**: open files from inside the crate (this
directory has a `Cargo.toml`).  It indexes `std` on first start, so the cold
start is the slowest of the wired servers -- the editor stays responsive and the
actions sharpen as the index settles.

See the user-facing guide [`docs/LSP.md`](../../LSP.md), or the **Language
servers** chapter of the manual (`info xwpe`), for the full setup notes.

## Run it

```sh
cd docs/examples/rust-lsp
wpe src/main.rs          # programming mode (xwpe in X11; wpe in a terminal)
```

`Alt-Q ?` opens the action menu, `Alt-Q <letter>` runs one directly.

## What to try

Each line in `src/main.rs`, `src/shapes.rs` and `src/actions.rs` carries an
inline comment telling you which action to try right there.  For reference:

| Key       | Action            | Where to try it                          |
|-----------|-------------------|------------------------------------------|
| `Alt-Q E` | Diagnostics       | anywhere (also marks problems inline)    |
| `Alt-Q D` | Definition        | on `Circle` (main) or `PI` -> `std`      |
| `Alt-Q I` | Implementation    | on `Shape` / `area` -> the `impl` blocks |
| `Alt-Q T` | Type              | on `shapes` in `main`                    |
| `Alt-Q H` | Hover             | on `area`, any identifier                |
| `Alt-Q C` | Complete          | after `shapes.`                          |
| `Alt-Q R` | References        | on `Shape` / on `total`                  |
| `Alt-Q B` | Incoming calls    | on `total` in `main.rs`                  |
| `Alt-Q G` | Outgoing calls    | on `describe` in `main.rs`               |
| `Alt-Q U` | Uses (highlight)  | on `name` in `shapes.rs`                 |
| `Alt-Q V` | Expand selection  | on a token in `main` (press again widens)|
| `Alt-Q Y` | Inlay hints       | toggle: the `let` bindings get `: T`, call args get param names |
| `Alt-Q M` | Semantic colours  | toggle, anywhere (server-driven highlight)|
| `Alt-Q O` | Outline           | any file                                 |
| `Alt-Q L` | code Lenses       | a `Run`/`Debug` lens sits above `main`; impl counts on types |
| `Alt-Q W` | Workspace symbol  | type `Shape` or `Circle`                 |
| `Alt-Q A` | code Actions      | `actions.rs` -- see "Code actions" below |
| `Alt-Q S` | Signature help    | inside `total(...)`                      |
| `Alt-Q N` | reName            | on `describe` (or any symbol)            |
| `Alt-Q F` | Format            | the whole file (rustfmt, via rust-analyzer)|

`Alt-Q` opens the action menu; `Alt-Q <letter>` runs one directly.  To compile
and run the program, use `F9` (build) and the debugger (`Ctrl-G T`, lldb/gdb).

> **`Alt-Q K`/`J` (super/subtypes):** Rust models polymorphism with traits, not
> a class hierarchy, so rust-analyzer surfaces that as **implementations** --
> use `Alt-Q I` on a trait or trait method to find its implementors.  The
> type-hierarchy keys may come back empty for Rust; that is expected.

## Go to definition into the standard library

`Alt-Q D` on `PI` (in `Circle::area`, `shapes.rs`) jumps into the `std` source
(you need `rust-src` installed) -- which xwpe opens **read-only**, marked by a
padlock at the left of the title bar.  You can read and copy it, not edit.

## Code actions (Alt-Q A)

`actions.rs` is a playground for rust-analyzer's assists -- the richest code-
action set of the wired servers.  Put the cursor on a marked spot, run `Alt-Q A`,
pick an entry; the buffer is rewritten in place (`F2` saves; `Ctrl-U`/`Ctrl-R`
undo/redo).

- **convert to `format!`** -- `Alt-Q A` on `"hi ".to_string() + who` in `greet`.
- **convert to `match`** -- `Alt-Q A` on the `if` in `classify`.
- **remove unused** -- `Alt-Q A` on the `let unused = 42;` in `helper` (the
  quick-fix attached to the `unused_variables` warning).
