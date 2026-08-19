#pragma once
 
// Embedded extensions for embg::Graph.
//
// These add the primitives that LangGraph lacks but edge/embedded systems require:
//   - Confidence-gated transitions  (probabilistic guard conditions)
//   - Hard timeout wrappers         (deterministic deadline enforcement)
//   - Degraded mode graph selection (capability-layered execution)
//
// Timeout implementation:
//   Default mode:  std::thread + std::atomic + shared_ptr — truly preemptive.
//                  On timeout, the worker is detached and on_timeout fires
//                  immediately. The caller does NOT block waiting for fn.
//   Static mode:   Runs fn inline, then checks elapsed time post-hoc.
//                  If fn overran the deadline, on_timeout fires as fallback.
//                  NOT preemptive — fn runs to completion. Documented.
//   Bare-metal:    Replace with RTOS task + notification + vTaskDelete.
 
#include "graph.hpp"
#include <chrono>
#include <stdexcept>
 
#if !defined(EMBG_STATIC_ALLOC)
#include <atomic>
#include <memory>
#include <thread>
#endif
 
namespace embg::embedded {
 
// ─── Concepts ─────────────────────────────────────────────────────────────────
 
template<typename S>
concept ConfidenceState = embg::GraphState<S> && requires(const S s) {
    { s.last_confidence } -> std::convertible_to<double>;
};
 
// ─── Confidence-gated router ──────────────────────────────────────────────────
 
template<ConfidenceState S, typename Cfg = embg::Config>
embg::detail::RouterFn<S, Cfg> confidence_router(
    double      threshold,
    const char* above,
    const char* below
) {
    return [threshold, above, below](const S& state) -> embg::detail::String<Cfg> {
        return state.last_confidence >= threshold ? above : below;
    };
}
 
// ─── Timeout wrapper ──────────────────────────────────────────────────────────
 
template<embg::GraphState S, typename Cfg = embg::Config,
         typename Fn, typename OnTimeout>
embg::detail::NodeFn<S, Cfg> with_timeout(
    Fn&&                       fn,
    std::chrono::milliseconds deadline,
    OnTimeout&&                on_timeout
) {
#if defined(EMBG_STATIC_ALLOC)
    // Static mode: no std::thread (avoids heap allocation for thread + shared_ptr).
    //
    // Runs fn inline, then checks if it exceeded the deadline. If so, runs
    // on_timeout as a post-hoc fallback.
    //
    // IMPORTANT: This is NOT preemptive — fn runs to completion regardless.
    // on_timeout fires AFTER fn if fn overran its budget. This gives the
    // deterministic fallback behavior without requiring thread/heap support.
    // For true preemptive timeout, use default mode or integrate RTOS task
    // primitives (Phase 2 of the hardening roadmap).
    return [fn = std::forward<Fn>(fn),
            on_timeout = std::forward<OnTimeout>(on_timeout),
            deadline](S& state) mutable {
        const auto start = std::chrono::steady_clock::now();
        try {
            fn(state);
        } catch (...) {
            // fn threw — run fallback immediately
            on_timeout(state);
            return;
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > deadline) {
            on_timeout(state);
        }
    };
#else
    // Default mode: truly preemptive timeout via std::thread + std::atomic.
    //
    // fn runs on a separate thread operating on a shared_ptr<S> copy of state.
    // If it completes within the deadline, the result is committed to state.
    // If it exceeds the deadline, on_timeout runs immediately on the caller
    // thread, and the worker thread is detached (it continues running on its
    // copy of state, which is safely owned via shared_ptr — no dangling refs).
    //
    // The detached thread cannot be cancelled — it will eventually finish and
    // its resources will be cleaned up when the shared_ptr refcount hits zero.
    // This is a fundamental limitation of non-cooperative cancellation in C++.
    // For cooperative cancellation, fn would need to check a cancellation token.
    //
    // If fn throws, the exception is caught in the worker thread. done is NOT
    // set, so the timeout path fires on_timeout. The partially-mutated local
    // copy is discarded (never committed to state).
    return [fn        = std::forward<Fn>(fn),
            deadline,
            on_timeout = std::forward<OnTimeout>(on_timeout)](S& state) mutable {
 
        auto local = std::make_shared<S>(state);
        auto done  = std::make_shared<std::atomic<bool>>(false);
 
        std::thread worker([local, fn, done]() {
            try {
                fn(*local);
                done->store(true, std::memory_order_release);
            } catch (...) {
                // fn threw — don't set done, let timeout path fire on_timeout.
                // local is partially mutated but will be discarded (never committed).
            }
        });
 
        const auto start = std::chrono::steady_clock::now();
        bool completed = false;
 
        while (std::chrono::steady_clock::now() - start < deadline) {
            if (done->load(std::memory_order_acquire)) {
                completed = true;
                break;
            }
            std::this_thread::yield();
        }
 
        if (completed) {
            worker.join();
            state = std::move(*local);
        } else {
            // Deadline exceeded — detach worker so we don't block.
            // Worker continues on its shared_ptr copy; no data race on state.
            worker.detach();
            on_timeout(state);
        }
    };
#endif
}
 
// ─── Capability levels ────────────────────────────────────────────────────────
 
enum class CapabilityLevel {
    Full,
    Degraded,
    MinimalSafe
};
 
// ─── Degraded-mode runner ─────────────────────────────────────────────────────
 
template<embg::GraphState S, typename Cfg = embg::Config>
class DegradedModeRunner {
public:
    using LevelMap = embg::detail::Map<CapabilityLevel, embg::Graph<S, Cfg>*, Cfg, Cfg::MaxCapLevels>;
 
    DegradedModeRunner& add_level(CapabilityLevel level, embg::Graph<S, Cfg>& graph) {
        levels_.insert_or_assign(level, &graph);
        return *this;
    }
 
    void run(S& state, CapabilityLevel level, std::size_t max_steps = 100) {
        auto it = levels_.find(level);
        if (it == levels_.end())
            throw std::runtime_error(
                "embg::embedded::DegradedModeRunner: no graph registered for level");
        it->second->run(state, max_steps);
    }
 
private:
    LevelMap levels_;
};
 
} // namespace embg::embedded
