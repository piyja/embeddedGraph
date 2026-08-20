#pragma once

// Event-driven execution layer for embg.
//
// Adds reactive, event-driven node execution to the graph framework:
//   - External events can be posted as inputs to trigger node execution
//   - Nodes (event handlers) can emit new events during execution
//   - Events fan out to multiple subscribers (1-to-N routing)
//   - Event chains propagate: handler A emits → handler B emits → ...
//
// This complements the synchronous Graph (linear traversal) and the
// event-driven HSM (state transitions). Use cases:
//   - Sensor hubs: tick → read → fan-out to logger + threshold → alarm
//   - Protocol handlers: packet → parse → validate → route → respond
//   - Control loops: timer → sense → compute → actuate → feedback
//   - Any reactive system where external events drive execution
//
// Integration with existing layers:
//   - A Graph node can call EventGraph::process() inside a node fn
//   - An EventGraph handler can call Graph::run() in response to an event
//   - HSM on_transition can post events to an EventGraph
//   - EventGraph handler can dispatch events to an HSM
//
// Static-allocation compatible: fixed-capacity queue + subscription list.

#include "config.hpp"
#include "error.hpp"
#include "storage.hpp"
#include "graph.hpp"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace embg::event {

// ─── Event ───────────────────────────────────────────────────────────────────
//
// An event is a type name (string) + an optional payload pointer.
// The type string is NOT owned — it must point to persistent storage
// (string literal, static char[], or a string that outlives the queue).
// The payload pointer is also non-owning. This is zero-copy and heap-free.

struct Event {
    const char*  type      = "";
    const void*  data      = nullptr;
    std::size_t  data_size = 0;
};

// ─── EventEmitter ────────────────────────────────────────────────────────────
//
// Lightweight, type-erased handle that event handlers use to emit new events.
// No heap allocation — uses a function pointer + context pointer (16 bytes).
// Constructed internally by EventGraph::process() and passed to each handler.
//
// Testing: bind() is public so EventEmitter can be created in unit tests
// bound to any queue-like container with push_back(const Event&).

class EventEmitter {
public:
    void emit(const char* type) {
        if (push_) push_(ctx_, {type, nullptr, 0});
    }

    void emit(const char* type, const void* data, std::size_t size) {
        if (push_) push_(ctx_, {type, data, size});
    }

    // Factory: bind an EventEmitter to any queue with push_back(const Event&).
    // Public for testability — EventGraph uses this internally, but tests can
    // also create an emitter bound to a local StaticVector or std::vector.
    template<typename Queue>
    static EventEmitter bind(Queue* q) {
        return EventEmitter(q, [](void* ctx, const Event& e) {
            auto* queue = static_cast<Queue*>(ctx);
            queue->push_back(e);
        });
    }

    // Default-constructed emitter is a no-op (push_ is null).
    EventEmitter() noexcept = default;

private:
    using PushFn = void(*)(void*, const Event&);

    void*   ctx_  = nullptr;
    PushFn  push_ = nullptr;

    EventEmitter(void* ctx, PushFn push) : ctx_(ctx), push_(push) {}

    template<GraphState S, typename Cfg> friend class EventGraph;
};

// ─── Config-conditional aliases ──────────────────────────────────────────────

namespace detail {

template<GraphState S, typename Cfg>
using EventHandler = std::conditional_t<Cfg::StaticAlloc,
    Function<void(S&, EventEmitter&), Cfg::FnInlineBytes>,
    std::function<void(S&, EventEmitter&)>>;

template<typename Cfg>
using EventQueue = std::conditional_t<Cfg::StaticAlloc,
    StaticVector<Event, Cfg::MaxEvents>,
    std::vector<Event>>;

} // namespace detail

// ─── EventGraph ──────────────────────────────────────────────────────────────
//
// Reactive event graph: handlers subscribe to event types, external code
// posts events, process() drains the queue dispatching to subscribers.
//
// Fan-out: multiple handlers can subscribe to the same event type.
// Fan-in: one handler can be subscribed to multiple event types (call on() twice).
// Event generation: handlers emit new events via the EventEmitter argument.

template<GraphState S, typename Cfg = Config>
class EventGraph {
public:
    using StringT    = embg::detail::String<Cfg>;
    using HandlerFn  = detail::EventHandler<S, Cfg>;
    using Queue      = detail::EventQueue<Cfg>;
    using ObserveFn  = std::conditional_t<Cfg::StaticAlloc,
        Function<void(const Event&, const S&), Cfg::FnInlineBytes>,
        std::function<void(const Event&, const S&)>>;

private:
    struct Subscription {
        StringT   event_type;
        HandlerFn handler;
    };

    using SubList = std::conditional_t<Cfg::StaticAlloc,
        StaticVector<Subscription, Cfg::MaxSubscriptions>,
        std::vector<Subscription>>;

public:
    // ── Builder API ───────────────────────────────────────────────────────────

    // Subscribe a handler to an event type.
    // Multiple handlers per type = fan-out. Returns *this for chaining.
    // The handler receives (state, emitter) — call emitter.emit() to
    // generate new events. If you don't need to emit, just ignore the parameter.
    EventGraph& on(const char* event_type, HandlerFn handler) {
        subscriptions_.push_back({StringT(event_type), std::move(handler)});
        return *this;
    }

    // Register an observer — fires on every event dispatch (before handlers run).
    EventGraph& on_event(ObserveFn fn) {
        observe_ = std::move(fn);
        return *this;
    }

    // ── Event input ───────────────────────────────────────────────────────────

    // Post an external event into the queue. Does not process — call process().
    void post(const char* type) {
        queue_.push_back({type, nullptr, 0});
    }

    void post(const char* type, const void* data, std::size_t size) {
        queue_.push_back({type, data, size});
    }

    void post(const Event& event) {
        queue_.push_back(event);
    }

    // ── Processing ────────────────────────────────────────────────────────────

    // Process all pending events. Each event is dispatched to all matching
    // handlers. Handlers may emit new events, which are enqueued and processed
    // in subsequent iterations (FIFO order).
    //
    // Returns the number of events processed. Stops when:
    //   - Queue is empty, OR
    //   - max_events reached (safety bound against infinite event loops)
    //
    std::size_t process(S& state, std::size_t max_events = 100) {
        EventEmitter emitter = EventEmitter::bind(&queue_);
        std::size_t processed = 0;

        while (head_ < queue_.size()) {
            if (processed >= max_events)
                EMBG_ERROR(MaxEventsExceeded,
                    "embg::event::EventGraph: max_events exceeded — "
                    "check for unbounded event loops");

            Event evt = queue_[head_];
            ++head_;
            ++processed;

            if (observe_) (*observe_)(evt, state);

            // Fan-out: dispatch to ALL matching subscribers
            for (auto& sub : subscriptions_) {
                if (sub.event_type == evt.type) {
                    sub.handler(state, emitter);
                }
            }
        }

        // Reset queue for next cycle
        queue_.clear();
        head_ = 0;
        return processed;
    }

    // Check if events are pending in the queue.
    bool pending() const noexcept {
        return head_ < queue_.size();
    }

    // Number of events waiting to be processed.
    std::size_t queue_depth() const noexcept {
        return queue_.size() - head_;
    }

    // Clear all pending events without processing.
    void clear() noexcept {
        queue_.clear();
        head_ = 0;
    }

private:
    SubList             subscriptions_;
    Queue               queue_;
    std::size_t         head_   = 0;
    std::optional<ObserveFn> observe_;
};

} // namespace embg::event
