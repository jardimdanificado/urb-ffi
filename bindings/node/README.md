# urb-ffi for Node.js

This directory is the publishable npm package for `urb-ffi`.

## Publish source

Publish from [bindings/node](bindings/node):

- `cd bindings/node`
- `npm publish`

## Package layout

The package is self-contained at publish time:

- [index.js](index.js): public JavaScript API
- [src](src): native binding sources
- [dist/urbc.h](dist/urbc.h): amalgamated C library header used to build the addon
- [dist/urbccli.c](dist/urbccli.c): generated CLI source artifact shipped with the package

The `.node` binary is built on install and is not published in the tarball.

## Repository development

Inside the repository, refresh the packaged amalgamated files with:

- `npm run sync-dist`

That command regenerates the root amalgamation and copies the needed files into [dist](dist).