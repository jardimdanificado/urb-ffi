# URBC Bytecode Reference

This file documents the on-disk format and every executable token/opcode implemented by this repository.

## 1. Execution model

- VM style: stack machine.
- Stack notation here uses **top on the right**.
  - Example: `[a, b] -> [c]` means `b` is popped first.
- Truthiness: any value with `v.i != 0` is true.
- Program counter (`pc`) is an **absolute index inside loaded `mem`**, not code-relative.
- `mem` layout after load:
  - `mem[0 .. const_count-1]`: constants
  - `mem[const_count .. const_count+code_count-1]`: code tokens
- First executed instruction: `entry_pc = const_count`.
- `max_stack` is used as the initial stack capacity; it is not a hard limit.

## 2. Global binary format

All integers are **little-endian**.

### Header (`40` bytes)

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `DFFI` |
| 4 | 1 | version_major | Must equal `1` |
| 5 | 1 | version_minor | Informational |
| 6 | 1 | profile | Must equal `1` (`URBC_PROFILE_URB64`) |
| 7 | 1 | flags | Must be `0` |
| 8 | 2 | max_stack | Initial stack capacity |
| 10 | 2 | string_count | Number of strings |
| 12 | 2 | signature_count | Number of signatures |
| 14 | 2 | schema_count | Number of schemas |
| 16 | 2 | const_count | Number of constants |
| 18 | 2 | code_count | Number of code tokens |
| 20 | 4 | string_bytes | Byte size of string section |
| 24 | 4 | signature_bytes | Byte size of signature section |
| 28 | 4 | schema_bytes | Byte size of schema section |
| 32 | 4 | const_bytes | Byte size of constant section |
| 36 | 4 | code_bytes | Byte size of code section |

### Section order

1. header
2. string section
3. signature section
4. schema section
5. constant section
6. code section

## 3. Section encodings

### 3.1 Strings

Repeated `string_count` times:

| Field | Size | Meaning |
|---|---:|---|
| `len` | 2 | String byte length |
| `bytes` | `len` | Raw bytes, no trailing `\0` in file |

Runtime copies each string and appends `\0` in memory.

### 3.2 Signatures

Repeated `signature_count` times:

| Field | Size | Meaning |
|---|---:|---|
| `string_index` | 2 | Index into string table |

A signature entry is only an indirection to a string.

### 3.3 Schemas

Schemas are packed back-to-back.

#### Schema header (`4` bytes)

| Field | Size | Meaning |
|---|---:|---|
| `schema_kind` | 1 | `0=struct`, `1=union` |
| `reserved` | 1 | Must be `0` |
| `field_count` | 2 | Number of fields |

#### Schema field (`10` bytes each)

| Field | Size | Meaning |
|---|---:|---|
| `name_index` | 2 | Index into string table |
| `field_kind` | 1 | See field kinds below |
| `prim_type` | 1 | Primitive type id |
| `aux` | 2 | Field-specific extra data |
| `ref_schema_index` | 2 | Nested schema index |
| `reserved` | 2 | Must be `0` |

Field semantics:

- `PRIM`: `prim_type` is required.
- `ARRAY`: fixed array of primitives; `prim_type` is element type, `aux` is element count (`> 0`).
- `STRUCT`: by-value nested schema; `ref_schema_index` is required.
- `POINTER`: native pointer field; stored size/alignment is `sizeof(void*)`; other metadata is ignored.

Layout rules:

- Structs: fields are aligned one by one, offsets increase.
- Unions: every field offset is `0`; size is max field size aligned to max field alignment.
- Recursive by-value schemas are not supported.

### 3.4 Constants

Each constant starts with a `4`-byte wire header:

| Field | Size | Meaning |
|---|---:|---|
| `kind` | 1 | Constant kind id |
| `reserved` | 1 | Must be `0` |
| `aux` | 2 | Kind-specific data |

Payload depends on `kind`:

| Kind | Id | Payload | Meaning |
|---|---:|---|---|
| `BOOL` | 0 | `u8` | `0` or non-zero |
| `I64` | 1 | `i64` | Signed integer |
| `U64` | 2 | `u64` | Unsigned integer |
| `F64` | 3 | `f64` | Float |
| `STRING` | 4 | none | `aux = string_index` |
| `SIG` | 5 | none | `aux = signature_index` |
| `SCHEMA` | 6 | none | `aux = schema_index` |
| `NULLPTR` | 7 | none | Null pointer |

Runtime form:

- `STRING` becomes `char*`.
- `SIG` becomes `char*` to the signature text.
- `SCHEMA` becomes an owned schema-handle wrapper.
- `NULLPTR` becomes `NULL`.

