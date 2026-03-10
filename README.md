# urb-ffi

`urb-ffi` is a binding-first FFI toolkit built on top of the `urbc` runtime.

The current project is centered on two host bindings:

- **Node.js**: a Node-API addon plus a JavaScript compatibility layer.
- **Lua 5.4**: a native C module plus a high-level Lua wrapper.

If the goal is to call native code, exchange raw pointers, inspect C layouts, or expose host callbacks back to C, this is what `urb-ffi` is for.

## What urb-ffi can do

`urb-ffi` covers the full flow of native interop:

1. **Open shared libraries** with `dlopen`-style flags.
2. **Resolve symbols** from a library or from the current process.
3. **Bind C function signatures** and call them from Node or Lua.
4. **Call variadic C functions**.
5. **Create native callbacks** so C code can call back into JavaScript or Lua.
6. **Read and write raw memory** with typed helpers.
7. **Allocate, resize, clear, copy, compare, and free native buffers**.
8. **Read and write C strings**.
9. **Read and write pointers directly**.
10. **Marshal arrays of primitive values**.
11. **Describe C structs and unions in the host language**.
12. **Compute `sizeof` and `offsetof` from host-side schemas**.
13. **Create live struct views over native memory**.
14. **Create array-of-struct views**.
15. **Handle nested structs, unions, fixed arrays, and pointer fields**.
16. **Inspect `errno` and the last dynamic-loader error**.

## Runtime status

- **Node.js**: primary packaged binding, `node >= 12.17`.
- **Lua 5.4**: native binding and examples included in the repository.
- **Windows**: Node build path is prepared through `node-gyp`; Lua examples and packaging are still Unix-oriented.

## Repository layout

- [bindings/node](bindings/node): Node binding
- [bindings/lua](bindings/lua): Lua 5.4 binding
- [include](include): public C headers used by the bindings
- [src](src): `urbc` implementation used by the bindings
- [docs/BYTECODE.md](docs/BYTECODE.md): bytecode/runtime documentation

## Build and package targets

From the repository root:

- `npm install` or `npm run build`: build the Node addon
- `npm test`: run the Node examples/tests
- `make node`: assemble a self-contained Node module folder in `dist/node`
- `make lua`: assemble a self-contained Lua module folder in `dist/lua`
- `make modules`: build both packaged binding folders

The generated folders are intended to look like distributable binding packages:

- `dist/node`
- `dist/lua`

## Installing the bindings

### Node.js

From the repository root:

```bash
npm install
```

That builds the native addon from source.

Requirements:

- Node.js `>= 12.17`
- a C toolchain
- `libffi`

On Linux and macOS, `libffi` is usually discovered with `pkg-config`.

On Windows, set these before install if `libffi` is not auto-discovered:

- `LIBFFI_INCLUDE_DIR`
- `LIBFFI_LIB_DIR`
- optional `LIBFFI_LIB_NAME`
- or `LIBFFI_LIBS`

### Lua 5.4

From the repository root:

```bash
make lua
```

Or build directly inside the binding folder:

```bash
make -C bindings/lua
```

If Lua headers and libraries are not in a standard location, the Lua binding accepts:

- `LUA_PREFIX=/path/to/lua`
- `LUA_INCLUDES=/path/to/lua/include`
- `LUA_CFLAGS='-I/path/to/lua/include'`
- `LUA_LIBS='-L/path/to/lua/lib -llua5.4'`

## Quick start

### Node.js: open a library, bind functions, call them

```js
const { ffi } = require('urb-ffi');

const libcName = process.platform === 'darwin'
  ? '/usr/lib/libSystem.B.dylib'
  : process.platform === 'win32'
    ? 'ucrtbase.dll'
    : 'libc.so.6';

const libc = ffi.open(libcName, ffi.flags.NOW | ffi.flags.LOCAL);
const puts = ffi.bind(ffi.sym(libc, 'puts'), 'i32 puts(cstring)');
const getenv = ffi.bind(ffi.sym(libc, 'getenv'), 'cstring getenv(cstring)');

puts('hello from urb-ffi');
console.log('HOME =', getenv('HOME'));

ffi.close(libc);
```

### Lua 5.4: call `qsort` with a Lua callback

