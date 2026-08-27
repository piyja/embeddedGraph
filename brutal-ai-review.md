# embeddedGraph — Code Audit

> **✗** = reproduced by compiling and running a focused test. All other items are static-analysis findings. Status reflects remediation progress.

## At a glance

| Area | Findings | Fixed | Open |
|---|---:|---:|---:|
| Correctness (§1) | 20 | 20 | 0 |
| API design (§2) | 8 | 8 | 0 |
| Missing functionality (§3) | 10 | 2 | 8 |
| Code quality (§4) | 7 | 5 | 2 |
| Static mode (§5) | 6 | 4 | 2 |
| HSM (§6) | 6 | 4 | 2 |
| Events (§7) | 8 | 1 | 7 |
| Inference (§8) | 9 | 0 | 9 |
| Examples (§9) | 6 | 3 | 3 |
| Tests and CI (§10) | 5 | 1 (partial) | 4 |
| **Total** | **85** | **48** | **37** |

## 1. Correctness bugs

| # | Finding | Severity | Status |
|---|---|---|---|
| 1.1 | **`with_timeout` did not enforce deadlines in default mode.** A timed-out `std::async` future blocks in its destructor, so the caller still waited for `fn`. | Critical | **Fixed.** A worker thread, atomic completion flag, and copied state allow the caller to return at the deadline. Verified: 300 ms work / 50 ms deadline returns in 50 ms. |
| 1.2 | `on_timeout(state)` could race with `fn(local)` when state shares resources. | Critical | **Fixed.** Worker and timeout handler now use separate state copies. |
| 1.3 | **Static-mode `with_timeout` ignored the deadline and handler.** | Critical | **Fixed.** It measures inline work and invokes the fallback after an overrun; this is post-hoc, not preemptive. |
| 1.4 | HSM external self-transitions fire neither exit nor entry actions; no UML-compliant option exists. | High | **Fixed.** Returning the current state name now exits and re-enters it; `INTERNAL` remains the no-action option. |
| 1.5 | **HSM history is written but never read.** Re-entry always follows static `.initial`. | High | **Fixed.** Re-entry chooses the composite state’s last active child, falling back to `.initial`. |
| 1.6 | Entry does not follow `.initial` through intermediate composite states. | Medium | **Fixed.** Entry resolves the complete nested history/initial chain. |
| 1.7 | `StaticString<N> == const char*` can match a longer string because it compares only `N` bytes. | High | **Fixed.** Equality now compares complete null-terminated strings in both operand orders. |
| 1.8 | `StaticMap` is unsafe in range-for: `begin()` is a pointer but `end()` is `nullptr_t`, so it can read beyond `data_`. | High | **Fixed.** `end()` returns a past-the-end pointer (`data() + size_`); range-for and `find(...) == end()` are now well-defined. Regression-tested. |
| 1.9 | `EventGraph::process` leaves its queue dirty after an exception; the next call rethrows unless the user calls `clear()`. | Medium | **Fixed.** An RAII guard clears the queue on every `process()` exit path, exceptions included. Regression-tested. |
| 1.10 | `Function` moves non-trivially-copyable captures with `memcpy` (undefined behavior). | Medium | **Fixed.** A type-erased move stub move-constructs into the destination, then destroys the source; callables must be nothrow-move-constructible (static_assert). Regression-tested with a heap-capturing lambda. |
| 1.11 | `StaticString::find(const char*, pos)` can read beyond the terminator when `pos` exceeds the length. | Medium | **Fixed.** Rejects null substrings and any `pos > size()`. Regression-tested. |
| 1.12 | `StaticVector::back()` and `operator[]` lack bounds checks. | Low | **Fixed.** `operator[]` raises `OutOfRange`; `back()` delegates to it (empty-vector `back()` is caught too). Regression-tested. |
| 1.13 | HSM member scratch buffers make callbacks non-reentrant; nested dispatch can corrupt them. | Medium | **Fixed.** Scratch buffers are function-local now; a nested `dispatch()` from an entry action resolves correctly. Regression-tested. |
| 1.14 | `HSM::init()` does not validate cycles or dangling hierarchy references and can leave the machine partly entered. | Medium | **Fixed.** `init()` validates dangling parents/initials, name-key mismatch, and parent cycles before entering anything. Regression-tested. |
| 1.15 | **An HSM transition to an ancestor can leave `current()` stale.** When a child transitions to a parent with no `.initial`, the child exits but `current_` is not updated because no entry path is processed. | High | **Fixed — ✗ reproduced, then fixed.** Transitions to ancestors update `current_` even when no entry path runs; a nested dispatch from an entry action still wins. Regression-tested. |
| 1.16 | **Default-mode `with_timeout` delays an immediate exception until the deadline.** A throwing worker never sets `done`, so the caller spins until timeout before invoking `on_timeout`; static mode invokes it immediately. | Medium | **Fixed — ✗ reproduced, then fixed.** Worker exceptions record a `Failed` outcome; the caller joins at once and invokes `on_timeout`. Verified: immediate throw with a 100 ms deadline returns in <50 ms. |
| 1.17 | **A detached timeout worker can hold dangling reference captures.** Copying `S` protects state, but a callable such as `[&device]` can outlive the referenced object after `with_timeout` returns. | High | **Fixed** (compile-time). Default-mode `with_timeout` static_asserts a captureless `fn`, so a detached worker can never retain references; task input belongs in `S`. |
| 1.18 | **`StaticString::substr` can overflow `pos + len`.** A very large non-`npos` length can wrap the calculation and make the copy loop read beyond the string. | Medium | **Fixed.** The remainder is computed as `sz - pos` and `len` clamped against it. Regression-tested with a near-`npos` length. |
| 1.19 | **`StaticVector::resize` can expose uninitialized elements.** It increases `size_` without constructing or value-initializing newly visible entries. | Medium | **Fixed.** New elements are value-initialized; over-capacity raises `CapacityExceeded`. Regression-tested. |
| 1.20 | **`StaticMap` silently drops excess initializer-list entries.** Unlike `insert_or_assign`, construction beyond capacity does not report `CapacityExceeded`. | Medium | **Fixed.** Initializer-list construction routes through `insert_or_assign`, so overflow raises `CapacityExceeded`. Regression-tested. |

