#pragma once

// Embedded extensions for embg::Graph.
//
// These add the primitives that LangGraph lacks but edge/embedded systems require:
//   - Confidence-gated transitions  (probabilistic guard conditions)
//   - Hard timeout wrappers         (deterministic deadline enforcement)
//   - Degraded mode graph selection (capability-layered execution)
//
// Timeout implementation uses std::async — suitable for POSIX/desktop targets.
// For bare-metal RTOS, replace with: RTOS task + notification + vTaskDelete.

#include "graph.hpp"
#include <chrono>
#include <future>
#include <stdexcept>
#include <unordered_map>

namespace embg::embedded {

// ─── Concepts ─────────────────────────────────────────────────────────────────

// States used with confidence-gated routing must expose last_confidence.
// The inference node writes to this field; the router reads it.
template<typename S>
concept ConfidenceState = embg::GraphState<S> && requires(const S s) {
    { s.last_confidence } -> std::convertible_to<double>;
};

// ─── Confidence-gated router ──────────────────────────────────────────────────

// Returns a RouterFn that branches based on state.last_confidence.
//
// Usage:
//   graph.add_conditional_edge("run_inference",
//       embg::embedded::confidence_router<MyState>(0.85, "act", "fallback"));
//
template<ConfidenceState S>
embg::RouterFn<S> confidence_router(
    double      threshold,   // minimum confidence to trust the model
    std::string above,       // next node when confidence >= threshold
    std::string below        // next node when confidence <  threshold
) {
    return [threshold,
            above = std::move(above),
            below = std::move(below)](const S& state) -> std::string {
        return state.last_confidence >= threshold ? above : below;
    };
}

// ─── Timeout wrapper ──────────────────────────────────────────────────────────

// Wraps a node with a hard execution deadline.
// If fn() does not complete within deadline, on_timeout() runs instead.
//
// IMPORTANT: on POSIX/desktop this uses std::async. The timed-out async task
// continues running after on_timeout() is called — state is not accessed
// concurrently here only because on_timeout() receives the same state reference
// and the future is abandoned (destructor blocks until thread finishes at next
// scope exit). For production embedded use, replace with RTOS primitives that
// can cancel the task cleanly.
//
template<embg::GraphState S>
embg::NodeFn<S> with_timeout(
    embg::NodeFn<S>              fn,
    std::chrono::milliseconds  deadline,
    embg::NodeFn<S>              on_timeout
) {
    return [fn        = std::move(fn),
            deadline,
            on_timeout = std::move(on_timeout)](S& state) mutable {

        // Run fn on a separate thread so we can enforce a deadline.
        // We copy state in to avoid the shared-reference hazard on timeout.
        S local = state;
        auto fut = std::async(std::launch::async, [&fn, &local] { fn(local); });

        if (fut.wait_for(deadline) == std::future_status::timeout) {
            on_timeout(state);  // deterministic fallback on the caller thread
            // fut destructor will block until the thread finishes naturally
        } else {
            state = std::move(local);  // commit the result
        }
    };
}

// ─── Capability levels ────────────────────────────────────────────────────────

// Models the degraded-mode graph pattern: the system selects which graph to
// run based on current capability (power, connectivity, thermal state, etc.).
enum class CapabilityLevel {
    Full,        // all AI inference available, nominal resources
    Degraded,    // reduced — rule-based heuristics preferred
    MinimalSafe  // safety-critical minimum — halt or alert only
};

// ─── Degraded-mode runner ─────────────────────────────────────────────────────

// Selects one of several registered graphs based on the current capability level
// and runs it. All graphs must share the same State type.
//
// Graphs are stored as non-owning pointers — the caller owns the Graph objects.
//
template<embg::GraphState S>
class DegradedModeRunner {
public:
    DegradedModeRunner& add_level(CapabilityLevel level, embg::Graph<S>& graph) {
        levels_.emplace(level, &graph);
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
    std::unordered_map<CapabilityLevel, embg::Graph<S>*> levels_;
};

} // namespace embg::embedded
