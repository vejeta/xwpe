# xwpe Tutorial Examples

These files are used in the xwpe Texinfo manual (chapters: compiling,
debugging, tutorials) and can be used to verify xwpe's compiler and
debugger integration.  This is the PEDAGOGICAL set (what the manual walks
through).  The fixtures the automated test suite feeds to the binary live
separately in `tests/inputs/`.

## Language-server (LSP / IDE) demos -- one per language

Each subfolder is a small, fully-commented project that exercises **every**
`Alt-Q` language-server action its server supports.  Open the entry file with
`wpe` and read down the code -- each line says which `Alt-Q` action to try right
there.  Pick the one for *your* language:

| Folder | Language | Server | Open with |
|--------|----------|--------|-----------|
| [`scala-lsp/`](scala-lsp/)   | Scala  | Metals               | `wpe main.scala`   |
| [`c-lsp/`](c-lsp/)           | C/C++  | clangd               | `wpe main.cpp`     |
| [`python-lsp/`](python-lsp/) | Python | pyright / pylsp      | `wpe main.py`      |
| [`go-lsp/`](go-lsp/)         | Go     | gopls                | `wpe main.go`      |
| [`rust-lsp/`](rust-lsp/)     | Rust   | rust-analyzer        | `wpe src/main.rs`  |

The IDE features themselves are documented in [`docs/LSP.md`](../LSP.md) and the
**Language servers** chapter of the manual; each folder's own `README.md` lists
its full `Alt-Q` key table and setup notes.

## Compilation examples (F9 + Alt-T/Alt-V error navigation)

Each file has deliberate errors in two separate functions so that
Alt-T (Next Error) and Alt-V (Previous Error) can navigate between them.

| File | Compiler | Expected errors |
|------|----------|----------------|
| compile_error_test.c | gcc | 2 type errors (lines 4, 8) |
| compile_error_test.cpp | g++ | 2 type errors (lines 4, 8) |
| compile_error_test.f90 | gfortran | 2 type errors (lines 5, 6) |
| compile_error_test.pas | fpc | 2 type errors (lines 6, 7) |
| compile_error_test.java | javac | 2 type errors (lines 3, 6) |
| compile_error_test.a68 | a68g | 2 type errors (lines 7, 8) |

### How to test

    wpe compile_error_test.c

1. Press **F9** (Make). Errors appear in Messages window.
2. Press **Alt-T** -- cursor jumps to first error (line 4).
3. Press **Alt-T** -- cursor jumps to second error (line 8).
4. Press **Alt-V** -- cursor jumps back to first error.

