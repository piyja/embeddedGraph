// Example 09: Voice Assistant — Full ASR → NLU → Arbitration → Agent → TTS Pipeline
//
// A voice assistant built entirely on embg::Graph. No AI/LLM — the NLU is
// a small string-matching intent classifier, and the agents are rule-based.
// The architecture is production-shaped: swap the NLU node for an LLM call
// and the agent nodes for tool-calling sub-graphs, and you have a real
// voice assistant with the same graph topology.
//
// Pipeline:
//
//   asr → nlu → arbitration → [router on decision]
//                               ├──→ clarify ──────────────────────→ END
//                               ├──→ greeting_agent  → tts → END
//                               ├──→ time_agent      → tts → END
//                               ├──→ weather_agent   → tts → END
//                               ├──→ music_agent     → tts → END
//                               ├──→ joke_agent      → tts → END
//                               ├──→ help_agent      → tts → END
//                               └──→ fallback_agent  → tts → END
//
// Each stage's responsibility:
//   asr        — receives the speech-to-text string (simulated input)
//   nlu        — intent classification + entity extraction (string matching)
//   arbitration— confidence gate: high → handle, low → ask for clarification
//   router     — conditional edge: routes to the agent matching the intent
//   agent_*    — satisfies the request, writes response text
//   tts        — "speaks" the response (prints to stdout for now)

#include <embg/graph.hpp>
#include <embg/embedded.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

// ─── State ───────────────────────────────────────────────────────────────────

struct VoiceState {
    // Input (from ASR)
    std::string asr_text = {};

    // NLU output
    std::string intent       = {};   // greeting|time|weather|music|joke|help|unknown
    double last_confidence = 0.0;  // 0.0–1.0 (required by ConfidenceState concept)
    std::string entity       = {};   // extracted slot (e.g. song name, city)

    // Arbitration
    std::string decision     = {};   // "handle" | "clarify"

    // Agent output
    std::string response     = {};

    // TTS
    std::string spoken       = {};

    // Turn tracking
    int         turn         = 0;
};

static_assert(embg::embedded::ConfidenceState<VoiceState>,
    "VoiceState must satisfy ConfidenceState for confidence-gated arbitration");

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// ─── NLU: Intent classification + entity extraction ──────────────────────────
//
// A real system would use an LLM or a fine-tuned intent classifier.
// Here we use keyword matching with a simple confidence model:
//   - Exact keyword match → high confidence (0.9+)
//   - Partial / fuzzy match → medium confidence (0.6–0.8)
//   - No match → low confidence (0.2)

struct IntentRule {
    const char* intent;
    const char* keywords[4];
    double confidence;
};

static const IntentRule INTENT_RULES[] = {
    { "greeting", {"hello", "hi", "hey", "good morning"}, 0.95 },
    { "time",     {"what time", "current time", "time is it", "clock"}, 0.92 },
    { "weather",  {"weather", "temperature", "forecast", "how hot"}, 0.90 },
    { "music",    {"play", "music", "song", "spotify"}, 0.88 },
    { "joke",     {"joke", "funny", "make me laugh", "humor"}, 0.90 },
    { "help",     {"help", "what can you do", "commands", "options"}, 0.85 },
};

static constexpr int NUM_RULES = sizeof(INTENT_RULES) / sizeof(INTENT_RULES[0]);

static void classify_intent(const std::string& text, std::string& intent,
                             double& last_confidence, std::string& entity) {
    std::string lower = to_lower(text);

    for (int i = 0; i < NUM_RULES; ++i) {
        const auto& rule = INTENT_RULES[i];
        for (int k = 0; k < 4; ++k) {
            if (rule.keywords[k] && contains(lower, rule.keywords[k])) {
                intent          = rule.intent;
                last_confidence = rule.confidence;

                // Entity extraction — very basic
                if (std::string(rule.intent) == "music") {
                    // Extract everything after "play"
                    auto pos = lower.find("play");
                    if (pos != std::string::npos) {
                        auto start = pos + 5; // skip "play "
                        while (start < text.size() && std::isspace(text[start])) ++start;
                        entity = text.substr(start);
                    }
                    if (entity.empty()) entity = "some music";
                }
                else if (std::string(rule.intent) == "weather") {
                    // Look for a city name after "in" or "for"
                    auto pos = lower.find(" in ");
                    if (pos == std::string::npos) pos = lower.find(" for ");
                    if (pos != std::string::npos) {
                        auto start = pos + 4;
                        while (start < text.size() && std::isspace(text[start])) ++start;
                        entity = text.substr(start);
                    }
                    if (entity.empty()) entity = "current location";
                }
                return;
            }
        }
    }

    intent          = "unknown";
    last_confidence = 0.20;
    entity     = {};
}

