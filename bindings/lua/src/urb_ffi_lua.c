#include <lua.h>
#include <lauxlib.h>

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define URBC_IMPLEMENTATION
#include "../../../dist/urbc.h"

typedef struct AddonState AddonState;
typedef struct CallbackBinding CallbackBinding;

typedef struct LuaLibHandle {
    AddonState *state;
    void *handle;
    int closed;
} LuaLibHandle;

typedef struct LuaBoundHandle {
    AddonState *state;
    void *handle;
} LuaBoundHandle;

typedef struct LuaDescriptorHandle {
    AddonState *state;
    UrbcFfiDescriptor *descriptor;
} LuaDescriptorHandle;

typedef struct LuaCallbackHandle {
    AddonState *state;
    void *fn_ptr;
    CallbackBinding *binding;
} LuaCallbackHandle;

struct CallbackBinding {
    lua_State *L;
    int fn_ref;
    FsigParsed sig;
    CallbackBinding *next;
};

struct AddonState {
    UrbcRuntime rt;
    UrbcImage image;
    uint64_t next_callback_id;
    CallbackBinding *callbacks;
};

#define URB_LUA_LIB_MT "urb_ffi.lib"
#define URB_LUA_BOUND_MT "urb_ffi.bound"
#define URB_LUA_DESC_MT "urb_ffi.desc"
#define URB_LUA_CALLBACK_MT "urb_ffi.callback"
#define URB_LUA_STATE_MT "urb_ffi.state"

static AddonState *urblua_state(lua_State *L)
{
    return (AddonState *)lua_touserdata(L, lua_upvalueindex(1));
}

static int urblua_error(lua_State *L, const char *msg)
{
    lua_pushstring(L, msg ? msg : "urb-ffi error");
    return lua_error(L);
}

static int urblua_is_nullish(lua_State *L, int idx)
{
    int t = lua_type(L, idx);
    return t == LUA_TNIL || t == LUA_TNONE;
}

static int urblua_dup_string(lua_State *L, int idx, char **out, char *err, size_t err_cap)
{
    size_t len = 0;
    const char *str;
    char *buf;

    if (!out) return 0;
    *out = NULL;
    if (lua_type(L, idx) != LUA_TSTRING) {
        snprintf(err, err_cap, "expected string");
        return 0;
    }
    str = lua_tolstring(L, idx, &len);
    buf = (char *)malloc(len + 1);
    if (!buf) {
        snprintf(err, err_cap, "out of memory");
        return 0;
    }
    memcpy(buf, str, len);
    buf[len] = '\0';
    *out = buf;
    return 1;
}

static int urblua_get_bool(lua_State *L, int idx, int *out, char *err, size_t err_cap)
{
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN:
        *out = lua_toboolean(L, idx) ? 1 : 0;
        return 1;
    case LUA_TNUMBER:
        *out = (lua_tonumber(L, idx) != 0.0) ? 1 : 0;
        return 1;
    default:
        snprintf(err, err_cap, "expected boolean-like value");
        return 0;
    }
}

static int urblua_get_int64(lua_State *L, int idx, int64_t *out, char *err, size_t err_cap)
{
    lua_Number num;

    switch (lua_type(L, idx)) {
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) {
            *out = (int64_t)lua_tointeger(L, idx);
            return 1;
        }
        num = lua_tonumber(L, idx);
        if (!isfinite((double)num)) {
            snprintf(err, err_cap, "expected finite number");
            return 0;
        }
        *out = (int64_t)num;
        return 1;
    case LUA_TBOOLEAN:
        *out = lua_toboolean(L, idx) ? 1 : 0;
        return 1;
    default:
        snprintf(err, err_cap, "expected integer-like value");
        return 0;
    }
}

static int urblua_get_uint64(lua_State *L, int idx, uint64_t *out, char *err, size_t err_cap)
{
    lua_Number num;
    lua_Integer i;

    switch (lua_type(L, idx)) {
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) {
            i = lua_tointeger(L, idx);
            if (i < 0) {
                snprintf(err, err_cap, "expected non-negative integer");
                return 0;
            }
            *out = (uint64_t)i;
            return 1;
        }
        num = lua_tonumber(L, idx);
        if (!isfinite((double)num) || num < 0.0) {
            snprintf(err, err_cap, "expected non-negative number");
            return 0;
        }
        *out = (uint64_t)num;
        return 1;
    case LUA_TBOOLEAN:
        *out = lua_toboolean(L, idx) ? 1u : 0u;
        return 1;
    default:
        snprintf(err, err_cap, "expected unsigned integer-like value");
        return 0;
    }
}

static int urblua_get_double(lua_State *L, int idx, double *out, char *err, size_t err_cap)
{
    if (!lua_isnumber(L, idx)) {
        snprintf(err, err_cap, "expected numeric value");
        return 0;
    }
    *out = (double)lua_tonumber(L, idx);
    if (!isfinite(*out)) {
        snprintf(err, err_cap, "expected finite number");
        return 0;
    }
    return 1;
}

