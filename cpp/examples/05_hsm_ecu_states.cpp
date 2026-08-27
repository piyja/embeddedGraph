// Example 05: HSM Managing Automotive ECU System States
//
// Demonstrates the HSM layer on top of embg::Graph.
//
// Responsibilities are split:
//   HSM   — manages coarse system states (OPERATING / DEGRADED / SAFE_HALT)
//   Graph — runs the diagnostic agent *within* the OPERATING state
//
// This is the two-layer architecture for embedded AI systems:
//   HSM  = the deterministic behavioral contract
//   Graph = the agentic tool-calling loop that runs inside a state
//
// HSM topology:
//
//   ┌─── OPERATING (default) ──────────────────────────┐
//   │  ┌── NORMAL (default) ──┐  ┌── ALERT ──────────┐ │
//   │  │  (run diagnostic     │  │  (run diagnostic + │ │
//   │  │   graph on entry)    │  │   elevate severity)│ │
//   │  └──────────────────────┘  └───────────────────┘ │
//   └──────────────────────────────────────────────────┘
//   ┌─── DEGRADED ──────────────────────────────────────┐
//   │   AI unavailable — rule-based graph only           │
//   └──────────────────────────────────────────────────┘
//   ┌─── SAFE_HALT ─────────────────────────────────────┐
//   │   Stop all processing — output alert only          │
//   └──────────────────────────────────────────────────┘
//
// Events dispatched to HSM:
//   "fault_detected"  → OPERATING/ALERT
//   "fault_cleared"   → OPERATING/NORMAL  (internal: no parent exit/entry)
//   "ai_unavailable"  → DEGRADED
//   "diagnostic_retry"→ ALERT (external self-transition: exit + re-enter)
//   "ai_restored"     → OPERATING/last active substate (shallow history)
//   "critical_fault"  → SAFE_HALT         (handled at OPERATING level)
//   "reset"           → OPERATING/NORMAL  (handled at SAFE_HALT level)

#include <embg/graph.hpp>
#include <embg/hsm.hpp>
#include "example_types.hpp"
#include <iostream>

using Str     = embg::examples::Str;
using LongStr = embg::examples::LongStr<>;
using StrVec  = embg::examples::StrVec<>;

// ─── Shared state ─────────────────────────────────────────────────────────────

struct ECUState {
    Str     dtc_code    = {};
    Str     vehicle_vin = "WBA3A5G59DNP26082";
    Str     system_mode = {};
    std::conditional_t<embg::Config::StaticAlloc,
        embg::StaticVector<LongStr, 32>,
        std::vector<std::string>> event_log = {};
    LongStr last_action = {};
};

// ─── Diagnostic graph node implementations ───────────────────────────────────
// Free functions so the graph builder reads as pure topology.

static void read_sensors_node(ECUState& s) {
    s.last_action = "read sensors — DTC=";
    s.last_action += s.dtc_code;
    s.last_action += " mode=";
    s.last_action += s.system_mode;
    std::cout << "    [graph/read_sensors] " << s.last_action << "\n";
}

static void analyze_node(ECUState& s) {
    s.last_action = "analyzed fault in ";
    s.last_action += s.system_mode;
    s.last_action += " mode";
    std::cout << "    [graph/analyze] " << s.last_action << "\n";
}

// ─── Build the diagnostic graph ──────────────────────────────────────────────
//
// Topology:
//   read_sensors → analyze → END
// Runs within OPERATING states (NORMAL and ALERT on_entry).

static embg::Graph<ECUState> make_diagnostic_graph() {
    embg::Graph<ECUState> g;

    // ── Nodes ─────────────────────────────────────────────────────────────────
    g.add_node("read_sensors", read_sensors_node);
    g.add_node("analyze",      analyze_node);

    // ── Edges ────────────────────────────────────────────────────────────────
    g.add_edge("read_sensors", "analyze");
    g.add_edge("analyze",      embg::END);

    // ── Entry ───────────────────────────────────────────────────────────────
    g.set_entry("read_sensors");

    return g;
}

// ─── HSM state actions ──────────────────────────────────────────────────────
// Entry/exit actions and event handlers as named functions so each
// .add_state({...}) declaration reads as a clean structural overview of the
// state machine — the "what states exist and how they transition" story is
// separated from the "what each action does" story.

// ── OPERATING (composite) ───────────────────────────────────────────────────
// Parent state handles events common to all operational substates.

