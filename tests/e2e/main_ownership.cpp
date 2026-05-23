// End-to-end harness for examples/ownership.vst.
// Compiled by CTest after `vestra build` has produced ownership.hpp/.cpp.

#include "ownership.hpp"

#include <cstdio>

int main() {
    std::printf("compute() = %d\n", examples::ownership::compute());
    return 0;
}
