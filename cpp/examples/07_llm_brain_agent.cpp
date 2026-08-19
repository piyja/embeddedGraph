// Example 07: LLM as the Orchestration Brain
//
// Key difference from example 06:
//   06: programmer hardcodes tool sequence — model only classifies
//   07: LLM decides WHICH tool to call next, AND when it has enough evidence
//
// The model receives: task + available tools + current observations
// It responds with a structured decision:
//   ACTION: use_tool | finish
//   TOOL:   <tool_name>       (only when ACTION is use_tool)
//   REASONING: <one sentence>
//
// The graph routes based on what the model decides — not programmer logic.
// This is true ReAct (Reason + Act): the model IS the orchestrator.
//
// Graph topology:
//
//   reason ──▶ [router on next_action] ──▶ tool_execute ──▶ reason  (loop)
//                                     └──▶ report_fault ──▶ END
//
// Confidence gate added: if model says "finish" but confidence < threshold,
// the embedded router overrides and forces another reasoning step.
//
// Each stage's responsibility:
//   reason        — calls the LLM, parses ACTION/TOOL/REASONING into state
//   tool_execute  — dispatches to the tool registry, appends to evidence
//   router        — routes on next_action; confidence gate overrides low-conf finish
//   report_fault  — formats and prints the final report

#include <embg/graph.hpp>
#include <embg/embedded.hpp>
#include <embg/inference.hpp>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ─── State ───────────────────────────────────────────────────────────────────

struct AgentState {
    // Input
    std::string task         = {};
    std::string vehicle_vin  = {};

    // LLM decision — written by the reason node
    std::string next_action  = {};   // "use_tool" | "finish"
    std::string tool_name    = {};   // which tool the LLM chose
    std::string llm_reasoning= {};   // model's one-line rationale

    // Evidence accumulated across turns
    std::vector<std::string> observations = {};

    // Required by ConfidenceState — set by the reason node
    double      last_confidence   = 0.0;
    std::string final_answer      = {};

    int  step = 0;
};

static_assert(embg::embedded::ConfidenceState<AgentState>);

// ─── Response parser ──────────────────────────────────────────────────────────
// Extracts ACTION, TOOL, REASONING from the model's structured output.

