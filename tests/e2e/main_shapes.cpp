// End-to-end harness for examples/shapes.vst.
// Compiled by CTest after `vestra build` has produced shapes.hpp/.cpp.

#include "shapes.hpp"

#include <cstdio>

int main() {
    std::printf("compute() = %d\n", examples::shapes::compute());
    return 0;
}
