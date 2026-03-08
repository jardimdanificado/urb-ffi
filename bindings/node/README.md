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

## Windows

The Node package is prepared for Windows builds, but `libffi` must be available.

Set these environment variables before install when building on Windows:

- `LIBFFI_INCLUDE_DIR`
- `LIBFFI_LIB_DIR`
- optionally `LIBFFI_LIB_NAME` (defaults to `ffi`)

Alternatively, provide `LIBFFI_LIBS` directly.

Examples and the full test suite are Linux-oriented. On Windows, `npm test` runs a smaller portable smoke test.