```lua
local urb = require('urb_ffi')
local ffi, mem = urb.ffi, urb.memory

local libc = ffi.open('libc.so.6')
local qsort = ffi.bind(ffi.sym(libc, 'qsort'), 'void qsort(pointer, u64, u64, pointer)')

local nums = { 42, 7, 99, -3, 15 }
local buf = mem.alloc(#nums * 4)
for i = 1, #nums do
    mem.writei32(buf + (i - 1) * 4, nums[i])
end

local cmp = ffi.callback('i32 cmp(pointer, pointer)', function(a, b)
    local va = mem.readi32(a)
    local vb = mem.readi32(b)
    if va < vb then return -1 end
    if va > vb then return 1 end
    return 0
end)

qsort(buf, #nums, 4, cmp.ptr)

for i = 1, #nums do
    print(mem.readi32(buf + (i - 1) * 4))
end

mem.free(buf)
ffi.close(libc)
```

## Binding model

Both bindings expose the same two top-level namespaces:

- `ffi`
- `memory`

The naming is intentionally close across runtimes. Lua also provides camelCase aliases for the helpers that naturally use snake_case there.

## `ffi`: native function interop

### `ffi.flags`

Dynamic loader flags exported by the runtime:

- `LAZY`
- `NOW`
- `LOCAL`
- `GLOBAL`
- `NODELETE`
- `NOLOAD`

Use them with `ffi.open(path, flags)`.

### `ffi.open(path[, flags])`

Opens a shared library and returns a handle.

Node example:

```js
const { ffi } = require('urb-ffi');
const libc = ffi.open('libc.so.6', ffi.flags.NOW | ffi.flags.LOCAL);
```

### `ffi.close(handle)`

Closes a library handle.

### `ffi.sym(handle, name)`

Looks up a symbol inside a loaded library and returns its address.

### `ffi.sym_self(name)`

Looks up a symbol in the current process without opening a library first.

Lua example:

```lua
local urb = require('urb_ffi')
local ffi = urb.ffi

local puts_desc = ffi.describe('i32 puts(cstring)')
local strlen_desc = ffi.describe('u64 strlen(cstring)')
local puts = ffi.bind(ffi.sym_self('puts'), puts_desc)
local strlen = ffi.bind(ffi.sym_self('strlen'), strlen_desc)

puts('hello world from sym_self')
print(strlen('hello world from sym_self'))
```

### `ffi.describe(signature)`

Parses a signature string once and returns a reusable descriptor object.

### `ffi.bind(ptr, descriptor)`

Binds a native function pointer to a callable host object.

- In Node, the return value is a JavaScript function.
- In Lua, the return value is a callable object.

Signature format:

```text
return_type function_name(arg0, arg1, arg2)
```

Examples:

- `i32 puts(cstring)`
- `cstring getenv(cstring)`
- `void qsort(pointer, u64, u64, pointer)`
- `i32 snprintf(pointer, u64, cstring, ...)`

Supported FFI base types:

- `void`
- `bool`
- `i8`, `u8`
- `i16`, `u16`
- `i32`, `u32`
- `i64`, `u64`
- `f32`, `f64`
- `pointer`
- `cstring`

Accepted aliases in the high-level wrappers include:

- `boolean` → `bool`
- `int8`, `uint8`, `byte`
- `int16`, `uint16`
- `int32`, `uint32`, `int`, `uint`
- `int64`, `uint64`, `long`, `ulong`
- `float32`, `float`, `float64`, `double`
- `ptr` → `pointer`
- `string` → `cstring`

### Variadic functions

Variadic signatures use `...` as the last argument.

Node example:

```js
const { ffi, memory: mem } = require('urb-ffi');

const libc = ffi.open('libc.so.6');
const snprintfDesc = ffi.describe('i32 snprintf(pointer, u64, cstring, ...)');
const snprintf = ffi.bind(
  ffi.sym(libc, 'snprintf'),
  snprintfDesc
);

const buf = mem.alloc(64n);
snprintf(buf, 64n, 'answer=%d pi=%.2f', 42, Math.PI);
console.log(mem.readcstring(buf));

mem.free(buf);
ffi.close(libc);
```

For variadic arguments, the runtime infers a suitable native type from the host value:

- booleans → `bool`
- strings → `cstring`
- integers → `i32`, `i64`, or `u64` depending on range/runtime
- floating-point values → `f64`
- null/pointer-like values → `pointer`

