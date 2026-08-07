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
//   # Real llama.cpp (after building llama.cpp and downloading a GGUF model,
//   # run as one line):
//   g++ -std=c++20 -DEMBG_WITH_LLAMACPP
//       -I include -I /path/to/llama.cpp/include
//       examples/06_llm_diagnostic.cpp -o build/06_llm_diagnostic
//       -L /path/to/llama.cpp/build -lllama -lpthread
//
// Recommended small models for embedded/edge targets:
//   Phi-3 Mini 3.8B Q4  — good reasoning, 2GB VRAM or CPU
//   Qwen2.5 0.5B Q8     — very fast, ~500MB, suited for MCU with NPU
//   Gemma 2 2B Q4       — strong instruction following, 1.5GB
//   TinyLlama 1.1B Q4   — smallest, Cortex-A series target

#include <embg/graph.hpp>
#include <embg/embedded.hpp>
#include <embg/inference.hpp>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ─── State ───────────────────────────────────────────────────────────────────

struct DiagnosticState {
    // Input
    std::string anomaly_code = {};
    std::string vehicle_vin  = {};

    // Agent control
    std::string next_action  = {};
    std::string tool_name    = {};
    int         iteration    = 0;

    // Evidence
    std::vector<std::string> observations = {};

    // Inference output — last_confidence required by ConfidenceState
    double      last_confidence   = 0.0;
    std::string fault_description = {};
    std::string severity          = {};
    std::string raw_llm_output    = {};

    // Final report
    std::string report = {};
};

static_assert(embg::embedded::ConfidenceState<DiagnosticState>);

// ─── Simulated tool implementations (same as example 04) ─────────────────────

static std::string tool_read_can_bus(const std::string& code) {
    return "CAN O2_S1=0.991V rich excursion code=" + code
         + " ECT=110°C TPS=47%";
}
static std::string tool_check_dtc(const std::string& code) {
    static const std::unordered_map<std::string, std::string> db = {
        {"P0420", "Catalyst System Efficiency Below Threshold (Bank 1)"},
        {"P0171", "System Too Lean (Bank 1)"},
        {"P0300", "Random/Multiple Cylinder Misfire Detected"},
    };
    auto it = db.find(code);
    return "DTC " + code + ": " + (it != db.end() ? it->second : "Unknown");
}
static std::string tool_read_live_pid(const std::string&) {
    return "FuelTrim ST=-3.9% LT=-6.3% O2_S2=0.712V (post-cat low) RPM=820";
}

using ToolFn = std::string(*)(const std::string&);
static const std::unordered_map<std::string, ToolFn> TOOLS = {
    {"read_can_bus",  tool_read_can_bus},
    {"check_dtc",     tool_check_dtc},
    {"read_live_pid", tool_read_live_pid},
};
static const std::vector<std::string> TOOL_SEQ = {
    "read_can_bus", "check_dtc", "read_live_pid"
};

// ─── Build graph with inference node ─────────────────────────────────────────

