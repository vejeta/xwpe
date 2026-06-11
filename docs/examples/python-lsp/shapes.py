"""Domain types used by main.py.  The cross-file LSP actions live here."""

import math
from abc import ABC, abstractmethod


# Alt-Q R on `Shape` -> all references;
# Alt-Q J on `Shape` -> SUBTYPES (Circle / Rectangle / Triangle)
class Shape(ABC):
    @abstractmethod
    def area(self) -> float:        # Alt-Q I on `area` -> implementations
        ...

    @abstractmethod
    def name(self) -> str:          # Alt-Q U on `name` -> uses in this file
        ...


# Alt-Q D from main.py lands here;
# Alt-Q K on `Circle` -> SUPERTYPES (Shape)
class Circle(Shape):
    def __init__(self, radius: float) -> None:
        self.radius = radius

    def area(self) -> float:        # Alt-Q H on `area` -> (method) def area(self) -> float
        return math.pi * self.radius ** 2   # Alt-Q D on `pi` -> the math stub (read-only)

    def name(self) -> str:
        return "circle"


class Rectangle(Shape):
    def __init__(self, width: float, height: float) -> None:
        self.width = width
        self.height = height

    def area(self) -> float:
        return self.width * self.height

    def name(self) -> str:
        return "rectangle"


class Triangle(Shape):
    def __init__(self, base: float, height: float) -> None:
        self.base = base
        self.height = height

    def area(self) -> float:
        return 0.5 * self.base * self.height

    def name(self) -> str:
        return "triangle"
