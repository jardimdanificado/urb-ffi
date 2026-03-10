#include <node_api.h>

#include <math.h>
#include <ffi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "urbc.h"
#include "urbc_internal.h"

typedef struct UrbcBoundFn {
    uint32_t magic;
    void *fn_ptr;
    ffi_cif cif;
    ffi_type *ret_type;
    ffi_type *arg_types[FSIG_MAX_ARGS];
    FsigParsed sig;
} UrbcBoundFn;

typedef struct CallbackBinding CallbackBinding;
typedef struct AddonState {
    UrbcRuntime rt;
    UrbcImage image;
    uint64_t next_callback_id;
    CallbackBinding *callbacks;
} AddonState;

typedef struct LibHandle {
    void *handle;
    int closed;
} LibHandle;

typedef struct BoundHandle {
    void *handle;
} BoundHandle;

typedef struct DescriptorHandle {
    UrbcFfiDescriptor *descriptor;
} DescriptorHandle;

typedef struct CallbackHandle {
    void *fn_ptr;
    CallbackBinding *binding;
} CallbackHandle;

struct CallbackBinding {
    napi_env env;
    napi_ref fn_ref;
    FsigParsed sig;
#ifdef _WIN32
    DWORD creator_thread;
#else
    pthread_t creator_thread;
#endif
    CallbackBinding *next;
};

#define NAPI_CALL(env, call) do { \
    napi_status status__ = (call); \
    if (status__ != napi_ok) { \
        urbnapi_throw_status((env), status__, #call); \
        return NULL; \
    } \
} while (0)

static void urbnapi_throw_status(napi_env env, napi_status status, const char *where)
{
    const napi_extended_error_info *info = NULL;
    const char *msg = "N-API error";
    char buf[256];
    (void)napi_get_last_error_info(env, &info);
    if (info && info->error_message) msg = info->error_message;
    snprintf(buf, sizeof(buf), "%s: %s", where, msg);
    (void)napi_throw_error(env, NULL, buf);
}

static napi_value urbnapi_undefined(napi_env env)
{
    napi_value out;
    if (napi_get_undefined(env, &out) != napi_ok) return NULL;
    return out;
}

static int urbnapi_callback_on_creator_thread(const CallbackBinding *binding)
{
    if (!binding) return 0;
#ifdef _WIN32
    return binding->creator_thread == GetCurrentThreadId();
#else
    return pthread_equal(binding->creator_thread, pthread_self());
#endif
}

static AddonState *urbnapi_state(napi_env env)
{
    AddonState *state = NULL;
    if (napi_get_instance_data(env, (void **)&state) != napi_ok) return NULL;
    return state;
}

static void urbnapi_exception_string(napi_env env, char *buf, size_t cap)
{
    napi_value exc;
    napi_value str;
    size_t len = 0;
    char *tmp;

    if (!buf || cap == 0) return;
    buf[0] = '\0';
    if (napi_get_and_clear_last_exception(env, &exc) != napi_ok) {
        snprintf(buf, cap, "JavaScript exception");
        return;
    }
    if (napi_coerce_to_string(env, exc, &str) != napi_ok) {
        snprintf(buf, cap, "JavaScript exception");
        return;
    }
    if (napi_get_value_string_utf8(env, str, NULL, 0, &len) != napi_ok) {
        snprintf(buf, cap, "JavaScript exception");
        return;
    }
    tmp = (char *)malloc(len + 1);
    if (!tmp) {
        snprintf(buf, cap, "JavaScript exception");
        return;
    }
    if (napi_get_value_string_utf8(env, str, tmp, len + 1, &len) != napi_ok) {
        free(tmp);
        snprintf(buf, cap, "JavaScript exception");
        return;
    }
    snprintf(buf, cap, "%s", tmp);
    free(tmp);
}

static int urbnapi_is_nullish(napi_env env, napi_value value)
{
    napi_value nullv;
    bool eq = false;
    napi_valuetype type;

    if (napi_typeof(env, value, &type) != napi_ok) return 0;
    if (type == napi_undefined) return 1;
    if (napi_get_null(env, &nullv) != napi_ok) return 0;
    if (napi_strict_equals(env, value, nullv, &eq) != napi_ok) return 0;
    return eq ? 1 : 0;
}

static int urbnapi_dup_string(napi_env env, napi_value value, char **out, char *err, size_t err_cap)
{
    size_t len = 0;
    char *buf;
    if (!out) return 0;
    *out = NULL;
    if (napi_get_value_string_utf8(env, value, NULL, 0, &len) != napi_ok) {
        snprintf(err, err_cap, "expected string");
        return 0;
    }
    buf = (char *)malloc(len + 1);
    if (!buf) {
        snprintf(err, err_cap, "out of memory");
        return 0;
    }
    if (napi_get_value_string_utf8(env, value, buf, len + 1, &len) != napi_ok) {
        free(buf);
        snprintf(err, err_cap, "failed to read string");
        return 0;
    }
    *out = buf;
    return 1;
}

static char *urbnapi_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = (char *)malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n + 1);
    return r;
}

static int urbnapi_get_bool(napi_env env, napi_value value, int *out, char *err, size_t err_cap)
{
    napi_valuetype type;
    bool b;
    double num;
    int64_t i64;
    bool lossless;

    if (!out) return 0;
    if (napi_typeof(env, value, &type) != napi_ok) {
        snprintf(err, err_cap, "invalid value");
        return 0;
    }
    switch (type) {
    case napi_boolean:
        if (napi_get_value_bool(env, value, &b) != napi_ok) {
            snprintf(err, err_cap, "expected boolean");
            return 0;
        }
        *out = b ? 1 : 0;
        return 1;
    case napi_number:
        if (napi_get_value_double(env, value, &num) != napi_ok) {
            snprintf(err, err_cap, "expected number");
            return 0;
        }
        *out = (num != 0.0) ? 1 : 0;
        return 1;
    case napi_bigint:
        if (napi_get_value_bigint_int64(env, value, &i64, &lossless) != napi_ok) {
            snprintf(err, err_cap, "expected bigint");
            return 0;
        }
        (void)lossless;
        *out = (i64 != 0) ? 1 : 0;
        return 1;
    default:
        snprintf(err, err_cap, "expected boolean-like value");
        return 0;
    }
}

static int urbnapi_get_int64(napi_env env, napi_value value, int64_t *out, char *err, size_t err_cap)
{
    napi_valuetype type;
    bool b;
    double num;
    int64_t i64;
    bool lossless;

    if (!out) return 0;
    if (napi_typeof(env, value, &type) != napi_ok) {
        snprintf(err, err_cap, "invalid value");
        return 0;
    }
    switch (type) {
    case napi_bigint:
        if (napi_get_value_bigint_int64(env, value, &i64, &lossless) != napi_ok) {
            snprintf(err, err_cap, "expected bigint");
            return 0;
        }
        (void)lossless;
        *out = i64;
        return 1;
    case napi_number:
        if (napi_get_value_double(env, value, &num) != napi_ok || !isfinite(num)) {
            snprintf(err, err_cap, "expected finite number");
            return 0;
        }
        *out = (int64_t)num;
        return 1;
    case napi_boolean:
        if (napi_get_value_bool(env, value, &b) != napi_ok) {
            snprintf(err, err_cap, "expected boolean");
            return 0;
        }
        *out = b ? 1 : 0;
        return 1;
    default:
        snprintf(err, err_cap, "expected integer-like value");
        return 0;
    }
}

static int urbnapi_get_uint64(napi_env env, napi_value value, uint64_t *out, char *err, size_t err_cap)
{
    napi_valuetype type;
    bool b;
    double num;
    uint64_t u64;
    int64_t i64;
    bool lossless;

    if (!out) return 0;
    if (napi_typeof(env, value, &type) != napi_ok) {
        snprintf(err, err_cap, "invalid value");
        return 0;
    }
    switch (type) {
    case napi_bigint:
        if (napi_get_value_bigint_uint64(env, value, &u64, &lossless) == napi_ok) {
            (void)lossless;
            *out = u64;
            return 1;
        }
        if (napi_get_value_bigint_int64(env, value, &i64, &lossless) == napi_ok && i64 >= 0) {
            (void)lossless;
            *out = (uint64_t)i64;
            return 1;
        }
        snprintf(err, err_cap, "expected unsigned bigint");
        return 0;
    case napi_number:
        if (napi_get_value_double(env, value, &num) != napi_ok || !isfinite(num) || num < 0.0) {
            snprintf(err, err_cap, "expected non-negative number");
            return 0;
        }
        *out = (uint64_t)num;
        return 1;
    case napi_boolean:
        if (napi_get_value_bool(env, value, &b) != napi_ok) {
            snprintf(err, err_cap, "expected boolean");
            return 0;
        }
        *out = b ? 1u : 0u;
        return 1;
    default:
        snprintf(err, err_cap, "expected unsigned integer-like value");
        return 0;
    }
}

static int urbnapi_get_double(napi_env env, napi_value value, double *out, char *err, size_t err_cap)
{
    napi_valuetype type;
    double num;
    int64_t i64;
    uint64_t u64;
    bool lossless;

    if (!out) return 0;
    if (napi_typeof(env, value, &type) != napi_ok) {
        snprintf(err, err_cap, "invalid value");
        return 0;
    }
    switch (type) {
    case napi_number:
        if (napi_get_value_double(env, value, &num) != napi_ok || !isfinite(num)) {
            snprintf(err, err_cap, "expected finite number");
            return 0;
        }
        *out = num;
        return 1;
    case napi_bigint:
        if (napi_get_value_bigint_int64(env, value, &i64, &lossless) == napi_ok) {
            (void)lossless;
            *out = (double)i64;
            return 1;
        }
        if (napi_get_value_bigint_uint64(env, value, &u64, &lossless) == napi_ok) {
            (void)lossless;
            *out = (double)u64;
            return 1;
        }
        snprintf(err, err_cap, "expected numeric value");
        return 0;
    default:
        snprintf(err, err_cap, "expected numeric value");
        return 0;
    }
}