static int urblua_get_pointer_depth(lua_State *L, int idx, uintptr_t *out, int depth,
                                    char *err, size_t err_cap)
{
    int type;
    lua_Number num;
    lua_Integer i;
    void *p;
    int abs_idx;

    if (!out) return 0;
    if (depth > 4) {
        snprintf(err, err_cap, "pointer indirection too deep");
        return 0;
    }
    if (urblua_is_nullish(L, idx)) {
        *out = (uintptr_t)0;
        return 1;
    }

    type = lua_type(L, idx);
    switch (type) {
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) {
            i = lua_tointeger(L, idx);
            if (i < 0) {
                snprintf(err, err_cap, "expected non-negative pointer value");
                return 0;
            }
            *out = (uintptr_t)(uint64_t)i;
            return 1;
        }
        num = lua_tonumber(L, idx);
        if (!isfinite((double)num) || num < 0.0) {
            snprintf(err, err_cap, "expected non-negative pointer value");
            return 0;
        }
        *out = (uintptr_t)num;
        return 1;
    case LUA_TLIGHTUSERDATA:
        p = lua_touserdata(L, idx);
        *out = (uintptr_t)p;
        return 1;
    case LUA_TTABLE:
    case LUA_TUSERDATA:
        abs_idx = lua_absindex(L, idx);
        lua_getfield(L, abs_idx, "ptr");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            snprintf(err, err_cap, "expected pointer-like value");
            return 0;
        }
        if (!urblua_get_pointer_depth(L, -1, out, depth + 1, err, err_cap)) {
            lua_pop(L, 1);
            return 0;
        }
        lua_pop(L, 1);
        return 1;
    default:
        snprintf(err, err_cap, "expected pointer-like value");
        return 0;
    }
}

static int urblua_get_pointer(lua_State *L, int idx, uintptr_t *out, char *err, size_t err_cap)
{
    return urblua_get_pointer_depth(L, idx, out, 0, err, err_cap);
}

static void urblua_push_uintptr(lua_State *L, uintptr_t value)
{
    lua_pushinteger(L, (lua_Integer)(intptr_t)value);
}

static void urblua_push_int64(lua_State *L, int64_t value)
{
    lua_pushinteger(L, (lua_Integer)value);
}

static void urblua_push_uint64(lua_State *L, uint64_t value)
{
    if (value <= (uint64_t)LUA_MAXINTEGER) {
        lua_pushinteger(L, (lua_Integer)value);
    } else {
        lua_pushnumber(L, (lua_Number)value);
    }
}

static int urblua_value_from_fsig(lua_State *L, Value value, FsigBase base)
{
    switch (base) {
    case FSIG_VOID:
        return 0;
    case FSIG_BOOL:
        lua_pushboolean(L, value.i != 0);
        return 1;
    case FSIG_I8:
    case FSIG_I16:
    case FSIG_I32:
        lua_pushinteger(L, (lua_Integer)(int32_t)value.i);
        return 1;
    case FSIG_U8:
    case FSIG_U16:
    case FSIG_U32:
        lua_pushinteger(L, (lua_Integer)(uint32_t)value.u);
        return 1;
    case FSIG_I64:
        urblua_push_int64(L, (int64_t)value.i);
        return 1;
    case FSIG_U64:
        urblua_push_uint64(L, (uint64_t)value.u);
        return 1;
    case FSIG_F32:
    case FSIG_F64:
        lua_pushnumber(L, (lua_Number)value.f);
        return 1;
    case FSIG_CSTRING:
        if (!value.p) {
            lua_pushnil(L);
        } else {
            lua_pushstring(L, (const char *)value.p);
        }
        return 1;
    case FSIG_POINTER:
        urblua_push_uintptr(L, (uintptr_t)value.p);
        return 1;
    default:
        lua_pushstring(L, "unsupported FFI return type");
        lua_error(L);
        return 0;
    }
}

static int urblua_value_to_fsig(lua_State *L, int idx, FsigBase base,
                                Value *out, char **tmp_string,
                                UrbcRuntime *rt, char *err, size_t err_cap)
{
    int64_t i64 = 0;
    uint64_t u64 = 0;
    double num = 0.0;
    uintptr_t ptr = 0;
    int b = 0;
    char *dup = NULL;
    char *owned = NULL;
    size_t len;

    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (tmp_string) *tmp_string = NULL;

    switch (base) {
    case FSIG_VOID:
        return 1;
    case FSIG_BOOL:
        if (!urblua_get_bool(L, idx, &b, err, err_cap)) return 0;
        out->i = b ? 1 : 0;
        return 1;
    case FSIG_I8:
    case FSIG_I16:
    case FSIG_I32:
    case FSIG_I64:
        if (!urblua_get_int64(L, idx, &i64, err, err_cap)) return 0;
        out->i = (Int)i64;
        return 1;
    case FSIG_U8:
    case FSIG_U16:
    case FSIG_U32:
    case FSIG_U64:
        if (!urblua_get_uint64(L, idx, &u64, err, err_cap)) return 0;
        out->u = (UInt)u64;
        return 1;
    case FSIG_F32:
    case FSIG_F64:
        if (!urblua_get_double(L, idx, &num, err, err_cap)) return 0;
        out->f = (Float)num;
        return 1;
    case FSIG_POINTER:
        if (!urblua_get_pointer(L, idx, &ptr, err, err_cap)) return 0;
        out->p = (void *)ptr;
        return 1;
    case FSIG_CSTRING:
        if (urblua_is_nullish(L, idx)) {
            out->p = NULL;
            return 1;
        }
        if (lua_type(L, idx) == LUA_TSTRING) {
            if (!urblua_dup_string(L, idx, &dup, err, err_cap)) return 0;
            if (rt) {
                len = strlen(dup);
                owned = (char *)urbc_runtime_alloc_owned(rt, len + 1);
                if (!owned) {
                    free(dup);
                    snprintf(err, err_cap, "out of memory");
                    return 0;
                }
                memcpy(owned, dup, len + 1);
                free(dup);
                out->p = owned;
                return 1;
            }
            if (tmp_string) *tmp_string = dup;
            out->p = dup;
            return 1;
        }
        if (!urblua_get_pointer(L, idx, &ptr, err, err_cap)) return 0;
        out->p = (void *)ptr;
        return 1;
    default:
        snprintf(err, err_cap, "unsupported FFI value type");
        return 0;
    }
}

