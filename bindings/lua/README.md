# urb-ffi Lua 5.4 binding

This binding targets Lua 5.4 and builds a native module at [dist/urb_ffi_native.so](../../dist/urb_ffi_native.so).

## Build

From the repository root:

- `make -C bindings/lua`
- `make -C bindings/lua test`

If Lua 5.4 headers are not in a standard path, set one of:

- `LUA_PREFIX=/path/to/lua`
- `LUA_INCLUDES=/path/to/lua/include`
- `LUA_CFLAGS='-I/path/to/lua/include'`
- `LUA_LIBS='-L/path/to/lua/lib -llua5.4'`

## Module layout

- [bindings/lua/urb_ffi.lua](urb_ffi.lua): high-level Lua wrapper
- [bindings/lua/src/urb_ffi_lua.c](src/urb_ffi_lua.c): native Lua C module
- [bindings/lua/examples](examples): usage examples

## API

The module returns:

- `ffi`
- `memory`

The surface is intentionally close to the Node binding.

## Schema format

Lua tables do not preserve object field order reliably, so schemas use an ordered array form:

```lua
local Point = {
    { name = "x", type = "i32" },
    { name = "y", type = "i32" },
    { name = "value", type = "f64" },
    { name = "flags", type = "u64" },
}
```

Supported field descriptors:

- primitive: `{ name = "x", type = "i32" }`
- fixed array: `{ name = "bytes", type = "u8", count = 16 }`
- pointer: `{ name = "next", pointer = true }`
- nested struct/union: `{ name = "pos", schema = OtherSchema }`
- union schema: add `__union = true` on the schema table
