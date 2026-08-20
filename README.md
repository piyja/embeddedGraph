# embeddedGraph

**Graph-Based Orchestration for Embedded & Edge Systems — C++20, Zero-Dependency Core**

A deterministic orchestration framework for building smart applications on embedded/edge targets. Combines a statechart-based Hierarchical State Machine (HSM) with a LangGraph-inspired execution graph. Use it for AI agents, sensor pipelines, control systems, protocol handlers, or any application that needs structured, routable, stateful execution with a deterministic safety shell.

The graph core is general-purpose — nodes mutate state, edges route. AI inference is an optional plug-in, not a requirement. Build a pure state machine, a pure pipeline, or a full agentic loop with confidence-gated LLM calls. All from the same framework.

```
┌─────────────────────────────────────────────────────────────┐
│  HSM  (embg::hsm)                                           │
│  Deterministic behavioral contract                          │
│  OPERATING ──▶ DEGRADED ──▶ SAFE_HALT                     │
│                                                             │
│  ┌─── OPERATING ──────────────────────────────────────┐     │
│  │  ┌── NORMAL ──┐    ┌── ALERT ──┐                   │     │
│  │  │            │    │           │                   │     │
│  │  │  ┌─────── Graph ─────────┐  │                   │     │
│  │  │  │  agent → tool → infer │  │                   │     │
│  │  │  │  → confidence_router  │  │                   │     │
│  │  │  │  → report_fault       │  │                   │     │
│  │  │  └───────────────────────┘  │                   │     │
│  │  └─────────────────────────────┘                   │     │
│  └────────────────────────────────────────────────────┘     │
└─────────────────────────────────────────────────────────────┘
```

## Why

LangGraph (Python) popularized orchestration graphs for LLM agents — but it has no embedded story, no state machines, no safety primitives, no deterministic fallback. embeddedGraph fills that gap. And while it's inspired by AI agent patterns, the core is a general-purpose graph + HSM engine that works equally well for non-AI applications: sensor processing, control loops, protocol state machines, workflow engines, or any system that needs structured routing with a deterministic shell.

| Concern | LangGraph | embeddedGraph |
|---|---|---|
| Language | Python | C++20 |
| Target | Cloud / desktop | MCU / SoC / RTOS |
| Scope | AI agents only | Any graph-based application |
| Deterministic shell | None | HSM (UML 2.0 statecharts) |
| Confidence gating | None | `confidence_router` primitive |
| Degraded mode | None | `DegradedModeRunner` |
| Timeout enforcement | None | `with_timeout` wrapper |
| Human-in-the-loop | `interrupt()` | `set_interrupt()` |
| Static allocation | None | `-DEMBG_STATIC_ALLOC` (no heap) |
| Dependencies | langchain, pydantic, etc. | stdlib only |

## Architecture — Two-Layer Pattern

The key insight: **the HSM is the behavioral contract, the Graph is the execution loop that runs inside a state.** For AI agents the graph handles tool calling and LLM inference. For non-AI applications the graph handles whatever your domain needs — sensor reads, control decisions, protocol steps, data transformations.

- **HSM** — manages coarse system states (OPERATING / DEGRADED / SAFE_HALT). Handles events like `fault_detected`, `ai_unavailable`, `critical_fault`. Transitions are deterministic, verifiable, and follow UML 2.0 LCA semantics.
- **Graph** — runs *within* an HSM state. For AI agents: tool calling, LLM inference, evidence accumulation, confidence-gated routing. For other applications: any node-edge pipeline with conditional routing, cycles, and streaming.

When the HSM enters SAFE_HALT, all graph execution stops. When it enters DEGRADED, only rule-based graphs run. This is the pattern that makes embedded systems — AI or not — certifiable.

## C++ Core

### `embg::Graph<S>` — Execution Engine