/* Map primitive/layout name strings to libffi ffi_type pointers. */
static ffi_type *urbnapi_ffi_type_for_name(const char *name)
{
    if (!name) return NULL;
    if (strcmp(name, "void") == 0) return &ffi_type_void;
    if (strcmp(name, "i8") == 0 || strcmp(name, "int8") == 0) return &ffi_type_sint8;
    if (strcmp(name, "u8") == 0 || strcmp(name, "uint8") == 0 || strcmp(name, "byte") == 0) return &ffi_type_uint8;
    if (strcmp(name, "i16") == 0 || strcmp(name, "int16") == 0) return &ffi_type_sint16;
    if (strcmp(name, "u16") == 0 || strcmp(name, "uint16") == 0) return &ffi_type_uint16;
    if (strcmp(name, "i32") == 0 || strcmp(name, "int32") == 0 || strcmp(name, "int") == 0) return &ffi_type_sint32;
    if (strcmp(name, "u32") == 0 || strcmp(name, "uint32") == 0 || strcmp(name, "uint") == 0) return &ffi_type_uint32;
    if (strcmp(name, "i64") == 0 || strcmp(name, "int64") == 0 || strcmp(name, "long") == 0) return &ffi_type_sint64;
    if (strcmp(name, "u64") == 0 || strcmp(name, "uint64") == 0 || strcmp(name, "ulong") == 0) return &ffi_type_uint64;
    if (strcmp(name, "f32") == 0 || strcmp(name, "float32") == 0 || strcmp(name, "float") == 0) return &ffi_type_float;
    if (strcmp(name, "f64") == 0 || strcmp(name, "float64") == 0 || strcmp(name, "double") == 0) return &ffi_type_double;
    if (strcmp(name, "bool") == 0) return &ffi_type_uint8;
    if (strcmp(name, "pointer") == 0 || strcmp(name, "ptr") == 0) return &ffi_type_pointer;
    if (strcmp(name, "cstring") == 0 || strcmp(name, "string") == 0) return &ffi_type_pointer;
    return NULL;
}

/* Count dynamic nodes and element pointer slots required for a layout that
 * requires building libffi dynamic types (struct/array). */
static int urbnapi_count_layout_nodes(napi_env env, napi_value layout, size_t *out_nodes, size_t *out_ptrs, char *err, size_t err_cap)
{
    napi_valuetype t;
    if (!out_nodes || !out_ptrs) return 0;
    *out_nodes = 0; *out_ptrs = 0;
    if (urbnapi_is_nullish(env, layout)) return 1;
    if (napi_typeof(env, layout, &t) != napi_ok) {
        snprintf(err, err_cap, "invalid layout"); return 0;
    }
    if (t == napi_string) return 1; /* primitive */
    /* arrays */
    bool is_array = false;
    if (napi_is_array(env, layout, &is_array) == napi_ok && is_array) {
        uint32_t len = 0;
        if (napi_get_array_length(env, layout, &len) != napi_ok || len < 2) {
            snprintf(err, err_cap, "invalid array layout"); return 0;
        }
        napi_value elemv; napi_get_element(env, layout, 0, &elemv);
        napi_value countv; napi_get_element(env, layout, 1, &countv);
        uint64_t count = 0;
        if (!urbnapi_get_uint64(env, countv, &count, err, err_cap)) return 0;
        size_t child_nodes = 0, child_ptrs = 0;
        if (!urbnapi_count_layout_nodes(env, elemv, &child_nodes, &child_ptrs, err, err_cap)) return 0;
        *out_nodes = 1 + child_nodes;
        *out_ptrs = (size_t)(count + 1) + child_ptrs;
        return 1;
    }

    /* objects: pointer marker or struct/union */
    if (t == napi_object) {
        napi_value ptrv;
        if (napi_get_named_property(env, layout, "__pointer", &ptrv) == napi_ok && !urbnapi_is_nullish(env, ptrv)) {
            return 1; /* pointer, no dynamic nodes */
        }
        napi_value keys; if (napi_get_property_names(env, layout, &keys) != napi_ok) { snprintf(err, err_cap, "invalid layout object"); return 0; }
        uint32_t klen = 0; napi_get_array_length(env, keys, &klen);
        size_t field_count = 0;
        size_t total_nodes = 0, total_ptrs = 0;
        for (uint32_t i = 0; i < klen; i++) {
            napi_value keyv; napi_get_element(env, keys, i, &keyv);
            char *k = NULL; if (!urbnapi_dup_string(env, keyv, &k, err, err_cap)) return 0;
            if (k[0] == '_' && k[1] == '_') { free(k); continue; }
            napi_value fieldv; if (napi_get_named_property(env, layout, k, &fieldv) != napi_ok) { free(k); snprintf(err, err_cap, "failed to read field"); return 0; }
            free(k);
            size_t cn = 0, cp = 0;
            if (!urbnapi_count_layout_nodes(env, fieldv, &cn, &cp, err, err_cap)) return 0;
            field_count++;
            total_nodes += cn;
            total_ptrs += cp;
        }
        *out_nodes = 1 + total_nodes;
        *out_ptrs = (size_t)(field_count + 1) + total_ptrs;
        return 1;
    }
    snprintf(err, err_cap, "unsupported layout type"); return 0;
}

/* Build a dynamic ffi_type graph for a layout. Returns ffi_type* and sets
 * *owner_out to a single malloc()'d block that must be free()'d by caller. */
/* context used when recursively constructing a dynamic ffi_type graph */
typedef struct {
    napi_env env;
    ffi_type *nodes_base;
    ffi_type **ptr_pool;
    size_t nodes_cap;
    size_t ptrs_cap;
    size_t next_node;
    size_t next_ptr;
    char *err;
    size_t err_cap;
} UrbnapiBuildCtx;

/* recursive helper for layout builder */
static ffi_type *urbnapi_build_node(UrbnapiBuildCtx *ctx, napi_value lay)
{
    napi_env e = ctx->env;
    napi_valuetype tv;
    if (napi_typeof(e, lay, &tv) != napi_ok) return NULL;
    if (tv == napi_string) {
        char *s = NULL;
        if (!urbnapi_dup_string(e, lay, &s, ctx->err, ctx->err_cap)) return NULL;
        ffi_type *ft = urbnapi_ffi_type_for_name(s);
        free(s);
        return ft;
    }
    bool is_array = false;
    if (napi_is_array(e, lay, &is_array) == napi_ok && is_array) {
        uint32_t alen = 0;
        napi_get_array_length(e, lay, &alen);
        napi_value elemv; napi_get_element(e, lay, 0, &elemv);
        napi_value countv; napi_get_element(e, lay, 1, &countv);
        uint64_t count = 0;
        if (!urbnapi_get_uint64(e, countv, &count, ctx->err, ctx->err_cap)) return NULL;
        if (ctx->next_node >= ctx->nodes_cap) { snprintf(ctx->err, ctx->err_cap, "internal builder overflow"); return NULL; }
        ffi_type *node = &ctx->nodes_base[ctx->next_node++];
        size_t slot = ctx->next_ptr; ctx->next_ptr += (size_t)count + 1;
        for (size_t i = 0; i < (size_t)count; i++) {
            ffi_type *child = urbnapi_build_node(ctx, elemv);
            if (!child) return NULL;
            ctx->ptr_pool[slot + i] = child;
        }
        ctx->ptr_pool[slot + (size_t)count] = NULL;
        node->type = FFI_TYPE_STRUCT;
        node->elements = &ctx->ptr_pool[slot];
        return node;
    }
    napi_value ptrv;
    if (napi_get_named_property(e, lay, "__pointer", &ptrv) == napi_ok && !urbnapi_is_nullish(e, ptrv)) {
        return &ffi_type_pointer;
    }
    napi_value keys;
    if (napi_get_property_names(e, lay, &keys) != napi_ok) { snprintf(ctx->err, ctx->err_cap, "invalid layout object"); return NULL; }
    uint32_t klen = 0; napi_get_array_length(e, keys, &klen);
    size_t field_count = 0;
    for (uint32_t i = 0; i < klen; i++) {
        napi_value keyv; napi_get_element(e, keys, i, &keyv);
        char *k = NULL;
        if (!urbnapi_dup_string(e, keyv, &k, ctx->err, ctx->err_cap)) return NULL;
        if (k[0] == '_' && k[1] == '_') { free(k); continue; }
        free(k);
        field_count++;
    }
    if (ctx->next_node >= ctx->nodes_cap) { snprintf(ctx->err, ctx->err_cap, "internal builder overflow"); return NULL; }
    ffi_type *node = &ctx->nodes_base[ctx->next_node++];
    size_t slot = ctx->next_ptr; ctx->next_ptr += field_count + 1;
    size_t idx = 0;
    for (uint32_t i = 0; i < klen; i++) {
        napi_value keyv; napi_get_element(e, keys, i, &keyv);
        char *k = NULL;
        if (!urbnapi_dup_string(e, keyv, &k, ctx->err, ctx->err_cap)) return NULL;
        if (k[0] == '_' && k[1] == '_') { free(k); continue; }
        napi_value fieldv;
        if (napi_get_named_property(e, lay, k, &fieldv) != napi_ok) { free(k); snprintf(ctx->err, ctx->err_cap, "failed to read field"); return NULL; }
        free(k);
        ffi_type *child = urbnapi_build_node(ctx, fieldv);
        if (!child) return NULL;
        ctx->ptr_pool[slot + idx] = child;
        idx++;
    }
    ctx->ptr_pool[slot + idx] = NULL;
    node->type = FFI_TYPE_STRUCT;
    node->elements = &ctx->ptr_pool[slot];
    return node;
}

static ffi_type *urbnapi_build_layout_to_ffi(napi_env env, napi_value layout, void **owner_out, char *err, size_t err_cap)
{
    size_t nodes = 0, ptrs = 0;
    *owner_out = NULL;
    if (!urbnapi_count_layout_nodes(env, layout, &nodes, &ptrs, err, err_cap)) return NULL;
    if (nodes == 0 && ptrs == 0) {
        /* primitive or pointer-only */
        napi_valuetype t; if (napi_typeof(env, layout, &t) != napi_ok) { snprintf(err, err_cap, "invalid layout"); return NULL; }
        if (t == napi_string) {
            char *s = NULL; if (!urbnapi_dup_string(env, layout, &s, err, err_cap)) return NULL;
            ffi_type *ft = urbnapi_ffi_type_for_name(s);
            free(s);
            if (!ft) { snprintf(err, err_cap, "unknown primitive layout"); return NULL; }
            return ft;
        }
        /* object pointer marker */
        napi_value ptrv; if (napi_get_named_property(env, layout, "__pointer", &ptrv) == napi_ok && !urbnapi_is_nullish(env, ptrv)) {
            return &ffi_type_pointer;
        }
        snprintf(err, err_cap, "unsupported simple layout"); return NULL;
    }

    size_t align = sizeof(void*);
    size_t nodes_sz = nodes * sizeof(ffi_type);
    size_t pad = (align - (nodes_sz % align)) % align;
    size_t ptrs_sz = ptrs * sizeof(ffi_type*);
    size_t total = nodes_sz + pad + ptrs_sz;
    void *mem = calloc(1, total);
    if (!mem) { snprintf(err, err_cap, "out of memory"); return NULL; }
    ffi_type *nodes_base = (ffi_type *)mem;
    ffi_type **ptr_pool = (ffi_type **)((char*)mem + nodes_sz + pad);


    UrbnapiBuildCtx ctx;
    ctx.env = env;
    ctx.nodes_base = nodes_base;
    ctx.ptr_pool = ptr_pool;
    ctx.nodes_cap = nodes;
    ctx.ptrs_cap = ptrs;
    ctx.next_node = 0;
    ctx.next_ptr = 0;
    ctx.err = err;
    ctx.err_cap = err_cap;

    ffi_type *root = urbnapi_build_node(&ctx, layout);
    if (!root) { free(mem); return NULL; }
    *owner_out = mem;
    return root;
}

