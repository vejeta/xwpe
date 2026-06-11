package main

// Domain types used by main.go.  The cross-file LSP actions live here.

// Shape is the interface the demo types satisfy.
// Alt-Q R on `Shape` -> all references;
// Alt-Q J on `Shape` -> SUBTYPES (the implementers: Circle / Rectangle / Triangle)
type Shape interface {
	Area() float64 // Alt-Q I on `Area` -> implementations
	Name() string  // Alt-Q U on `Name` -> uses in this file
}

// Alt-Q D from main.go lands here;
// Alt-Q K on `Circle` -> SUPERTYPES (the Shape interface it implements)
type Circle struct{ Radius float64 }

func (c Circle) Area() float64 { return 3.14159265358979 * c.Radius * c.Radius } // Alt-Q H on Area
func (c Circle) Name() string  { return "circle" }

type Rectangle struct{ Width, Height float64 }

func (r Rectangle) Area() float64 { return r.Width * r.Height }
func (r Rectangle) Name() string  { return "rectangle" }

type Triangle struct{ Base, Height float64 }

func (t Triangle) Area() float64 { return 0.5 * t.Base * t.Height }
func (t Triangle) Name() string  { return "triangle" }
