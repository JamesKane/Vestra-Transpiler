// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "panic.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <optional>
#include <print>

namespace {

// Run `body` in a forked child. Returns true iff the child terminated
// abnormally with SIGABRT (the std::abort signal). The parent
// continues regardless of the child's outcome.
template <class F>
[[nodiscard]] bool child_aborts(F&& body) {
    pid_t pid = fork();
    if (pid < 0) {
        std::println("fork failed");
        std::exit(EXIT_FAILURE);
    }
    if (pid == 0) {
        // Suppress the child's stderr so the test log stays clean.
        (void)freopen("/dev/null", "w", stderr);
        body();
        // If body returned, the panic contract was violated.
        std::exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

}  // namespace

int main() {
    using namespace examples::panic;

    // --- happy paths -----------------------------------------------
    if (divideOrPanic(10, 2) != 5) {
        std::println("divideOrPanic(10, 2) wrong");
        return EXIT_FAILURE;
    }
    if (defaultLatencyMs(Mode::fast) != 1) {
        std::println("defaultLatencyMs(fast) wrong");
        return EXIT_FAILURE;
    }
    if (forced(std::optional<std::int32_t>{42}) != 42) {
        std::println("forced(some) wrong");
        return EXIT_FAILURE;
    }

    // --- panic paths via fork+abort --------------------------------
    if (!child_aborts([]() { (void)divideOrPanic(10, 0); })) {
        std::println("divideOrPanic(10, 0) did not abort");
        return EXIT_FAILURE;
    }
    if (!child_aborts([]() { giveUp(); })) {
        std::println("giveUp() did not abort");
        return EXIT_FAILURE;
    }
    if (!child_aborts([]() { (void)forced(std::nullopt); })) {
        std::println("forced(nil) did not abort");
        return EXIT_FAILURE;
    }

    std::println("panic OK");
    return EXIT_SUCCESS;
}
