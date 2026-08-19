embeddedGraph cpp/ — Brutal Code Audit
Verified-by-test bugs are marked ✗ (I compiled and reproduced them). Other findings are static-analysis findings I'm confident about.


1. Correctness bugs ✗
#	Bug
1.1	with_timeout does not enforce the deadline in default mode. std::future from std::async has a blocking destructor, so on timeout the caller still blocks for the full fn duration. The "hard timeout wrapper" is fiction.
1.2	on_timeout(state) runs concurrently with fn(local) on timeout — if S shares resources (pointers, file handles) you get a data race; partial side effects from fn leak while on_timeout overwrites state.
1.3	with_timeout is a no-op in static mode. Captures on_timeout and deadline but never uses them (dead captures); just calls fn(state). The embedded target — the one that needs timeouts — gets zero protection and no warning.
1.4	HSM self-transition does nothing. A handler returning its own state name yields LCA=itself; both exit and enter loops are skipped (cur != pivot is false immediately). No exit/entry actions fire. UML requires exit+entry for external self-transitions; there's no way to request one.
1.5	HSM history is written but never read. history_.insert_or_assign(cfg.parent, cur) records the last active child on exit; no code path ever consults history_. Re-entry always follows the static .initial. The "History pseudostates" feature advertised in the header comment (line 12) is non-functional.
1.6	HSM does not descend into intermediate composite states' .initial — only the target's. Deviates from UML 2.0 deep-entry.
1.7	StaticString<N> == const char* returns true for longer strings when content is exactly N chars. Uses strncmp(buf_, other, N); comparison stops at N bytes.
1.8	StaticMap is not range-for compatible → UB. end() returns nullptr_t (for the it==end() idiom) but begin() returns a pointer; for(auto&x:m) never terminates via end-condition and reads past data_. There's a separate end_ptr() for the real past-the-end — split-brained API.
1.9	EventGraph::process leaves the queue dirty after throwing. queue_.clear()/head_=0 are at the end of the function; an exception skips them. Next process() re-throws immediately. No self-healing; user must know to call clear().
1.10	Function move-constructs non-trivially-copyable captured types with memcpy. UB per the standard for lambdas capturing std::string/std::shared_ptr etc.; works on mainstream ABIs but trips sanitizers.
1.11	StaticString::find(const char*, pos) over-reads when pos > strlen(buf_) — strstr(buf_+pos, …) walks past the null terminator into uninitialized buffer memory.
1.12	StaticVector::back() / operator[] have no bounds check (UB on empty / OOR); only push_back throws. Inconsistent safety.
1.13	HSM scratch buffers (scratch_a_, scratch_b_, entry_path_) are members → non-reentrant. An on_entry/on_exit/handler that dispatches to the same HSM corrupts them. No assert, no documentation.
1.14	HSM::init() doesn't validate the hierarchy (cycles in parent, orphan initial, dangling parent refs). A malformed HSM throws from get_state() mid-recursion, leaving current_ half-entered.


