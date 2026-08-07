// Example 01: Simple Chain
//
// Demonstrates a linear pipeline with a conditional branch — the simplest
// pattern in LangGraph. Equivalent to a LangChain SequentialChain but with
// explicit routing instead of implicit ordering.
//
// Graph topology:
//
//   preprocess ──▶ classify ──▶ [router] ──▶ respond_technical ──▶ END
//                                       └──▶ respond_general   ──▶ END

#include <embg/graph.hpp>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

struct PipelineState {
    std::string input    = {};
    std::string cleaned  = {};
    std::string category = {};  // "technical" | "general"
    std::string response = {};
};

int main() {
    embg::Graph<PipelineState> graph;

    graph
        .add_node("preprocess", [](PipelineState& s) {
            s.cleaned = s.input;
            std::transform(s.cleaned.begin(), s.cleaned.end(),
                           s.cleaned.begin(), [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
        })
        .add_node("classify", [](PipelineState& s) {
            // Simulates an LLM classification call
            const bool is_technical =
                s.cleaned.find("error")   != std::string::npos ||
                s.cleaned.find("crash")   != std::string::npos ||
                s.cleaned.find("compile") != std::string::npos ||
                s.cleaned.find("segfault")!= std::string::npos;

            s.category = is_technical ? "technical" : "general";
        })
        .add_node("respond_technical", [](PipelineState& s) {
            s.response = "[TECHNICAL] Looks like a code issue in: \"" + s.cleaned + "\"";
        })
        .add_node("respond_general", [](PipelineState& s) {
            s.response = "[GENERAL] Here is a general answer for: \"" + s.cleaned + "\"";
        })

        // Linear edges
        .add_edge("preprocess", "classify")

        // Conditional edge — router reads state, returns next node name
        .add_conditional_edge("classify", [](const PipelineState& s) {
            return s.category == "technical" ? "respond_technical" : "respond_general";
        })

        .add_edge("respond_technical", embg::END)
        .add_edge("respond_general",   embg::END)

        .set_entry("preprocess")

        // Step callback = LangGraph event streaming
        .on_step([](std::string_view node, const PipelineState& s) {
            std::cout << "  [step] " << node;
            if (!s.category.empty()) std::cout << "  (category=" << s.category << ")";
            std::cout << "\n";
        });

    // ── Run ──────────────────────────────────────────────────────────────────

    auto run = [&](const std::string& input) {
        PipelineState state{ .input = input };
        std::cout << "\nInput: \"" << input << "\"\n";
        graph.run(state);
        std::cout << "Output: " << state.response << "\n";
    };

    std::cout << "=== 01 Simple Chain ===\n";
    run("I keep getting a segfault in my C++ code");
    run("What is the capital of France?");

    return 0;
}
