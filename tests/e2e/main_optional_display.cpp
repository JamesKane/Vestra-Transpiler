// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "optional_display.hpp"

#include <cstdlib>
#include <optional>
#include <print>
#include <string_view>

int main() {
    using namespace examples::optional_display;

    // .some / .none for a primitive Optional<Int32> in a Str splice.
    auto s_some = renderName(std::optional<std::string_view>{std::string_view{"alice"}},
                             std::optional<std::int32_t>{3});
    if (s_some != "name=alice count=3") {
        std::println("primitive .some wrong: '{}'", s_some);
        return EXIT_FAILURE;
    }
    auto s_none = renderName(std::optional<std::string_view>{}, std::optional<std::int32_t>{});
    if (s_none != "name=nil count=nil") {
        std::println("primitive .none wrong: '{}'", s_none);
        return EXIT_FAILURE;
    }

    // derive(Debug) struct in Optional: the Optional formatter
    // delegates to Point's std::formatter spec on .some, "nil" on .none.
    auto p_some = renderPoint(std::optional<Point>{Point{.x = 3, .y = 4}});
    if (p_some != "p=Point{x: 3, y: 4}") {
        std::println("Point .some wrong: '{}'", p_some);
        return EXIT_FAILURE;
    }
    auto p_none = renderPoint(std::optional<Point>{});
    if (p_none != "p=nil") {
        std::println("Point .none wrong: '{}'", p_none);
        return EXIT_FAILURE;
    }

    std::println("optional display OK");
    return EXIT_SUCCESS;
}
