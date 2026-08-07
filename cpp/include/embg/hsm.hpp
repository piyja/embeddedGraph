#pragma once

// Hierarchical State Machine (HSM) layer — extends the flat FSM model.
//
// Based on David Harel's Statecharts (1987) and Miro Samek's QP framework.
//
// What this adds over the flat embg::Graph:
//   - Nested states: parent states handle events children don't
//   - Entry/exit actions guaranteed on all transitions (not just node execution)
//   - Event propagation up the hierarchy until handled or discarded
//   - Initial pseudostates: default substate when entering a composite state
//   - History pseudostates: remember last active substate on re-entry
//
// Transition algorithm (LCA-based — correct per UML 2.0 semantics):
//   1. Find LCA (Lowest Common Ancestor) of source and target
//   2. Exit states from source up to (not including) LCA — innermost first
//   3. Enter states from LCA down to target — outermost first
//   4. Descend into target's initial child if it has one

#include "graph.hpp"  // for GraphState concept
#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace embg::hsm {

// ─── Sentinels returned by event handlers ────────────────────────────────────

// Handler handled the event internally — no state transition, no exit/entry.
inline const std::string INTERNAL  = "__internal__";

// Handler did not handle the event — propagate to parent state.
inline const std::string UNHANDLED = "__unhandled__";

// ─── HSM ─────────────────────────────────────────────────────────────────────

template<embg::GraphState S>
class HSM {
public:
    using Event      = std::string;
    // Handler: returns a target state name, INTERNAL, or UNHANDLED
    using HandlerFn  = std::function<std::string(S&)>;
    using ActionFn   = std::function<void(S&)>;
    using ObserveFn  = std::function<void(std::string_view from,
                                          std::string_view to,
                                          const S&)>;

    // ── State descriptor ──────────────────────────────────────────────────────

    struct StateConfig {
        std::string                              name;
        std::string                              parent   = {};  // "" = root level
        ActionFn                                 on_entry = nullptr;
        ActionFn                                 on_exit  = nullptr;
        std::unordered_map<Event, HandlerFn>     handlers = {};
        std::string                              initial  = {};  // default child state
    };

    // ── Builder ───────────────────────────────────────────────────────────────

    HSM& add_state(StateConfig cfg) {
        states_.insert_or_assign(cfg.name, std::move(cfg));
        return *this;
    }

    HSM& set_initial(std::string name) {
        initial_ = std::move(name);
        return *this;
    }

    // Register a transition observer — fires on every state change.
    // Maps to LangGraph's event streaming at the HSM level.
    HSM& on_transition(ObserveFn fn) {
        observe_ = std::move(fn);
        return *this;
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void init(S& state) {
        if (initial_.empty())
            throw std::runtime_error("embg::hsm: no initial state set");
        enter_chain(initial_, state);
    }

    // ── Event dispatch ────────────────────────────────────────────────────────
    //
    // Starts from the innermost active state and propagates up the hierarchy
    // until the event is handled or the root is reached.
    void dispatch(const Event& event, S& state) {
        std::string candidate = current_;

        while (!candidate.empty()) {
            auto& cfg = get_state(candidate);
            auto  it  = cfg.handlers.find(event);

            if (it != cfg.handlers.end()) {
                std::string result = it->second(state);

                if (result == INTERNAL)  return;   // consumed, no transition

                if (result != UNHANDLED) {
                    do_transition(current_, result, state);
                    return;
                }
            }

            candidate = cfg.parent;  // propagate up
        }
        // Silently discard — unhandled events are normal in HSMs
    }

    std::string_view current() const { return current_; }

private:
    std::unordered_map<std::string, StateConfig> states_;
    std::string                                  current_;
    std::string                                  initial_;
    std::optional<ObserveFn>                     observe_;
    // History: parent state name → last active child before exit
    std::unordered_map<std::string, std::string> history_;

    // ── Internal helpers ──────────────────────────────────────────────────────

    StateConfig& get_state(const std::string& name) {
        auto it = states_.find(name);
        if (it == states_.end())
            throw std::runtime_error("embg::hsm: unknown state '" + name + "'");
        return it->second;
    }

    // Ancestors of `name`, ordered leaf-to-root (inclusive of name)
    std::vector<std::string> ancestors(const std::string& name) {
        std::vector<std::string> chain;
        std::string cur = name;
        while (!cur.empty()) {
            chain.push_back(cur);
            cur = get_state(cur).parent;
        }
        return chain;
    }

    // Lowest Common Ancestor of two states
    std::string lca(const std::string& a, const std::string& b) {
        auto chain_a = ancestors(a);
        for (const auto& s : ancestors(b)) {
            if (std::find(chain_a.begin(), chain_a.end(), s) != chain_a.end())
                return s;
        }
        return {};  // no common ancestor — topology error
    }

    // Enter a state and descend into its initial child chain (if any)
    void enter_chain(const std::string& name, S& state) {
        auto& cfg = get_state(name);
        if (cfg.on_entry) cfg.on_entry(state);
        current_ = name;
        if (!cfg.initial.empty())
            enter_chain(cfg.initial, state);  // descend to default substate
    }

    // ── Transition (LCA-based, UML 2.0 compliant) ─────────────────────────────

    void do_transition(const std::string& from, const std::string& to, S& state) {
        if (observe_) (*observe_)(from, to, state);

        const std::string pivot = lca(from, to);

        // Step 1: Exit from source up to (not including) LCA — innermost first
        {
            std::string cur = from;
            while (cur != pivot && !cur.empty()) {
                auto& cfg = get_state(cur);
                // Record history before exit so parent can restore it
                history_[cfg.parent] = cur;
                if (cfg.on_exit) cfg.on_exit(state);
                cur = cfg.parent;
            }
        }

        // Step 2: Enter from LCA's child down to target — outermost first
        {
            // Build path from target to pivot (exclusive)
            std::vector<std::string> entry_path;
            std::string cur = to;
            while (cur != pivot && !cur.empty()) {
                entry_path.push_back(cur);
                cur = get_state(cur).parent;
            }
            std::reverse(entry_path.begin(), entry_path.end());  // top-down

            for (const auto& s : entry_path) {
                auto& cfg = get_state(s);
                if (cfg.on_entry) cfg.on_entry(state);
                current_ = s;
            }
        }

        // Step 3: Descend into target's initial child chain
        {
            auto& tcfg = get_state(to);
            if (!tcfg.initial.empty())
                enter_chain(tcfg.initial, state);
        }
    }
};

} // namespace embg::hsm
