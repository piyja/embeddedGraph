// Test: with_timeout deadline enforcement
//
// Verifies the two critical bugs fixed in this MR:
//   1. Default mode: with_timeout must NOT block for the full fn duration
//   2. Static mode:  with_timeout must call on_timeout when fn overruns
//
// Build:
//   g++ -std=c++20 -Wall -Wextra -I include tests/test_with_timeout.cpp -o build/test_with_timeout
//   g++ -std=c++20 -Wall -Wextra -DEMBG_STATIC_ALLOC -I include tests/test_with_timeout.cpp -o build_static/test_with_timeout

#include <embg/embedded.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

struct TestState {
    std::string result      = {};
    double      last_confidence = 0.0;
    bool        timeout_fired   = false;
    bool        fn_completed    = false;
};

static_assert(embg::embedded::ConfidenceState<TestState>);

int main() {
    std::cout << "=== test_with_timeout ===\n";

    // ── Test 1: fn completes within deadline → result committed ──────────────
    {
        TestState s;
        auto fast_fn = [](TestState& s) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            s.result = "fast result";
            s.fn_completed = true;
        };
        auto on_timeout = [](TestState& s) {
            s.timeout_fired = true;
            s.result = "timeout fallback";
        };

        auto wrapped = embg::embedded::with_timeout<TestState>(
            fast_fn, std::chrono::milliseconds(100), on_timeout);

        auto start = std::chrono::steady_clock::now();
        wrapped(s);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        std::cout << "Test 1 (fast fn, 100ms deadline): "
                  << "elapsed=" << elapsed.count() << "ms"
                  << " result=\"" << s.result << "\""
                  << " timeout=" << s.timeout_fired << "\n";

        assert(s.result == "fast result");
        assert(!s.timeout_fired);
        assert(s.fn_completed);
        assert(elapsed < std::chrono::milliseconds(80));
        std::cout << "  PASS\n";
    }

    // ── Test 2: fn exceeds deadline → on_timeout fires, caller doesn't block ─
    {
        TestState s;
        auto slow_fn = [](TestState& s) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            s.result = "slow result";
            s.fn_completed = true;
        };
        auto on_timeout = [](TestState& s) {
            s.timeout_fired = true;
            s.result = "timeout fallback";
        };

        auto wrapped = embg::embedded::with_timeout<TestState>(
            slow_fn, std::chrono::milliseconds(50), on_timeout);

        auto start = std::chrono::steady_clock::now();
        wrapped(s);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        std::cout << "Test 2 (slow fn, 50ms deadline): "
                  << "elapsed=" << elapsed.count() << "ms"
                  << " result=\"" << s.result << "\""
                  << " timeout=" << s.timeout_fired << "\n";

        // THE critical assertion: elapsed must be closer to the deadline (50ms)
        // than to the fn duration (300ms). Before the fix, elapsed was ~300ms.
        assert(s.timeout_fired);
        assert(s.result == "timeout fallback");
#if !defined(EMBG_STATIC_ALLOC)
        // Default mode: preemptive — caller should NOT block for 300ms.
        // Allow generous slack for thread scheduling overhead.
        assert(elapsed < std::chrono::milliseconds(200));
        std::cout << "  PASS (preemptive — did not block for full 300ms)\n";
#else
        // Static mode: NOT preemptive — fn runs to completion (~300ms),
        // then on_timeout fires post-hoc. The elapsed time WILL be ~300ms.
        assert(elapsed >= std::chrono::milliseconds(250));
        std::cout << "  PASS (post-hoc — fn completed, then fallback fired)\n";
#endif
    }

    // ── Test 3: fn throws → on_timeout fires, no crash ───────────────────────
    {
        TestState s;
        auto throwing_fn = [](TestState&) {
            throw std::runtime_error("boom");
        };
        auto on_timeout = [](TestState& s) {
            s.timeout_fired = true;
            s.result = "timeout fallback";
        };

        auto wrapped = embg::embedded::with_timeout<TestState>(
            throwing_fn, std::chrono::milliseconds(100), on_timeout);

        auto start = std::chrono::steady_clock::now();
        wrapped(s);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        std::cout << "Test 3 (throwing fn, 100ms deadline): "
                  << "result=\"" << s.result << "\""
                  << " timeout=" << s.timeout_fired << "\n";

        assert(s.timeout_fired);
        assert(s.result == "timeout fallback");
#if !defined(EMBG_STATIC_ALLOC)
        assert(elapsed < std::chrono::milliseconds(50));
#endif
        std::cout << "  PASS\n";
    }

    std::cout << "\nAll tests passed.\n";
    return 0;
}
