// Example 03: Embedded Sensor with Confidence-Gated Routing
//
// Demonstrates the primitives that LangGraph lacks but edge/embedded systems need:
//
//   1. Confidence-gated transitions  — probabilistic guard conditions
//   2. Deterministic fallback node   — rule-based decision when model is uncertain
//   3. Degraded mode graph selection — full AI vs rule-only vs safe-halt
//
// Graph topology (full capability level):
//
//   read_sensor ──▶ run_inference ──[confidence_router]
//                                       │ conf >= 0.85 ──▶ act_on_inference ──▶ log ──▶ END
//                                       └ conf <  0.85 ──▶ rule_based       ──▶ log ──▶ END
//
// Degraded capability level (no AI):
//
//   read_sensor ──▶ rule_based ──▶ log ──▶ END
//
// Each stage's responsibility:
//   read_sensor       — samples the temperature sensor
//   run_inference     — on-device model classifies the reading + sets confidence
//   act_on_inference  — acts on model output (high-confidence path)
//   rule_based        — deterministic fallback (low-confidence or degraded path)
//   log               — records the action taken

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
};

// Verify the concept at compile time — compiler error here means the state
// struct is missing the last_confidence field or it is the wrong type.
static_assert(embg::embedded::ConfidenceState<SensorState>,
    "SensorState must satisfy ConfidenceState");

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::mt19937 rng{42};

// Simulates reading a temperature sensor.
static float read_temperature() {
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
// Free functions so the graph builders read as pure topology. Where both
// graphs share identical node logic, a single function is reused.

static void read_sensor_node(SensorState& s) {
    s.raw_value = read_temperature();
    std::cout << "  [read_sensor]   raw=" << s.raw_value << " °C\n";
}

static void run_inference_node(SensorState& s) {
    // Simulates a small on-device model.
    // Clear deviation from baseline → high confidence.
    // Near the boundary → lower confidence (model is uncertain).
    const float deviation = std::abs(s.raw_value - 25.0f);

    if (deviation < 1.5f) {
        s.classification  = "normal";
        s.last_confidence = 0.96;
    } else if (deviation < 3.5f) {
        // Ambiguous zone — confidence drops below typical threshold
        s.classification  = "elevated";
        s.last_confidence = 0.71;
    } else {
        s.classification  = "critical";
        s.last_confidence = 0.92;
    }

    s.processed_value = s.raw_value;
    std::cout << "  [run_inference] class=" << s.classification
              << "  conf=" << s.last_confidence << "\n";
}

static void act_on_inference_node(SensorState& s) {
    s.action_taken = "model-driven → " + s.classification;
    s.log_entry    = "conf=" + std::to_string(s.last_confidence)
                   + " ≥ threshold — acting on model output";
    std::cout << "  [act_on_inference] " << s.action_taken << "\n";
}

// Full-capability rule-based fallback: confidence was too low to trust the model.
static void rule_based_full_node(SensorState& s) {
    s.classification = classify_by_rule(s.raw_value);
    s.action_taken   = "rule-based → " + s.classification;
    s.log_entry      = "conf=" + std::to_string(s.last_confidence)
                     + " < threshold — deterministic fallback used";
    std::cout << "  [rule_based]    " << s.action_taken << "\n";
}

// Degraded-mode rule-based: AI unavailable, rule-only processing.
static void rule_based_degraded_node(SensorState& s) {
    s.classification = classify_by_rule(s.raw_value);
    s.action_taken   = "rule-based (degraded mode) → " + s.classification;
    s.log_entry      = "running in degraded mode — AI unavailable";
    std::cout << "  [rule_based]    " << s.action_taken << "\n";
}

static void log_node(SensorState& s) {
    std::cout << "  [log]           " << s.log_entry << "\n";
}

// ─── Build the full-capability graph ─────────────────────────────────────────
//
// Topology:
//   read_sensor → run_inference → [confidence_router] → {act_on_inference | rule_based} → log → END

static embg::Graph<SensorState> make_full_graph() {
    embg::Graph<SensorState> g;

    // ── Nodes ─────────────────────────────────────────────────────────────────
    g.add_node("read_sensor",      read_sensor_node);
    g.add_node("run_inference",    run_inference_node);
    g.add_node("act_on_inference", act_on_inference_node);
    g.add_node("rule_based",       rule_based_full_node);
    g.add_node("log",              log_node);

    // ── Edges (declared together so the topology is visible at a glance) ────
    g.add_edge("read_sensor",      "run_inference");
    g.add_edge("act_on_inference", "log");
    g.add_edge("rule_based",       "log");
    g.add_edge("log",              embg::END);

    // ── Router: run_inference → {act_on_inference | rule_based} ──────────────
    // Confidence-gated router — the key embedded primitive.
    // Below 0.85 → rule_based; at or above → act_on_inference.
    g.add_conditional_edge("run_inference",
        embg::embedded::confidence_router<SensorState>(
            /*threshold=*/0.85,
            /*above=*/"act_on_inference",
            /*below=*/"rule_based"
        ));

    // ── Entry ───────────────────────────────────────────────────────────────
    g.set_entry("read_sensor");

    return g;
}

// ─── Build the degraded-mode graph (no AI) ───────────────────────────────────
//
// Topology:
//   read_sensor → rule_based → log → END

static embg::Graph<SensorState> make_degraded_graph() {
    embg::Graph<SensorState> g;

    // ── Nodes ─────────────────────────────────────────────────────────────────
    g.add_node("read_sensor", read_sensor_node);
    g.add_node("rule_based",  rule_based_degraded_node);
    g.add_node("log",         log_node);

    // ── Edges ────────────────────────────────────────────────────────────────
    g.add_edge("read_sensor", "rule_based");
    g.add_edge("rule_based",  "log");
    g.add_edge("log",         embg::END);

    // ── Entry ───────────────────────────────────────────────────────────────
    g.set_entry("read_sensor");

    return g;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== 03 Embedded Sensor — Confidence-Gated Routing ===\n";

    auto full_graph     = make_full_graph();
    auto degraded_graph = make_degraded_graph();

    embg::embedded::DegradedModeRunner<SensorState> runner;
    runner.add_level(embg::embedded::CapabilityLevel::Full,     full_graph)
          .add_level(embg::embedded::CapabilityLevel::Degraded, degraded_graph);

    // ── Run multiple sensor cycles to show both paths ─────────────────────

    std::cout << "\n--- Cycle 1 (Full capability) ---\n";
    {
        SensorState s;
        runner.run(s, embg::embedded::CapabilityLevel::Full);
        std::cout << "  Result: " << s.action_taken << "\n";
    }

    std::cout << "\n--- Cycle 2 (Full capability) ---\n";
    {
        SensorState s;
        runner.run(s, embg::embedded::CapabilityLevel::Full);
        std::cout << "  Result: " << s.action_taken << "\n";
    }

    std::cout << "\n--- Cycle 3 (Degraded — AI unavailable) ---\n";
    {
        SensorState s;
        runner.run(s, embg::embedded::CapabilityLevel::Degraded);
        std::cout << "  Result: " << s.action_taken << "\n";
    }

    return 0;
}