static void operating_on_entry(ECUState& s) {
    s.system_mode = "OPERATING";
    s.event_log.push_back("→ OPERATING");
    std::cout << "  [HSM] entered OPERATING\n";
}

static void operating_on_exit(ECUState& s) {
    s.event_log.push_back("← OPERATING");
    std::cout << "  [HSM] exited OPERATING\n";
}

static Str handle_critical_fault(ECUState& s) {
    std::cout << "  [HSM] critical fault — escalating to SAFE_HALT\n";
    s.event_log.push_back("critical_fault escalated");
    return "SAFE_HALT";
}

static Str handle_ai_unavailable(ECUState&) {
    std::cout << "  [HSM] AI unavailable — switching to DEGRADED\n";
    return "DEGRADED";
}

// ── OPERATING/NORMAL (leaf) ──────────────────────────────────────────────────

static void normal_on_entry(ECUState& s, embg::Graph<ECUState>& diag_graph) {
    s.system_mode = "OPERATING/NORMAL";
    s.event_log.push_back("→ NORMAL");
    std::cout << "  [HSM] entered NORMAL — running diagnostic graph\n";
    if (!s.dtc_code.empty()) diag_graph.run(s);
}

static void normal_on_exit(ECUState& s) {
    s.event_log.push_back("← NORMAL");
    std::cout << "  [HSM] exited NORMAL\n";
}

static Str handle_fault_detected(ECUState& s) {
    std::cout << "  [HSM] fault detected — transitioning to ALERT\n";
    s.event_log.push_back("fault_detected");
    return "ALERT";
}

// ── OPERATING/ALERT (leaf) ───────────────────────────────────────────────────

static void alert_on_entry(ECUState& s, embg::Graph<ECUState>& diag_graph) {
    s.system_mode = "OPERATING/ALERT";
    s.event_log.push_back("→ ALERT");
    std::cout << "  [HSM] entered ALERT — running diagnostic graph with elevated priority\n";
    diag_graph.run(s);
}

static void alert_on_exit(ECUState& s) {
    s.event_log.push_back("← ALERT");
    std::cout << "  [HSM] exited ALERT\n";
}

static Str handle_fault_cleared(ECUState& s) {
    std::cout << "  [HSM] fault cleared — returning to NORMAL\n";
    s.event_log.push_back("fault_cleared");
    return "NORMAL";
}

static Str handle_diagnostic_retry(ECUState& s) {
    std::cout << "  [HSM] retrying diagnostic — restarting ALERT actions\n";
    s.event_log.push_back("diagnostic_retry");
    return "ALERT";
}

// ── DEGRADED (leaf) ──────────────────────────────────────────────────────────
// AI inference unavailable — rule-based processing only.

static void degraded_on_entry(ECUState& s) {
    s.system_mode = "DEGRADED";
    s.event_log.push_back("→ DEGRADED");
    std::cout << "  [HSM] entered DEGRADED — AI unavailable, rule-based only\n";
}

static void degraded_on_exit(ECUState& s) {
    s.event_log.push_back("← DEGRADED");
    std::cout << "  [HSM] exited DEGRADED\n";
}

static Str handle_ai_restored(ECUState&) {
    std::cout << "  [HSM] AI restored — returning to OPERATING\n";
    return "OPERATING";
}

static Str handle_degraded_critical_fault(ECUState&) {
    return "SAFE_HALT";
}

// ── SAFE_HALT (leaf) ─────────────────────────────────────────────────────────
// Safety-critical minimum: no processing, output alert only.
// Maps to ASIL-D safe state — deterministic, verifiable, minimal.

static void safe_halt_on_entry(ECUState& s) {
    s.system_mode = "SAFE_HALT";
    s.event_log.push_back("→ SAFE_HALT");
    std::cout << "  [HSM] *** SAFE HALT — all processing stopped ***\n";
    std::cout << "  [HSM]     VIN=" << s.vehicle_vin
              << " DTC=" << s.dtc_code << "\n";
}

static void safe_halt_on_exit(ECUState& s) {
    s.event_log.push_back("← SAFE_HALT");
}

static Str handle_reset(ECUState& s) {
    std::cout << "  [HSM] reset received — returning to OPERATING\n";
    s.dtc_code = {};
    return "OPERATING";
}

// ─── HSM transition observer ──────────────────────────────────────────────────

