// Example 07: LLM as the Orchestration Brain
//
// Key difference from example 06:
//   06: programmer hardcodes tool sequence -- model only classifies
//   07: LLM decides WHICH tool to call next, AND when it has enough evidence
//
// The model receives: task + available tools + current observations
// It responds with a structured decision:
//   ACTION: use_tool | finish
//   TOOL:   <tool_name>       (only when ACTION is use_tool)
//   REASONING: <one sentence>
//
// The graph routes based on what the model decides -- not programmer logic.
// This is true ReAct (Reason + Act): the model IS the orchestrator.
//
// Uses shared automotive tools from automotive_tools.hpp.

#include <embg/graph.hpp>
#include <embg/embedded.hpp>
#include <embg/inference.hpp>
#include "automotive_tools.hpp"
#include "example_types.hpp"
#include <cctype>
#include <cstdio>
#include <iostream>

using Str     = embg::examples::Str;
using LongStr = embg::examples::LongStr<>;
using StrVec  = embg::examples::StrVec<>;

struct AgentState {
    LongStr task         = {};
    Str     vehicle_vin  = {};
    Str     anomaly_code = {};

    Str     next_action  = {};
    Str     tool_name    = {};
    LongStr llm_reasoning= {};

    StrVec  observations = {};

    double  last_confidence = 0.0;
    LongStr final_answer    = {};

    int     step = 0;
};

static_assert(embg::embedded::ConfidenceState<AgentState>);

static Str extract_dtc(const LongStr& task) {
    auto p = task.find("P0");
    if (p != LongStr::npos && p + 4 < task.size()
        && std::isdigit(static_cast<unsigned char>(task[p + 2]))
        && std::isdigit(static_cast<unsigned char>(task[p + 3]))
        && std::isdigit(static_cast<unsigned char>(task[p + 4]))) {
        return task.substr(p, 5);
    }
    return {};
}

// ─── Domain-aware stub engine ─────────────────────────────────────────────────
// Simulates an LLM that reasons about which tool to call next.

class DiagnosticBrainStub : public embg::inference::InferenceEngineT<> {
public:
    using Req = embg::inference::RequestT<>;
    using Resp = embg::inference::ResponseT<>;
    using PStr = embg::inference::PromptStringT<>;

