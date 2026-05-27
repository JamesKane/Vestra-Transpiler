// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "panic_handler.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <print>

namespace {

// Run `body` in a forked child. The child aborts via panic; the
// parent waitpid's for the signal and reads back the static
// `handler_ran` flag from a shared-memory mapping.
template <class F>
[[nodiscard]] bool child_aborts_with_handler(F&& body) {
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
    using namespace examples::panic_handler;

    // Pre-flight: confirm the handler isn't registered as a side
    // effect of just including the header. handler_ran starts at 0.
    if (handler_ran.load(std::memory_order_seq_cst) != 0) {
        std::println("handler_ran should start at 0");
        return EXIT_FAILURE;
    }

    // The static-init block in the generated source assigned
    // &vestraPanic into __vstr::panic_handler. Confirm by checking
    // the slot is non-null at this point.
    if (__vstr::panic_handler == nullptr) {
        std::println("panic_handler slot should be set after static init");
        return EXIT_FAILURE;
    }

    // In a child process: cause_panic() triggers the runtime
    // panic, which delegates to our handler, which sets the flag
    // and then calls abort(). The fork+SIGABRT pattern lets us
    // observe the abort without killing the test driver.
    //
    // The flag check inside the child is the load-bearing part —
    // a non-zero flag means the handler ran before abort(). Since
    // handler_ran lives in process-private memory the parent can't
    // see it; the SIGABRT signal is our proxy that cause_panic
    // reached the handler's abort() call.
    if (!child_aborts_with_handler([] { cause_panic(); })) {
        std::println("cause_panic should have aborted via the handler");
        return EXIT_FAILURE;
    }

    std::println("panic_handler OK");
    return EXIT_SUCCESS;
}
