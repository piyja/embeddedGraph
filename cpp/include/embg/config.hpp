#pragma once

#include <cstddef>

// Compile-time configuration for embg.
//
// Two presets:
//   DefaultConfig — uses std::* containers (heap). Current behavior.
//   StaticConfig  — uses fixed-capacity preallocated storage. No heap.
//
// Select at compile time:
//   g++ -DEMBG_STATIC_ALLOC ...   → embg::Config = StaticConfig
//   (default)                     → embg::Config = DefaultConfig
//
// The application is unaware: embg::Graph<S> resolves to Graph<S, embg::Config>,
// and the build flag flips the storage strategy. Per-type override is still
// possible: Graph<S, MyCustomConfig>.
//
// To tune static-mode capacities, define your own config struct (or specialize
// StaticConfig's constants) and pass it as the second template argument.

namespace embg {

// ─── Default: heap-backed, std::* containers ──────────────────────────────────

struct DefaultConfig {
    static constexpr bool StaticAlloc = false;

    // Capacity constants — unused by std::* containers but referenced in
    // template aliases. Keep in sync with StaticConfig.
    static constexpr std::size_t MaxNodes         = 16;
    static constexpr std::size_t MaxEdges         = 16;
    static constexpr std::size_t MaxInterrupts    = 4;
    static constexpr std::size_t MaxHsmStates     = 16;
    static constexpr std::size_t MaxHsmDepth      = 8;
    static constexpr std::size_t MaxHandlers      = 8;
    static constexpr std::size_t MaxHistory       = 8;
    static constexpr std::size_t MaxCapLevels     = 4;
    static constexpr std::size_t MaxStrLen        = 32;
    static constexpr std::size_t MaxPromptLen     = 512;
    static constexpr std::size_t MaxInferenceResp = 8;
    static constexpr std::size_t FnInlineBytes    = 64;
    // Larger SBO for router functions (confidence_router captures owned strings)
    static constexpr std::size_t RouterFnInlineBytes = 128;
    static constexpr std::size_t NodeFnInlineBytes = 256;
    // EventGraph capacities
    static constexpr std::size_t MaxEvents        = 32;  // max pending events in queue
    static constexpr std::size_t MaxSubscriptions = 32;  // total event subscriptions
};

// ─── Static: no dynamic allocation, fixed-capacity storage ────────────────────

struct StaticConfig {
    static constexpr bool StaticAlloc = true;

    // Graph capacities
    static constexpr std::size_t MaxNodes      = 16;  // max nodes per graph
    static constexpr std::size_t MaxEdges      = 16;  // max edges per graph
    static constexpr std::size_t MaxInterrupts = 4;   // max interrupt nodes

    // HSM capacities
    static constexpr std::size_t MaxHsmStates  = 16;  // max states in HSM
    static constexpr std::size_t MaxHsmDepth   = 8;   // max nesting depth
    static constexpr std::size_t MaxHandlers   = 8;   // max handlers per state
    static constexpr std::size_t MaxHistory    = 8;   // max history entries

    // DegradedModeRunner
    static constexpr std::size_t MaxCapLevels  = 4;

    // String sizes
    static constexpr std::size_t MaxStrLen     = 32;  // node/state/event names
    static constexpr std::size_t MaxPromptLen  = 512; // inference prompts/responses

    // StubEngine
    static constexpr std::size_t MaxInferenceResp = 8;

    // SBO callable storage — lambdas with captures up to this size store inline
    static constexpr std::size_t FnInlineBytes = 64;
    // Larger SBO for router functions (confidence_router captures owned strings)
    static constexpr std::size_t RouterFnInlineBytes = 128;
    // Larger SBO for node functions (inference nodes capture sub-callables)
    static constexpr std::size_t NodeFnInlineBytes = 256;

    // EventGraph capacities
    static constexpr std::size_t MaxEvents        = 32;  // max pending events in queue
    static constexpr std::size_t MaxSubscriptions = 32;  // total event subscriptions
};

// ─── Active config selection ──────────────────────────────────────────────────

#if defined(EMBG_STATIC_ALLOC)
using Config = StaticConfig;
#else
using Config = DefaultConfig;
#endif

} // namespace embg