2. API design flaws
- inference layer is not config-aware despite the template — inference.hpp:25-26. PromptString hardcodes embg::Config::StaticAlloc, ignoring the Cfg of make_node/Graph. The comment on line 22 ("uses embg::Config for string storage") is a lie — it doesn't use Cfg. embg::Graph<S, MyCustomConfig> cannot influence prompt storage.
- make_node captures the engine by raw reference — inference.hpp:201-214. Lifetime footgun: engine must outlive the graph; no shared_ptr option, no documentation.
- DegradedModeRunner stores raw Graph* — embedded.hpp:90-95. Dangling pointer if graphs leave scope before the runner is used.
- with_timeout accepts an on_timeout it silently ignores in static mode — embedded.hpp:46-75. No static_assert, no warning. The API lies.
- EventEmitter has a private constructor + friend EventGraph — event.hpp:56-83. Handlers can't be unit-tested without an EventGraph instance.
- confidence_router captures const char* above/below — embedded.hpp:33-42. Caller must pass persistent storage (string literals); passing std::string("").c_str() dangles. Undocumented at the boundary.
- All error handling is throw std::runtime_error — graph.hpp:126,135,157; hsm.hpp:88,138; event.hpp:179; storage.hpp:139,202,315. No error-code API, no noexcept path, no -fno-exceptions/-fno-rtti mode. MISRA C++:2023 (cited in the README) forbids exceptions; the framework is unbuildable for the safety standard it name-drops.
- Function::operator() const does const_cast — storage.hpp:313-317,331-336. Comment claims it "matches std::function's internal behavior" — it doesn't; std::function::operator() is non-const for mutable callables. Concurrent const calls to the same node = data race; single-threaded-only is undocumented.
3. Missing functionality (day-one embedded needs)
- No real timeout/cancellation primitive — see §1.1–1.3. The header comment at embedded.hpp:10-12 punts to "Phase 2."
- No -fno-exceptions / -fno-rtti / freestanding build — every error path throws.
- No HSM orthogonal regions — hsm.hpp has no AND-states. The Harel/QP citation in the header is misleading given a core feature is absent. Real automotive/industrial HSMs need regions (e.g., OPERATING || DIAGNOSTIC).
- No HSM fork/join, entry/exit-point, choice/junction pseudostates; no deferred events.
- No event priorities / timers / deferred dispatch in EventGraph (event.hpp) — FIFO only.
- No typed event payloads — event.hpp:44-48. const void* + size; handlers reinterpret-cast. No schema, no ownership.
- No sub-graph composition primitive — README claims "a Graph node can call EventGraph::process()" but there's no first-class subgraph node, no state-scoping, no child-graph lifecycle.
- No peripheral abstractions — zero CAN/CAN-FD/LIN/FlexRay/SPI/I2C/UART adapters. The "automotive" examples simulate CAN with string literals (04_automotive_diagnostic.cpp:66-69).
- No logging abstraction — every example hardcodes std::cout. No log levels, no severity filtering for ASIL contexts.
- No watchdog integration, no heartbeat, no deadline-monotonic hooks.
4. Code smells
- Copy-paste across examples 04, 06, 07 — tool_check_dtc, tool_read_can_bus, tool_read_live_pid, tool_run_actuator_test are redefined nearly verbatim in three files. No shared tools.hpp. Same DiagnosticState struct copy-pasted between 04_automotive_diagnostic.cpp:36-57 and 06_llm_diagnostic.cpp:36-57.
- make_full_graph() / make_degraded_graph() duplicated between 03 and 04 with minor edits.
- Dead config constants in DefaultConfig — config.hpp:31-47. The comment admits "unused by std::* containers but referenced in template aliases." Should be factored into a shared base.
- Magic max_tokens + 1024 — inference.hpp:126. Arbitrary pad, no constant, no justification.
- Hardcoded "stub" in example 06 — 06_llm_diagnostic.cpp:157. engine.model_name() exists but isn't used; the abstraction is shown then ignored.
- Mutable global state hidden in free functions / function-local statics:
- examples/03_embedded_sensor.cpp:44 — static std::mt19937 rng{42}
- examples/08_event_sensor_hub.cpp:68 — static int reading = 0 inside read_temperature_sensor
- examples/09_voice_assistant.cpp:240-247 — static int idx = 0 inside the joke node lambda (joke rotation lives outside VoiceState)
- examples/09_voice_assistant.cpp:313 — the graph object is reused across 8 turns with a mutable step counter that never resets between user inputs.
These make examples non-reentrant and non-reproducible across graph instances.
- Function::operator() const + const_cast (see §2) — code-smell deviation from std::function.
5. Static-allocation mode — does it actually work?
Partially. The framework's own containers are heap-free; the data path is not.
- The user's STATE structs still heap-allocate at runtime. Every example uses std::string/std::vector in state: 01_simple_chain.cpp:18-23, 02_agent_loop.cpp:19-26, 03_embedded_sensor.cpp:28-35, 04_automotive_diagnostic.cpp:36-57, 05_hsm_ecu_states.cpp:44-50, 06_llm_diagnostic.cpp:36-57, 07_llm_brain_agent.cpp:35-53, 08_event_sensor_hub.cpp:40-57, 09_voice_assistant.cpp:41-61. make STATIC=1 compiles these unchanged. README line 301's "no std::string heap" claim is false for the state — which is where most allocation actually happens.
- make check only verifies behavioral equivalence of default vs static output, NOT absence of heap allocation. No -fno-exceptions, no ASan/LSan run, no global operator new poison test was ever performed. The "static mode works" claim is unverified.
- inference.hpp uses the global embg::Config, not the graph's Cfg (see §2) — a custom static config doesn't change prompt storage.
- LlamaCppEngine is entirely heap-based — std::string model_path_ (inference.hpp:184), std::vector<llama_token> (line 126), std::string output (line 140) — and isn't gated on StaticAlloc. The "real" inference path can't be used in a no-heap build.
- with_timeout's static-mode branch captures on_timeout (an SBO Function) and never uses it — embedded.hpp:56-59. Wastes FnInlineBytes and silently misleads.
- Function SBO move uses memcpy — UB for non-trivially-copyable captures (§1.10).
The static mode is a half-measure: it makes the framework's bookkeeping heap-free but leaves the actual data — where 90% of real-world allocations live — untouched, and ships zero examples showing a truly heap-free state.
6. The HSM
- History non-functional (§1.5) — written, never read.
- Orthogonal regions missing — no AND-states. The Harel/QP citation (hsm.hpp:5-6) is misleading.
- LCA is correct for the cases I tested (siblings, parent, child, top-level) but:
- self-transition is a silent no-op (§1.4);
- intermediate composite .initial not followed (§1.6);
- no fork/join, no entry/exit points, no choice pseudostates.
- do_transition uses current_ (active leaf) as from, not the handling state. This happens to be correct because LCA exits up from the leaf, but the design conflates "transition source" with "active leaf" — any future local/transition-action feature will be built on quicksand.
- Scratch buffers are non-reentrant (§1.13) — undocumented hazard.
- init() performs no hierarchy validation (§1.14).
7. The event layer
- Toy-grade. FIFO only; no priorities, no timers, no deferred events, no dead-letter queue, no backpressure.
- max_events throws and leaves the graph stuck (§1.9).
- Payload is const void* (event.hpp:44-48) — zero type safety; every handler reinterpret-casts. No schema, no ownership.
- Event.type is a non-owned const char* — caller-lifetime footgun; documented but unenforced.
- No subscriber priority — fan-out order = subscription order; no way to express "alarm handler runs before logger."
- observe_ fires only before handlers (event.hpp:187) — can't observe after, can't filter.
- No HSM adapter — the header comment (lines 18-22) claims "HSM on_transition can post events to an EventGraph" but there's no adapter; user wires it manually.
- No event lifecycle (transient vs persistent), no scheduling, no coalescing.
8. The inference layer — would it actually work?
No. The llama.cpp integration is a skeleton, not working code.
- No chat template. inference.hpp:122-124 builds the prompt as "system: " + sys + " user: " + usr. Every modern instruction-tuned model (Phi-3, Qwen2.5, Gemma 2, Llama-3, TinyLlama-chat) requires a specific chat template (ChatML / Llama-3 <|...|> / etc.). Without it, the model emits incoherent text. The integration would not produce usable output.
- req.temperature is ignored — inference.hpp:145-169 is always greedy argmax. The Request field is dead.
- Confidence is meaningless. inference.hpp:160-173: min(1.0, (sum_logit/n)/10 + 0.5). Logits are unbounded and model-specific; this number does not measure certainty and is incomparable across models. The entire downstream confidence_router gates on noise.
- API drift risk. llama_tokenize, llama_batch_get_one, llama_get_logits_ith, llama_token_to_piece signatures have changed across llama.cpp releases; the code targets one unspecified snapshot. No version pin, no feature-test macros.
- std::vector<llama_token> tokens(req.max_tokens + 1024) — heap, arbitrarily sized, and not StaticAlloc-aware.
- No context reuse / session memory / streaming — each generate() clears KV cache and is fully stateless, making multi-turn agents (example 07) impossible to wire to a real engine efficiently.
- No stop strings, no grammar/constrained decoding — essential for the structured "ACTION/TOOL/REASONING" parsing example 07 depends on.
- Errors are conflated with model output — inference.hpp:131,138 return "tokenization failed" / "decode failed" in the text field.
- make_node's inner Function<Request(const S&)> must fit FnInlineBytes=64 or static_assert fires — easy to blow with non-trivial prompt builders (example 06/07 build an ostringstream inside; the lambda itself is small but the pattern invites failure).
9. The examples
- 04, 06, 07 are the same agent loop with different window dressing. All three: agent → tool_execute → run_inference → confidence_router → (loop | report) for automotive DTC diagnosis. 04 uses simulated inference; 06 swaps in StubEngine; 07 swaps in a "brain" stub that picks tools. Graph topology and control flow are identical — one demo thrice told.
- 03 and 04 share the DegradedModeRunner pattern with different domain labels (sensor vs automotive).
- No example exercises: HSM orthogonal regions (can't — missing); EventGraph + HSM together; HSM history (can't — broken); a real CAN/SPI/I2C peripheral; a real RTOS integration; multi-threaded event processing; sub-graph composition; static mode with a heap-free state struct; with_timeout actually timing out; -DEMBG_WITH_LLAMACPP (the real inference path is unverified by any example).
- 09_voice_assistant.cpp is a linear keyword classifier, not a voice assistant — no ASR, no TTS, no streaming, no wake-word. The summary at lines 324-329 admits everything that matters is a stub.
- 05_hsm_ecu_states.cpp runs a trivial 2-node diagnostic graph inside HSM states; the "two-layer architecture" is shallow — the inner graph does nothing the HSM couldn't do directly.
- Mutable global state hidden in lambdas/statics (§4) makes examples non-reentrant.
10. Test coverage
- No test suite. Zero. Not one TEST(), assert, Catch2, doctest, or GTest file in the repo. (The grep hits are all static_assert in examples/storage.hpp.)
- No CI. No .github/, no .gitlab-ci.yml, no .circleci/, no azure-pipelines.yml. Three commits, no pipeline.
- No static analysis config. No clang-tidy, no cppcheck, no scan-build, no -fsanitize=address,undefined in any target. The Makefile uses -Wall -Wextra -Wpedantic only.
- make check is not a test suite — it builds default + static and diffs stdout of 9 examples. This verifies behavioral equivalence of two storage strategies, not correctness. It cannot catch any of the 14 bugs in §1 because no example exercises those paths. It's a regression check for the storage layer, marketed as testing.
- No coverage measurement (gcov/llvm-cov), no benchmarks, no fuzzing.
- README line 104 / 257 imply CI exists ("StubEngine — canned responses for dev/test/CI ✅"). It does not.
Bonus: documentation drift
- README says "builds all 7 examples" (README.md:281, 291) — there are 9. The Makefile lists 9.
- README line 301: "no std::string heap" — false for user state (§5).
- Two divergent HTML overviews: cpp/overview.html (981 lines) and project-overview.html (1277 lines), plus knowledge-base.mdx (748 lines). Three sources of truth for one small library.
Bottom line
The library is a clean, well-organized prototype with good aesthetic structure (config-conditional aliases, SBO callable, builder API) but it is not production-ready and not embedded-ready. The two headline features that distinguish it from LangGraph — static allocation and hard timeouts — are both half-implemented: static mode only covers framework bookkeeping (not the user state where allocation actually lives), and with_timeout enforces no deadline in either mode. The HSM advertises history and cites Harel/QP but ships no orthogonal regions and a non-functional history map. The llama.cpp integration would not produce coherent output from any real model. There are zero tests, zero CI, zero static analysis, and the one "check" target is marketed as testing while only diffing two storage backends. The 9 examples collapse to ~3 distinct demos. For a safety-critical/embedded pitch that name-drops MISRA, ASIL, and Samek's QP, the absence of a no-exceptions build path alone is disqualifying.
Every bug marked ✗ above was reproduced by compiling and running a small test against your headers — they are real, not theoretical.