The core execution graph. Nodes mutate state; edges route. Routing is always separate from execution (mirrors LangGraph's model).

```cpp
embg::Graph<MyState> g;
g.add_node("step_a", [](MyState& s) { /* mutate s */ })
  .add_node("step_b", [](MyState& s) { /* mutate s */ })
  .add_edge("step_a", "step_b")
  .add_conditional_edge("step_b", [](const MyState& s) -> std::string {
      return s.done ? embg::END : "step_a";   // cycle
  })
  .set_entry("step_a");

g.run(state);
```

**Features:** conditional routing, cycles (agent loops), streaming via `on_step()`, human-in-the-loop via `set_interrupt()`, max-step safety bound.

### `embg::hsm::HSM<S>` — Hierarchical State Machine

UML 2.0 compliant statecharts. Nested states, entry/exit actions, event propagation up the hierarchy, history pseudostates, LCA-based transitions.

```cpp
embg::hsm::HSM<MyState> hsm;
hsm.add_state({ .name = "OPERATING", .initial = "NORMAL",
                .on_entry = [](MyState& s) { ... },
                .handlers = { {"fault", ...} } })
   .add_state({ .name = "NORMAL", .parent = "OPERATING", ... })
   .set_initial("OPERATING");

hsm.init(state);
hsm.dispatch("fault", state);   // LCA transition
```

### `embg::embedded` — Embedded Primitives

Primitives LangGraph lacks but embedded systems need:

- **`confidence_router<S>(threshold, above, below)`** — branches based on `state.last_confidence`. The probabilistic guard condition.
- **`with_timeout(fn, deadline, on_timeout)`** — hard execution deadline with deterministic fallback.
- **`DegradedModeRunner<S>`** — selects which graph to run based on capability level (Full / Degraded / MinimalSafe).

### `embg::inference` — LLM/SLM Integration

Swappable inference engine abstraction:

- **`InferenceEngine`** — abstract interface (`generate`, `is_available`, `model_name`)
- **`StubEngine`** — canned responses for dev/test/CI
- **`LlamaCppEngine`** — real on-device inference via llama.cpp (compile with `-DEMBG_WITH_LLAMACPP`)
- **`make_node<S>(engine, build_prompt, apply_response)`** — factory that wraps any engine into a `NodeFn<S>`

### `embg::event` — Event-Driven Execution Layer

Reactive, event-driven execution — complements the synchronous Graph and the state-driven HSM. External events drive node execution; nodes emit new events that propagate to multiple subscribers.

- **`EventGraph<S>`** — pub/sub + event queue + reactive processing loop
- **`on(type, handler)`** — subscribe a handler to an event type (multiple per type = fan-out)
- **`post(type)`** — inject an external event into the queue
- **`process(state)`** — drain the queue, dispatching to all matching handlers
- **`EventEmitter`** — passed to handlers; call `emit.emit("new_event")` to generate events

```cpp
embg::event::EventGraph<MyState> hub;

hub.on("tick", [](MyState& s, embg::event::EventEmitter& emit) {
    s.value = read_sensor();
    emit.emit("sensor_data");
})
.on("sensor_data", [](MyState& s, embg::event::EventEmitter&) {  // fan-out 1
    log_reading(s.value);
})
.on("sensor_data", [](MyState& s, embg::event::EventEmitter& emit) {  // fan-out 2
    if (s.value > s.threshold) emit.emit("alarm");
})
.on("alarm", [](MyState& s, embg::event::EventEmitter&) {
    s.led_on = true;
});

hub.post("tick");
hub.process(state);  // drains queue: tick → sensor_data → [alarm] → ...
```

## Project Structure

```
embeddedGraph/
├── cpp/
│   ├── include/embg/
│   │   ├── graph.hpp          # Core execution graph
│   │   ├── hsm.hpp            # Hierarchical state machine
│   │   ├── embedded.hpp       # Confidence router, timeout, degraded mode
│   │   ├── inference.hpp      # LLM engine abstraction + llama.cpp integration
│   │   ├── config.hpp         # Compile-time config (default vs static alloc)
│   │   ├── error.hpp          # Error handling (throw or -fno-exceptions handler)
│   │   ├── storage.hpp        # Fixed-capacity primitives (StaticString, StaticMap, Function)
│   │   └── event.hpp          # Event-driven execution layer (pub/sub, fan-out, event queue)
│   ├── examples/
│   │   ├── 01_simple_chain.cpp           # Linear pipeline + conditional branch
│   │   ├── 02_agent_loop.cpp             # ReAct pattern (cycles)
│   │   ├── 03_embedded_sensor.cpp        # Confidence-gated routing
│   │   ├── 04_automotive_diagnostic.cpp  # Full agent: tools + inference + degraded mode
│   │   ├── 05_hsm_ecu_states.cpp         # HSM managing ECU system states
│   │   ├── 06_llm_diagnostic.cpp         # LLM inference node (StubEngine)
│   │   ├── 07_llm_brain_agent.cpp        # LLM as orchestration brain
│   │   ├── 08_event_sensor_hub.cpp       # Event-driven sensor hub (non-AI, pub/sub)
│   │   └── 09_voice_assistant.cpp        # Voice assistant (ASR→NLU→arbitration→agent→TTS)
│   ├── Makefile
│   └── overview.html
├── knowledge-base.mdx         # Deep research: LangGraph, FSM/HSM, automotive safety
└── README.md
```

## Examples

### 01 — Simple Chain
Linear pipeline with conditional branch. The simplest pattern: preprocess → classify → route → respond.

### 02 — Agent Loop (ReAct)
Cycles — the key thing LangGraph adds over LangChain. Agent decides whether to call a tool or finish, looping until done.

### 03 — Embedded Sensor
Confidence-gated routing. Model output above threshold → act on inference. Below threshold → deterministic rule-based fallback. Plus degraded mode (no AI at all).

### 04 — Automotive Diagnostic Agent
Full agent: tool calling loop + inference + confidence gating + degraded mode. Simulates ECU diagnostic with CAN bus, DTC lookup, live PIDs, actuator tests.

### 05 — HSM ECU States
The two-layer pattern in action. HSM manages OPERATING/DEGRADED/SAFE_HALT. Graph runs the diagnostic agent *within* OPERATING states.

### 06 — LLM Inference Node
Plugs a real `InferenceEngine` (StubEngine by default, LlamaCppEngine optional) into the graph via `make_node()`.

### 07 — LLM Brain Agent
LLM decides *which tool to call next*, not the programmer. The model IS the orchestrator. Confidence gate overrides low-confidence "finish" decisions.

### 08 — Event-Driven Sensor Hub (non-AI)
Reactive sensor processing with `EventGraph`: external tick events → sensor read → fan-out to logger + threshold checker → alarm → fan-out to LED + notification. Demonstrates event-driven execution, fan-out, and event generation without any AI/LLM.

### 09 — Voice Assistant
Full voice assistant pipeline: ASR (input) → NLU (intent classification + entity extraction) → Arbitration (confidence gate) → Router (conditional edge to matching agent) → Agent (satisfy request) → TTS (output). No AI/LLM — NLU is keyword-based, agents are rule-based. Swap NLU for an LLM node and agents for tool-calling sub-graphs to go production.

## Use Cases — Beyond AI Agents

The graph + HSM core is general-purpose. The examples use AI agents for demonstration, but the same primitives apply to:

| Domain | Graph usage | HSM usage |
|---|---|---|
| **AI agents** | Tool calling, LLM inference, ReAct loops | OPERATING / DEGRADED / SAFE_HALT |
| **Sensor pipelines** | Read → filter → fuse → actuate | Calibration / running / fault |
| **Protocol handlers** | Parse → validate → route → respond | Handshake / connected / error / closed |
| **Control systems** | Sense → compute PID → actuate | Auto / manual / tuning / safe |
| **Workflow engines** | Step → approve → branch → complete | Draft / review / approved / rejected |
| **Robotics** | Plan → execute → feedback → replan | Idle / executing / emergency / recovery |
| **IoT gateways** | Ingest → transform → route → publish | Online / offline / degraded / maintenance |

The pattern is the same: **Graph handles the domain logic (with cycles, routing, streaming), HSM handles the system-level safety contract.** AI is just one domain where the graph happens to call an LLM.

## LangGraph → embeddedGraph Mapping

| LangGraph Concept | embeddedGraph Equivalent |
|---|---|
| `StateGraph` | `embg::Graph<S>` |
| `add_node(name, fn)` | `add_node(name, NodeFn<S>)` |
| `add_edge(from, to)` | `add_edge(from, to)` |
| `add_conditional_edge(from, router)` | `add_conditional_edge(from, RouterFn<S>)` |
| `START` / `END` | `embg::START` / `embg::END` |
| `interrupt()` | `set_interrupt(node, fn)` |
| Event stream | `on_step(StepFn<S>)` |
| *(none)* | `embg::hsm::HSM<S>` — deterministic shell |
| *(none)* | `confidence_router` — probabilistic guard |
| *(none)* | `DegradedModeRunner` — capability layers |
| *(none)* | `with_timeout` — deadline enforcement |

## Safety & Hardening Roadmap

The current C++ core uses `std::unordered_map`, `std::function`, `std::async`, and exceptions — suitable for Linux/POSIX embedded targets. The hardening roadmap targets bare-metal RTOS and safety certification:

| Phase | Goal | Status |
|---|---|---|
| **Phase 1** | Functional core on Linux/POSIX | ✅ Done |
| **Phase 2** | Replace `std::async` with RTOS task primitives | Planned |
| **Phase 3** | Replace heap allocation with static pools / `std::array` | ✅ Done |
| **Phase 4** | Replace exceptions with error codes / `std::expected` | Planned |
| **Phase 5** | MISRA C++:2023 compliance audit | Planned |
| **Phase 6** | Bare-metal port (Cortex-M / RISC-V) | Planned |

The design keeps the FFI seam open: should a Rust core (Option B) ever be revisited, the `extern "C"` boundary is straightforward. But the current path is all-C++ — simplicity and independence are the safety story, not language choice.

## Feature Matrix

| Feature | Status |
|---|---|
| Execution graph with conditional routing | ✅ |
| Cycles (agent loops) | ✅ |
| Streaming / observation hooks | ✅ |
| Human-in-the-loop interrupts | ✅ |
| Hierarchical state machine (UML 2.0) | ✅ |
| Confidence-gated routing | ✅ |
| Timeout wrapper | ✅ |
| Degraded mode runner | ✅ |
| LLM inference abstraction | ✅ |
| Stub engine (dev/test) | ✅ |
| llama.cpp integration | ✅ (skeleton) |
| Checkpointing / NVRAM persistence | Planned |
| Orthogonal regions | Planned |
| Deferred events | Planned |
| Multimodal inference | Planned |
| CAN / MQTT / ROS2 adapters | Planned |
| Graph visualization (DOT export) | Planned |
| Model hot-swap | Planned |
| MCP (Model Context Protocol) | Planned |

## Recommended Small Language Models

| Model | Size | Target | Notes |
|---|---|---|---|
| Qwen2.5 0.5B Q8 | ~500MB | MCU + NPU | Very fast, good for simple routing |
| TinyLlama 1.1B Q4 | ~600MB | Cortex-A | Smallest viable for tool calling |
| Gemma 2 2B Q4 | ~1.5GB | Edge SoC | Strong instruction following |
| Phi-3 Mini 3.8B Q4 | ~2GB | Desktop / Jetson | Good reasoning, CPU-runnable |

## Build

```bash
cd cpp
make          # builds all 7 examples into build/
make clean    # removes build/
```

Requirements: g++ with C++20 support (GCC 10+, Clang 12+).

## Build

```bash
cd cpp
make          # builds all 7 examples into build/
make clean    # removes build/
```

Requirements: g++ with C++20 support (GCC 10+, Clang 12+).

### Resource-Constrained Mode (Static Allocation)

For devices without heap or with limited memory, compile with `-DEMBG_STATIC_ALLOC`:
all `std::*` containers are replaced with fixed-capacity preallocated storage —
no dynamic allocation, no `std::function`, no `std::string` heap.

**The application code is identical in both modes.** The build flag selects the
storage strategy; `embg::Graph<S>` resolves to the right types automatically.

```bash
make STATIC=1          # builds all examples into build_static/
make check             # builds both modes, runs all examples, diffs output — must match
```

Tune capacities in `cpp/include/embg/config.hpp` (`StaticConfig`):

| Config parameter | Default | Controls |
|---|---|---|
| `MaxNodes` | 16 | Max nodes per graph |
| `MaxEdges` | 16 | Max edges per graph |
| `MaxHsmStates` | 16 | Max states in HSM |
| `MaxHsmDepth` | 8 | Max nesting depth (scratch buffer) |
| `MaxHandlers` | 8 | Max event handlers per HSM state |
| `MaxStrLen` | 32 | Node/state/event name length |
| `MaxPromptLen` | 512 | Inference prompt/response length |
| `FnInlineBytes` | 64 | SBO size for router/step lambdas |
| `NodeFnInlineBytes` | 256 | SBO size for node lambdas (inference nodes are larger) |

To use a custom config, pass it as the second template argument:
```cpp
embg::Graph<MyState, MyCustomConfig> g;
```

A lambda capture exceeding the SBO size produces a clear `static_assert` at
compile time telling you to bump `FnInlineBytes` or `NodeFnInlineBytes`.

To enable llama.cpp:
```bash
g++ -std=c++20 -DEMBG_WITH_LLAMACPP -I include -I /path/to/llama.cpp/include \
    examples/06_llm_diagnostic.cpp -o build/06_llm_diagnostic \
    -L /path/to/llama.cpp/build -lllama -lpthread
```

## Reading List

- **LangGraph** — [langchain-ai/langgraph](https://github.com/langchain-ai/langgraph) — the Python inspiration
- **Statecharts** — David Harel (1987), Miro Samek's QP framework
- **Boost.SML** — C++14 state machine DSL
- **llama.cpp** — [ggerganov/llama.cpp](https://github.com/ggerganov/llama.cpp) — on-device LLM inference
- **MISRA C++:2023** — safety-critical C++ coding guidelines
- **Kani** — Rust verification tool (relevant if Option B is revisited)
- **UPPAAL** — model checker for real-time systems

## License

MIT
