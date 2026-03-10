NODE ?= node
NODE_PREFIX ?=
ROOT := $(abspath ../..)
DIST_DIR := $(CURDIR)/dist
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
CPPFLAGS ?= -DBUILDING_NODE_EXTENSION -I$(NODE_INCLUDES) -I$(ROOT)/include -I$(ROOT)/src -I$(CURDIR)/src $(FFI_CFLAGS)
LDFLAGS ?= -shared -fPIC -pthread
LDLIBS ?= $(FFI_LIBS) -ldl -lm

LOCAL_SRCS = \
	$(CURDIR)/src/urb_ffi_napi.c
CORE_SRCS = \
	$(ROOT)/src/urbc_format.c \
	$(ROOT)/src/urbc_platform.c \
	$(ROOT)/src/urbc_api.c \
	$(ROOT)/src/urbc_ffi_sig.c \
	$(ROOT)/src/urbc_loader.c \
	$(ROOT)/src/urbc_schema.c \
	$(ROOT)/src/urbc_runtime.c \
	$(ROOT)/src/urbc_ops_core.c \
	$(ROOT)/src/urbc_ops_mem.c \
	$(ROOT)/src/urbc_ops_schema.c \
	$(ROOT)/src/urbc_ops_ffi.c
COMMON_DEPS = \
	$(ROOT)/include/urbc.h \
	$(ROOT)/include/urb.h \
	$(wildcard $(ROOT)/include/urbc/*.h) \
	$(ROOT)/src/urbc_internal.h
OBJS = \
	$(patsubst $(CURDIR)/src/%.c,$(DIST_DIR)/%.o,$(LOCAL_SRCS)) \
	$(patsubst $(ROOT)/src/%.c,$(DIST_DIR)/core_%.o,$(CORE_SRCS))
TARGET = $(DIST_DIR)/urb-ffi.node

.PHONY: all clean test

all: $(TARGET)

$(DIST_DIR)/%.o: $(CURDIR)/src/%.c $(COMMON_DEPS) | $(DIST_DIR)/
	@echo "[CC] $<"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(DIST_DIR)/core_%.o: $(ROOT)/src/%.c $(COMMON_DEPS) | $(DIST_DIR)/
	@echo "[CC] $<"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	@echo "[LD] $(TARGET)"
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(DIST_DIR)/:
	mkdir -p $(DIST_DIR)

clean:
	rm -rf $(DIST_DIR)

test: all
	@echo "=== hello ===" && $(NODE) examples/hello.js
	@echo "=== manual_types ===" && $(NODE) examples/manual_types.js
	@echo "=== memory ===" && $(NODE) examples/memory.js
	@echo "=== view ===" && $(NODE) examples/view.js
	@echo "=== callback ===" && $(NODE) examples/callback.js
	@echo "=== meta_fields ===" && $(NODE) examples/meta_fields.js
	@echo "=== memory_utils ===" && $(NODE) examples/memory_utils.js
	@echo "=== sym_self ===" && $(NODE) examples/sym_self.js
