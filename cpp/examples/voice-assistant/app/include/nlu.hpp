#pragma once

// NLU (Natural Language Understanding) module.
//
// Intent classification via keyword string matching — the "for now" baseline
// requested for this demo. The public surface (NluResult: intent + confidence
// + matched evidence) is designed so a real classifier (embeddings, small
// transformer, on-device LLM) can replace the implementation without any
// change to the dialogue module or the graph wiring.

#include "skills.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace voice {

struct NluResult {
    std::string intent;           // "greeting", "time", "weather", ...
    double      confidence = 0.0;
    std::string matched_keyword;  // which keyword triggered the intent
};

class Nlu {
public:
    struct Rule {
        std::vector<std::string> keywords;  // owned copies of skill triggers
        std::string              intent;
    };

    explicit Nlu(std::vector<Rule> rules) : rules_(std::move(rules)) {}

    NluResult classify(const std::string& transcript) const {
        const std::string lower = to_lower(transcript);

        NluResult best;
        for (const auto& rule : rules_) {
            for (const auto& keyword : rule.keywords) {
                const std::string kw = to_lower(keyword);
                if (kw.empty()) continue;
                if (contains_word(lower, kw)) {
                    // Longer keyword matches are stronger evidence.
                    const double conf = std::min(0.98,
                        0.60 + 0.04 * static_cast<double>(kw.size()));
                    if (conf > best.confidence) {
                        best.intent          = rule.intent;
                        best.confidence      = conf;
                        best.matched_keyword = keyword;
                    }
                }
            }
        }
        if (best.intent.empty()) {
            best.intent     = "unknown";
            best.confidence = 0.30;
        }
        return best;
    }

private:
    std::vector<Rule> rules_;

    static std::string to_lower(const std::string& s) {
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return out;
    }

    // Whole-word containment so "hi" doesn't match inside "this".
    static bool contains_word(const std::string& text, const std::string& word) {
        std::size_t pos = 0;
        while ((pos = text.find(word, pos)) != std::string::npos) {
            const bool left_ok  = pos == 0 || !is_alnum(text[pos - 1]);
            const std::size_t end = pos + word.size();
            const bool right_ok  = end == text.size() || !is_alnum(text[end]);
            if (left_ok && right_ok) return true;
            ++pos;
        }
        return false;
    }

    static bool is_alnum(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0;
    }
};

// ─── Rule set derived from the skill registry ─────────────────────────────────
// One NLU rule per skill with trigger keywords; skills without keywords
// (e.g. the "unknown" fallback) are skipped — they can never be matched.

inline Nlu make_default_nlu(const SkillRegistry& skills) {
    std::vector<Nlu::Rule> rules;
    for (const auto& skill : skills) {
        if (skill.keywords.empty()) continue;
        Nlu::Rule rule;
        rule.intent = skill.intent;
        rule.keywords.assign(skill.keywords.begin(), skill.keywords.end());
        rules.push_back(std::move(rule));
    }
    return Nlu(std::move(rules));
}

} // namespace voice
