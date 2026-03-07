CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
CPPFLAGS ?= -Iinclude

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

.PHONY: all clean

all: $(TARGET)

build:
	mkdir -p build

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -rf build
