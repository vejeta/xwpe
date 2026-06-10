package demo

// xwpe + Metals demo.  Open in programming mode:  wpe main.scala
// The FIRST language-server action starts Metals (a JVM boots, ~1 min cold);
// it is ready when Messages shows "LSP: no problems.".  Alt-Q ? opens the menu,
// Alt-Q <letter> runs one action.  Two actions have no single spot, so try them
// anywhere:  Alt-Q E  (diagnostics, also marks problems inline) and
// Alt-Q W  (workspace symbol -- type e.g. "Shape" and jump to it project-wide).
// Every other action is anchored to a line below.

object Main:

  /** Render a shape as "name with area N.NN". */         // Alt-Q O: file outline
  def describe(s: Shape): String =                        // Alt-Q D on Shape -> shapes.scala
    f"${s.name} with area ${s.area}%.2f"                  // Alt-Q I on `area` -> an impl

  def total(shapes: List[Shape]): Double =                // Alt-Q R on `total` -> references
    shapes.map(_.area).sum                                // Alt-Q H on `sum` -> its type/doc

  def main(args: Array[String]): Unit =                   // Alt-Q L: a run|debug lens sits here
    // Alt-Q Y (toggle): the vals below have NO written type, so the inferred
    // type appears dim at the end of each line.  Alt-Q H does one on demand.
    val shapes  = List(Circle(2.0), Rectangle(3.0, 4.0), Triangle(6.0, 1.5))
    val count   = shapes.size                             // inlay -> : Int
    val biggest = shapes.maxBy(_.area)                    // inlay -> : Shape
    val names   = shapes.map(_.name)                      // inlay -> : List[String]
    val area    = total(shapes)                           // Alt-Q T on `area` -> Double

    // Call graph:  Alt-Q B on `total` (above) -> its callers (main);
    //              Alt-Q G on `describe` below -> what it calls (name, area).
    println(describe(biggest))                            // Alt-Q S inside (...) -> param list
    shapes.foreach(s => println(describe(s)))             // Alt-Q C after `shapes.` -> members
    println(s"$count shapes, total area $area")
    names.foreach(println)                                // Alt-Q N on `names` -> rename it
                                                          // Alt-Q F -> reformat the whole file
