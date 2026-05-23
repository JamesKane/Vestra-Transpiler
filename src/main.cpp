// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/driver/driver.hpp"

#include <iostream>
#include <span>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return vestra::driver::run(args, std::cout, std::cerr);
}
