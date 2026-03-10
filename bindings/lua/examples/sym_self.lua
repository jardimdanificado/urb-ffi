local source = debug.getinfo(1, "S").source:sub(2)
local dir = source:match("^(.*)/[^/]+$") or "."
package.path = dir .. "/../?.lua;" .. package.path

local urb = require("urb_ffi")
local ffi = urb.ffi

print("urb-ffi ffi.sym_self demo (lua)")

local puts_ptr = ffi.sym_self("puts")
local strlen_ptr = ffi.sym_self("strlen")
local puts_desc = ffi.describe("i32 puts(cstring)")
local strlen_desc = ffi.describe("u64 strlen(cstring)")
local puts = ffi.bind(puts_ptr, puts_desc)
local strlen = ffi.bind(strlen_ptr, strlen_desc)
local text = "hello world from sym_self"

puts(text)
print(string.format("strlen(%q) = %s", text, tostring(strlen(text))))