static int urblua_guess_vararg_type(lua_State *L, int idx, uint8_t *out_prim,
                                    char *err, size_t err_cap)
{
    lua_Number num;
    lua_Integer i;

    if (!out_prim) return 0;
    if (urblua_is_nullish(L, idx)) {
        *out_prim = URBC_PRIM_POINTER;
        return 1;
    }

    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN:
        *out_prim = URBC_PRIM_BOOL;
        return 1;
    case LUA_TSTRING:
        *out_prim = URBC_PRIM_CSTRING;
        return 1;
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) {
            i = lua_tointeger(L, idx);
            if (i < INT32_MIN || i > INT32_MAX) {
                *out_prim = (i < 0) ? URBC_PRIM_I64 : URBC_PRIM_U64;
            } else {
                *out_prim = URBC_PRIM_I32;
            }
            return 1;
        }
        num = lua_tonumber(L, idx);
        if (!isfinite((double)num)) {
            snprintf(err, err_cap, "expected finite variadic number");
            return 0;
        }
        *out_prim = URBC_PRIM_F64;
        return 1;
    case LUA_TTABLE:
    case LUA_TUSERDATA:
    case LUA_TLIGHTUSERDATA:
        *out_prim = URBC_PRIM_POINTER;
        return 1;
    default:
        snprintf(err, err_cap, "unsupported variadic argument type");
        return 0;
    }
}

static int urblua_value_to_prim(lua_State *L, int idx, uint8_t prim,
                                Value *out, char **tmp_string,
                                UrbcRuntime *rt, char *err, size_t err_cap)
{
    FsigBase base = FSIG_VOID;

    switch (prim) {
    case URBC_PRIM_BOOL: base = FSIG_BOOL; break;
    case URBC_PRIM_I8: base = FSIG_I8; break;
    case URBC_PRIM_U8: base = FSIG_U8; break;
    case URBC_PRIM_I16: base = FSIG_I16; break;
    case URBC_PRIM_U16: base = FSIG_U16; break;
    case URBC_PRIM_I32: base = FSIG_I32; break;
    case URBC_PRIM_U32: base = FSIG_U32; break;
    case URBC_PRIM_I64: base = FSIG_I64; break;
    case URBC_PRIM_U64: base = FSIG_U64; break;
    case URBC_PRIM_F32: base = FSIG_F32; break;
    case URBC_PRIM_F64: base = FSIG_F64; break;
    case URBC_PRIM_POINTER: base = FSIG_POINTER; break;
    case URBC_PRIM_CSTRING: base = FSIG_CSTRING; break;
    default:
        snprintf(err, err_cap, "unsupported variadic primitive type");
        return 0;
    }
    return urblua_value_to_fsig(L, idx, base, out, tmp_string, rt, err, err_cap);
}

static Value urblua_lua_callback(UrbcRuntime *rt, int argc, const Value *argv, void *opaque)
{
    CallbackBinding *binding = (CallbackBinding *)opaque;
    lua_State *L;
    Value out;
    char err[URBC_ERROR_CAP];
    int top;
    int i;

    memset(&out, 0, sizeof(out));
    if (!binding || !(L = binding->L)) {
        urbc_runtime_fail(rt, "ffi.callback: missing Lua callback binding");
        return out;
    }

    top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, binding->fn_ref);
    for (i = 0; i < argc; i++) {
        urblua_value_from_fsig(L, argv[i], binding->sig.args[i].base);
    }
    if (lua_pcall(L, argc, 1, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        urbc_runtime_fail(rt, "ffi.callback: %s", msg ? msg : "Lua callback failed");
        lua_settop(L, top);
        return out;
    }
    if (!urblua_value_to_fsig(L, -1, binding->sig.ret.base, &out, NULL, rt, err, sizeof(err))) {
        urbc_runtime_fail(rt, "ffi.callback: %s", err);
        memset(&out, 0, sizeof(out));
        lua_settop(L, top);
        return out;
    }
    lua_settop(L, top);
    return out;
}

static LuaLibHandle *urblua_check_lib(lua_State *L, int idx)
{
    return (LuaLibHandle *)luaL_checkudata(L, idx, URB_LUA_LIB_MT);
}

static LuaBoundHandle *urblua_check_bound(lua_State *L, int idx)
{
    return (LuaBoundHandle *)luaL_checkudata(L, idx, URB_LUA_BOUND_MT);
}

static LuaDescriptorHandle *urblua_check_desc(lua_State *L, int idx)
{
    return (LuaDescriptorHandle *)luaL_checkudata(L, idx, URB_LUA_DESC_MT);
}

static int urblua_lib_gc(lua_State *L)
{
    LuaLibHandle *wrap = (LuaLibHandle *)luaL_checkudata(L, 1, URB_LUA_LIB_MT);
    if (!wrap->closed && wrap->handle) {
        urbc_ffi_close(wrap->handle);
        wrap->handle = NULL;
        wrap->closed = 1;
    }
    return 0;
}

static int urblua_bound_gc(lua_State *L)
{
    (void)luaL_checkudata(L, 1, URB_LUA_BOUND_MT);
    return 0;
}