### `ffi.callback(descriptor, fn)`

Creates a real C-callable function pointer backed by a JavaScript or Lua function.

With rich descriptors from `ffi.type.func(...)`, callback adapters now also:

- expose by-value `struct` / `union` / fixed-array arguments as host-side views
- accept schema-compatible by-value record/array return values from the host callback
- expose function-pointer callback arguments as callable bound functions

The returned object contains at least:

- `ptr`: the native function pointer to pass back into C

It also owns internal resources, so **keep the callback object alive for as long as C may call it**.

Current policy/limits:

- callbacks are only supported on the same thread that created them
- variadic callbacks are still unsupported

Node example:

```js
const { ffi, memory: mem } = require('urb-ffi');

const libc = ffi.open('libc.so.6');
const qsortDesc = ffi.describe('void qsort(pointer, u64, u64, pointer)');
const cmpDesc = ffi.describe('i32 cmp(pointer, pointer)');
const qsort = ffi.bind(ffi.sym(libc, 'qsort'), qsortDesc);

const buf = mem.alloc(5n * 4n);
mem.writeArray(buf, 'i32', [42, 7, 99, -3, 15]);

const cmp = ffi.callback(cmpDesc, (a, b) => {
  const va = mem.readi32(a);
  const vb = mem.readi32(b);
  return va < vb ? -1 : va > vb ? 1 : 0;
});

qsort(buf, 5n, 4n, cmp.ptr);
console.log(mem.readArray(buf, 'i32', 5));

mem.free(buf);
ffi.close(libc);
```

### `ffi.errno()`

Returns the current thread-local `errno` value after a native call.

### `ffi.dlerror()`

Returns the last dynamic-loader error string recorded by the runtime.

## `memory`: raw memory and C layout tools

The `memory` namespace covers raw allocation, typed reads and writes, string handling, arrays, and schema-driven views.

### Allocation and lifetime

Available in both bindings:

- `memory.alloc(size)`
- `memory.free(ptr)`
- `memory.realloc(ptr, size)`
- `memory.zero(ptr, size)`
- `memory.copy(dst, src, size)`
- `memory.set(ptr, byteValue, size)`
- `memory.compare(a, b, size)`
- `memory.nullptr()`
- `memory.sizeof_ptr()` in Node and Lua

Node example:

```js
const { memory: mem } = require('urb-ffi');

let p = mem.alloc(8n);
mem.writei32(p, 1);
mem.writei32(p + 4n, 2);

p = mem.realloc(p, 16n);
mem.writei32(p + 8n, 3);
mem.writei32(p + 12n, 4);

console.log(mem.readi32(p), mem.readi32(p + 4n), mem.readi32(p + 8n), mem.readi32(p + 12n));
mem.free(p);
```

### Typed primitive reads and writes

Available in both bindings:

- `readi8`, `readu8`
- `readi16`, `readu16`
- `readi32`, `readu32`
- `readi64`, `readu64`
- `readf32`, `readf64`
- `writei8`, `writeu8`
- `writei16`, `writeu16`
- `writei32`, `writeu32`
- `writei64`, `writeu64`
- `writef32`, `writef64`
- `readptr`, `writeptr`
- `readcstring`, `writecstring`

Lua example:

```lua
local mem = require('urb_ffi').memory

local p = mem.alloc(8)
mem.writei32(p, -7)
mem.writef32(p + 4, 3.5)

print(mem.readi32(p))
print(mem.readf32(p + 4))

mem.free(p)
```

### Convenience string allocation

- Node: `memory.allocStr(text)`
- Lua: `memory.alloc_str(text)` and `memory.allocStr(text)`

Node example:

```js
const { memory: mem } = require('urb-ffi');
const text = mem.allocStr('urb-ffi');
console.log(mem.readcstring(text));
mem.free(text);
```

### Primitive arrays

- Node: `memory.readArray(ptr, type, count)` and `memory.writeArray(ptr, type, values)`
- Lua: `memory.read_array`, `memory.write_array`, plus `readArray` and `writeArray` aliases

Lua example:

```lua
local mem = require('urb_ffi').memory

local p = mem.alloc(4 * 4)
mem.write_array(p, 'i32', { 10, 20, 30, 40 })

local values = mem.read_array(p, 'i32', 4)
for i = 1, #values do
    print(values[i])
end

mem.free(p)
```

