#include "shapes.hpp"

#include <cmath>

// Implementations of the Shape subtypes declared in shapes.hpp.
// Alt-Q D on any of these from main.cpp / shapes.hpp jumps straight here.

double Circle::area() const { return M_PI * radius * radius; }   // Alt-Q H on M_PI -> cmath

double Rectangle::area() const { return width * height; }

double Triangle::area() const { return 0.5 * base * height; }