    Resp generate(const Req& req) override {
        const PStr& prompt = req.user_prompt;

        const bool has_dtc      = prompt.find("check_dtc]")         != PStr::npos;
        const bool has_live_pid = prompt.find("read_live_pid]")      != PStr::npos;
        const bool has_can      = prompt.find("read_can_bus]")       != PStr::npos;

        PStr text;
        double confidence;

        if (!has_dtc) {
            text       = "ACTION: use_tool\n"
                         "TOOL: check_dtc\n"
                         "REASONING: Start by resolving the DTC code to a human-readable description.";
            confidence = 0.95;
        } else if (!has_live_pid) {
            text       = "ACTION: use_tool\n"
                         "TOOL: read_live_pid\n"
                         "REASONING: DTC identified -- live PIDs will confirm O2 sensor and fuel trim anomaly.";
            confidence = 0.92;
        } else if (!has_can) {
            text       = "ACTION: use_tool\n"
                         "TOOL: read_can_bus\n"
                         "REASONING: Live PIDs show lean condition -- CAN frames will confirm lambda sensor excursion.";
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

static embg::inference::Request build_reasoning_request(const AgentState& s) {
    std::string prompt = "Task: ";
    prompt += std::string(s.task);
    prompt += "\n\nAvailable tools:\n"
        "  check_dtc         -- look up DTC code description\n"
        "  read_live_pid     -- read live OBD-II sensor values\n"
        "  read_can_bus      -- capture raw CAN bus frames\n"
        "  run_actuator_test -- run hardware actuator self-test\n\n"
        "Observations so far:\n";
    if (s.observations.empty()) {
        prompt += "  (none)\n";
    } else {
        for (std::size_t i = 0; i < s.observations.size(); ++i) {
            prompt += "  ";
            prompt += std::string(s.observations[i]);
            prompt += "\n";
        }
    }
    prompt += "\nDecide the next action. "
        "If you have enough evidence, say finish. "
        "Otherwise, pick exactly one tool.";

    return {
        .system_prompt =
            "You are an automotive ECU diagnostic reasoning engine. "
            "Respond with exactly three lines:\n"
            "ACTION: use_tool OR finish\n"
            "TOOL: <tool_name>  (omit line if ACTION is finish)\n"
            "REASONING: <one sentence>",
        .user_prompt   = prompt.c_str(),
        .max_tokens    = 80,
        .temperature   = 0.05f,
    };
}

static LongStr extract_field(const LongStr& text, const char* key) {
    LongStr marker = key;
    marker += ": ";
    auto pos = text.find(marker.c_str());
    if (pos == LongStr::npos) return {};
    pos += marker.size();
    auto end = text.find('\n', pos);
    return text.substr(pos, end == LongStr::npos ? LongStr::npos : end - pos);
}

static void apply_reasoning_response(AgentState& s, const embg::inference::Response& r) {
    s.step++;
    s.last_confidence = r.confidence;
    s.next_action     = extract_field(r.text, "ACTION");
    s.tool_name       = extract_field(r.text, "TOOL");
    s.llm_reasoning   = extract_field(r.text, "REASONING");

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
    LongStr result = automotive::run_tool(s.tool_name, s.anomaly_code);
    LongStr obs;
    obs += "[";
    obs += s.tool_name.c_str();
    obs += "] ";
    obs += result.c_str();
    s.observations.push_back(obs);
    std::cout << "  [tool/" << s.tool_name << "] " << result << "\n";
}

static void report_fault_node(AgentState& s) {
    std::cout << "\n+-- FINAL REPORT (LLM-orchestrated) ----------------+\n";
    std::cout << "|  Task:       " << s.task << "\n";
    std::cout << "|  Steps:      " << s.step << " reasoning turns\n";
    std::cout << "|  Evidence:   " << s.observations.size() << " tool result(s)\n";
    std::cout << "|  Confidence: " << s.last_confidence << "\n";
    std::cout << "|  Conclusion: " << s.final_answer << "\n";
    std::cout << "+----------------------------------------------------+\n";
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

    g.add_node("reason",
        embg::inference::make_node<AgentState>(
            brain,
            build_reasoning_request,
            apply_reasoning_response
        ));

    g.add_node("tool_execute", tool_execute_node);
    g.add_node("report_fault", report_fault_node);

    g.add_edge("tool_execute", "reason");
    g.add_edge("report_fault", embg::END);

    g.add_conditional_edge("reason", [](const AgentState& s) -> Str {
        if (s.next_action == "use_tool" && !s.tool_name.empty())
            return "tool_execute";

        if (s.last_confidence >= 0.85)
            return "report_fault";

        std::cout << "  [confidence gate] model wants to finish but conf="
                  << s.last_confidence << " < 0.85 -- forcing another turn\n";
        return "reason";
    });

    g.set_entry("reason");
    g.on_step([](std::string_view node, const AgentState& s) {
        std::cout << "\n-- [" << node << "]  step=" << s.step
                  << "  obs=" << s.observations.size() << "\n";
    });

    return g;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== 07 LLM Brain Agent -- Model-Driven Orchestration ===\n";

    DiagnosticBrainStub brain;

    std::cout << "\n--- Task: Diagnose P0420 ---\n";
    {
        auto graph = make_graph(brain);
        AgentState s;
        s.task        = "Diagnose fault code P0420 on this vehicle.";
        s.vehicle_vin = "WBA3A5G59DNP26082";
        s.anomaly_code = extract_dtc(s.task);
        graph.run(s, /*max_steps=*/20);
    }

    std::cout << "\n--- Task: Diagnose P0300 ---\n";
    {
        auto graph = make_graph(brain);
        AgentState s;
        s.task        = "Diagnose fault code P0300 on this vehicle.";
        s.vehicle_vin = "WBA3A5G59DNP26082";
        s.anomaly_code = extract_dtc(s.task);
        graph.run(s, /*max_steps=*/20);
    }

    return 0;
}
