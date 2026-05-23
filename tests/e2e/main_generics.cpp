// End-to-end harness for examples/generics.vst.
// Compiled by CTest after `vestra build` has produced generics.hpp/.cpp.

#include "generics.hpp"

#include <cstdio>

int main() {
    std::printf("compute() = %d\n", examples::generics::compute());
    return 0;
}
