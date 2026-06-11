// scala-cli build config for this example.
//
// Pin the LTS Scala 3.3 line, NOT scala-cli's bleeding-edge default (3.8.x):
// the newest presentation compilers occasionally crash with
//   java.lang.AssertionError: assertion failed: asTerm called on not-a-Term
// which makes hover/completion/inlay-hints flaky.  The 3.3 LTS PC is stable.
//> using scala 3.3.7
//
// Pin the JVM too: Metals' Scala 3 presentation compiler does not run on the
// newest JDKs (24+ crash it at start-up).  Temurin 21 LTS keeps both the build
// and Metals on a supported JVM; scala-cli downloads and caches it.  (xwpe also
// auto-pins JAVA_HOME for Metals when the default JDK is too new.)
//> using jvm temurin:21
//
// Warn on unused imports/locals so the unused imports in actions.scala produce
// diagnostics (Alt-Q E marks them) and Alt-Q A on an import line offers
// "Organize imports" / "Remove unused" -- the server-command code action.
//> using option -Wunused:all
