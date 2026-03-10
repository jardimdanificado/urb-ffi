local function dirname(path)
    return path:match("^(.*)/[^/]+$") or "."
end

local function normalize_path(path)
    -- Simple normalization: resolve ".." components
    local parts = {}
    for part in path:gmatch("[^/]+") do
        if part == ".." and #parts > 0 and parts[#parts] ~= ".." then
            table.remove(parts)
        elseif part ~= "." then
            table.insert(parts, part)
        end
    end
    if #parts == 0 then
        return "."
    end
    return table.concat(parts, "/")
end

local function module_dir()
    local source = debug.getinfo(1, "S").source
    if source:sub(1, 1) ~= "@" then
        error("urb_ffi.lua must be loaded from a file", 2)
    end
    local file = source:sub(2)
    return normalize_path(dirname(file))
end

local function load_native()
    local lua_dir = module_dir()
    local candidates = {
        lua_dir .. "/urb_ffi_native.so",
        dirname(dirname(lua_dir)) .. "/dist/urb_ffi_native.so",
        lua_dir .. "/../../dist/urb_ffi_native.so",  -- For bindings/lua location
    }

    for _, so_path in ipairs(candidates) do
        local loader = package.loadlib(so_path, "luaopen_urb_ffi_native")
        if loader then
            return loader()
        end
    end

    error("failed to load urb_ffi_native.so from known locations", 2)
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

local function make_abi_type(kind, props)
    local out = { __ffi_abi_type = true, kind = kind }
    if props then
        for k, v in pairs(props) do
            out[k] = v
        end
    end
    return out
end

local function is_abi_type(value)
    return type(value) == "table" and value.__ffi_abi_type == true
end

local function is_ffi_descriptor(value)
    return type(value) == "table" and value.__ffi_descriptor == true
end

local function sanitize_abi_name(name)
    local text = tostring(name or "fn")
    text = text:gsub("^%s+", ""):gsub("%s+$", "")
    local safe = text:gsub("[^%w_]", "_")
    if safe == "" then safe = "fn_anon" end
    if not safe:match("^[A-Za-z_]") then
        safe = "fn_" .. safe
    end
    return safe
end

local function normalize_abi_type(value, what)
    what = what or "type"
    if is_abi_type(value) then
        return value
    end
    if type(value) == "string" then
        local text = value:match("^%s*(.-)%s*$")
        if text == "void" then
            return make_abi_type("void")
        end
        local normalized = normalize_type(text)
        if normalized == "pointer" then
            return make_abi_type("pointer", { to = nil })
        end
        if normalized == "cstring" then
            return make_abi_type("cstring")
        end
        return make_abi_type("primitive", { name = normalized })
    end
    error(what .. " must be a type string or ffi.type.* descriptor", 3)
end

local function normalize_abi_field_entries(fields, what)
    what = what or "fields"
    if type(fields) ~= "table" then
        error(what .. " must be a table", 3)
    end

    local out = {}
    local max_index = 0
    for k in pairs(fields) do
        if type(k) == "number" and k > max_index then
            max_index = k
        end
    end

    if max_index > 0 then
        for i = 1, max_index do
            local entry = fields[i]
            if type(entry) == "table" and entry.name ~= nil then
                if type(entry.name) ~= "string" or entry.name == "" then
                    error(what .. "[" .. i .. "] must have a non-empty name", 3)
                end
                out[#out + 1] = {
                    name = entry.name,
                    type = normalize_abi_type(entry.type, what .. "[" .. i .. "] type"),
                }
            elseif type(entry) == "table" and #entry == 2 then
                out[#out + 1] = {
                    name = tostring(entry[1]),
                    type = normalize_abi_type(entry[2], what .. "[" .. i .. "] type"),
                }
            else
                error(what .. "[" .. i .. "] must be { name = ..., type = ... } or { name, type }", 3)
            end
        end
        return out
    end

    for name, field_type in pairs(fields) do
        out[#out + 1] = {
            name = tostring(name),
            type = normalize_abi_type(field_type, what .. "." .. tostring(name)),
        }
    end
    table.sort(out, function(a, b) return a.name < b.name end)
    return out
end

local abi_type_to_string

