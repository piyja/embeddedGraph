#pragma once

// TTS (Text-To-Speech) module.
//
// Server side: normalizes the reply into a speech-friendly string and marks
// it ready. In an embedded deployment this stage would drive a hardware TTS
// engine (piper, festival-lite) or stream audio to a speaker driver.
//
// Demo deployment: the browser performs actual audio synthesis via the Web
// Speech API (speechSynthesis) using `tts_text`. The module boundary stays
// honest — the graph produces a finished, speakable utterance either way.

#include "state.hpp"

#include <string>

namespace voice {

struct TtsResult {
    std::string tts_text;
    bool        ready = false;
};

class Tts {
public:
    TtsResult prepare(const std::string& reply) const {
        TtsResult out;
        out.tts_text = expand_abbreviations(reply);
        out.ready    = !out.tts_text.empty();
        return out;
    }

private:
    // Expand common written forms so spoken output sounds natural.
    static std::string expand_abbreviations(std::string text) {
        replace_all(text, " degrees ",  " degrees ");
        replace_all(text, "I'm",   "I am");
        replace_all(text, "don't", "do not");
        replace_all(text, "can't", "cannot");
        replace_all(text, "&",     " and ");
        return text;
    }

    static void replace_all(std::string& s,
                            const std::string& from,
                            const std::string& to) {
        if (from.empty()) return;
        std::size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
};

} // namespace voice
