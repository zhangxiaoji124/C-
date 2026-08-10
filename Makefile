CXX ?= g++
CC ?= gcc
.DEFAULT_GOAL := all
BUILD_DIR := build
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wpedantic -MMD -MP -Iinclude -Ithird_party -Ithird_party/sqlite
CFLAGS := -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -Ithird_party/sqlite
LDLIBS := -lws2_32

ifeq ($(OS),Windows_NT)
  EXE := .exe
else
  EXE :=
  LDLIBS := -pthread -ldl
endif

CORE_SOURCES := src/database.cpp src/ollama_client.cpp src/agent.cpp src/dev_agent.cpp src/worker.cpp src/http_server.cpp
CORE_OBJECTS := $(CORE_SOURCES:src/%.cpp=$(BUILD_DIR)/%.o)

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean test run

all: $(BUILD_DIR)/orbitops$(EXE) $(BUILD_DIR)/orbitops_tests$(EXE) $(BUILD_DIR)/orbitops_dev_review_test$(EXE)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/sqlite3.o: third_party/sqlite/sqlite3.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/orbitops$(EXE): $(BUILD_DIR)/main.o $(CORE_OBJECTS) $(BUILD_DIR)/sqlite3.o
	$(CXX) $^ -o $@ $(LDLIBS)

$(BUILD_DIR)/unit_tests.o: tests/unit_tests.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/orbitops_tests$(EXE): $(BUILD_DIR)/unit_tests.o $(BUILD_DIR)/database.o $(BUILD_DIR)/ollama_client.o $(BUILD_DIR)/agent.o $(BUILD_DIR)/sqlite3.o
	$(CXX) $^ -o $@ $(LDLIBS)

$(BUILD_DIR)/dev_review_integration_test.o: tests/dev_review_integration_test.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/orbitops_dev_review_test$(EXE): $(BUILD_DIR)/dev_review_integration_test.o $(BUILD_DIR)/ollama_client.o
	$(CXX) $^ -o $@ $(LDLIBS)

test: all
	$(BUILD_DIR)/orbitops_tests$(EXE)

run: $(BUILD_DIR)/orbitops$(EXE)
	$(BUILD_DIR)/orbitops$(EXE)

clean:
	rm -rf $(BUILD_DIR)
