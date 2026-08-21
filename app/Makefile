# Voice assistant demo — embg pipeline + HTTP server + web UI.
#
#   make          build the server binary
#   make run      build + start on http://localhost:8080
#   make test     smoke-test the API with curl (requires server running)

CXX      := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -O2 -pthread \
            -I include -I third_party -I ../cpp/include
LDFLAGS  := -pthread

BIN := build/voice-assistant

.PHONY: all run clean test

all: $(BIN)

$(BIN): src/main.cpp include/*.hpp \
        ../cpp/include/embg/graph.hpp \
        ../cpp/include/embg/config.hpp \
        ../cpp/include/embg/storage.hpp \
        third_party/httplib.h third_party/nlohmann/json.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) src/main.cpp -o $@ $(LDFLAGS)

run: $(BIN)
	./$(BIN) 8080

test:
	@curl -s -X POST http://localhost:8080/api/chat \
		-H 'Content-Type: application/json' \
		-d '{"text":"what time is it?"}' ; echo

clean:
	rm -rf build
