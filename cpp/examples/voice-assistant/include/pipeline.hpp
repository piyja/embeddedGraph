#pragma once

// Voice assistant pipeline — wires ASR → NLU → router → dialogue → TTS
// into an embg::Graph.
//
// Topology:
//
//   START ──▶ asr ──▶ nlu ──[intent router]──▶ reply_<intent> ──▶ tts ──▶ END
//
// The reply nodes are not hand-wired: every skill in the SkillRegistry
// (skills.hpp) becomes a "reply_<intent>" node converging on tts, and the
// same skills also feed the NLU keyword rules. Adding a capability is one
// add() call in make_default_skills().
//
// Each stage is a separate module (asr.hpp, nlu.hpp, dialogue.hpp, tts.hpp);
// this file is pure topology — the graph reads like the block diagram above.

#include <embg/graph.hpp>

#include "asr.hpp"
#include "nlu.hpp"
#include "skills.hpp"
#include "state.hpp"
#include "tts.hpp"

namespace voice {

class Pipeline {
public:
    explicit Pipeline(SkillRegistry skills = make_default_skills())
        : nlu_(make_default_nlu(skills)) {
        using embg::END;

        // ── Nodes: one per pipeline stage ──────────────────────────────────
        g_.add_node("asr", [this](VoiceState& s) {
            const AsrResult r = asr_.transcribe(s.raw_input);
            s.transcript      = r.transcript;
            s.asr_confidence  = r.confidence;
            s.asr_latency_us  = r.latency_us;
        });

        g_.add_node("nlu", [this](VoiceState& s) {
            const NluResult r = nlu_.classify(s.transcript);
            s.intent          = r.intent;
            s.nlu_confidence  = r.confidence;
            s.matched_keyword = r.matched_keyword;
        });

        // One reply node per registered skill — discovered at init.
        for (const auto& skill : skills) {
            g_.add_node("reply_" + skill.intent, skill.handler);
        }

        g_.add_node("tts", [this](VoiceState& s) {
            const TtsResult r = tts_.prepare(s.reply);
            s.tts_text  = r.tts_text;
            s.tts_ready = r.ready;
        });

        // ── Edges ──────────────────────────────────────────────────────────
        g_.add_edge("asr", "nlu");

        // Intent router — conditional edge dispatching to one reply node.
        g_.add_conditional_edge("nlu", [](const VoiceState& s) -> std::string {
            return "reply_" + s.intent;
        });

        // All reply nodes converge on tts.
        for (const auto& skill : skills) {
            g_.add_edge("reply_" + skill.intent, "tts");
        }
        g_.add_edge("tts", END);

        g_.set_entry("asr");
    }

    // Run one utterance through the full pipeline and return the final state
    // (contains every stage's output for UI display).
    VoiceState process(std::string input) {
        VoiceState s;
        s.raw_input = std::move(input);
        g_.run(s);
        return s;
    }

private:
    embg::Graph<VoiceState> g_;
    Asr                     asr_;
    Nlu                     nlu_;
    Tts                     tts_;
};

} // namespace voice
