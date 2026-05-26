// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "docatch_where.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <print>

namespace {

// Same fork+SIGABRT helper shape as the §10 panic e2e — the child
// runs the body, the parent verifies it terminated abnormally with
// SIGABRT (the abort signal __vstr::panic raises).
template <class F>
[[nodiscard]] bool child_aborts(F&& body) {
    pid_t pid = fork();
    if (pid < 0) {
        std::exit(EXIT_FAILURE);
    }
    if (pid == 0) {
        (void)freopen("/dev/null", "w", stderr);
        body();
        std::exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

}  // namespace

int main() {
    using namespace examples::docatch_where;

    if (attempt(0) != 42) {
        std::println("kind=0 should succeed");
        return EXIT_FAILURE;
    }
    if (attempt(1) != -1) {
        std::println("kind=1 (timeout) should hit the retry arm");
        return EXIT_FAILURE;
    }
    if (attempt(2) != -1) {
        std::println("kind=2 (refused) should hit the retry arm");
        return EXIT_FAILURE;
    }
    if (!child_aborts([] { (void)attempt(3); })) {
        std::println("kind=3 (fatal) should fall through the guard and panic");
        return EXIT_FAILURE;
    }

    std::println("docatch where OK");
    return EXIT_SUCCESS;
}
