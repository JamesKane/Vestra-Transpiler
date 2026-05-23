// End-to-end harness for examples/generics.vst.
// Compiled by CTest after `vestra build` has produced generics.hpp/.cpp.

#include "generics.hpp"

#include <print>

int main() {
    std::println("compute() = {}", examples::generics::compute());
    return 0;
}
