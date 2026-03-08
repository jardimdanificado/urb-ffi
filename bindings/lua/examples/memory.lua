local source = debug.getinfo(1, "S").source:sub(2)
local dir = source:match("^(.*)/[^/]+$") or "."
package.path = dir .. "/../?.lua;" .. package.path

local mem = require("urb_ffi").memory

local p = mem.alloc(4)
mem.writei32(p, 42)
print("readi32:", mem.readi32(p))

mem.writei32(p, -7)
print("readi32 negativo:", mem.readi32(p))

p = mem.realloc(p, 8)
mem.writef64(p, 3.14159)
print("readf64:", mem.readf64(p))

local s = mem.alloc(32)
mem.writecstring(s, "olá do urb-ffi (lua)")
print("readcstring:", mem.readcstring(s))

mem.free(p)
mem.free(s)
print("tudo ok — sem leaks")
