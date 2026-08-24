// Example 06: LLM Inference Node in Automotive Diagnostic Agent
//
// Replaces the hard-coded simulated inference in example 04 with a real
// InferenceEngine plugged via embg::inference::make_node().
//
// By default: uses StubEngine (no model file needed, works immediately).
// For real inference: compile with -DEMBG_WITH_LLAMACPP and set LG_MODEL_PATH.
//
//   # Stub (default)
//   g++ -std=c++20 -I include examples/06_llm_diagnostic.cpp -o build/06_llm_diagnostic
//
//   # Real llama.cpp (after building llama.cpp and downloading a GGUF model):
//   g++ -std=c++20 -DEMBG_WITH_LLAMACPP
//       -I include -I /path/to/llama.cpp/include
//       examples/06_llm_diagnostic.cpp -o build/06_llm_diagnostic
//       -L /path/to/llama.cpp/build -lllama -lpthread
//
// Graph topology:
//
//   agent ──[router]──▶ tool_execute ──▶ run_inference ──[conf_router]
//     ▲                                                           │
//     │                                          conf < 0.85      │
//     └──────────────────────────────────────────────────────────┘
//                                         conf >= 0.85 ──▶ report_fault ──▶ END

#include <embg/graph.hpp>
#include <embg/embedded.hpp>
#include <embg/inference.hpp>
#include "automotive_tools.hpp"
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using State = automotive::DiagnosticState;

// ─── Tool sequence (subset — no actuator test needed for inference demo) ─────

static const std::vector<std::string> TOOL_SEQ = {
    "read_can_bus", "check_dtc", "read_live_pid"
};

// ─── Node implementations ─────────────────────────────────────────────────────

static void agent_node(State& s) {
    s.iteration++;
    const auto idx = static_cast<std::size_t>(s.iteration - 1);
    if (idx < TOOL_SEQ.size()) {
        s.next_action = "use_tool";
        s.tool_name   = TOOL_SEQ[idx];
        std::cout << "  [agent] step=" << s.iteration
                  << " -> calling: " << s.tool_name << "\n";
    } else {
        s.next_action = "finish";
        std::cout << "  [agent] tools exhausted -> forcing report\n";
    }
}

static void tool_execute_node(State& s) {
    std::string result = automotive::run_tool(s.tool_name, s.anomaly_code);
    s.observations.push_back("[" + s.tool_name + "] " + result);
    std::cout << "  [tool] " << result.substr(0, 80) << "\n";
}

// ─── Inference: prompt builder + response handler ────────────────────────────

static embg::inference::Request build_diagnostic_request(const State& s) {
    std::ostringstream oss;
    oss << "Vehicle VIN: " << s.vehicle_vin << "\n";
    oss << "Fault code: "  << s.anomaly_code << "\n";
    oss << "Evidence collected:\n";
    for (const auto& obs : s.observations)
        oss << "  - " << obs << "\n";
    oss << "Based on this evidence, what is the fault and severity?";

    return {
        .system_prompt = "You are an automotive ECU diagnostic assistant. "
                         "Respond concisely with: fault description, "
                         "severity (low/medium/high/critical), "
                         "and recommended action.",
        .user_prompt   = oss.str(),
        .max_tokens    = 128,
        .temperature   = 0.1f,
    };
}

static void apply_inference_response(State& s, const embg::inference::Response& r,
                                     const std::string& engine_name) {
    s.last_confidence   = r.confidence;
    s.fault_description = automotive::tool_check_dtc(s.anomaly_code);
    s.severity          = (s.anomaly_code == "P0300") ? "critical" : "medium";

    std::cout << "  [inference/" << (r.confidence >= 0.85 ? "pass" : "retry")
              << "] conf=" << r.confidence
              << " engine=" << engine_name
              << "\n";
    std::cout << "  [inference] response: " << r.text.substr(0, 100) << "\n";
}

static void report_fault_node(State& s) {
    std::ostringstream oss;
    oss << "\n+-- DIAGNOSTIC REPORT -------------------------------+\n";
    oss << "|  VIN        : " << s.vehicle_vin        << "\n";
    oss << "|  DTC        : " << s.anomaly_code       << "\n";
    oss << "|  Fault      : " << s.fault_description  << "\n";
    oss << "|  Severity   : " << s.severity           << "\n";
    oss << "|  Confidence : " << s.last_confidence    << "\n";
    oss << "|  Evidence   : " << s.observations.size() << " tool(s)\n";
    oss << "+----------------------------------------------------+\n";
    s.report = oss.str();
    std::cout << s.report;
}

// ─── Build the graph ──────────────────────────────────────────────────────────

static embg::Graph<State> make_graph(embg::inference::InferenceEngine& engine) {
    embg::Graph<State> g;

    g.add_node("agent",         agent_node);
    g.add_node("tool_execute",  tool_execute_node);

    // run_inference — uses the InferenceEngine via make_node().
    // engine is captured by reference (nothrow-movable, required by static mode)
    // and model_name() is read inside — fixes the hardcoded "stub" issue (4.5).
    g.add_node("run_inference",
        embg::inference::make_node<State>(
            engine,
            build_diagnostic_request,
            [&engine](State& s, const embg::inference::Response& r) {
                apply_inference_response(s, r, engine.model_name());
            }
        ));

    g.add_node("report_fault", report_fault_node);

    g.add_edge("tool_execute", "run_inference");
    g.add_edge("report_fault", embg::END);

    g.add_conditional_edge("agent", [](const State& s) {
        return (s.next_action == "use_tool") ? "tool_execute" : "report_fault";
    });

    g.add_conditional_edge("run_inference",
        embg::embedded::confidence_router<State>(
            /*threshold=*/0.85,
            /*above=*/"report_fault",
            /*below=*/"agent"
        ));

    g.set_entry("agent");

    return g;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== 06 LLM Inference Node -- Automotive Diagnostic ===\n";

    // Swap this for LlamaCppEngine when model is available.
    embg::inference::StubEngine engine;
    engine
        .add_response("P0420",
            "Catalyst system efficiency below threshold. "
            "Post-cat O2 sensor reading indicates degraded catalyst. "
            "Severity: medium. Action: inspect catalytic converter.",
            /*confidence=*/0.91)
        .add_response("P0300",
            "Random cylinder misfire detected. "
            "Multiple cylinders misfiring -- check ignition, fuel injectors. "
            "Severity: critical. Action: immediate inspection required.",
            /*confidence=*/0.94)
        .set_fallback("Insufficient data for confident diagnosis.", /*confidence=*/0.45);

    std::cout << "\n--- P0420 -- Catalyst fault ---\n";
    {
        auto graph = make_graph(engine);
        State s;
        s.anomaly_code = "P0420";
        s.vehicle_vin  = "WBA3A5G59DNP26082";
        graph.run(s, 20);
    }

    std::cout << "\n--- P0300 -- Critical misfire ---\n";
    {
        auto graph = make_graph(engine);
        State s;
        s.anomaly_code = "P0300";
        s.vehicle_vin  = "WBA3A5G59DNP26082";
        graph.run(s, 20);
    }

    return 0;
}
