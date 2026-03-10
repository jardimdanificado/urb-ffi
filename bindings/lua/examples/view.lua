local source = debug.getinfo(1, "S").source:sub(2)
local dir = source:match("^(.*)/[^/]+$") or "."
package.path = dir .. "/../?.lua;" .. package.path

local urb = require("urb_ffi")
local ffi, mem = urb.ffi, urb.memory

local PointSchema = {
    { name = "x", type = "i32" },
    { name = "y", type = "i32" },
    { name = "value", type = "f64" },
    { name = "flags", type = "u64" },
}

local sz = mem.struct_sizeof(PointSchema)
print("sizeof Point =", sz)
print("offsetof x =", mem.struct_offsetof(PointSchema, "x"))
print("offsetof y =", mem.struct_offsetof(PointSchema, "y"))
print("offsetof value =", mem.struct_offsetof(PointSchema, "value"))
print("offsetof flags =", mem.struct_offsetof(PointSchema, "flags"))

local p = mem.alloc(sz)
mem.zero(p, sz)
local v = mem.view(p, PointSchema)
v.x = 10
v.y = 20
v.value = 3.14
v.flags = 0xDEADBEEF

local v2 = mem.view(p, PointSchema)
print("x =", v2.x, "y =", v2.y, string.format("value = %.2f", v2.value), string.format("flags = 0x%x", v2.flags))

local n = 3
local arr_ptr = mem.alloc(n * sz)
mem.zero(arr_ptr, n * sz)
local pts = mem.view_array(arr_ptr, PointSchema, n)
pts[1].x = 1
pts[2].x = 2
pts[3].x = 3

for i = 1, #pts do
    print(string.format("pts[%d].x = %d", i, pts[i].x))
end

local libc = ffi.open("libc.so.6")
local ClockSchema = {
    { name = "tv_sec", type = "i64" },
    { name = "tv_nsec", type = "i64" },
}
local clock_ptr = mem.alloc(mem.struct_sizeof(ClockSchema))
local clockgettime_desc = ffi.describe("i32 clock_gettime(i32, pointer)")
local clockgettime = ffi.bind(ffi.sym(libc, "clock_gettime"), clockgettime_desc)
clockgettime(0, clock_ptr)
local ts = mem.view(clock_ptr, ClockSchema)
print("tv_nsec =", ts.tv_nsec)

mem.free(p)
mem.free(arr_ptr)
mem.free(clock_ptr)
ffi.close(libc)
