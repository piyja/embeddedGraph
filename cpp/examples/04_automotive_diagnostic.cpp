// Example 04: Automotive Diagnostic Agent
//
// Demonstrates the full agent pattern with degraded mode:
//   - Agent loop: tool calling + inference + confidence-gated routing
//   - Degraded mode: AI unavailable falls back to static DTC lookup
//
// Uses shared automotive tools from automotive_tools.hpp (see examples 04/06/07).
//
// Graph topology (Full capability):
//
//   agent ──[router]──▶ tool_execute ──▶ run_inference ──[conf_router]
//     ▲                                                           │
//     │                                          conf < 0.85      │
//     └──────────────────────────────────────────────────────────┘
//                                         conf >= 0.85 ──▶ report_fault ──▶ END
//
// Graph topology (Degraded — no AI):
//
//   dtc_lookup ──▶ report_fault ──▶ END

#include <embg/graph.hpp>
#include <embg/embedded.hpp>
#include "automotive_tools.hpp"
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using State = automotive::DiagnosticState;

// ─── Tool sequence ───────────────────────────────────────────────────────────
// Agent works through this fixed sequence until confidence is high enough.
// A real agent would use LLM reasoning to pick tools adaptively.

static const std::vector<std::string> TOOL_SEQUENCE = {
    "read_can_bus", "check_dtc", "read_live_pid", "run_actuator_test"
};

// ─── Node implementations ─────────────────────────────────────────────────────

static void agent_node(State& s) {
    s.iteration++;
    std::cout << "  [agent] iteration=" << s.iteration
              << "  evidence=" << s.observations.size()
              << "  conf=" << s.last_confidence << "\n";

    const auto idx = static_cast<std::size_t>(s.iteration - 1);
    if (idx < TOOL_SEQUENCE.size()) {
        s.next_action = "use_tool";
        s.tool_name   = TOOL_SEQUENCE[idx];
        std::cout << "  [agent] -> selected tool: " << s.tool_name << "\n";
    } else {
        s.next_action = "finish";
        std::cout << "  [agent] -> all tools exhausted, forcing report\n";
    }
}

static void tool_execute_node(State& s) {
    std::string result = automotive::run_tool(s.tool_name, s.anomaly_code);
    s.observations.push_back("[" + s.tool_name + "] " + result);
    std::cout << "  [tool_execute] " << s.tool_name
              << " -> " << result.substr(0, 72) << "\n";
}

static void run_inference_node(State& s) {
    const auto n = s.observations.size();
    if      (n == 0) s.last_confidence = 0.00;
    else if (n == 1) s.last_confidence = 0.58;
    else if (n == 2) s.last_confidence = 0.74;
    else if (n == 3) s.last_confidence = 0.91;
    else             s.last_confidence = 0.97;

    s.fault_description = automotive::tool_check_dtc(s.anomaly_code);
    if (s.anomaly_code == "P0420")      s.severity = "medium";
    else if (s.anomaly_code == "P0300") s.severity = "critical";
    else                                s.severity = "high";

    std::cout << "  [run_inference] conf=" << s.last_confidence
              << "  severity=" << s.severity << "\n";
}

static void report_fault_full_node(State& s) {
    std::ostringstream oss;
    oss << "\n+-- DIAGNOSTIC REPORT -------------------------------+\n";
    oss << "|  VIN        : " << s.vehicle_vin        << "\n";
    oss << "|  DTC        : " << s.anomaly_code       << "\n";
    oss << "|  Fault      : " << s.fault_description  << "\n";
    oss << "|  Severity   : " << s.severity           << "\n";
    oss << "|  Confidence : " << s.last_confidence    << "\n";
    oss << "|  Evidence   : " << s.observations.size() << " tool result(s)\n";
    oss << "|  Mode       : AI-assisted (full capability)\n";
    oss << "+----------------------------------------------------+\n";
    s.report = oss.str();
    std::cout << s.report;
}

static void dtc_lookup_node(State& s) {
    s.fault_description = automotive::tool_check_dtc(s.anomaly_code);
    s.severity          = "unknown";
    s.last_confidence   = 1.0;
    std::cout << "  [dtc_lookup] " << s.fault_description << "\n";
}

static void report_fault_degraded_node(State& s) {
    std::ostringstream oss;
    oss << "\n+-- DIAGNOSTIC REPORT (DEGRADED MODE) ---------------+\n";
    oss << "|  VIN      : " << s.vehicle_vin        << "\n";
    oss << "|  DTC      : " << s.anomaly_code       << "\n";
    oss << "|  Fault    : " << s.fault_description  << "\n";
    oss << "|  Severity : unknown -- AI inference unavailable\n";
    oss << "|  Mode     : static DTC lookup -- refer to technician\n";
    oss << "+----------------------------------------------------+\n";
    s.report = oss.str();
    std::cout << s.report;
}

// ─── Full-capability graph ────────────────────────────────────────────────────

static embg::Graph<State> make_full_graph() {
    embg::Graph<State> g;

    g.add_node("agent",         agent_node);
    g.add_node("tool_execute",  tool_execute_node);
    g.add_node("run_inference", run_inference_node);
    g.add_node("report_fault",  report_fault_full_node);

    g.add_edge("tool_execute",  "run_inference");
    g.add_edge("report_fault",  embg::END);

    g.add_conditional_edge("agent", [](const State& s) -> std::string {
        return (s.next_action == "use_tool") ? "tool_execute" : "report_fault";
    });

    g.add_conditional_edge("run_inference",
        embg::embedded::confidence_router<State>(
            /*threshold=*/0.85,
            /*above=*/"report_fault",
            /*below=*/"agent"
        ));

    g.set_entry("agent");
    g.on_step([](std::string_view node, const State&) {
        std::cout << "\n-- [" << node << "]\n";
    });

    return g;
}

// ─── Degraded-mode graph (no AI — static DTC table only) ─────────────────────

static embg::Graph<State> make_degraded_graph() {
    embg::Graph<State> g;

    g.add_node("dtc_lookup",   dtc_lookup_node);
    g.add_node("report_fault", report_fault_degraded_node);

    g.add_edge("dtc_lookup",   "report_fault");
    g.add_edge("report_fault", embg::END);

    g.set_entry("dtc_lookup");

    return g;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== 04 Automotive Diagnostic Agent ===\n";

    auto full_graph     = make_full_graph();
    auto degraded_graph = make_degraded_graph();

    embg::embedded::DegradedModeRunner<State> runner;
    runner.add_level(embg::embedded::CapabilityLevel::Full,     full_graph)
          .add_level(embg::embedded::CapabilityLevel::Degraded, degraded_graph);

    std::cout << "\n--- Scenario 1: Full capability (AI-assisted) ---\n";
    {
        State s;
        s.anomaly_code = "P0420";
        s.vehicle_vin  = "WBA3A5G59DNP26082";
        runner.run(s, embg::embedded::CapabilityLevel::Full, /*max_steps=*/20);
    }

    std::cout << "\n--- Scenario 2: Degraded mode (static lookup) ---\n";
    {
        State s;
        s.anomaly_code = "P0420";
        s.vehicle_vin  = "WBA3A5G59DNP26082";
        runner.run(s, embg::embedded::CapabilityLevel::Degraded, /*max_steps=*/5);
    }

    std::cout << "\n--- Scenario 3: Critical fault (P0300 misfire) ---\n";
    {
        State s;
        s.anomaly_code = "P0300";
        s.vehicle_vin  = "WBA3A5G59DNP26082";
        runner.run(s, embg::embedded::CapabilityLevel::Full, /*max_steps=*/20);
    }

    return 0;
}
