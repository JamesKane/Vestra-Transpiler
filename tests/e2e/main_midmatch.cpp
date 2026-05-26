// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "midmatch.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::midmatch;

    // --- bare enum: return position --------------------------------
    if (dispatch_return(Cmd::noop).value() != 0) {
        std::println("noop return wrong");
        return EXIT_FAILURE;
    }
    if (dispatch_return(Cmd::fetch).value() != 42) {
        std::println("fetch return wrong");
        return EXIT_FAILURE;
    }
    // panic arm: the throw escapes the cond-hoist lambda and then the
    // function via the outer propagation check.
    if (dispatch_return(Cmd::panic).has_value()) {
        std::println("panic return leaked");
        return EXIT_FAILURE;
    }

    // --- bare enum: let-binding position ---------------------------
    if (dispatch_let(Cmd::noop).value() != 100) {
        std::println("noop let wrong: {}", dispatch_let(Cmd::noop).value());
        return EXIT_FAILURE;
    }
    if (dispatch_let(Cmd::fetch).value() != 142) {
        std::println("fetch let wrong: {}", dispatch_let(Cmd::fetch).value());
        return EXIT_FAILURE;
    }
    if (dispatch_let(Cmd::panic).has_value()) {
        std::println("panic let leaked");
        return EXIT_FAILURE;
    }

    // --- payloaded enum -------------------------------------------
    Shape c{Shape::circle_t{3}};     // → tryDouble(3) = 6
    Shape r{Shape::rect_t{4, 5}};    // → tryDouble(20) = 40
    Shape e{Shape::empty_t{}};       // → 0
    Shape bad{Shape::circle_t{-1}};  // → tryDouble(-1) throws

    if (area(c).value() != 6) {
        std::println("circle wrong: {}", area(c).value());
        return EXIT_FAILURE;
    }
    if (area(r).value() != 40) {
        std::println("rect wrong: {}", area(r).value());
        return EXIT_FAILURE;
    }
    if (area(e).value() != 0) {
        std::println("empty wrong: {}", area(e).value());
        return EXIT_FAILURE;
    }
    if (area(bad).has_value()) {
        std::println("bad path leaked");
        return EXIT_FAILURE;
    }

    std::println("midmatch OK");
    return EXIT_SUCCESS;
}
