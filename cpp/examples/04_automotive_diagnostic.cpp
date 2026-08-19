// Example 04: Automotive Diagnostic Agent
//
// An ECU diagnostic agent that:
//   1. Receives a fault trigger (DTC code + VIN)
//   2. Loops: agent decides which diagnostic tool to call next
//   3. After each tool call, inference assesses accumulated evidence
//   4. Confidence-gated routing: >= 0.85 → report fault; < 0.85 → gather more data
//   5. Degraded mode: AI unavailable → static DTC lookup table only
//
// This example combines:
//   - Example 02: agent loop / tool calling (cycles)
//   - Example 03: confidence-gated routing + degraded mode runner
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
//
// Each stage's responsibility:
//   agent          — picks the next diagnostic tool from TOOL_SEQUENCE
//   tool_execute   — dispatches to the tool registry, appends to evidence
//   run_inference  — simulated on-device model; confidence grows with evidence
//   report_fault   — formats and prints the diagnostic report
//   dtc_lookup     — degraded-mode static DTC table lookup

#include <embg/graph.hpp>
#include <embg/embedded.hpp>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ─── State ───────────────────────────────────────────────────────────────────

struct DiagnosticState {
    // Input
    std::string anomaly_code     = {};   // e.g. "P0420"
    std::string vehicle_vin      = {};

    // Agent control
    std::string next_action      = {};   // "use_tool" | "finish"
    std::string tool_name        = {};
    int         iteration        = 0;

    // Evidence accumulated across tool calls
    std::vector<std::string> observations = {};

    // Inference output — last_confidence required by ConfidenceState concept
    double      last_confidence  = 0.0;
    std::string fault_code       = {};
    std::string fault_description= {};
    std::string severity         = {};   // "low" | "medium" | "high" | "critical"

    // Final report
    std::string report = {};
};

static_assert(embg::embedded::ConfidenceState<DiagnosticState>,
    "DiagnosticState must satisfy ConfidenceState");

// ─── Simulated tool implementations ──────────────────────────────────────────
// In a real ECU these would call: OBD-II stack, CAN driver, actuator controller.

static std::string tool_read_can_bus(const std::string& code) {
    return "CAN[0x7E8] 41 05 6E ECT=110°C | "
           "CAN[0x7E8] 41 11 7A TPS=47%   | "
           "CAN[0x7E8] 41 24 FF B0 O2_S1=0.991V (rich excursion, code=" + code + ")";
}

static std::string tool_check_dtc(const std::string& code) {
    static const std::unordered_map<std::string, std::string> dtc_db = {
        {"P0420", "Catalyst System Efficiency Below Threshold (Bank 1)"},
        {"P0171", "System Too Lean (Bank 1)"},
        {"P0300", "Random/Multiple Cylinder Misfire Detected"},
        {"P0455", "EVAP System Large Leak Detected"},
    };
    auto it = dtc_db.find(code);
    return "DTC " + code + ": " +
           (it != dtc_db.end() ? it->second : "Unknown — refer to OEM table");
}

static std::string tool_read_live_pid(const std::string&) {
    return "PID[0x44] FuelTrim ST=-3.9% LT=-6.3% | "
           "PID[0x36] O2_S2=0.712V post-cat (low — efficiency suspect) | "
           "PID[0x0C] RPM=820 idle stable";
}

static std::string tool_run_actuator_test(const std::string&) {
    return "EGR valve response: OK | "
           "Injector balance: cyl1=+1.2% cyl2=+0.8% cyl3=-0.4% cyl4=-1.0% | "
           "Post-cat O2 delta=0.041V (threshold 0.100V — catalyst degraded)";
}

using ToolFn = std::string(*)(const std::string&);
static const std::unordered_map<std::string, ToolFn> TOOL_REGISTRY = {
    {"read_can_bus",      tool_read_can_bus},
    {"check_dtc",         tool_check_dtc},
    {"read_live_pid",     tool_read_live_pid},
    {"run_actuator_test", tool_run_actuator_test},
};

// Tool call order — agent works through this sequence until confidence is high enough.
// A real agent would use LLM reasoning to pick tools adaptively.
static const std::vector<std::string> TOOL_SEQUENCE = {
    "read_can_bus", "check_dtc", "read_live_pid", "run_actuator_test"
};

// ─── Node implementations ─────────────────────────────────────────────────────
// Free functions so the graph builders read as pure topology.

