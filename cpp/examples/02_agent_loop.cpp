// Example 02: Agent Loop (ReAct pattern)
//
// Demonstrates cycles — the key thing LangGraph adds over LangChain.
// The agent decides whether to call a tool or finish, looping until done.
// This is the core pattern behind all tool-calling agents.
//
// Graph topology:
//
//   agent ──[router]──▶ tool_execute ──▶ agent  (loop)
//             │
//             └──▶ END                     (when done)
//
// The cycle is what LangChain's SequentialChain cannot express.
//
// Each stage's responsibility:
//   agent         — ReAct reasoning: decides which tool to call or to finish
//   tool_execute  — dispatches to the tool registry, appends result to evidence
//   router        — conditional edge: finish → END, otherwise → tool_execute

#include <embg/graph.hpp>
#include <iostream>
#include <string>
#include <vector>

struct AgentState {
    std::string              task         = {};
    std::string              next_action  = {};  // "use_tool" | "finish"
    std::string              tool_name    = {};  // which tool to call this step
    std::vector<std::string> observations = {};  // accumulated tool results
    std::string              final_answer = {};
    int                      step         = 0;
};

// ─── Simulated tool registry ──────────────────────────────────────────────────
// In a real system these would call external APIs.

static std::string call_tool(const std::string& name, const std::string& task) {
    if (name == "search")
        return "search result: France has ~68M people";
    if (name == "calculator")
        return "calculator result: 68000000^2 = 4.624e+15";
    return "tool '" + name + "' not found for: " + task;
}

// ─── Node implementations ─────────────────────────────────────────────────────
// Free functions so the graph builder reads as pure topology.

static void agent_node(AgentState& s) {
    s.step++;
    std::cout << "  [agent] step=" << s.step
              << "  observations=" << s.observations.size() << "\n";

    // Simulated LLM reasoning (ReAct: Reason + Act)
    if (s.step == 1) {
        s.next_action = "use_tool";
        s.tool_name   = "search";
        std::cout << "  [agent] thought: I need to look up the population first\n";
    } else if (s.step == 2) {
        s.next_action = "use_tool";
        s.tool_name   = "calculator";
        std::cout << "  [agent] thought: now I can square the number\n";
    } else {
        s.next_action = "finish";
        s.final_answer = s.observations.empty()
            ? "No data collected."
            : "Answer: " + s.observations.back();
        std::cout << "  [agent] thought: I have enough information to answer\n";
    }
}

static void tool_execute_node(AgentState& s) {
    std::cout << "  [tool_execute] calling: " << s.tool_name << "\n";
    std::string result = call_tool(s.tool_name, s.task);
    s.observations.push_back(result);
    std::cout << "  [tool_execute] got: " << result << "\n";
}

// ─── Build the agent loop graph ───────────────────────────────────────────────
//
// The builder is intentionally declarative: Nodes, Edges, Router, and Entry
// are each their own labeled section so the topology can be read at a glance.
// The cycle (tool_execute → agent) is what creates the ReAct loop.

static embg::Graph<AgentState> make_agent_loop_graph() {
    embg::Graph<AgentState> g;

    // ── Nodes ─────────────────────────────────────────────────────────────────
    g.add_node("agent",        agent_node);
    g.add_node("tool_execute", tool_execute_node);

    // ── Edges (declared together so the topology is visible at a glance) ────
    //
    //   agent ──[router]──▶ tool_execute ──▶ agent  (loop)
    //             └──▶ END
    //
    // The back-edge tool_execute → agent is what creates the cycle —
    // impossible to express in a DAG-only framework.
    g.add_edge("tool_execute", "agent");

    // ── Router: agent → {tool_execute | END} ─────────────────────────────────
    // The conditional edge on "agent" is what creates the loop.
    g.add_conditional_edge("agent", [](const AgentState& s) {
        return s.next_action == "finish" ? embg::END : "tool_execute";
    });

    // ── Entry + streaming ─────────────────────────────────────────────────────
    g.set_entry("agent");
    g.on_step([](std::string_view node, const AgentState&) {
        std::cout << "\n[step → " << node << "]\n";
    });

    return g;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    auto graph = make_agent_loop_graph();

    AgentState state{ .task = "What is the population of France squared?" };

    std::cout << "=== 02 Agent Loop (ReAct) ===\n";
    std::cout << "Task: " << state.task << "\n";
    graph.run(state, /*max_steps=*/20);
    std::cout << "\nFinal answer: " << state.final_answer << "\n";

    return 0;
}
