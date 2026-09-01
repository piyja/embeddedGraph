#pragma once

// Shared pipeline state — flows through every node of the voice graph.
//
// Each stage reads only the fields produced by upstream stages:
//   asr  → writes transcript, asr_confidence, asr_latency_us
//   nlu  → writes intent, nlu_confidence, matched_keyword
//   reply_* → writes reply
//   tts  → writes tts_text, tts_ready

#include <string>

namespace voice {

struct VoiceState {
    // Raw input (what would be audio in a real system)
    std::string raw_input;

    // ── ASR stage output ──
    std::string transcript;
    double      asr_confidence = 0.0;
    long        asr_latency_us = 0;

    // ── NLU stage output ──
    std::string intent;
    double      nlu_confidence = 0.0;
    std::string matched_keyword;

    // ── Dialogue stage output ──
    std::string reply;

    // ── TTS stage output ──
    std::string tts_text;
    bool        tts_ready = false;
};

} // namespace voice
