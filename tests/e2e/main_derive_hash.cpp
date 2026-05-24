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

    // Payloaded-enum hash: distinct cases / distinct payloads should
    // all bucket separately; equal Shape values should collide.
    std::unordered_map<Shape, int> by_shape;
    by_shape[make_circle(1.0)] = 1;
    by_shape[make_circle(2.0)] = 2;
    by_shape[make_rect(1.0, 2.0)] = 3;
    by_shape[Shape{Shape::point_t{}}] = 4;
    by_shape[make_circle(1.0)] = 5;  // overwrite same key

    if (by_shape.size() != 4) {
        std::println("shape size wrong: {}", by_shape.size());
        return EXIT_FAILURE;
    }
    if (by_shape[make_circle(1.0)] != 5 || by_shape[make_circle(2.0)] != 2
        || by_shape[make_rect(1.0, 2.0)] != 3 || by_shape[Shape{Shape::point_t{}}] != 4) {
        std::println("shape lookup wrong");
        return EXIT_FAILURE;
    }

    // The alt-index seed should disambiguate cases with equal byte
    // patterns — circle(0.0) and an empty point shouldn't collide.
    const auto h_c0 = std::hash<Shape>{}(make_circle(0.0));
    const auto h_pt = std::hash<Shape>{}(Shape{Shape::point_t{}});
    if (h_c0 == h_pt) {
        std::println("alt-index seed missing: circle(0) == point");
        return EXIT_FAILURE;
    }

    std::println("derive(Hash) OK");
    return EXIT_SUCCESS;
}