static int urblua_desc_gc(lua_State *L)
{
    LuaDescriptorHandle *wrap = (LuaDescriptorHandle *)luaL_checkudata(L, 1, URB_LUA_DESC_MT);
    if (wrap->descriptor) {
        urbc_ffi_descriptor_release(wrap->descriptor);
        wrap->descriptor = NULL;
    }
    return 0;
}

static int urblua_callback_gc(lua_State *L)
{
    (void)luaL_checkudata(L, 1, URB_LUA_CALLBACK_MT);
    return 0;
}

static int urblua_state_gc(lua_State *L)
{
    AddonState *state = (AddonState *)luaL_checkudata(L, 1, URB_LUA_STATE_MT);
    CallbackBinding *cb = state->callbacks;

    while (cb) {
        CallbackBinding *next = cb->next;
        luaL_unref(L, LUA_REGISTRYINDEX, cb->fn_ref);
        free(cb);
        cb = next;
    }
    state->callbacks = NULL;
    urbc_runtime_destroy(&state->rt);
    urbc_image_destroy(&state->image);
    return 0;
}

static int urblua_open(lua_State *L)
{
    AddonState *state = urblua_state(L);
    const char *path = luaL_checkstring(L, 1);
    lua_Integer flags = luaL_optinteger(L, 2, 0);
    char err[URBC_ERROR_CAP];
    void *handle = NULL;
    LuaLibHandle *wrap;

    (void)state;
    if (urbc_ffi_open(path, (int)flags, &handle, err, sizeof(err)) != URBC_OK) {
        lua_pushnil(L);
        return 1;
    }

    wrap = (LuaLibHandle *)lua_newuserdatauv(L, sizeof(*wrap), 0);
    memset(wrap, 0, sizeof(*wrap));
    wrap->state = state;
    wrap->handle = handle;
    luaL_setmetatable(L, URB_LUA_LIB_MT);
    return 1;
}

static int urblua_close(lua_State *L)
{
    LuaLibHandle *wrap = urblua_check_lib(L, 1);
    if (!wrap->closed && wrap->handle) {
        urbc_ffi_close(wrap->handle);
        wrap->handle = NULL;
        wrap->closed = 1;
    }
    return 0;
}

static int urblua_sym(lua_State *L)
{
    LuaLibHandle *wrap = urblua_check_lib(L, 1);
    const char *name = luaL_checkstring(L, 2);
    char err[URBC_ERROR_CAP];
    void *sym = NULL;

    if (wrap->closed || !wrap->handle) {
        urblua_push_uintptr(L, (uintptr_t)0);
        return 1;
    }
    if (urbc_ffi_sym(wrap->handle, name, &sym, err, sizeof(err)) != URBC_OK) {
        urblua_push_uintptr(L, (uintptr_t)0);
        return 1;
    }
    urblua_push_uintptr(L, (uintptr_t)sym);
    return 1;
}

static int urblua_sym_self(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    char err[URBC_ERROR_CAP];
    void *sym = NULL;

    if (urbc_ffi_sym_self(name, &sym, err, sizeof(err)) != URBC_OK) {
        urblua_push_uintptr(L, (uintptr_t)0);
        return 1;
    }
    urblua_push_uintptr(L, (uintptr_t)sym);
    return 1;
}

static int urblua_describe(lua_State *L)
{
    AddonState *state = urblua_state(L);
    const char *sig = luaL_checkstring(L, 1);
    char err[URBC_ERROR_CAP];
    LuaDescriptorHandle *wrap;

    wrap = (LuaDescriptorHandle *)lua_newuserdatauv(L, sizeof(*wrap), 0);
    memset(wrap, 0, sizeof(*wrap));
    wrap->state = state;
    if (urbc_ffi_describe(sig, &wrap->descriptor, err, sizeof(err)) != URBC_OK) {
        lua_pop(L, 1);
        return urblua_error(L, err);
    }
    luaL_setmetatable(L, URB_LUA_DESC_MT);
    return 1;
}

static int urblua_bind(lua_State *L)
{
    AddonState *state = urblua_state(L);
    uintptr_t ptr = 0;
    LuaDescriptorHandle *desc = urblua_check_desc(L, 2);
    char err[URBC_ERROR_CAP];
    void *bound = NULL;
    LuaBoundHandle *wrap;

    if (!urblua_get_pointer(L, 1, &ptr, err, sizeof(err))) {
        return urblua_error(L, err);
    }
    if (ptr == 0) {
        return urblua_error(L, "ffi.bind: function pointer is null");
    }
    if (!desc->descriptor) {
        return urblua_error(L, "ffi.bind: invalid descriptor");
    }
    if (urbc_ffi_bind_desc(&state->rt, (void *)ptr, desc->descriptor, &bound, err, sizeof(err)) != URBC_OK) {
        return urblua_error(L, err);
    }

    wrap = (LuaBoundHandle *)lua_newuserdatauv(L, sizeof(*wrap), 0);
    memset(wrap, 0, sizeof(*wrap));
    wrap->state = state;
    wrap->handle = bound;
    luaL_setmetatable(L, URB_LUA_BOUND_MT);
    return 1;
}

