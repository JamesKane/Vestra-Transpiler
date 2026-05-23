// End-to-end harness for examples/hello.vst.
// Compiled by CTest after `vestra build` has produced hello.hpp/.cpp.

#include "hello.hpp"

#include <cstdio>

int main() {
    std::printf("compute() = %d\n", examples::hello::compute());
    return 0;
}
