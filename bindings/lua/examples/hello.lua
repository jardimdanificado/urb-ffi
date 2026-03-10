local source = debug.getinfo(1, "S").source:sub(2)
local dir = source:match("^(.*)/[^/]+$") or "."
package.path = dir .. "/../?.lua;" .. package.path

local urb = require("urb_ffi")
local ffi, mem = urb.ffi, urb.memory

local libc = ffi.open("libc.so.6")
local puts_desc = ffi.describe("i32 puts(cstring)")
local getenv_desc = ffi.describe("cstring getenv(cstring)")
local puts = ffi.bind(ffi.sym(libc, "puts"), puts_desc)
local getenv = ffi.bind(ffi.sym(libc, "getenv"), getenv_desc)

puts("hello from urb-ffi (lua)")
local home = getenv("HOME")
print("HOME =", home)

ffi.close(libc)
