// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "display.hpp"

#include <cstdlib>
#include <print>
#include <string>

int main() {
    using namespace examples::display;
    Point p{.x = 1, .y = 2};
    Shape s{Shape::circle_t{1.5}};
    auto got = render(p, s, 7);
    const std::string want = "p = Point{x: 1, y: 2}, s = Shape::circle{radius: 1.5}, count = 7";
    if (got != want) {
        std::println("got : {}", got);
        std::println("want: {}", want);
        return EXIT_FAILURE;
    }
    std::println("display OK");
    return EXIT_SUCCESS;
}
