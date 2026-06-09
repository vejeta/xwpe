// debug_test.scala -- DAP debugging demo for the Scala backend (Bloop/scala-cli).
//
// Requires scala-cli in PATH (install with coursier: `cs install scala-cli`).
// In wpe/xwpe: put the cursor on the `f = f * i` line, Ctrl-G B (breakpoint),
// Ctrl-G R (Run -- the first Run boots a JVM build server, give it ~30-60s),
// Ctrl-G W then `f` to watch it, Ctrl-G R to continue around the loop (f grows
// 1, 1, 2, 6, 24, 120, ...), Ctrl-G Q to quit.
object Factorial:
  def main(args: Array[String]): Unit =
    var f = 1L
    var i = 1
    while i <= 10 do
      f = f * i              // <-- set the breakpoint on this line
      i = i + 1
    println(s"factorial(10) = $f")