static int urbnapi_get_pointer_depth(napi_env env, napi_value value, uintptr_t *out,
                                     int depth, char *err, size_t err_cap)
{
    napi_valuetype type;
    bool is_buffer = false;
    void *buf_data = NULL;
    size_t buf_len = 0;
    napi_value ptr_prop;
    bool has_ptr = false;
    uint64_t u64 = 0;
    double num = 0.0;

    if (!out) return 0;
    if (depth > 4) {
        snprintf(err, err_cap, "pointer indirection too deep");
        return 0;
    }
    if (urbnapi_is_nullish(env, value)) {
        *out = (uintptr_t)0;
        return 1;
    }
    if (napi_typeof(env, value, &type) != napi_ok) {
        snprintf(err, err_cap, "invalid value");
        return 0;
    }
    switch (type) {
    case napi_bigint:
        if (!urbnapi_get_uint64(env, value, &u64, err, err_cap)) return 0;
        *out = (uintptr_t)u64;
        return 1;
    case napi_number:
        if (napi_get_value_double(env, value, &num) != napi_ok || !isfinite(num) || num < 0.0) {
            snprintf(err, err_cap, "expected non-negative pointer value");
            return 0;
        }
        *out = (uintptr_t)num;
        return 1;
    case napi_object:
        if (napi_is_buffer(env, value, &is_buffer) != napi_ok) {
            snprintf(err, err_cap, "invalid object value");
            return 0;
        }
        if (is_buffer) {
            if (napi_get_buffer_info(env, value, &buf_data, &buf_len) != napi_ok) {
                snprintf(err, err_cap, "failed to read buffer");
                return 0;
            }
            (void)buf_len;
            *out = (uintptr_t)buf_data;
            return 1;
        }
        if (napi_has_named_property(env, value, "ptr", &has_ptr) != napi_ok) {
            snprintf(err, err_cap, "failed to inspect pointer-like object");
            return 0;
        }
        if (!has_ptr) {
            snprintf(err, err_cap, "expected pointer-like value");
            return 0;
        }
        if (napi_get_named_property(env, value, "ptr", &ptr_prop) != napi_ok) {
            snprintf(err, err_cap, "failed to read ptr property");
            return 0;
        }
        return urbnapi_get_pointer_depth(env, ptr_prop, out, depth + 1, err, err_cap);
    default:
        snprintf(err, err_cap, "expected pointer-like value");
        return 0;
    }
}

static int urbnapi_get_pointer(napi_env env, napi_value value, uintptr_t *out, char *err, size_t err_cap)
{
    return urbnapi_get_pointer_depth(env, value, out, 0, err, err_cap);
}

static napi_value urbnapi_make_uintptr(napi_env env, uintptr_t value)
{
    napi_value out;
    if (napi_create_bigint_uint64(env, (uint64_t)value, &out) != napi_ok) return NULL;
    return out;
}

static napi_value urbnapi_make_int64(napi_env env, int64_t value)
{
    napi_value out;
    if (napi_create_bigint_int64(env, value, &out) != napi_ok) return NULL;
    return out;
}

static napi_value urbnapi_make_uint64(napi_env env, uint64_t value)
{
    napi_value out;
    if (napi_create_bigint_uint64(env, value, &out) != napi_ok) return NULL;
    return out;
}

static napi_value urbnapi_value_from_fsig(napi_env env, Value value, FsigBase base)
{
    napi_value out = NULL;
    switch (base) {
    case FSIG_VOID:
        return urbnapi_undefined(env);
    case FSIG_BOOL:
        if (napi_get_boolean(env, value.i != 0, &out) != napi_ok) return NULL;
        return out;
    case FSIG_I8:
    case FSIG_I16:
    case FSIG_I32:
        if (napi_create_int32(env, (int32_t)value.i, &out) != napi_ok) return NULL;
        return out;
    case FSIG_U8:
    case FSIG_U16:
    case FSIG_U32:
        if (napi_create_uint32(env, (uint32_t)value.u, &out) != napi_ok) return NULL;
        return out;
    case FSIG_I64:
        return urbnapi_make_int64(env, (int64_t)value.i);
    case FSIG_U64:
        return urbnapi_make_uint64(env, (uint64_t)value.u);
    case FSIG_F32:
    case FSIG_F64:
        if (napi_create_double(env, (double)value.f, &out) != napi_ok) return NULL;
        return out;
    case FSIG_CSTRING:
        if (!value.p) {
            if (napi_get_null(env, &out) != napi_ok) return NULL;
            return out;
        }
        if (napi_create_string_utf8(env, (const char *)value.p, NAPI_AUTO_LENGTH, &out) != napi_ok) return NULL;
        return out;
    case FSIG_POINTER:
        return urbnapi_make_uintptr(env, (uintptr_t)value.p);
    default:
        (void)napi_throw_error(env, NULL, "unsupported FFI return type");
        return NULL;
    }
}

static int urbnapi_value_to_fsig(napi_env env, napi_value js_value, FsigBase base,
                                 Value *out, char **tmp_string,
                                 UrbcRuntime *rt, char *err, size_t err_cap)
{
    napi_valuetype type;
    int b = 0;
    int64_t i64 = 0;
    uint64_t u64 = 0;
    double num = 0.0;
    uintptr_t ptr = 0;
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
        if (!urbnapi_get_bool(env, js_value, &b, err, err_cap)) return 0;
        out->i = b ? 1 : 0;
        return 1;
    case FSIG_I8:
    case FSIG_I16:
    case FSIG_I32:
    case FSIG_I64:
        if (!urbnapi_get_int64(env, js_value, &i64, err, err_cap)) return 0;
        out->i = (Int)i64;
        return 1;
    case FSIG_U8:
    case FSIG_U16:
    case FSIG_U32:
    case FSIG_U64:
        if (!urbnapi_get_uint64(env, js_value, &u64, err, err_cap)) return 0;
        out->u = (UInt)u64;
        return 1;
    case FSIG_F32:
    case FSIG_F64:
        if (!urbnapi_get_double(env, js_value, &num, err, err_cap)) return 0;
        out->f = (Float)num;
        return 1;
    case FSIG_POINTER:
        if (!urbnapi_get_pointer(env, js_value, &ptr, err, err_cap)) return 0;
        out->p = (void *)ptr;
        return 1;
    case FSIG_CSTRING:
        if (urbnapi_is_nullish(env, js_value)) {
            out->p = NULL;
            return 1;
        }
        if (napi_typeof(env, js_value, &type) == napi_ok && type == napi_string) {
            if (!urbnapi_dup_string(env, js_value, &dup, err, err_cap)) return 0;
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
        if (!urbnapi_get_pointer(env, js_value, &ptr, err, err_cap)) return 0;
        out->p = (void *)ptr;
        return 1;
    default:
        snprintf(err, err_cap, "unsupported FFI value type");
        return 0;
    }
}

static int urbnapi_guess_vararg_type(napi_env env, napi_value value, uint8_t *out_prim,
                                     char *err, size_t err_cap)
{
    napi_valuetype type;
    double num;
    int64_t i64;
    uint64_t u64;
    bool lossless;
    uintptr_t ptr;

    if (!out_prim) return 0;
    if (urbnapi_is_nullish(env, value)) {
        *out_prim = URBC_PRIM_POINTER;
        return 1;
    }
    if (napi_typeof(env, value, &type) != napi_ok) {
        snprintf(err, err_cap, "invalid variadic value");
        return 0;
    }
    switch (type) {
    case napi_boolean:
        *out_prim = URBC_PRIM_BOOL;
        return 1;
    case napi_string:
        *out_prim = URBC_PRIM_CSTRING;
        return 1;
    case napi_bigint:
        if (napi_get_value_bigint_int64(env, value, &i64, &lossless) == napi_ok && i64 < 0) {
            (void)lossless;
            *out_prim = URBC_PRIM_I64;
            return 1;
        }
        if (napi_get_value_bigint_uint64(env, value, &u64, &lossless) == napi_ok) {
            (void)lossless;
            *out_prim = URBC_PRIM_U64;
            return 1;
        }
        *out_prim = URBC_PRIM_I64;
        return 1;
    case napi_number:
        if (napi_get_value_double(env, value, &num) != napi_ok || !isfinite(num)) {
            snprintf(err, err_cap, "expected finite variadic number");
            return 0;
        }
        if (floor(num) == num) {
            *out_prim = URBC_PRIM_I32;
            return 1;
        }
        *out_prim = URBC_PRIM_F64;
        return 1;
    case napi_object:
        if (!urbnapi_get_pointer(env, value, &ptr, err, err_cap)) return 0;
        (void)ptr;
        *out_prim = URBC_PRIM_POINTER;
        return 1;
    default:
        snprintf(err, err_cap, "unsupported variadic argument type");
        return 0;
    }
}

static int urbnapi_value_to_prim(napi_env env, napi_value js_value, uint8_t prim,
                                 Value *out, char **tmp_string,
                                 UrbcRuntime *rt, char *err, size_t err_cap)
{
    FsigBase base = FSIG_VOID;
    switch (prim) {
    case URBC_PRIM_BOOL: base = FSIG_BOOL; break;
    case URBC_PRIM_I8:   base = FSIG_I8; break;
    case URBC_PRIM_U8:   base = FSIG_U8; break;
    case URBC_PRIM_I16:  base = FSIG_I16; break;
    case URBC_PRIM_U16:  base = FSIG_U16; break;
    case URBC_PRIM_I32:  base = FSIG_I32; break;
    case URBC_PRIM_U32:  base = FSIG_U32; break;
    case URBC_PRIM_I64:  base = FSIG_I64; break;
    case URBC_PRIM_U64:  base = FSIG_U64; break;
    case URBC_PRIM_F32:  base = FSIG_F32; break;
    case URBC_PRIM_F64:  base = FSIG_F64; break;
    case URBC_PRIM_POINTER: base = FSIG_POINTER; break;
    case URBC_PRIM_CSTRING: base = FSIG_CSTRING; break;
    default:
        snprintf(err, err_cap, "unsupported variadic primitive type");
        return 0;
    }
    return urbnapi_value_to_fsig(env, js_value, base, out, tmp_string, rt, err, err_cap);
}

