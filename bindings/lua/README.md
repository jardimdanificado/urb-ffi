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

## By-value descriptors

`ffi.type.func(...)` now supports complex by-value arguments/returns (for example
struct/union/array layouts) through descriptor fallback in the native bridge.

Rich callbacks also adapt by-value record/array args, schema-compatible complex
returns, and function-pointer callback args. Callbacks are creator-thread only.

See:

- [bindings/lua/examples/byvalue.c](examples/byvalue.c)
- [bindings/lua/examples/byvalue.lua](examples/byvalue.lua)

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
- typed pointer: `{ name = "next", type = "pointer", to = OtherSchema }`
- enum field: `{ name = "mode", type = ffi.type.enum({ Idle = 0, Run = 1 }) }`
- function pointer field: `{ name = "cb", type = ffi.type.func(ffi.type.i32(), { ffi.type.i32() }) }`
- flexible array member: `{ name = "bytes", type = "u8", flexible = true }`
- nested struct/union: `{ name = "pos", schema = OtherSchema }`
- union schema: add `__union = true` on the schema table

## Memory/view model

The Lua wrapper now mirrors the richer Node-side memory model.

- enum fields in views
- function-pointer fields as callable values
- typed pointer fields with `deref()`, `read()`, `view()`, and `write()`
- direct nested-struct assignment from Lua tables
- flexible array members on the last field
- `memory.view(ptr, schema, total_size)` for flexible-array access

See [bindings/lua/examples/memory_phase6.lua](examples/memory_phase6.lua).
