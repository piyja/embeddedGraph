#pragma once

// Inference engine abstraction for embedding LLM/SLM inference into embg::Graph nodes.
//
// Design goals:
//   - Swap between stub (dev/test) and real model (production) without changing node code
//   - Expose confidence as a first-class output → feeds confidence_router in embedded.hpp
//   - Provide a node factory so any InferenceEngine becomes a NodeFn<S>
//
// Usage pattern:
//   1. Define how to build a prompt from your state
//   2. Define how to apply the response back to state
//   3. Pick an engine: StubEngine for dev, LlamaCppEngine for production
//   4. Call make_inference_node() to get a NodeFn<S> to plug into your graph
//
// To enable real llama.cpp: compile with -DEMBG_WITH_LLAMACPP and link against llama.cpp.
// The LlamaCppEngine skeleton below documents what to implement.

#include "graph.hpp"
#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace embg::inference {

// ─── Request / Response ───────────────────────────────────────────────────────

struct Request {
    std::string system_prompt = {};
    std::string user_prompt   = {};
    int         max_tokens    = 256;
    float       temperature   = 0.1f;  // low temp → deterministic, good for embedded
};

struct Response {
    std::string text        = {};
    double      confidence  = 0.0;  // derived from token logits or heuristic
    bool        timed_out   = false;
    int         tokens_used = 0;
};

// ─── Abstract engine interface ────────────────────────────────────────────────

class InferenceEngine {
public:
    virtual ~InferenceEngine() = default;

    virtual Response    generate(const Request& req)    = 0;
    virtual bool        is_available()            const = 0;
    virtual std::string model_name()              const = 0;
};

// ─── StubEngine — for development and testing ─────────────────────────────────
//
// Returns canned responses from a registry. Useful for:
//   - Development without a real model
//   - Unit testing graph topology
//   - CI pipelines where model inference is too slow

class StubEngine : public InferenceEngine {
public:
    // Register a canned response for a keyword in the user prompt.
    // First match wins. Fallback is used when no keyword matches.
    StubEngine& add_response(std::string keyword,
                             std::string text,
                             double      confidence = 0.90) {
        responses_.push_back({ std::move(keyword), std::move(text), confidence });
        return *this;
    }

    StubEngine& set_fallback(std::string text, double confidence = 0.55) {
        fallback_text_       = std::move(text);
        fallback_confidence_ = confidence;
        return *this;
    }

    Response generate(const Request& req) override {
        for (const auto& entry : responses_) {
            if (req.user_prompt.find(entry.keyword) != std::string::npos) {
                return { entry.text, entry.confidence, false, 10 };
            }
        }
        return { fallback_text_, fallback_confidence_, false, 5 };
    }

    bool        is_available() const override { return true; }
    std::string model_name()   const override { return "stub"; }

private:
    struct Entry { std::string keyword, text; double confidence; };
    std::vector<Entry> responses_;
    std::string        fallback_text_       = "I am not sure.";
    double             fallback_confidence_ = 0.40;
};

// ─── LlamaCppEngine — real on-device inference ────────────────────────────────
//
// Skeleton. Compile with -DEMBG_WITH_LLAMACPP and link against llama.cpp to activate.
// Tested structure against llama.cpp master (~2025). Adapt if API has changed.
//
// To build llama.cpp:
//   git clone https://github.com/ggerganov/llama.cpp
//   cd llama.cpp && cmake -B build && cmake --build build
//   # Download a GGUF model: e.g. Phi-3 Mini, Gemma 2B, Qwen2.5 0.5B
//
// Compile this project with (all on one line):
//   g++ -std=c++20 -DEMBG_WITH_LLAMACPP -I include -I /path/to/llama.cpp/include
//       examples/06_llm_diagnostic.cpp -o build/06_llm_diagnostic
//       -L /path/to/llama.cpp/build -lllama -lpthread

#ifdef EMBG_WITH_LLAMACPP
#include "llama.h"  // from llama.cpp — must be on include path

class LlamaCppEngine : public InferenceEngine {
public:
    explicit LlamaCppEngine(const std::string& model_path,
                            int                n_ctx    = 2048,
                            int                n_threads = 4) {
        llama_backend_init();

        llama_model_params mparams = llama_model_default_params();
        model_ = llama_load_model_from_file(model_path.c_str(), mparams);
        if (!model_) throw std::runtime_error("LlamaCppEngine: failed to load model");

        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx     = n_ctx;
        cparams.n_threads = n_threads;
        ctx_ = llama_new_context_with_model(model_, cparams);
        if (!ctx_) throw std::runtime_error("LlamaCppEngine: failed to create context");

        model_path_ = model_path;
    }

    ~LlamaCppEngine() {
        if (ctx_)   llama_free(ctx_);
        if (model_) llama_free_model(model_);
        llama_backend_free();
    }