static int urblua_call_bound(lua_State *L)
{
    AddonState *state = urblua_state(L);
    LuaBoundHandle *wrap = urblua_check_bound(L, 1);
    UrbcBoundFn *bound = (UrbcBoundFn *)wrap->handle;
    size_t js_argc;
    Value call_argv[FSIG_MAX_ARGS];
    uint8_t vararg_types[FSIG_MAX_ARGS];
    char *tmp_strings[FSIG_MAX_ARGS];
    Value out;
    char err[URBC_ERROR_CAP];
    size_t i;

    if (!lua_istable(L, 2)) {
        return urblua_error(L, "ffi.call_bound expects an array table");
    }
    if (!bound || bound->magic != URBC_BOUND_MAGIC) {
        return urblua_error(L, "invalid bound function handle");
    }

    memset(tmp_strings, 0, sizeof(tmp_strings));
    memset(&out, 0, sizeof(out));
    js_argc = lua_rawlen(L, 2);
    if (js_argc > FSIG_MAX_ARGS) {
        return urblua_error(L, "too many FFI arguments");
    }

    for (i = 0; i < js_argc; i++) {
        lua_rawgeti(L, 2, (lua_Integer)i + 1);
        if ((int)i < bound->sig.argc) {
            if (!urblua_value_to_fsig(L, -1, bound->sig.args[i].base,
                                      &call_argv[i], &tmp_strings[i], NULL,
                                      err, sizeof(err))) {
                lua_pop(L, 1);
                while (i-- > 0) free(tmp_strings[i]);
                return urblua_error(L, err);
            }
        } else {
            if (!urblua_guess_vararg_type(L, -1, &vararg_types[i - (size_t)bound->sig.argc], err, sizeof(err)) ||
                !urblua_value_to_prim(L, -1, vararg_types[i - (size_t)bound->sig.argc],
                                      &call_argv[i], &tmp_strings[i], NULL,
                                      err, sizeof(err))) {
                lua_pop(L, 1);
                while (i-- > 0) free(tmp_strings[i]);
                return urblua_error(L, err);
            }
        }
        lua_pop(L, 1);
    }

    if (urbc_ffi_call(&state->rt, wrap->handle, (int)js_argc, call_argv,
                      (js_argc > (size_t)bound->sig.argc) ? vararg_types : NULL,
                      &out, err, sizeof(err)) != URBC_OK) {
        for (i = 0; i < js_argc; i++) free(tmp_strings[i]);
        return urblua_error(L, err);
    }
    for (i = 0; i < js_argc; i++) free(tmp_strings[i]);
    return urblua_value_from_fsig(L, out, bound->sig.ret.base);
}

static int urblua_callback(lua_State *L)
{
    AddonState *state = urblua_state(L);
    LuaDescriptorHandle *desc = urblua_check_desc(L, 1);
    char err[URBC_ERROR_CAP];
    char name[64];
    CallbackBinding *binding;
    UrbcHostBinding *host;
    void *fn_ptr = NULL;
    LuaCallbackHandle *wrap;

    luaL_checktype(L, 2, LUA_TFUNCTION);

    binding = (CallbackBinding *)calloc(1, sizeof(*binding));
    if (!binding) {
        return urblua_error(L, "out of memory");
    }
    binding->L = L;
    if (urbc_ffi_descriptor_copy_parsed(desc->descriptor, &binding->sig, err, sizeof(err)) != URBC_OK) {
        free(binding);
        return urblua_error(L, err);
    }

    lua_pushvalue(L, 2);
    binding->fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    binding->next = state->callbacks;
    state->callbacks = binding;

    snprintf(name, sizeof(name), "lua_cb_%llu", (unsigned long long)++state->next_callback_id);
    if (urbc_runtime_register_host(&state->rt, name, urblua_lua_callback, binding,
                                   err, sizeof(err)) != URBC_OK) {
        return urblua_error(L, err);
    }
    host = urbc_runtime_find_host(&state->rt, name);
    if (!host) {
        return urblua_error(L, "ffi.callback: failed to resolve host binding");
    }
    if (!desc->descriptor) {
        return urblua_error(L, "ffi.callback: invalid descriptor");
    }
    if (urbc_ffi_callback_desc(&state->rt, desc->descriptor, (Value){ .p = host }, &fn_ptr, err, sizeof(err)) != URBC_OK) {
        return urblua_error(L, err);
    }

    lua_newtable(L);
    urblua_push_uintptr(L, (uintptr_t)fn_ptr);
    lua_setfield(L, -2, "ptr");

    wrap = (LuaCallbackHandle *)lua_newuserdatauv(L, sizeof(*wrap), 0);
    memset(wrap, 0, sizeof(*wrap));
    wrap->state = state;
    wrap->fn_ptr = fn_ptr;
    wrap->binding = binding;
    luaL_setmetatable(L, URB_LUA_CALLBACK_MT);
    lua_setfield(L, -2, "handle");
    return 1;
}

static int urblua_errno_value(lua_State *L)
{
    (void)urblua_state(L);
    lua_pushinteger(L, (lua_Integer)urbc_ffi_errno_value());
    return 1;
}

static int urblua_dlerror(lua_State *L)
{
    AddonState *state = urblua_state(L);
    char *msg = NULL;
    char err[URBC_ERROR_CAP];

    if (urbc_ffi_dlerror_copy(&state->rt, &msg, err, sizeof(err)) != URBC_OK) {
        return urblua_error(L, err);
    }
    if (!msg) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, msg);
    }
    return 1;
}

static int urblua_mem_alloc(lua_State *L)
{
    uint64_t size = 0;
    char err[URBC_ERROR_CAP];
    void *ptr;

    if (!urblua_get_uint64(L, 1, &size, err, sizeof(err))) {
        return urblua_error(L, err);
    }
    ptr = malloc((size_t)size);
    urblua_push_uintptr(L, (uintptr_t)ptr);
    return 1;
}

