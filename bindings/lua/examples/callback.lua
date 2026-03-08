local source = debug.getinfo(1, "S").source:sub(2)
local dir = source:match("^(.*)/[^/]+$") or "."
package.path = dir .. "/../?.lua;" .. package.path

local urb = require("urb_ffi")
local ffi, mem = urb.ffi, urb.memory

local libc = ffi.open("libc.so.6")
local qsort = ffi.bind(ffi.sym(libc, "qsort"), "void qsort(pointer, u64, u64, pointer)")

local nums = { 42, 7, 99, -3, 15 }
local n = #nums
local buf = mem.alloc(n * 4)
for i = 1, n do
    mem.writei32(buf + (i - 1) * 4, nums[i])
end

io.write("antes:")
for i = 1, n do io.write(i == 1 and " " or ", ", nums[i]) end
io.write("\n")

local cmp = ffi.callback("i32 cmp(pointer, pointer)", function(a, b)
    local va = mem.readi32(a)
    local vb = mem.readi32(b)
    if va < vb then return -1 end
    if va > vb then return 1 end
    return 0
end)

qsort(buf, n, 4, cmp.ptr)

io.write("depois:")
for i = 1, n do
    io.write(i == 1 and " " or ", ", mem.readi32(buf + (i - 1) * 4))
end
io.write("\n")

mem.free(buf)
ffi.close(libc)