static Value urbnapi_js_callback(UrbcRuntime *rt, int argc, const Value *argv, void *opaque)
{
    CallbackBinding *binding = (CallbackBinding *)opaque;
    napi_env env;
    napi_handle_scope scope;
    napi_value recv;
    napi_value fn;
    napi_value js_argv[FSIG_MAX_ARGS];
    napi_value js_ret;
    Value out;
    char err[URBC_ERROR_CAP];
    int i;

    memset(&out, 0, sizeof(out));
    if (!binding || !(env = binding->env)) {
        urbc_runtime_fail(rt, "ffi.callback: missing callback binding");
        return out;
    }
    if (!urbnapi_callback_on_creator_thread(binding)) {
        urbc_runtime_fail(rt, "ffi.callback: Node callbacks must run on the creator thread");
        return out;
    }
    if (napi_open_handle_scope(env, &scope) != napi_ok) {
        urbc_runtime_fail(rt, "ffi.callback: failed to open handle scope");
        return out;
    }
    if (napi_get_undefined(env, &recv) != napi_ok ||
        napi_get_reference_value(env, binding->fn_ref, &fn) != napi_ok) {
        napi_close_handle_scope(env, scope);
        urbc_runtime_fail(rt, "ffi.callback: failed to resolve JS function");
        return out;
    }
    for (i = 0; i < argc; i++) {
        js_argv[i] = urbnapi_value_from_fsig(env, argv[i], binding->sig.args[i].base);
        if (!js_argv[i]) {
            napi_close_handle_scope(env, scope);
            urbc_runtime_fail(rt, "ffi.callback: failed to marshal argument %d", i);
            return out;
        }
    }
    if (napi_call_function(env, recv, fn, (size_t)argc, js_argv, &js_ret) != napi_ok) {
        urbnapi_exception_string(env, err, sizeof(err));
        napi_close_handle_scope(env, scope);
        urbc_runtime_fail(rt, "ffi.callback: %s", err[0] ? err : "JavaScript callback failed");
        return out;
    }
    if (!urbnapi_value_to_fsig(env, js_ret, binding->sig.ret.base, &out, NULL, rt, err, sizeof(err))) {
        napi_close_handle_scope(env, scope);
        urbc_runtime_fail(rt, "ffi.callback: %s", err);
        memset(&out, 0, sizeof(out));
        return out;
    }
    (void)napi_close_handle_scope(env, scope);
    return out;
}

static void urbnapi_lib_finalize(napi_env env, void *data, void *hint)
{
    LibHandle *wrap = (LibHandle *)data;
    (void)env;
    (void)hint;
    if (!wrap) return;
    if (!wrap->closed && wrap->handle) {
        (void)urbc_dyn_close(wrap->handle);
        wrap->handle = NULL;
        wrap->closed = 1;
    }
    free(wrap);
}

static void urbnapi_bound_finalize(napi_env env, void *data, void *hint)
{
    (void)env;
    (void)hint;
    free(data);
}

static void urbnapi_descriptor_finalize(napi_env env, void *data, void *hint)
{
    DescriptorHandle *wrap = (DescriptorHandle *)data;
    (void)env;
    (void)hint;
    if (!wrap) return;
    if (wrap->descriptor) {
        urbc_ffi_descriptor_release(wrap->descriptor);
        wrap->descriptor = NULL;
    }
    free(wrap);
}

static void urbnapi_callback_finalize(napi_env env, void *data, void *hint)
{
    (void)env;
    (void)hint;
    free(data);
}

static void urbnapi_addon_finalize(napi_env env, void *data, void *hint)
{
    AddonState *state = (AddonState *)data;
    CallbackBinding *cb;
    (void)hint;
    if (!state) return;
    for (cb = state->callbacks; cb; ) {
        CallbackBinding *next = cb->next;
        if (cb->fn_ref) (void)napi_delete_reference(env, cb->fn_ref);
        free(cb);
        cb = next;
    }
    urbc_runtime_destroy(&state->rt);
    urbc_image_destroy(&state->image);
    free(state);
}

static LibHandle *urbnapi_unwrap_lib(napi_env env, napi_value value)
{
    LibHandle *wrap = NULL;
    if (urbnapi_is_nullish(env, value)) return NULL;
    if (napi_get_value_external(env, value, (void **)&wrap) != napi_ok) {
        (void)napi_throw_type_error(env, NULL, "expected library handle");
        return NULL;
    }
    return wrap;
}

static BoundHandle *urbnapi_unwrap_bound(napi_env env, napi_value value)
{
    BoundHandle *wrap = NULL;
    if (napi_get_value_external(env, value, (void **)&wrap) != napi_ok) {
        (void)napi_throw_type_error(env, NULL, "expected bound function handle");
        return NULL;
    }
    return wrap;
}

static DescriptorHandle *urbnapi_unwrap_descriptor(napi_env env, napi_value value)
{
    DescriptorHandle *wrap = NULL;
    if (napi_get_value_external(env, value, (void **)&wrap) != napi_ok ||
        !wrap || !wrap->descriptor) {
        (void)napi_throw_type_error(env, NULL, "expected signature descriptor handle");
        return NULL;
    }
    return wrap;
}

static napi_value urbnapi_open(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    char *path = NULL;
    char err[URBC_ERROR_CAP];
    int64_t flags64 = 0;
    void *handle = NULL;
    LibHandle *wrap;
    napi_value out;

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 1) {
        napi_throw_type_error(env, NULL, "ffi.open expects a path");
        return NULL;
    }
    if (!urbnapi_dup_string(env, argv[0], &path, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, err);
        return NULL;
    }
    if (argc >= 2 && !urbnapi_get_int64(env, argv[1], &flags64, err, sizeof(err))) {
        free(path);
        napi_throw_type_error(env, NULL, err);
        return NULL;
    }
    if (urbc_ffi_open(path, (int)flags64, &handle, err, sizeof(err)) != URBC_OK) {
        free(path);
        NAPI_CALL(env, napi_get_null(env, &out));
        return out;
    }
    free(path);
    wrap = (LibHandle *)calloc(1, sizeof(*wrap));
    if (!wrap) {
        urbc_ffi_close(handle);
        napi_throw_error(env, NULL, "out of memory");
        return NULL;
    }
    wrap->handle = handle;
    if (napi_create_external(env, wrap, urbnapi_lib_finalize, NULL, &out) != napi_ok) {
        urbnapi_lib_finalize(env, wrap, NULL);
        urbnapi_throw_status(env, napi_generic_failure, "napi_create_external");
        return NULL;
    }
    return out;
}

static napi_value urbnapi_close(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    LibHandle *wrap;

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 1 || urbnapi_is_nullish(env, argv[0]))
        return urbnapi_undefined(env);
    wrap = urbnapi_unwrap_lib(env, argv[0]);
    if (!wrap) return NULL;
    if (!wrap->closed && wrap->handle) {
        urbc_ffi_close(wrap->handle);
        wrap->handle = NULL;
        wrap->closed = 1;
    }
    return urbnapi_undefined(env);
}

static napi_value urbnapi_sym(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    LibHandle *wrap;
    char *name = NULL;
    char err[URBC_ERROR_CAP];
    void *sym = NULL;

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 2) {
        napi_throw_type_error(env, NULL, "ffi.sym expects a library handle and symbol name");
        return NULL;
    }
    wrap = urbnapi_unwrap_lib(env, argv[0]);
    if (!wrap) return NULL;
    if (wrap->closed || !wrap->handle) return urbnapi_make_uintptr(env, (uintptr_t)0);
    if (!urbnapi_dup_string(env, argv[1], &name, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, err);
        return NULL;
    }
    if (urbc_ffi_sym(wrap->handle, name, &sym, err, sizeof(err)) != URBC_OK) {
        free(name);
        return urbnapi_make_uintptr(env, (uintptr_t)0);
    }
    free(name);
    return urbnapi_make_uintptr(env, (uintptr_t)sym);
}

static napi_value urbnapi_sym_self(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    char *name = NULL;
    char err[URBC_ERROR_CAP];
    void *sym = NULL;

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 1) {
        napi_throw_type_error(env, NULL, "ffi.symSelf expects a symbol name");
        return NULL;
    }
    if (!urbnapi_dup_string(env, argv[0], &name, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, err);
        return NULL;
    }
    if (urbc_ffi_sym_self(name, &sym, err, sizeof(err)) != URBC_OK) {
        free(name);
        return urbnapi_make_uintptr(env, (uintptr_t)0);
    }
    free(name);
    return urbnapi_make_uintptr(env, (uintptr_t)sym);
}

static napi_value urbnapi_describe(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    char *sig = NULL;
    char err[URBC_ERROR_CAP];
    DescriptorHandle *wrap;
    napi_value out;

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 1) {
        napi_throw_type_error(env, NULL, "ffi.describe expects a signature string");
        return NULL;
    }
    if (!urbnapi_dup_string(env, argv[0], &sig, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, err);
        return NULL;
    }
    wrap = (DescriptorHandle *)calloc(1, sizeof(*wrap));
    if (!wrap) {
        free(sig);
        napi_throw_error(env, NULL, "out of memory");
        return NULL;
    }
    if (urbc_ffi_describe(sig, &wrap->descriptor, err, sizeof(err)) != URBC_OK) {
        free(sig);
        free(wrap);
        napi_throw_error(env, NULL, err);
        return NULL;
    }
    free(sig);
    if (napi_create_external(env, wrap, urbnapi_descriptor_finalize, NULL, &out) != napi_ok) {
        urbnapi_descriptor_finalize(env, wrap, NULL);
        urbnapi_throw_status(env, napi_generic_failure, "napi_create_external");
        return NULL;
    }
    return out;
}

/*
 * Describe a descriptor JS object (from the high-level wrapper) by
 * synthesizing a native-callable signature string. Complex layout
 * types (struct/union/array/function) are currently represented as
 * `pointer` in the synthesized signature (pointer-fallback).
 */
static char *urbnapi_leaf_from_abi_type(napi_env env, napi_value abi, char *err, size_t err_cap)
{
    napi_valuetype t;
    if (napi_typeof(env, abi, &t) != napi_ok) {
        snprintf(err, err_cap, "invalid ABI type");
        return NULL;
    }
    if (t == napi_string) {
        char *s = NULL;
        if (!urbnapi_dup_string(env, abi, &s, err, err_cap)) return NULL;
        return s; /* caller frees */
    }
    if (t != napi_object) {
        snprintf(err, err_cap, "ABI type must be object or string");
        return NULL;
    }

    napi_value kindv;
    if (napi_get_named_property(env, abi, "kind", &kindv) != napi_ok) {
        snprintf(err, err_cap, "ABI type missing kind");
        return NULL;
    }
    char *kind = NULL;
    if (!urbnapi_dup_string(env, kindv, &kind, err, err_cap)) return NULL;

    if (strcmp(kind, "void") == 0) {
        free(kind);
        return urbnapi_strdup("void");
    }
    if (strcmp(kind, "primitive") == 0) {
        napi_value namev;
        if (napi_get_named_property(env, abi, "name", &namev) != napi_ok) {
            free(kind);
            snprintf(err, err_cap, "primitive ABI missing name");
            return NULL;
        }
        char *name = NULL;
        if (!urbnapi_dup_string(env, namev, &name, err, err_cap)) { free(kind); return NULL; }
        free(kind);
        return name; /* already heap allocated */
    }
    if (strcmp(kind, "cstring") == 0) { free(kind); return urbnapi_strdup("cstring"); }
    if (strcmp(kind, "pointer") == 0) { free(kind); return urbnapi_strdup("pointer"); }
    if (strcmp(kind, "enum") == 0) {
        napi_value undv;
        if (napi_get_named_property(env, abi, "underlying", &undv) != napi_ok) {
            free(kind);
            snprintf(err, err_cap, "enum missing underlying type");
            return NULL;
        }
        free(kind);
        return urbnapi_leaf_from_abi_type(env, undv, err, err_cap);
    }

    /* array/struct/union/function/etc => pointer fallback */
    free(kind);
    return urbnapi_strdup("pointer");
}

