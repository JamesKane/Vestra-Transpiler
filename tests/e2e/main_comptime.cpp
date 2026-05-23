// End-to-end harness for examples/comptime.vst.
// Compiled by CTest after `vestra build` has produced comptime.hpp/.cpp.

#include "comptime.hpp"

#include <print>

int main() {
    std::println("compute() = {}", examples::consts::compute());
    return 0;
}
