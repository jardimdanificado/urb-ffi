local source = debug.getinfo(1, "S").source:sub(2)
local dir = source:match("^(.*)/[^/]+$") or "."
package.path = dir .. "/../?.lua;" .. package.path

local urb = require("urb_ffi")
local ffi, mem = urb.ffi, urb.memory

local libc = ffi.open("libc.so.6")
local puts_type = ffi.type.func(ffi.type.i32(), { ffi.type.cstring() }, { name = "puts" })
local puts = ffi.bind(ffi.sym(libc, "puts"), puts_type)

puts("hello from urb-ffi manual type descriptors (lua)")

local int_ptr = mem.alloc(4)
local int_ref = ffi.global(int_ptr, ffi.type.i32())
int_ref.value = 123
print("global i32 =", int_ref.value)

local point_type = ffi.type.struct({
    { name = "x", type = ffi.type.i32() },
    { name = "y", type = ffi.type.i32() },
})
local point_ptr = mem.alloc(ffi.type.sizeof(point_type))
local point = ffi.global(point_ptr, point_type)
point.x = 7
point.y = 11
print("point =", point.x, point.y)

mem.free(point_ptr)
mem.free(int_ptr)
ffi.close(libc)