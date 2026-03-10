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
- function-pointer parameters and returns in generated rich descriptors
- fixed primitive arrays in record fields
- nested structs/unions by value
- record and enum typedef aliases
- variadic function declarations

## Current limitations

- no macro constant extraction yet
- no bit-field schema generation
- no anonymous field flattening
- no VLA / incomplete array schema generation
- raw string `urb-ffi` FFI signatures still do not encode by-value record arguments or returns
- generated Node/Lua wrappers now bind schema-compatible non-variadic by-value record calls directly through rich `ffi.type.*` descriptors
- no `long double` / 128-bit float lowering
- plain generated record schemas still expose function-pointer fields as generic pointers

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

If a generated Node wrapper ever needs a sidecar C shim, it is written next to the wrapper, for example:

- `stdio.shim.c`
- `stdio.shim.so` (or platform equivalent)

For schema-compatible non-variadic by-value records, current generated wrappers bind those functions directly and no shim sidecar is needed.

If a future wrapper path still needs a shim, `urb-bindgen` can also compile that shim shared library automatically.

If a shim is emitted and you only want the C source without the automatic compile step, pass:

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

Like the Node emitter, the Lua emitter binds schema-compatible non-variadic by-value record calls directly and only falls back to a sidecar shim if a generated wrapper path explicitly needs one.

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
- richer type metadata including function-pointer pointee information
- wrapper support status (`direct`, `shim`, or unsupported)

### Node

The Node emitter generates a wrapper that exports:

- `load(runtime, libraryPath)`
- `enums`
- `records`
- `signatures`
- `unsupportedFunctions`

`load()` binds callable functions against the selected library through `urb-ffi`. Schema-compatible non-variadic by-value record functions use rich `ffi.type.*` descriptors directly, without a shim sidecar.

### Lua

The Lua emitter generates a wrapper that exports:

- `load(urb, library_path)`
- `enums`
- `records`
- `signatures`
- `unsupported_functions`

`load()` follows the same model as the Node wrapper: direct calls use the target library, and schema-compatible non-variadic by-value record calls bind directly through rich descriptors.

## Intended next steps

Useful next iterations are:

1. macro constant extraction
2. header allow/deny filters
3. richer typedef export
4. direct JSON-to-wrapper secondary emitters
5. richer generated record schemas for function-pointer fields
