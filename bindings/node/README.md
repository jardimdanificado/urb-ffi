# urb-ffi for Node.js

This directory contains the Node.js implementation for `urb-ffi`.

## Package layout

The package is self-contained at publish time and builds from source:

- [index.js](index.js): public JavaScript API
- [src](src): native binding sources
- [../../include](../../include): public C headers shipped in the npm tarball
- [../../src](../../src): C implementation sources shipped in the npm tarball

The `.node` binary is built on install and is not published in the tarball.

## Build paths

- `npm install` / `npm run build`: cross-platform build via `node-gyp`
- `make -C bindings/node`: local Unix-oriented development build

The JS loader accepts both outputs:

- [dist/urb-ffi.node](dist/urb-ffi.node)
- [build/Release/urb_ffi.node](build/Release/urb_ffi.node)

## By-value descriptors

The Node binding supports rich host descriptors (`ffi.type.*`) for by-value
struct/array arguments and returns.

- Native descriptor construction uses `describeDescriptor` under the hood.
- Dynamic libffi graphs are attached to descriptors and consumed by bind/callback.
- Rich callbacks also adapt by-value record/array args, schema-compatible complex returns, and function-pointer callback args.
- Callbacks are creator-thread only.
- See [examples/byvalue.js](examples/byvalue.js) with its companion C helper
	[examples/byvalue.c](examples/byvalue.c).

## Memory/view model

The Node wrapper also supports richer schema-driven memory views.

- enum fields via `ffi.type.enum(...)`
- function-pointer fields via `ffi.type.func(...)`
- typed pointer fields with `{ type: 'pointer', to: ... }`
- whole nested-struct assignment
- flexible array members as the last field via `{ type: 'u8', flexible: true }`
- `memory.view(ptr, schema, totalSize)` for flexible-array access

See [examples/memory_phase6.js](examples/memory_phase6.js).

## Windows

The Node package is prepared for Windows builds, but `libffi` must be available.

Set these environment variables before install when building on Windows:

- `LIBFFI_INCLUDE_DIR`
- `LIBFFI_LIB_DIR`
- optionally `LIBFFI_LIB_NAME` (defaults to `ffi`)

Alternatively, provide `LIBFFI_LIBS` directly.

Examples and the full test suite are Linux-oriented. On Windows, `npm test` runs a smaller portable smoke test.