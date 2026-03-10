local function dirname(path)
    return path:match("^(.*)/[^/]+$") or "."
end

local function module_dir()
    local source = debug.getinfo(1, "S").source
    print("DEBUG: source =", source)
    if source:sub(1, 1) ~= "@" then
        error("urb_ffi.lua must be loaded from a file", 2)
    end
    local file = source:sub(2)
    print("DEBUG: file =", file)
    return dirname(file)
end

local lua_dir = module_dir()
print("DEBUG: lua_dir =", lua_dir)
local candidates = {
    lua_dir .. "/urb_ffi_native.so",
    dirname(dirname(lua_dir)) .. "/dist/urb_ffi_native.so",
}

for i, so_path in ipairs(candidates) do
    print(string.format("DEBUG: trying [%d] = %s", i, so_path))
    local loader, err = package.loadlib(so_path, "luaopen_urb_ffi_native")
    if loader then
        print("DEBUG: SUCCESS loading from", so_path)
        print("DEBUG: loader =", loader)
        return
    else
        print("DEBUG: FAILED:", err)
    end
end

print("DEBUG: all candidates failed")
