// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "optchain.hpp"

#include <cstdlib>
#include <optional>
#include <print>
#include <string_view>

int main() {
    using namespace examples::optchain;

    Profile prof{.name = "Alice", .age = 30};
    User u_full{.profile = prof};
    User u_no_profile{.profile = std::nullopt};

    // Full chain: every link present → wrapped result.
    auto n1 = get_name(u_full);
    if (!n1.has_value() || std::string_view{*n1} != "Alice") {
        std::println("get_name(full) wrong");
        return EXIT_FAILURE;
    }

    // Middle link missing: chain short-circuits to nullopt.
    auto n2 = get_name(u_no_profile);
    if (n2.has_value()) {
        std::println("get_name(no_profile) should be nullopt");
        return EXIT_FAILURE;
    }

    // Outermost missing: chain short-circuits to nullopt.
    auto n3 = get_name(std::nullopt);
    if (n3.has_value()) {
        std::println("get_name(nullopt) should be nullopt");
        return EXIT_FAILURE;
    }

    // Numeric field via the same chain shape.
    auto a = get_age(u_full);
    if (!a.has_value() || *a != 30) {
        std::println("get_age(full) wrong");
        return EXIT_FAILURE;
    }

    // One-link `?.` over an already-Optional field flattens — the
    // result is Optional<Profile>, not Optional<Optional<Profile>>.
    auto p = get_profile(u_full);
    if (!p.has_value()) {
        std::println("get_profile(full) wrong");
        return EXIT_FAILURE;
    }
    if (std::string_view{p->name} != "Alice") {
        std::println("get_profile(full) name wrong");
        return EXIT_FAILURE;
    }
    if (get_profile(u_no_profile).has_value()) {
        std::println("get_profile(no_profile) should be nullopt");
        return EXIT_FAILURE;
    }

    std::println("optchain OK");
    return EXIT_SUCCESS;
}