### 3.5 Code tokens

Each code entry is always `4` bytes:

| Field | Size | Meaning |
|---|---:|---|
| `kind` | 1 | Token kind id |
| `reserved` | 1 | Must be `0` |
| `value` | 2 | Kind-specific value |

Token kinds:

| Kind | Id | `value` meaning |
|---|---:|---|
| `ALIAS` | 0 | Alias id |
| `OP` | 1 | Opcode id |
| `CONST_REF` | 2 | Constant index |

Load-time lowering:

- `ALIAS` -> `INT_MIN + alias_id`
- `OP` -> `INT_MIN + 8 + op_id`
- `CONST_REF` -> `INT_MAX - const_index`

`CONST_REF` does **not** inline data; it pushes `mem[const_index]` at runtime.

## 4. Enum ids used by bytecode

### 4.1 Alias ids

| Id | Name | Runtime effect |
|---:|---|---|
| 0 | `goto` | Pop target `pc`; jump there |
| 1 | `goif` | Pop `cond`, then target; jump if true |
| 2 | `goie` | Pop `cond`, then true-target, then else-target |
| 3 | `exec` | Push the exec registry list pointer |
| 4 | `mem` | Push the loaded memory list pointer |

Important:

- Jump targets are **absolute `mem` indexes**.
- In raw stack form:
  - `goto`: `[target] -> []`, then jump
  - `goif`: `[target, cond] -> []`, then maybe jump
  - `goie`: `[else_pc, true_pc, cond] -> []`, then jump

### 4.2 Primitive type ids

| Id | Name | Size/alignment |
|---:|---|---|
| 0 | `invalid` | invalid |
| 1 | `bool` | 1 / 1 |
| 2 | `i8` | 1 / 1 |
| 3 | `u8` | 1 / 1 |
| 4 | `i16` | 2 / 2 |
| 5 | `u16` | 2 / 2 |
| 6 | `i32` | 4 / 4 |
| 7 | `u32` | 4 / 4 |
| 8 | `i64` | 8 / 8 |
| 9 | `u64` | 8 / 8 |
| 10 | `f32` | 4 / 4 |
| 11 | `f64` | 8 / 8 |
| 12 | `pointer` | `sizeof(void*)` |
| 13 | `cstring` | `sizeof(void*)` |

### 4.3 Schema kind ids

| Id | Name |
|---:|---|
| 0 | `struct` |
| 1 | `union` |

### 4.4 Field kind ids

| Id | Name |
|---:|---|
| 0 | `prim` |
| 1 | `array` |
| 2 | `struct` |
| 3 | `pointer` |

## 5. Opcode reference

Notes:

- Stack notation uses top on the right.
- `ptr` means native address stored in `Value.u`/`Value.p`.
- Many memory ops are NULL-tolerant:
  - reads from `NULL` return zero,
  - writes/copies/sets on `NULL` do nothing.
- On runtime failure, execution stops and `last_error` is filled.

### 5.1 Core / stack / compare

| Id | Name | Stack effect | Meaning |
|---:|---|---|---|
| 0 | `stack.dup` | `[x] -> [x, x]` | Duplicate top value |
| 1 | `stack.pop` | `[x] -> []` | Discard top value |
| 2 | `stack.swap` | `[a, b] -> [b, a]` | Swap top two |
| 3 | `ptr.add` | `[ptr, offset] -> [ptr+offset]` | Pointer arithmetic; `offset` uses signed `.i` |
| 4 | `ptr.sub` | `[a, b] -> [a-b]` | Unsigned subtraction, result stored as signed `.i` |
| 5 | `cmp.eq` | `[a, b] -> [bool]` | Raw equality on `.u` |
| 6 | `cmp.ne` | `[a, b] -> [bool]` | Raw inequality on `.u` |
| 7 | `cmp.lt_i64` | `[a, b] -> [bool]` | Signed compare |
| 8 | `cmp.le_i64` | `[a, b] -> [bool]` | Signed compare |
| 9 | `cmp.gt_i64` | `[a, b] -> [bool]` | Signed compare |
| 10 | `cmp.ge_i64` | `[a, b] -> [bool]` | Signed compare |
| 11 | `logic.not` | `[x] -> [!truthy(x)]` | `truthy(x)` is `x.i != 0` |

### 5.2 Memory opcodes

