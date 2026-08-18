#pragma once

// Embedded extensions for embg::Graph.
//
// These add the primitives that LangGraph lacks but edge/embedded systems require:
//   - Confidence-gated transitions  (probabilistic guard conditions)
//   - Hard timeout wrappers         (deterministic deadline enforcement)
//   - Degraded mode graph selection (capability-layered execution)
//
// Timeout implementation uses std::async — suitable for POSIX/desktop targets.
// In static-allocation mode, the async path is compiled out (no heap).
// For bare-metal RTOS, replace with: RTOS task + notification + vTaskDelete.

#include "graph.hpp"
#include <chrono>
#include <stdexcept>

#if !defined(EMBG_STATIC_ALLOC)
#include <future>
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

template<embg::GraphState S, typename Cfg = embg::Config>
embg::detail::NodeFn<S, Cfg> with_timeout(
    embg::detail::NodeFn<S, Cfg>              fn,
    std::chrono::milliseconds                deadline,
    embg::detail::NodeFn<S, Cfg>              on_timeout
) {
#if defined(EMBG_STATIC_ALLOC)
    // Static mode: no std::async (avoids thread + heap allocation).
    // Runs fn inline without a deadline — the node itself must be bounded.
    // Full RTOS-task-based timeout is Phase 2 of the hardening roadmap.
    return [fn = std::move(fn), on_timeout = std::move(on_timeout), deadline](S& state) mutable {
        (void)deadline;  // unused — no async enforcement in static mode
        fn(state);
    };
#else
    return [fn        = std::move(fn),
            deadline,
            on_timeout = std::move(on_timeout)](S& state) mutable {

        S local = state;
        auto fut = std::async(std::launch::async, [&fn, &local] { fn(local); });

        if (fut.wait_for(deadline) == std::future_status::timeout) {
            on_timeout(state);
        } else {
            state = std::move(local);
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
