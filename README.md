# embeddedGraph

**Embedded AI Orchestration — C++20, Zero-Dependency Core**

A deterministic orchestration framework for AI agents on embedded/edge targets. Combines a statechart-based Hierarchical State Machine (HSM) with a LangGraph-inspired execution graph, providing the behavioral contract that safety-critical embedded systems require around stochastic AI inference.

```
┌─────────────────────────────────────────────────────────────┐
│  HSM  (embg::hsm)                                           │
│  Deterministic behavioral contract                          │
│  OPERATING ──▶ DEGRADED ──▶ SAFE_HALT                       │
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

LangGraph (Python) popularized orchestration graphs for LLM agents — but it has no embedded story, no state machines, no safety primitives, no deterministic fallback. embeddedGraph fills that gap:

| Concern | LangGraph | embeddedGraph |
|---|---|---|
| Language | Python | C++20 |
| Target | Cloud / desktop | MCU / SoC / RTOS |
| Deterministic shell | None | HSM (UML 2.0 statecharts) |
| Confidence gating | None | `confidence_router` primitive |
| Degraded mode | None | `DegradedModeRunner` |
| Timeout enforcement | None | `with_timeout` wrapper |
| Human-in-the-loop | `interrupt()` | `set_interrupt()` |
| Dependencies | langchain, pydantic, etc. | stdlib only |

## Architecture — Two-Layer Pattern

The key insight: **the HSM is the behavioral contract, the Graph is the agentic loop that runs inside a state.**

- **HSM** — manages coarse system states (OPERATING / DEGRADED / SAFE_HALT). Handles events like `fault_detected`, `ai_unavailable`, `critical_fault`. Transitions are deterministic, verifiable, and follow UML 2.0 LCA semantics.
- **Graph** — runs *within* an HSM state. Handles the stochastic part: tool calling, LLM inference, evidence accumulation. Routes via confidence gates.

When the HSM enters SAFE_HALT, all graph execution stops. When it enters DEGRADED, only rule-based graphs run. This is the pattern that makes embedded AI certifiable.

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

## Project Structure

```
embeddedGraph/
├── cpp/
│   ├── include/embg/
│   │   ├── graph.hpp          # Core execution graph
│   │   ├── hsm.hpp            # Hierarchical state machine
│   │   ├── embedded.hpp       # Confidence router, timeout, degraded mode
│   │   └── inference.hpp      # LLM engine abstraction + llama.cpp integration
│   ├── examples/
│   │   ├── 01_simple_chain.cpp           # Linear pipeline + conditional branch
│   │   ├── 02_agent_loop.cpp             # ReAct pattern (cycles)
│   │   ├── 03_embedded_sensor.cpp        # Confidence-gated routing
│   │   ├── 04_automotive_diagnostic.cpp  # Full agent: tools + inference + degraded mode
│   │   ├── 05_hsm_ecu_states.cpp         # HSM managing ECU system states
│   │   ├── 06_llm_diagnostic.cpp         # LLM inference node (StubEngine)
│   │   └── 07_llm_brain_agent.cpp        # LLM as orchestration brain
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
LLM decides *which* tool to call next, not the programmer. The model IS the orchestrator. Confidence gate overrides low-confidence "finish" decisions.

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
| **Phase 3** | Replace heap allocation with static pools / `std::array` | Planned |
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