static int urblua_mem_free(lua_State *L)
{
    uintptr_t ptr = 0;
    char err[URBC_ERROR_CAP];

    if (!urblua_is_nullish(L, 1)) {
        if (!urblua_get_pointer(L, 1, &ptr, err, sizeof(err))) {
            return urblua_error(L, err);
        }
        free((void *)ptr);
    }
    return 0;
}

static int urblua_mem_realloc(lua_State *L)
{
    uintptr_t ptr = 0;
    uint64_t size = 0;
    char err[URBC_ERROR_CAP];
    void *out;

    if (!urblua_get_pointer(L, 1, &ptr, err, sizeof(err)) ||
        !urblua_get_uint64(L, 2, &size, err, sizeof(err))) {
        return urblua_error(L, err);
    }
    out = realloc((void *)ptr, (size_t)size);
    urblua_push_uintptr(L, (uintptr_t)out);
    return 1;
}

static int urblua_mem_zero(lua_State *L)
{
    uintptr_t ptr = 0;
    uint64_t size = 0;
    char err[URBC_ERROR_CAP];

    if (!urblua_get_pointer(L, 1, &ptr, err, sizeof(err)) ||
        !urblua_get_uint64(L, 2, &size, err, sizeof(err))) {
        return urblua_error(L, err);
    }
    if (ptr && size) memset((void *)ptr, 0, (size_t)size);
    return 0;
}

static int urblua_mem_copy(lua_State *L)
{
    uintptr_t dst = 0;
    uintptr_t src = 0;
    uint64_t size = 0;
    char err[URBC_ERROR_CAP];

    if (!urblua_get_pointer(L, 1, &dst, err, sizeof(err)) ||
        !urblua_get_pointer(L, 2, &src, err, sizeof(err)) ||
        !urblua_get_uint64(L, 3, &size, err, sizeof(err))) {
        return urblua_error(L, err);
    }
    if (dst && src && size) memcpy((void *)dst, (const void *)src, (size_t)size);
    return 0;
}

static int urblua_mem_set(lua_State *L)
{
    uintptr_t ptr = 0;
    int64_t bytev = 0;
    uint64_t size = 0;
    char err[URBC_ERROR_CAP];

    if (!urblua_get_pointer(L, 1, &ptr, err, sizeof(err)) ||
        !urblua_get_int64(L, 2, &bytev, err, sizeof(err)) ||
        !urblua_get_uint64(L, 3, &size, err, sizeof(err))) {
        return urblua_error(L, err);
    }
    if (ptr && size) memset((void *)ptr, (int)(bytev & 0xFF), (size_t)size);
    return 0;
}

static int urblua_mem_compare(lua_State *L)
{
    uintptr_t a = 0;
    uintptr_t b = 0;
    uint64_t size = 0;
    char err[URBC_ERROR_CAP];
    int cmp = 0;

    if (!urblua_get_pointer(L, 1, &a, err, sizeof(err)) ||
        !urblua_get_pointer(L, 2, &b, err, sizeof(err)) ||
        !urblua_get_uint64(L, 3, &size, err, sizeof(err))) {
        return urblua_error(L, err);
    }
    if (a && b && size) cmp = memcmp((const void *)a, (const void *)b, (size_t)size);
    lua_pushinteger(L, (lua_Integer)cmp);
    return 1;
}

static int urblua_mem_nullptr(lua_State *L)
{
    (void)L;
    urblua_push_uintptr(L, (uintptr_t)0);
    return 1;
}

static int urblua_mem_sizeof_ptr(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)sizeof(void *));
    return 1;
}

static int urblua_mem_readptr(lua_State *L)
{
    uintptr_t ptr = 0;
    uintptr_t value = 0;
    char err[URBC_ERROR_CAP];

    if (!urblua_get_pointer(L, 1, &ptr, err, sizeof(err))) {
        return urblua_error(L, err);
    }
    if (ptr) memcpy(&value, (const void *)ptr, sizeof(value));
    urblua_push_uintptr(L, value);
    return 1;
}

static int urblua_mem_writeptr(lua_State *L)
{
    uintptr_t ptr = 0;
    uintptr_t value = 0;
    char err[URBC_ERROR_CAP];

    if (!urblua_get_pointer(L, 1, &ptr, err, sizeof(err)) ||
        !urblua_get_pointer(L, 2, &value, err, sizeof(err))) {
        return urblua_error(L, err);
    }
    if (ptr) memcpy((void *)ptr, &value, sizeof(value));
    return 0;
}

static int urblua_mem_readcstring(lua_State *L)
{
    uintptr_t ptr = 0;
    char err[URBC_ERROR_CAP];

    if (!urblua_get_pointer(L, 1, &ptr, err, sizeof(err))) {
        return urblua_error(L, err);
    }
    if (!ptr) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, (const char *)ptr);
    }
    return 1;
}

static int urblua_mem_writecstring(lua_State *L)
{
    uintptr_t ptr = 0;
    char err[URBC_ERROR_CAP];
    char *str = NULL;

    if (!urblua_get_pointer(L, 1, &ptr, err, sizeof(err))) {
        return urblua_error(L, err);
    }
    if (urblua_is_nullish(L, 2)) {
        if (ptr) ((char *)ptr)[0] = '\0';
        return 0;
    }
    if (!urblua_dup_string(L, 2, &str, err, sizeof(err))) {
        return urblua_error(L, err);
    }
    if (ptr) memcpy((void *)ptr, str, strlen(str) + 1);
    free(str);
    return 0;
}

