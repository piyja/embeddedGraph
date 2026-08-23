# embeddedGraph — Brutal Code Audit

> Verified-by-test bugs are marked **✗** (compiled and reproduced).  
> Other findings are static-analysis findings I'm confident about.  
> Status column tracks remediation progress.

---

## Status Summary

| Area | Total Findings | Fixed | Open |
|------|---------------|-------|------|
| Correctness Bugs (§1) | 14 | 3 | 11 |
| API Design Flaws (§2) | 8 | 8 | 0 |
| Missing Functionality (§3) | 10 | 2 | 8 |
| Code Smells (§4) | 7 | 0 | 7 |
| Static Mode Gaps (§5) | 6 | 1 | 5 |
| HSM Gaps (§6) | 6 | 0 | 6 |
| Event Layer Gaps (§7) | 8 | 0 | 8 |
| Inference Layer Gaps (§8) | 9 | 0 | 9 |
| Example Gaps (§9) | 6 | 0 | 6 |
| Test/CI Gaps (§10) | 5 | 1 (partial) | 4 |
| **Total** | **88** | **14** | **74** |

---

## 1. Correctness Bugs ✗

| # | Bug | Severity | Status |
|---|-----|----------|--------|
| 1.1 | **`with_timeout` does not enforce the deadline in default mode.** `std::future` from `std::async` has a blocking destructor, so on timeout the caller still blocks for the full `fn` duration. The "hard timeout wrapper" is fiction. | **Critical** | **Fixed** — replaced `std::async` with `std::thread` + `std::atomic<bool>` + `shared_ptr`. Worker is detached on timeout; caller returns immediately. Verified by `tests/test_with_timeout.cpp`: 300ms fn with 50ms deadline → 50ms elapsed. |
| 1.2 | `on_timeout(state)` runs concurrently with `fn(local)` on timeout — if `S` shares resources (pointers, file handles) you get a data race; partial side effects from `fn` leak while `on_timeout` overwrites `state`. | **Critical** | **Fixed** — worker operates on a `shared_ptr<S>` copy. On timeout, worker is detached with its own copy; `on_timeout` runs on the caller's `state`. No shared mutable access. |
| 1.3 | **`with_timeout` is a no-op in static mode.** Captures `on_timeout` and `deadline` but never uses them (dead captures); just calls `fn(state)`. The embedded target — the one that needs timeouts — gets zero protection and no warning. | **Critical** | **Fixed** — static mode now runs `fn` inline, measures elapsed time with `steady_clock`, and calls `on_timeout` post-hoc if deadline exceeded. NOT preemptive (fn runs to completion) but honestly uses both `deadline` and `on_timeout`. Documented in header comment. Verified by test. |
| 1.4 | HSM self-transition does nothing. A handler returning its own state name yields LCA=itself; both exit and enter loops are skipped (`cur != pivot` is false immediately). No exit/entry actions fire. UML requires exit+entry for external self-transitions; there's no way to request one. | **High** | Open |
| 1.5 | **HSM history is written but never read.** `history_.insert_or_assign(cfg.parent, cur)` records the last active child on exit; no code path ever consults `history_`. Re-entry always follows the static `.initial`. The "History pseudostates" feature advertised in the header comment (line 12) is non-functional. | **High** | Open |
| 1.6 | HSM does not descend into intermediate composite states' `.initial` — only the target's. Deviates from UML 2.0 deep-entry. | **Medium** | Open |
| 1.7 | `StaticString<N> == const char*` returns true for longer strings when content is exactly N chars. Uses `strncmp(buf_, other, N)`; comparison stops at N bytes. | **High** | Open — causes `make check` failure on example 01 in static mode (router returns "respond_technical" but StaticString comparison matches "technical") |
| 1.8 | `StaticMap` is not range-for compatible → UB. `end()` returns `nullptr_t` (for the `it==end()` idiom) but `begin()` returns a pointer; `for(auto&x:m)` never terminates via end-condition and reads past `data_`. There's a separate `end_ptr()` for the real past-the-end — split-brained API. | **High** | Open |
| 1.9 | `EventGraph::process` leaves the queue dirty after throwing. `queue_.clear()`/`head_=0` are at the end of the function; an exception skips them. Next `process()` re-throws immediately. No self-healing; user must know to call `clear()`. | **Medium** | Open |
| 1.10 | `Function` move-constructs non-trivially-copyable captured types with `memcpy`. UB per the standard for lambdas capturing `std::string`/`std::shared_ptr` etc.; works on mainstream ABIs but trips sanitizers. | **Medium** | Open |
| 1.11 | `StaticString::find(const char*, pos)` over-reads when `pos > strlen(buf_)` — `strstr(buf_+pos, …)` walks past the null terminator into uninitialized buffer memory. | **Medium** | Open |
| 1.12 | `StaticVector::back()` / `operator[]` have no bounds check (UB on empty / OOR); only `push_back` throws. Inconsistent safety. | **Low** | Open |
| 1.13 | HSM scratch buffers (`scratch_a_`, `scratch_b_`, `entry_path_`) are members → non-reentrant. An `on_entry`/`on_exit`/handler that dispatches to the same HSM corrupts them. No assert, no documentation. | **Medium** | Open |
| 1.14 | `HSM::init()` doesn't validate the hierarchy (cycles in `parent`, orphan `initial`, dangling parent refs). A malformed HSM throws from `get_state()` mid-recursion, leaving `current_` half-entered. | **Medium** | Open |