Same workflow for `.java` with javac (cursor goes to start of line
since javac doesn't report column numbers).

**Algol 68 (a68g)**: Make (F9) runs `a68g --norun` to check the syntax.
Both type errors are listed in Messages and Alt-T/Alt-V step between them.
a68g names no file and reports the enclosing clause's line, so the cursor
jump is approximate -- read the diagnostic text in Messages. `Ctrl-F9`
runs the program. A minimal runnable program is `tests/inputs/hello.a68`.

## Debugging examples (Ctrl-G B/R, F7/F8, Ctrl-G P)

Each file computes `factorial(5) = 120` and prints the result.
All use the same debugging workflow.

| File | Compiler | Debugger | Notes |
|------|----------|----------|-------|
| debug_test.c | gcc | gdb | Baseline test |
| debug_test.cpp | g++ | gdb | C++ strings, member functions |
| debug_test.f90 | gfortran | gdb | Fortran recursive function |
| debug_test.pas | fpc | gdb | Pascal, no `.e` extension |
| debug_test.py | python3 | pdb | Interpreted, no compile step |
| debug_test.a68 | a68g | a68g --monitor | Interpreted, built-in monitor |

### How to test

    wpe debug_test.c

1. **Ctrl-G B** on the `x = 5` line (set breakpoint).
2. **Ctrl-G R** (compile, link, start gdb). Execution stops at breakpoint.
3. **F8** (Step) several times until past the print/printf/writeln line.
   The highlighted line is the *next* to execute -- step ONE MORE time
   after the print line to actually execute it.
4. **Ctrl-G W**, type `x`, Enter (watch variable). Shows `x = 5`.
5. **Ctrl-G K** (call stack).
6. **Ctrl-G P** (User Screen). Shows program output:
   - C/C++: `factorial(5) = 120`
   - Fortran: ` factorial(           5 ) =          120`
   - Pascal: `factorial(5) = 120`
7. Press any key to return to editor.
8. **Ctrl-G Q** to quit debugger.

### Language-specific notes

**C (gcc)**: baseline. All features work. Breakpoints, stepping into
`factorial()`, watch variables with exact column positioning.

**C++ (g++)**: works like C. Watch expressions can use C++ syntax
(e.g., `name.length()`). Step into `greet()` works.

**Fortran (gfortran)**: GNU compiler, uses `-c -o` and `.e` extension
like C. gdb's Fortran support handles `implicit none`, `intent(in)`,
and recursive functions. Array inspection may be limited.

**Pascal (fpc)**: non-GNU compiler. xwpe skips `-c -o` flags and the
separate link step (fpc links internally). Executable has no `.e`
extension. gdb has basic Free Pascal support -- breakpoints and
stepping work; variable inspection may be limited for Pascal-specific
types (strings, sets, records).

**Java (javac)**: compilation and error navigation only. gdb does not
debug Java bytecode. Use a Java-specific debugger (jdb) for debugging.

**Algol 68 (a68g)**: interpreted. xwpe auto-selects the Algol 68 Genie
monitor (`a68g --monitor`) -- no compile step. `Ctrl-G R` runs to the
breakpoint; a68g auto-breaks at the first line, like gdb at `main`.
Breakpoints (`Ctrl-G B`) are re-hit on each recursive call. `Ctrl-G W`
on `n` shows the typed value, e.g. `(INT) +5`. The program's output and
end-of-run are captured in Messages (`Ctrl-G P`). Watch expressions are
unquoted (type `n`, not `"n"`).

## DAP examples (modern debuggers over the Debug Adapter Protocol)

`debug_test.go`, `debug_test.rs` and `debug_test.scala` are factorial
demos for the DAP backend (xwpe's seventh debugger, `DEB_DAP`, auto-selected
by extension). Each prints `factorial(10) = 3628800`; set a breakpoint on the
`f = f * i` line and watch `f` grow `1, 1, 2, 6, 24, 120, ...`.

| File | Needs | Adapter | Notes |
|------|-------|---------|-------|
| debug_test.go | `dlv`, `go` | Delve (`dlv dap`) | run `go mod init demo` in the dir first (dlv builds the package) |
| debug_test.rs | `rustc`, `gdb` (or `lldb-dap`) | `gdb --interpreter=dap` | xwpe compiles `rustc -g`; adapter picked in Ctrl-G O |
| debug_test.scala | `scala-cli` (`cs install scala-cli`) | Bloop / scala-debug-adapter via BSP | first Run boots a JVM build server (~30-60s); modern Scala only (not Debian's 2.11) |

### How to test (same keys for all three)

    wpe debug_test.scala            # or debug_test.go / debug_test.rs

1. Move the cursor to the `f = f * i` line.
2. **Ctrl-G B** -- set a breakpoint.
3. **Ctrl-G R** -- Run. (Scala's first Run is slow: it starts Bloop.)
   Execution stops at the breakpoint.
4. **Ctrl-G W**, type `f`, Enter -- a live watch on `f`.
5. **Ctrl-G R** repeatedly -- continue around the loop; `f` grows.
6. **Ctrl-G Q** -- quit the debugger.

Scala frames arrive demangled to the Scala signature
(`Factorial.main(args: Array[String]): Unit`). The Scala toolchain is an
external coursier tool -- nothing Scala ships inside xwpe.

## Feature examples

These illustrate specific xwpe features used in the manual.

| Files | Feature | Chapter |
|-------|---------|---------|
| headers_test.c + headers_test.h + fake.h + disabled.h | Automatic header-dependency recompilation | compiling |
| debug_constructor.c | Configurable debugger start symbol | debugging |
| interactive_test.c | Interactive program I/O in Messages | debugging |

### headers_test.c -- header dependencies (F9 / Make)

`headers_test.c` includes the real header `headers_test.h`, names `fake.h`
inside a `//` comment, and includes `disabled.h` inside an `#if 0` block.

    wpe headers_test.c

1. Press **F9**. The file compiles.
2. Press **F9** again -- nothing happens (the `.o` is up to date).
3. Edit and save `headers_test.h`, press **F9** -- it **recompiles**
   (a real dependency changed).
4. Edit and save `fake.h` or `disabled.h`, press **F9** -- it does
   **NOT** recompile: those `#include`s are commented out or disabled,
   so they are not dependencies.

### debug_constructor.c -- debugger start symbol

`setup()` runs before `main()` via the constructor attribute.

    wpe debug_constructor.c

1. By default the debugger breaks at `main()`.
2. Set the start symbol to `setup` (Options -> Compiler -> start symbol).
3. **Ctrl-G R** -- execution stops in `setup()`, before `main()`.

### interactive_test.c -- interactive input

The program prompts for a name and blocks on `fgets()`.

    wpe interactive_test.c

1. **Ctrl-F9** (Run). The prompt "What is your name?" appears in Messages.
2. Type a name in the Messages window and press **Enter**.
3. The program prints "Hello, <name>! Welcome to xwpe" (UTF-8 output).

The same works under the debugger: step (F8) onto the `fgets()` line,
run it, type the answer in Messages, and continue.

## Keyboard quick reference

| Key | Action |
|-----|--------|
| F9 | Compile + Link (Make) |
| Alt-T | Next Error |
| Alt-V | Previous Error |
| Ctrl-G B | Toggle Breakpoint |
| Ctrl-G R | Run/Continue debugger |
| F7 | Step into (Trace) |
| F8 | Step over |
| Ctrl-G W | Watch variable |
| Ctrl-G K | Call stack |
| Ctrl-G P | Program output (User Screen) |
| Ctrl-G Q | Quit debugger |
| F6 | Switch window (editor <-> Messages) |
