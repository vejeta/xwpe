# xwpe Tutorial Examples

These files are used in the xwpe Texinfo manual (chapters: compiling,
debugging, tutorials) and can be used to verify xwpe's compiler and
debugger integration.

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

### How to test

    wpe compile_error_test.c

1. Press **F9** (Make). Errors appear in Messages window.
2. Press **Alt-T** -- cursor jumps to first error (line 4).
3. Press **Alt-T** -- cursor jumps to second error (line 8).
4. Press **Alt-V** -- cursor jumps back to first error.

Same workflow for `.java` with javac (cursor goes to start of line
since javac doesn't report column numbers).

## Debugging examples (Ctrl-G B/R, F7/F8, Ctrl-G P)

Each file computes `factorial(5) = 120` and prints the result.
All use the same debugging workflow.

| File | Compiler | Debugger | Notes |
|------|----------|----------|-------|
| debug_test.c | gcc | gdb | Baseline test |
| debug_test.cpp | g++ | gdb | C++ strings, member functions |
| debug_test.f90 | gfortran | gdb | Fortran recursive function |
| debug_test.pas | fpc | gdb | Pascal, no `.e` extension |

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