static void on_transition_handler(std::string_view from, std::string_view to, const ECUState&) {
    std::cout << "\n  [HSM transition] " << from << " → " << to << "\n";
}

// ─── Build the HSM ────────────────────────────────────────────────────────────
//
// Each .add_state({...}) is now a clean declaration: state name, parent,
// entry/exit actions, event handlers, and initial substate — all visible at
// a glance. The implementation details live in the named functions above.

static embg::hsm::HSM<ECUState> make_ecu_hsm(embg::Graph<ECUState>& diag_graph) {
    embg::hsm::HSM<ECUState> hsm;

    hsm
        // ── OPERATING (composite) ───────────────────────────────────────────
        // Parent state handles events common to all operational substates.
        .add_state({
            .name     = "OPERATING",
            .parent   = {},
            .on_entry = operating_on_entry,
            .on_exit  = operating_on_exit,
            .handlers = {
                {"critical_fault", handle_critical_fault},
                {"ai_unavailable", handle_ai_unavailable},
            },
            .initial  = "NORMAL",
        })

        // ── OPERATING/NORMAL (leaf) ─────────────────────────────────────────
        .add_state({
            .name     = "NORMAL",
            .parent   = "OPERATING",
            .on_entry = [&diag_graph](ECUState& s) { normal_on_entry(s, diag_graph); },
            .on_exit  = normal_on_exit,
            .handlers = {
                {"fault_detected", handle_fault_detected},
            },
        })

        // ── OPERATING/ALERT (leaf) ──────────────────────────────────────────
        .add_state({
            .name     = "ALERT",
            .parent   = "OPERATING",
            .on_entry = [&diag_graph](ECUState& s) { alert_on_entry(s, diag_graph); },
            .on_exit  = alert_on_exit,
            .handlers = {
                // Return to NORMAL without leaving the OPERATING composite.
                {"fault_cleared", handle_fault_cleared},
                // Returning the current state requests an external self-transition.
                {"diagnostic_retry", handle_diagnostic_retry},
            },
        })

        // ── DEGRADED (leaf) ─────────────────────────────────────────────────
        // AI inference unavailable — rule-based processing only.
        .add_state({
            .name     = "DEGRADED",
            .parent   = {},
            .on_entry = degraded_on_entry,
            .on_exit  = degraded_on_exit,
            .handlers = {
                {"ai_restored",   handle_ai_restored},
                {"critical_fault", handle_degraded_critical_fault},
            },
        })

        // ── SAFE_HALT (leaf) ────────────────────────────────────────────────
        // Safety-critical minimum: no processing, output alert only.
        // Maps to ASIL-D safe state — deterministic, verifiable, minimal.
        .add_state({
            .name     = "SAFE_HALT",
            .parent   = {},
            .on_entry = safe_halt_on_entry,
            .on_exit  = safe_halt_on_exit,
            .handlers = {
                {"reset", handle_reset},
            },
        })

        .set_initial("OPERATING")

        // Observe all transitions
        .on_transition(on_transition_handler);

    return hsm;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== 05 HSM — Automotive ECU System States ===\n";

    auto diag_graph = make_diagnostic_graph();
    auto hsm        = make_ecu_hsm(diag_graph);

    // ── Scenario: event sequence ────────────────────────────────────────────

    ECUState s;
    s.dtc_code = "P0420";

    auto dispatch = [&](const std::string& event) {
        std::cout << "\n━━━ event: " << event << " ━━━\n";
        hsm.dispatch(event, s);
        std::cout << "  current state: " << hsm.current() << "\n";
    };

    std::cout << "\n── init ──\n";
    hsm.init(s);
    std::cout << "  current state: " << hsm.current() << "\n";

    dispatch("fault_detected");   // NORMAL → ALERT (within OPERATING, no parent exit/entry)
    dispatch("diagnostic_retry"); // ALERT → ALERT (external: exit + re-enter ALERT actions)
    dispatch("ai_unavailable");   // OPERATING exits, DEGRADED enters
    dispatch("ai_restored");      // DEGRADED exits, OPERATING/ALERT resumes from history
    dispatch("fault_cleared");    // ALERT → NORMAL
    dispatch("critical_fault");   // OPERATING/NORMAL → SAFE_HALT (handled at OPERATING level)
    dispatch("reset");            // SAFE_HALT → OPERATING/NORMAL

    std::cout << "\n── event log ──\n";
    for (const auto& e : s.event_log)
        std::cout << "  " << e << "\n";

    return 0;
}
