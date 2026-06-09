// debug_test.go -- DAP debugging demo for the Go backend (Delve over DAP).
//
// Requires dlv (delve) and go, AND a Go module: run `go mod init demo` in this
// directory first, because `dlv dap` builds the package.
// In wpe/xwpe: put the cursor on the `f = f * i` line, Ctrl-G B (breakpoint),
// Ctrl-G R (Run), Ctrl-G W then `f` to watch it, Ctrl-G R to continue around
// the loop (f grows 1, 1, 2, 6, 24, 120, ...), Ctrl-G Q to quit.
package main

import "fmt"

func main() {
	f := 1
	for i := 1; i <= 10; i++ {
		f = f * i              // <-- set the breakpoint on this line
	}
	fmt.Printf("factorial(10) = %d\n", f)
}
