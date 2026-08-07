CXX      := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -O2 -I include
BINDIR   := build

EXAMPLES := 01_simple_chain 02_agent_loop 03_embedded_sensor 04_automotive_diagnostic \
            05_hsm_ecu_states 06_llm_diagnostic 07_llm_brain_agent

.PHONY: all clean

all: $(BINDIR) $(addprefix $(BINDIR)/, $(EXAMPLES))

$(BINDIR):
	mkdir -p $(BINDIR)

$(BINDIR)/%: examples/%.cpp include/embg/graph.hpp include/embg/embedded.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -rf $(BINDIR)
