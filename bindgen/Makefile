CMAKE ?= cmake
BUILD_DIR ?= $(CURDIR)/build
CONFIG ?= Release
TARGET ?= urb-bindgen

LIBCLANG_INCLUDE_DIR ?=
LIBCLANG_LIBRARY ?=
CMAKE_ARGS ?=
BUILD_ARGS ?=
RUN_ARGS ?=

CONFIGURE_ARGS := -S $(CURDIR) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG)

ifneq ($(strip $(LIBCLANG_INCLUDE_DIR)),)
CONFIGURE_ARGS += -DLIBCLANG_INCLUDE_DIR=$(LIBCLANG_INCLUDE_DIR)
endif

ifneq ($(strip $(LIBCLANG_LIBRARY)),)
CONFIGURE_ARGS += -DLIBCLANG_LIBRARY=$(LIBCLANG_LIBRARY)
endif

ifneq ($(strip $(CMAKE_ARGS)),)
CONFIGURE_ARGS += $(CMAKE_ARGS)
endif

.PHONY: all configure build clean rebuild run help

all: build

configure:
	$(CMAKE) $(CONFIGURE_ARGS)

build: configure
	$(CMAKE) --build $(BUILD_DIR) --config $(CONFIG) $(BUILD_ARGS)

run: build
	$(BUILD_DIR)/$(TARGET) $(RUN_ARGS)

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean build

help:
	@echo "urb-bindgen Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  make              Build urb-bindgen"
	@echo "  make configure    Configure CMake in bindgen/build"
	@echo "  make build        Configure and build"
	@echo "  make run          Build and run with RUN_ARGS='...'"
	@echo "  make clean        Remove bindgen/build"
	@echo "  make rebuild      Clean and build again"
	@echo ""
	@echo "Variables:"
	@echo "  LIBCLANG_INCLUDE_DIR=/path/to/llvm/include"
	@echo "  LIBCLANG_LIBRARY=/path/to/libclang.so"
	@echo "  CONFIG=Release|Debug"
	@echo "  CMAKE_ARGS='...extra cmake cache args...'"
	@echo "  BUILD_ARGS='...extra cmake --build args...'"
	@echo "  RUN_ARGS='...bindgen cli args...'"
