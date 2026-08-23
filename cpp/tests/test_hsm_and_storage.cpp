// Regression tests for HSM transition semantics and StaticString equality.
// Build from cpp/:
//   g++ -std=c++20 -Wall -Wextra -I include tests/test_hsm_and_storage.cpp -o /tmp/test_hsm
//   g++ -std=c++20 -Wall -Wextra -DEMBG_STATIC_ALLOC -I include tests/test_hsm_and_storage.cpp -o /tmp/test_hsm_static

#include <embg/hsm.hpp>
#include <embg/storage.hpp>
#include <embg/event.hpp>

#include <cassert>
#include <stdexcept>
#include <iostream>
#include <string>
#include <vector>

struct TestState {
    std::vector<std::string> actions;
};

template<typename Hsm>
void add_state(Hsm& hsm, const char* name, const char* parent = "", const char* initial = "") {
    hsm.add_state({
        .name = name,
        .parent = parent,
        .on_entry = [name](TestState& state) { state.actions.push_back(std::string("enter ") + name); },
        .on_exit = [name](TestState& state) { state.actions.push_back(std::string("exit ") + name); },
        .initial = initial,
    });
}

void test_external_self_transition() {
    embg::hsm::HSM<TestState> hsm;
    add_state(hsm, "IDLE");
    hsm.add_state({
        .name = "IDLE",
        .on_entry = [](TestState& state) { state.actions.push_back("enter IDLE"); },
        .on_exit = [](TestState& state) { state.actions.push_back("exit IDLE"); },
        .handlers = {{"restart", [](TestState&) { return "IDLE"; }}},
    });
    hsm.set_initial("IDLE");

    TestState state;
    hsm.init(state);
    state.actions.clear();
    hsm.dispatch("restart", state);

    assert((state.actions == std::vector<std::string>{"exit IDLE", "enter IDLE"}));
    assert(hsm.current() == "IDLE");
}

void test_history_reentry() {
    embg::hsm::HSM<TestState> hsm;
    add_state(hsm, "ACTIVE", "", "A");
    hsm.add_state({
        .name = "A", .parent = "ACTIVE",
        .handlers = {{"next", [](TestState&) { return "B"; }}},
    });
    add_state(hsm, "B", "ACTIVE");
    hsm.add_state({
        .name = "OFF",
        .handlers = {{"resume", [](TestState&) { return "ACTIVE"; }}},
    });
    hsm.add_state({
        .name = "ACTIVE",
        .parent = "",
        .on_entry = [](TestState&) {},
        .on_exit = [](TestState&) {},
        .handlers = {{"off", [](TestState&) { return "OFF"; }}},
        .initial = "A",
    });
    hsm.set_initial("ACTIVE");

    TestState state;
    hsm.init(state);
    assert(hsm.current() == "A");
    hsm.dispatch("next", state);
    assert(hsm.current() == "B");
    hsm.dispatch("off", state);
    assert(hsm.current() == "OFF");
    hsm.dispatch("resume", state);
    assert(hsm.current() == "B");
}

void test_nested_initial_descent() {
    embg::hsm::HSM<TestState> hsm;
    add_state(hsm, "TOP", "", "MIDDLE");
    add_state(hsm, "MIDDLE", "TOP", "LEAF");
    add_state(hsm, "LEAF", "MIDDLE");
    hsm.set_initial("TOP");

    TestState state;
    hsm.init(state);

    assert((state.actions == std::vector<std::string>{"enter TOP", "enter MIDDLE", "enter LEAF"}));
    assert(hsm.current() == "LEAF");
}

void test_transition_to_ancestor_updates_current() {
    embg::hsm::HSM<TestState> hsm;
    add_state(hsm, "PARENT");
    hsm.add_state({
        .name = "CHILD", .parent = "PARENT",
        .handlers = {{"up", [](TestState&) { return "PARENT"; }}},
    });
    hsm.set_initial("CHILD");

    TestState state;
    hsm.init(state);
    hsm.dispatch("up", state);
    assert(hsm.current() == "PARENT");
}

