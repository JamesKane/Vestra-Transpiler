// End-to-end harness for examples/comptime.vst.
// Compiled by CTest after `vestra build` has produced comptime.hpp/.cpp.

#include "comptime.hpp"

#include <cstdio>

int main() {
    std::printf("compute() = %d\n", examples::consts::compute());
    return 0;
}
