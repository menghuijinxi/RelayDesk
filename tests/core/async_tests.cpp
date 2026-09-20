#include "core/platform/async.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

int fail(const std::string& message)
{
    std::cerr << message << '\n';
    return 1;
}

class AsyncShutdownGuard {
public:
    ~AsyncShutdownGuard()
    {
        core::async::shutdown();
    }
};

int convertsWorkerExceptionToFailureResult()
{
    bool completionCalled = false;
    core::async::Result<void> completionResult;
    const bool accepted = core::async::runOnce(
        "async.worker.exception",
        []() -> core::async::Result<void> {
            throw std::runtime_error("expected worker failure");
        },
        [&completionCalled, &completionResult](
            const core::async::Result<void>& result) {
            completionCalled = true;
            completionResult = result;
        });
    if (!accepted) {
        return fail("Async worker exception test task was not accepted.");
    }

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!completionCalled && std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(core::async::dispatchReady());
        std::this_thread::sleep_for(10ms);
    }

    if (!completionCalled) {
        return fail("Async worker exception did not reach completion callback.");
    }
    if (completionResult.ok) {
        return fail("Async worker exception was reported as success.");
    }
    if (completionResult.error != "expected worker failure") {
        return fail("Async worker exception message was not preserved.");
    }
    return 0;
}

} // namespace

int main()
{
    AsyncShutdownGuard shutdownGuard;
    return convertsWorkerExceptionToFailureResult();
}
