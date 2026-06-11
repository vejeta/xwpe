//! Code-actions playground (Alt-Q A).  rust-analyzer ships a rich set of
//! "assists" (refactors) plus quick-fixes attached to compiler diagnostics.
//! Put the cursor on a marked spot, run Alt-Q A, pick an entry; the buffer is
//! rewritten in place (F2 saves, Ctrl-U undoes, Ctrl-R redoes).

#![allow(dead_code)]

// Alt-Q A on the `"hi ".to_string() + who` expression -> "Replace with format!".
fn greet(who: &str) -> String {
    "hi ".to_string() + who // Alt-Q A here -> convert to format!()
}

// Alt-Q A on the `if` -> assists like "Convert to match" / "Invert if".
fn classify(n: i32) -> i32 {
    if n > 0 {
        1
    } else {
        helper(n)
    } // Alt-Q A on `if` -> convert to match
}

// `unused` is never read -> rust-analyzer flags it (Alt-Q E); Alt-Q A offers a fix.
fn helper(n: i32) -> i32 {
    let unused = 42; // Alt-Q A here -> remove this / prefix with underscore
    n * 2
}
