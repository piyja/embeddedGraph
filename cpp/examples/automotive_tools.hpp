// Shared automotive diagnostic tools for examples 04, 06, and 07.
//
// Consolidates DiagnosticState, tool functions, and the tool registry
// that were duplicated across the three examples.

#ifndef EMBG_EXAMPLES_AUTOMOTIVE_TOOLS_HPP
#define EMBG_EXAMPLES_AUTOMOTIVE_TOOLS_HPP

#include <embg/embedded.hpp>
#include "example_types.hpp"

namespace automotive {

using Str     = embg::examples::Str;
using LongStr = embg::examples::LongStr<>;
using StrVec  = embg::examples::StrVec<>;

// ─── State ───────────────────────────────────────────────────────────────────

struct DiagnosticState {
    Str     anomaly_code      = {};
    Str     vehicle_vin       = {};

    Str     next_action       = {};
    Str     tool_name         = {};
    int     iteration         = 0;

    StrVec  observations      = {};

    double  last_confidence   = 0.0;
    LongStr fault_description = {};
    Str     severity          = {};
    LongStr report            = {};
};

static_assert(embg::embedded::ConfidenceState<DiagnosticState>,
    "DiagnosticState must satisfy ConfidenceState");

// ─── Tool functions ──────────────────────────────────────────────────────────

inline LongStr tool_read_can_bus(const Str& code) {
    LongStr result = "CAN O2_S1=0.991V rich excursion code=";
    result += code.c_str();
    result += " ECT=110C TPS=47%";
    return result;
}

inline LongStr tool_check_dtc(const Str& code) {
    struct DtcEntry { const char* code; const char* desc; };
    static const DtcEntry db[] = {
        {"P0420", "Catalyst System Efficiency Below Threshold (Bank 1)"},
        {"P0171", "System Too Lean (Bank 1)"},
        {"P0300", "Random/Multiple Cylinder Misfire Detected"},
        {"P0455", "EVAP System Large Leak Detected"},
    };
    for (const auto& entry : db) {
        if (code == entry.code) {
            LongStr result = "DTC ";
            result += code.c_str();
            result += ": ";
            result += entry.desc;
            return result;
        }
    }
    LongStr result = "DTC ";
    result += code.c_str();
    result += ": Unknown";
    return result;
}

inline LongStr tool_read_live_pid(const Str&) {
    return "FuelTrim ST=-3.9% LT=-6.3% O2_S2=0.712V (post-cat low) RPM=820";
}

inline LongStr tool_run_actuator_test(const Str&) {
    return "EGR OK | Post-cat O2 delta=0.041V (threshold 0.100V) catalyst degraded";
}

// ─── Tool registry ───────────────────────────────────────────────────────────

using ToolFn = LongStr(*)(const Str&);

struct ToolEntry {
    const char* name;
    ToolFn      fn;
};

inline const ToolEntry* tool_registry(std::size_t& count) {
    static const ToolEntry reg[] = {
        {"read_can_bus",      tool_read_can_bus},
        {"check_dtc",         tool_check_dtc},
        {"read_live_pid",     tool_read_live_pid},
        {"run_actuator_test", tool_run_actuator_test},
    };
    count = sizeof(reg) / sizeof(reg[0]);
    return reg;
}

inline LongStr run_tool(const Str& name, const Str& code) {
    std::size_t count = 0;
    const auto* reg = tool_registry(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (name == reg[i].name) return reg[i].fn(code);
    }
    LongStr result = "[tool not found: ";
    result += name.c_str();
    result += "]";
    return result;
}

} // namespace automotive

#endif // EMBG_EXAMPLES_AUTOMOTIVE_TOOLS_HPP
