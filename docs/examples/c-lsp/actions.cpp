// Code-actions playground (Alt-Q A).  Put the cursor on the marked spot, run
// Alt-Q A, choose an entry from the popup; the buffer is rewritten in place (F2
// to save).  clangd offers compiler fix-its (driven by a diagnostic) and a few
// tweak refactors.  -Wall -Wextra in compile_flags.txt make the diagnostics
// appear, so Alt-Q E marks them and Alt-Q A offers the matching fix.

#include <string>

namespace demo {

// `unused` is never read -> clangd warns (Alt-Q E); Alt-Q A on it offers a fix.
int helper(int n) {
    int unused = 42;                  // Alt-Q A here -> remove the unused variable
    return n * 2;
}

// Alt-Q A on the `if` offers a tweak: "Add braces to 'if'".
int classify(int n) {
    if (n > 0)                        // Alt-Q A on `if` -> add braces
        return 1;
    return helper(n);
}

// Alt-Q A on `auto x = ...` offers "Expand auto type" (write the deduced type).
std::string greet(const std::string& who) {
    auto msg = "hi " + who;           // Alt-Q A on `auto` -> expand to std::string
    return msg;
}

}  // namespace demo
