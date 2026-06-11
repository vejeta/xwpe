// xwpe + rust-analyzer demo.  Open in programming mode, from inside the crate:
//   cd docs/examples/rust-lsp && wpe src/main.rs
// The FIRST language-server action starts rust-analyzer.  It indexes the standard
// library on first run, so the cold start is a little slow (the editor stays
// responsive); the actions sharpen as the index settles.  Alt-Q ? opens the
// menu, Alt-Q <letter> runs one action.  A few have no single spot, so try them
// anywhere:  Alt-Q E (diagnostics), Alt-Q W (workspace symbol -- type "Shape"),
// and Alt-Q M (semantic colours -- toggle).

mod actions;
mod shapes;

use shapes::{Circle, Rectangle, Shape, Triangle};

// Alt-Q R on `total` -> references.  Alt-Q B on `total` -> its callers (main).
fn total(shapes: &[Box<dyn Shape>]) -> f64 {
    shapes.iter().map(|s| s.area()).sum() // Alt-Q I on `area` -> the impls
}

// Alt-Q G on `describe` -> outgoing calls (name, area, println!).
fn describe(s: &dyn Shape) {
    println!("{} with area {:.2}", s.name(), s.area()); // Alt-Q D on println! -> std
}

fn main() {
    let shapes: Vec<Box<dyn Shape>> = vec![ // Alt-Q T on `shapes` -> Vec<Box<dyn Shape>>
        Box::new(Circle { radius: 2.0 }),   // Alt-Q D on Circle -> shapes.rs
        Box::new(Rectangle {
            width: 3.0,
            height: 4.0,
        }),
        Box::new(Triangle {
            base: 6.0,
            height: 1.5,
        }),
    ];
    // Alt-Q Y (toggle): the `let` bindings have no written type, so the inferred
    // type appears dim after each name.  Alt-Q H does one on demand.
    let count = shapes.len(); // inlay -> : usize
    let area = total(&shapes); // Alt-Q S inside total(...) -> the signature
    for s in &shapes {
        describe(s.as_ref()); // Alt-Q N on `describe` -> rename it everywhere
    }
    // Alt-Q C after `shapes.` -> slice/Vec members (iter, len, ...).
    println!("{} shapes, total area {:.2}", count, area); // Alt-Q V grows the selection
}
