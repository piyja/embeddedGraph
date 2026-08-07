#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace embg {

// ─── Sentinel node names ──────────────────────────────────────────────────────

inline const std::string END   = "__end__";
inline const std::string START = "__start__";

// ─── Concepts ─────────────────────────────────────────────────────────────────

// Any copyable, movable type can be graph state.
template<typename S>
concept GraphState = std::copyable<S> && std::movable<S>;

// ─── Core type aliases ────────────────────────────────────────────────────────

// Node: executes and mutates state. Routing is always separate from execution.
// This separation is deliberate — it mirrors the LangGraph model and makes
// the transition table independently readable from node logic.
template<GraphState S>
using NodeFn = std::function<void(S&)>;

// Router: reads state after a node executes and returns the next node name.
template<GraphState S>
using RouterFn = std::function<std::string(const S&)>;

// An edge destination is either a fixed name or a runtime routing function.
template<GraphState S>
using EdgeDest = std::variant<std::string, RouterFn<S>>;

// Optional observation hook: called before each node executes.
// Useful for logging, tracing, and streaming — maps to LangGraph's event stream.
template<GraphState S>
using StepFn = std::function<void(std::string_view node_name, const S& state)>;

// ─── Graph ────────────────────────────────────────────────────────────────────

template<GraphState S>
class Graph {
public:
    // ── Builder API ───────────────────────────────────────────────────────────

    Graph& add_node(std::string name, NodeFn<S> fn) {
        nodes_.insert_or_assign(name, std::move(fn));
        return *this;
    }

    // Unconditional edge: from always goes to to.
    Graph& add_edge(std::string from, std::string to) {
        edges_.insert_or_assign(from, EdgeDest<S>{ std::move(to) });
        return *this;
    }

    // Conditional edge: router reads state and returns the next node name.
    Graph& add_conditional_edge(std::string from, RouterFn<S> router) {
        edges_.insert_or_assign(from, EdgeDest<S>{ std::move(router) });
        return *this;
    }

    Graph& set_entry(std::string name) {
        entry_ = std::move(name);
        return *this;
    }

    // Register an observation callback (streaming equivalent).
    Graph& on_step(StepFn<S> fn) {
        step_fn_ = std::move(fn);
        return *this;
    }

    // Register a human-in-the-loop interrupt on a specific node.
    // The interrupt fires before the node executes; it may modify state
    // (inject input, approve/reject, etc.) and then returns.
    // Maps to LangGraph's interrupt() primitive.
    Graph& set_interrupt(std::string node_name, NodeFn<S> interrupt_fn) {
        interrupts_.insert_or_assign(std::move(node_name), std::move(interrupt_fn));
        return *this;
    }

    // ── Execution ─────────────────────────────────────────────────────────────

    void run(S& state, std::size_t max_steps = 100) {
        if (entry_.empty())
            throw std::runtime_error("embg::Graph: no entry node set — call set_entry()");

        std::string current = entry_;

        for (std::size_t step = 0; step < max_steps; ++step) {
            if (current == END) return;

            auto node_it = nodes_.find(current);
            if (node_it == nodes_.end())
                throw std::runtime_error("embg::Graph: unknown node '" + current + "'");

            // Observation hook — fires before execution (event streaming)
            if (step_fn_) (*step_fn_)(current, state);

            // Human-in-the-loop interrupt — pause before this node if registered
            auto intr_it = interrupts_.find(current);
            if (intr_it != interrupts_.end()) intr_it->second(state);

            // Execute the node — allowed to freely mutate state
            node_it->second(state);

            // Resolve the next node
            auto edge_it = edges_.find(current);
            if (edge_it == edges_.end()) return;  // no outgoing edge → implicit END

            current = std::visit([&state](const auto& dest) -> std::string {
                using T = std::decay_t<decltype(dest)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    return dest;
                } else {
                    return dest(state);  // router function
                }
            }, edge_it->second);
        }

        throw std::runtime_error(
            "embg::Graph: max_steps exceeded — check for unbounded loops");
    }

private:
    std::unordered_map<std::string, NodeFn<S>>   nodes_;
    std::unordered_map<std::string, EdgeDest<S>> edges_;
    std::unordered_map<std::string, NodeFn<S>>   interrupts_;
    std::string                                  entry_;
    std::optional<StepFn<S>>                     step_fn_;
};

} // namespace embg