// Picks the next diagnostic tool. Once all tools are tried it signals
// "finish" to force a report regardless of confidence (loop safety).
static void agent_node(DiagnosticState& s) {
    s.iteration++;
    std::cout << "  [agent] iteration=" << s.iteration
              << "  evidence=" << s.observations.size()
              << "  conf=" << s.last_confidence << "\n";

    const auto idx = static_cast<std::size_t>(s.iteration - 1);
    if (idx < TOOL_SEQUENCE.size()) {
        s.next_action = "use_tool";
        s.tool_name   = TOOL_SEQUENCE[idx];
        std::cout << "  [agent] → selected tool: " << s.tool_name << "\n";
    } else {
        s.next_action = "finish";
        std::cout << "  [agent] → all tools exhausted, forcing report\n";
    }
}

// Dispatches to the tool registry and appends result to observations.
static void tool_execute_node(DiagnosticState& s) {
    auto it = TOOL_REGISTRY.find(s.tool_name);
    std::string result = (it != TOOL_REGISTRY.end())
        ? it->second(s.anomaly_code)
        : "[tool not found: " + s.tool_name + "]";

    s.observations.push_back("[" + s.tool_name + "] " + result);

    // Print truncated to keep output readable
    std::cout << "  [tool_execute] " << s.tool_name
              << " → " << result.substr(0, 72) << "…\n";
}

// Simulates an on-device diagnostic model (e.g., ONNX Runtime on Cortex-A).
// Confidence grows as more tool evidence is accumulated — this is the key
// embedded pattern: the model becomes more certain with richer context.
static void run_inference_node(DiagnosticState& s) {
    const auto n = s.observations.size();

    // Confidence profile — simulates model uncertainty reducing with evidence
    if      (n == 0) s.last_confidence = 0.00;  // no data
    else if (n == 1) s.last_confidence = 0.58;  // CAN bus alone — ambiguous
    else if (n == 2) s.last_confidence = 0.74;  // + DTC — clearer but uncertain
    else if (n == 3) s.last_confidence = 0.91;  // + live PIDs — high confidence
    else             s.last_confidence = 0.97;  // + actuator test — near certain

    s.fault_code        = s.anomaly_code;
    s.fault_description = tool_check_dtc(s.anomaly_code);

    // Severity depends on fault type — would be model output in production
    if (s.anomaly_code == "P0420")      s.severity = "medium";
    else if (s.anomaly_code == "P0300") s.severity = "critical";
    else                                s.severity = "high";

    std::cout << "  [run_inference] conf=" << s.last_confidence
              << "  severity=" << s.severity << "\n";
}

static void report_fault_full_node(DiagnosticState& s) {
    std::ostringstream oss;
    oss << "\n┌── DIAGNOSTIC REPORT ─────────────────────────────────────┐\n";
    oss << "│  VIN        : " << s.vehicle_vin        << "\n";
    oss << "│  DTC        : " << s.fault_code         << "\n";
    oss << "│  Fault      : " << s.fault_description  << "\n";
    oss << "│  Severity   : " << s.severity           << "\n";
    oss << "│  Confidence : " << s.last_confidence    << "\n";
    oss << "│  Evidence   : " << s.observations.size() << " tool result(s)\n";
    oss << "│  Mode       : AI-assisted (full capability)\n";
    oss << "└───────────────────────────────────────────────────────────┘\n";
    s.report = oss.str();
    std::cout << s.report;
}

// Degraded-mode DTC lookup: static table only, no model.
static void dtc_lookup_node(DiagnosticState& s) {
    s.fault_code        = s.anomaly_code;
    s.fault_description = tool_check_dtc(s.anomaly_code);
    s.severity          = "unknown";  // no model to assess severity
    s.last_confidence   = 1.0;        // rule-based lookup is deterministic
    std::cout << "  [dtc_lookup] " << s.fault_description << "\n";
}

static void report_fault_degraded_node(DiagnosticState& s) {
    std::ostringstream oss;
    oss << "\n┌── DIAGNOSTIC REPORT (DEGRADED MODE) ─────────────────────┐\n";
    oss << "│  VIN      : " << s.vehicle_vin        << "\n";
    oss << "│  DTC      : " << s.fault_code         << "\n";
    oss << "│  Fault    : " << s.fault_description  << "\n";
    oss << "│  Severity : unknown — AI inference unavailable\n";
    oss << "│  Mode     : static DTC lookup — refer to technician\n";
    oss << "└───────────────────────────────────────────────────────────┘\n";
    s.report = oss.str();
    std::cout << s.report;
}

