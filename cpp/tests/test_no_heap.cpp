// Test: verify that static-allocation mode performs zero heap allocations.
//
// Overrides global operator new/delete to count allocations.
// Runs framework primitives (Graph, HSM, EventGraph, storage) and
// asserts that no heap allocation occurred.
//
// Build: g++ -std=c++20 -DEMBG_STATIC_ALLOC -I include tests/test_no_heap.cpp -o test_no_heap

#include <embg/graph.hpp>
#include <embg/hsm.hpp>
#include <embg/event.hpp>
#include <embg/embedded.hpp>
#include <embg/storage.hpp>
#include <cstdio>
#include <cstdlib>
#include <new>

static long g_alloc_count = 0;
static long g_alloc_bytes = 0;
static bool g_tracking = false;

void* operator new(std::size_t size) {
    if (g_tracking) {
        ++g_alloc_count;
        g_alloc_bytes += static_cast<long>(size);
    }
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void* operator new[](std::size_t size) {
    if (g_tracking) {
        ++g_alloc_count;
        g_alloc_bytes += static_cast<long>(size);
    }
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

struct TestState {
    embg::StaticString<64> name = {};
    int value = 0;
};

static void test_static_string() {
    embg::StaticString<64> s1 = "hello";
    embg::StaticString<64> s2 = " world";
    embg::StaticString<64> s3 = s1 + s2;
    (void)s3;
}

static void test_static_vector() {
    embg::StaticVector<int, 16> v;
    for (int i = 0; i < 16; ++i) v.push_back(i);
    v.reverse();
}

static void test_static_map() {
    embg::StaticMap<embg::StaticString<32>, int, 8> m;
    m.insert_or_assign("key1", 1);
    m.insert_or_assign("key2", 2);
    auto it = m.find("key1");
    (void)it;
}

static void test_function() {
    embg::Function<int(int), 64> fn = [](int x) { return x * 2; };
    int result = fn(21);
    (void)result;
}

static void test_graph() {
    embg::Graph<TestState> g;
    g.add_node("init", [](TestState& s) { s.value = 1; });
    g.add_node("step", [](TestState& s) { s.value++; });
    g.add_edge("init", "step");
    g.add_edge("step", embg::END);
    g.set_entry("init");

    TestState state;
    g.run(state);
}

static void test_hsm() {
    embg::hsm::HSM<TestState> hsm;
    hsm.add_state({.name = "A", .initial = "A1"});
    hsm.add_state({.name = "A1", .parent = "A"});
    hsm.add_state({.name = "B"});
    hsm.set_initial("A");

    TestState state;
    hsm.init(state);
    hsm.dispatch("go", state);
}

static void test_event_graph() {
    embg::event::EventGraph<TestState> hub;
    hub.on("tick", [](TestState& s, embg::event::EventEmitter& emit) {
        s.value++;
        emit.emit("data");
    });
    hub.on("data", [](TestState& s, embg::event::EventEmitter&) {
        s.value += 10;
    });

    TestState state;
    hub.post("tick");
    hub.process(state);
}

int main() {
    std::printf("=== test_no_heap (static allocation mode) ===\n");

    g_tracking = true;
    g_alloc_count = 0;
    g_alloc_bytes = 0;

    test_static_string();
    test_static_vector();
    test_static_map();
    test_function();
    test_graph();
    test_hsm();
    test_event_graph();

    g_tracking = false;

    std::printf("  Heap allocations during framework operations: %ld (%ld bytes)\n",
                g_alloc_count, g_alloc_bytes);

    if (g_alloc_count == 0) {
        std::printf("  PASS — zero heap allocations in static mode\n");
        return 0;
    } else {
        std::printf("  FAIL — %ld heap allocation(s) detected\n", g_alloc_count);
        return 1;
    }
}
