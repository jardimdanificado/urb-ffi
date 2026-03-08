CC ?= cc
AR ?= ar
NODE ?= node
LUA ?= lua5.4
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS ?= -Iinclude

DIST_DIR := $(CURDIR)/dist
NODE_MODULE_DIR := $(DIST_DIR)/node
LUA_MODULE_DIR := $(DIST_DIR)/lua

SRCS = \
	src/urbc_format.c \
	src/urbc_platform.c \
	src/urbc_api.c \
	src/urbc_ffi_sig.c \
	src/urbc_loader.c \
	src/urbc_schema.c \
	src/urbc_runtime.c \
	src/urbc_ops_core.c \
	src/urbc_ops_mem.c \
	src/urbc_ops_schema.c \
	src/urbc_ops_ffi.c

OBJS = $(SRCS:src/%.c=build/%.o)
TARGET = build/liburbc.a

.PHONY: all clean node lua modules

all: $(TARGET)

modules: node lua

build:
	mkdir -p build

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -rf build
	rm -rf $(NODE_MODULE_DIR) $(LUA_MODULE_DIR)

node:
	@echo "[BUILD] node addon"
	$(NODE) ./bindings/node/scripts/build.js
	@echo "[PKG] $(NODE_MODULE_DIR)"
	rm -rf $(NODE_MODULE_DIR)
	mkdir -p $(NODE_MODULE_DIR)/dist
	cp bindings/node/index.js $(NODE_MODULE_DIR)/index.js
	cp bindings/node/README.md $(NODE_MODULE_DIR)/README.md
	cp bindings/node/build/Release/urb_ffi.node $(NODE_MODULE_DIR)/dist/urb-ffi.node
	$(NODE) -e "const fs=require('fs'); const pkg=require('./package.json'); fs.writeFileSync('$(NODE_MODULE_DIR)/package.json', JSON.stringify({name:pkg.name, version:pkg.version, description:pkg.description, author:pkg.author, main:'./index.js', exports:{'.':'./index.js'}, engines:pkg.engines}, null, 2)+'\\n');"

lua:
	@echo "[BUILD] lua module"
	rm -rf $(LUA_MODULE_DIR)
	mkdir -p $(LUA_MODULE_DIR)
	$(MAKE) -C bindings/lua LUA=$(LUA) DIST_DIR=$(LUA_MODULE_DIR) clean all
	cp bindings/lua/urb_ffi.lua $(LUA_MODULE_DIR)/urb_ffi.lua
	cp bindings/lua/README.md $(LUA_MODULE_DIR)/README.md
	rm -f $(LUA_MODULE_DIR)/*.o $(LUA_MODULE_DIR)/urbc.h $(LUA_MODULE_DIR)/urbccli.c
