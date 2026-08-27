// Example 08: Event-Driven Sensor Hub (non-AI)
//
// Demonstrates the event-driven execution layer (embg::event::EventGraph)
// for a non-AI application — a sensor hub that:
//
//   1. Receives external "tick" events (from a timer, interrupt, or scheduler)
//   2. On tick: reads sensor, emits "sensor_data" event
//   3. "sensor_data" fans out to multiple handlers:
//        - Logger (records reading)
//        - Threshold checker (emits "alarm" if value exceeds limit)
//   4. "alarm" fans out to:
//        - LED controller (sets alarm LED)
//        - Notification sender (emits "notify" for external consumption)
//   5. External code reads "notify" events
//
// This is the reactive pattern: events drive execution, not linear traversal.
// No AI/LLM involved — pure embedded sensor processing with event-driven routing.
//
// Event flow:
//
//   [external] tick ──▶ read_sensor ──emit──▶ sensor_data
//                                            ├──▶ log_reading     (fan-out)
//                                            └──▶ check_threshold (fan-out)
//                                                 │ if > limit
//                                                 └─emit──▶ alarm
//                                                            ├──▶ set_led    (fan-out)
//                                                            └──▶ send_notify
//                                                                 └─emit──▶ notify
//                                                                            [external output]
//
// Each handler's responsibility:
//   tick_handler           — reads the sensor, emits sensor_data
//   log_reading_handler    — records the reading (sensor_data fan-out 1)
//   check_threshold_handler — emits alarm if value exceeds threshold (fan-out 2)
//   set_led_handler        — turns on the alarm LED (alarm fan-out 1)
//   send_notify_handler    — builds notification, emits notify (alarm fan-out 2)
//   notify_handler          — external output confirmation
//   event_observer         — fires on every event before handlers run

#include <embg/event.hpp>
#include "example_types.hpp"
#include <cmath>
#include <cstdio>
#include <iostream>
#include <random>

using Str      = embg::examples::Str;
using LongStr  = embg::examples::LongStr<>;
using FloatVec = embg::examples::FloatVec<>;
using StrVec   = embg::examples::StrVec<>;

struct SensorHubState {
    int       tick_count      = 0;
    float     current_value   = 0.0f;
    float     threshold       = 30.0f;

    LongStr   log_line        = {};
    bool      alarm_active    = false;
    bool      led_on          = false;
    int       notify_count    = 0;

    FloatVec  readings        = {};
    std::conditional_t<embg::Config::StaticAlloc,
        embg::StaticVector<LongStr, 8>,
        std::vector<std::string>> notifications = {};

    std::mt19937 rng{42};
    int          reading_count = 0;
};

// ─── Simulated sensor ────────────────────────────────────────────────────────

static float read_temperature_sensor(SensorHubState& s) {
    // Simulate a temperature sensor with occasional spikes
    std::normal_distribution<float> dist(25.0f, 3.0f);
    float value = dist(s.rng);
    // Every 4th reading, inject a spike above threshold
    if (++s.reading_count % 4 == 0) value += 8.0f;
    return value;
}

// ─── Event handler implementations ──────────────────────────────────────────
// Free functions so the .on() registrations read as a clean event-flow
// overview — the "what events flow where" story is separated from the
// "what each handler does" story.

// tick → read sensor, emit sensor_data
static void tick_handler(SensorHubState& s, embg::event::EventEmitter& emit) {
    s.tick_count++;
    s.current_value = read_temperature_sensor(s);
    s.readings.push_back(s.current_value);
    std::cout << "  [tick " << s.tick_count << "] sensor=" << s.current_value << " °C\n";
    emit.emit("sensor_data", &s.current_value, sizeof(float));
}

// sensor_data → log (fan-out handler 1)
static void log_reading_handler(SensorHubState& s, embg::event::EventEmitter&) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "tick=%d value=%.1fC", s.tick_count, s.current_value);
    s.log_line = buf;
    std::cout << "  [log] " << s.log_line << "\n";
}

// sensor_data → check threshold (fan-out handler 2)
static void check_threshold_handler(SensorHubState& s, embg::event::EventEmitter& emit) {
    if (s.current_value > s.threshold) {
        std::cout << "  [threshold] " << s.current_value
                  << " > " << s.threshold << " — ALARM\n";
        emit.emit("alarm");
    }
}

// alarm → set LED (fan-out handler 1)
static void set_led_handler(SensorHubState& s, embg::event::EventEmitter&) {
    s.led_on = true;
    s.alarm_active = true;
    std::cout << "  [led] ALARM LED turned ON\n";
}

// alarm → send notification (fan-out handler 2)
static void send_notify_handler(SensorHubState& s, embg::event::EventEmitter& emit) {
    s.notify_count++;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "ALARM: temp=%.1f at tick=%d", s.current_value, s.tick_count);
    LongStr msg = buf;
    s.notifications.push_back(msg);
    std::cout << "  [notify] " << msg << "\n";
    emit.emit("notify", msg.c_str(), msg.size());
}

// notify → external output (handler for the notify event)
static void notify_handler(SensorHubState& s, embg::event::EventEmitter&) {
    std::cout << "  [external] notification sent (total="
              << s.notify_count << ")\n";
}

// Observer — fires on every event before handlers run
static void event_observer(const embg::event::Event& evt, const SensorHubState&) {
    std::cout << "  [event] → " << evt.type << "\n";
}

// ─── Build the sensor hub ────────────────────────────────────────────────────
//
// Each .on() is now a clean declaration: event type → handler. The fan-out
// structure (multiple .on() per event type) is visible at a glance.

static embg::event::EventGraph<SensorHubState> make_sensor_hub() {
    embg::event::EventGraph<SensorHubState> hub;

    // ── Subscribe handlers ──────────────────────────────────────────────────
    hub
        .on("tick",        tick_handler)
        .on("sensor_data", log_reading_handler)       // fan-out handler 1
        .on("sensor_data", check_threshold_handler)    // fan-out handler 2
        .on("alarm",       set_led_handler)            // fan-out handler 1
        .on("alarm",       send_notify_handler)        // fan-out handler 2
        .on("notify",      notify_handler)
        .on_event(event_observer);

    return hub;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== 08 Event-Driven Sensor Hub ===\n";

    auto hub = make_sensor_hub();

    // ── Run simulation: post tick events ─────────────────────────────────────

    SensorHubState state;
    for (int i = 0; i < 8; ++i) {
        std::cout << "\n── cycle " << (i + 1) << " ──\n";
        hub.post("tick");
        hub.process(state, 50);
    }

    // ── Summary ──────────────────────────────────────────────────────────────

    std::cout << "\n═══ Sensor Hub Summary ═══\n";
    std::cout << "  Ticks processed: " << state.tick_count << "\n";
    std::cout << "  Readings logged: " << state.readings.size() << "\n";
    std::cout << "  Alarms fired:    " << state.notify_count << "\n";
    std::cout << "  LED state:       " << (state.led_on ? "ON" : "OFF") << "\n";
    std::cout << "  Threshold:       " << state.threshold << " °C\n";
    std::cout << "\n";
    std::cout << "  This example demonstrates event-driven execution:\n";
    std::cout << "    - External 'tick' events drive the processing loop\n";
    std::cout << "    - 'sensor_data' fans out to logger + threshold checker\n";
    std::cout << "    - 'alarm' fans out to LED controller + notification sender\n";
    std::cout << "    - Handlers emit new events that propagate through the graph\n";
    std::cout << "    - No AI/LLM involved — pure embedded sensor processing\n";

    return 0;
}