## C layout schemas

One of the most useful parts of `urb-ffi` is that structs and unions can be described directly in the host language.

That unlocks:

- `memory.struct_sizeof(schema)`
- `memory.struct_offsetof(schema, fieldName)`
- `memory.view(ptr, schema)`
- `memory.viewArray(ptr, schema, count)` in Node
- `memory.view_array(ptr, schema, count)` in Lua, plus `viewArray`

### Node schema format

Node uses plain objects. Field order is the object insertion order.

```js
const Point = {
  x: 'i32',
  y: 'i32',
  value: 'f64',
  flags: 'u64',
};
```

Supported field forms in Node:

- primitive: `x: 'i32'`
- fixed array: `bytes: ['u8', 16]`
- pointer field: `next: { __pointer: true }`
- typed pointer field: `next: { type: 'pointer', to: OtherSchema }`
- enum field: `mode: ffi.type.enum({ Idle: 0, Run: 1 })`
- function pointer field: `cb: ffi.type.func(ffi.type.i32(), [ffi.type.i32()])`
- flexible array member: `bytes: { type: 'u8', flexible: true }`
- nested struct/union: `origin: OtherSchema`
- top-level or nested union: add `__union: true`
- explicit struct marker: `__struct: true`

### Lua schema format

Lua uses an ordered array form so field order is explicit.

```lua
local Point = {
    { name = 'x', type = 'i32' },
    { name = 'y', type = 'i32' },
    { name = 'value', type = 'f64' },
    { name = 'flags', type = 'u64' },
}
```

Supported field forms in Lua:

- primitive: `{ name = 'x', type = 'i32' }`
- fixed array: `{ name = 'bytes', type = 'u8', count = 16 }`
- pointer field: `{ name = 'next', pointer = true }`
- typed pointer field: `{ name = 'next', type = 'pointer', to = OtherSchema }`
- enum field: `{ name = 'mode', type = ffi.type.enum({ Idle = 0, Run = 1 }) }`
- function pointer field: `{ name = 'cb', type = ffi.type.func(ffi.type.i32(), { ffi.type.i32() }) }`
- flexible array member: `{ name = 'bytes', type = 'u8', flexible = true }`
- nested struct/union: `{ name = 'origin', schema = OtherSchema }`
- top-level or nested union: add `__union = true` on the schema table

### `memory.struct_sizeof(schema)` and `memory.struct_offsetof(schema, field)`

Lua example:

```lua
local mem = require('urb_ffi').memory

local Point = {
    { name = 'x', type = 'i32' },
    { name = 'y', type = 'i32' },
    { name = 'value', type = 'f64' },
    { name = 'flags', type = 'u64' },
}

print(mem.struct_sizeof(Point))
print(mem.struct_offsetof(Point, 'value'))
```

### `memory.view(ptr, schema[, totalSize])`

Creates a live host-side view over native memory.

- primitive fields are readable and writable
- typed pointer fields expose `{ ptr, isNull, deref(), read(), view(), write() }`
- function pointer fields are exposed as callable bound functions
- nested structs/unions are exposed as nested views
- fixed arrays are readable as host arrays/tables
- whole nested struct fields can be assigned from plain objects
- fixed and flexible array fields can be assigned from plain arrays
- use `totalSize` when the schema ends with a flexible array member

Node example:

```js
const { memory: mem } = require('urb-ffi');

const Vec3 = { x: 'f32', y: 'f32', z: 'f32' };
const Ray = {
  origin: Vec3,
  direction: Vec3,
};

const rayPtr = mem.alloc(BigInt(mem.struct_sizeof(Ray)));
mem.zero(rayPtr, BigInt(mem.struct_sizeof(Ray)));

const ray = mem.view(rayPtr, Ray);
ray.origin.x = 1;
ray.origin.y = 2;
ray.origin.z = 3;
ray.direction.y = 1;

console.log(ray.origin.x, ray.origin.y, ray.origin.z);
console.log(ray.direction.x, ray.direction.y, ray.direction.z);

mem.free(rayPtr);
```

Advanced Node example with enum fields, function-pointer fields, typed pointer dereference, and a flexible array member:

- [bindings/node/examples/memory_phase6.js](bindings/node/examples/memory_phase6.js)

