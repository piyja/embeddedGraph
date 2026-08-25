// Example 03: Embedded Sensor with Confidence-Gated Routing
//
// Demonstrates the primitives LangGraph lacks but edge/embedded systems need:
//
//   1. Confidence-gated transitions  — probabilistic guard conditions
//   2. Deterministic rule fallback   — when the model is uncertain, a fixed
//                                      rule handles the decision (no AI needed)
//
// Graph topology:
//
//   read_sensor ──▶ run_inference ──[confidence_router]
//                                       │ conf >= 0.85 ──▶ act_on_inference ──▶ log ──▶ END
//                                       └ conf <  0.85 ──▶ rule_based       ──▶ log ──▶ END

#include <embg/graph.hpp>
#include <embg/embedded.hpp>
#include <cmath>
#include <iostream>
#include <random>
#include <string>

// ─── State ───────────────────────────────────────────────────────────────────

struct SensorState {
    float       raw_value         = 0.0f;
    float       processed_value   = 0.0f;
    double      last_confidence   = 0.0;   // required by ConfidenceState concept
    std::string classification;            // "normal" | "elevated" | "critical"
    std::string action_taken;
    std::string log_entry;
    std::mt19937 rng{42};                  // per-state RNG — reentrant, no global state
};

static_assert(embg::embedded::ConfidenceState<SensorState>,
    "SensorState must satisfy ConfidenceState");

// ─── Helpers ─────────────────────────────────────────────────────────────────

static float read_temperature(std::mt19937& rng) {
    std::normal_distribution<float> dist(25.0f, 4.0f);
    return dist(rng);
}

// Rule-based classification — always deterministic, no model required.
static std::string classify_by_rule(float value) {
    if (value > 32.0f) return "critical";
    if (value > 28.0f) return "elevated";
    return "normal";
}

// ─── Node implementations ─────────────────────────────────────────────────────

static void read_sensor_node(SensorState& s) {
    s.raw_value = read_temperature(s.rng);
    std::cout << "  [read_sensor]   raw=" << s.raw_value << " C\n";
}

static void run_inference_node(SensorState& s) {
    // Simulated on-device model: clear deviation -> high confidence,
    // near the boundary -> low confidence (model is uncertain).
    const float dev = std::abs(s.raw_value - 25.0f);
    if      (dev < 1.5f) { s.classification = "normal";   s.last_confidence = 0.96; }
    else if (dev < 3.5f) { s.classification = "elevated"; s.last_confidence = 0.71; }
    else                 { s.classification = "critical"; s.last_confidence = 0.92; }
    s.processed_value = s.raw_value;
    std::cout << "  [run_inference] class=" << s.classification
              << "  conf=" << s.last_confidence << "\n";
}

static void act_on_inference_node(SensorState& s) {
    s.action_taken = "model-driven -> " + s.classification;
    s.log_entry    = "conf=" + std::to_string(s.last_confidence)
                   + " >= threshold -- acting on model output";
    std::cout << "  [act_on_inference] " << s.action_taken << "\n";
}

static void rule_based_node(SensorState& s) {
    s.classification = classify_by_rule(s.raw_value);
    s.action_taken   = "rule-based -> " + s.classification;
    s.log_entry      = "conf=" + std::to_string(s.last_confidence)
                     + " < threshold -- deterministic fallback used";
    std::cout << "  [rule_based]    " << s.action_taken << "\n";
}

static void log_node(SensorState& s) {
    std::cout << "  [log]           " << s.log_entry << "\n";
}

// ─── Build the graph ──────────────────────────────────────────────────────────

static embg::Graph<SensorState> make_graph() {
    embg::Graph<SensorState> g;

    g.add_node("read_sensor",      read_sensor_node);
    g.add_node("run_inference",    run_inference_node);
    g.add_node("act_on_inference", act_on_inference_node);
    g.add_node("rule_based",       rule_based_node);
    g.add_node("log",              log_node);

    g.add_edge("read_sensor",      "run_inference");
    g.add_edge("act_on_inference", "log");
    g.add_edge("rule_based",       "log");
    g.add_edge("log",              embg::END);

    // Confidence gate: the key embedded primitive.
    g.add_conditional_edge("run_inference",
        embg::embedded::confidence_router<SensorState>(
            /*threshold=*/0.85,
            /*above=*/"act_on_inference",
            /*below=*/"rule_based"
        ));

    g.set_entry("read_sensor");
    return g;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== 03 Embedded Sensor -- Confidence-Gated Routing ===\n";

    auto g = make_graph();

    for (int i = 1; i <= 3; ++i) {
        std::cout << "\n--- Cycle " << i << " ---\n";
        SensorState s;
        g.run(s);
        std::cout << "  Result: " << s.action_taken << "\n";
    }

    return 0;
}
