#pragma once

// Error handling abstraction for embg.
//
// By default, embg throws std::runtime_error on errors. This is fine for
// desktop development and testing.
//
// For -fno-exceptions / MISRA / safety-critical builds, define EMBG_NO_EXCEPTIONS
// at compile time. This replaces throws with a configurable error handler
// (default: std::abort). Register a custom handler with embg::set_error_handler()
// to integrate with your platform's error reporting (logging, watchdog, reset).
//
//   g++ -DEMBG_NO_EXCEPTIONS ...  → no throws, calls error handler instead
//
// Usage in embg headers:
//   EMBG_ERROR(CapacityExceeded, "StaticVector: capacity exceeded");
//
// The handler receives the error code and message. If it returns, execution
// continues past the error site (the caller must handle the degraded state).
// Most handlers will not return (abort, reset, trap).

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string_view>

namespace embg {

// ─── Error codes ──────────────────────────────────────────────────────────────

enum class Error {
    // Storage
    CapacityExceeded,
    EmptyFunction,

    // Graph
    UnknownNode,
    NoEntry,
    MaxStepsExceeded,

    // HSM
    UnknownState,
    NoInitialState,

    // Event
    MaxEventsExceeded,

    // Inference
    EngineUnavailable,

    // DegradedModeRunner
    NoGraphRegistered,
};

// ─── Error handler ─────────────────────────────────────────────────────────────

using ErrorHandler = void(*)(Error, const char* message);

// Set a custom error handler. Must not be null.
// Called when EMBG_NO_EXCEPTIONS is defined and an error occurs.
// If the handler returns, execution continues past the error site.
void set_error_handler(ErrorHandler handler) noexcept;

// Get the current error handler.
ErrorHandler get_error_handler() noexcept;

// Default error handler: prints to stderr and calls std::abort().
void default_error_handler(Error code, const char* message) noexcept;

} // namespace embg

// ─── EMBG_ERROR macro ──────────────────────────────────────────────────────────
//
// Expands to either a throw or a handler call depending on EMBG_NO_EXCEPTIONS.

namespace embg::detail {

inline ErrorHandler& error_handler_ref() noexcept {
    static ErrorHandler handler = &default_error_handler;
    return handler;
}

} // namespace embg::detail

inline void embg::set_error_handler(ErrorHandler handler) noexcept {
    if (handler) detail::error_handler_ref() = handler;
}

inline embg::ErrorHandler embg::get_error_handler() noexcept {
    return detail::error_handler_ref();
}

inline void embg::default_error_handler(Error code, const char* message) noexcept {
    const char* label = "unknown";
    switch (code) {
        case Error::CapacityExceeded:   label = "capacity exceeded"; break;
        case Error::EmptyFunction:      label = "call on empty function"; break;
        case Error::UnknownNode:        label = "unknown node"; break;
        case Error::NoEntry:            label = "no entry node"; break;
        case Error::MaxStepsExceeded:   label = "max steps exceeded"; break;
        case Error::UnknownState:       label = "unknown HSM state"; break;
        case Error::NoInitialState:     label = "no initial state"; break;
        case Error::MaxEventsExceeded:  label = "max events exceeded"; break;
        case Error::EngineUnavailable:  label = "engine unavailable"; break;
        case Error::NoGraphRegistered:  label = "no graph registered"; break;
    }
    std::fputs("embg error [", stderr);
    std::fputs(label, stderr);
    std::fputs("]: ", stderr);
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

#if defined(EMBG_NO_EXCEPTIONS)
    #define EMBG_ERROR(code, msg) \
        do { \
            ::embg::detail::error_handler_ref()(::embg::Error::code, (msg)); \
        } while(0)
#else
    #include <stdexcept>
    #define EMBG_ERROR(code, msg) \
        do { \
            throw ::std::runtime_error((msg)); \
        } while(0)
#endif
