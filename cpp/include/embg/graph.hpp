#pragma once

#include "config.hpp"
#include "storage.hpp"
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
// const char* — no heap, works with both std::string and StaticString via
// implicit construction / comparison operators.

inline constexpr const char* END   = "__end__";
inline constexpr const char* START = "__start__";

// ─── Concepts ─────────────────────────────────────────────────────────────────

template<typename S>
concept GraphState = std::copyable<S> && std::movable<S>;

// ─── Config-conditional type aliases ──────────────────────────────────────────
//
// In default mode these resolve to std::* types (identical to the original API).
// In static mode they resolve to fixed-capacity types from storage.hpp.
// The application code is the same either way.

namespace detail {

// String type for names/keys
template<typename Cfg>
using String = std::conditional_t<Cfg::StaticAlloc,
    StaticString<Cfg::MaxStrLen>, std::string>;

// Callable types
template<GraphState S, typename Cfg>
using NodeFn = std::conditional_t<Cfg::StaticAlloc,
    Function<void(S&), Cfg::NodeFnInlineBytes>,
    std::function<void(S&)>>;

template<GraphState S, typename Cfg>
using RouterFn = std::conditional_t<Cfg::StaticAlloc,
    Function<String<Cfg>(const S&), Cfg::FnInlineBytes>,
    std::function<String<Cfg>(const S&)>>;

template<GraphState S, typename Cfg>
using StepFn = std::conditional_t<Cfg::StaticAlloc,
    Function<void(std::string_view, const S&), Cfg::FnInlineBytes>,
    std::function<void(std::string_view, const S&)>>;

// Edge destination: fixed name or runtime router
template<GraphState S, typename Cfg>
using EdgeDest = std::variant<String<Cfg>, RouterFn<S, Cfg>>;

// Map type for node/edge/interrupt storage
template<typename K, typename V, typename Cfg, std::size_t Cap>
using Map = std::conditional_t<Cfg::StaticAlloc,
    StaticMap<K, V, Cap>,
    std::unordered_map<K, V>>;

} // namespace detail

// ─── Public type aliases (default config) ─────────────────────────────────────
//
// These keep backward compatibility: code that writes embg::NodeFn<S> gets
// the default-config version. Config-aware code uses detail::NodeFn<S, Cfg>.

template<GraphState S> using NodeFn   = detail::NodeFn<S, Config>;
template<GraphState S> using RouterFn = detail::RouterFn<S, Config>;
template<GraphState S> using StepFn   = detail::StepFn<S, Config>;
template<GraphState S> using EdgeDest = detail::EdgeDest<S, Config>;

// ─── Graph ────────────────────────────────────────────────────────────────────

template<GraphState S, typename Cfg = Config>
class Graph {
public:
    using StringT  = detail::String<Cfg>;
    using NodeFnT  = detail::NodeFn<S, Cfg>;
    using RouterT  = detail::RouterFn<S, Cfg>;
    using StepFnT  = detail::StepFn<S, Cfg>;
    using EdgeDestT = detail::EdgeDest<S, Cfg>;

    // ── Builder API ───────────────────────────────────────────────────────────

    Graph& add_node(StringT name, NodeFnT fn) {
        nodes_.insert_or_assign(std::move(name), std::move(fn));
        return *this;
    }

    Graph& add_edge(StringT from, StringT to) {
        edges_.insert_or_assign(std::move(from), EdgeDestT{ std::move(to) });
        return *this;
    }

    Graph& add_conditional_edge(StringT from, RouterT router) {
        edges_.insert_or_assign(std::move(from), EdgeDestT{ std::move(router) });
        return *this;
    }

    Graph& set_entry(StringT name) {
        entry_ = std::move(name);
        return *this;
    }

    Graph& on_step(StepFnT fn) {
        step_fn_ = std::move(fn);
        return *this;
    }

    Graph& set_interrupt(StringT node_name, NodeFnT interrupt_fn) {
        interrupts_.insert_or_assign(std::move(node_name), std::move(interrupt_fn));
        return *this;
    }

    // ── Execution ─────────────────────────────────────────────────────────────

    void run(S& state, std::size_t max_steps = 100) {
        if (entry_.empty())
            throw std::runtime_error("embg::Graph: no entry node set — call set_entry()");

        StringT current = entry_;

        for (std::size_t step = 0; step < max_steps; ++step) {
            if (current == END) return;

            auto node_it = nodes_.find(current);
            if (node_it == nodes_.end())
                throw std::runtime_error("embg::Graph: unknown node '" + std::string(current) + "'");

            if (step_fn_) (*step_fn_)(current, state);

            auto intr_it = interrupts_.find(current);
            if (intr_it != interrupts_.end()) intr_it->second(state);

            node_it->second(state);

            auto edge_it = edges_.find(current);
            if (edge_it == edges_.end()) return;

            current = std::visit([&state](const auto& dest) -> StringT {
                using T = std::decay_t<decltype(dest)>;
                if constexpr (std::is_same_v<T, StringT>) {
                    return dest;
                } else {
                    return dest(state);
                }
            }, edge_it->second);
        }

        throw std::runtime_error(
            "embg::Graph: max_steps exceeded — check for unbounded loops");
    }

private:
    using NodeMap   = detail::Map<StringT, NodeFnT, Cfg, Cfg::MaxNodes>;
    using EdgeMap   = detail::Map<StringT, EdgeDestT, Cfg, Cfg::MaxEdges>;
    using IntrMap   = detail::Map<StringT, NodeFnT, Cfg, Cfg::MaxInterrupts>;

    NodeMap                    nodes_;
    EdgeMap                    edges_;
    IntrMap                    interrupts_;
    StringT                    entry_;
    std::optional<StepFnT>     step_fn_;
};

} // namespace embg