| Id | Name | Stack effect | Meaning |
|---:|---|---|---|
| 20 | `mem.alloc` | `[size] -> [ptr]` | `malloc(size)` |
| 21 | `mem.free` | `[ptr] -> []` | `free(ptr)` |
| 22 | `mem.realloc` | `[ptr, size] -> [new_ptr]` | `realloc(ptr, size)` |
| 23 | `mem.zero` | `[ptr, size] -> []` | `memset(ptr, 0, size)` |
| 24 | `mem.copy` | `[dst, src, size] -> []` | `memcpy(dst, src, size)` |
| 25 | `mem.set` | `[ptr, byte, size] -> []` | `memset(ptr, byte & 0xFF, size)` |
| 26 | `mem.compare` | `[a, b, size] -> [result]` | `memcmp(a, b, size)` |
| 27 | `mem.nullptr` | `[] -> [NULL]` | Push null pointer |
| 28 | `mem.sizeof_ptr` | `[] -> [sizeof(void*)]` | Pointer size |
| 29 | `mem.readptr` | `[ptr] -> [value]` | Read `uintptr_t` from memory |
| 30 | `mem.writeptr` | `[ptr, value] -> []` | Write `uintptr_t` |
| 31 | `mem.readcstring` | `[ptr] -> [cstring]` | Return same pointer as C string |
| 32 | `mem.writecstring` | `[ptr, cstring] -> []` | Copy string plus trailing `\0` |
| 33 | `mem.readi8` | `[ptr] -> [i8]` | Read `int8_t` |
| 34 | `mem.readu8` | `[ptr] -> [u8]` | Read `uint8_t` |
| 35 | `mem.readi16` | `[ptr] -> [i16]` | Read `int16_t` |
| 36 | `mem.readu16` | `[ptr] -> [u16]` | Read `uint16_t` |
| 37 | `mem.readi32` | `[ptr] -> [i32]` | Read `int32_t` |
| 38 | `mem.readu32` | `[ptr] -> [u32]` | Read `uint32_t` |
| 39 | `mem.readi64` | `[ptr] -> [i64]` | Read `int64_t` |
| 40 | `mem.readu64` | `[ptr] -> [u64]` | Read `uint64_t` |
| 41 | `mem.readf32` | `[ptr] -> [f32]` | Read `float` |
| 42 | `mem.readf64` | `[ptr] -> [f64]` | Read `double` |
| 43 | `mem.writei8` | `[ptr, i8] -> []` | Write `int8_t` |
| 44 | `mem.writeu8` | `[ptr, u8] -> []` | Write `uint8_t` |
| 45 | `mem.writei16` | `[ptr, i16] -> []` | Write `int16_t` |
| 46 | `mem.writeu16` | `[ptr, u16] -> []` | Write `uint16_t` |
| 47 | `mem.writei32` | `[ptr, i32] -> []` | Write `int32_t` |
| 48 | `mem.writeu32` | `[ptr, u32] -> []` | Write `uint32_t` |
| 49 | `mem.writei64` | `[ptr, i64] -> []` | Write `int64_t` |
| 50 | `mem.writeu64` | `[ptr, u64] -> []` | Write `uint64_t` |
| 51 | `mem.writef32` | `[ptr, f32] -> []` | Write `float` |
| 52 | `mem.writef64` | `[ptr, f64] -> []` | Write `double` |

### 5.3 Schema / view / union opcodes

Schema operands are values produced from `CONST_SCHEMA` constants.

| Id | Name | Stack effect | Meaning |
|---:|---|---|---|
| 60 | `schema.sizeof` | `[schema] -> [size]` | Compiled schema size |
| 61 | `schema.offsetof` | `[schema, field_name] -> [offset]` | Field offset by name |
| 62 | `view.make` | `[ptr, schema] -> [view]` | View over one struct/union at `ptr` |
| 63 | `view.array` | `[ptr, schema, count] -> [array_view]` | Array view with stride `schema.size` |
| 64 | `view.get` | `[handle, name] -> [value]` | Read field/property |
| 65 | `view.set` | `[view, name, value] -> []` | Write primitive/pointer field |
| 66 | `union.make` | `[ptr, schema] -> [view]` | Same as `view.make`, but schema must be union |
| 67 | `union.sizeof` | `[schema] -> [size]` | Same as `schema.sizeof`, but union-only |

`view.get` details:

- On a normal view:
  - primitive field -> pushes decoded value
  - pointer field -> pushes pointer value
  - primitive array field -> pushes raw base address of the array
  - struct field -> pushes nested view
- On an array view:
  - name `"count"` -> element count
  - name `"ptr"` -> base pointer
  - name `"N"` -> element view at decimal index `N`

`view.set` only accepts primitive and pointer fields.

### 5.4 FFI opcodes

Library open flags are numeric ORs of:

| Flag | Value |
|---|---:|
| `URBC_DLOPEN_LAZY` | `1` |
| `URBC_DLOPEN_NOW` | `2` |
| `URBC_DLOPEN_LOCAL` | `4` |
| `URBC_DLOPEN_GLOBAL` | `8` |
| `URBC_DLOPEN_NODELETE` | `16` |
| `URBC_DLOPEN_NOLOAD` | `32` |

