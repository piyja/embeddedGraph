// Voice assistant demo — HTTP server exposing the embg pipeline.
//
// Endpoints:
//   GET  /                → web UI (index.html)
//   GET  /static/<file>   → web assets (css/js)
//   POST /api/chat        → {"text": "..."} → full pipeline trace as JSON
//
// The pipeline itself is pure C++ (embg::Graph); this file only handles
// transport: HTTP in, JSON out.

#include <httplib.h>

#include <embg/graph.hpp>

#include <nlohmann/json.hpp>

#include "pipeline.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using nlohmann::json;

} // namespace

int main(int argc, char** argv) {
    const int port = argc > 1 ? std::atoi(argv[1]) : 8080;

    voice::Pipeline pipeline;

    httplib::Server svr;

    // ── Static web UI ──────────────────────────────────────────────────────────
    svr.set_mount_point("/", "web");

    // ── Pipeline API ───────────────────────────────────────────────────────────
    svr.Post("/api/chat", [&pipeline](const httplib::Request& req,
                                      httplib::Response& res) {
        std::string text;
        try {
            const auto body = json::parse(req.body);
            if (body.contains("text")) text = body.at("text").get<std::string>();
        } catch (const std::exception&) {
            json err = { {"error", "invalid JSON body"} };
            res.status = 400;
            res.set_content(err.dump(), "application/json");
            return;
        }

        const voice::VoiceState s = pipeline.process(text);

        json j;
        j["asr"]   = { {"transcript", s.transcript},
                       {"confidence", s.asr_confidence},
                       {"latency_us", s.asr_latency_us} };
        j["nlu"]   = { {"intent", s.intent},
                       {"confidence", s.nlu_confidence},
                       {"matched_keyword", s.matched_keyword} };
        j["reply"] = s.reply;
        j["tts"]   = { {"text", s.tts_text}, {"ready", s.tts_ready} };

        res.set_content(j.dump(), "application/json");
    });

    std::cout << "Voice assistant demo listening on http://localhost:" << port << "\n";
    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "Failed to bind port " << port << "\n";
        return 1;
    }
    return 0;
}
