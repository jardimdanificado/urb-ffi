NODE ?= node
NODE_PREFIX ?=
ROOT := $(abspath ../..)
AMALGAMATE := $(ROOT)/tools/amalgamate.sh
AMALGAMATED_HEADER := $(ROOT)/dist/urbc.h
AMALGAMATED_CLI := $(ROOT)/dist/urbccli.c
AMALGAMATE_INPUTS := $(AMALGAMATE) $(ROOT)/include/urb.h $(ROOT)/src/urbc_internal.h $(wildcard $(ROOT)/include/urbc/*.h) $(wildcard $(ROOT)/src/*.c)
NODE_BIN := $(shell command -v $(NODE) 2>/dev/null)
NODE_INCLUDES ?= $(shell \
	if [ -n "$(NODE_PREFIX)" ] && [ -f "$(NODE_PREFIX)/include/node/node_api.h" ]; then \
		printf "%s" "$(NODE_PREFIX)/include/node"; \
	elif [ -n "$(NODE_BIN)" ]; then \
		node_prefix=$$(cd "$$(dirname "$(NODE_BIN)")/.." && pwd); \
		for cand in \
			"$$node_prefix/include/node" \
			"/usr/include/node" \
			"/usr/local/include/node"; do \
			if [ -f "$$cand/node_api.h" ]; then \
				printf "%s" "$$cand"; \
				break; \
			fi; \
		done; \
	fi)
PKG_CONFIG ?= pkg-config

ifeq ($(NODE_INCLUDES),)
$(error Could not locate node_api.h. Set NODE_INCLUDES or NODE_PREFIX.)
endif

CC ?= cc
FFI_CFLAGS := $(shell $(PKG_CONFIG) --cflags libffi)
FFI_LIBS := $(shell $(PKG_CONFIG) --libs libffi)

CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -fPIC -pthread
CPPFLAGS ?= -DBUILDING_NODE_EXTENSION -I$(NODE_INCLUDES) $(FFI_CFLAGS)
LDFLAGS ?= -shared -fPIC -pthread
LDLIBS ?= $(FFI_LIBS) -ldl -lm

SRCS = src/disturb_napi.c
OBJS = $(patsubst src/%.c, build/%.o, $(SRCS))
TARGET = build/disturb.node

.PHONY: all clean test

all: $(TARGET)

$(AMALGAMATED_HEADER) $(AMALGAMATED_CLI): $(AMALGAMATE_INPUTS)
	$(AMALGAMATE)

build/%.o: src/%.c $(AMALGAMATED_HEADER) | build/
	@echo "[CC] $<"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	@echo "[LD] $(TARGET)"
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/:
	mkdir -p build

clean:
	rm -rf build/

test: all
	@echo "=== hello ===" && $(NODE) examples/hello.js
	@echo "=== memory ===" && $(NODE) examples/memory.js
	@echo "=== view ===" && $(NODE) examples/view.js
	@echo "=== callback ===" && $(NODE) examples/callback.js
	@echo "=== meta_fields ===" && $(NODE) examples/meta_fields.js
	@echo "=== memory_utils ===" && $(NODE) examples/memory_utils.js
	@echo "=== sym_self ===" && $(NODE) examples/sym_self.js
