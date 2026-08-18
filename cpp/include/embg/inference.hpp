#pragma once

// Inference engine abstraction for embedding LLM/SLM inference into embg::Graph nodes.
//
// Design goals:
//   - Swap between stub (dev/test) and real model (production) without changing node code
//   - Expose confidence as a first-class output → feeds confidence_router in embedded.hpp
//   - Provide a node factory so any InferenceEngine becomes a NodeFn<S>
//
// To enable real llama.cpp: compile with -DEMBG_WITH_LLAMACPP and link against llama.cpp.

#include "graph.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace embg::inference {

// ─── Request / Response ───────────────────────────────────────────────────────
//
// Non-template types that use embg::Config for string storage.
// In default mode → std::string; in static mode → StaticString<MaxPromptLen>.
// Application code writes embg::inference::Request (no template args needed).

using PromptString = std::conditional_t<embg::Config::StaticAlloc,
    StaticString<embg::Config::MaxPromptLen>, std::string>;

struct Request {
    PromptString system_prompt = {};
    PromptString user_prompt   = {};
    int          max_tokens    = 256;
    float        temperature   = 0.1f;
};

struct Response {
    PromptString text        = {};
    double       confidence  = 0.0;
    bool         timed_out   = false;
    int          tokens_used = 0;
};

// ─── Abstract engine interface ────────────────────────────────────────────────

class InferenceEngine {
public:
    virtual ~InferenceEngine() = default;

    virtual Response generate(const Request& req) = 0;
    virtual bool        is_available()            const = 0;
    virtual std::string model_name()              const = 0;
};

// ─── StubEngine — for development and testing ─────────────────────────────────

class StubEngine : public InferenceEngine {
public:
    StubEngine& add_response(const char* keyword,
                             const char* text,
                             double      confidence = 0.90) {
        responses_.push_back({ PromptString(keyword),
                               PromptString(text), confidence });
        return *this;
    }

    StubEngine& set_fallback(const char* text, double confidence = 0.55) {
        fallback_text_       = PromptString(text);
        fallback_confidence_ = confidence;
        return *this;
    }

    Response generate(const Request& req) override {
        for (const auto& entry : responses_) {
            if (req.user_prompt.find(entry.keyword.c_str()) != PromptString::npos) {
                return { entry.text, entry.confidence, false, 10 };
            }
        }
        return { fallback_text_, fallback_confidence_, false, 5 };
    }

    bool        is_available() const override { return true; }
    std::string model_name()   const override { return "stub"; }

private:
    struct Entry { PromptString keyword, text; double confidence; };
    StaticVector<Entry, embg::Config::MaxInferenceResp> responses_;
    PromptString  fallback_text_       = "I am not sure.";
    double        fallback_confidence_ = 0.40;
};

// ─── LlamaCppEngine — real on-device inference ────────────────────────────────

#ifdef EMBG_WITH_LLAMACPP
#include "llama.h"

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
        std::string prompt =
            "system: " + std::string(req.system_prompt) +
            " user: "   + std::string(req.user_prompt);

        std::vector<llama_token> tokens(req.max_tokens + 1024);
        int n_tokens = llama_tokenize(
            model_, prompt.c_str(), static_cast<int>(prompt.size()),
            tokens.data(), static_cast<int>(tokens.size()),
            true, true);
        if (n_tokens < 0) return { "tokenization failed", 0.0, false, 0 };
        tokens.resize(n_tokens);

        llama_kv_cache_clear(ctx_);

        llama_batch batch = llama_batch_get_one(tokens.data(), n_tokens);
        if (llama_decode(ctx_, batch) != 0)
            return { "decode failed", 0.0, false, 0 };

        std::string output;
        double      sum_confidence = 0.0;
        int         n_generated    = 0;
        const llama_token eos      = llama_token_eos(model_);

        while (n_generated < req.max_tokens) {
            float* logits = llama_get_logits_ith(ctx_, -1);
            int    n_vocab = llama_n_vocab(model_);

            llama_token next_token = 0;
            float       best_logit = logits[0];
            for (int i = 1; i < n_vocab; ++i) {
                if (logits[i] > best_logit) {
                    best_logit = logits[i];
                    next_token = static_cast<llama_token>(i);
                }
            }

            if (next_token == eos) break;

            sum_confidence += static_cast<double>(best_logit);
            n_generated++;

            char buf[64];
            int  len = llama_token_to_piece(model_, next_token, buf, sizeof(buf), 0, true);
            if (len > 0) output.append(buf, len);

            llama_batch next_batch = llama_batch_get_one(&next_token, 1);
            if (llama_decode(ctx_, next_batch) != 0) break;
        }

        double confidence = n_generated > 0
            ? std::min(1.0, (sum_confidence / n_generated) / 10.0 + 0.5)
            : 0.0;

        return { PromptString(output), confidence, false, n_generated };
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

template<embg::GraphState S, typename Cfg = embg::Config>
embg::detail::NodeFn<S, Cfg> make_node(
    InferenceEngine&    engine,
    std::conditional_t<Cfg::StaticAlloc,
        embg::Function<Request(const S&), Cfg::FnInlineBytes>,
        std::function<Request(const S&)>>                   build_prompt,
    std::conditional_t<Cfg::StaticAlloc,
        embg::Function<void(S&, const Response&), Cfg::FnInlineBytes>,
        std::function<void(S&, const Response&)>>           apply_response
) {
    return [&engine,
            build_prompt    = std::move(build_prompt),
            apply_response  = std::move(apply_response)](S& state) mutable {

        if (!engine.is_available()) {
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
