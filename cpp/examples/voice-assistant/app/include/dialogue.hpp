#pragma once

// Dialogue module — intent-specific reply generators.
//
// One free function per intent so the graph routes to a distinct node per
// intent (showing off conditional edges). Each generator reads only the
// NLU/ASR outputs from state and writes `reply`.
//
// In a real system these would call an LLM (embg::inference::make_node) or a
// full dialogue manager. Here they are deterministic templates, with the time
// intent using the actual system clock.

#include "state.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace voice::dialogue {

inline void greeting(VoiceState& s) {
    s.reply = "Hello! I am your embedded voice assistant. "
              "Ask me for the time, weather, a joke, or say help.";
}

inline void time_query(VoiceState& s) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&t, &local);
    std::ostringstream oss;
    oss << "It is " << std::put_time(&local, "%I:%M %p") << ".";
    s.reply = oss.str();
}

inline void weather(VoiceState& s) {
    // Deterministic stand-in for a real weather service call.
    s.reply = "The forecast says 24 degrees and sunny — "
              "a perfect day to test embedded software outdoors.";
}

inline void joke(VoiceState& s) {
    s.reply = "Why do programmers prefer dark mode? "
              "Because light attracts bugs.";
}

inline void help(VoiceState& s) {
    s.reply = "I understand greetings, requests for the time, the weather, "
              "jokes, and this help message. Try: what time is it?";
}

inline void unknown(VoiceState& s) {
    s.reply = "Sorry, I did not understand that. Say help to hear what I can do.";
}

} // namespace voice::dialogue