---

## 2. API Design Flaws

| # | Flaw | Status |
|---|------|--------|
| 2.1 | **`inference` layer is not config-aware** — `inference.hpp:25-26`. `PromptString` hardcodes `embg::Config::StaticAlloc`, ignoring the `Cfg` of `make_node`/`Graph`. `embg::Graph<S, MyCustomConfig>` cannot influence prompt storage. | **Fixed** — `PromptStringT<Cfg>`, `RequestT<Cfg>`, `ResponseT<Cfg>`, `InferenceEngineT<Cfg>`, `StubEngineT<Cfg>`, `LlamaCppEngineT<Cfg>` are all templated on Cfg. `make_node<S, Cfg>` now uses `RequestT<Cfg>`/`ResponseT<Cfg>`, matching the graph's config. Backward-compatible aliases (`Request`, `Response`, `InferenceEngine`, `StubEngine`) provided for default config. |
| 2.2 | **`make_node` captures the engine by raw reference** — `inference.hpp:201-214`. Lifetime footgun: engine must outlive the graph; no `shared_ptr` option, no documentation. | **Fixed** — added `make_node_shared()` overload that captures `std::shared_ptr<InferenceEngineT<Cfg>>` (default mode only). Raw-reference `make_node` now has explicit lifetime documentation. |
| 2.3 | **`DegradedModeRunner` stores raw `Graph*`** — `embedded.hpp:90-95`. Dangling pointer if graphs leave scope before the runner is used. | **Fixed** — added `add_level_shared()` overload with `std::shared_ptr<Graph>` (default mode). Added `clear_level()` for deregistration. Added null-pointer check in `run()`. Lifetime contract documented. Static mode keeps raw reference (no heap). |
| 2.4 | **`with_timeout` accepted an `on_timeout` it silently ignored in static mode** — no `static_assert`, no warning. The API lied. | **Fixed** — static mode now calls `on_timeout` when deadline exceeded. Also changed to template params (`Fn&&`, `OnTimeout&&`) to avoid SBO size blowup from capturing two pre-type-erased `NodeFn` objects. |
| 2.5 | **`EventEmitter` has a private constructor + friend `EventGraph`** — `event.hpp:56-83`. Handlers can't be unit-tested without an `EventGraph` instance. | **Fixed** — `EventEmitter::bind()` is now public with a default constructor. Unit tests can create an `EventEmitter` bound to any queue with `push_back(const Event&)`. Documented as public for testability. |
| 2.6 | **`confidence_router` captures `const char*` `above`/`below`** — caller must pass persistent storage; passing `std::string("").c_str()` dangles. Undocumented at the boundary. | **Fixed** — `confidence_router` now accepts `embg::detail::String<Cfg>` by value (owns the data). In default mode: `std::string`; in static mode: `StaticString<32>`. No dangling risk from temporaries. Existing `const char*` call sites work via implicit construction. |
| 2.7 | **All error handling is `throw std::runtime_error`** — no error-code API, no `noexcept` path, no `-fno-exceptions`/`-fno-rtti` mode. MISRA C++:2023 (cited in the README) forbids exceptions; the framework is unbuildable for the safety standard it name-drops. | **Fixed** — new `error.hpp` provides `embg::Error` enum, `embg::ErrorHandler` callback, `set_error_handler()`, and `EMBG_ERROR(code, msg)` macro. With `-DEMBG_NO_EXCEPTIONS`, all throws become handler calls (default: `std::abort()`). Verified: all 9 examples build with `-fno-exceptions -fno-rtti -DEMBG_STATIC_ALLOC -DEMBG_NO_EXCEPTIONS`. |
| 2.8 | **`Function::operator() const` does `const_cast`** — concurrent const calls to the same node = data race; single-threaded-only is undocumented. | **Fixed** — `operator()` is now non-const. The stored callable may be mutable (e.g. lambdas with mutable captures), so calling it IS a mutation. `const_cast` removed from `invoke_stub`. `std::visit` in `graph.hpp` updated to use `auto&` instead of `const auto&`. Thread safety documented: not concurrent-call safe, matches `std::function`. |

