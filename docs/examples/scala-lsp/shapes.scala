package demo

// Domain types used by main.scala.  The cross-file LSP actions live here.

trait Shape:                                   // Alt-Q R on `Shape` -> all references
  def area: Double                             // Alt-Q I on `area`  -> implementations
  def name: String                             // Alt-Q U on `name`  -> uses in this file

case class Circle(radius: Double) extends Shape:   // Alt-Q D from main.scala lands here
  def area: Double = math.Pi * radius * radius     // Alt-Q H on `Pi` -> math.Pi: Double
  def name: String = "circle"

case class Rectangle(width: Double, height: Double) extends Shape:
  def area: Double = width * height
  def name: String = "rectangle"

case class Triangle(base: Double, height: Double) extends Shape:
  def area: Double = 0.5 * base * height
  def name: String = "triangle"

enum Color:                                    // Alt-Q W: type "Color" to jump here
  case Red, Green, Blue

case class Paint(shape: Shape, color: Color)
