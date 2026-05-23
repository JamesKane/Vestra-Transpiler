// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "derive_hash.hpp"

#include <cstdlib>
#include <print>
#include <unordered_map>

int main() {
    using namespace examples::derive_hash;

    // Bucket-place a few Points and read them back. If the hash works
    // and the equality works, this round-trips.
    std::unordered_map<Point, int> by_point;
    by_point[make_point(1, 2)] = 42;
    by_point[make_point(3, 4)] = 99;
    by_point[make_point(1, 2)] = 7;  // overwrite: same key

    if (by_point.size() != 2) {
        std::println("size wrong: {}", by_point.size());
        return EXIT_FAILURE;
    }
    if (by_point[make_point(1, 2)] != 7 || by_point[make_point(3, 4)] != 99) {
        std::println(
            "lookup wrong: ({},{})", by_point[make_point(1, 2)], by_point[make_point(3, 4)]);
        return EXIT_FAILURE;
    }

    // Different Points should generally hash to different buckets;
    // we can't assert that directly without poking the implementation,
    // but the call should at least compile and produce some std::size_t.
    const auto h12 = std::hash<Point>{}(make_point(1, 2));
    const auto h34 = std::hash<Point>{}(make_point(3, 4));
    if (h12 == h34) {
        // Theoretically possible but vanishingly unlikely for distinct
        // inputs with a real hash combine; treat a hit as a failure
        // since boost-combine wouldn't collide on these.
        std::println("hash collision on disjoint inputs: {} == {}", h12, h34);
        return EXIT_FAILURE;
    }

    std::println("derive(Hash) OK");
    return EXIT_SUCCESS;
}