static napi_value urbnapi_describe_descriptor(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    char err[URBC_ERROR_CAP];
    DescriptorHandle *wrap;
    char *sig = NULL;

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 1) {
        napi_throw_type_error(env, NULL, "describeDescriptor expects a descriptor object");
        return NULL;
    }

    napi_value desc_obj = argv[0];
    napi_value typev;
    if (napi_get_named_property(env, desc_obj, "type", &typev) != napi_ok) {
        napi_throw_type_error(env, NULL, "descriptor missing type");
        return NULL;
    }

    /* Expect a function-type descriptor */
    napi_value kindv; char *kind = NULL;
    if (napi_get_named_property(env, typev, "kind", &kindv) != napi_ok ||
        !urbnapi_dup_string(env, kindv, &kind, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, "invalid descriptor.type.kind");
        return NULL;
    }
    if (strcmp(kind, "function") != 0) {
        free(kind);
        napi_throw_type_error(env, NULL, "describeDescriptor currently only supports function types");
        return NULL;
    }
    free(kind);

    /* Build native-friendly signature string: <ret> <name>(<args...>) with pointer-fallback */
    /* debug: if we generated layout tags, show them */
    char *name = NULL;
    napi_value namev;
    if (napi_get_named_property(env, typev, "name", &namev) == napi_ok) {
        (void)urbnapi_dup_string(env, namev, &name, err, sizeof(err));
    }
    if (!name) name = urbnapi_strdup("fn");

    /* optional host-provided layouts: { ret, args } */
    napi_value layouts_obj = NULL;
    napi_value layouts_ret = NULL;
    napi_value layouts_args = NULL;
    bool have_layouts = false;
    if (napi_get_named_property(env, desc_obj, "layouts", &layouts_obj) == napi_ok && !urbnapi_is_nullish(env, layouts_obj)) {
        have_layouts = true;
        (void)napi_get_named_property(env, layouts_obj, "ret", &layouts_ret);
        (void)napi_get_named_property(env, layouts_obj, "args", &layouts_args);
    }

    napi_value retv;
    if (napi_get_named_property(env, typev, "ret", &retv) != napi_ok) {
        free(name); napi_throw_type_error(env, NULL, "function type missing ret"); return NULL;
    }
    char *ret_leaf = urbnapi_leaf_from_abi_type(env, retv, err, sizeof(err));
    if (!ret_leaf) { free(name); napi_throw_type_error(env, NULL, err); return NULL; }
    char *ret_tag = NULL;
    static unsigned int urbnapi_layout_seq = 1;
    if (have_layouts && strcmp(ret_leaf, "pointer") == 0 && !urbnapi_is_nullish(env, layouts_ret)) {
        /* if layout is not a pointer marker, attach by-value/array struct types */
        napi_valuetype ltype; napi_typeof(env, layouts_ret, &ltype);
        bool is_arr = false; napi_is_array(env, layouts_ret, &is_arr);
        if (is_arr || ltype == napi_object) {
            napi_value ptrv; if (!(napi_get_named_property(env, layouts_ret, "__pointer", &ptrv) == napi_ok && !urbnapi_is_nullish(env, ptrv))) {
                char tbuf[64]; snprintf(tbuf, sizeof(tbuf), "urb_layout_%u", urbnapi_layout_seq++);
                ret_tag = urbnapi_strdup(tbuf);
                size_t newlen = strlen("pointer()") + strlen(ret_tag) + 8;
                char *rl = (char *)malloc(newlen);
                if (!rl) { free(ret_leaf); free(name); napi_throw_error(env, NULL, "out of memory"); return NULL; }
                snprintf(rl, newlen, "pointer(%s)", ret_tag);
                free(ret_leaf); ret_leaf = rl;
            }
        }
    }

    napi_value argv_arr;
    if (napi_get_named_property(env, typev, "args", &argv_arr) != napi_ok) {
        free(name); free(ret_leaf); napi_throw_type_error(env, NULL, "function type missing args"); return NULL;
    }
    uint32_t argc_len = 0;
    if (napi_get_array_length(env, argv_arr, &argc_len) != napi_ok) argc_len = 0;

    char **arg_leaves = NULL;
    if (argc_len > 0) {
        arg_leaves = (char **)calloc((size_t)argc_len, sizeof(char*));
        if (!arg_leaves) { free(name); free(ret_leaf); napi_throw_error(env, NULL, "out of memory"); return NULL; }
    }

    /* optional per-arg layout values and tags */
    napi_value *arg_layout_vals = NULL;
    char **arg_tags = NULL;
    if (have_layouts && argc_len > 0) {
        arg_layout_vals = (napi_value *)calloc((size_t)argc_len, sizeof(napi_value));
        arg_tags = (char **)calloc((size_t)argc_len, sizeof(char*));
        if (!arg_layout_vals || !arg_tags) {
            free(name); free(ret_leaf); free(arg_leaves);
            free(arg_layout_vals); free(arg_tags);
            napi_throw_error(env, NULL, "out of memory"); return NULL;
        }
    }

    size_t total_len = strlen(ret_leaf) + 1 + strlen(name) + 2; /* ret + space + name + () */
    for (uint32_t i = 0; i < argc_len; i++) {
        napi_value aelem; napi_get_element(env, argv_arr, i, &aelem);
        arg_leaves[i] = urbnapi_leaf_from_abi_type(env, aelem, err, sizeof(err));
        if (!arg_leaves[i]) {
            for (uint32_t j = 0; j < i; j++) free(arg_leaves[j]);
            free(arg_leaves); free(name); free(ret_leaf);
            free(arg_layout_vals); free(arg_tags);
            napi_throw_type_error(env, NULL, err);
            return NULL;
        }
        /* if leaf is pointer and host supplied a layout for this arg, create a tag */
        if (have_layouts && strcmp(arg_leaves[i], "pointer") == 0 && !urbnapi_is_nullish(env, layouts_args)) {
            napi_value layout_elem; napi_get_element(env, layouts_args, i, &layout_elem);
            arg_layout_vals[i] = layout_elem;
            if (!urbnapi_is_nullish(env, layout_elem)) {
                napi_valuetype ltype; napi_typeof(env, layout_elem, &ltype);
                bool is_arr = false; napi_is_array(env, layout_elem, &is_arr);
                if (is_arr || ltype == napi_object) {
                    napi_value ptrv; if (!(napi_get_named_property(env, layout_elem, "__pointer", &ptrv) == napi_ok && !urbnapi_is_nullish(env, ptrv))) {
                        char tbuf[64]; snprintf(tbuf, sizeof(tbuf), "urb_layout_%u", urbnapi_layout_seq++);
                        arg_tags[i] = urbnapi_strdup(tbuf);
                        size_t newlen = strlen("pointer()") + strlen(arg_tags[i]) + 8;
                        char *al = (char *)malloc(newlen);
                        if (!al) {
                            for (uint32_t j = 0; j <= i; j++) free(arg_leaves[j]);
                            free(arg_leaves); free(name); free(ret_leaf);
                            for (uint32_t j = 0; j < argc_len; j++) free(arg_tags[j]);
                            free(arg_layout_vals); free(arg_tags);
                            napi_throw_error(env, NULL, "out of memory"); return NULL;
                        }
                        snprintf(al, newlen, "pointer(%s)", arg_tags[i]);
                        free(arg_leaves[i]); arg_leaves[i] = al;
                    }
                }
            }
        } else if (have_layouts && layouts_args) {
            /* record layout element even if no tag created, for later checks */
            napi_value layout_elem; napi_get_element(env, layouts_args, i, &layout_elem);
            arg_layout_vals[i] = layout_elem;
        }
        total_len += strlen(arg_leaves[i]) + 2; /* comma + space */
    }

    napi_value varv; bool varargs = false;
    if (napi_get_named_property(env, typev, "varargs", &varv) == napi_ok) {
        napi_valuetype vt; napi_typeof(env, varv, &vt);
        if (vt == napi_boolean) { napi_get_value_bool(env, varv, &varargs); }
    }
    if (varargs) total_len += 4; /* for ..., */

    sig = (char *)malloc(total_len + 16);
    if (!sig) {
        for (uint32_t i = 0; i < argc_len; i++) free(arg_leaves[i]);
        free(arg_leaves); free(name); free(ret_leaf);
        napi_throw_error(env, NULL, "out of memory"); return NULL;
    }

    /* Compose signature */
    char *p = sig;
    size_t need = total_len + 1;
    snprintf(p, need, "%s %s(", ret_leaf, name);
    for (uint32_t ii = 0; ii < argc_len; ii++) {
        if (ii > 0) strcat(p, ", ");
        strcat(p, arg_leaves[ii]);
    }
    if (varargs) {
        if (argc_len > 0) strcat(p, ", ..."); else strcat(p, "...");
    }
    strcat(p, ")");
    strcat(p, ")");

    /* Do not free leaf/tag buffers yet — we may need them while attaching types. */

    wrap = (DescriptorHandle *)calloc(1, sizeof(*wrap));
    if (!wrap) { free(sig); napi_throw_error(env, NULL, "out of memory"); return NULL; }
    if (urbc_ffi_describe(sig, &wrap->descriptor, err, sizeof(err)) != URBC_OK) {
        free(sig); free(wrap);
        napi_throw_type_error(env, NULL, err[0] ? err : "failed to describe descriptor");
        return NULL;
    }
    free(sig);
    /* Build and attach dynamic ffi_type graphs for any tagged layouts */
    if (ret_tag || (arg_tags != NULL)) {
        /* attach return layout if present */
        if (ret_tag) {
            void *owner = NULL;
            ffi_type *ft = urbnapi_build_layout_to_ffi(env, layouts_ret, &owner, err, sizeof(err));
            if (!ft) {
                if (owner) free(owner);
                urbc_ffi_descriptor_release(wrap->descriptor);
                free(wrap);
                /* clean up tags/leaves */
                if (arg_leaves) { for (uint32_t i = 0; i < argc_len; i++) free(arg_leaves[i]); free(arg_leaves); }
                if (arg_tags) { for (uint32_t i = 0; i < argc_len; i++) free(arg_tags[i]); free(arg_tags); }
                if (arg_layout_vals) free(arg_layout_vals);
                free(ret_tag);
                free(name);
                napi_throw_type_error(env, NULL, err[0] ? err : "failed to build layout type");
                return NULL;
            }
            if (urbc_ffi_descriptor_attach_ffi_type(wrap->descriptor, ret_tag, ft, 1, owner, err, sizeof(err)) != URBC_OK) {
                if (owner) free(owner);
                urbc_ffi_descriptor_release(wrap->descriptor);
                free(wrap);
                if (arg_leaves) { for (uint32_t i = 0; i < argc_len; i++) free(arg_leaves[i]); free(arg_leaves); }
                if (arg_tags) { for (uint32_t i = 0; i < argc_len; i++) free(arg_tags[i]); free(arg_tags); }
                if (arg_layout_vals) free(arg_layout_vals);
                free(ret_tag); free(name);
                napi_throw_type_error(env, NULL, err[0] ? err : "failed to attach layout type");
                return NULL;
            }
            free(ret_tag);
            ret_tag = NULL;
        }
        /* attach arg layouts */
        if (arg_tags) {
            for (uint32_t i = 0; i < argc_len; i++) {
                if (!arg_tags[i]) continue;
                void *owner = NULL;
                napi_value lay = arg_layout_vals ? arg_layout_vals[i] : NULL;
                ffi_type *ft = urbnapi_build_layout_to_ffi(env, lay, &owner, err, sizeof(err));
                if (!ft) {
                    if (owner) free(owner);
                    urbc_ffi_descriptor_release(wrap->descriptor);
                    free(wrap);
                    if (arg_leaves) { for (uint32_t j = 0; j < argc_len; j++) free(arg_leaves[j]); free(arg_leaves); }
                    if (arg_tags) { for (uint32_t j = 0; j < argc_len; j++) free(arg_tags[j]); free(arg_tags); }
                    if (arg_layout_vals) free(arg_layout_vals);
                    free(name);
                    napi_throw_type_error(env, NULL, err[0] ? err : "failed to build layout type");
                    return NULL;
                }
                if (urbc_ffi_descriptor_attach_ffi_type(wrap->descriptor, arg_tags[i], ft, 1, owner, err, sizeof(err)) != URBC_OK) {
                    if (owner) free(owner);
                    urbc_ffi_descriptor_release(wrap->descriptor);
                    free(wrap);
                    if (arg_leaves) { for (uint32_t j = 0; j < argc_len; j++) free(arg_leaves[j]); free(arg_leaves); }
                    if (arg_tags) { for (uint32_t j = 0; j < argc_len; j++) free(arg_tags[j]); free(arg_tags); }
                    if (arg_layout_vals) free(arg_layout_vals);
                    free(name);
                    napi_throw_type_error(env, NULL, err[0] ? err : "failed to attach layout type");
                    return NULL;
                }
                free(arg_tags[i]); arg_tags[i] = NULL;
            }
        }
    }

    /* cleanup local buffers (safe to free here after attachments) */
    if (arg_leaves) { for (uint32_t i = 0; i < argc_len; i++) free(arg_leaves[i]); free(arg_leaves); }
    if (arg_tags) { free(arg_tags); }
    if (arg_layout_vals) free(arg_layout_vals);
    free(name); free(ret_leaf);

    napi_value out;
    if (napi_create_external(env, wrap, urbnapi_descriptor_finalize, NULL, &out) != napi_ok) {
        urbnapi_descriptor_finalize(env, wrap, NULL);
        urbnapi_throw_status(env, napi_generic_failure, "napi_create_external");
        return NULL;
    }
    return out;
}

