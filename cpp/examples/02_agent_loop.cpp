// Example 02: Agent Loop (ReAct pattern)
//
// Demonstrates cycles — the key thing LangGraph adds over LangChain.
// The agent decides whether to call a tool or finish, looping until done.
// This is the core pattern behind all tool-calling agents.
//
// Graph topology:
//
//   agent ──▶ [router] ──▶ tool_execute ──▶ agent  (loop)
//                     └──▶ END                     (when done)
//
// The cycle is what LangChain's SequentialChain cannot express.

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

// Simulated tool registry — in a real system these would call external APIs.
static std::string call_tool(const std::string& name, const std::string& task) {
    if (name == "search")
        return "search result: France has ~68M people";
    if (name == "calculator")
        return "calculator result: 68000000^2 = 4.624e+15";
    return "tool '" + name + "' not found for: " + task;
}

int main() {
    embg::Graph<AgentState> graph;

    graph
        .add_node("agent", [](AgentState& s) {
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
        })

        .add_node("tool_execute", [](AgentState& s) {
            std::cout << "  [tool_execute] calling: " << s.tool_name << "\n";
            std::string result = call_tool(s.tool_name, s.task);
            s.observations.push_back(result);
            std::cout << "  [tool_execute] got: " << result << "\n";
        })

        // The conditional edge on "agent" is what creates the loop.
        // A cyclic graph — impossible to express in a DAG-only framework.
        .add_conditional_edge("agent", [](const AgentState& s) {
            return s.next_action == "finish" ? embg::END : "tool_execute";
        })

        // Tool result feeds back to agent — this is the cycle
        .add_edge("tool_execute", "agent")

        .set_entry("agent")
        .on_step([](std::string_view node, const AgentState&) {
            std::cout << "\n[step → " << node << "]\n";
        });

    AgentState state{ .task = "What is the population of France squared?" };

    std::cout << "=== 02 Agent Loop (ReAct) ===\n";
    std::cout << "Task: " << state.task << "\n";
    graph.run(state, /*max_steps=*/20);
    std::cout << "\nFinal answer: " << state.final_answer << "\n";

    return 0;
}
