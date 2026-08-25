// Shared automotive diagnostic tools for examples 04, 06, and 07.
//
// Consolidates DiagnosticState, tool functions, and the tool registry
// that were duplicated across the three examples.

#ifndef EMBG_EXAMPLES_AUTOMOTIVE_TOOLS_HPP
#define EMBG_EXAMPLES_AUTOMOTIVE_TOOLS_HPP

#include <embg/embedded.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace automotive {

// ─── State ───────────────────────────────────────────────────────────────────

struct DiagnosticState {
    std::string anomaly_code      = {};
    std::string vehicle_vin       = {};

    std::string next_action       = {};
    std::string tool_name         = {};
    int         iteration         = 0;

    std::vector<std::string> observations = {};

    double      last_confidence   = 0.0;
    std::string fault_description = {};
    std::string severity          = {};
    std::string report            = {};
};

static_assert(embg::embedded::ConfidenceState<DiagnosticState>,
    "DiagnosticState must satisfy ConfidenceState");

// ─── Tool functions ──────────────────────────────────────────────────────────

inline std::string tool_read_can_bus(const std::string& code) {
    return "CAN O2_S1=0.991V rich excursion code=" + code
         + " ECT=110C TPS=47%";
}

inline std::string tool_check_dtc(const std::string& code) {
    static const std::unordered_map<std::string, std::string> db = {
        {"P0420", "Catalyst System Efficiency Below Threshold (Bank 1)"},
        {"P0171", "System Too Lean (Bank 1)"},
        {"P0300", "Random/Multiple Cylinder Misfire Detected"},
        {"P0455", "EVAP System Large Leak Detected"},
    };
    auto it = db.find(code);
    return "DTC " + code + ": " + (it != db.end() ? it->second : "Unknown");
}

inline std::string tool_read_live_pid(const std::string&) {
    return "FuelTrim ST=-3.9% LT=-6.3% O2_S2=0.712V (post-cat low) RPM=820";
}

inline std::string tool_run_actuator_test(const std::string&) {
    return "EGR OK | Post-cat O2 delta=0.041V (threshold 0.100V) catalyst degraded";
}

// ─── Tool registry ───────────────────────────────────────────────────────────

using ToolFn = std::string(*)(const std::string&);

inline const std::unordered_map<std::string, ToolFn>& tool_registry() {
    static const std::unordered_map<std::string, ToolFn> reg = {
        {"read_can_bus",      tool_read_can_bus},
        {"check_dtc",         tool_check_dtc},
        {"read_live_pid",     tool_read_live_pid},
        {"run_actuator_test", tool_run_actuator_test},
    };
    return reg;
}

inline std::string run_tool(const std::string& name, const std::string& code) {
    const auto& reg = tool_registry();
    auto it = reg.find(name);
    return (it != reg.end()) ? it->second(code) : "[tool not found: " + name + "]";
}

} // namespace automotive

#endif // EMBG_EXAMPLES_AUTOMOTIVE_TOOLS_HPP
