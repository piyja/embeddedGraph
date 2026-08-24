#pragma once

// Skill registry — single source of truth for the assistant's capabilities.
//
// Each skill bundles everything the pipeline needs to know about one intent:
//
//   intent    → graph node name ("reply_<intent>") and NLU classification label
//   keywords  → trigger phrases fed to the NLU keyword rules
//   handler   → dialogue function that writes VoiceState::reply
//
// Adding a new skill is one add() call in make_default_skills(): the reply
// nodes, tts edges, and NLU triggers are all derived from this list at init.
// A skill with no keywords (e.g. "unknown") can never be matched directly —
// it only serves as what the NLU falls back to when nothing matches.

#include "dialogue.hpp"
#include "state.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace voice {

struct Skill {
    std::string                      intent;
    std::vector<const char*>         keywords;
    std::function<void(VoiceState&)> handler;
};

class SkillRegistry {
public:
    SkillRegistry& add(Skill skill) {
        skills_.push_back(std::move(skill));
        return *this;
    }

    auto begin() const { return skills_.begin(); }
    auto end()   const { return skills_.end();   }

private:
    std::vector<Skill> skills_;
};

// ─── Built-in demo skill set ──────────────────────────────────────────────────
// The one place to touch when extending the assistant.

inline SkillRegistry make_default_skills() {
    SkillRegistry r;
    r.add({"greeting",
           {"hello", "hi", "hey", "good morning", "good evening", "good afternoon"},
           dialogue::greeting});
    r.add({"time",
           {"time", "clock", "hour", "what time"},
           dialogue::time_query});
    r.add({"weather",
           {"weather", "rain", "temperature", "forecast", "sunny", "cold", "hot"},
           dialogue::weather});
    r.add({"joke",
           {"joke", "funny", "laugh", "make me laugh"},
           dialogue::joke});
    r.add({"help",
           {"help", "what can you do", "commands", "who are you"},
           dialogue::help});
    r.add({"unknown", {}, dialogue::unknown});
    return r;
}

} // namespace voice
