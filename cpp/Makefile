CXX      := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -O2 -I include
BINDIR   := build
STATIC_BINDIR := build_static

EXAMPLES := 01_simple_chain 02_agent_loop 03_embedded_sensor 04_automotive_diagnostic \
            05_hsm_ecu_states 06_llm_diagnostic 07_llm_brain_agent 08_event_sensor_hub \
            09_voice_assistant

HEADERS  := include/embg/config.hpp include/embg/error.hpp include/embg/storage.hpp \
            include/embg/graph.hpp include/embg/embedded.hpp \
            include/embg/hsm.hpp include/embg/inference.hpp \
            include/embg/event.hpp

# Static-allocation mode: make STATIC=1
ifdef STATIC
  CXXFLAGS += -DEMBG_STATIC_ALLOC
  BINDIR   := $(STATIC_BINDIR)
endif

.PHONY: all clean run check test

all: $(BINDIR) $(addprefix $(BINDIR)/, $(EXAMPLES))

run: all
	@for ex in $(EXAMPLES); do \
		echo "\n──────────────────────── $$ex ────────────────────────\n"; \
		./$(BINDIR)/$$ex; \
	done

$(BINDIR):
	mkdir -p $(BINDIR)

$(BINDIR)/%: examples/%.cpp $(HEADERS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -rf $(BINDIR) $(STATIC_BINDIR)

# Build and run both modes, diff outputs — must be identical
check:
	@echo "=== Building default mode ==="
	@mkdir -p /tmp/opencode
	@make clean > /dev/null 2>&1
	@make all 2>&1
	@echo "=== Building static mode ==="
	@make all STATIC=1 2>&1
	@echo "=== Comparing outputs ==="
	@passed=0; failed=0; \
	for ex in $(EXAMPLES); do \
		./build/$$ex > /tmp/opencode/out_default.txt 2>&1; \
		./build_static/$$ex > /tmp/opencode/out_static.txt 2>&1; \
		if diff -q /tmp/opencode/out_default.txt /tmp/opencode/out_static.txt > /dev/null; then \
			echo "  PASS $$ex — outputs match"; \
			passed=`expr $$passed + 1`; \
		else \
			echo "  FAIL $$ex — outputs differ"; \
			diff /tmp/opencode/out_default.txt /tmp/opencode/out_static.txt | head -10; \
			failed=`expr $$failed + 1`; \
		fi; \
	done; \
	echo "\nResults: $$passed passed, $$failed failed"; \
	[ $$failed -eq 0 ]

test:
	$(CXX) $(CXXFLAGS) tests/test_with_timeout.cpp -o /tmp/embg_test_with_timeout
	/tmp/embg_test_with_timeout
	$(CXX) $(CXXFLAGS) tests/test_hsm_and_storage.cpp -o /tmp/embg_test_hsm_and_storage
	/tmp/embg_test_hsm_and_storage
	$(CXX) $(CXXFLAGS) -DEMBG_STATIC_ALLOC tests/test_with_timeout.cpp -o /tmp/embg_test_with_timeout_static
	/tmp/embg_test_with_timeout_static
	$(CXX) $(CXXFLAGS) -DEMBG_STATIC_ALLOC tests/test_hsm_and_storage.cpp -o /tmp/embg_test_hsm_and_storage_static
	/tmp/embg_test_hsm_and_storage_static
