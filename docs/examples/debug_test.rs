// debug_test.rs -- DAP debugging demo for the Rust backend (gdb/lldb over DAP).
//
// Requires rustc and gdb (or lldb-dap).  xwpe compiles with `rustc -g` and
// debugs the binary with `gdb --interpreter=dap`.
// In wpe/xwpe: put the cursor on the `f = f * i;` line, Ctrl-G B (breakpoint),
// Ctrl-G R (Run), Ctrl-G W then `f` to watch it, Ctrl-G R to continue around
// the loop (f grows 1, 1, 2, 6, 24, 120, ...), Ctrl-G Q to quit.
fn main() {
    let mut f: i64 = 1;
    for i in 1..=10 {
        f = f * i;             // <-- set the breakpoint on this line
    }
    println!("factorial(10) = {}", f);
}