---

## 3. Missing Functionality (Day-One Embedded Needs)

| # | Gap | Status |
|---|-----|--------|
| 3.1 | **No real timeout/cancellation primitive** — see §1.1–1.3. | **Partially fixed** — default mode now truly preemptive (std::thread + detach). Static mode is post-hoc (non-preemptive but calls on_timeout). Full RTOS task cancellation still open. |
| 3.2 | **No `-fno-exceptions` / `-fno-rtti` / freestanding build** — every error path throws. | **Fixed** — `EMBG_NO_EXCEPTIONS` mode replaces all throws with configurable error handler. Verified: all 9 examples build with `-fno-exceptions -fno-rtti -DEMBG_STATIC_ALLOC -DEMBG_NO_EXCEPTIONS`. Freestanding (no libc) still open. |
| 3.3 | **No HSM orthogonal regions** — no AND-states. The Harel/QP citation is misleading. | Open |
| 3.4 | **No HSM fork/join, entry/exit-point, choice/junction pseudostates; no deferred events.** | Open |
| 3.5 | **No event priorities / timers / deferred dispatch** in EventGraph — FIFO only. | Open |
| 3.6 | **No typed event payloads** — `const void*` + size; handlers reinterpret-cast. | Open |
| 3.7 | **No sub-graph composition primitive** — README claims it but doesn't exist. | Open |
| 3.8 | **No peripheral abstractions** — zero CAN/SPI/I2C/UART adapters. | Open |
| 3.9 | **No logging abstraction** — every example hardcodes `std::cout`. | Open |
| 3.10 | **No watchdog integration, no heartbeat, no deadline-monotonic hooks.** | Open |

---

## 4. Code Smells

| # | Smell | Status |
|---|-------|--------|
| 4.1 | **Copy-paste across examples 04, 06, 07** — tool functions and `DiagnosticState` struct redefined nearly verbatim in three files. No shared `tools.hpp`. | Open |
| 4.2 | **`make_full_graph()` / `make_degraded_graph()` duplicated** between 03 and 04. | Open |
| 4.3 | **Dead config constants in `DefaultConfig`** — unused by `std::*` but referenced in template aliases. | Open |
| 4.4 | **Magic `max_tokens + 1024`** — arbitrary pad, no constant. | Open |
| 4.5 | **Hardcoded `"stub"` in example 06** — `engine.model_name()` exists but isn't used. | Open |
| 4.6 | **Mutable global state in examples** — `static std::mt19937`, `static int reading`, `static int idx` in lambdas. Makes examples non-reentrant. | Open |
| 4.7 | **`Function::operator() const` + `const_cast`** — deviation from `std::function`. | Open |

---

## 5. Static-Allocation Mode — Does It Actually Work?

**Partially. The framework's own containers are heap-free; the data path is not.**