## 2. API design

| # | Finding | Status |
|---|---|---|
| 2.1 | `inference` hard-coded `embg::Config`, ignoring graph `Cfg`. | **Fixed.** Config-aware `*T<Cfg>` types and backward-compatible aliases added. |
| 2.2 | `make_node` captured an engine by raw reference without a documented lifetime rule. | **Fixed.** Documented contract and default-mode `make_node_shared()`. |
| 2.3 | `DegradedModeRunner` stored raw `Graph*` values that could dangle. | **Fixed.** Shared-ownership overload, deregistration, null checks, and documentation added. |
| 2.4 | Static-mode `with_timeout` accepted but ignored `on_timeout`. | **Fixed.** It invokes the handler and uses forwarding templates to avoid SBO overflow. |
| 2.5 | `EventEmitter` could not be unit-tested independently because construction and `bind()` were private. | **Fixed.** Public default construction and `bind()`. |
| 2.6 | `confidence_router` captured non-owning `const char*` values. | **Fixed.** It stores config-aware strings by value. |
| 2.7 | Errors were exception-only, contradicting the README’s MISRA claim. | **Fixed.** `EMBG_NO_EXCEPTIONS` and configurable error handling support no-exception/no-RTTI builds. |
| 2.8 | `Function::operator() const` used `const_cast`; concurrent calls were unsafe and undocumented. | **Fixed.** The call operator is non-const and its thread-safety contract is documented. |

## 3. Missing functionality

| # | Gap | Status |
|---|---|---|
| 3.1 | No full cancellation: default mode times out preemptively; static mode only detects an overrun. RTOS cancellation remains absent. | Partial |
| 3.2 | No-exceptions/no-RTTI support; freestanding support also absent. | **Fixed** for the former; freestanding open. |
| 3.3 | No HSM orthogonal regions (AND-states). | Open |
| 3.4 | No fork/join, entry/exit-point, choice/junction pseudostates, or deferred events. | Open |
| 3.5 | EventGraph has no priorities, timers, or deferred dispatch. | Open |
| 3.6 | Event payloads are untyped (`const void*` plus size). | Open |
| 3.7 | No sub-graph composition primitive, despite the README claim. | Open |
| 3.8 | No CAN, SPI, I2C, or UART adapters. | Open |
| 3.9 | No logging abstraction; examples use `std::cout`. | Open |
| 3.10 | No watchdog, heartbeat, or deadline-monotonic integration. | Open |

## 4. Code quality