Equivalent Lua example:

- [bindings/lua/examples/memory_phase6.lua](bindings/lua/examples/memory_phase6.lua)

### `memory.viewArray` / `memory.view_array`

Creates an array-like view over a contiguous sequence of structs.

Node example:

```js
const { memory: mem } = require('urb-ffi');

const Point = { x: 'i32', y: 'i32' };
const size = mem.struct_sizeof(Point);
const ptr = mem.alloc(BigInt(size * 3));
mem.zero(ptr, BigInt(size * 3));

const points = mem.viewArray(ptr, Point, 3);
points[0].x = 10;
points[1].x = 20;
points[2].x = 30;

for (const point of points) {
  console.log(point.x, point.y);
}

mem.free(ptr);
```

### Unions, pointer fields, and fixed arrays

Node example showing all three:

```js
const { memory: mem } = require('urb-ffi');

const FloatBits = {
  data: {
    __union: true,
    f: 'f32',
    u: 'u32',
    b: ['u8', 4],
  },
};

const NodeSchema = {
  value: 'i32',
  next: { __pointer: true },
};

const floatPtr = mem.alloc(4n);
mem.writef32(floatPtr, 3.14);

const unionView = mem.view(floatPtr, FloatBits);
console.log(unionView.data.f);
console.log(unionView.data.u);
console.log(unionView.data.b);

mem.free(floatPtr);
```

## API reference

### Node.js surface

```js
const { ffi, memory } = require('urb-ffi');
```

#### `ffi`

- `ffi.flags`
- `ffi.open(path, flags?)`
- `ffi.close(handle)`
- `ffi.sym(handle, name)`
- `ffi.sym_self(name)`
- `ffi.describe(signature)`
- `ffi.bind(ptr, descriptor)`
- `ffi.callback(descriptor, fn)`
- `ffi.errno()`
- `ffi.dlerror()`

#### `memory`

- `memory.alloc(size)`
- `memory.free(ptr)`
- `memory.realloc(ptr, size)`
- `memory.zero(ptr, size)`
- `memory.copy(dst, src, size)`
- `memory.set(ptr, byteValue, size)`
- `memory.compare(a, b, size)`
- `memory.nullptr()`
- `memory.sizeof_ptr()`
- `memory.readptr(ptr)`
- `memory.writeptr(ptr, value)`
- `memory.readcstring(ptr)`
- `memory.writecstring(ptr, text)`
- `memory.readi8(ptr)` / `memory.writei8(ptr, value)`
- `memory.readu8(ptr)` / `memory.writeu8(ptr, value)`
- `memory.readi16(ptr)` / `memory.writei16(ptr, value)`
- `memory.readu16(ptr)` / `memory.writeu16(ptr, value)`
- `memory.readi32(ptr)` / `memory.writei32(ptr, value)`
- `memory.readu32(ptr)` / `memory.writeu32(ptr, value)`
- `memory.readi64(ptr)` / `memory.writei64(ptr, value)`
- `memory.readu64(ptr)` / `memory.writeu64(ptr, value)`
- `memory.readf32(ptr)` / `memory.writef32(ptr, value)`
- `memory.readf64(ptr)` / `memory.writef64(ptr, value)`
- `memory.allocStr(text)`
- `memory.readArray(ptr, type, count)`
- `memory.writeArray(ptr, type, values)`
- `memory.struct_sizeof(schema)`
- `memory.struct_offsetof(schema, fieldName)`
- `memory.view(ptr, schema)`
- `memory.viewArray(ptr, schema, count)`

### Lua 5.4 surface

```lua
local urb = require('urb_ffi')
local ffi, memory = urb.ffi, urb.memory
```

#### `ffi`

- `ffi.flags`
- `ffi.open(path, flags?)`
- `ffi.close(handle)`
- `ffi.sym(handle, name)`
- `ffi.sym_self(name)`
- `ffi.describe(signature)`
- `ffi.bind(ptr, descriptor)`
- `ffi.callback(descriptor, fn)`
- `ffi.errno()`
- `ffi.dlerror()`

#### `memory`

