local source = debug.getinfo(1, "S").source:sub(2)
local dir = source:match("^(.*)/[^/]+$") or "."
package.path = dir .. "/../?.lua;" .. package.path

local urb = require("urb_ffi")
local ffi, mem = urb.ffi, urb.memory

local Mode = ffi.type.enum({
    IDLE = 0,
    RUNNING = 2,
})

local Point = {
    { name = "x", type = "i32" },
    { name = "y", type = "i32" },
}

local UnaryOp = ffi.type.func(ffi.type.i32(), { ffi.type.i32() }, { name = "unary_op" })

local Holder = {
    { name = "mode", type = Mode },
    { name = "cb", type = UnaryOp },
    { name = "point", type = "pointer", to = Point },
    { name = "nested", schema = Point },
}

local holder_ptr = mem.alloc(mem.struct_sizeof(Holder))
local point_ptr = mem.alloc(mem.struct_sizeof(Point))
mem.zero(holder_ptr, mem.struct_sizeof(Holder))
mem.zero(point_ptr, mem.struct_sizeof(Point))

local point = mem.view(point_ptr, Point)
point.x = 10
point.y = 20

local add_three = ffi.callback(UnaryOp, function(value)
    return value + 3
end)

local holder = mem.view(holder_ptr, Holder)
holder.mode = 2
holder.cb = add_three.ptr
holder.point = point_ptr
holder.nested = { x = 7, y = 9 }

print("enum mode", holder.mode)
print("fnptr call", holder.cb(39))
print("nested struct", holder.nested.x, holder.nested.y)

local point_ref = holder.point
local point_view = point_ref.deref()
print("pointer deref", point_view.x, point_view.y)

point_ref.write({ x = 33, y = 44 })
print("pointer write", point.x, point.y)

local Packet = {
    { name = "len", type = "u32" },
    { name = "bytes", type = "u8", flexible = true },
}

local packet_size = mem.struct_sizeof(Packet) + 5
local packet_ptr = mem.alloc(packet_size)
mem.zero(packet_ptr, packet_size)

local packet = mem.view(packet_ptr, Packet, packet_size)
packet.len = 5
packet.bytes = { 1, 2, 3, 4, 5 }

print("flex len", packet.len, "byteSize", packet.byte_size)
print("flex bytes", table.concat(packet.bytes, ","))

mem.free(packet_ptr)
mem.free(point_ptr)
mem.free(holder_ptr)