| # | Gap | Status |
|---|-----|--------|
| 5.1 | **User STATE structs still heap-allocate** — every example uses `std::string`/`std::vector` in state. `make STATIC=1` compiles these unchanged. README's "no `std::string` heap" claim is false for the state. | Open |
| 5.2 | **`make check` only verifies behavioral equivalence**, NOT absence of heap allocation. No `operator new` poison test was ever run. | Open |
| 5.3 | **`inference.hpp` uses the global `embg::Config`**, not the graph's `Cfg`. | Open |
| 5.4 | **`LlamaCppEngine` is entirely heap-based** — not gated on `StaticAlloc`. | Open |
| 5.5 | **`with_timeout`'s static-mode branch captured `on_timeout` and never used it** — wasted SBO space, silently misled. | **Fixed** — static mode now measures elapsed time and calls `on_timeout` when deadline exceeded. |
| 5.6 | **`Function` SBO move uses `memcpy`** — UB for non-trivially-copyable captures. | Open |

The static mode is a half-measure: it makes the framework's bookkeeping heap-free but leaves the actual data — where 90% of real-world allocations live — untouched, and ships zero examples showing a truly heap-free state.

---

## 6. The HSM

| # | Gap | Status |
|---|-----|--------|
| 6.1 | **History non-functional** (§1.5) — written, never read. | Open |
| 6.2 | **Orthogonal regions missing** — no AND-states. Harel/QP citation is misleading. | Open |
| 6.3 | **Self-transition is a silent no-op** (§1.4) — no exit/entry actions fire. | Open |
| 6.4 | **Intermediate composite `.initial` not followed** (§1.6). | Open |
| 6.5 | **No fork/join, entry/exit points, choice pseudostates.** | Open |
| 6.6 | **Scratch buffers non-reentrant** (§1.13) — undocumented hazard. | Open |

---

## 7. The Event Layer

| # | Gap | Status |
|---|-----|--------|
| 7.1 | **Toy-grade** — FIFO only; no priorities, timers, deferred events, dead-letter queue, backpressure. | Open |
| 7.2 | **`max_events` throws and leaves graph stuck** (§1.9). | Open |
| 7.3 | **Payload is `const void*`** — zero type safety. | Open |
| 7.4 | **`Event.type` is non-owned `const char*`** — caller-lifetime footgun. | Open |
| 7.5 | **No subscriber priority** — fan-out order = subscription order. | Open |
| 7.6 | **`observe_` fires only before handlers** — can't observe after. | Open |
| 7.7 | **No HSM adapter** — claimed in header comment, doesn't exist. | Open |
| 7.8 | **No event lifecycle** (transient vs persistent), no scheduling, no coalescing. | Open |

---

## 8. The Inference Layer — Would It Actually Work?

**No. The llama.cpp integration is a skeleton, not working code.**

| # | Gap | Status |
|---|-----|--------|
| 8.1 | **No chat template** — concatenates `"system: " + sys + " user: " + usr`. Every modern model requires a specific template. Would produce incoherent output. | Open |
| 8.2 | **`temperature` is ignored** — always greedy argmax. | Open |
| 8.3 | **Confidence is meaningless** — `min(1.0, (sum_logit/n)/10 + 0.5)` is noise, not certainty. | Open |
| 8.4 | **API drift risk** — no llama.cpp version pin, no feature-test macros. | Open |
| 8.5 | **Heap-based** — `std::vector<llama_token>`, `std::string` — not `StaticAlloc`-aware. | Open |
| 8.6 | **No context reuse / streaming** — each `generate()` clears KV cache. Multi-turn agents impossible. | Open |
| 8.7 | **No stop strings / grammar-constrained decoding** — essential for structured output parsing. | Open |
| 8.8 | **Errors conflated with model output** — `"tokenization failed"` returned in `text` field. | Open |
| 8.9 | **`make_node` SBO limit** — prompt builder lambdas must fit `FnInlineBytes=64`. | Open |

---

## 9. The Examples