    Response generate(const Request& req) override {
        // Build prompt — adapt the template to your model's chat format
        // Phi-3: <|system|>{system}<|end|><|user|>{user}<|end|><|assistant|>
        // Llama-3: <|begin_of_text|><|start_header_id|>system<|end_header_id|>...
        // Gemma-2: <start_of_turn>user\n{user}<end_of_turn>\n<start_of_turn>model
        std::string prompt =
            "<|system|>" + req.system_prompt + "<|end|>"
            "<|user|>"   + req.user_prompt   + "<|end|>"
            "<|assistant|>";

        // Tokenise
        std::vector<llama_token> tokens(req.max_tokens + 1024);
        int n_tokens = llama_tokenize(
            model_, prompt.c_str(), static_cast<int>(prompt.size()),
            tokens.data(), static_cast<int>(tokens.size()),
            /*add_special=*/true, /*parse_special=*/true);
        if (n_tokens < 0) return { "tokenization failed", 0.0, false, 0 };
        tokens.resize(n_tokens);

        llama_kv_cache_clear(ctx_);

        // Decode input tokens in one batch
        llama_batch batch = llama_batch_get_one(tokens.data(), n_tokens);
        if (llama_decode(ctx_, batch) != 0)
            return { "decode failed", 0.0, false, 0 };

        // Greedy sampling loop
        std::string output;
        double      max_logit      = 0.0;
        double      sum_confidence = 0.0;
        int         n_generated    = 0;
        const llama_token eos      = llama_token_eos(model_);

        while (n_generated < req.max_tokens) {
            float* logits = llama_get_logits_ith(ctx_, -1);
            int    n_vocab = llama_n_vocab(model_);

            // Greedy: pick highest logit token
            llama_token next_token = 0;
            float       best_logit = logits[0];
            for (int i = 1; i < n_vocab; ++i) {
                if (logits[i] > best_logit) {
                    best_logit = logits[i];
                    next_token = static_cast<llama_token>(i);
                }
            }

            if (next_token == eos) break;

            // Accumulate confidence as softmax of top logit (simplified)
            // A production system would use proper token probabilities here
            max_logit       = best_logit;
            sum_confidence += static_cast<double>(best_logit);
            n_generated++;

            // Decode token to string
            char buf[64];
            int  len = llama_token_to_piece(model_, next_token, buf, sizeof(buf), 0, true);
            if (len > 0) output.append(buf, len);

            // Feed the generated token back
            llama_batch next_batch = llama_batch_get_one(&next_token, 1);
            if (llama_decode(ctx_, next_batch) != 0) break;
        }

        // Normalise confidence to [0, 1] — heuristic, replace with proper logprob
        double confidence = n_generated > 0
            ? std::min(1.0, (sum_confidence / n_generated) / 10.0 + 0.5)
            : 0.0;

        return { output, confidence, false, n_generated };
    }

    bool        is_available() const override { return model_ != nullptr && ctx_ != nullptr; }
    std::string model_name()   const override { return model_path_; }

private:
    llama_model*   model_      = nullptr;
    llama_context* ctx_        = nullptr;
    std::string    model_path_;
};

#endif  // EMBG_WITH_LLAMACPP

// ─── InferenceNode factory ────────────────────────────────────────────────────
//
// Wraps an InferenceEngine into a NodeFn<S> that fits directly into embg::Graph.
//
// The two lambdas decouple the engine from your state type:
//   build_prompt:    (const S&) → Request      — read state, build the prompt
//   apply_response:  (S&, Response) → void     — write inference result to state
//
// Example:
//   auto node = embg::inference::make_node<DiagnosticState>(
//       engine,
//       [](const DiagnosticState& s) -> embg::inference::Request {
//           return { .system_prompt = "You are an automotive diagnostic assistant.",
//                    .user_prompt   = "Evidence: " + join(s.observations) };
//       },
//       [](DiagnosticState& s, const embg::inference::Response& r) {
//           s.last_confidence  = r.confidence;
//           s.fault_description = r.text;
//       });

template<embg::GraphState S>
embg::NodeFn<S> make_node(
    InferenceEngine&                                       engine,
    std::function<Request(const S&)>                       build_prompt,
    std::function<void(S&, const Response&)>               apply_response
) {
    return [&engine,
            build_prompt    = std::move(build_prompt),
            apply_response  = std::move(apply_response)](S& state) {

        if (!engine.is_available()) {
            // Engine offline — write zero confidence so the graph falls back
            Response offline{ "engine unavailable", 0.0, false, 0 };
            apply_response(state, offline);
            return;
        }

        Request  req = build_prompt(state);
        Response res = engine.generate(req);
        apply_response(state, res);
    };
}

} // namespace embg::inference