static std::string extract_field(const std::string& text, const std::string& key) {
    const std::string marker = key + ": ";
    auto pos = text.find(marker);
    if (pos == std::string::npos) return {};
    pos += marker.size();
    auto end = text.find('\n', pos);
    return text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// ─── Simulated tool implementations ──────────────────────────────────────────

static std::string tool_check_dtc(const std::string& task) {
    if (task.find("P0420") != std::string::npos)
        return "DTC P0420: Catalyst System Efficiency Below Threshold (Bank 1)";
    if (task.find("P0300") != std::string::npos)
        return "DTC P0300: Random/Multiple Cylinder Misfire Detected";
    return "DTC: unknown code in task";
}

static std::string tool_read_live_pid(const std::string&) {
    return "FuelTrim ST=-3.9% LT=-6.3% | O2_S2=0.712V post-cat | RPM=820 stable";
}

static std::string tool_read_can_bus(const std::string&) {
    return "CAN O2_S1=0.991V rich excursion | ECT=110°C | TPS=47% | no bus errors";
}

static std::string tool_run_actuator_test(const std::string&) {
    return "EGR OK | Post-cat O2 delta=0.041V (threshold 0.100V) → catalyst degraded";
}

using ToolFn = std::string(*)(const std::string&);
static const std::unordered_map<std::string, ToolFn> TOOLS = {
    {"check_dtc",         tool_check_dtc},
    {"read_live_pid",     tool_read_live_pid},
    {"read_can_bus",      tool_read_can_bus},
    {"run_actuator_test", tool_run_actuator_test},
};

// ─── Domain-aware stub engine ─────────────────────────────────────────────────
// Simulates an LLM that reasons about which tool to call next.
// The decision logic mirrors what a well-prompted small model would do:
//   no observations      → start with DTC lookup
//   have DTC             → get live sensor data
//   have DTC + live data → confirm with CAN bus
//   have 3+ observations → enough evidence, finish

class DiagnosticBrainStub : public embg::inference::InferenceEngine {
public:
    embg::inference::Response generate(const embg::inference::Request& req) override {
        const std::string& prompt = req.user_prompt;

        const bool has_dtc      = prompt.find("check_dtc]")         != std::string::npos;
        const bool has_live_pid = prompt.find("read_live_pid]")      != std::string::npos;
        const bool has_can      = prompt.find("read_can_bus]")       != std::string::npos;

        std::string text;
        double      confidence;

        if (!has_dtc) {
            text       = "ACTION: use_tool\n"
                         "TOOL: check_dtc\n"
                         "REASONING: Start by resolving the DTC code to a human-readable description.";
            confidence = 0.95;
        } else if (!has_live_pid) {
            text       = "ACTION: use_tool\n"
                         "TOOL: read_live_pid\n"
                         "REASONING: DTC identified — live PIDs will confirm O2 sensor and fuel trim anomaly.";
            confidence = 0.92;
        } else if (!has_can) {
            text       = "ACTION: use_tool\n"
                         "TOOL: read_can_bus\n"
                         "REASONING: Live PIDs show lean condition — CAN frames will confirm lambda sensor excursion.";
            confidence = 0.88;
        } else {
            text       = "ACTION: finish\n"
                         "REASONING: Three independent data sources confirm catalyst degradation. "
                         "Post-cat O2 delta below threshold, fuel trim lean, DTC P0420 valid. "
                         "Recommend catalytic converter replacement.";
            confidence = 0.94;
        }

        return { text, confidence, false, static_cast<int>(text.size() / 4) };
    }

    bool        is_available() const override { return true; }
    std::string model_name()   const override { return "diagnostic-brain-stub"; }
};

// ─── Inference: prompt builder + response handler ────────────────────────────
// Passed to embg::inference::make_node() to wrap the brain engine into a
// NodeFn<AgentState>.

static embg::inference::Request build_reasoning_request(const AgentState& s) {
    std::ostringstream oss;
    oss << "Task: " << s.task << "\n\n";
    oss << "Available tools:\n"
        << "  check_dtc         — look up DTC code description\n"
        << "  read_live_pid     — read live OBD-II sensor values\n"
        << "  read_can_bus      — capture raw CAN bus frames\n"
        << "  run_actuator_test — run hardware actuator self-test\n\n";
    oss << "Observations so far:\n";
    if (s.observations.empty()) {
        oss << "  (none)\n";
    } else {
        for (const auto& obs : s.observations)
            oss << "  " << obs << "\n";
    }
    oss << "\nDecide the next action. "
        << "If you have enough evidence, say finish. "
        << "Otherwise, pick exactly one tool.";

    return {
        .system_prompt =
            "You are an automotive ECU diagnostic reasoning engine. "
            "Respond with exactly three lines:\n"
            "ACTION: use_tool OR finish\n"
            "TOOL: <tool_name>  (omit line if ACTION is finish)\n"
            "REASONING: <one sentence>",
        .user_prompt   = oss.str(),
        .max_tokens    = 80,
        .temperature   = 0.05f,  // near-deterministic for embedded
    };
}

static void apply_reasoning_response(AgentState& s, const embg::inference::Response& r) {
    s.step++;
    s.last_confidence = r.confidence;
    s.next_action     = extract_field(r.text, "ACTION");
    s.tool_name       = extract_field(r.text, "TOOL");
    s.llm_reasoning   = extract_field(r.text, "REASONING");

    // Sanitise: if parse failed, default to finish
    if (s.next_action.empty()) s.next_action = "finish";

    std::cout << "  [reason/step=" << s.step << "] "
              << "action=" << s.next_action;
    if (!s.tool_name.empty()) std::cout << " tool=" << s.tool_name;
    std::cout << "\n";
    std::cout << "  [reason] \"" << s.llm_reasoning << "\"\n";
    std::cout << "  [reason] conf=" << r.confidence << "\n";

    if (s.next_action == "finish") {
        s.final_answer = s.llm_reasoning;
    }
}

// ─── Node implementations ─────────────────────────────────────────────────────

static void tool_execute_node(AgentState& s) {
    auto it = TOOLS.find(s.tool_name);
    std::string result = (it != TOOLS.end())
        ? it->second(s.task)
        : "[unknown tool: " + s.tool_name + "]";
    s.observations.push_back("[" + s.tool_name + "] " + result);
    std::cout << "  [tool/" << s.tool_name << "] " << result << "\n";
}

static void report_fault_node(AgentState& s) {
    std::cout << "\n┌── FINAL REPORT (LLM-orchestrated) ───────────────────────┐\n";
    std::cout << "│  Task:       " << s.task << "\n";
    std::cout << "│  Steps:      " << s.step << " reasoning turns\n";
    std::cout << "│  Evidence:   " << s.observations.size() << " tool result(s)\n";
    std::cout << "│  Confidence: " << s.last_confidence << "\n";
    std::cout << "│  Conclusion: " << s.final_answer << "\n";
    std::cout << "└───────────────────────────────────────────────────────────┘\n";
}

// ─── Build the graph ──────────────────────────────────────────────────────────
//
// Topology:
//   reason ──[router]──▶ tool_execute ──▶ reason  (loop)
//             │
//             └──▶ report_fault ──▶ END
//
// The reason node calls the LLM; the router dispatches on next_action.
// Confidence gate: even if model says "finish", if conf < 0.85 it routes
// back for another reasoning turn.

static embg::Graph<AgentState> make_graph(embg::inference::InferenceEngine& brain) {
    embg::Graph<AgentState> g;

    // ── Nodes ─────────────────────────────────────────────────────────────────
    // reason — calls the LLM with full context, parses the decision.
    // The model acts as the orchestrator.
    g.add_node("reason",
        embg::inference::make_node<AgentState>(
            brain,
            build_reasoning_request,
            apply_reasoning_response
        ));

    g.add_node("tool_execute", tool_execute_node);
    g.add_node("report_fault", report_fault_node);

    // ── Edges (declared together so the topology is visible at a glance) ────
    g.add_edge("tool_execute", "reason");
    g.add_edge("report_fault", embg::END);

    // ── Router: reason → {tool_execute | report_fault | reason} ──────────────
    // Primary routing: model says use_tool → tool_execute, finish → report.
    // Confidence gate sits on top: even if model says "finish",
    // if confidence < 0.85 it routes back for another reasoning turn.
    g.add_conditional_edge("reason", [](const AgentState& s) -> std::string {
        if (s.next_action == "use_tool" && !s.tool_name.empty())
            return "tool_execute";

        // Model wants to finish — but confidence gate gets the final say
        if (s.last_confidence >= 0.85)
            return "report_fault";

        // Low confidence on a finish decision → force another turn
        std::cout << "  [confidence gate] model wants to finish but conf="
                  << s.last_confidence << " < 0.85 — forcing another turn\n";
        return "reason";
    });

    // ── Entry + streaming ─────────────────────────────────────────────────────
    g.set_entry("reason");
    g.on_step([](std::string_view node, const AgentState& s) {
        std::cout << "\n── [" << node << "]  step=" << s.step
                  << "  obs=" << s.observations.size() << "\n";
    });

    return g;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== 07 LLM Brain Agent — Model-Driven Orchestration ===\n";

    DiagnosticBrainStub brain;

    // ── Task 1: P0420 ──────────────────────────────────────────────────────
    std::cout << "\n━━━ Task: Diagnose P0420 ━━━\n";
    {
        auto graph = make_graph(brain);
        AgentState s;
        s.task        = "Diagnose fault code P0420 on this vehicle.";
        s.vehicle_vin = "WBA3A5G59DNP26082";
        graph.run(s, /*max_steps=*/20);
    }

    // ── Task 2: P0300 ──────────────────────────────────────────────────────
    std::cout << "\n━━━ Task: Diagnose P0300 ━━━\n";
    {
        auto graph = make_graph(brain);
        AgentState s;
        s.task        = "Diagnose fault code P0300 on this vehicle.";
        s.vehicle_vin = "WBA3A5G59DNP26082";
        graph.run(s, /*max_steps=*/20);
    }

    return 0;
}
