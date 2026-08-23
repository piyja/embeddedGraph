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

#include "graph.hpp"  // for GraphState concept + config aliases
#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace embg::hsm {

// ─── Sentinels returned by event handlers ────────────────────────────────────

inline constexpr const char* INTERNAL  = "__internal__";
inline constexpr const char* UNHANDLED = "__unhandled__";

// ─── HSM ─────────────────────────────────────────────────────────────────────

template<embg::GraphState S, typename Cfg = embg::Config>
class HSM {
public:
    using StringT   = embg::detail::String<Cfg>;
    using Event     = StringT;
    using HandlerFn = std::conditional_t<Cfg::StaticAlloc,
        embg::Function<StringT(S&), Cfg::FnInlineBytes>,
        std::function<StringT(S&)>>;
    using ActionFn  = std::conditional_t<Cfg::StaticAlloc,
        embg::Function<void(S&), Cfg::FnInlineBytes>,
        std::function<void(S&)>>;
    using ObserveFn = std::conditional_t<Cfg::StaticAlloc,
        embg::Function<void(std::string_view, std::string_view, const S&), Cfg::FnInlineBytes>,
        std::function<void(std::string_view, std::string_view, const S&)>>;

    using HandlerMap = embg::detail::Map<Event, HandlerFn, Cfg, Cfg::MaxHandlers>;

    // ── State descriptor ──────────────────────────────────────────────────────

    struct StateConfig {
        StringT     name;
        StringT     parent   = {};
        ActionFn    on_entry = nullptr;
        ActionFn    on_exit  = nullptr;
        HandlerMap  handlers = {};
        StringT     initial  = {};
    };

    // ── Builder ───────────────────────────────────────────────────────────────

    HSM& add_state(StateConfig cfg) {
        // Keep the descriptor name intact: history is keyed by the composite
        // state's own name. Moving cfg.name and cfg in the same call makes the
        // stored name dependent on argument evaluation order.
        StringT key = cfg.name;
        states_.insert_or_assign(std::move(key), std::move(cfg));
        return *this;
    }

    HSM& set_initial(StringT name) {
        initial_ = std::move(name);
        return *this;
    }

