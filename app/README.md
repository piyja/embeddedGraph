# Voice Assistant Demo

An interactive voice-assistant pipeline built on [embeddedGraph](../../README.md):

```
ASR ──▶ NLU ──[intent router]──▶ reply_greeting ──▶ TTS ──▶ END
                             ├───────────────▶ reply_time ─────┘
                             ├───────────────▶ reply_weather ──┘
                             ├───────────────▶ reply_joke ─────┘
                             ├───────────────▶ reply_help ─────┘
                             └───────────────▶ reply_unknown ──┘
```

Type text in the browser UI; each turn runs through the full C++ graph on the
server and every stage's output is displayed:

| Stage | Module | What it does |
|-------|--------|--------------|
| **ASR** | `include/asr.hpp` | Simulated speech-to-text (passthrough + confidence). Swap for whisper.cpp/vosk without touching other stages. |
| **NLU** | `include/nlu.hpp` | Intent classification via keyword string matching (`greeting`, `time`, `weather`, `joke`, `help`, fallback `unknown`). |
| **Router** | `include/pipeline.hpp` | embg conditional edge dispatching to one reply node per intent. |
| **Dialogue** | `include/dialogue.hpp` | One deterministic reply generator per intent (time uses the real clock). |
| **TTS** | `include/tts.hpp` + Web Speech API | Server normalizes text for speech; the browser speaks it via `speechSynthesis`. |

## Run

```bash
cd app
make run          # builds and serves http://localhost:8080
```

Open http://localhost:8080, type an utterance (or click a hint), and watch the
pipeline chips light up — ASR transcript → NLU intent + confidence → spoken
reply.

## API

```bash
curl -s -X POST localhost:8080/api/chat \
     -H 'Content-Type: application/json' \
     -d '{"text":"what time is it?"}'
```

```json
{
  "asr":   { "transcript": "what time is it?", "confidence": 0.90, "latency_us": 2 },
  "nlu":   { "intent": "time", "confidence": 0.96, "matched_keyword": "what time" },
  "reply": "It is 01:05 AM.",
  "tts":   { "text": "It is 01:05 AM.", "ready": true }
}
```

## Layout

```
app/
├── include/
│   ├── state.hpp        # VoiceState — shared pipeline state
│   ├── asr.hpp          # ASR module (simulated)
│   ├── nlu.hpp          # NLU module (keyword string match)
│   ├── dialogue.hpp     # Reply generators, one per intent
│   ├── tts.hpp          # TTS text normalization
│   └── pipeline.hpp     # embg::Graph wiring (pure topology)
├── src/main.cpp         # HTTP server + JSON API (cpp-httplib)
├── web/                 # UI: vanilla HTML/CSS/JS, no build step
│   ├── index.html
│   ├── style.css
│   └── app.js           # Chat rendering + speechSynthesis TTS
└── third_party/         # Vendored single-header deps
    ├── httplib.h        # cpp-httplib v0.15.3
    └── nlohmann/json.hpp # nlohmann/json v3.11.3
```

## Why this stack

- **Server**: C++ with vendored single-header deps — zero system packages,
  consistent with the library's embedded ethos.
- **UI**: vanilla HTML/CSS/JS with no build step — nothing to install, instant
  startup, trivially auditable.
- **TTS audio**: the browser's built-in Web Speech API — real spoken output
  with no model download or native audio stack required.
