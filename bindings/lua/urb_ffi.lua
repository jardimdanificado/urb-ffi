local function dirname(path)
    return path:match("^(.*)/[^/]+$") or "."
end

local function module_root()
    local source = debug.getinfo(1, "S").source
    if source:sub(1, 1) ~= "@" then
        error("urb_ffi.lua must be loaded from a file", 2)
    end
    local file = source:sub(2)
    local lua_dir = dirname(file)
    return dirname(dirname(lua_dir))
end

local function load_native()
    local root = module_root()
    local so_path = root .. "/dist/urb_ffi_native.so"
    local loader, err = package.loadlib(so_path, "luaopen_urb_ffi_native")
    if not loader then
        error("failed to load " .. so_path .. ": " .. tostring(err), 2)
    end
    return loader()
end

local native = load_native()
local PTR_SIZE = native.sizeof_ptr()

local TYPE_ALIASES = {
    bool = "bool",
    boolean = "bool",
    i8 = "i8",
    int8 = "i8",
    u8 = "u8",
    uint8 = "u8",
    byte = "u8",
    i16 = "i16",
    int16 = "i16",
    u16 = "u16",
    uint16 = "u16",
    i32 = "i32",
    int32 = "i32",
    int = "i32",
    u32 = "u32",
    uint32 = "u32",
    uint = "u32",
    i64 = "i64",
    int64 = "i64",
    long = "i64",
    u64 = "u64",
    uint64 = "u64",
    ulong = "u64",
    f32 = "f32",
    float32 = "f32",
    float = "f32",
    f64 = "f64",
    float64 = "f64",
    double = "f64",
    pointer = "pointer",
    ptr = "pointer",
    cstring = "cstring",
    string = "cstring",
}

local TYPE_INFO = {
    bool = { size = 1, align = 1 },
    i8 = { size = 1, align = 1 },
    u8 = { size = 1, align = 1 },
    i16 = { size = 2, align = 2 },
    u16 = { size = 2, align = 2 },
    i32 = { size = 4, align = 4 },
    u32 = { size = 4, align = 4 },
    i64 = { size = 8, align = 8 },
    u64 = { size = 8, align = 8 },
    f32 = { size = 4, align = 4 },
    f64 = { size = 8, align = 8 },
    pointer = { size = PTR_SIZE, align = PTR_SIZE },
    cstring = { size = PTR_SIZE, align = PTR_SIZE },
}

local SCHEMA_CACHE = setmetatable({}, { __mode = "k" })
local SCHEMA_STACK = setmetatable({}, { __mode = "k" })

