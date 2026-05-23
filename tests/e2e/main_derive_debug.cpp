// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "derive_debug.hpp"

#include <cstdlib>
#include <format>
#include <print>
#include <string>

int main() {
    using namespace examples::derive_debug;

    const auto p = make_point(1, 2);
    const auto color = Color::green;
    const auto shape_c = make_circle(1.5);
    const Shape shape_r{Shape::rect_t{3.0, 4.0}};
    const Shape shape_p{Shape::point_t{}};

    const std::string s_p = std::format("{}", p);
    const std::string s_color = std::format("{}", color);
    const std::string s_c = std::format("{}", shape_c);
    const std::string s_r = std::format("{}", shape_r);
    const std::string s_pt = std::format("{}", shape_p);

    if (s_p != "Point{x: 1, y: 2}") {
        std::println("Point wrong: {}", s_p);
        return EXIT_FAILURE;
    }
    if (s_color != "Color.green") {
        std::println("Color wrong: {}", s_color);
        return EXIT_FAILURE;
    }
    if (s_c != "Shape::circle{radius: 1.5}") {
        std::println("Shape::circle wrong: {}", s_c);
        return EXIT_FAILURE;
    }
    if (s_r != "Shape::rect{width: 3, height: 4}") {
        std::println("Shape::rect wrong: {}", s_r);
        return EXIT_FAILURE;
    }
    if (s_pt != "Shape::point") {
        std::println("Shape::point wrong: {}", s_pt);
        return EXIT_FAILURE;
    }

    std::println("derive(Debug) OK");
    return EXIT_SUCCESS;
}