    HSM& on_transition(ObserveFn fn) {
        observe_ = std::move(fn);
        return *this;
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void init(S& state) {
        if (initial_.empty())
            EMBG_ERROR(NoInitialState, "embg::hsm: no initial state set");
        validate();
        current_ = {};
        enter_chain(initial_, state);
    }

    // ── Event dispatch ────────────────────────────────────────────────────────
    void dispatch(const Event& event, S& state) {
        StringT candidate = current_;

        while (!candidate.empty()) {
            auto& cfg = get_state(candidate);
            auto  it  = cfg.handlers.find(event);

            if (it != cfg.handlers.end()) {
                StringT result = it->second(state);

                if (result == INTERNAL)  return;

                if (result != UNHANDLED) {
                    const StringT source = current_;
                    do_transition(source, result, state);
                    return;
                }
            }

            candidate = cfg.parent;
        }
    }

    std::string_view current() const { return current_; }

private:
    using StateMap = embg::detail::Map<StringT, StateConfig, Cfg, Cfg::MaxHsmStates>;
    using HistoryMap = embg::detail::Map<StringT, StringT, Cfg, Cfg::MaxHistory>;

    StateMap                    states_;
    StringT                     current_;
    StringT                     initial_;
    std::optional<ObserveFn>    observe_;
    HistoryMap                  history_;

    // ── Internal helpers ──────────────────────────────────────────────────────

    StateConfig& get_state(const StringT& name) {
        auto it = states_.find(name);
        if (it == states_.end())
            EMBG_ERROR(UnknownState, "embg::hsm: unknown state");
        return it->second;
    }

    void validate() {
        if (states_.find(initial_) == states_.end())
            EMBG_ERROR(InvalidHsm, "embg::hsm: initial state is not registered");

        for (const auto& [name, cfg] : states_) {
            if (cfg.name != name)
                EMBG_ERROR(InvalidHsm, "embg::hsm: state name does not match its key");
            if (!cfg.parent.empty() && states_.find(cfg.parent) == states_.end())
                EMBG_ERROR(InvalidHsm, "embg::hsm: parent state is not registered");
            if (!cfg.initial.empty()) {
                auto initial_it = states_.find(cfg.initial);
                if (initial_it == states_.end() || initial_it->second.parent != name)
                    EMBG_ERROR(InvalidHsm, "embg::hsm: initial state must be a direct child");
            }

            StaticVector<StringT, Cfg::MaxHsmDepth> seen;
            StringT current = name;
            while (!current.empty()) {
                for (const auto& visited : seen) {
                    if (visited == current)
                        EMBG_ERROR(InvalidHsm, "embg::hsm: cycle in parent hierarchy");
                }
                seen.push_back(current);
                current = get_state(current).parent;
            }
        }
    }

    // Fill a local buffer with ancestors of name, leaf-to-root.
    void ancestors(const StringT& name, StaticVector<StringT, Cfg::MaxHsmDepth>& out) {
        out.clear();
        StringT cur = name;
        while (!cur.empty()) {
            out.push_back(cur);
            cur = get_state(cur).parent;
        }
    }

    bool in_ancestors(const StringT& key,
                      const StaticVector<StringT, Cfg::MaxHsmDepth>& ancestors) const {
        for (const auto& s : ancestors)
            if (s == key) return true;
        return false;
    }

    // Lowest Common Ancestor — fills scratch_a_ with ancestors of a,
    // then walks ancestors of b to find the first match.
    StringT lca(const StringT& a, const StringT& b) {
        StaticVector<StringT, Cfg::MaxHsmDepth> first_ancestors;
        ancestors(a, first_ancestors);
        StringT cur = b;
        while (!cur.empty()) {
            if (in_ancestors(cur, first_ancestors)) return cur;
            cur = get_state(cur).parent;
        }
        return {};
    }

    // Enter a state and then its last active child (shallow history), falling
    // back to its configured initial child. Repeating this step handles any
    // number of nested composite states.
    void enter_chain(const StringT& name, S& state) {
        auto& cfg = get_state(name);
        current_ = name;
        if (cfg.on_entry) cfg.on_entry(state);
        enter_default_child(cfg, state);
    }

    void enter_default_child(const StateConfig& cfg, S& state) {
        StringT child = cfg.initial;
        auto history_it = history_.find(cfg.name);
        if (history_it != history_.end())
            child = history_it->second;

        if (!child.empty())
            enter_chain(child, state);
    }

    // ── Transition (LCA-based, UML 2.0 compliant) ─────────────────────────────

    void do_transition(const StringT& from, const StringT& to, S& state) {
        if (observe_) (*observe_)(from, to, state);

        // Returning the same state name is an external self-transition: exit
        // and re-enter the state. INTERNAL is the explicit no-action variant.
        const StringT pivot = (from == to) ? get_state(from).parent : lca(from, to);

        // Step 1: Exit from source up to (not including) LCA — innermost first
        {
            StringT cur = from;
            while (cur != pivot && !cur.empty()) {
                auto& cfg = get_state(cur);
                // Moving directly to a parent does not leave that composite,
                // so it must not immediately resume the child just exited.
                if (cfg.parent != to)
                    history_.insert_or_assign(cfg.parent, cur);
                if (cfg.on_exit) cfg.on_exit(state);
                cur = cfg.parent;
            }
        }

        // Step 2: Enter from LCA's child down to target — outermost first
        {
            StaticVector<StringT, Cfg::MaxHsmDepth> entry_path;
            StringT cur = to;
            while (cur != pivot && !cur.empty()) {
                entry_path.push_back(cur);
                cur = get_state(cur).parent;
            }
            entry_path.reverse();

            for (const auto& s : entry_path) {
                auto& cfg = get_state(s);
                current_ = s;
                if (cfg.on_entry) cfg.on_entry(state);
            }
        }

        // A transition to an ancestor has no entry path, but it still changes
        // the active leaf. An entry callback may have dispatched recursively;
        // preserve that nested transition instead of overwriting its current state.
        if (current_ == from) current_ = to;
        if (current_ != to) return;
        enter_default_child(get_state(to), state);
    }
};

} // namespace embg::hsm