local function align_up(value, align)
    return ((value + align - 1) // align) * align
end

local function to_non_negative_int(value, what)
    if type(value) ~= "number" or value % 1 ~= 0 or value < 0 then
        error(what .. " must be a non-negative integer", 3)
    end
    return value
end

local function normalize_type(type_name)
    if type(type_name) ~= "string" then
        error("type must be a string", 3)
    end
    local normalized = TYPE_ALIASES[type_name:match("^%s*(.-)%s*$")]
    if not normalized then
        error("unsupported type: " .. tostring(type_name), 3)
    end
    return normalized
end

local compile_schema

local function compile_field(entry)
    if type(entry) ~= "table" then
        error("schema fields must be tables", 4)
    end
    if type(entry.name) ~= "string" or entry.name == "" then
        error("schema field must have a non-empty name", 4)
    end

    if entry.pointer or entry.__pointer then
        return {
            name = entry.name,
            kind = "pointer",
            size = PTR_SIZE,
            align = PTR_SIZE,
        }
    end

    if entry.schema ~= nil then
        local schema = compile_schema(entry.schema)
        return {
            name = entry.name,
            kind = "struct",
            schema = schema,
            size = schema.size,
            align = schema.align,
        }
    end

    if type(entry.type) ~= "string" then
        error("field " .. entry.name .. " must have a string type, schema, or pointer=true", 4)
    end

    local normalized = normalize_type(entry.type)
    local info = TYPE_INFO[normalized]
    if entry.count ~= nil then
        local count = to_non_negative_int(entry.count, "array count for " .. entry.name)
        if count == 0 then
            error("array field " .. entry.name .. " must have count > 0", 4)
        end
        return {
            name = entry.name,
            kind = "array",
            type = normalized,
            count = count,
            size = info.size * count,
            align = info.align,
        }
    end

    if normalized == "pointer" then
        return {
            name = entry.name,
            kind = "pointer",
            size = info.size,
            align = info.align,
        }
    end

    return {
        name = entry.name,
        kind = "prim",
        type = normalized,
        size = info.size,
        align = info.align,
    }
end

compile_schema = function(schema)
    if type(schema) ~= "table" then
        error("schema must be a table", 3)
    end
    local cached = SCHEMA_CACHE[schema]
    if cached then
        return cached
    end
    if SCHEMA_STACK[schema] then
        error("recursive by-value schema is not supported", 3)
    end

    SCHEMA_STACK[schema] = true
    local ok, result = pcall(function()
        local kind = schema.__union and "union" or "struct"
        local fields = {}
        local field_map = {}
        local offset = 0
        local max_align = 1
        local max_size = 0

        for i, entry in ipairs(schema) do
            local field = compile_field(entry)
            if field_map[field.name] then
                error("duplicate field name: " .. field.name, 4)
            end
            max_align = math.max(max_align, field.align)
            if kind == "union" then
                field.offset = 0
                max_size = math.max(max_size, field.size)
            else
                offset = align_up(offset, field.align)
                field.offset = offset
                offset = offset + field.size
            end
            fields[i] = field
            field_map[field.name] = field
        end

        return {
            kind = kind,
            align = max_align,
            size = align_up(kind == "union" and max_size or offset, max_align),
            fields = fields,
            field_map = field_map,
        }
    end)
    SCHEMA_STACK[schema] = nil
    if not ok then
        error(result, 3)
    end
    SCHEMA_CACHE[schema] = result
    return result
end

local function read_primitive(ptr, kind)
    if kind == "bool" then return native.readu8(ptr) ~= 0 end
    if kind == "i8" then return native.readi8(ptr) end
    if kind == "u8" then return native.readu8(ptr) end
    if kind == "i16" then return native.readi16(ptr) end
    if kind == "u16" then return native.readu16(ptr) end
    if kind == "i32" then return native.readi32(ptr) end
    if kind == "u32" then return native.readu32(ptr) end
    if kind == "i64" then return native.readi64(ptr) end
    if kind == "u64" then return native.readu64(ptr) end
    if kind == "f32" then return native.readf32(ptr) end
    if kind == "f64" then return native.readf64(ptr) end
    if kind == "pointer" or kind == "cstring" then return native.readptr(ptr) end
    error("unsupported read type: " .. tostring(kind), 3)
end

local function write_primitive(ptr, kind, value)
    if kind == "bool" then native.writeu8(ptr, value and 1 or 0); return end
    if kind == "i8" then native.writei8(ptr, value); return end
    if kind == "u8" then native.writeu8(ptr, value); return end
    if kind == "i16" then native.writei16(ptr, value); return end
    if kind == "u16" then native.writeu16(ptr, value); return end
    if kind == "i32" then native.writei32(ptr, value); return end
    if kind == "u32" then native.writeu32(ptr, value); return end
    if kind == "i64" then native.writei64(ptr, value); return end
    if kind == "u64" then native.writeu64(ptr, value); return end
    if kind == "f32" then native.writef32(ptr, value); return end
    if kind == "f64" then native.writef64(ptr, value); return end
    if kind == "pointer" or kind == "cstring" then native.writeptr(ptr, value); return end
    error("unsupported write type: " .. tostring(kind), 3)
end

local function read_array(ptr, kind, count)
    local normalized = normalize_type(kind)
    local info = TYPE_INFO[normalized]
    local values = {}
    count = to_non_negative_int(count, "count")
    for i = 1, count do
        values[i] = read_primitive(ptr + (i - 1) * info.size, normalized)
    end
    return values
end

local function write_array(ptr, kind, values)
    local normalized = normalize_type(kind)
    local info = TYPE_INFO[normalized]
    if type(values) ~= "table" then
        error("values must be an array table", 3)
    end
    for i, value in ipairs(values) do
        write_primitive(ptr + (i - 1) * info.size, normalized, value)
    end
end

local create_view

local function create_view_array(ptr, schema_meta, count)
    local length = to_non_negative_int(count, "count")
    local size = schema_meta.size
    return setmetatable({ ptr = ptr, count = length, length = length }, {
        __index = function(_, key)
            if key == "ptr" then return ptr end
            if key == "count" or key == "length" then return length end
            if type(key) == "number" and key % 1 == 0 and key >= 1 and key <= length then
                return create_view(ptr + (key - 1) * size, schema_meta)
            end
            return nil
        end,
        __len = function()
            return length
        end,
    })
end

create_view = function(ptr, schema_meta)
    return setmetatable({ ptr = ptr }, {
        __index = function(_, key)
            if key == "ptr" then return ptr end
            local field = schema_meta.field_map[key]
            if not field then return nil end
            local addr = ptr + field.offset
            if field.kind == "prim" then
                return read_primitive(addr, field.type)
            elseif field.kind == "pointer" then
                return native.readptr(addr)
            elseif field.kind == "array" then
                return read_array(addr, field.type, field.count)
            elseif field.kind == "struct" then
                return create_view(addr, field.schema)
            end
            return nil
        end,
        __newindex = function(_, key, value)
            local field = schema_meta.field_map[key]
            if not field then
                rawset(_, key, value)
                return
            end
            local addr = ptr + field.offset
            if field.kind == "prim" then
                write_primitive(addr, field.type, value)
                return
            end
            if field.kind == "pointer" then
                native.writeptr(addr, value)
                return
            end
            error("field " .. tostring(key) .. " is not directly assignable", 2)
        end,
    })
end

local ffi = {
    flags = native.dlopen_flags,
}

function ffi.open(path, flags)
    return native.open(tostring(path), flags or 0)
end

function ffi.close(handle)
    if handle ~= nil then
        native.close(handle)
    end
end

function ffi.sym(handle, name)
    return native.sym(handle, tostring(name))
end

function ffi.sym_self(name)
    return native.sym_self(tostring(name))
end

function ffi.bind(ptr, sig)
    local handle = native.bind(ptr, tostring(sig))
    return setmetatable({ handle = handle, sig = tostring(sig) }, {
        __call = function(self, ...)
            return native.call_bound(self.handle, { ... })
        end,
    })
end

function ffi.callback(sig, fn)
    return native.callback(tostring(sig), fn)
end

function ffi.errno()
    return native.errno_value()
end

function ffi.dlerror()
    return native.dlerror()
end

local memory = {
    alloc = native.alloc,
    free = native.free,
    realloc = native.realloc,
    zero = native.zero,
    copy = native.copy,
    set = native.set,
    compare = native.compare,
    nullptr = native.nullptr,
    readptr = native.readptr,
    writeptr = native.writeptr,
    readcstring = native.readcstring,
    writecstring = native.writecstring,
    readi8 = native.readi8,
    readu8 = native.readu8,
    readi16 = native.readi16,
    readu16 = native.readu16,
    readi32 = native.readi32,
    readu32 = native.readu32,
    readi64 = native.readi64,
    readu64 = native.readu64,
    readf32 = native.readf32,
    readf64 = native.readf64,
    writei8 = native.writei8,
    writeu8 = native.writeu8,
    writei16 = native.writei16,
    writeu16 = native.writeu16,
    writei32 = native.writei32,
    writeu32 = native.writeu32,
    writei64 = native.writei64,
    writeu64 = native.writeu64,
    writef32 = native.writef32,
    writef64 = native.writef64,
}

function memory.sizeof_ptr()
    return PTR_SIZE
end

function memory.alloc_str(value)
    local text = tostring(value)
    local ptr = native.alloc(#text + 1)
    native.writecstring(ptr, text)
    return ptr
end

memory.allocStr = memory.alloc_str
memory.read_array = read_array
memory.readArray = read_array
memory.write_array = write_array
memory.writeArray = write_array

function memory.struct_sizeof(schema)
    return compile_schema(schema).size
end

function memory.struct_offsetof(schema, field_name)
    local meta = compile_schema(schema)
    local field = meta.field_map[tostring(field_name)]
    if not field then
        error("field not found: " .. tostring(field_name), 2)
    end
    return field.offset
end

function memory.view(ptr, schema)
    return create_view(ptr, compile_schema(schema))
end

function memory.view_array(ptr, schema, count)
    return create_view_array(ptr, compile_schema(schema), count)
end

memory.viewArray = memory.view_array

return {
    ffi = ffi,
    memory = memory,
}
