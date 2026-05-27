// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "match_guards.hpp"

#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::match_guards;

    // (1) where-guards on payloaded enum binds. A positive-radius
    // circle returns r; a degenerate one falls to the catch arm.
    if (auto v = circle_or_invalid(Shape{Shape::circle_t{5}}); v != 5) {
        std::println("circle(5) expected 5, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = circle_or_invalid(Shape{Shape::circle_t{0}}); v != -1) {
        std::println("circle(0) expected -1, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = circle_or_invalid(Shape{Shape::circle_t{-3}}); v != -1) {
        std::println("circle(-3) expected -1, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = circle_or_invalid(Shape{Shape::rect_t{4, 5}}); v != 0) {
        std::println("rect expected 0, got {}", v);
        return EXIT_FAILURE;
    }

    // (3) let-pattern + guard combination over a payload. Diagonal
    // points fire the first arm; off-diagonal fall to the catch.
    if (auto v = diagonal_only(Shape{Shape::point_t{7, 7}}); v != 7) {
        std::println("diagonal(7,7) expected 7, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = diagonal_only(Shape{Shape::point_t{3, 4}}); v != -1) {
        std::println("diagonal(3,4) expected -1, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = diagonal_only(Shape{Shape::circle_t{99}}); v != 0) {
        std::println("diagonal default expected 0, got {}", v);
        return EXIT_FAILURE;
    }

    // (2) tuple wildcards. (1, _) matches anything with 1 first;
    // (_, 2) matches anything with 2 second; default catches the
    // rest. The first-match-wins order matters: (1, 2) hits arm 1.
    if (auto v = tuple_match(std::tuple{1, 99}); v != 100) {
        std::println("(1, 99) expected 100, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = tuple_match(std::tuple{99, 2}); v != 200) {
        std::println("(99, 2) expected 200, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = tuple_match(std::tuple{1, 2}); v != 100) {
        std::println("(1, 2) expected 100 (first arm wins), got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = tuple_match(std::tuple{99, 99}); v != 0) {
        std::println("(99, 99) expected default 0, got {}", v);
        return EXIT_FAILURE;
    }

    // Bare enum + guard. The if-chain dispatches correctly: a
    // running mode with a positive tick fires the first arm,
    // otherwise the catch.
    if (auto v = tick(Mode::running, 5); v != 6) {
        std::println("tick(running, 5) expected 6, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = tick(Mode::running, 0); v != 1) {
        std::println("tick(running, 0) expected 1, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = tick(Mode::idle, 5); v != 0) {
        std::println("tick(idle) expected 0, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = tick(Mode::stopped, 5); v != -1) {
        std::println("tick(stopped) expected -1, got {}", v);
        return EXIT_FAILURE;
    }

    std::println("match_guards OK");
    return EXIT_SUCCESS;
}
