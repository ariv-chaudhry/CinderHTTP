# CinderHTTP build system.
#
# Targets:
#   make           - optimized build            -> bin/cinderhttp
#   make debug     - ASan/UBSan debug build      -> bin/cinderhttp
#   make test      - build and run unit tests under tests/
#   make run       - build (if needed) and run the server with default config
#   make format    - run scripts/format.sh (clang-format, if installed)
#   make clean     - remove build/ and bin/
#
# Object files and dependency files live under build/, the final binary
# under bin/ - neither directory is committed (see .gitignore).

CC := gcc
STD := -std=c11

# -Wall -Wextra -Wpedantic -Werror catch mistakes at compile time instead of
# in production; treating warnings as errors keeps them from silently
# accumulating as the project grows.
WARN_FLAGS := -Wall -Wextra -Wpedantic -Werror

# glibc hides POSIX / XSI declarations (sigaction, inet_ntop, realpath, etc.)
# under strict -std=c11 unless feature-test macros say otherwise. 200809L is
# POSIX.1-2008; _XOPEN_SOURCE=700 additionally exposes realpath() and related
# XSI APIs used by the static-file path confinement code.
FEATURE_FLAGS := -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700

INCLUDES := -Iinclude

COMMON_CFLAGS := $(STD) $(WARN_FLAGS) $(FEATURE_FLAGS) $(INCLUDES)
RELEASE_CFLAGS := $(COMMON_CFLAGS) -O2
DEBUG_CFLAGS := $(COMMON_CFLAGS) -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer

# Linked in now even though the thread pool lands in a later stage, since
# every subsequent stage depends on pthreads and this way there is one less
# thing to remember to change later.
LDLIBS := -lpthread
DEBUG_LDFLAGS := -fsanitize=address,undefined

SRC_DIR := src
BUILD_DIR := build
BIN_DIR := bin
TEST_DIR := tests

TARGET := $(BIN_DIR)/cinderhttp

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Every object file except main.o - linked into unit test binaries so tests
# can call into config/server/utils (and later parser/router/...) without
# pulling in the real process entry point.
MAIN_OBJ := $(BUILD_DIR)/main.o
CORE_OBJS := $(filter-out $(MAIN_OBJ),$(OBJS))

TEST_SRCS := $(wildcard $(TEST_DIR)/test_*.c)
TEST_BINS := $(patsubst $(TEST_DIR)/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRCS))

CFLAGS ?= $(RELEASE_CFLAGS)

# Make only tracks file timestamps, so switching between `make` and `make
# debug` would otherwise leave stale, sanitizer-instrumented .o files in
# place with nothing forcing a rebuild (their .c files have not changed,
# only the flags have). This marker file records the flags used for the
# *last* build; object files depend on it, so touching it when CFLAGS
# changes is what actually forces a full recompile on the next build.
CFLAGS_MARKER := $(BUILD_DIR)/.cflags

.DEFAULT_GOAL := all

.PHONY: all debug test integration clean run format help FORCE

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(CFLAGS_MARKER) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# FORCE is phony, so it is always "newer" than $(CFLAGS_MARKER), so this
# recipe always runs - but it only rewrites (and thus only bumps the mtime
# of) the marker file when CFLAGS actually differs from last time. Object
# files depend on the marker itself (a real file), so they only see it as
# stale, and only get recompiled, on an actual flag change - a no-op
# `make` right after another still skips everything, as expected.
$(CFLAGS_MARKER): FORCE | $(BUILD_DIR)
	@if [ ! -f $@ ] || [ "$$(cat $@ 2>/dev/null)" != "$(CFLAGS)" ]; then \
		echo "$(CFLAGS)" > $@; \
	fi

FORCE:

$(BUILD_DIR) $(BIN_DIR) $(BUILD_DIR)/tests:
	mkdir -p $@

debug: CFLAGS := $(DEBUG_CFLAGS)
debug: LDFLAGS := $(DEBUG_LDFLAGS)
debug: $(TARGET)

-include $(DEPS)

$(BUILD_DIR)/tests/%: $(TEST_DIR)/%.c $(CORE_OBJS) | $(BUILD_DIR)/tests
	$(CC) $(CFLAGS) $< $(CORE_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

test: $(TEST_BINS)
	@if [ -z "$(strip $(TEST_BINS))" ]; then \
		echo "No unit tests found (expected tests/test_*.c)."; \
		exit 1; \
	else \
		for t in $(TEST_BINS); do \
			echo "== Running $$t =="; \
			$$t || exit 1; \
		done; \
	fi

integration: all
	@bash tests/integration/test_server.sh

run: all
	./$(TARGET)

format:
	@bash scripts/format.sh

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

help:
	@echo "Targets:"
	@echo "  make             - optimized build -> $(TARGET)"
	@echo "  make debug       - ASan/UBSan debug build -> $(TARGET)"
	@echo "  make test        - build and run unit tests"
	@echo "  make integration - run curl-based Stage 2 integration checks"
	@echo "  make run         - build and run the server with default config"
	@echo "  make format      - format sources with clang-format, if installed"
	@echo "  make clean       - remove build/ and bin/"