// ─── Full-capability graph ────────────────────────────────────────────────────
//
// Topology:
//   agent ──[router]──▶ tool_execute ──▶ run_inference ──[conf_router]
//     ▲                                                           │
//     │                                          conf < 0.85      │
//     └──────────────────────────────────────────────────────────┘
//                                         conf >= 0.85 ──▶ report_fault ──▶ END

static embg::Graph<DiagnosticState> make_full_graph() {
    embg::Graph<DiagnosticState> g;

    // ── Nodes ─────────────────────────────────────────────────────────────────
    g.add_node("agent",         agent_node);
    g.add_node("tool_execute",  tool_execute_node);
    g.add_node("run_inference", run_inference_node);
    g.add_node("report_fault",  report_fault_full_node);

    // ── Edges (declared together so the topology is visible at a glance) ────
    //
    // After every tool call, re-run inference on the updated evidence.
    g.add_edge("tool_execute",  "run_inference");
    g.add_edge("report_fault",  embg::END);

    // ── Routers ──────────────────────────────────────────────────────────────
    // Agent routes to tool or to report (when all tools exhausted).
    g.add_conditional_edge("agent", [](const DiagnosticState& s) -> std::string {
        return (s.next_action == "use_tool") ? "tool_execute" : "report_fault";
    });

    // Confidence gate: enough evidence → report; not yet → back to agent.
    g.add_conditional_edge("run_inference",
        embg::embedded::confidence_router<DiagnosticState>(
            /*threshold=*/0.85,
            /*above=*/"report_fault",
            /*below=*/"agent"
        ));

    // ── Entry + streaming ─────────────────────────────────────────────────────
    g.set_entry("agent");
    g.on_step([](std::string_view node, const DiagnosticState&) {
        std::cout << "\n── [" << node << "]\n";
    });

    return g;
}

// ─── Degraded-mode graph (no AI — static DTC table only) ─────────────────────
//
// Topology:
//   dtc_lookup → report_fault → END

static embg::Graph<DiagnosticState> make_degraded_graph() {
    embg::Graph<DiagnosticState> g;

    // ── Nodes ─────────────────────────────────────────────────────────────────
    g.add_node("dtc_lookup",   dtc_lookup_node);
    g.add_node("report_fault", report_fault_degraded_node);

    // ── Edges ────────────────────────────────────────────────────────────────
    g.add_edge("dtc_lookup",   "report_fault");
    g.add_edge("report_fault", embg::END);

    // ── Entry ───────────────────────────────────────────────────────────────
    g.set_entry("dtc_lookup");

    return g;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== 04 Automotive Diagnostic Agent ===\n";

    auto full_graph     = make_full_graph();
    auto degraded_graph = make_degraded_graph();

    embg::embedded::DegradedModeRunner<DiagnosticState> runner;
    runner.add_level(embg::embedded::CapabilityLevel::Full,     full_graph)
          .add_level(embg::embedded::CapabilityLevel::Degraded, degraded_graph);

    // ── Scenario 1: Full capability ─────────────────────────────────────────
    // Agent loops, calling tools, until confidence crosses the 0.85 threshold.
    // Expected: 3 tool calls (CAN bus + DTC + live PIDs) before conf=0.91 fires.

    std::cout << "\n━━━ Scenario 1: Full capability (AI-assisted) ━━━\n";
    {
        DiagnosticState s;
        s.anomaly_code = "P0420";
        s.vehicle_vin  = "WBA3A5G59DNP26082";
        runner.run(s, embg::embedded::CapabilityLevel::Full, /*max_steps=*/20);
    }

    // ── Scenario 2: Degraded mode ───────────────────────────────────────────
    // Inference engine unavailable (thermal shutdown, update in progress, etc.)
    // Falls back to static DTC table — no tool loop, no confidence scoring.

    std::cout << "\n━━━ Scenario 2: Degraded mode (static lookup) ━━━\n";
    {
        DiagnosticState s;
        s.anomaly_code = "P0420";
        s.vehicle_vin  = "WBA3A5G59DNP26082";
        runner.run(s, embg::embedded::CapabilityLevel::Degraded, /*max_steps=*/5);
    }

    // ── Scenario 3: Different fault code ────────────────────────────────────
    // Misfire — classified as "critical", higher severity than catalyst fault.

    std::cout << "\n━━━ Scenario 3: Critical fault (P0300 misfire) ━━━\n";
    {
        DiagnosticState s;
        s.anomaly_code = "P0300";
        s.vehicle_vin  = "WBA3A5G59DNP26082";
        runner.run(s, embg::embedded::CapabilityLevel::Full, /*max_steps=*/20);
    }

    return 0;
}