// ─── Build the voice assistant graph ──────────────────────────────────────────

static embg::Graph<VoiceState> make_voice_assistant() {
    embg::Graph<VoiceState> g;

    g
        // ── ASR: receives speech-to-text output ─────────────────────────────
        // In production, this node would interface with Porcupine, Whisper,
        // or an audio front-end. Here we just pass the input through.
        .add_node("asr", [](VoiceState& s) {
            s.turn++;
            std::cout << "  [ASR]  heard: \"" << s.asr_text << "\"\n";
        })
        // ── NLU: intent classification + entity extraction ───────────────────
        .add_node("nlu", [](VoiceState& s) {
            classify_intent(s.asr_text, s.intent, s.last_confidence, s.entity);
            std::cout << "  [NLU]  intent=" << s.intent
                      << "  conf=" << s.last_confidence;
            if (!s.entity.empty())
                std::cout << "  entity=\"" << s.entity << "\"";
            std::cout << "\n";
        })
        .add_edge("asr", "nlu")

        // ── Arbitration: confidence gate ─────────────────────────────────────
        // High confidence → handle. Low confidence → ask for clarification.
        // This is the "safety" layer — prevents acting on uncertain understanding.
        .add_node("arbitration", [](VoiceState& s) {
            if (s.last_confidence >= 0.70) {
                s.decision = "handle";
                std::cout << "  [ARB]  confidence " << s.last_confidence
                          << " ≥ 0.70 → handling\n";
            } else {
                s.decision = "clarify";
                std::cout << "  [ARB]  confidence " << s.last_confidence
                          << " < 0.70 → needs clarification\n";
            }
        })

        // ── Edge: nlu → arbitration ───────────────────────────────────────────
        .add_edge("nlu", "arbitration")

        // ── Router 1: route based on arbitration decision ─────────────────────
        // If "handle" → route to the agent matching the intent.
        // If "clarify" → go to the clarify node.
        .add_conditional_edge("arbitration", [](const VoiceState& s) -> std::string {
            if (s.decision == "clarify") return "clarify";
            return s.intent;   // routes to the agent node named after the intent
        })

        // ── Clarify node: asks the user to rephrase ───────────────────────────
        .add_node("clarify", [](VoiceState& s) {
            s.response = "I'm not sure I understood. Could you rephrase that?";
            std::cout << "  [CLAR] " << s.response << "\n";
        })
        .add_edge("clarify", "tts")

        // ── Agents: one per intent ────────────────────────────────────────────
        // Each agent "satisfies" the request and writes a response.

        .add_node("greeting", [](VoiceState& s) {
            s.response = "Hello! How can I help you today?";
            std::cout << "  [AGT]  greeting agent activated\n";
        })
        .add_edge("greeting", "tts")

        .add_node("time", [](VoiceState& s) {
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            char buf[64];
            std::strftime(buf, sizeof(buf), "%I:%M %p", std::localtime(&t));
            s.response = std::string("The current time is ") + buf + ".";
            std::cout << "  [AGT]  time agent activated\n";
        })
        .add_edge("time", "tts")

        .add_node("weather", [](VoiceState& s) {
            // Simulated weather — in production this would call a weather API
            std::string location = s.entity.empty() ? "your location" : s.entity;
            s.response = "The weather in " + location +
                        " is 22 degrees Celsius, partly cloudy, with light wind.";
            std::cout << "  [AGT]  weather agent activated (location=" << location << ")\n";
        })
        .add_edge("weather", "tts")

        .add_node("music", [](VoiceState& s) {
            std::string track = s.entity.empty() ? "a random playlist" : s.entity;
            s.response = "Now playing " + track + ".";
            std::cout << "  [AGT]  music agent activated (track=" << track << ")\n";
        })
        .add_edge("music", "tts")

        .add_node("joke", [](VoiceState& s) {
            static const char* jokes[] = {
                "Why don't programmers like nature? It has too many bugs.",
                "I told my computer I needed a break, and it said 'No problem — I'll go to sleep.'",
                "Why did the developer go broke? Because he used up all his cache.",
                "There are 10 types of people: those who understand binary and those who don't.",
            };
            static int idx = 0;
            s.response = std::string(jokes[idx++ % 4]);
            std::cout << "  [AGT]  joke agent activated\n";
        })
        .add_edge("joke", "tts")

        .add_node("help", [](VoiceState& s) {
            s.response = "I can help with: greetings, telling the time, "
                        "weather forecasts, playing music, and telling jokes. "
                        "Just say things like 'what time is it' or 'play some music'.";
            std::cout << "  [AGT]  help agent activated\n";
        })
        .add_edge("help", "tts")

        // Fallback agent — intent "unknown" routes here (if confidence is high enough)
        .add_node("unknown", [](VoiceState& s) {
            s.response = "I heard you say: \"" + s.asr_text +
                        "\", but I'm not sure how to help with that.";
            std::cout << "  [AGT]  fallback agent activated\n";
        })
        .add_edge("unknown", "tts")

        // ── TTS: "speaks" the response ────────────────────────────────────────
        // In production, this would feed into a text-to-speech engine
        // (e.g. Piper, espeak, Coqui TTS). For now, we print to stdout.
        .add_node("tts", [](VoiceState& s) {
            s.spoken = s.response;
            std::cout << "  [TTS]  ♪ \"" << s.spoken << "\"\n";
        })
        .add_edge("tts", embg::END)

        // ── Entry point ───────────────────────────────────────────────────────
        .set_entry("asr")

        // ── Streaming: log each node as it executes ───────────────────────────
        .on_step([](std::string_view node, const VoiceState& s) {
            std::cout << "\n── [" << node << "]  turn=" << s.turn
                      << "  intent=" << (s.intent.empty() ? "(pending)" : s.intent) << "\n";
        });

    return g;
}