| Id | Name | Stack effect | Meaning |
|---:|---|---|---|
| 80 | `ffi.open` | `[path, flags] -> [handle]` | Open shared library; returns `NULL` on failure |
| 81 | `ffi.close` | `[handle] -> []` | Close shared library handle |
| 82 | `ffi.sym` | `[handle, name] -> [symbol_ptr]` | Resolve symbol from library |
| 83 | `ffi.sym_self` | `[name] -> [symbol_ptr]` | Resolve symbol from current process |
| 84 | `ffi.bind` | `[fn_ptr, sig] -> [bound_fn]` | Build libffi callable wrapper |
| 85 | `ffi.call0` | `[bound_fn] -> [ret]` | Call bound function with 0 args |
| 86 | `ffi.call1` | `[bound_fn, a0] -> [ret]` | Call with 1 arg |
| 87 | `ffi.call2` | `[bound_fn, a0, a1] -> [ret]` | Call with 2 args |
| 88 | `ffi.call3` | `[bound_fn, a0, a1, a2] -> [ret]` | Call with 3 args |
| 89 | `ffi.call4` | `[bound_fn, a0, a1, a2, a3] -> [ret]` | Call with 4 args |
| 90 | `ffi.callback` | `[sig, callable] -> [fn_ptr]` | Build native callback that calls a host binding |
| 91 | `ffi.errno` | `[] -> [errno]` | Push current thread `errno` |
| 92 | `ffi.dlerror` | `[] -> [cstring_or_null]` | Copy last dynamic-loader error |
| 93 | `ffi.callv` | see below | Variadic/general call |

Notes:

- `ffi.open`, `ffi.sym`, `ffi.sym_self` do not raise runtime failure on lookup/open failure; check `ffi.dlerror`.
- `ffi.call*` on a `void` function pushes a zeroed `Value`.
- `ffi.callback` only targets **host bindings**, not bytecode closures.
- `ffi.callback` is creator-thread only; foreign-thread invocation returns a zero/default result.
- Variadic callbacks are not supported.

#### `ffi.callv`

Stack form:

`[bound_fn, arg0, ..., argN-1, vararg_kind0, ..., vararg_kindK-1, argc, extra] -> [ret]`

Where:

- `argc` = total argument count
- `extra` = number of variadic arguments (`K`)
- `vararg_kind*` are `URBC_PRIM_*` ids for the **last `extra` arguments**, in left-to-right argument order

Example shape for a function with 2 fixed args and 2 variadic args:

`[fn, fixed0, fixed1, var0, var1, kind(var0), kind(var1), 4, 2]`

### 5.5 Host call opcodes

Host call target may be:

- a host binding pointer, or
- a host binding name string

| Id | Name | Stack effect | Meaning |
|---:|---|---|---|
| 100 | `host.call0` | `[callable] -> [ret]` | Host callback with 0 args |
| 101 | `host.call1` | `[callable, a0] -> [ret]` | Host callback with 1 arg |
| 102 | `host.call2` | `[callable, a0, a1] -> [ret]` | Host callback with 2 args |
| 103 | `host.call3` | `[callable, a0, a1, a2] -> [ret]` | Host callback with 3 args |

## 6. FFI signature strings

`CONST_SIG` and `ffi.bind`/`ffi.callback` use a small C-like signature grammar.

### Grammar

`return_type name(arg0, arg1, ..., ...)`

The function name is optional for binding purposes; parsing also accepts just a return type, or a return type plus a name without parentheses.

### Supported base types

- `void`
- `i8`, `u8`
- `i16`, `u16`
- `i32`, `u32`, `int`, `uint`
- `i64`, `u64`, `long`, `ulong`
- `f32`, `f64`, `float`, `double`
- `bool`
- `cstring`, `string`
- `pointer`, `ptr`

Extra rules:

- `const` is accepted.
- Any trailing `*` turns the type into `pointer`.
- `pointer(tag)` is accepted; the tag is metadata only.
- `...` must be the last argument.
- Max argument count: `32`.

## 7. Practical rules for assemblers/emitters

- Emit only little-endian integers.
- Keep all `reserved` bytes/words at `0`.
- Use absolute PCs for branch aliases.
- First executable PC is `const_count`, not `0`.
- Use constants for strings, signatures, schemas, and literal numbers; code has no inline immediates.
- For primitive arrays inside schemas, `view.get` returns the raw array address, not a view handle.
- For `ffi.callv`, variadic type ids describe only the variadic tail.