local function abi_function_to_string(fn_type)
    local prefix = ""
    if fn_type.abi and fn_type.abi ~= "default" then
        prefix = "abi(" .. fn_type.abi .. ") "
    end
    local args = {}
    for i = 1, #fn_type.args do
        args[#args + 1] = abi_type_to_string(fn_type.args[i])
    end
    if fn_type.varargs then
        args[#args + 1] = "..."
    end
    return prefix .. abi_type_to_string(fn_type.ret) .. " " .. sanitize_abi_name(fn_type.name) .. "(" .. table.concat(args, ", ") .. ")"
end

abi_type_to_string = function(value)
    local abi_type = normalize_abi_type(value)
    if abi_type.kind == "void" then
        return "void"
    elseif abi_type.kind == "primitive" then
        return abi_type.name
    elseif abi_type.kind == "cstring" then
        return "cstring"
    elseif abi_type.kind == "pointer" then
        if abi_type.to ~= nil then
            return "pointer(" .. abi_type_to_string(abi_type.to) .. ")"
        end
        return "pointer"
    elseif abi_type.kind == "array" then
        return "array(" .. abi_type_to_string(abi_type.element) .. ", " .. tostring(abi_type.length) .. ")"
    elseif abi_type.kind == "struct" or abi_type.kind == "union" then
        local parts = {}
        for i = 1, #abi_type.fields do
            local field = abi_type.fields[i]
            parts[#parts + 1] = field.name .. ": " .. abi_type_to_string(field.type)
        end
        return abi_type.kind .. "(" .. table.concat(parts, ", ") .. ")"
    elseif abi_type.kind == "enum" then
        return "enum(" .. abi_type_to_string(abi_type.underlying) .. ")"
    elseif abi_type.kind == "function" then
        return abi_function_to_string(abi_type)
    end
    error("unsupported ABI type kind: " .. tostring(abi_type.kind), 2)
end

local function abi_type_to_native_leaf(value)
    local abi_type = normalize_abi_type(value)
    if abi_type.kind == "void" then
        return "void"
    elseif abi_type.kind == "primitive" then
        return abi_type.name
    elseif abi_type.kind == "cstring" then
        return "cstring"
    elseif abi_type.kind == "pointer" then
        return "pointer"
    elseif abi_type.kind == "enum" then
        return abi_type_to_native_leaf(abi_type.underlying)
    end
    return nil
end

local function abi_function_to_native_signature(fn_type)
    if fn_type.abi and fn_type.abi ~= "default" then
        return nil
    end
    local ret = abi_type_to_native_leaf(fn_type.ret)
    if not ret then return nil end
    local args = {}
    for i = 1, #fn_type.args do
        local arg = abi_type_to_native_leaf(fn_type.args[i])
        if not arg then return nil end
        args[#args + 1] = arg
    end
    if fn_type.varargs then
        args[#args + 1] = "..."
    end
    return ret .. " " .. sanitize_abi_name(fn_type.name) .. "(" .. table.concat(args, ", ") .. ")"
end

local function create_ffi_descriptor(sig, handle, abi_type, native_sig)
    return {
        __ffi_descriptor = true,
        handle = handle,
        sig = tostring(sig),
        type = abi_type,
        native_sig = native_sig or tostring(sig),
        native_capable = handle ~= nil,
    }
end

local function describe_function_type(fn_type)
    local sig = abi_function_to_string(fn_type)
    local native_sig = abi_function_to_native_signature(fn_type)
    local handle = native_sig and native.describe(native_sig) or nil
    return create_ffi_descriptor(sig, handle, fn_type, native_sig or sig)
end

local function ensure_ffi_descriptor(value, context)
    context = context or "ffi"
    if is_ffi_descriptor(value) then
        return value
    end
    if type(value) == "string" then
        local text = tostring(value)
        return create_ffi_descriptor(text, native.describe(text), nil, text)
    end
    if is_abi_type(value) and value.kind == "function" then
        return describe_function_type(value)
    end
    error(context .. " expects a signature string, ffi.type.func(...), or descriptor", 3)
end

local function abi_type_to_layout_schema(value)
    local abi_type = normalize_abi_type(value)
    if abi_type.kind == "primitive" then
        return abi_type.name
    elseif abi_type.kind == "cstring" then
        return "cstring"
    elseif abi_type.kind == "pointer" or abi_type.kind == "function" then
        return { __pointer = true }
    elseif abi_type.kind == "enum" then
        return abi_type_to_layout_schema(abi_type.underlying)
    elseif abi_type.kind == "array" then
        local element = abi_type_to_layout_schema(abi_type.element)
        if type(element) ~= "string" then
            error("only arrays of scalar/pointer-compatible elements are layout-compatible in this binding", 3)
        end
        return { element, abi_type.length }
    elseif abi_type.kind == "struct" or abi_type.kind == "union" then
        local schema = abi_type.kind == "union" and { __union = true } or {}
        for i = 1, #abi_type.fields do
            local field = abi_type.fields[i]
            schema[field.name] = abi_type_to_layout_schema(field.type)
        end
        return schema
    end
    error("type " .. abi_type_to_string(abi_type) .. " cannot be converted to a memory layout", 3)
end

local function abi_type_size(value)
    local abi_type = normalize_abi_type(value)
    if abi_type.kind == "void" then
        return 0
    elseif abi_type.kind == "primitive" then
        return TYPE_INFO[abi_type.name].size
    elseif abi_type.kind == "cstring" or abi_type.kind == "pointer" or abi_type.kind == "function" then
        return PTR_SIZE
    elseif abi_type.kind == "enum" then
        return abi_type_size(abi_type.underlying)
    elseif abi_type.kind == "array" then
        return abi_type_size(abi_type.element) * abi_type.length
    elseif abi_type.kind == "struct" or abi_type.kind == "union" then
        return compile_schema(abi_type_to_layout_schema(abi_type)).size
    end
    error("unsupported ABI type kind: " .. tostring(abi_type.kind), 3)
end

local function normalize_global_type(value)
    if is_ffi_descriptor(value) then
        if not value.type then
            error("ffi.global requires a type descriptor or a manual function descriptor", 3)
        end
        if value.type.kind == "function" then
            return make_abi_type("pointer", { to = value.type })
        end
        return value.type
    end
    local abi_type = normalize_abi_type(value, "global type")
    if abi_type.kind == "function" then
        return make_abi_type("pointer", { to = abi_type })
    end
    return abi_type
end

local function read_abi_value(ptr, value)
    local abi_type = normalize_abi_type(value)
    if abi_type.kind == "primitive" then
        return read_primitive(ptr, abi_type.name)
    elseif abi_type.kind == "enum" then
        return read_abi_value(ptr, abi_type.underlying)
    elseif abi_type.kind == "cstring" or abi_type.kind == "pointer" or abi_type.kind == "function" then
        return native.readptr(ptr)
    end
    error("ffi.global cannot read " .. abi_type_to_string(abi_type) .. " directly", 3)
end

local function write_abi_value(ptr, abi_type_value, value)
    local abi_type = normalize_abi_type(abi_type_value)
    if abi_type.kind == "primitive" then
        write_primitive(ptr, abi_type.name, value)
        return
    elseif abi_type.kind == "enum" then
        write_abi_value(ptr, abi_type.underlying, value)
        return
    elseif abi_type.kind == "cstring" or abi_type.kind == "pointer" or abi_type.kind == "function" then
        native.writeptr(ptr, value)
        return
    end
    error("ffi.global cannot write " .. abi_type_to_string(abi_type) .. " directly", 3)
end

local function create_scalar_global(ptr, abi_type_value)
    local abi_type = normalize_abi_type(abi_type_value)
    return setmetatable({ ptr = ptr, type = abi_type }, {
        __index = function(self, key)
            if key == "ptr" then return ptr end
            if key == "type" then return abi_type end
            if key == "value" then return read_abi_value(ptr, abi_type) end
            if key == "read" then
                return function()
                    return read_abi_value(ptr, abi_type)
                end
            end
            if key == "write" then
                return function(_, value)
                    write_abi_value(ptr, abi_type, value)
                end
            end
            return rawget(self, key)
        end,
        __newindex = function(self, key, value)
            if key == "value" then
                write_abi_value(ptr, abi_type, value)
                return
            end
            rawset(self, key, value)
        end,
    })
end

local function create_array_global(ptr, abi_type_value)
    local abi_type = normalize_abi_type(abi_type_value)
    if abi_type.element.kind == "struct" or abi_type.element.kind == "union" then
        return create_view_array(ptr, compile_schema(abi_type_to_layout_schema(abi_type.element)), abi_type.length)
    end
    local stride = abi_type_size(abi_type.element)
    return setmetatable({ ptr = ptr, len = abi_type.length, count = abi_type.length }, {
        __index = function(self, key)
            if key == "ptr" then return ptr end
            if key == "len" or key == "count" or key == "length" then return abi_type.length end
            if key == "to_array" then
                return function()
                    local out = {}
                    for i = 1, abi_type.length do
                        out[i] = read_abi_value(ptr + (i - 1) * stride, abi_type.element)
                    end
                    return out
                end
            end
            if type(key) == "number" and key >= 1 and key <= abi_type.length and key % 1 == 0 then
                return read_abi_value(ptr + (key - 1) * stride, abi_type.element)
            end
            return rawget(self, key)
        end,
        __newindex = function(self, key, value)
            if type(key) == "number" and key >= 1 and key <= abi_type.length and key % 1 == 0 then
                write_abi_value(ptr + (key - 1) * stride, abi_type.element, value)
                return
            end
            rawset(self, key, value)
        end,
    })
end

local ffi_type = {}

function ffi_type.primitive(name)
    return normalize_abi_type(name, "primitive type")
end

function ffi_type.void()
    return make_abi_type("void")
end

function ffi_type.cstring()
    return make_abi_type("cstring")
end

function ffi_type.pointer(to)
    if to == nil then
        return make_abi_type("pointer", { to = nil })
    end
    return make_abi_type("pointer", { to = normalize_abi_type(to, "pointer target") })
end

function ffi_type.array(element, length)
    local count = to_non_negative_int(length, "array length")
    if count <= 0 then
        error("array length must be greater than zero", 2)
    end
    return make_abi_type("array", {
        element = normalize_abi_type(element, "array element"),
        length = count,
    })
end

function ffi_type.struct(fields, options)
    options = options or {}
    return make_abi_type("struct", {
        name = options.name and tostring(options.name) or nil,
        fields = normalize_abi_field_entries(fields, "struct fields"),
    })
end

function ffi_type.union(fields, options)
    options = options or {}
    return make_abi_type("union", {
        name = options.name and tostring(options.name) or nil,
        fields = normalize_abi_field_entries(fields, "union fields"),
    })
end

function ffi_type.enum(values, options)
    options = options or {}
    return make_abi_type("enum", {
        values = type(values) == "table" and values or {},
        underlying = normalize_abi_type(options.underlying or options.base or "i32", "enum underlying type"),
    })
end

function ffi_type.func(ret, args, options)
    options = options or {}
    args = args or {}
    if type(args) ~= "table" then
        error("ffi.type.func args must be a table", 2)
    end
    local arg_types = {}
    for i = 1, #args do
        arg_types[i] = normalize_abi_type(args[i], "function arg " .. i)
    end
    return make_abi_type("function", {
        name = sanitize_abi_name(options.name or "fn"),
        abi = options.abi and tostring(options.abi) or "default",
        varargs = not not options.varargs,
        ret = normalize_abi_type(ret, "function return type"),
        args = arg_types,
    })
end

function ffi_type.layout(abi_type_value)
    return abi_type_to_layout_schema(normalize_abi_type(abi_type_value))
end

function ffi_type.sizeof(abi_type_value)
    return abi_type_size(abi_type_value)
end

function ffi_type.offsetof(abi_type_value, field_name)
    local abi_type = normalize_abi_type(abi_type_value)
    if abi_type.kind ~= "struct" and abi_type.kind ~= "union" then
        error("ffi.type.offsetof expects a struct or union type", 2)
    end
    local meta = compile_schema(abi_type_to_layout_schema(abi_type))
    local field = meta.field_map[tostring(field_name)]
    if not field then
        error("field not found: " .. tostring(field_name), 2)
    end
    return field.offset
end

for _, name in ipairs({ "bool", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64" }) do
    ffi_type[name] = function()
        return make_abi_type("primitive", { name = name })
    end
end

local ffi = {
    flags = native.dlopen_flags,
    type = ffi_type,
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

function ffi.describe(sig)
    return ensure_ffi_descriptor(sig, "ffi.describe")
end

function ffi.bind(ptr, descriptor)
    local desc = ensure_ffi_descriptor(descriptor, "ffi.bind")
    if desc.handle == nil then
        error("ffi.bind descriptor is not natively callable yet: " .. tostring(desc.sig), 2)
    end
    local handle = native.bind(ptr, desc.handle)
    return setmetatable({ handle = handle, sig = tostring(desc.sig or ""), descriptor = desc }, {
        __call = function(self, ...)
            return native.call_bound(self.handle, { ... })
        end,
    })
end

function ffi.callback(descriptor, fn)
    local desc = ensure_ffi_descriptor(descriptor, "ffi.callback")
    if desc.handle == nil then
        error("ffi.callback descriptor is not natively callable yet: " .. tostring(desc.sig), 2)
    end
    local result = native.callback(desc.handle, fn)
    if type(result) == "table" then
        result.sig = tostring(desc.sig or "")
    end
    return result
end

function ffi.global(ptr, descriptor)
    local abi_type = normalize_global_type(descriptor)
    if abi_type.kind == "struct" or abi_type.kind == "union" then
        return create_view(ptr, compile_schema(abi_type_to_layout_schema(abi_type)))
    end
    if abi_type.kind == "array" then
        return create_array_global(ptr, abi_type)
    end
    return create_scalar_global(ptr, abi_type)
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
