"""Code-actions playground (Alt-Q A).

Python's code actions are the most server-dependent of the wired languages:

* pyright offers "Organize Imports" and quick-fixes attached to diagnostics
  (e.g. add a missing import).
* pylsp offers refactors -- "Extract variable/method", "Inline" -- only when the
  optional rope plugin is installed (python3-rope), and quick-fixes from
  pyflakes/pycodestyle when those are installed.

Put the cursor on a marked spot, run Alt-Q A, pick an entry; the buffer is
rewritten in place (F2 saves, Ctrl-U undoes, Ctrl-R redoes).
"""

import os       # deliberately unused -> Alt-Q A on the import line: Organize Imports
import sys       # (unused on purpose)


# Alt-Q A on the `"hi " + who` expression (pylsp + rope) -> "Extract variable".
def greet(who):
    return "hi " + who          # Alt-Q A here -> extract variable (rope)


# Alt-Q A on the body (pylsp + rope) -> "Extract method".
def area_of(width, height):
    return width * height        # Alt-Q A here -> extract method (rope)
