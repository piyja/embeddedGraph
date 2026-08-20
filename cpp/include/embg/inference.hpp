#pragma once

// Inference engine abstraction for embedding LLM/SLM inference into embg::Graph nodes.
//
// Design goals:
//   - Swap between stub (dev/test) and real model (production) without changing node code
//   - Expose confidence as a first-class output → feeds confidence_router in embedded.hpp
//   - Provide a node factory so any InferenceEngine becomes a NodeFn<S>
//   - Config-aware: prompt storage follows the graph's Cfg, not a hardcoded global
//
// To enable real llama.cpp: compile with -DEMBG_WITH_LLAMACPP and link against llama.cpp.
//
// Config-awareness (fixes 2.1):
//   RequestT<Cfg>, ResponseT<Cfg>, InferenceEngineT<Cfg>, StubEngineT<Cfg> are
//   templated on Cfg. Default-config aliases (Request, Response, InferenceEngine,
//   StubEngine) are provided for backward compatibility — existing code unchanged.
//   For custom configs: embg::inference::make_node<S, MyCfg>(engine, ...) expects
//   InferenceEngineT<MyCfg>&.

#include "graph.hpp"
#include "error.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace embg::inference {

// ─── Config-conditional prompt string ──────────────────────────────────────────

template<typename Cfg = embg::Config>
using PromptStringT = std::conditional_t<Cfg::StaticAlloc,
    StaticString<Cfg::MaxPromptLen>, std::string>;

// ─── Request / Response (config-aware) ─────────────────────────────────────────
//
// Template on Cfg so prompt storage follows the graph's config.
// Default-config aliases provided for backward compatibility.

template<typename Cfg = embg::Config>
struct RequestT {
    PromptStringT<Cfg> system_prompt = {};
    PromptStringT<Cfg> user_prompt   = {};
    int                max_tokens    = 256;
    float              temperature   = 0.1f;
};

template<typename Cfg = embg::Config>
struct ResponseT {
    PromptStringT<Cfg> text        = {};
    double             confidence  = 0.0;
    bool               timed_out   = false;
    int                tokens_used = 0;
};

// Backward-compatible aliases (default config)
using Request  = RequestT<>;
using Response = ResponseT<>;

// ─── Abstract engine interface (config-aware) ──────────────────────────────────

template<typename Cfg = embg::Config>
class InferenceEngineT {
public:
    virtual ~InferenceEngineT() = default;

    virtual ResponseT<Cfg> generate(const RequestT<Cfg>& req) = 0;
    virtual bool           is_available()              const = 0;
    virtual std::string    model_name()                const = 0;
};

// Backward-compatible alias
using InferenceEngine = InferenceEngineT<>;

// ─── StubEngine — for development and testing (config-aware) ───────────────────

template<typename Cfg = embg::Config>
class StubEngineT : public InferenceEngineT<Cfg> {
public:
    using PStr = PromptStringT<Cfg>;

    StubEngineT& add_response(const char* keyword,
                               const char* text,
                               double      confidence = 0.90) {
        responses_.push_back({ PStr(keyword), PStr(text), confidence });
        return *this;
    }

    StubEngineT& set_fallback(const char* text, double confidence = 0.55) {
        fallback_text_       = PStr(text);
        fallback_confidence_ = confidence;
        return *this;
    }

    ResponseT<Cfg> generate(const RequestT<Cfg>& req) override {
        for (const auto& entry : responses_) {
            if (req.user_prompt.find(entry.keyword.c_str()) != PStr::npos) {
                return { entry.text, entry.confidence, false, 10 };
            }
        }
        return { fallback_text_, fallback_confidence_, false, 5 };
    }

    bool        is_available() const override { return true; }
    std::string model_name()   const override { return "stub"; }

private:
    struct Entry { PStr keyword, text; double confidence; };
    StaticVector<Entry, Cfg::MaxInferenceResp> responses_;
    PStr   fallback_text_       = "I am not sure.";
    double fallback_confidence_ = 0.40;
};

// Backward-compatible alias
using StubEngine = StubEngineT<>;

// ─── LlamaCppEngine — real on-device inference (config-aware) ──────────────────

#ifdef EMBG_WITH_LLAMACPP
#include "llama.h"

template<typename Cfg = embg::Config>
class LlamaCppEngineT : public InferenceEngineT<Cfg> {
public:
    using PStr = PromptStringT<Cfg>;