static napi_value urbnapi_bind(napi_env env, napi_callback_info info)
{
    AddonState *state = urbnapi_state(env);
    napi_value argv[2];
    size_t argc = 2;
    uintptr_t ptr = 0;
    char err[URBC_ERROR_CAP];
    DescriptorHandle *desc_wrap;
    void *bound = NULL;
    BoundHandle *wrap;
    napi_value out;

    if (!state) {
        napi_throw_error(env, NULL, "addon state not initialized");
        return NULL;
    }
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 2) {
        napi_throw_type_error(env, NULL, "ffi.bind expects a pointer and descriptor");
        return NULL;
    }
    if (!urbnapi_get_pointer(env, argv[0], &ptr, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, err);
        return NULL;
    }
    if (ptr == 0) {
        napi_throw_error(env, NULL, "ffi.bind: function pointer is null");
        return NULL;
    }
    desc_wrap = urbnapi_unwrap_descriptor(env, argv[1]);
    if (!desc_wrap) return NULL;
    if (urbc_ffi_bind_desc(&state->rt, (void *)ptr, desc_wrap->descriptor, &bound, err, sizeof(err)) != URBC_OK) {
        napi_throw_error(env, NULL, err);
        return NULL;
    }
    wrap = (BoundHandle *)calloc(1, sizeof(*wrap));
    if (!wrap) {
        napi_throw_error(env, NULL, "out of memory");
        return NULL;
    }
    wrap->handle = bound;
    NAPI_CALL(env, napi_create_external(env, wrap, urbnapi_bound_finalize, NULL, &out));
    return out;
}

static napi_value urbnapi_call_bound(napi_env env, napi_callback_info info)
{
    AddonState *state = urbnapi_state(env);
    napi_value argv[2];
    size_t argc = 2;
    BoundHandle *wrap;
    UrbcBoundFn *bound;
    bool is_array = false;
    uint32_t js_argc = 0;
    Value call_argv[FSIG_MAX_ARGS];
    uint8_t vararg_types[FSIG_MAX_ARGS];
    char *tmp_strings[FSIG_MAX_ARGS];
    Value out;
    char err[URBC_ERROR_CAP];
    int i;

    if (!state) {
        napi_throw_error(env, NULL, "addon state not initialized");
        return NULL;
    }
    memset(tmp_strings, 0, sizeof(tmp_strings));
    memset(&out, 0, sizeof(out));
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 2) {
        napi_throw_type_error(env, NULL, "ffi.callBound expects a bound handle and argument array");
        return NULL;
    }
    wrap = urbnapi_unwrap_bound(env, argv[0]);
    if (!wrap) return NULL;
    bound = (UrbcBoundFn *)wrap->handle;
    if (!bound || bound->magic != URBC_BOUND_MAGIC) {
        napi_throw_type_error(env, NULL, "invalid bound function handle");
        return NULL;
    }
    NAPI_CALL(env, napi_is_array(env, argv[1], &is_array));
    if (!is_array) {
        napi_throw_type_error(env, NULL, "ffi.callBound expects an array of arguments");
        return NULL;
    }
    NAPI_CALL(env, napi_get_array_length(env, argv[1], &js_argc));
    if (js_argc > FSIG_MAX_ARGS) {
        napi_throw_range_error(env, NULL, "too many FFI arguments");
        return NULL;
    }
    for (i = 0; i < (int)js_argc; i++) {
        napi_value item;
        NAPI_CALL(env, napi_get_element(env, argv[1], (uint32_t)i, &item));
        if (i < bound->sig.argc) {
            if (!urbnapi_value_to_fsig(env, item, bound->sig.args[i].base,
                                       &call_argv[i], &tmp_strings[i], NULL,
                                       err, sizeof(err))) {
                while (i-- > 0) free(tmp_strings[i]);
                napi_throw_type_error(env, NULL, err);
                return NULL;
            }
        } else {
            if (!urbnapi_guess_vararg_type(env, item, &vararg_types[i - bound->sig.argc], err, sizeof(err)) ||
                !urbnapi_value_to_prim(env, item, vararg_types[i - bound->sig.argc],
                                       &call_argv[i], &tmp_strings[i], NULL,
                                       err, sizeof(err))) {
                while (i-- > 0) free(tmp_strings[i]);
                napi_throw_type_error(env, NULL, err);
                return NULL;
            }
        }
    }
    if (urbc_ffi_call(&state->rt, wrap->handle, (int)js_argc, call_argv,
                      (js_argc > (uint32_t)bound->sig.argc) ? vararg_types : NULL,
                      &out, err, sizeof(err)) != URBC_OK) {
        for (i = 0; i < (int)js_argc; i++) free(tmp_strings[i]);
        napi_throw_error(env, NULL, err);
        return NULL;
    }
    for (i = 0; i < (int)js_argc; i++) free(tmp_strings[i]);
    return urbnapi_value_from_fsig(env, out, bound->sig.ret.base);
}

static napi_value urbnapi_callback(napi_env env, napi_callback_info info)
{
    AddonState *state = urbnapi_state(env);
    napi_value argv[2];
    size_t argc = 2;
    napi_valuetype fn_type;
    char err[URBC_ERROR_CAP];
    char name[64];
    DescriptorHandle *desc_wrap;
    CallbackBinding *binding;
    UrbcHostBinding *host;
    void *fn_ptr = NULL;
    CallbackHandle *wrap;
    napi_value handle_ext;
    napi_value ptrv;
    napi_value obj;

    if (!state) {
        napi_throw_error(env, NULL, "addon state not initialized");
        return NULL;
    }
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 2) {
        napi_throw_type_error(env, NULL, "ffi.callback expects a descriptor and function");
        return NULL;
    }
    desc_wrap = urbnapi_unwrap_descriptor(env, argv[0]);
    if (!desc_wrap) return NULL;
    NAPI_CALL(env, napi_typeof(env, argv[1], &fn_type));
    if (fn_type != napi_function) {
        napi_throw_type_error(env, NULL, "ffi.callback expects a JavaScript function");
        return NULL;
    }
    binding = (CallbackBinding *)calloc(1, sizeof(*binding));
    if (!binding) {
        napi_throw_error(env, NULL, "out of memory");
        return NULL;
    }
    binding->env = env;
#ifdef _WIN32
    binding->creator_thread = GetCurrentThreadId();
#else
    binding->creator_thread = pthread_self();
