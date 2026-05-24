// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "derive_clone.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::derive_clone;

    // clone() on a struct: receiver and result are independent values.
    Point p{.x = 1, .y = 2};
    auto q = bump_x(p);
    if (q.x != 101 || q.y != 2) {
        std::println("bump_x q wrong: ({}, {})", q.x, q.y);
        return EXIT_FAILURE;
    }
    if (p.x != 1 || p.y != 2) {
        std::println("bump_x mutated the input: ({}, {})", p.x, p.y);
        return EXIT_FAILURE;
    }

    // clone() on a sum-type enum: result equals input.
    Shape s{Shape::circle_t{2.5}};
    auto s2 = dup_shape(s);
    auto* alt_in = std::get_if<Shape::circle_t>(&s.value);
    auto* alt_out = std::get_if<Shape::circle_t>(&s2.value);
    if (alt_in == nullptr || alt_out == nullptr || alt_in->radius != alt_out->radius) {
        std::println("dup_shape circle round-trip wrong");
        return EXIT_FAILURE;
    }

    Shape sp{Shape::point_t{}};
    auto sp2 = dup_shape(sp);
    if (!std::holds_alternative<Shape::point_t>(sp2.value)) {
        std::println("dup_shape point round-trip wrong");
        return EXIT_FAILURE;
    }

    // clone() directly on a value yields an equal value too.
    Point p2 = p.clone();
    if (p2.x != p.x || p2.y != p.y) {
        std::println("direct Point.clone() wrong");
        return EXIT_FAILURE;
    }

    std::println("derive(Clone) OK");
    return EXIT_SUCCESS;
}
