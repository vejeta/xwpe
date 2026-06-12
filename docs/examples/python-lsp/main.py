# xwpe + Python language-server demo.  Open in programming mode:  wpe main.py
# xwpe uses whichever Python server is installed -- it prefers pyright and falls
# back to pylsp.  The FIRST language-server action starts it; Alt-Q ? opens the
# menu, Alt-Q <letter> runs one action.  A few actions have no single spot, so
# try them anywhere:  Alt-Q E (diagnostics), Alt-Q W (workspace symbol -- type
# e.g. "Shape"), and Alt-Q M (semantic colours -- toggle).
#
# With pyright you get the full set (type inference -> rich hover, completion,
# the type hierarchy Alt-Q K/J and implementation Alt-Q I).  With pylsp the jedi
# core (definition, hover, completion, references, rename) works; diagnostics
# need python3-pyflakes installed alongside.

from shapes import Circle, Rectangle, Shape, Triangle


# Alt-Q R on `total` -> references.  Alt-Q B on `total` -> its callers (main).
def total(shapes: list[Shape]) -> float:
    return sum(s.area() for s in shapes)        # Alt-Q I on `area` -> implementations


# Alt-Q G on `describe` -> outgoing calls (name, area, print).
def describe(s: Shape) -> None:
    print(f"{s.name()} with area {s.area():.2f}")   # Alt-Q S inside (...) -> the signature


def main() -> None:                             # Alt-Q O: file outline
    shapes = [Circle(2.0), Rectangle(3.0, 4.0), Triangle(6.0, 1.5)]  # Alt-Q T on `shapes`
    # Alt-Q Y (toggle): the vars below have no annotation, so the inferred type
    # pops in as a grey pill after each name.  Alt-Q H does one on demand.
    count = len(shapes)         # Alt-Q Y: the inferred type pops in ->>
    area = total(shapes)        # Alt-Q D on `total` -> its def above
    for s in shapes:
        describe(s)             # Alt-Q N on `describe` -> rename it
    # Alt-Q C after `shapes.` -> list members (append, sort, ...).
    print(f"{count} shapes, total area {area:.2f}")     # Alt-Q V grows the selection


main()
