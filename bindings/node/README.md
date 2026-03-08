# urb-ffi for Node.js

This directory is the publishable npm package for `urb-ffi`.

## Publish source

Publish from [bindings/node](bindings/node):

- `cd bindings/node`
- `npm publish`

## Package layout

The package is self-contained at publish time and builds from source:

- [index.js](index.js): public JavaScript API
- [src](src): native binding sources
- [../../include](../../include): public C headers shipped in the npm tarball
- [../../src](../../src): C implementation sources shipped in the npm tarball

The `.node` binary is built on install and is not published in the tarball.