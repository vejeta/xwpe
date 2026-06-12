// xwpe + gopls demo.  Open in programming mode:  wpe main.go
// The FIRST language-server action starts gopls.  gopls wants a module, and this
// directory has a go.mod, so it loads with full features.  Alt-Q ? opens the
// menu, Alt-Q <letter> runs one action.  A few actions have no single spot, so
// try them anywhere:  Alt-Q E (diagnostics, also marks problems inline),
// Alt-Q W (workspace symbol -- type e.g. "Shape" and jump to it), and Alt-Q M
// (semantic colours -- toggle server-driven highlighting).

package main

import "fmt"

// Alt-Q R on `total` -> references.  Alt-Q B on `total` -> its callers (main).
func total(shapes []Shape) float64 {
	sum := 0.0
	for _, s := range shapes { // Alt-Q Y (toggle): `s` gets an inferred-type hint
		sum += s.Area() // Alt-Q I on `Area` -> the concrete implementations
	}
	return sum
}

// Alt-Q G on `describe` -> outgoing calls (Name, Area, Printf).
func describe(s Shape) {
	fmt.Printf("%s with area %.2f\n", s.Name(), s.Area()) // Alt-Q D on Printf -> fmt (read-only)
}

func main() {
	shapes := []Shape{ // Alt-Q T on `shapes` -> []Shape;  Alt-Q O: file outline
		Circle{2.0},      // Alt-Q D on Circle -> shapes.go
		Rectangle{3.0, 4.0},
		Triangle{6.0, 1.5},
	}
	// Alt-Q Y (toggle): the := vars below have no written type, so the inferred
	// type pops in as a grey pill after each name.  Alt-Q H does one on demand.
	count := len(shapes)  // Alt-Q Y: the inferred type pops in ->>
	area := total(shapes) // Alt-Q S inside total(...) -> the signature
	for _, s := range shapes {
		describe(s) // Alt-Q N on `describe` -> rename it everywhere
	}
	// Alt-Q C after `fmt.` -> package members (Printf, Println, ...).
	fmt.Printf("%d shapes, total area %.2f\n", count, area) // Alt-Q V grows the selection
}
