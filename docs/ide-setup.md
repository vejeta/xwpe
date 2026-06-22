# The `Alt-Q` IDE layer: compilers, language servers, debuggers

Once the editor builds (see [BUILDING.md](../BUILDING.md) and the
[install guides](install/)), this is the optional layer that turns xwpe into an
IDE: the compilers it builds with (F9), the debuggers it drives, and the
language servers behind the `Alt-Q` LSP features. The feature guide for the
`Alt-Q` actions themselves is [docs/LSP.md](LSP.md).

xwpe auto-detects the compiler, debugger and language server by file extension
and runs whatever is on `PATH` -- install only the ones for the languages you
use. (`JAVA_HOME` for Metals is covered in the **Environment** section below.)

## External tools

### Debian / Ubuntu

```sh
# compilers (F9 build + error navigation)
sudo apt install gcc g++ gfortran      # C/C++/Fortran
sudo apt install fpc                   # Free Pascal
sudo apt install default-jdk           # Java (javac + jdb)
sudo apt install python3               # Python (py_compile + pdb)
sudo apt install texlive-latex-base    # LaTeX (pdflatex)
sudo apt install perl                  # Perl (perl -c)
sudo apt install gnucobol              # COBOL (cobc)
sudo apt install algol68g              # Algol 68 (a68g + its monitor debugger)
sudo apt install golang-go             # Go (compile)
sudo apt install rustc                 # Rust (rustc -g)
sudo apt install gdb                   # C/C++/Fortran/Pascal/Rust debugger

# language servers (the Alt-Q LSP layer)
sudo apt install clangd gopls rust-analyzer python3-pylsp   # C/C++, Go, Rust, Python
cs install metals scala-cli                                 # Scala (coursier; get-coursier.io)
sudo apt install openjdk-21-jdk                             # Metals' JVM: an LTS JDK 17/21

# DAP debug servers not in the archive
go install github.com/go-delve/delve/cmd/dlv@latest         # Go (Delve)
```

### macOS (Homebrew)

```sh
# compilers: clang ships with the Xcode Command Line Tools
# (xcode-select --install); add the others you need
brew install go rust gcc               # Go, Rust, gfortran (in gcc)

# language servers
brew install llvm gopls rust-analyzer pyright   # clangd lives inside llvm (keg-only)
brew install coursier openjdk@21 && coursier install metals scala-cli   # Scala/Metals + a JDK

# DAP debug servers
go install github.com/go-delve/delve/cmd/dlv@latest         # Go (Delve)
```

> **Metals needs an LTS JDK (17 or 21)** in `JAVA_HOME` -- its presentation
> compiler (hover, completion, go-to-definition) runs there, and a too-new JDK
> (e.g. OpenJDK 26) crashes it (`asTerm called on not-a-Term`) so
> hover/navigation silently return empty. Wiring `JAVA_HOME` is covered next.

## Debugging in action

xwpe drives a real source-level debugger -- breakpoints, stepping (F8/F7), live
watches (Ctrl-G W) and program output in Messages -- with the same Borland keys
for every backend (gdb, jdb, pdb, a68g, and the DAP client for Go/Rust/Scala).

<p align="center">
  <img src="../screenshots/xwpe-go-dap-debug.png" width="720" alt="Debugging a Go program in xwpe via Delve over DAP: the editor stopped at a breakpoint on line 9 (highlighted), and a Watches window below showing the live value fact: 6 as the factorial loop runs.">
  <br><em>Go through Delve/DAP: stopped at a breakpoint, with a live watch (<code>fact</code>) updating as the loop runs.</em>
</p>

<p align="center">
  <img src="../screenshots/xwpe-ga68-watch.gif" width="720" alt="Debugging a GNU Algol 68 (ga68) program in xwpe: Ctrl-G R compiles with ga68 and starts gdb, Ctrl-G W adds a watch on a variable, Window/Size-Move tiles the editor, Watches and Messages windows, and F8 single-steps while the watch value grows. The pressed keys are overlaid in the corner.">
  <br><em>ga68 + gdb: stepping a factorial with a live watch (<code>fact</code>: 1 &rarr; 2 &rarr; 6 &rarr; 24 &rarr; 120). The full Algol 68 story (a68g's monitor vs ga68/gdb, per-file dialect detection) is in the manual: <code>info xwpe</code>, "Debugging Algol 68 programs".</em>
</p>

## Environment

xwpe finds the language servers, the JDK and (for an uninstalled build) its data
files through a few variables. Rather than exporting them by hand, let the
bundled **`contrib/xwpe-env`** helper set them -- the `brew shellenv` idiom: it
emits shell code rather than a list you copy, finds clangd / the JDK / the
Coursier dir / this checkout, and skips whatever is absent. Plain POSIX `sh`, so
it works the same on macOS, Linux and the BSDs.

**Permanent (every new terminal) -- add it to your profile, once:**

```sh
sh contrib/xwpe-env --persist     # detects bash/zsh/fish, writes the line with
                                  # this script's ABSOLUTE path, idempotent
```

**Just this shell (no profile change):**

```sh
eval "$(sh contrib/xwpe-env)"                  # bash / zsh
sh contrib/xwpe-env --shell fish | source      # fish (it cannot eval POSIX export)
```

Then confirm: `echo $XWPE_LIB` is set and `command -v metals` finds the server.

> **Gotchas this avoids.** `eval`/`| source` change only the CURRENT shell --
> `--persist` is what makes it stick. And a hand-written profile line must use an
> ABSOLUTE path (`contrib/xwpe-env` is relative to the checkout); `--persist`
> writes the absolute path for you. Seeing
> `fish: Unknown command: contrib/xwpe-env`? You ran the bash/zsh line in fish.

The helper sets `XWPE_LIB` (so a non-installed build finds its data files) and,
for Metals, points `JAVA_HOME` at a 17/21 JDK -- on Linux it looks under
`/usr/lib/jvm`, on macOS it asks Homebrew / `java_home`. Metals' first start then
indexes for minutes before hover and navigation answer.

## Run a bundled demo

Each wired language has a small, fully-commented LSP demo. Open one **as `wpe`**,
then `Alt-Q E` starts the server and `Alt-Q ?` lists the actions; each
[`docs/examples/*-lsp/`](examples/) has its own walkthrough:

```sh
./wpe docs/examples/c-lsp/main.cpp        # clangd, ready in ~2s
./wpe docs/examples/rust-lsp/src/main.rs  # rust-analyzer
./wpe docs/examples/scala-lsp/main.scala  # Metals (slow first start)
```