    explicit LlamaCppEngineT(const std::string& model_path,
                             int                n_ctx    = 2048,
                             int                n_threads = 4) {
        llama_backend_init();

        llama_model_params mparams = llama_model_default_params();
        model_ = llama_load_model_from_file(model_path.c_str(), mparams);
        if (!model_) EMBG_ERROR(EngineUnavailable, "LlamaCppEngine: failed to load model");

        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx     = n_ctx;
        cparams.n_threads = n_threads;
        ctx_ = llama_new_context_with_model(model_, cparams);
        if (!ctx_) EMBG_ERROR(EngineUnavailable, "LlamaCppEngine: failed to create context");

        model_path_ = model_path;
    }

    ~LlamaCppEngineT() {
        if (ctx_)   llama_free(ctx_);
        if (model_) llama_free_model(model_);
        llama_backend_free();
    }

    ResponseT<Cfg> generate(const RequestT<Cfg>& req) override {
        std::string prompt =
            "system: " + std::string(req.system_prompt) +
            " user: "   + std::string(req.user_prompt);

        std::vector<llama_token> tokens(req.max_tokens + 1024);
        int n_tokens = llama_tokenize(
            model_, prompt.c_str(), static_cast<int>(prompt.size()),
            tokens.data(), static_cast<int>(tokens.size()),
            true, true);
        if (n_tokens < 0) return { PStr("tokenization failed"), 0.0, false, 0 };
        tokens.resize(n_tokens);

        llama_kv_cache_clear(ctx_);

        llama_batch batch = llama_batch_get_one(tokens.data(), n_tokens);
        if (llama_decode(ctx_, batch) != 0)
            return { PStr("decode failed"), 0.0, false, 0 };

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

        return { PStr(output), confidence, false, n_generated };
    }

    bool        is_available() const override { return model_ != nullptr && ctx_ != nullptr; }
    std::string model_name()   const override { return model_path_; }

private:
    llama_model*   model_      = nullptr;
    llama_context* ctx_        = nullptr;
    std::string    model_path_;
};

// Backward-compatible alias
using LlamaCppEngine = LlamaCppEngineT<>;

#endif  // EMBG_WITH_LLAMACPP

// ─── InferenceNode factory ────────────────────────────────────────────────────
//
// make_node: captures engine by raw reference. Lifetime contract:
//   The engine MUST outlive the graph. For automatic lifetime management,
//   use make_node_shared() with std::shared_ptr<InferenceEngineT<Cfg>>.
//
// Config-aware: Request/Response use Cfg's prompt storage, matching the graph.

template<embg::GraphState S, typename Cfg = embg::Config>
embg::detail::NodeFn<S, Cfg> make_node(
    InferenceEngineT<Cfg>& engine,
    std::conditional_t<Cfg::StaticAlloc,
        embg::Function<RequestT<Cfg>(const S&), Cfg::FnInlineBytes>,
        std::function<RequestT<Cfg>(const S&)>>                   build_prompt,
    std::conditional_t<Cfg::StaticAlloc,
        embg::Function<void(S&, const ResponseT<Cfg>&), Cfg::FnInlineBytes>,
        std::function<void(S&, const ResponseT<Cfg>&)>>           apply_response
) {
    return [&engine,
            build_prompt    = std::move(build_prompt),
            apply_response  = std::move(apply_response)](S& state) mutable {

        if (!engine.is_available()) {
            ResponseT<Cfg> offline{ "engine unavailable", 0.0, false, 0 };
            apply_response(state, offline);
            return;
        }

        RequestT<Cfg>  req = build_prompt(state);
        ResponseT<Cfg> res = engine.generate(req);
        apply_response(state, res);
    };
}

#if !defined(EMBG_STATIC_ALLOC)
// make_node_shared: captures engine via shared_ptr — no dangling risk.
// The graph keeps the engine alive for its full lifetime.
template<embg::GraphState S, typename Cfg = embg::Config>
embg::detail::NodeFn<S, Cfg> make_node_shared(
    std::shared_ptr<InferenceEngineT<Cfg>> engine,
    std::function<RequestT<Cfg>(const S&)>                   build_prompt,
    std::function<void(S&, const ResponseT<Cfg>&)>           apply_response
) {
    return [engine,
            build_prompt    = std::move(build_prompt),
            apply_response  = std::move(apply_response)](S& state) mutable {

        if (!engine->is_available()) {
            ResponseT<Cfg> offline{ "engine unavailable", 0.0, false, 0 };
            apply_response(state, offline);
            return;
        }

        RequestT<Cfg>  req = build_prompt(state);
        ResponseT<Cfg> res = engine->generate(req);
        apply_response(state, res);
    };
}
#endif

} // namespace embg::inference
