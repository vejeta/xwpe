package main

// Code-actions playground (Alt-Q A).  gopls offers refactors and quick-fixes.
// Put the cursor on a marked spot, run Alt-Q A, pick an entry from the popup;
// the buffer is rewritten in place (F2 to save).  The rewrite is a single
// Ctrl-U (Undo) away, and Ctrl-R redoes it -- like any edit.

import "strings"

// Alt-Q A on the `strings.Join(...)` expression -> "Extract to variable".
func greet(names []string) string {
	return "hi, " + strings.Join(names, " and ") // Alt-Q A here -> extract variable
}

// Alt-Q A on the marked expression -> "Extract to function".
func banner(title string) string {
	return strings.ToUpper(title) + " " + strings.Repeat("=", len(title)) // Alt-Q A -> extract function
}

// Alt-Q A inside the Point{...} literal (with a field missing) -> "Fill Point".
type Point struct{ X, Y, Z int }

func origin() Point {
	return Point{X: 0} // Alt-Q A here -> fill the remaining fields (Y, Z)
}
