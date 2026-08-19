// Example 01: Simple Chain
//
// Demonstrates a linear pipeline with a conditional branch — the simplest
// pattern in LangGraph. Equivalent to a LangChain SequentialChain but with
// explicit routing instead of implicit ordering.
//
// Graph topology:
//
//   preprocess ──▶ classify ──[router on s.category]──▶ respond_* ──▶ END
//
// Each stage's responsibility:
//   preprocess — lowercases the input text
//   classify   — inspects the text for technical keywords, sets s.category
//   router     — conditional edge: routes to respond_technical or
//                respond_general based on s.category
//   respond_*  — formats a category-appropriate response

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

// ─── Node implementations ─────────────────────────────────────────────────────
// Free functions so the graph builder reads as pure topology — the "what each
// stage does" story is separated from the "how are they wired" story.

static void preprocess_node(PipelineState& s) {
    s.cleaned = s.input;
    std::transform(s.cleaned.begin(), s.cleaned.end(),
                   s.cleaned.begin(), [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
}

static void classify_node(PipelineState& s) {
    // Simulates an LLM classification call.
    const bool is_technical =
        s.cleaned.find("error")   != std::string::npos ||
        s.cleaned.find("crash")   != std::string::npos ||
        s.cleaned.find("compile") != std::string::npos ||
        s.cleaned.find("segfault")!= std::string::npos;

    s.category = is_technical ? "technical" : "general";
}

static void respond_technical_node(PipelineState& s) {
    s.response = "[TECHNICAL] Looks like a code issue in: \"" + s.cleaned + "\"";
}

static void respond_general_node(PipelineState& s) {
    s.response = "[GENERAL] Here is a general answer for: \"" + s.cleaned + "\"";
}

// ─── Build the pipeline graph ─────────────────────────────────────────────────
//
// The builder is intentionally declarative: Nodes, Edges, Router, and Entry
// are each their own labeled section so the topology can be read at a glance.

static embg::Graph<PipelineState> make_pipeline_graph() {
    embg::Graph<PipelineState> g;

    // ── Nodes ─────────────────────────────────────────────────────────────────
    g.add_node("preprocess",         preprocess_node);
    g.add_node("classify",           classify_node);
    g.add_node("respond_technical",  respond_technical_node);
    g.add_node("respond_general",    respond_general_node);

    // ── Edges (declared together so the topology is visible at a glance) ────
    //
    //   preprocess → classify → [router] → respond_* → END
    g.add_edge("preprocess",        "classify");
    g.add_edge("respond_technical", embg::END);
    g.add_edge("respond_general",   embg::END);

    // ── Router: classify → responder matching s.category ─────────────────────
    g.add_conditional_edge("classify", [](const PipelineState& s) {
        return s.category == "technical" ? "respond_technical" : "respond_general";
    });

    // ── Entry + streaming ─────────────────────────────────────────────────────
    g.set_entry("preprocess");
    g.on_step([](std::string_view node, const PipelineState& s) {
        std::cout << "  [step] " << node;
        if (!s.category.empty()) std::cout << "  (category=" << s.category << ")";
        std::cout << "\n";
    });

    return g;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    auto graph = make_pipeline_graph();

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
