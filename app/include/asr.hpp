#pragma once

// ASR (Automatic Speech Recognition) module.
//
// Simulated speech-to-text stage. In a real deployment this would consume an
// audio stream from a microphone driver and run an on-device ASR model
// (whisper.cpp, vosk, etc.). For this demo the "audio" is already text —
// the user types instead of speaks — so the stage is a passthrough that:
//   1. normalizes whitespace
//   2. simulates a recognition confidence score
//   3. measures processing latency
//
// The stage boundary is real: downstream code only sees `transcript`,
// never `raw_input`. Swapping in a real ASR engine means replacing this
// module's implementation without touching NLU/dialogue/TTS or the graph.

#include <algorithm>
#include <chrono>
#include <string>

namespace voice {

struct AsrResult {
    std::string transcript;
    double      confidence = 0.0;
    long        latency_us = 0;
};

class Asr {
public:
    // Process raw input ("audio") into a transcript.
    AsrResult transcribe(const std::string& audio) const {
        const auto start = std::chrono::steady_clock::now();

        AsrResult out;
        out.transcript = normalize(audio);
        out.confidence = simulate_confidence(out.transcript);

        const auto end = std::chrono::steady_clock::now();
        out.latency_us =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        return out;
    }

private:
    // Trim + collapse internal whitespace — mimics ASR post-processing.
    static std::string normalize(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        bool last_space = true;  // also strips leading spaces
        for (char c : in) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!last_space) { out.push_back(' '); last_space = true; }
            } else {
                out.push_back(c);
                last_space = false;
            }
        }
        if (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    }

    // Toy confidence model: longer, cleaner utterances score higher.
    static double simulate_confidence(const std::string& t) {
        if (t.empty()) return 0.0;
        const std::size_t letters =
            std::count_if(t.begin(), t.end(), [](unsigned char c) {
                return std::isalpha(c) || std::isdigit(c);
            });
        const double ratio = static_cast<double>(letters) /
                             static_cast<double>(t.size());
        // Scale by length up to a cap so short inputs are less certain.
        const double len_factor = std::min(1.0, t.size() / 12.0);
        return std::min(0.99, 0.55 + 0.35 * ratio + 0.09 * len_factor);
    }
};

} // namespace voice