#define URBLUA_DEFINE_READ_NUM(name, ctype, push_stmt) \
static int urblua_##name(lua_State *L) \
{ \
    uintptr_t ptr = 0; \
    char err[URBC_ERROR_CAP]; \
    ctype value = 0; \
    if (!urblua_get_pointer(L, 1, &ptr, err, sizeof(err))) { \
        return urblua_error(L, err); \
    } \
    if (ptr) memcpy(&value, (const void *)ptr, sizeof(value)); \
    push_stmt; \
    return 1; \
}

#define URBLUA_DEFINE_WRITE_NUM(name, ctype, getter_stmt) \
static int urblua_##name(lua_State *L) \
{ \
    uintptr_t ptr = 0; \
    char err[URBC_ERROR_CAP]; \
    ctype value = 0; \
    if (!urblua_get_pointer(L, 1, &ptr, err, sizeof(err))) { \
        return urblua_error(L, err); \
    } \
    getter_stmt; \
    if (ptr) memcpy((void *)ptr, &value, sizeof(value)); \
    return 0; \
}

URBLUA_DEFINE_READ_NUM(readi8, int8_t, lua_pushinteger(L, (lua_Integer)value))
URBLUA_DEFINE_READ_NUM(readu8, uint8_t, lua_pushinteger(L, (lua_Integer)value))
URBLUA_DEFINE_READ_NUM(readi16, int16_t, lua_pushinteger(L, (lua_Integer)value))
URBLUA_DEFINE_READ_NUM(readu16, uint16_t, lua_pushinteger(L, (lua_Integer)value))
URBLUA_DEFINE_READ_NUM(readi32, int32_t, lua_pushinteger(L, (lua_Integer)value))
URBLUA_DEFINE_READ_NUM(readu32, uint32_t, lua_pushinteger(L, (lua_Integer)value))
URBLUA_DEFINE_READ_NUM(readi64, int64_t, urblua_push_int64(L, value))
URBLUA_DEFINE_READ_NUM(readu64, uint64_t, urblua_push_uint64(L, value))
URBLUA_DEFINE_READ_NUM(readf32, float, lua_pushnumber(L, (lua_Number)value))
URBLUA_DEFINE_READ_NUM(readf64, double, lua_pushnumber(L, (lua_Number)value))

URBLUA_DEFINE_WRITE_NUM(writei8, int8_t, do { int64_t v; if (!urblua_get_int64(L, 2, &v, err, sizeof(err))) return urblua_error(L, err); value = (int8_t)v; } while (0))
URBLUA_DEFINE_WRITE_NUM(writeu8, uint8_t, do { uint64_t v; if (!urblua_get_uint64(L, 2, &v, err, sizeof(err))) return urblua_error(L, err); value = (uint8_t)v; } while (0))
URBLUA_DEFINE_WRITE_NUM(writei16, int16_t, do { int64_t v; if (!urblua_get_int64(L, 2, &v, err, sizeof(err))) return urblua_error(L, err); value = (int16_t)v; } while (0))
URBLUA_DEFINE_WRITE_NUM(writeu16, uint16_t, do { uint64_t v; if (!urblua_get_uint64(L, 2, &v, err, sizeof(err))) return urblua_error(L, err); value = (uint16_t)v; } while (0))
URBLUA_DEFINE_WRITE_NUM(writei32, int32_t, do { int64_t v; if (!urblua_get_int64(L, 2, &v, err, sizeof(err))) return urblua_error(L, err); value = (int32_t)v; } while (0))
URBLUA_DEFINE_WRITE_NUM(writeu32, uint32_t, do { uint64_t v; if (!urblua_get_uint64(L, 2, &v, err, sizeof(err))) return urblua_error(L, err); value = (uint32_t)v; } while (0))
URBLUA_DEFINE_WRITE_NUM(writei64, int64_t, do { int64_t v; if (!urblua_get_int64(L, 2, &v, err, sizeof(err))) return urblua_error(L, err); value = (int64_t)v; } while (0))
URBLUA_DEFINE_WRITE_NUM(writeu64, uint64_t, do { uint64_t v; if (!urblua_get_uint64(L, 2, &v, err, sizeof(err))) return urblua_error(L, err); value = (uint64_t)v; } while (0))
URBLUA_DEFINE_WRITE_NUM(writef32, float, do { double v; if (!urblua_get_double(L, 2, &v, err, sizeof(err))) return urblua_error(L, err); value = (float)v; } while (0))
URBLUA_DEFINE_WRITE_NUM(writef64, double, do { double v; if (!urblua_get_double(L, 2, &v, err, sizeof(err))) return urblua_error(L, err); value = (double)v; } while (0))

static void urblua_setfunc(lua_State *L, int module_index, int state_index,
                           const char *name, lua_CFunction fn)
{
    module_index = lua_absindex(L, module_index);
    state_index = lua_absindex(L, state_index);
    lua_pushvalue(L, state_index);
    lua_pushcclosure(L, fn, 1);
    lua_setfield(L, module_index, name);
}