static embg::Graph<DiagnosticState> make_graph(embg::inference::InferenceEngine& engine) {
    embg::Graph<DiagnosticState> g;

    // ── agent ──────────────────────────────────────────────────────────────
    g.add_node("agent", [](DiagnosticState& s) {
        s.iteration++;
        const auto idx = static_cast<std::size_t>(s.iteration - 1);
        if (idx < TOOL_SEQ.size()) {
            s.next_action = "use_tool";
            s.tool_name   = TOOL_SEQ[idx];
            std::cout << "  [agent] step=" << s.iteration
                      << " → calling: " << s.tool_name << "\n";
        } else {
            s.next_action = "finish";
            std::cout << "  [agent] tools exhausted → forcing report\n";
        }
    });

    // ── tool_execute ───────────────────────────────────────────────────────
    g.add_node("tool_execute", [](DiagnosticState& s) {
        auto it = TOOLS.find(s.tool_name);
        std::string result = (it != TOOLS.end())
            ? it->second(s.anomaly_code)
            : "[tool not found]";
        s.observations.push_back("[" + s.tool_name + "] " + result);
        std::cout << "  [tool] " << result.substr(0, 80) << "\n";
    });

    // ── run_inference — uses the InferenceEngine ───────────────────────────
    // This is the key node: embg::inference::make_node() wraps the engine into
    // a NodeFn<DiagnosticState> with no coupling to the engine type.
    g.add_node("run_inference",
        embg::inference::make_node<DiagnosticState>(
            engine,

            // Build prompt from accumulated observations
            [](const DiagnosticState& s) -> embg::inference::Request {
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
            },

            // Apply inference response back to state
            [](DiagnosticState& s, const embg::inference::Response& r) {
                s.last_confidence   = r.confidence;
                s.raw_llm_output    = r.text;
                s.fault_description = tool_check_dtc(s.anomaly_code);  // structured fallback
                s.severity          = (s.anomaly_code == "P0300") ? "critical" : "medium";

                std::cout << "  [inference/" << (r.confidence >= 0.85 ? "✓" : "~")
                          << "] conf=" << r.confidence
                          << " engine=" << "stub"  // swap for engine.model_name()
                          << "\n";
                std::cout << "  [inference] response: " << r.text.substr(0, 100) << "\n";
            }
        ));

    // ── report_fault ───────────────────────────────────────────────────────
    g.add_node("report_fault", [](DiagnosticState& s) {
        std::ostringstream oss;
        oss << "\n┌── DIAGNOSTIC REPORT ─────────────────────────────────────┐\n";
        oss << "│  VIN        : " << s.vehicle_vin        << "\n";
        oss << "│  DTC        : " << s.anomaly_code       << "\n";
        oss << "│  Fault      : " << s.fault_description  << "\n";
        oss << "│  Severity   : " << s.severity           << "\n";
        oss << "│  Confidence : " << s.last_confidence    << "\n";
        oss << "│  LLM output : " << s.raw_llm_output.substr(
                                        0, std::min<std::size_t>(60, s.raw_llm_output.size()))
                                  << "\n";
        oss << "│  Evidence   : " << s.observations.size() << " tool(s)\n";
        oss << "└───────────────────────────────────────────────────────────┘\n";
        s.report = oss.str();
        std::cout << s.report;
    });

    // ── edges ──────────────────────────────────────────────────────────────
    g.add_conditional_edge("agent", [](const DiagnosticState& s) {
        return (s.next_action == "use_tool") ? "tool_execute" : "report_fault";
    });

    g.add_edge("tool_execute", "run_inference");

    g.add_conditional_edge("run_inference",
        embg::embedded::confidence_router<DiagnosticState>(
            /*threshold=*/0.85,
            /*above=*/"report_fault",
            /*below=*/"agent"
        ));

    g.add_edge("report_fault", embg::END);
    g.set_entry("agent");

    return g;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== 06 LLM Inference Node — Automotive Diagnostic ===\n";

    // ── Configure StubEngine ───────────────────────────────────────────────
    // Swap this for LlamaCppEngine when model is available.
    //
    // To use llama.cpp:
    //   #ifdef EMBG_WITH_LLAMACPP
    //   embg::inference::LlamaCppEngine engine("/path/to/model.gguf");
    //   #endif

    embg::inference::StubEngine engine;
    engine
        .add_response("P0420",
            "Catalyst system efficiency below threshold. "
            "Post-cat O2 sensor reading indicates degraded catalyst. "
            "Severity: medium. Action: inspect catalytic converter.",
            /*confidence=*/0.91)
        .add_response("P0300",
            "Random cylinder misfire detected. "
            "Multiple cylinders misfiring — check ignition, fuel injectors. "
            "Severity: critical. Action: immediate inspection required.",
            /*confidence=*/0.94)
        .add_response("misfire",
            "Misfire pattern consistent with ignition coil failure on cylinder 2.",
            /*confidence=*/0.88)
        .set_fallback("Insufficient data for confident diagnosis.", /*confidence=*/0.45);

    // ── Run scenarios ──────────────────────────────────────────────────────

    std::cout << "\n━━━ P0420 — Catalyst fault ━━━\n";
    {
        auto graph = make_graph(engine);
        DiagnosticState s;
        s.anomaly_code = "P0420";
        s.vehicle_vin  = "WBA3A5G59DNP26082";
        graph.run(s, 20);
    }

    std::cout << "\n━━━ P0300 — Critical misfire ━━━\n";
    {
        auto graph = make_graph(engine);
        DiagnosticState s;
        s.anomaly_code = "P0300";
        s.vehicle_vin  = "WBA3A5G59DNP26082";
        graph.run(s, 20);
    }

    return 0;
}
