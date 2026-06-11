// xwpe + clangd demo.  Open in programming mode:  wpe main.cpp
// The FIRST language-server action starts clangd -- fast, no JVM; it is ready in
// a second or two.  Alt-Q ? opens the menu, Alt-Q <letter> runs one action.
// A compile_flags.txt next to this file pins -std=c++17 -Wall so clangd analyses
// the code the same way your build does.  A few actions have no single spot, so
// try them anywhere:  Alt-Q E (diagnostics, also marks problems inline),
// Alt-Q W (workspace symbol -- type e.g. "Shape" and jump to it project-wide),
// and Alt-Q M (semantic colours -- toggle server-driven highlighting).
//
// (Plain C works too -- open a .c file and the same Alt-Q keys drive clangd;
//  this demo is C++ so it can show the class-only actions I / K / J as well.)

#include <cstdio>
#include <memory>
#include <vector>

#include "shapes.hpp"

using ShapeList = std::vector<std::unique_ptr<Shape>>;   // Alt-Q O: file outline

// Alt-Q R on `total` -> references.  Alt-Q B on `total` -> its callers (main).
double total(const ShapeList& shapes) {
    double sum = 0.0;
    for (const auto& s : shapes)          // Alt-Q Y (toggle): `s` gets a dim : Shape& hint
        sum += s->area();                 // Alt-Q I on `area` -> the concrete overrides
    return sum;
}

// Alt-Q G on `describe` -> outgoing calls (name, area, printf).
void describe(const Shape& s) {
    std::printf("%s with area %.2f\n", s.name().c_str(), s.area());  // Alt-Q D on printf -> cstdio (read-only)
}

int main() {
    ShapeList shapes;                                          // Alt-Q T on `shapes` -> its type
    shapes.push_back(std::make_unique<Circle>(2.0));           // Alt-Q D on Circle -> shapes.hpp
    shapes.push_back(std::make_unique<Rectangle>(3.0, 4.0));
    shapes.push_back(std::make_unique<Triangle>(6.0, 1.5));

    // Alt-Q Y (toggle): the `auto` vars below have no written type, so the
    // deduced type appears dim after each name.  Alt-Q H does one on demand.
    auto count = shapes.size();           // inlay -> : size_t
    auto area  = total(shapes);           // Alt-Q S inside total(...) -> the signature

    for (const auto& s : shapes)
        describe(*s);                     // Alt-Q N on `describe` -> rename it everywhere
    // Alt-Q C after `shapes.` -> member completions (push_back, size, ...).
    std::printf("%zu shapes, total area %.2f\n", count, area);  // Alt-Q V grows the selection
    return 0;
}
