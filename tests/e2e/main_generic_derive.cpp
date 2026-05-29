// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for derives on generic types (§12.3 / §7). Exercises the
// inline operator== (Eq) and the partial-specialization std::hash (Hash)
// and std::formatter (Debug) through both the Vestra helpers and direct
// C++ use of the emitted templates, for a generic struct and a generic
// sum-type enum.

#include "generic_derive.hpp"

#include <cassert>
#include <cstdio>
#include <format>
#include <string>
#include <unordered_map>

namespace gd = examples::generic_derive;

int main() {
    gd::Pair<std::int32_t> a{.first = 1, .second = 2};
    gd::Pair<std::int32_t> b{.first = 1, .second = 2};
    gd::Pair<std::int32_t> c{.first = 9, .second = 9};

    // derive(Eq) via the Vestra helper.
    assert(gd::eqPair(a, b) == true);
    assert(gd::eqPair(a, c) == false);

    // derive(Debug) via Vestra interpolation.
    assert(gd::showPair(a) == "Pair{first: 1, second: 2}");

    // derive(Hash) — the partial specialization makes Pair<T> a usable key.
    std::unordered_map<gd::Pair<std::int32_t>, int> m;
    m[a] = 42;
    assert(m[b] == 42);

    // derive(Default) — value-init zeroes both fields.
    gd::Pair<std::int32_t> z{};
    assert(z.first == 0 && z.second == 0);

    // derive(Debug) via the C++ formatter directly.
    assert(std::format("{}", a) == "Pair{first: 1, second: 2}");

    // Generic sum-type enum: Eq + Hash + Debug.
    gd::Maybe<std::int32_t> j{.value = gd::Maybe<std::int32_t>::just_t{7}};
    gd::Maybe<std::int32_t> n{.value = gd::Maybe<std::int32_t>::nothing_t{}};
    assert(gd::eqMaybe(j, j) == true);
    assert(gd::eqMaybe(j, n) == false);
    assert(std::format("{}", j) == "Maybe::just{_0: 7}");
    std::unordered_map<gd::Maybe<std::int32_t>, int> mm;
    mm[j] = 3;
    mm[n] = 4;
    assert(mm[j] == 3 && mm[n] == 4);

    std::puts("generic_derive OK");
    return 0;
}
