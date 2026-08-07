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
//   "ai_restored"     → OPERATING
//   "critical_fault"  → SAFE_HALT         (handled at OPERATING level)
//   "reset"           → OPERATING/NORMAL  (handled at SAFE_HALT level)

#include <embg/graph.hpp>
#include <embg/hsm.hpp>
#include <iostream>
#include <string>
#include <vector>

// ─── Shared state ─────────────────────────────────────────────────────────────

struct ECUState {
    std::string              dtc_code    = {};
    std::string              vehicle_vin = "WBA3A5G59DNP26082";
    std::string              system_mode = {};  // mirrors HSM current state
    std::vector<std::string> event_log   = {};
    std::string              last_action = {};
};

// ─── Simple diagnostic graph (runs within OPERATING states) ───────────────────

static embg::Graph<ECUState> make_diagnostic_graph() {
    embg::Graph<ECUState> g;

    g
        .add_node("read_sensors", [](ECUState& s) {
            s.last_action = "read sensors — DTC=" + s.dtc_code
                          + " mode=" + s.system_mode;
            std::cout << "    [graph/read_sensors] " << s.last_action << "\n";
        })
        .add_node("analyze", [](ECUState& s) {
            s.last_action = "analyzed fault in " + s.system_mode + " mode";
            std::cout << "    [graph/analyze] " << s.last_action << "\n";
        })
        .add_edge("read_sensors", "analyze")
        .add_edge("analyze",      embg::END)
        .set_entry("read_sensors");

    return g;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== 05 HSM — Automotive ECU System States ===\n";

    auto diag_graph = make_diagnostic_graph();

    // ── Build HSM ──────────────────────────────────────────────────────────

    embg::hsm::HSM<ECUState> hsm;

    hsm
        // ── OPERATING (composite) ───────────────────────────────────────────
        // Parent state handles events common to all operational substates.
        .add_state({
            .name     = "OPERATING",
            .parent   = {},
            .on_entry = [](ECUState& s) {
                s.system_mode = "OPERATING";
                s.event_log.push_back("→ OPERATING");
                std::cout << "  [HSM] entered OPERATING\n";
            },
            .on_exit  = [](ECUState& s) {
                s.event_log.push_back("← OPERATING");
                std::cout << "  [HSM] exited OPERATING\n";
            },
            .handlers = {
                // Handled at OPERATING level: affects all substates
                {"critical_fault", [](ECUState& s) -> std::string {
                    std::cout << "  [HSM] critical fault — escalating to SAFE_HALT\n";
                    s.event_log.push_back("critical_fault escalated");
                    return "SAFE_HALT";
                }},
                {"ai_unavailable", [](ECUState&) -> std::string {
                    std::cout << "  [HSM] AI unavailable — switching to DEGRADED\n";
                    return "DEGRADED";
                }},
            },
            .initial  = "NORMAL",    // default substate when entering OPERATING
        })

        // ── OPERATING/NORMAL (leaf) ─────────────────────────────────────────
        .add_state({
            .name     = "NORMAL",
            .parent   = "OPERATING",
            .on_entry = [&diag_graph](ECUState& s) {
                s.system_mode = "OPERATING/NORMAL";
                s.event_log.push_back("→ NORMAL");
                std::cout << "  [HSM] entered NORMAL — running diagnostic graph\n";
                if (!s.dtc_code.empty()) diag_graph.run(s);
            },
            .on_exit  = [](ECUState& s) {
                s.event_log.push_back("← NORMAL");
                std::cout << "  [HSM] exited NORMAL\n";
            },
            .handlers = {
                {"fault_detected", [](ECUState& s) -> std::string {
                    std::cout << "  [HSM] fault detected — transitioning to ALERT\n";
                    s.event_log.push_back("fault_detected");
                    return "ALERT";
                }},
            },
        })

        // ── OPERATING/ALERT (leaf) ──────────────────────────────────────────
        .add_state({
            .name     = "ALERT",
            .parent   = "OPERATING",
            .on_entry = [&diag_graph](ECUState& s) {
                s.system_mode = "OPERATING/ALERT";
                s.event_log.push_back("→ ALERT");
                std::cout << "  [HSM] entered ALERT — running diagnostic graph with elevated priority\n";
                diag_graph.run(s);
            },
            .on_exit  = [](ECUState& s) {
                s.event_log.push_back("← ALERT");
                std::cout << "  [HSM] exited ALERT\n";
            },
            .handlers = {
                // Internal transition: fault cleared, stay in OPERATING but go to NORMAL
                {"fault_cleared", [](ECUState& s) -> std::string {
                    std::cout << "  [HSM] fault cleared — returning to NORMAL\n";
                    s.event_log.push_back("fault_cleared");
                    return "NORMAL";
                }},
            },
        })

        // ── DEGRADED (leaf) ─────────────────────────────────────────────────
        // AI inference unavailable — rule-based processing only.
        .add_state({
            .name     = "DEGRADED",
            .parent   = {},
            .on_entry = [](ECUState& s) {
                s.system_mode = "DEGRADED";
                s.event_log.push_back("→ DEGRADED");
                std::cout << "  [HSM] entered DEGRADED — AI unavailable, rule-based only\n";
            },
            .on_exit  = [](ECUState& s) {
                s.event_log.push_back("← DEGRADED");
                std::cout << "  [HSM] exited DEGRADED\n";
            },
            .handlers = {
                {"ai_restored", [](ECUState&) -> std::string {
                    std::cout << "  [HSM] AI restored — returning to OPERATING\n";
                    return "OPERATING";
                }},
                {"critical_fault", [](ECUState&) -> std::string {
                    return "SAFE_HALT";
                }},
            },
        })

        // ── SAFE_HALT (leaf) ────────────────────────────────────────────────
        // Safety-critical minimum: no processing, output alert only.
        // Maps to ASIL-D safe state — deterministic, verifiable, minimal.
        .add_state({
            .name     = "SAFE_HALT",
            .parent   = {},
            .on_entry = [](ECUState& s) {
                s.system_mode = "SAFE_HALT";
                s.event_log.push_back("→ SAFE_HALT");
                std::cout << "  [HSM] *** SAFE HALT — all processing stopped ***\n";
                std::cout << "  [HSM]     VIN=" << s.vehicle_vin
                          << " DTC=" << s.dtc_code << "\n";
            },
            .on_exit  = [](ECUState& s) {
                s.event_log.push_back("← SAFE_HALT");
            },
            .handlers = {
                {"reset", [](ECUState& s) -> std::string {
                    std::cout << "  [HSM] reset received — returning to OPERATING\n";
                    s.dtc_code = {};
                    return "OPERATING";
                }},
            },
        })

        .set_initial("OPERATING")

        // Observe all transitions
        .on_transition([](std::string_view from, std::string_view to, const ECUState&) {
            std::cout << "\n  [HSM transition] " << from << " → " << to << "\n";
        });

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
    dispatch("fault_cleared");    // ALERT  → NORMAL
    dispatch("ai_unavailable");   // OPERATING exits, DEGRADED enters
    dispatch("ai_restored");      // DEGRADED exits, OPERATING/NORMAL enters
    dispatch("critical_fault");   // OPERATING/NORMAL → SAFE_HALT (handled at OPERATING level)
    dispatch("reset");            // SAFE_HALT → OPERATING/NORMAL

    std::cout << "\n── event log ──\n";
    for (const auto& e : s.event_log)
        std::cout << "  " << e << "\n";

    return 0;
}