| # | Finding | Status |
|---|---|---|
| 4.1 | Examples 04, 06, and 07 duplicate tool functions and `DiagnosticState`; no shared `tools.hpp`. | **Fixed.** `examples/automotive_tools.hpp` holds `DiagnosticState` + the tool set; 04/06/07 include it instead of redefining. |
| 4.2 | `make_full_graph()` and `make_degraded_graph()` are duplicated between 03 and 04. | **Fixed.** 03 slimmed to confidence-gated routing only; 04 now owns the `DegradedModeRunner` pattern. |
| 4.3 | `DefaultConfig` contains constants unused by `std::*` implementations. | Open |
| 4.4 | `max_tokens + 1024` is unexplained magic padding. | Open |
| 4.5 | Example 06 hard-codes `"stub"` despite `engine.model_name()`. | **Fixed.** The handler now prints `engine.model_name()`. |
| 4.6 | Example lambdas use mutable global/static state, making them non-reentrant. | **Fixed.** Per-state RNG/counters in 03, 08, 09 remove the globals. |
| 4.7 | `Function::operator() const` used `const_cast` rather than `std::function` semantics. | **Fixed** (duplicate of §2.8). |

## 5. Static allocation: remaining gaps

The framework can avoid heap allocation internally, but typical user data cannot. It is not an end-to-end heap-free mode.

| # | Gap | Status |
|---|---|---|
| 5.1 | Examples use `std::string` and `std::vector` state; `STATIC=1` does not change that. | **Fixed.** Config-aware type aliases (`Str`, `LongStr`, `StrVec`, `FloatVec`) in `example_types.hpp`; all example state structs use them. `StaticString` enhanced with `begin()`/`end()`, `operator[]`, `operator+`, cross-size conversion. |
| 5.2 | `make check` tests output equivalence, not heap allocation; there is no `operator new` poison test. | Open |
| 5.3 | `inference.hpp` used global `Config` rather than graph `Cfg`. | **Fixed** (§2.1). |
| 5.4 | `LlamaCppEngine` is heap-based and not gated by `StaticAlloc`. | Open |
| 5.5 | Static `with_timeout` discarded its deadline and callback. | **Fixed** (§1.3). |
| 5.6 | `Function` SBO move uses invalid `memcpy` for non-trivial captures. | **Fixed** (§1.10). |

## 6. HSM gaps

History (§1.5), UML-compliant external self-transitions (§1.4), `.initial` descent through intermediate composites (§1.6), hierarchy validation at `init()` (§1.14), and reentrant transition scratch space (§1.13) are **Fixed**. Orthogonal regions (AND-states) and fork/join, entry/exit-point, and choice/junction pseudostates remain open.

## 7. Event-layer gaps

The event system is FIFO-only and lacks priorities, timers, deferred events, dead-letter handling, backpressure, typed or owned event data, subscriber priorities, post-handler observation, an HSM adapter, lifecycle controls, scheduling, and coalescing. The queue-stuck-after-exception bug (§1.9) is **Fixed**; everything else remains open.

## 8. Inference-layer gaps

The llama.cpp integration remains a skeleton: no model chat template; ignored temperature; non-meaningful confidence; no version pinning; heap-based storage; no context reuse or streaming; no stop strings or grammar constraints; error text mixed into output; and a 64-byte `make_node` SBO limit for prompt builders. All remain open.

## 9. Example gaps

Examples 04/06/07 were largely the same agent loop, and 03/04 repeated the degraded-mode pattern. **Fixed:** `automotive_tools.hpp` de-duplicates the shared state/tools across 04/06/07; 03 is now a focused confidence-gated-routing example and 04 owns the `DegradedModeRunner`; mutable static state in 03/08/09 is gone. Remaining open coverage gaps: orthogonal regions, EventGraph+HSM, history, hardware, RTOS, multithreading, subgraphs, heap-free state, llama.cpp, and a dedicated `with_timeout` example — `with_timeout` is exercised by `tests/test_with_timeout.cpp` rather than a standalone example. The “voice assistant” is still a keyword classifier and the ECU HSM remains shallow by design.

## 10. Test and CI gaps

`tests/test_with_timeout.cpp` covers fast work, deadline enforcement, immediate-exception timing, and static-mode post-hoc detection. `tests/test_hsm_and_storage.cpp` adds regression coverage for HSM semantics (external self-transitions, history, ancestor transitions, reentrant entry actions, hierarchy validation) and storage safety (`StaticMap`, `StaticVector`, `StaticString`, `Function` move), plus event-queue recovery — all built and run in both modes via `make test`. The project still lacks CI, static-analysis configuration, coverage measurement, benchmarks, and fuzzing.