- `memory.alloc(size)`
- `memory.free(ptr)`
- `memory.realloc(ptr, size)`
- `memory.zero(ptr, size)`
- `memory.copy(dst, src, size)`
- `memory.set(ptr, byteValue, size)`
- `memory.compare(a, b, size)`
- `memory.nullptr()`
- `memory.sizeof_ptr()`
- `memory.readptr(ptr)`
- `memory.writeptr(ptr, value)`
- `memory.readcstring(ptr)`
- `memory.writecstring(ptr, text)`
- `memory.readi8(ptr)` / `memory.writei8(ptr, value)`
- `memory.readu8(ptr)` / `memory.writeu8(ptr, value)`
- `memory.readi16(ptr)` / `memory.writei16(ptr, value)`
- `memory.readu16(ptr)` / `memory.writeu16(ptr, value)`
- `memory.readi32(ptr)` / `memory.writei32(ptr, value)`
- `memory.readu32(ptr)` / `memory.writeu32(ptr, value)`
- `memory.readi64(ptr)` / `memory.writei64(ptr, value)`
- `memory.readu64(ptr)` / `memory.writeu64(ptr, value)`
- `memory.readf32(ptr)` / `memory.writef32(ptr, value)`
- `memory.readf64(ptr)` / `memory.writef64(ptr, value)`
- `memory.alloc_str(text)` and `memory.allocStr(text)`
- `memory.read_array(ptr, type, count)` and `memory.readArray(ptr, type, count)`
- `memory.write_array(ptr, type, values)` and `memory.writeArray(ptr, type, values)`
- `memory.struct_sizeof(schema)`
- `memory.struct_offsetof(schema, fieldName)`
- `memory.view(ptr, schema)`
- `memory.view_array(ptr, schema, count)` and `memory.viewArray(ptr, schema, count)`

## Examples in the repository

### Node examples

- [bindings/node/examples/hello.js](bindings/node/examples/hello.js)
- [bindings/node/examples/manual_types.js](bindings/node/examples/manual_types.js)
- [bindings/node/examples/memory.js](bindings/node/examples/memory.js)
- [bindings/node/examples/memory_utils.js](bindings/node/examples/memory_utils.js)
- [bindings/node/examples/view.js](bindings/node/examples/view.js)
- [bindings/node/examples/memory_phase6.js](bindings/node/examples/memory_phase6.js)
- [bindings/node/examples/meta_fields.js](bindings/node/examples/meta_fields.js)
- [bindings/node/examples/callback.js](bindings/node/examples/callback.js)
- [bindings/node/examples/byvalue.js](bindings/node/examples/byvalue.js)
- [bindings/node/examples/sym_self.js](bindings/node/examples/sym_self.js)
- [bindings/node/examples/smoke.js](bindings/node/examples/smoke.js)

### Lua examples

- [bindings/lua/examples/hello.lua](bindings/lua/examples/hello.lua)
- [bindings/lua/examples/memory.lua](bindings/lua/examples/memory.lua)
- [bindings/lua/examples/view.lua](bindings/lua/examples/view.lua)
- [bindings/lua/examples/memory_phase6.lua](bindings/lua/examples/memory_phase6.lua)
- [bindings/lua/examples/callback.lua](bindings/lua/examples/callback.lua)
- [bindings/lua/examples/byvalue.lua](bindings/lua/examples/byvalue.lua)
- [bindings/lua/examples/sym_self.lua](bindings/lua/examples/sym_self.lua)

## Notes and limitations

- Rich host descriptors built with `ffi.type.func(...)` support schema-compatible by-value struct/union/array arguments and returns in the Node and Lua bindings.
- Rich callbacks built with `ffi.type.func(...)` now adapt by-value record/array arguments, schema-compatible complex returns, and function-pointer callback arguments.
- Host callbacks are creator-thread only; foreign-thread invocation is not supported.
- By-value recursive schemas are not supported.
- Recursive data structures can still be represented through pointer fields, but dereferencing is manual: store the address in a pointer field and create a new view from that address when traversing.
- The Node binding is the npm-focused package surface.
- The Lua binding currently loads a Unix-style shared object and is primarily documented for Unix-like environments.
- If a symbol lookup or `dlopen` fails, check `ffi.dlerror()`.
- If a native call reports failure through `errno`, inspect it with `ffi.errno()`.

## In one sentence

`urb-ffi` lets Node.js and Lua 5.4 load native libraries, call C functions, create callbacks, manage raw memory, and model real C layouts without leaving the host language.
