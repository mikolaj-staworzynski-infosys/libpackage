# PC test for findApplicationsToRestart - compiles the REAL library source
# (../src/ApplicationsToRestart.cpp), no mirrored copy. Self-contained:
# googletest is fetched/built into ../.deps (independent of other projects).

CXX ?= g++

PROJECT_ROOT := $(abspath ..)
GTEST_DIR ?= $(PROJECT_ROOT)/.deps/googletest
GTEST_BUILD_DIR ?= $(PROJECT_ROOT)/.deps/build
GTEST_REPO ?= https://github.com/google/googletest.git

GTEST_CFLAGS = -isystem $(GTEST_DIR)/googletest/include -I$(GTEST_DIR)/googletest -pthread
GTEST_OBJS = $(GTEST_BUILD_DIR)/gtest-all.o $(GTEST_BUILD_DIR)/gtest_main.o

TEST_BIN = build/applications_to_restart_test

all: $(TEST_BIN)

build:
	mkdir -p build

$(GTEST_DIR):
	mkdir -p $(PROJECT_ROOT)/.deps
	git clone --depth 1 $(GTEST_REPO) $(GTEST_DIR)

$(GTEST_BUILD_DIR):
	mkdir -p $(GTEST_BUILD_DIR)

# googletest's own sources are third-party: build them without our warning flags.
$(GTEST_BUILD_DIR)/gtest-all.o: | $(GTEST_DIR) $(GTEST_BUILD_DIR)
	$(CXX) -std=c++17 -O2 $(GTEST_CFLAGS) -w -c -o $@ $(GTEST_DIR)/googletest/src/gtest-all.cc

$(GTEST_BUILD_DIR)/gtest_main.o: | $(GTEST_DIR) $(GTEST_BUILD_DIR)
	$(CXX) -std=c++17 -O2 $(GTEST_CFLAGS) -w -c -o $@ $(GTEST_DIR)/googletest/src/gtest_main.cc

$(TEST_BIN): ApplicationsToRestartTest.cpp ../src/ApplicationsToRestart.cpp ../include/ApplicationsToRestart.h $(GTEST_OBJS) | build
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 -I$(CURDIR)/../include $(GTEST_CFLAGS) -o $@ ApplicationsToRestartTest.cpp ../src/ApplicationsToRestart.cpp $(GTEST_OBJS)

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -rf build

.PHONY: all test clean
