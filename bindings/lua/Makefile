LUA ?= lua5.4
LUA_PREFIX ?=
ROOT := $(abspath ../..)
DIST_DIR := $(ROOT)/dist
AMALGAMATE := $(ROOT)/tools/amalgamate.sh
AMALGAMATED_HEADER := $(DIST_DIR)/urbc.h
AMALGAMATED_CLI := $(DIST_DIR)/urbccli.c
AMALGAMATE_INPUTS := $(AMALGAMATE) $(ROOT)/include/urb.h $(ROOT)/src/urbc_internal.h $(wildcard $(ROOT)/include/urbc/*.h) $(wildcard $(ROOT)/src/*.c)
LUA_BIN := $(shell command -v $(LUA) 2>/dev/null)
PKG_CONFIG ?= pkg-config

LUA_PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags lua5.4 2>/dev/null || $(PKG_CONFIG) --cflags lua-5.4 2>/dev/null || true)
LUA_PKG_LIBS := $(shell $(PKG_CONFIG) --libs lua5.4 2>/dev/null || $(PKG_CONFIG) --libs lua-5.4 2>/dev/null || true)
LUA_INCLUDES ?= $(shell \
	if [ -n "$(LUA_PREFIX)" ]; then \
		for cand in \
			"$(LUA_PREFIX)/include/lua5.4" \
			"$(LUA_PREFIX)/include/lua-5.4" \
			"$(LUA_PREFIX)/include"; do \
			if [ -f "$$cand/lua.h" ]; then \
				printf "%s" "$$cand"; \
				break; \
			fi; \
		done; \
	else \
		for cand in \
			"/usr/include/lua5.4" \
			"/usr/local/include/lua5.4" \
			"/usr/include/lua-5.4" \
			"/usr/local/include/lua-5.4" \
			"/opt/homebrew/include/lua5.4" \
			"/opt/homebrew/include/lua-5.4"; do \
			if [ -f "$$cand/lua.h" ]; then \
				printf "%s" "$$cand"; \
				break; \
			fi; \
		done; \
	fi)

ifeq ($(strip $(LUA_PKG_CFLAGS)),)
ifeq ($(strip $(LUA_INCLUDES)),)
$(error Could not locate lua.h. Set LUA_PREFIX, LUA_INCLUDES, or LUA_PKG_CFLAGS.)
endif
LUA_CFLAGS := -I$(LUA_INCLUDES)
else
LUA_CFLAGS := $(LUA_PKG_CFLAGS)
endif

LUA_LIBS ?= $(if $(strip $(LUA_PKG_LIBS)),$(LUA_PKG_LIBS),-llua5.4)

CC ?= cc
FFI_CFLAGS := $(shell $(PKG_CONFIG) --cflags libffi 2>/dev/null)
FFI_LIBS := $(shell $(PKG_CONFIG) --libs libffi 2>/dev/null)
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -fPIC -pthread
CPPFLAGS ?= $(LUA_CFLAGS) -I$(ROOT)/bindings/node/src $(FFI_CFLAGS)
LDFLAGS ?= -shared -fPIC -pthread
LDLIBS ?= $(LUA_LIBS) $(FFI_LIBS) -ldl -lm

TARGET = $(DIST_DIR)/urb_ffi_native.so
OBJS = $(DIST_DIR)/urb_ffi_lua.o

.PHONY: all clean test

all: $(TARGET)

$(AMALGAMATED_HEADER) $(AMALGAMATED_CLI): $(AMALGAMATE_INPUTS)
	$(AMALGAMATE)

$(DIST_DIR)/urb_ffi_lua.o: src/urb_ffi_lua.c $(AMALGAMATED_HEADER) | $(DIST_DIR)/
	@echo "[CC] $<"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	@echo "[LD] $(TARGET)"
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(DIST_DIR)/:
	mkdir -p $(DIST_DIR)

clean:
	rm -f $(OBJS) $(TARGET)

test: all
	@if [ -z "$(LUA_BIN)" ]; then \
		echo "lua5.4 interpreter not found in PATH" >&2; \
		exit 1; \
	fi
	@echo "=== lua hello ===" && $(LUA_BIN) examples/hello.lua
	@echo "=== lua manual_types ===" && $(LUA_BIN) examples/manual_types.lua
	@echo "=== lua memory ===" && $(LUA_BIN) examples/memory.lua
	@echo "=== lua view ===" && $(LUA_BIN) examples/view.lua
	@echo "=== lua callback ===" && $(LUA_BIN) examples/callback.lua
	@echo "=== lua sym_self ===" && $(LUA_BIN) examples/sym_self.lua
