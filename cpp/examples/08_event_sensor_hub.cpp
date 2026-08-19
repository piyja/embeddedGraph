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

#include <embg/event.hpp>
#include <cmath>
#include <iostream>
#include <random>
#include <string>
#include <vector>

// ─── State ───────────────────────────────────────────────────────────────────

struct SensorHubState {
    // Sensor readings
    int   tick_count      = 0;
    float current_value   = 0.0f;
    float threshold       = 30.0f;

    // Outputs
    std::string log_line     = {};
    bool        alarm_active = false;
    bool        led_on       = false;
    int         notify_count = 0;

    // History
    std::vector<float> readings = {};

    // External notification output (consumed by main)
    std::vector<std::string> notifications = {};
};

// ─── Simulated sensor ────────────────────────────────────────────────────────

static std::mt19937 rng{42};

static float read_temperature_sensor() {
    // Simulate a temperature sensor with occasional spikes
    std::normal_distribution<float> dist(25.0f, 3.0f);
    float value = dist(rng);
    // Every 4th reading, inject a spike above threshold
    static int reading = 0;
    if (++reading % 4 == 0) value += 8.0f;
    return value;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== 08 Event-Driven Sensor Hub ===\n";

    embg::event::EventGraph<SensorHubState> hub;

    // ── Subscribe handlers ──────────────────────────────────────────────────

    hub
        // tick → read sensor, emit sensor_data
        .on("tick", [](SensorHubState& s, embg::event::EventEmitter& emit) {
            s.tick_count++;
            s.current_value = read_temperature_sensor();
            s.readings.push_back(s.current_value);
            std::cout << "  [tick " << s.tick_count << "] sensor=" << s.current_value << " °C\n";
            emit.emit("sensor_data", &s.current_value, sizeof(float));
        })

        // sensor_data → log (fan-out handler 1)
        .on("sensor_data", [](SensorHubState& s, embg::event::EventEmitter&) {
            s.log_line = "tick=" + std::to_string(s.tick_count) +
                        " value=" + std::to_string(s.current_value) + "C";
            std::cout << "  [log] " << s.log_line << "\n";
        })

        // sensor_data → check threshold (fan-out handler 2)
        .on("sensor_data", [](SensorHubState& s, embg::event::EventEmitter& emit) {
            if (s.current_value > s.threshold) {
                std::cout << "  [threshold] " << s.current_value
                          << " > " << s.threshold << " — ALARM\n";
                emit.emit("alarm");
            }
        })

        // alarm → set LED (fan-out handler 1)
        .on("alarm", [](SensorHubState& s, embg::event::EventEmitter&) {
            s.led_on = true;
            s.alarm_active = true;
            std::cout << "  [led] ALARM LED turned ON\n";
        })

        // alarm → send notification (fan-out handler 2)
        .on("alarm", [](SensorHubState& s, embg::event::EventEmitter& emit) {
            s.notify_count++;
            std::string msg = "ALARM: temp=" + std::to_string(s.current_value) +
                              " at tick=" + std::to_string(s.tick_count);
            s.notifications.push_back(msg);
            std::cout << "  [notify] " << msg << "\n";
            emit.emit("notify", msg.c_str(), msg.size());
        })

        // notify → external output (handler for the notify event)
        .on("notify", [](SensorHubState& s, embg::event::EventEmitter&) {
            std::cout << "  [external] notification sent (total="
                      << s.notify_count << ")\n";
        })

        // Observer — fires on every event before handlers run
        .on_event([](const embg::event::Event& evt, const SensorHubState&) {
            std::cout << "  [event] → " << evt.type << "\n";
        });

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
