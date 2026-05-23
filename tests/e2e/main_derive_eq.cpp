// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "derive_eq.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::derive_eq;

    // Structural equality on a struct.
    const auto a = make_point(1, 2);
    const auto b = make_point(1, 2);
    const auto c = make_point(1, 9);
    if (!(a == b) || (a == c)) {
        std::println("Point== wrong: a==b? {}, a==c? {}", a == b, a == c);
        return EXIT_FAILURE;
    }

    // Structural equality on a payloaded enum: same variant + same
    // payload bytes is true; different variant is false.
    const auto s1 = make_circle(1.5);
    const auto s2 = make_circle(1.5);
    const auto s3 = make_circle(2.5);
    const Shape rect{Shape::rect_t{1.5, 1.5}};
    if (!(s1 == s2) || (s1 == s3) || (s1 == rect)) {
        std::println(
            "Shape== wrong: s1==s2? {}, s1==s3? {}, s1==rect? {}", s1 == s2, s1 == s3, s1 == rect);
        return EXIT_FAILURE;
    }

    std::println("derive(Eq) OK");
    return EXIT_SUCCESS;
}
