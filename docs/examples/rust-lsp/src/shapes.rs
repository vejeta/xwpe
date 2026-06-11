//! Domain types used by main.rs.  The cross-file LSP actions live here.

// Alt-Q R on `Shape` -> all references;
// Alt-Q I on `Shape` -> implementations (the `impl Shape for ...` blocks below).
// Rust models "subtypes" as trait impls, so Alt-Q I is the idiom here; the type
// hierarchy keys (Alt-Q K / J) may come back empty for Rust -- expected.
pub trait Shape {
    fn area(&self) -> f64; // Alt-Q I on `area` -> the impls
    fn name(&self) -> &str; // Alt-Q U on `name` -> uses in this file
}

// Alt-Q D from main.rs lands here.
pub struct Circle {
    pub radius: f64,
}

impl Shape for Circle {
    fn area(&self) -> f64 {
        std::f64::consts::PI * self.radius * self.radius // Alt-Q D on `PI` -> std (read-only)
    }
    fn name(&self) -> &str {
        "circle"
    }
}

pub struct Rectangle {
    pub width: f64,
    pub height: f64,
}

impl Shape for Rectangle {
    fn area(&self) -> f64 {
        self.width * self.height
    }
    fn name(&self) -> &str {
        "rectangle"
    }
}

pub struct Triangle {
    pub base: f64,
    pub height: f64,
}

impl Shape for Triangle {
    fn area(&self) -> f64 {
        0.5 * self.base * self.height
    }
    fn name(&self) -> &str {
        "triangle"
    }
}
