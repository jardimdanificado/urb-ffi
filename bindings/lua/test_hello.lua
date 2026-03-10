local source = debug.getinfo(1, "S").source:sub(2)
local dir = source:match("^(.*)/[^/]+$") or "."
package.path = dir .. "/../?.lua;" .. package.path

print("DEBUG from hello.lua: requiring urb_ffi")
print("DEBUG: package.path =", package.path)

-- Now create a test module that prints its source
local test_file = io.open("test_module.lua", "w")
test_file:write([[
local source = debug.getinfo(1, "S").source
print("DEBUG from test_module: source =", source)
return {}
]])
test_file:close()

require("test_module")
