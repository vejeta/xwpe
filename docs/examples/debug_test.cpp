#include <iostream>
#include <string>

std::string greet(const std::string &name) {
    std::string msg = "Hello, " + name + "!";
    return msg;
}

int main() {
    std::string name = "xwpe";
    std::string result = greet(name);
    std::cout << result << std::endl;
    std::cout << "Length: " << result.length() << std::endl;
    return 0;
}
