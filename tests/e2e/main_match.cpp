// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "match.hpp"

#include <cmath>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::matchdemo;

    // Direct-call check for each variant. Computes individually so a
    // wrong dispatch shows up on the failing line, not as a noisy sum.
    const double c_area = area(Shape{Shape::circle_t{1.0}});
    const double r_area = area(Shape{Shape::rect_t{3.0, 4.0}});
    const double p_area = area(Shape{Shape::point_t{}});

    constexpr double Pi = 3.141592653589793;
    constexpr double Tol = 1e-12;
    if (std::abs(c_area - Pi) > Tol || std::abs(r_area - 12.0) > Tol || std::abs(p_area) > Tol) {
        std::println("per-variant area wrong: {} {} {}", c_area, r_area, p_area);
        return EXIT_FAILURE;
    }

    // sample_total() builds each variant and sums the areas; rechecks
    // that the match dispatches the right body when the variant comes
    // through a let-binding rather than a freshly constructed value.
    const double total = sample_total();
    if (std::abs(total - (Pi + 12.0)) > Tol) {
        std::println("sample_total wrong: {}", total);
        return EXIT_FAILURE;
    }

    // §17.7 or-pattern over payloaded-enum cases: keyDown / keyUp share the
    // arm body and each binds its own payload field to `n`.
    if (keycode(Event{Event::keyDown_t{65}}) != 65 || keycode(Event{Event::keyUp_t{66}}) != 66
        || keycode(Event{Event::quit_t{}}) != -1) {
        std::println("or-pattern keycode dispatch wrong");
        return EXIT_FAILURE;
    }

    std::println("match OK: total = {:.6f}", total);
    return EXIT_SUCCESS;
}