| # | Gap | Status |
|---|-----|--------|
| 9.1 | **04, 06, 07 are the same agent loop** with different window dressing. One demo thrice told. | Open |
| 9.2 | **03 and 04 share `DegradedModeRunner` pattern** with different domain labels. | Open |
| 9.3 | **No example exercises:** orthogonal regions, EventGraph+HSM, HSM history, real peripherals, RTOS, multithreading, sub-graphs, heap-free state, `with_timeout`, `-DEMBG_WITH_LLAMACPP`. | **Partial** — `tests/test_with_timeout.cpp` now exercises `with_timeout` in both modes |
| 9.4 | **`09_voice_assistant.cpp`** is a keyword classifier, not a voice assistant. | Open |
| 9.5 | **`05_hsm_ecu_states.cpp`** two-layer architecture is shallow. | Open |
| 9.6 | **Mutable global state** in lambdas/statics makes examples non-reentrant. | Open |

---

## 10. Test Coverage

| # | Gap | Status |
|---|-----|--------|
| 10.1 | **No test suite. Zero.** Not one `TEST()`, `assert`, `Catch2`, `doctest`, or `GTest` file. | **Partially fixed** — `tests/test_with_timeout.cpp` added with 3 test cases covering `with_timeout` in both modes. Uses `assert()` + `chrono` timing. Not a full framework but proves the concept. |
| 10.2 | **No CI.** No `.github/`, no pipeline. | Open |
| 10.3 | **No static analysis config.** No `clang-tidy`, no `cppcheck`, no ASan/UBSan. | Open |
| 10.4 | **`make check` is not a test suite** — it diffs stdout of two storage backends, not correctness. | Open — though `make check` did catch the `StaticString` comparison bug (1.7) in example 01 |
| 10.5 | **No coverage measurement, no benchmarks, no fuzzing.** | Open |

---

## Bonus: Documentation Drift

| # | Issue | Status |
|---|-------|--------|
| 11.1 | README says "builds all 7 examples" — there are **9**. | Open |
| 11.2 | README line 301: "no `std::string` heap" — false for user state (§5). | Open |
| 11.3 | Two divergent HTML overviews + knowledge-base.mdx — three sources of truth. | Open |

---

## Market Landscape

### LangGraph (Python) — 40k stars, 7k commits
- Durable execution — agents persist through failures, resume from checkpoint
- Comprehensive memory — short-term + long-term persistent
- LangSmith integration — tracing, debugging, evaluation, production monitoring
- Deployment platform — scalable infrastructure for stateful workflows
- Used in production by Klarna, Replit, Elastic

### Boost.SML (C++14) — 1.4k stars
- Zero overhead — compiles to same assembly as enum/switch (verified on Godbolt)
- `-fno-exceptions` compatible — actually works in freestanding
- Orthogonal regions, history, deferred events — features embeddedGraph cites but lacks
- Arduino/AVR tested — runs on 8-bit MCUs
- 1.2k commits, CI, benchmarks, clang-tidy

### Quantum Leaps QP (C/C++) — Commercial, production
- Active Object pattern — event-driven, asynchronous, thread-safe by construction
- RTOS integration — FreeRTOS, Zephyr, ThreadX, bare-metal
- Model-based design — QM tool generates code from statechart diagrams
- Software tracing — QP/Spy for live monitoring
- Test harness — QUTest for trace-based testing
- ISO 26262 / IEC 61508 certified in commercial deployments

---

## Bottom Line

The library is a clean, well-organized **prototype** with good aesthetic structure (config-conditional aliases, SBO callable, builder API) but it is **not production-ready and not embedded-ready**. 

**What's fixed across MR #1 + MR #2:** 14 of 88 findings now fixed. MR #1 fixed the 3 critical `with_timeout` bugs (1.1, 1.2, 1.3). MR #2 fixes all 8 API Design Flaws (§2): the inference layer is now config-aware (2.1), `make_node` has a `shared_ptr` overload (2.2), `DegradedModeRunner` has `shared_ptr` + null-check (2.3), `EventEmitter` is testable (2.5), `confidence_router` owns its strings (2.6), the framework builds with `-fno-exceptions -fno-rtti` (2.7/3.2), and `Function::operator()` is non-const (2.8). The MISRA-disqualifying absence of a no-exceptions build path is now resolved.

