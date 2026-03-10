local source = debug.getinfo(1, "S").source:sub(2)
local dir = source:match("^(.*)/[^/]+$") or "."
package.path = dir .. "/../?.lua;" .. package.path

local urb = require("urb_ffi")
local ffi, mem = urb.ffi, urb.memory

local suf = package.cpath:match("%.dylib") and ".dylib" or ".so"
local lib = ffi.open(dir .. "/libbyvalue" .. suf, ffi.flags.NOW + ffi.flags.LOCAL)

local point_type = ffi.type.struct({
    { name = "x", type = ffi.type.i32() },
    { name = "y", type = ffi.type.i32() },
}, { name = "Point" })

local point_schema = {
    { name = "x", type = "i32" },
    { name = "y", type = "i32" },
}

local pptr = mem.alloc(mem.struct_sizeof(point_schema))
local point = mem.view(pptr, point_schema)
point.x = 7
point.y = 11

local sum_sig = ffi.type.func(ffi.type.i32(), { point_type }, { name = "sum_point" })
local sum = ffi.bind(ffi.sym(lib, "sum_point"), sum_sig)
print("sum_point", sum(point))

local swap_sig = ffi.type.func(point_type, { point_type }, { name = "swap_point" })
local swap = ffi.bind(ffi.sym(lib, "swap_point"), swap_sig)
local swapped_ptr = swap(point)
local swapped = mem.view(swapped_ptr, point_schema)
print("swapped", swapped.x, swapped.y)

local cb_type = ffi.type.func(ffi.type.i32(), { point_type }, { name = "cb" })
local js_cb = ffi.callback(cb_type, function(pt)
    print("callback got", pt.x, pt.y)
    return pt.x * 100 + pt.y
end)

local call_cb_sig = ffi.type.func(ffi.type.i32(), { point_type, ffi.type.pointer(cb_type) }, { name = "call_cb" })
local call_cb = ffi.bind(ffi.sym(lib, "call_cb"), call_cb_sig)
print("call_cb returns", call_cb(point, js_cb.ptr))

local mapper_type = ffi.type.func(point_type, { point_type }, { name = "mapper" })
local mapper_cb = ffi.callback(mapper_type, function(pt)
    return {
        x = pt.y + 1,
        y = pt.x + 2,
    }
end)
local map_point_sig = ffi.type.func(point_type, { point_type, ffi.type.pointer(mapper_type) }, { name = "map_point" })
local map_point = ffi.bind(ffi.sym(lib, "map_point"), map_point_sig)
local mapped = mem.view(map_point(point, mapper_cb.ptr), point_schema)
print("mapped", mapped.x, mapped.y)

local op_type = ffi.type.func(ffi.type.i32(), { ffi.type.i32() }, { name = "op" })
local use_op_type = ffi.type.func(ffi.type.i32(), { ffi.type.pointer(op_type), ffi.type.i32() }, { name = "use_op" })
local use_op_cb = ffi.callback(use_op_type, function(op, value)
    local applied = op(value)
    print("op(value)", applied)
    return applied * 2
end)
local call_with_op_sig = ffi.type.func(ffi.type.i32(), { ffi.type.i32(), ffi.type.pointer(use_op_type) }, { name = "call_with_op" })
local call_with_op = ffi.bind(ffi.sym(lib, "call_with_op"), call_with_op_sig)
print("call_with_op returns", call_with_op(10, use_op_cb.ptr))

ffi.close(lib)
mem.free(pptr)
