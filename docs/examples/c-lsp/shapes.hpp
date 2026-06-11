#ifndef SHAPES_HPP
#define SHAPES_HPP

#include <string>

// Domain types used by main.cpp.  The cross-file LSP actions live here.

// Alt-Q R on `Shape` -> all references;
// Alt-Q J on `Shape` -> SUBTYPES (Circle / Rectangle / Triangle)
struct Shape {
    virtual double area() const = 0;            // Alt-Q I on `area` -> implementations
    virtual std::string name() const = 0;       // Alt-Q U on `name` -> uses in this file
    virtual ~Shape() = default;
};

// Alt-Q D from main.cpp lands here;
// Alt-Q K on `Circle` -> SUPERTYPES (Shape)
struct Circle : Shape {
    double radius;
    explicit Circle(double r) : radius(r) {}
    double area() const override;               // Alt-Q H on `area` -> double Circle::area()
    std::string name() const override { return "circle"; }
};

struct Rectangle : Shape {
    double width, height;
    Rectangle(double w, double h) : width(w), height(h) {}
    double area() const override;
    std::string name() const override { return "rectangle"; }
};

struct Triangle : Shape {
    double base, height;
    Triangle(double b, double h) : base(b), height(h) {}
    double area() const override;
    std::string name() const override { return "triangle"; }
};

#endif  // SHAPES_HPP