// ─── Main: simulate voice assistant interactions ──────────────────────────────

int main() {
    std::cout << "=== 09 Voice Assistant ===\n\n";

    auto assistant = make_voice_assistant();

    // Simulated ASR inputs — what the user "said"
    const char* inputs[] = {
        "Hey, hello there!",
        "What time is it right now?",
        "What's the weather like in Munich?",
        "Play Bohemian Rhapsody",
        "Tell me a joke",
        "What can you do?",
        "Xyzzy quux blargh",          // low confidence → clarification
        "Good morning",               // greeting again
    };

    for (const char* input : inputs) {
        VoiceState state;
        state.asr_text = input;

        std::cout << "━━━ User speaks ━━━\n";
        assistant.run(state, 20);
        std::cout << "\n";
    }

    // Summary
    std::cout << "═══ Voice Assistant Summary ═══\n";
    std::cout << "  Pipeline: asr → nlu → arbitration → [router] → agent → tts → END\n";
    std::cout << "  Intents:  greeting, time, weather, music, joke, help, unknown\n";
    std::cout << "  Arbitration: confidence ≥ 0.70 → handle, < 0.70 → clarify\n";
    std::cout << "  TTS: prints to stdout (would feed a real TTS engine in production)\n";
    std::cout << "\n";
    std::cout << "  To make this a real assistant:\n";
    std::cout << "    1. Replace ASR node → audio capture + Whisper/Porcupine\n";
    std::cout << "    2. Replace NLU node → LLM intent classification (embg::inference)\n";
    std::cout << "    3. Replace agent nodes → tool-calling sub-graphs\n";
    std::cout << "    4. Replace TTS node → Piper/espeak/Coqui TTS\n";
    std::cout << "    5. Wrap in HSM for OPERATING/LISTENING/PROCESSING/SPEAKING states\n";

    return 0;
}
