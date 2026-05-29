#include <iostream>

void foo() {
    int x = "hello";
}

void bar() {
    std::string s = 42;
}

int main() {
    std::cout << "Hello from C++" << std::endl;
    foo();
    bar();
    return 0;
}