static void urblua_set_constants(lua_State *L, int module_index)
{
    module_index = lua_absindex(L, module_index);
    lua_newtable(L);
    lua_pushinteger(L, URBC_DLOPEN_LAZY);
    lua_setfield(L, -2, "LAZY");
    lua_pushinteger(L, URBC_DLOPEN_NOW);
    lua_setfield(L, -2, "NOW");
    lua_pushinteger(L, URBC_DLOPEN_LOCAL);
    lua_setfield(L, -2, "LOCAL");
    lua_pushinteger(L, URBC_DLOPEN_GLOBAL);
    lua_setfield(L, -2, "GLOBAL");
    lua_pushinteger(L, URBC_DLOPEN_NODELETE);
    lua_setfield(L, -2, "NODELETE");
    lua_pushinteger(L, URBC_DLOPEN_NOLOAD);
    lua_setfield(L, -2, "NOLOAD");
    lua_setfield(L, module_index, "dlopen_flags");
}

static void urblua_create_metatable(lua_State *L, const char *name, lua_CFunction gc)
{
    if (luaL_newmetatable(L, name)) {
        lua_pushcfunction(L, gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}

int luaopen_urb_ffi_native(lua_State *L)
{
    AddonState *state;

    luaL_checkversion(L);
    urblua_create_metatable(L, URB_LUA_LIB_MT, urblua_lib_gc);
    urblua_create_metatable(L, URB_LUA_BOUND_MT, urblua_bound_gc);
    urblua_create_metatable(L, URB_LUA_DESC_MT, urblua_desc_gc);
    urblua_create_metatable(L, URB_LUA_CALLBACK_MT, urblua_callback_gc);
    urblua_create_metatable(L, URB_LUA_STATE_MT, urblua_state_gc);

    state = (AddonState *)lua_newuserdatauv(L, sizeof(*state), 0);
    memset(state, 0, sizeof(*state));
    luaL_setmetatable(L, URB_LUA_STATE_MT);

    urbc_image_init(&state->image);
    urbc_runtime_init(&state->rt);
    state->rt.image = &state->image;
    state->next_callback_id = 0;
    state->callbacks = NULL;

    lua_newtable(L);
    urblua_setfunc(L, -1, -2, "open", urblua_open);
    urblua_setfunc(L, -1, -2, "close", urblua_close);
    urblua_setfunc(L, -1, -2, "sym", urblua_sym);
    urblua_setfunc(L, -1, -2, "sym_self", urblua_sym_self);
    urblua_setfunc(L, -1, -2, "describe", urblua_describe);
    urblua_setfunc(L, -1, -2, "bind", urblua_bind);
    urblua_setfunc(L, -1, -2, "call_bound", urblua_call_bound);
    urblua_setfunc(L, -1, -2, "callback", urblua_callback);
    urblua_setfunc(L, -1, -2, "errno_value", urblua_errno_value);
    urblua_setfunc(L, -1, -2, "dlerror", urblua_dlerror);
    urblua_setfunc(L, -1, -2, "alloc", urblua_mem_alloc);
    urblua_setfunc(L, -1, -2, "free", urblua_mem_free);
    urblua_setfunc(L, -1, -2, "realloc", urblua_mem_realloc);
    urblua_setfunc(L, -1, -2, "zero", urblua_mem_zero);
    urblua_setfunc(L, -1, -2, "copy", urblua_mem_copy);
    urblua_setfunc(L, -1, -2, "set", urblua_mem_set);
    urblua_setfunc(L, -1, -2, "compare", urblua_mem_compare);
    urblua_setfunc(L, -1, -2, "nullptr", urblua_mem_nullptr);
    urblua_setfunc(L, -1, -2, "sizeof_ptr", urblua_mem_sizeof_ptr);
    urblua_setfunc(L, -1, -2, "readptr", urblua_mem_readptr);
    urblua_setfunc(L, -1, -2, "writeptr", urblua_mem_writeptr);
    urblua_setfunc(L, -1, -2, "readcstring", urblua_mem_readcstring);
    urblua_setfunc(L, -1, -2, "writecstring", urblua_mem_writecstring);
    urblua_setfunc(L, -1, -2, "readi8", urblua_readi8);
    urblua_setfunc(L, -1, -2, "readu8", urblua_readu8);
    urblua_setfunc(L, -1, -2, "readi16", urblua_readi16);
    urblua_setfunc(L, -1, -2, "readu16", urblua_readu16);
    urblua_setfunc(L, -1, -2, "readi32", urblua_readi32);
    urblua_setfunc(L, -1, -2, "readu32", urblua_readu32);
    urblua_setfunc(L, -1, -2, "readi64", urblua_readi64);
    urblua_setfunc(L, -1, -2, "readu64", urblua_readu64);
    urblua_setfunc(L, -1, -2, "readf32", urblua_readf32);
    urblua_setfunc(L, -1, -2, "readf64", urblua_readf64);
    urblua_setfunc(L, -1, -2, "writei8", urblua_writei8);
    urblua_setfunc(L, -1, -2, "writeu8", urblua_writeu8);
    urblua_setfunc(L, -1, -2, "writei16", urblua_writei16);
    urblua_setfunc(L, -1, -2, "writeu16", urblua_writeu16);
    urblua_setfunc(L, -1, -2, "writei32", urblua_writei32);
    urblua_setfunc(L, -1, -2, "writeu32", urblua_writeu32);
    urblua_setfunc(L, -1, -2, "writei64", urblua_writei64);
    urblua_setfunc(L, -1, -2, "writeu64", urblua_writeu64);
    urblua_setfunc(L, -1, -2, "writef32", urblua_writef32);
    urblua_setfunc(L, -1, -2, "writef64", urblua_writef64);
    urblua_set_constants(L, -1);

    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "_state");
    lua_remove(L, -2);
    return 1;
}