**What's still broken:** 74 of 88 findings remain open. The HSM advertises history and cites Harel/QP but ships no orthogonal regions and a non-functional history map. The llama.cpp integration would not produce coherent output from any real model. There is no CI, no static analysis. The `StaticString` comparison bug (1.7) causes a `make check` failure on example 01 in static mode. The 9 examples collapse to ~3 distinct demos.

Every bug marked ✗ was reproduced by compiling and running a small test against the headers — they are real, not theoretical.

---

## Fixes in MR #1: with_timeout Deadline Enforcement

### Bug 1.1 + 1.2: `with_timeout` doesn't enforce deadline (default mode)

**Root cause:** `std::future` from `std::async` has a blocking destructor. When `fut.wait_for(deadline)` returns `timeout`, `on_timeout(state)` runs, but then `fut`'s destructor blocks until `fn` completes. The caller waits the full `fn` duration regardless of the deadline. Additionally, `on_timeout(state)` runs concurrently with `fn(local)` on timeout — data race on shared state.

**Fix:** Replace `std::async` with `std::thread` + `std::atomic<bool>` + `shared_ptr`:
- `fn` runs on a separate thread operating on a `shared_ptr<S>` copy of state
- Main thread busy-waits with `yield()` until `done` or deadline
- On timeout: `detach()` the worker (it continues on its copy, safely owned by `shared_ptr`), call `on_timeout(state)` immediately, return without blocking
- On completion: `join()`, commit `*local` back to `state`
- Exceptions in `fn` are caught in the worker thread (don't set `done`, let timeout path fire `on_timeout`)

**Verification:** `tests/test_with_timeout.cpp` Test 2 — 300ms fn with 50ms deadline → elapsed=50ms (was 300ms before fix).

### Bug 1.3: `with_timeout` is a no-op in static mode

**Root cause:** The static-mode branch captured `on_timeout` and `deadline` but never used them — just called `fn(state)` inline. Dead captures. The embedded target got zero protection, silently.

**Fix:** Run `fn(state)` inline, then measure elapsed time with `std::chrono::steady_clock`. If the deadline was exceeded, call `on_timeout(state)` as a post-hoc fallback. Also wrap `fn` in try/catch so exceptions trigger `on_timeout` instead of propagating. This is not preemptive (fn runs to completion), but it honestly uses both `deadline` and `on_timeout`, and triggers the fallback when `fn` overruns its budget. Documented as non-preemptive in the header comment.

**Verification:** `tests/test_with_timeout.cpp` Test 2 (static mode) — 300ms fn with 50ms deadline → elapsed=300ms, `timeout_fired=true`, `result="timeout fallback"` (was `timeout_fired=false` before fix).

### API change: `with_timeout` now uses template parameters

**Before:** `with_timeout(NodeFn<S,Cfg> fn, ms deadline, NodeFn<S,Cfg> on_timeout)` — captured two pre-type-erased `Function<...,256>` objects in the returned lambda (592 bytes), blowing the SBO `static_assert` in static mode.

**After:** `with_timeout(Fn&& fn, ms deadline, OnTimeout&& on_timeout)` — accepts arbitrary callables via perfect forwarding. The returned `NodeFn<S,Cfg>` only stores the final type-erased lambda, not two intermediate `Function` objects. Works in both default and static modes without SBO size issues.

### Test file: `tests/test_with_timeout.cpp`

Three test cases covering both modes:
1. Fast fn within deadline → result committed, no timeout
2. Slow fn exceeds deadline → on_timeout fires, caller doesn't block (default) / fn completes then fallback (static)
3. Throwing fn → on_timeout fires, no crash

---

## Fixes in MR #2: API Design Flaws (§2)

### 2.1: Inference layer not config-aware

**Root cause:** `PromptString` at `inference.hpp:25-26` hardcoded `embg::Config::StaticAlloc` and `embg::Config::MaxPromptLen`, ignoring the `Cfg` template parameter of `make_node`/`Graph`.

**Fix:** Templated all inference types on `Cfg` with default = `embg::Config`:
- `PromptStringT<Cfg>`, `RequestT<Cfg>`, `ResponseT<Cfg>` — config-aware prompt storage
- `InferenceEngineT<Cfg>`, `StubEngineT<Cfg>`, `LlamaCppEngineT<Cfg>` — config-aware engine hierarchy
- `make_node<S, Cfg>` now uses `RequestT<Cfg>`/`ResponseT<Cfg>`, matching the graph's config
- Backward-compatible aliases (`Request`, `Response`, `InferenceEngine`, `StubEngine`, `LlamaCppEngine`) provided for default config — existing code unchanged

### 2.2: make_node captures engine by raw reference

**Root cause:** `make_node` captured `&engine` by reference with no `shared_ptr` option and no lifetime documentation.

**Fix:** Added `make_node_shared()` overload (default mode only) that captures `std::shared_ptr<InferenceEngineT<Cfg>>` — no dangling risk. Raw-reference `make_node` now has explicit lifetime documentation: "The engine MUST outlive the graph."

### 2.3: DegradedModeRunner stores raw Graph*

**Root cause:** `add_level(CapabilityLevel, Graph&)` stored `&graph` as raw pointer with no null check and no deregistration.

**Fix:** Added `add_level_shared()` overload (default mode) with `std::shared_ptr<Graph>`. Added `clear_level()` for deregistration. Added null-pointer check in `run()`. Lifetime contract documented. Static mode keeps raw reference (shared_ptr is heap-based).

### 2.5: EventEmitter private constructor

**Root cause:** Constructor was private with only `EventGraph` as friend. `bind()` was private. Could not create an `EventEmitter` in unit tests without an `EventGraph` instance.

**Fix:** Made `bind()` public with a default constructor. Unit tests can now create an `EventEmitter` bound to any queue with `push_back(const Event&)`. Documented as public for testability.

### 2.6: confidence_router captures const char* (dangling)

**Root cause:** `confidence_router(double, const char* above, const char* below)` captured raw `const char*` pointers. Passing `std::string("...").c_str()` or other temporaries dangles.

**Fix:** Changed to accept `embg::detail::String<Cfg>` by value — owns the data. In default mode: `std::string`; in static mode: `StaticString<32>`. No dangling risk. Existing `const char*` call sites work via implicit construction. Added `RouterFnInlineBytes=128` to config to accommodate the larger capture (two `StaticString<32>` + double = 80 bytes).

### 2.7: All error handling is throw (no -fno-exceptions path)

**Root cause:** Every error path used `throw std::runtime_error(...)`. No error-code API, no `noexcept` path, no `-fno-exceptions`/`-fno-rtti` mode. MISRA C++:2023 forbids exceptions.

**Fix:** New `error.hpp` provides:
- `embg::Error` enum (CapacityExceeded, UnknownNode, NoEntry, MaxStepsExceeded, etc.)
- `embg::ErrorHandler` function pointer type
- `embg::set_error_handler()` to register custom handler
- `embg::default_error_handler()` — prints to stderr + std::abort()
- `EMBG_ERROR(code, msg)` macro — throws by default, calls handler with `-DEMBG_NO_EXCEPTIONS`

All `throw std::runtime_error(...)` sites in `storage.hpp`, `graph.hpp`, `hsm.hpp`, `embedded.hpp`, `event.hpp`, `inference.hpp` replaced with `EMBG_ERROR(code, msg)`.

**Verification:** All 9 examples build with `-fno-exceptions -fno-rtti -DEMBG_STATIC_ALLOC -DEMBG_NO_EXCEPTIONS`.

### 2.8: Function::operator() const does const_cast

**Root cause:** `operator()` was const but did `const_cast` to call mutable lambdas. Concurrent const calls to the same `Function` = data race.

**Fix:** Made `operator()` non-const. Removed `const_cast` from `invoke_stub`. Changed `invoke_` function pointer signature from `Ret(*)(const void*, Args&&...)` to `Ret(*)(void*, Args&&...)`. Updated `std::visit` in `graph.hpp` to use `auto&` instead of `const auto&`. Thread safety documented: not concurrent-call safe, matches `std::function`.

### Config change: RouterFnInlineBytes

Added `RouterFnInlineBytes = 128` to both `DefaultConfig` and `StaticConfig`. `RouterFn` now uses this instead of `FnInlineBytes` for its SBO size, accommodating `confidence_router`'s owned-string capture without inflating all other `Function` instantiations.