void test_reentrant_entry_action() {
    embg::hsm::HSM<TestState> hsm;
    hsm.add_state({
        .name = "IDLE",
        .handlers = {{"go", [](TestState&) { return "A"; }}},
    });
    hsm.add_state({
        .name = "A",
        .on_entry = [&hsm](TestState& state) { hsm.dispatch("next", state); },
        .handlers = {{"next", [](TestState&) { return "B"; }}},
    });
    add_state(hsm, "B");
    hsm.set_initial("IDLE");

    TestState state;
    hsm.init(state);
    hsm.dispatch("go", state);
    assert(hsm.current() == "B");
}

void test_hierarchy_validation() {
    auto rejects = [](auto configure) {
        embg::hsm::HSM<TestState> hsm;
        configure(hsm);
        TestState state;
        try {
            hsm.init(state);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };

    assert(rejects([](auto& hsm) {
        hsm.add_state({.name = "ORPHAN", .parent = "MISSING"}).set_initial("ORPHAN");
    }));
    assert(rejects([](auto& hsm) {
        hsm.add_state({.name = "A", .parent = "B"})
           .add_state({.name = "B", .parent = "A"})
           .set_initial("A");
    }));
    assert(rejects([](auto& hsm) {
        hsm.add_state({.name = "PARENT", .initial = "NOT_A_CHILD"})
           .add_state({.name = "NOT_A_CHILD"})
           .set_initial("PARENT");
    }));
}

void test_static_string_equality() {
    const embg::StaticString<3> value{"abc"};
    assert(value == "abc");
    assert("abc" == value);
    assert(!(value == "abcd"));
    assert(!("abcd" == value));
}

void test_static_storage_safety() {
    embg::StaticMap<int, int, 2> map{{{1, 10}, {2, 20}}};
    int total = 0;
    for (const auto& [key, value] : map) total += key + value;
    assert(total == 33);
    assert(map.find(3) == map.end());
    bool capacity_rejected = false;
    try {
        (void)embg::StaticMap<int, int, 1>{{{1, 10}, {2, 20}}};
    } catch (const std::runtime_error&) {
        capacity_rejected = true;
    }
    assert(capacity_rejected);

    embg::StaticVector<int, 3> values;
    values.resize(2);
    assert(values[0] == 0);
    assert(values[1] == 0);
    bool bounds_rejected = false;
    try {
        (void)values[2];
    } catch (const std::runtime_error&) {
        bounds_rejected = true;
    }
    assert(bounds_rejected);

    const embg::StaticString<8> word{"abc"};
    assert(word.find("a", 4) == embg::StaticString<8>::npos);
    assert(word.substr(1, embg::StaticString<8>::npos - 1) == "bc");

    embg::Function<std::string(), 64> original([text = std::string{"moved"}] { return text; });
    embg::Function<std::string(), 64> moved(std::move(original));
    assert(moved() == "moved");
}

void test_event_queue_recovers_after_exception() {
    embg::event::EventGraph<TestState> graph;
    graph.on("boom", [](TestState&, embg::event::EventEmitter&) { throw std::runtime_error("boom"); });
    graph.post("boom");

    try {
        TestState state;
        graph.process(state);
    } catch (const std::runtime_error&) {
    }

    assert(!graph.pending());
    assert(graph.queue_depth() == 0);
    graph.post("ok");
    TestState state;
    assert(graph.process(state) == 1);
}

int main() {
    test_external_self_transition();
    test_history_reentry();
    test_nested_initial_descent();
    test_transition_to_ancestor_updates_current();
    test_reentrant_entry_action();
    test_hierarchy_validation();
    test_static_string_equality();
    test_static_storage_safety();
    test_event_queue_recovers_after_exception();
    std::cout << "HSM and storage regression tests passed.\n";
}
