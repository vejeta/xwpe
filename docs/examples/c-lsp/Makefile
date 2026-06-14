# Build the C/C++ language-server demo.
#
# IMPORTANT: this is a THREE-file program -- main.cpp + shapes.cpp + actions.cpp.
# Building or debugging main.cpp ALONE does not link: the compiler is happy
# (it produces main.o) but the linker leaves Shape/Circle/Rectangle/Triangle
# undefined and fails with "collect2: error: ld returned 1 exit status".
# That is expected for a multi-file project, not an xwpe bug.
#
# So link all three translation units together -- either with this Makefile
# (`make` in a terminal) or, inside xwpe, by opening a Project (.prj) that lists
# the three sources instead of pressing F9 on a single file.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -g
SRC       = main.cpp shapes.cpp actions.cpp
OBJ       = $(SRC:.cpp=.o)
BIN       = clsp-demo

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ)

%.o: %.cpp shapes.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