#endif
    if (urbc_ffi_descriptor_copy_parsed(desc_wrap->descriptor, &binding->sig, err, sizeof(err)) != URBC_OK) {
        free(binding);
        napi_throw_error(env, NULL, err);
        return NULL;
    }
    NAPI_CALL(env, napi_create_reference(env, argv[1], 1, &binding->fn_ref));
    binding->next = state->callbacks;
    state->callbacks = binding;

    snprintf(name, sizeof(name), "node_cb_%llu", (unsigned long long)++state->next_callback_id);
    if (urbc_runtime_register_host(&state->rt, name, urbnapi_js_callback, binding,
                                   err, sizeof(err)) != URBC_OK) {
        napi_throw_error(env, NULL, err);
        return NULL;
    }
    host = urbc_runtime_find_host(&state->rt, name);
    if (!host) {
        napi_throw_error(env, NULL, "ffi.callback: failed to resolve host binding");
        return NULL;
    }
    if (urbc_ffi_callback_desc(&state->rt, desc_wrap->descriptor,
                               (Value){ .p = host }, &fn_ptr, err, sizeof(err)) != URBC_OK) {
        napi_throw_error(env, NULL, err);
        return NULL;
    }
    wrap = (CallbackHandle *)calloc(1, sizeof(*wrap));
    if (!wrap) {
        napi_throw_error(env, NULL, "out of memory");
        return NULL;
    }
    wrap->fn_ptr = fn_ptr;
    wrap->binding = binding;
    NAPI_CALL(env, napi_create_external(env, wrap, urbnapi_callback_finalize, NULL, &handle_ext));
    ptrv = urbnapi_make_uintptr(env, (uintptr_t)fn_ptr);
    if (!ptrv) return NULL;
    NAPI_CALL(env, napi_create_object(env, &obj));
    NAPI_CALL(env, napi_set_named_property(env, obj, "ptr", ptrv));
    NAPI_CALL(env, napi_set_named_property(env, obj, "handle", handle_ext));
    return obj;
}

static napi_value urbnapi_errno_value(napi_env env, napi_callback_info info)
{
    napi_value out;
    (void)info;
    NAPI_CALL(env, napi_create_int32(env, urbc_ffi_errno_value(), &out));
    return out;
}

static napi_value urbnapi_dlerror(napi_env env, napi_callback_info info)
{
    AddonState *state = urbnapi_state(env);
    napi_value out;
    char *msg = NULL;
    char err[URBC_ERROR_CAP];
    (void)info;
    if (!state) {
        napi_throw_error(env, NULL, "addon state not initialized");
        return NULL;
    }
    if (urbc_ffi_dlerror_copy(&state->rt, &msg, err, sizeof(err)) != URBC_OK) {
        napi_throw_error(env, NULL, err);
        return NULL;
    }
    if (!msg) {
        NAPI_CALL(env, napi_get_null(env, &out));
        return out;
    }
    NAPI_CALL(env, napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &out));
    return out;
}

static napi_value urbnapi_mem_alloc(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    uint64_t size = 0;
    char err[URBC_ERROR_CAP];
    void *ptr;

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 1 || !urbnapi_get_uint64(env, argv[0], &size, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, argc < 1 ? "alloc expects a size" : err);
        return NULL;
    }
    ptr = malloc((size_t)size);
    return urbnapi_make_uintptr(env, (uintptr_t)ptr);
}

static napi_value urbnapi_mem_free(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    uintptr_t ptr = 0;
    char err[URBC_ERROR_CAP];

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc >= 1) {
        if (!urbnapi_get_pointer(env, argv[0], &ptr, err, sizeof(err))) {
            napi_throw_type_error(env, NULL, err);
            return NULL;
        }
        free((void *)ptr);
    }
    return urbnapi_undefined(env);
}

static napi_value urbnapi_mem_realloc(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    uintptr_t ptr = 0;
    uint64_t size = 0;
    char err[URBC_ERROR_CAP];
    void *out;

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 2) {
        napi_throw_type_error(env, NULL, "realloc expects a pointer and size");
        return NULL;
    }
    if (!urbnapi_get_pointer(env, argv[0], &ptr, err, sizeof(err)) ||
        !urbnapi_get_uint64(env, argv[1], &size, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, err);
        return NULL;
    }
    out = realloc((void *)ptr, (size_t)size);
    return urbnapi_make_uintptr(env, (uintptr_t)out);
}

static napi_value urbnapi_mem_zero(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    uintptr_t ptr = 0;
    uint64_t size = 0;
    char err[URBC_ERROR_CAP];

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 2) {
        napi_throw_type_error(env, NULL, "zero expects a pointer and size");
        return NULL;
    }
    if (!urbnapi_get_pointer(env, argv[0], &ptr, err, sizeof(err)) ||
        !urbnapi_get_uint64(env, argv[1], &size, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, err);
        return NULL;
    }
    if (ptr && size) memset((void *)ptr, 0, (size_t)size);
    return urbnapi_undefined(env);
}

static napi_value urbnapi_mem_copy(napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    uintptr_t dst = 0, src = 0;
    uint64_t size = 0;
    char err[URBC_ERROR_CAP];

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 3) {
        napi_throw_type_error(env, NULL, "copy expects dst, src and size");
        return NULL;
    }
    if (!urbnapi_get_pointer(env, argv[0], &dst, err, sizeof(err)) ||
        !urbnapi_get_pointer(env, argv[1], &src, err, sizeof(err)) ||
        !urbnapi_get_uint64(env, argv[2], &size, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, err);
        return NULL;
    }
    if (dst && src && size) memcpy((void *)dst, (void *)src, (size_t)size);
    return urbnapi_undefined(env);
}

static napi_value urbnapi_mem_set(napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    uintptr_t ptr = 0;
    int64_t bytev = 0;
    uint64_t size = 0;
    char err[URBC_ERROR_CAP];

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 3) {
        napi_throw_type_error(env, NULL, "set expects ptr, byte and size");
        return NULL;
    }
    if (!urbnapi_get_pointer(env, argv[0], &ptr, err, sizeof(err)) ||
        !urbnapi_get_int64(env, argv[1], &bytev, err, sizeof(err)) ||
        !urbnapi_get_uint64(env, argv[2], &size, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, err);
        return NULL;
    }
    if (ptr && size) memset((void *)ptr, (int)(bytev & 0xFF), (size_t)size);
    return urbnapi_undefined(env);
}

static napi_value urbnapi_mem_compare(napi_env env, napi_callback_info info)
{
    napi_value argv[3];
    size_t argc = 3;
    uintptr_t a = 0, b = 0;
    uint64_t size = 0;
    char err[URBC_ERROR_CAP];
    int cmp = 0;
    napi_value out;

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 3) {
        napi_throw_type_error(env, NULL, "compare expects a, b and size");
        return NULL;
    }
    if (!urbnapi_get_pointer(env, argv[0], &a, err, sizeof(err)) ||
        !urbnapi_get_pointer(env, argv[1], &b, err, sizeof(err)) ||
        !urbnapi_get_uint64(env, argv[2], &size, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, err);
        return NULL;
    }
    if (a && b && size) cmp = memcmp((void *)a, (void *)b, (size_t)size);
    NAPI_CALL(env, napi_create_int32(env, cmp, &out));
    return out;
}

static napi_value urbnapi_mem_nullptr(napi_env env, napi_callback_info info)
{
    (void)info;
    return urbnapi_make_uintptr(env, (uintptr_t)0);
}

static napi_value urbnapi_mem_sizeof_ptr(napi_env env, napi_callback_info info)
{
    napi_value out;
    (void)info;
    NAPI_CALL(env, napi_create_uint32(env, (uint32_t)sizeof(void *), &out));
    return out;
}

static napi_value urbnapi_mem_readptr(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    uintptr_t ptr = 0;
    uintptr_t value = 0;
    char err[URBC_ERROR_CAP];

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 1 || !urbnapi_get_pointer(env, argv[0], &ptr, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, argc < 1 ? "readptr expects a pointer" : err);
        return NULL;
    }
    if (ptr) memcpy(&value, (const void *)ptr, sizeof(value));
    return urbnapi_make_uintptr(env, value);
}

static napi_value urbnapi_mem_writeptr(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    uintptr_t ptr = 0, value = 0;
    char err[URBC_ERROR_CAP];

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 2) {
        napi_throw_type_error(env, NULL, "writeptr expects a pointer and value");
        return NULL;
    }
    if (!urbnapi_get_pointer(env, argv[0], &ptr, err, sizeof(err)) ||
        !urbnapi_get_pointer(env, argv[1], &value, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, err);
        return NULL;
    }
    if (ptr) memcpy((void *)ptr, &value, sizeof(value));
    return urbnapi_undefined(env);
}

static napi_value urbnapi_mem_readcstring(napi_env env, napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    uintptr_t ptr = 0;
    char err[URBC_ERROR_CAP];
    napi_value out;

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 1 || !urbnapi_get_pointer(env, argv[0], &ptr, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, argc < 1 ? "readcstring expects a pointer" : err);
        return NULL;
    }
    if (!ptr) {
        NAPI_CALL(env, napi_get_null(env, &out));
        return out;
    }
    NAPI_CALL(env, napi_create_string_utf8(env, (const char *)ptr, NAPI_AUTO_LENGTH, &out));
    return out;
}

static napi_value urbnapi_mem_writecstring(napi_env env, napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    uintptr_t ptr = 0;
    char err[URBC_ERROR_CAP];
    char *str = NULL;

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    if (argc < 2) {
        napi_throw_type_error(env, NULL, "writecstring expects a pointer and string");
        return NULL;
    }
    if (!urbnapi_get_pointer(env, argv[0], &ptr, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, err);
        return NULL;
    }
    if (urbnapi_is_nullish(env, argv[1])) {
        if (ptr) ((char *)ptr)[0] = '\0';
        return urbnapi_undefined(env);
    }
    if (!urbnapi_dup_string(env, argv[1], &str, err, sizeof(err))) {
        napi_throw_type_error(env, NULL, err);
        return NULL;
    }
    if (ptr) memcpy((void *)ptr, str, strlen(str) + 1);
    free(str);
    return urbnapi_undefined(env);
}

#define URBNAPI_DEFINE_READ_NUM(name, ctype, read_expr, create_expr) \
static napi_value urbnapi_##name(napi_env env, napi_callback_info info) \
{ \
    napi_value argv[1]; \
    size_t argc = 1; \
    uintptr_t ptr = 0; \
    char err[URBC_ERROR_CAP]; \
    ctype value = 0; \
    napi_value out; \
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL)); \
    if (argc < 1 || !urbnapi_get_pointer(env, argv[0], &ptr, err, sizeof(err))) { \
        napi_throw_type_error(env, NULL, argc < 1 ? #name " expects a pointer" : err); \
        return NULL; \
    } \
    if (ptr) memcpy(&value, (const void *)ptr, sizeof(value)); \
    create_expr; \
    return out; \
}

#define URBNAPI_DEFINE_WRITE_NUM(name, ctype, getter_stmt) \
static napi_value urbnapi_##name(napi_env env, napi_callback_info info) \
{ \
    napi_value argv[2]; \
    size_t argc = 2; \
    uintptr_t ptr = 0; \
    char err[URBC_ERROR_CAP]; \
    ctype value = 0; \
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL)); \
    if (argc < 2) { \
        napi_throw_type_error(env, NULL, #name " expects a pointer and value"); \
        return NULL; \
    } \
    if (!urbnapi_get_pointer(env, argv[0], &ptr, err, sizeof(err))) { \
        napi_throw_type_error(env, NULL, err); \
        return NULL; \
    } \
    getter_stmt; \
    if (ptr) memcpy((void *)ptr, &value, sizeof(value)); \
    return urbnapi_undefined(env); \
}

