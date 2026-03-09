# urb-bindgen

`urb-bindgen` is a standalone libclang-based binding generator for `urb-ffi`.

It lives under [bindgen](.) on purpose so the main runtime stays free from a hard `libclang` dependency.

## Current scope

The initial implementation parses one or more C headers and emits one of:

- JSON metadata
- Node.js wrapper source
- Lua wrapper source

The generator currently extracts:

- exported functions
- structs and unions
- enums
- typedef aliases for records and enums

It maps declarations into the current `urb-ffi` feature set.

## What it handles well

- primitive integer and floating-point types supported by `urb-ffi`
- pointers and `char *`/`const char *`
- fixed primitive arrays in record fields
- nested structs/unions by value
- record and enum typedef aliases
- variadic function declarations

## Current limitations

- no macro constant extraction yet
- no bit-field schema generation
- no anonymous field flattening
- no VLA / incomplete array schema generation
- direct `urb-ffi` FFI signatures still do not encode by-value record arguments or returns
- Node/Lua wrappers only auto-recover by-value record calls when the records are schema-compatible and the function is not variadic
- no `long double` / 128-bit float lowering
- function pointers are treated as generic pointers in generated signatures

## Build

The subproject expects libclang headers and a libclang shared/static library.

At runtime, `urb-bindgen` also tries to discover the host C standard include paths from `$CC` (or `cc` by default), so normal headers that include `stddef.h`, `stdbool.h`, `stdarg.h`, and similar should work without extra flags.

### With the local Makefile

From [bindgen](.):

```bash
make \
  LIBCLANG_INCLUDE_DIR=/path/to/llvm/include \
  LIBCLANG_LIBRARY=/path/to/libclang.so
```

Useful targets:

- `make`
- `make configure`
- `make build`
- `make clean`
- `make rebuild`
- `make run RUN_ARGS='--header ./foo.h --emit json'`

Typical configuration:

```bash
cmake -S bindgen -B bindgen/build \
  -DLIBCLANG_INCLUDE_DIR=/path/to/llvm/include \
  -DLIBCLANG_LIBRARY=/path/to/libclang.so
cmake --build bindgen/build
```

The resulting executable is:

- [bindgen/build/urb-bindgen](build/urb-bindgen)

## Usage

### Emit JSON metadata

```bash
bindgen/build/urb-bindgen \
  --header /usr/include/stdio.h \
  --emit json \
  --library libc.so.6 \
  --output stdio.urb.json
```

### Emit a Node wrapper

```bash
bindgen/build/urb-bindgen \
  --header /usr/include/stdio.h \
  --emit node \
  --library libc.so.6 \
  --output stdio.js
```

When the header contains functions that pass structs/unions by value, the Node emitter also writes a sidecar C shim next to the wrapper, for example:

- `stdio.shim.c`
- `stdio.shim.so` (or platform equivalent)

By default, `urb-bindgen` now also compiles that shim shared library automatically.

If you only want the C source and do not want the automatic compile step, pass:

```bash
bindgen/build/urb-bindgen \
  --header /usr/include/stdio.h \
  --emit node \
  --library libc.so.6 \
  --output stdio.js \
  --no-build-shim
```

### Emit a Lua wrapper

```bash
bindgen/build/urb-bindgen \
  --header /usr/include/stdio.h \
  --emit lua \
  --library libc.so.6 \
  --output stdio.lua
```

Like the Node emitter, the Lua emitter also writes and auto-builds a `.shim.c` sidecar when by-value record adapters are needed.

### Extra include paths / defines

```bash
bindgen/build/urb-bindgen \
  --header ./vendor/foo.h \
  --include ./vendor/include \
  --define FOO_USE_LEGACY=1 \
  --clang-arg -target \
  --clang-arg x86_64-unknown-linux-gnu \
  --emit json
```

If automatic standard include discovery is not enough, extra system include paths can still be passed manually, for example:

```bash
./build/urb-bindgen \
  --header ./raylib/include/raylib.h \
  --include ./raylib/include \
  --clang-arg -isystem \
  --clang-arg /usr/lib/gcc/x86_64-unknown-linux-gnu/15.2.0/include \
  --clang-arg -isystem \
  --clang-arg /usr/include \
  --emit json
```

## Output model

### JSON

The JSON output is the canonical IR-like export for now. It includes:

- module name
- default library
- input headers
- records with schema support status
- enums
- functions with generated `urb-ffi` signatures when possible
- wrapper support status (`direct`, `shim`, or unsupported)

### Node

The Node emitter generates a wrapper that exports:

- `load(runtime, libraryPath)`
- `enums`
- `records`
- `signatures`
- `unsupportedFunctions`

`load()` binds direct functions against the selected library through `urb-ffi` and uses the generated shim library for by-value record functions when needed.

### Lua

The Lua emitter generates a wrapper that exports:

- `load(urb, library_path)`
- `enums`
- `records`
- `signatures`
- `unsupported_functions`

`load()` follows the same model as the Node wrapper: direct calls use the target library, while by-value record calls go through the generated shim library.

## Intended next steps

Useful next iterations are:

1. macro constant extraction
2. function-pointer metadata for callbacks
3. header allow/deny filters
4. richer typedef export
5. direct JSON-to-wrapper secondary emitters
