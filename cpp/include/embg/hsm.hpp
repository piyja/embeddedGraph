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
        states_.insert_or_assign(std::move(cfg.name), std::move(cfg));
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
                    do_transition(current_, result, state);
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

    // Scratch buffers for transition computation — avoids heap alloc per transition.
    // Non-reentrant: a single HSM instance must not be called recursively.
    StaticVector<StringT, Cfg::MaxHsmDepth>  scratch_a_;
    StaticVector<StringT, Cfg::MaxHsmDepth>  scratch_b_;
    StaticVector<StringT, Cfg::MaxHsmDepth>  entry_path_;

    // ── Internal helpers ──────────────────────────────────────────────────────

    StateConfig& get_state(const StringT& name) {
        auto it = states_.find(name);
        if (it == states_.end())
            EMBG_ERROR(UnknownState, "embg::hsm: unknown state");
        return it->second;
    }

    // Fill scratch_a_ with ancestors of name, leaf-to-root
    void ancestors(const StringT& name) {
        scratch_a_.clear();
        StringT cur = name;
        while (!cur.empty()) {
            scratch_a_.push_back(cur);
            cur = get_state(cur).parent;
        }
    }

    // Check if key is in scratch_a_
    bool in_ancestors(const StringT& key) const {
        for (const auto& s : scratch_a_)
            if (s == key) return true;
        return false;
    }

    // Lowest Common Ancestor — fills scratch_a_ with ancestors of a,
    // then walks ancestors of b to find the first match.
    StringT lca(const StringT& a, const StringT& b) {
        ancestors(a);
        scratch_b_.clear();
        StringT cur = b;
        while (!cur.empty()) {
            if (in_ancestors(cur)) return cur;
            cur = get_state(cur).parent;
        }
        return {};
    }

    void enter_chain(const StringT& name, S& state) {
        auto& cfg = get_state(name);
        if (cfg.on_entry) cfg.on_entry(state);
        current_ = name;
        if (!cfg.initial.empty())
            enter_chain(cfg.initial, state);
    }

    // ── Transition (LCA-based, UML 2.0 compliant) ─────────────────────────────

    void do_transition(const StringT& from, const StringT& to, S& state) {
        if (observe_) (*observe_)(from, to, state);

        const StringT pivot = lca(from, to);

        // Step 1: Exit from source up to (not including) LCA — innermost first
        {
            StringT cur = from;
            while (cur != pivot && !cur.empty()) {
                auto& cfg = get_state(cur);
                history_.insert_or_assign(cfg.parent, cur);
                if (cfg.on_exit) cfg.on_exit(state);
                cur = cfg.parent;
            }
        }

        // Step 2: Enter from LCA's child down to target — outermost first
        {
            entry_path_.clear();
            StringT cur = to;
            while (cur != pivot && !cur.empty()) {
                entry_path_.push_back(cur);
                cur = get_state(cur).parent;
            }
            entry_path_.reverse();

            for (const auto& s : entry_path_) {
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
