package demo

// Code actions playground (Alt-Q A).  Put the cursor on the marked spot, run
// Alt-Q A, choose an entry from the popup; the buffer is rewritten in place (F2
// to save).  These cover the THREE ways a server delivers a code action -- xwpe
// applies all of them:
//   * a direct edit                (e.g. "Convert to interpolation string")
//   * a server command             (workspace/executeCommand + applyEdit)
//   * an UNRESOLVED action         (only a `data` field -> codeAction/resolve;
//                                   this is how Metals ships its refactors)
//
// These two imports are deliberately UNUSED so Alt-Q E flags them and Alt-Q A
// on an import line offers "Organize imports" / "Remove unused" (a server
// command).  -Wunused is enabled in project.scala so the diagnostics appear.
import scala.collection.mutable.ListBuffer    // Alt-Q A here -> organize/remove imports
import scala.util.Try                         // (unused on purpose)

object Actions:

  /** Greet a person.  Two POSITIONAL arguments at the call below. */
  def greet(name: String, age: Int): String =
    "hi " + name + " (" + age + ")"           // Alt-Q A on a string -> interpolation

  // UNRESOLVED refactor: Alt-Q A on the call `greet(...)` offers
  // "Convert to named arguments" -> greet(name = "Ada", age = 42).  xwpe runs a
  // codeAction/resolve round-trip to fetch the edit before applying it.
  def callIt(): String =
    greet("Ada", 42)                          // Alt-Q A on greet(...) -> named arguments

  // Single-expression body: Alt-Q A on the def offers "Rewrite ... to braces".
  def twice(n: Int): Int = n * 2

  // An extractable expression: Alt-Q A on `base * height` offers
  // "Extract value" (pull it into a local val).
  def rectArea(base: Double, height: Double): Double =
    base * height                             // Alt-Q A here -> extract value