URBNAPI_DEFINE_READ_NUM(readi8,  int8_t,   value, NAPI_CALL(env, napi_create_int32(env, value, &out)))
URBNAPI_DEFINE_READ_NUM(readu8,  uint8_t,  value, NAPI_CALL(env, napi_create_uint32(env, value, &out)))
URBNAPI_DEFINE_READ_NUM(readi16, int16_t,  value, NAPI_CALL(env, napi_create_int32(env, value, &out)))
URBNAPI_DEFINE_READ_NUM(readu16, uint16_t, value, NAPI_CALL(env, napi_create_uint32(env, value, &out)))
URBNAPI_DEFINE_READ_NUM(readi32, int32_t,  value, NAPI_CALL(env, napi_create_int32(env, value, &out)))
URBNAPI_DEFINE_READ_NUM(readu32, uint32_t, value, NAPI_CALL(env, napi_create_uint32(env, value, &out)))
URBNAPI_DEFINE_READ_NUM(readi64, int64_t,  value, out = urbnapi_make_int64(env, value); if (!out) return NULL)
URBNAPI_DEFINE_READ_NUM(readu64, uint64_t, value, out = urbnapi_make_uint64(env, value); if (!out) return NULL)
URBNAPI_DEFINE_READ_NUM(readf32, float,    value, NAPI_CALL(env, napi_create_double(env, (double)value, &out)))
URBNAPI_DEFINE_READ_NUM(readf64, double,   value, NAPI_CALL(env, napi_create_double(env, value, &out)))

URBNAPI_DEFINE_WRITE_NUM(writei8,  int8_t,  do { int64_t v; if (!urbnapi_get_int64(env, argv[1], &v, err, sizeof(err))) { napi_throw_type_error(env, NULL, err); return NULL; } value = (int8_t)v; } while (0))
URBNAPI_DEFINE_WRITE_NUM(writeu8,  uint8_t, do { uint64_t v; if (!urbnapi_get_uint64(env, argv[1], &v, err, sizeof(err))) { napi_throw_type_error(env, NULL, err); return NULL; } value = (uint8_t)v; } while (0))
URBNAPI_DEFINE_WRITE_NUM(writei16, int16_t, do { int64_t v; if (!urbnapi_get_int64(env, argv[1], &v, err, sizeof(err))) { napi_throw_type_error(env, NULL, err); return NULL; } value = (int16_t)v; } while (0))
URBNAPI_DEFINE_WRITE_NUM(writeu16, uint16_t, do { uint64_t v; if (!urbnapi_get_uint64(env, argv[1], &v, err, sizeof(err))) { napi_throw_type_error(env, NULL, err); return NULL; } value = (uint16_t)v; } while (0))
URBNAPI_DEFINE_WRITE_NUM(writei32, int32_t, do { int64_t v; if (!urbnapi_get_int64(env, argv[1], &v, err, sizeof(err))) { napi_throw_type_error(env, NULL, err); return NULL; } value = (int32_t)v; } while (0))
URBNAPI_DEFINE_WRITE_NUM(writeu32, uint32_t, do { uint64_t v; if (!urbnapi_get_uint64(env, argv[1], &v, err, sizeof(err))) { napi_throw_type_error(env, NULL, err); return NULL; } value = (uint32_t)v; } while (0))
URBNAPI_DEFINE_WRITE_NUM(writei64, int64_t, do { int64_t v; if (!urbnapi_get_int64(env, argv[1], &v, err, sizeof(err))) { napi_throw_type_error(env, NULL, err); return NULL; } value = (int64_t)v; } while (0))
URBNAPI_DEFINE_WRITE_NUM(writeu64, uint64_t, do { uint64_t v; if (!urbnapi_get_uint64(env, argv[1], &v, err, sizeof(err))) { napi_throw_type_error(env, NULL, err); return NULL; } value = (uint64_t)v; } while (0))
URBNAPI_DEFINE_WRITE_NUM(writef32, float, do { double v; if (!urbnapi_get_double(env, argv[1], &v, err, sizeof(err))) { napi_throw_type_error(env, NULL, err); return NULL; } value = (float)v; } while (0))
URBNAPI_DEFINE_WRITE_NUM(writef64, double, do { double v; if (!urbnapi_get_double(env, argv[1], &v, err, sizeof(err))) { napi_throw_type_error(env, NULL, err); return NULL; } value = (double)v; } while (0))

static napi_value urbnapi_make_constants(napi_env env)
{
    napi_value obj;
    napi_value v;
    NAPI_CALL(env, napi_create_object(env, &obj));
#define SET_CONST(name, value) do { \
    NAPI_CALL(env, napi_create_int32(env, (value), &v)); \
    NAPI_CALL(env, napi_set_named_property(env, obj, (name), v)); \
} while (0)
    SET_CONST("LAZY", URBC_DLOPEN_LAZY);
    SET_CONST("NOW", URBC_DLOPEN_NOW);
    SET_CONST("LOCAL", URBC_DLOPEN_LOCAL);
    SET_CONST("GLOBAL", URBC_DLOPEN_GLOBAL);
    SET_CONST("NODELETE", URBC_DLOPEN_NODELETE);
    SET_CONST("NOLOAD", URBC_DLOPEN_NOLOAD);
#undef SET_CONST
    return obj;
}

static napi_value Init(napi_env env, napi_value exports)
{
    AddonState *state = (AddonState *)calloc(1, sizeof(*state));
    napi_property_descriptor props[] = {
        { "open", 0, urbnapi_open, 0, 0, 0, napi_default, 0 },
        { "close", 0, urbnapi_close, 0, 0, 0, napi_default, 0 },
        { "sym", 0, urbnapi_sym, 0, 0, 0, napi_default, 0 },
        { "symSelf", 0, urbnapi_sym_self, 0, 0, 0, napi_default, 0 },
        { "describe", 0, urbnapi_describe, 0, 0, 0, napi_default, 0 },
        { "describeDescriptor", 0, urbnapi_describe_descriptor, 0, 0, 0, napi_default, 0 },
        { "bind", 0, urbnapi_bind, 0, 0, 0, napi_default, 0 },
        { "callBound", 0, urbnapi_call_bound, 0, 0, 0, napi_default, 0 },
        { "callback", 0, urbnapi_callback, 0, 0, 0, napi_default, 0 },
        { "errnoValue", 0, urbnapi_errno_value, 0, 0, 0, napi_default, 0 },
        { "dlerror", 0, urbnapi_dlerror, 0, 0, 0, napi_default, 0 },
        { "alloc", 0, urbnapi_mem_alloc, 0, 0, 0, napi_default, 0 },
        { "free", 0, urbnapi_mem_free, 0, 0, 0, napi_default, 0 },
        { "realloc", 0, urbnapi_mem_realloc, 0, 0, 0, napi_default, 0 },
        { "zero", 0, urbnapi_mem_zero, 0, 0, 0, napi_default, 0 },
        { "copy", 0, urbnapi_mem_copy, 0, 0, 0, napi_default, 0 },
        { "set", 0, urbnapi_mem_set, 0, 0, 0, napi_default, 0 },
        { "compare", 0, urbnapi_mem_compare, 0, 0, 0, napi_default, 0 },
        { "nullptr", 0, urbnapi_mem_nullptr, 0, 0, 0, napi_default, 0 },
        { "sizeofPtr", 0, urbnapi_mem_sizeof_ptr, 0, 0, 0, napi_default, 0 },
        { "readptr", 0, urbnapi_mem_readptr, 0, 0, 0, napi_default, 0 },
        { "writeptr", 0, urbnapi_mem_writeptr, 0, 0, 0, napi_default, 0 },
        { "readcstring", 0, urbnapi_mem_readcstring, 0, 0, 0, napi_default, 0 },
        { "writecstring", 0, urbnapi_mem_writecstring, 0, 0, 0, napi_default, 0 },
        { "readi8", 0, urbnapi_readi8, 0, 0, 0, napi_default, 0 },
        { "readu8", 0, urbnapi_readu8, 0, 0, 0, napi_default, 0 },
        { "readi16", 0, urbnapi_readi16, 0, 0, 0, napi_default, 0 },
        { "readu16", 0, urbnapi_readu16, 0, 0, 0, napi_default, 0 },
        { "readi32", 0, urbnapi_readi32, 0, 0, 0, napi_default, 0 },
        { "readu32", 0, urbnapi_readu32, 0, 0, 0, napi_default, 0 },
        { "readi64", 0, urbnapi_readi64, 0, 0, 0, napi_default, 0 },
        { "readu64", 0, urbnapi_readu64, 0, 0, 0, napi_default, 0 },
        { "readf32", 0, urbnapi_readf32, 0, 0, 0, napi_default, 0 },
        { "readf64", 0, urbnapi_readf64, 0, 0, 0, napi_default, 0 },
        { "writei8", 0, urbnapi_writei8, 0, 0, 0, napi_default, 0 },
        { "writeu8", 0, urbnapi_writeu8, 0, 0, 0, napi_default, 0 },
        { "writei16", 0, urbnapi_writei16, 0, 0, 0, napi_default, 0 },
        { "writeu16", 0, urbnapi_writeu16, 0, 0, 0, napi_default, 0 },
        { "writei32", 0, urbnapi_writei32, 0, 0, 0, napi_default, 0 },
        { "writeu32", 0, urbnapi_writeu32, 0, 0, 0, napi_default, 0 },
        { "writei64", 0, urbnapi_writei64, 0, 0, 0, napi_default, 0 },
        { "writeu64", 0, urbnapi_writeu64, 0, 0, 0, napi_default, 0 },
        { "writef32", 0, urbnapi_writef32, 0, 0, 0, napi_default, 0 },
        { "writef64", 0, urbnapi_writef64, 0, 0, 0, napi_default, 0 },
    };
    napi_value constants;

    if (!state) {
        napi_throw_error(env, NULL, "out of memory");
        return NULL;
    }
    urbc_image_init(&state->image);
    urbc_runtime_init(&state->rt);
    state->rt.image = &state->image;
    state->next_callback_id = 0;
    state->callbacks = NULL;

    if (napi_set_instance_data(env, state, urbnapi_addon_finalize, NULL) != napi_ok) {
        urbnapi_addon_finalize(env, state, NULL);
        urbnapi_throw_status(env, napi_generic_failure, "napi_set_instance_data");
        return NULL;
    }
    NAPI_CALL(env, napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props));
    constants = urbnapi_make_constants(env);
    if (!constants) return NULL;
    NAPI_CALL(env, napi_set_named_property(env, exports, "dlopenFlags", constants));
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
