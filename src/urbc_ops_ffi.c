#include "urbc_internal.h"

#include <errno.h>
#include <ffi.h>

typedef union UrbcArgBuf {
    int8_t i8; uint8_t u8;
    int16_t i16; uint16_t u16;
    int32_t i32; uint32_t u32;
    int64_t i64; uint64_t u64;
    float f32; double f64;
    uintptr_t ptr;
} UrbcArgBuf;

typedef struct UrbcBoundFn {
    uint32_t magic;
    void *fn_ptr;
    ffi_cif cif;
    ffi_type *ret_type;
    ffi_type *arg_types[FSIG_MAX_ARGS];
    FsigParsed sig;
} UrbcBoundFn;

typedef struct UrbcCallbackClosure {
    uint32_t magic;
    UrbcRuntime *rt;
    ffi_closure *closure;
    void *fn_ptr;
    ffi_cif cif;
    ffi_type *arg_types[FSIG_MAX_ARGS];
    FsigParsed sig;
    UrbcHostBinding *binding;
} UrbcCallbackClosure;

typedef struct UrbcFfiDescriptorCacheEntry {
    char *signature;
    UrbcFfiDescriptor *descriptor;
} UrbcFfiDescriptorCacheEntry;

struct UrbcFfiDescriptor {
    uint32_t magic;
    FsigParsed sig;
};

#define URBC_FFI_DESC_MAGIC 0x55464453u

static void urbc_callback_trampoline(ffi_cif *cif, void *ret, void **args, void *user);

static int urbc_ffi_descriptor_valid(const UrbcFfiDescriptor *desc)
{
    return desc && desc->magic == URBC_FFI_DESC_MAGIC;
}

static UrbcStatus urbc_ffi_descriptor_cache_get(UrbcRuntime *rt,
                                                const char *sig,
                                                const UrbcFfiDescriptor **out_desc,
                                                char *err, size_t err_cap)
{
    UHalf i;
    UrbcFfiDescriptor *desc;
    UrbcFfiDescriptorCacheEntry *entry;
    UrbcStatus st;

    if (!rt || !sig || !out_desc) {
        urbc_copy_error(err, err_cap, "invalid ffi descriptor cache arguments");
        return URBC_ERR_ARGUMENT;
    }
    if (rt->owned_ffi_descriptors) {
        for (i = 0; i < rt->owned_ffi_descriptors->size; i++) {
            entry = (UrbcFfiDescriptorCacheEntry *)rt->owned_ffi_descriptors->data[i].p;
            if (entry && entry->signature && strcmp(entry->signature, sig) == 0 &&
                urbc_ffi_descriptor_valid(entry->descriptor)) {
                *out_desc = entry->descriptor;
                return URBC_OK;
            }
        }
    }

    st = urbc_ffi_describe(sig, &desc, err, err_cap);
    if (st != URBC_OK) return st;

    if (!rt->owned_ffi_descriptors) {
        rt->owned_ffi_descriptors = urb_new(4);
        if (!rt->owned_ffi_descriptors) {
            urbc_ffi_descriptor_release(desc);
            urbc_copy_error(err, err_cap, "ffi descriptor cache: out of memory");
            return URBC_ERR_ALLOC;
        }
    }

    entry = (UrbcFfiDescriptorCacheEntry *)calloc(1, sizeof(*entry));
    if (!entry) {
        urbc_ffi_descriptor_release(desc);
        urbc_copy_error(err, err_cap, "ffi descriptor cache: out of memory");
        return URBC_ERR_ALLOC;
    }

    entry->signature = (char *)malloc(strlen(sig) + 1);
    if (!entry->signature) {
        free(entry);
        urbc_ffi_descriptor_release(desc);
        urbc_copy_error(err, err_cap, "ffi descriptor cache: out of memory");
        return URBC_ERR_ALLOC;
    }
    strcpy(entry->signature, sig);
    entry->descriptor = desc;
    urb_push(rt->owned_ffi_descriptors, (Value){ .p = entry });
    *out_desc = desc;
    return URBC_OK;
}

static int urbc_ffi_need_stack(List *stack, Int n, const char *name)
{
    UrbcRuntime *rt = urbc_runtime_current();
    if (!stack || stack->size < n) {
        urbc_runtime_fail(rt, "%s: stack underflow", name);
        return 0;
    }
    return 1;
}

static ffi_type *urbc_fsig_ffi_type(FsigBase base)
{
    switch (base) {
    case FSIG_VOID: return &ffi_type_void;
    case FSIG_I8: return &ffi_type_sint8;
    case FSIG_U8: case FSIG_BOOL: return &ffi_type_uint8;
    case FSIG_I16: return &ffi_type_sint16;
    case FSIG_U16: return &ffi_type_uint16;
    case FSIG_I32: return &ffi_type_sint32;
    case FSIG_U32: return &ffi_type_uint32;
    case FSIG_I64: return &ffi_type_sint64;
    case FSIG_U64: return &ffi_type_uint64;
    case FSIG_F32: return &ffi_type_float;
    case FSIG_F64: return &ffi_type_double;
    case FSIG_CSTRING: case FSIG_POINTER: return &ffi_type_pointer;
    default: return NULL;
    }
}

static ffi_type *urbc_prim_ffi_type(uint8_t prim)
{
    switch (prim) {
    case URBC_PRIM_BOOL:
    case URBC_PRIM_U8:      return &ffi_type_uint8;
    case URBC_PRIM_I8:      return &ffi_type_sint8;
    case URBC_PRIM_U16:     return &ffi_type_uint16;
    case URBC_PRIM_I16:     return &ffi_type_sint16;
    case URBC_PRIM_U32:     return &ffi_type_uint32;
    case URBC_PRIM_I32:     return &ffi_type_sint32;
    case URBC_PRIM_U64:     return &ffi_type_uint64;
    case URBC_PRIM_I64:     return &ffi_type_sint64;
    case URBC_PRIM_F32:     return &ffi_type_float;
    case URBC_PRIM_F64:     return &ffi_type_double;
    case URBC_PRIM_POINTER:
    case URBC_PRIM_CSTRING: return &ffi_type_pointer;
    default: return NULL;
    }
}

static int urbc_fsig_to_prim(FsigBase base, uint8_t *prim)
{
    switch (base) {
    case FSIG_BOOL:    *prim = URBC_PRIM_BOOL; return 1;
    case FSIG_I8:      *prim = URBC_PRIM_I8; return 1;
    case FSIG_U8:      *prim = URBC_PRIM_U8; return 1;
    case FSIG_I16:     *prim = URBC_PRIM_I16; return 1;
    case FSIG_U16:     *prim = URBC_PRIM_U16; return 1;
    case FSIG_I32:     *prim = URBC_PRIM_I32; return 1;
    case FSIG_U32:     *prim = URBC_PRIM_U32; return 1;
    case FSIG_I64:     *prim = URBC_PRIM_I64; return 1;
    case FSIG_U64:     *prim = URBC_PRIM_U64; return 1;
    case FSIG_F32:     *prim = URBC_PRIM_F32; return 1;
    case FSIG_F64:     *prim = URBC_PRIM_F64; return 1;
    case FSIG_POINTER: *prim = URBC_PRIM_POINTER; return 1;
    case FSIG_CSTRING: *prim = URBC_PRIM_CSTRING; return 1;
    default: return 0;
    }
}

static UrbcBoundFn *urbc_bound_from_value(Value v, const char *name)
{
    UrbcRuntime *rt = urbc_runtime_current();
    if (!v.p || *(uint32_t *)v.p != URBC_BOUND_MAGIC) {
        urbc_runtime_fail(rt, "%s: expected bound fn handle", name);
        return NULL;
    }
    return (UrbcBoundFn *)v.p;
}

static UrbcBoundFn *urbc_bound_from_ptr(void *ptr, UrbcRuntime *rt, const char *name)
{
    if (!ptr || *(uint32_t *)ptr != URBC_BOUND_MAGIC) {
        urbc_runtime_fail(rt, "%s: expected bound fn handle", name);
        return NULL;
    }
    return (UrbcBoundFn *)ptr;
}

static void *urbc_value_to_arg(Value v, const FsigType *type, UrbcArgBuf *buf)
{
    switch (type->base) {
    case FSIG_BOOL: buf->u8 = (uint8_t)(v.i != 0); return buf;
    case FSIG_I8:   buf->i8 = (int8_t)v.i; return buf;
    case FSIG_U8:   buf->u8 = (uint8_t)v.u; return buf;
    case FSIG_I16:  buf->i16 = (int16_t)v.i; return buf;
    case FSIG_U16:  buf->u16 = (uint16_t)v.u; return buf;
    case FSIG_I32:  buf->i32 = (int32_t)v.i; return buf;
    case FSIG_U32:  buf->u32 = (uint32_t)v.u; return buf;
    case FSIG_I64:  buf->i64 = (int64_t)v.i; return buf;
    case FSIG_U64:  buf->u64 = (uint64_t)v.u; return buf;
    case FSIG_F32:  buf->f32 = (float)v.f; return buf;
    case FSIG_F64:  buf->f64 = (double)v.f; return buf;
    case FSIG_CSTRING:
    case FSIG_POINTER:
        buf->ptr = (uintptr_t)v.u;
        return buf;
    default:
        return NULL;
    }
}

static void *urbc_value_to_arg_prim(Value v, uint8_t prim_type, UrbcArgBuf *buf)
{
    switch (prim_type) {
    case URBC_PRIM_BOOL:    buf->u8 = (uint8_t)(v.i != 0); return buf;
    case URBC_PRIM_I8:      buf->i8 = (int8_t)v.i; return buf;
    case URBC_PRIM_U8:      buf->u8 = (uint8_t)v.u; return buf;
    case URBC_PRIM_I16:     buf->i16 = (int16_t)v.i; return buf;
    case URBC_PRIM_U16:     buf->u16 = (uint16_t)v.u; return buf;
    case URBC_PRIM_I32:     buf->i32 = (int32_t)v.i; return buf;
    case URBC_PRIM_U32:     buf->u32 = (uint32_t)v.u; return buf;
    case URBC_PRIM_I64:     buf->i64 = (int64_t)v.i; return buf;
    case URBC_PRIM_U64:     buf->u64 = (uint64_t)v.u; return buf;
    case URBC_PRIM_F32:     buf->f32 = (float)v.f; return buf;
    case URBC_PRIM_F64:     buf->f64 = (double)v.f; return buf;
    case URBC_PRIM_CSTRING:
    case URBC_PRIM_POINTER: buf->ptr = (uintptr_t)v.p; return buf;
    default: return NULL;
    }
}

static Value urbc_value_from_ret(FsigBase base, const void *ret)
{
    uint8_t prim;
    Value out;
    memset(&out, 0, sizeof(out));
    if (base == FSIG_VOID || !ret) return out;
    if (!urbc_fsig_to_prim(base, &prim)) return out;
    return urbc_value_from_memory(prim, ret);
}

UrbcStatus urbc_ffi_describe(const char *sig,
                             UrbcFfiDescriptor **out_desc,
                             char *err, size_t err_cap)
{
    UrbcFfiDescriptor *desc;
    char parse_err[256];

    if (!sig || !out_desc) {
        urbc_copy_error(err, err_cap, "invalid ffi.describe arguments");
        return URBC_ERR_ARGUMENT;
    }
    desc = (UrbcFfiDescriptor *)calloc(1, sizeof(*desc));
    if (!desc) {
        urbc_copy_error(err, err_cap, "ffi.describe: out of memory");
        return URBC_ERR_ALLOC;
    }
    if (!fsig_parse(sig, &desc->sig, parse_err, sizeof(parse_err))) {
        free(desc);
        urbc_copy_error(err, err_cap, parse_err);
        return URBC_ERR_ARGUMENT;
    }
    desc->magic = URBC_FFI_DESC_MAGIC;
    *out_desc = desc;
    return URBC_OK;
}

void urbc_ffi_descriptor_release(UrbcFfiDescriptor *desc)
{
    if (!desc) return;
    desc->magic = 0;
    free(desc);
}

UrbcStatus urbc_ffi_descriptor_copy_parsed(const UrbcFfiDescriptor *desc,
                                           FsigParsed *out_sig,
                                           char *err, size_t err_cap)
{
    if (!urbc_ffi_descriptor_valid(desc) || !out_sig) {
        urbc_copy_error(err, err_cap, "invalid ffi descriptor");
        return URBC_ERR_ARGUMENT;
    }
    *out_sig = desc->sig;
    return URBC_OK;
}

UrbcStatus urbc_ffi_bind_handle_desc(UrbcRuntime *rt, void *fn_ptr,
                                     const UrbcFfiDescriptor *desc,
                                     void **out_handle, char *err, size_t err_cap)
{
    UrbcBoundFn *bf;
    UrbcRuntime *prev;
    int i;
    ffi_status st;
    if (!rt || !urbc_ffi_descriptor_valid(desc) || !out_handle) {
        urbc_copy_error(err, err_cap, "invalid ffi.bind arguments");
        return URBC_ERR_ARGUMENT;
    }
    urbc_runtime_lock(rt);
    prev = urbc_runtime_current();
    urbc_runtime_set_current(rt);
    rt->failed = 0;
    rt->last_error[0] = '\0';
    bf = (UrbcBoundFn *)urbc_runtime_alloc_owned(rt, sizeof(*bf));
    if (!bf) {
        urbc_runtime_fail(rt, "ffi.bind: out of memory");
        goto fail;
    }
    bf->sig = desc->sig;
    bf->magic = URBC_BOUND_MAGIC;
    bf->fn_ptr = fn_ptr;
    bf->ret_type = urbc_fsig_ffi_type(bf->sig.ret.base);
    if (!bf->ret_type) {
        urbc_runtime_fail(rt, "ffi.bind: unsupported return type");
        goto fail;
    }
    for (i = 0; i < bf->sig.argc; i++) {
        bf->arg_types[i] = urbc_fsig_ffi_type(bf->sig.args[i].base);
        if (!bf->arg_types[i]) {
            urbc_runtime_fail(rt, "ffi.bind: unsupported arg type %d", i);
            goto fail;
        }
    }
    st = bf->sig.has_varargs
        ? ffi_prep_cif_var(&bf->cif, FFI_DEFAULT_ABI,
                           (unsigned)bf->sig.argc, (unsigned)bf->sig.argc,
                           bf->ret_type, bf->arg_types)
        : ffi_prep_cif(&bf->cif, FFI_DEFAULT_ABI,
                       (unsigned)bf->sig.argc, bf->ret_type, bf->arg_types);
    if (st != FFI_OK) {
        urbc_runtime_fail(rt, "ffi.bind: ffi_prep_cif failed");
        goto fail;
    }
    *out_handle = bf;
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_OK;
fail:
    urbc_copy_error(err, err_cap, rt->last_error);
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_ERR_RUNTIME;
}

UrbcStatus urbc_ffi_bind_handle(UrbcRuntime *rt, void *fn_ptr, const char *sig,
                                void **out_handle, char *err, size_t err_cap)
{
    UrbcFfiDescriptor *desc = NULL;
    UrbcStatus st;
    char desc_err[URBC_ERROR_CAP];

    if (!rt || !sig || !out_handle) {
        urbc_copy_error(err, err_cap, "invalid ffi.bind arguments");
        return URBC_ERR_ARGUMENT;
    }
    st = urbc_ffi_describe(sig, &desc, desc_err, sizeof(desc_err));
    if (st != URBC_OK) {
        urbc_copy_error(err, err_cap, desc_err);
        return st;
    }
    st = urbc_ffi_bind_handle_desc(rt, fn_ptr, desc, out_handle, err, err_cap);
    urbc_ffi_descriptor_release(desc);
    return st;
}

UrbcStatus urbc_ffi_call_bound(UrbcRuntime *rt, void *bound_handle, int argc,
                               const Value *argv, const uint8_t *vararg_types,
                               Value *out, char *err, size_t err_cap)
{
    UrbcArgBuf argbufs[FSIG_MAX_ARGS];
    void *argptrs[FSIG_MAX_ARGS];
    ffi_type *all_types[FSIG_MAX_ARGS];
    ffi_cif var_cif;
    ffi_cif *cif = NULL;
    UrbcArgBuf retbuf;
    UrbcBoundFn *bf;
    UrbcRuntime *prev;
    int extra;
    int i;
    if (!rt || !bound_handle || argc < 0 || (argc > 0 && !argv)) {
        urbc_copy_error(err, err_cap, "invalid ffi.call arguments");
        return URBC_ERR_ARGUMENT;
    }
    if (argc > FSIG_MAX_ARGS) {
        urbc_copy_error(err, err_cap, "too many ffi.call arguments");
        return URBC_ERR_RANGE;
    }
    urbc_runtime_lock(rt);
    prev = urbc_runtime_current();
    urbc_runtime_set_current(rt);
    rt->failed = 0;
    rt->last_error[0] = '\0';
    bf = urbc_bound_from_ptr(bound_handle, rt, "ffi.call");
    if (!bf) goto fail;
    extra = argc - bf->sig.argc;
    if (!bf->sig.has_varargs && argc != bf->sig.argc) {
        urbc_runtime_fail(rt, "ffi.call: expected %d args, got %d", bf->sig.argc, argc);
        goto fail;
    }
    if (bf->sig.has_varargs && argc < bf->sig.argc) {
        urbc_runtime_fail(rt, "ffi.call: expected at least %d args, got %d", bf->sig.argc, argc);
        goto fail;
    }
    if (bf->sig.has_varargs && extra > 0 && !vararg_types) {
        urbc_runtime_fail(rt, "ffi.call: vararg types are required for variadic arguments");
        goto fail;
    }

    for (i = 0; i < bf->sig.argc; i++) {
        argptrs[i] = urbc_value_to_arg(argv[i], &bf->sig.args[i], &argbufs[i]);
        if (!argptrs[i]) {
            urbc_runtime_fail(rt, "ffi.call: incompatible fixed argument %d", i);
            goto fail;
        }
        all_types[i] = bf->arg_types[i];
    }
    for (; i < argc; i++) {
        ffi_type *t = urbc_prim_ffi_type(vararg_types[i - bf->sig.argc]);
        argptrs[i] = urbc_value_to_arg_prim(argv[i], vararg_types[i - bf->sig.argc], &argbufs[i]);
        if (!t || !argptrs[i]) {
            urbc_runtime_fail(rt, "ffi.call: incompatible variadic argument %d", i);
            goto fail;
        }
        all_types[i] = t;
    }

    if (bf->sig.has_varargs) {
        if (ffi_prep_cif_var(&var_cif, FFI_DEFAULT_ABI,
                             (unsigned)bf->sig.argc, (unsigned)argc,
                             bf->ret_type, all_types) != FFI_OK) {
            urbc_runtime_fail(rt, "ffi.call: ffi_prep_cif_var failed");
            goto fail;
        }
        cif = &var_cif;
    } else {
        cif = &bf->cif;
    }

    memset(&retbuf, 0, sizeof(retbuf));
    ffi_call(cif, FFI_FN(bf->fn_ptr), &retbuf, argptrs);
    if (out) *out = urbc_value_from_ret(bf->sig.ret.base, &retbuf);
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_OK;
fail:
    urbc_copy_error(err, err_cap, rt->last_error);
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_ERR_RUNTIME;
}

UrbcStatus urbc_ffi_make_callback_desc(UrbcRuntime *rt,
                                       const UrbcFfiDescriptor *desc,
                                       Value callable,
                                       void **out_fn_ptr,
                                       char *err, size_t err_cap)
{
    UrbcHostBinding *binding;
    UrbcCallbackClosure *cb;
    UrbcRuntime *prev;
    ffi_type *ret_type;
    int i;
    if (!rt || !urbc_ffi_descriptor_valid(desc) || !out_fn_ptr) {
        urbc_copy_error(err, err_cap, "invalid ffi.callback arguments");
        return URBC_ERR_ARGUMENT;
    }
    urbc_runtime_lock(rt);
    prev = urbc_runtime_current();
    urbc_runtime_set_current(rt);
    rt->failed = 0;
    rt->last_error[0] = '\0';
    binding = urbc_host_resolve(rt, callable, "ffi.callback");
    if (!binding) goto fail;
    cb = (UrbcCallbackClosure *)calloc(1, sizeof(*cb));
    if (!cb) {
        urbc_runtime_fail(rt, "ffi.callback: out of memory");
        goto fail;
    }
    cb->sig = desc->sig;
    if (cb->sig.has_varargs) {
        free(cb);
        urbc_runtime_fail(rt, "ffi.callback: variadic callbacks are not supported");
        goto fail;
    }
    ret_type = urbc_fsig_ffi_type(cb->sig.ret.base);
    if (!ret_type) {
        free(cb);
        urbc_runtime_fail(rt, "ffi.callback: unsupported return type");
        goto fail;
    }
    for (i = 0; i < cb->sig.argc; i++) {
        cb->arg_types[i] = urbc_fsig_ffi_type(cb->sig.args[i].base);
        if (!cb->arg_types[i]) {
            free(cb);
            urbc_runtime_fail(rt, "ffi.callback: unsupported arg type %d", i);
            goto fail;
        }
    }
    if (ffi_prep_cif(&cb->cif, FFI_DEFAULT_ABI, (unsigned)cb->sig.argc,
                     ret_type, cb->arg_types) != FFI_OK) {
        free(cb);
        urbc_runtime_fail(rt, "ffi.callback: ffi_prep_cif failed");
        goto fail;
    }
    cb->closure = ffi_closure_alloc(sizeof(ffi_closure), &cb->fn_ptr);
    if (!cb->closure) {
        free(cb);
        urbc_runtime_fail(rt, "ffi.callback: ffi_closure_alloc failed");
        goto fail;
    }
    cb->magic = URBC_CALLBACK_MAGIC;
    cb->rt = rt;
    cb->binding = binding;
    if (ffi_prep_closure_loc(cb->closure, &cb->cif, urbc_callback_trampoline, cb, cb->fn_ptr) != FFI_OK) {
        ffi_closure_free(cb->closure);
        free(cb);
        urbc_runtime_fail(rt, "ffi.callback: ffi_prep_closure_loc failed");
        goto fail;
    }
    if (!rt->owned_callbacks) {
        rt->owned_callbacks = urb_new(4);
        if (!rt->owned_callbacks) {
            ffi_closure_free(cb->closure);
            free(cb);
            urbc_runtime_fail(rt, "ffi.callback: out of memory");
            goto fail;
        }
    }
    urb_push(rt->owned_callbacks, (Value){ .p = cb });
    *out_fn_ptr = cb->fn_ptr;
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_OK;
fail:
    urbc_copy_error(err, err_cap, rt->last_error);
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_ERR_RUNTIME;
}

UrbcStatus urbc_ffi_make_callback(UrbcRuntime *rt, const char *sig, Value callable,
                                  void **out_fn_ptr, char *err, size_t err_cap)
{
    UrbcFfiDescriptor *desc = NULL;
    UrbcStatus st;
    char desc_err[URBC_ERROR_CAP];

    if (!rt || !sig || !out_fn_ptr) {
        urbc_copy_error(err, err_cap, "invalid ffi.callback arguments");
        return URBC_ERR_ARGUMENT;
    }
    st = urbc_ffi_describe(sig, &desc, desc_err, sizeof(desc_err));
    if (st != URBC_OK) {
        urbc_copy_error(err, err_cap, desc_err);
        return st;
    }
    st = urbc_ffi_make_callback_desc(rt, desc, callable, out_fn_ptr, err, err_cap);
    urbc_ffi_descriptor_release(desc);
    return st;
}

static Value urbc_host_calln(List *stack, int argc, const char *name)
{
    Value argv[4];
    Value namev;
    UrbcHostBinding *binding;
    int i;
    Value ret = { .i = 0 };
    if (!urbc_ffi_need_stack(stack, argc + 1, name)) return ret;
    for (i = argc - 1; i >= 0; i--) argv[i] = urb_pop(stack);
    namev = urb_pop(stack);
    binding = urbc_host_resolve(urbc_runtime_current(), namev, name);
    if (!binding) {
        urbc_runtime_fail(urbc_runtime_current(), "%s: host binding not found", name);
        return ret;
    }
    ret = urbc_host_invoke(urbc_runtime_current(), binding, argc, argv);
    urb_push(stack, ret);
    return ret;
}

static void urbc_callback_trampoline(ffi_cif *cif, void *ret, void **args, void *user)
{
    UrbcCallbackClosure *cb = (UrbcCallbackClosure *)user;
    Value argv[FSIG_MAX_ARGS];
    Value out;
    UrbcRuntime *prev;
    uint8_t prim;
    int i;
    (void)cif;
    if (!cb || !cb->binding || !cb->rt) return;
    urbc_runtime_lock(cb->rt);
    prev = urbc_runtime_current();
    urbc_runtime_set_current(cb->rt);
    cb->rt->failed = 0;
    cb->rt->last_error[0] = '\0';
    for (i = 0; i < cb->sig.argc; i++) {
        if (!urbc_fsig_to_prim(cb->sig.args[i].base, &prim)) {
            cb->rt->failed = 1;
            snprintf(cb->rt->last_error, sizeof(cb->rt->last_error), "ffi.callback: unsupported callback arg type %d", i);
            urbc_runtime_set_current(prev);
            urbc_runtime_unlock(cb->rt);
            return;
        }
        argv[i] = urbc_value_from_memory(prim, args[i]);
    }
    out = urbc_host_invoke(cb->rt, cb->binding, cb->sig.argc, argv);
    if (cb->sig.ret.base != FSIG_VOID && ret && !cb->rt->failed) {
        if (!urbc_fsig_to_prim(cb->sig.ret.base, &prim) || !urbc_value_to_memory(prim, out, ret)) {
            cb->rt->failed = 1;
            snprintf(cb->rt->last_error, sizeof(cb->rt->last_error), "ffi.callback: unsupported callback return type");
        }
    }
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(cb->rt);
}

void urbc_ffi_cleanup_runtime(UrbcRuntime *rt)
{
    if (!rt) return;
    if (rt->owned_callbacks) {
        while (rt->owned_callbacks->size > 0) {
            UrbcCallbackClosure *cb = (UrbcCallbackClosure *)urb_pop(rt->owned_callbacks).p;
            if (cb) {
                if (cb->closure) ffi_closure_free(cb->closure);
                free(cb);
            }
        }
        urb_free(rt->owned_callbacks);
        rt->owned_callbacks = NULL;
    }

    if (rt->owned_ffi_descriptors) {
        while (rt->owned_ffi_descriptors->size > 0) {
            UrbcFfiDescriptorCacheEntry *entry =
                (UrbcFfiDescriptorCacheEntry *)urb_pop(rt->owned_ffi_descriptors).p;
            if (entry) {
                free(entry->signature);
                urbc_ffi_descriptor_release(entry->descriptor);
                free(entry);
            }
        }
        urb_free(rt->owned_ffi_descriptors);
        rt->owned_ffi_descriptors = NULL;
    }
}

static void urbc_ffi_calln(List *stack, int argc, const char *name)
{
    Value argv[FSIG_MAX_ARGS];
    Value fnv;
    Value out;
    int i;
    char err[URBC_ERROR_CAP];

    if (!urbc_ffi_need_stack(stack, argc + 1, name)) return;
    for (i = argc - 1; i >= 0; i--) argv[i] = urb_pop(stack);
    fnv = urb_pop(stack);
    if (urbc_ffi_call_bound(urbc_runtime_current(), fnv.p, argc, argv, NULL,
                            &out, err, sizeof(err)) != URBC_OK) {
        urbc_runtime_fail(urbc_runtime_current(), "%s", err);
        return;
    }
    urb_push(stack, out);
}

void urbc_op_ffi_open(List *stack)
{
    Value flagsv, pathv;
    int flags;
    void *handle;
    if (!urbc_ffi_need_stack(stack, 2, "ffi.open")) return;
    flagsv = urb_pop(stack);
    pathv = urb_pop(stack);
    if (!pathv.p) {
        urbc_runtime_fail(urbc_runtime_current(), "ffi.open: path is null");
        return;
    }
    flags = (int)flagsv.i;
    handle = urbc_dyn_open((const char *)pathv.p, flags);
    urb_push(stack, (Value){ .p = handle });
}

void urbc_op_ffi_close(List *stack)
{
    Value hv;
    if (!urbc_ffi_need_stack(stack, 1, "ffi.close")) return;
    hv = urb_pop(stack);
    if (hv.p) (void)urbc_dyn_close(hv.p);
}

void urbc_op_ffi_sym(List *stack)
{
    Value namev, hv;
    void *sym;
    if (!urbc_ffi_need_stack(stack, 2, "ffi.sym")) return;
    namev = urb_pop(stack);
    hv = urb_pop(stack);
    if (!namev.p) {
        urbc_runtime_fail(urbc_runtime_current(), "ffi.sym: name is null");
        return;
    }
    sym = urbc_dyn_sym(hv.p, (const char *)namev.p);
    urb_push(stack, (Value){ .p = sym });
}

void urbc_op_ffi_sym_self(List *stack)
{
    Value namev;
    void *sym;
    if (!urbc_ffi_need_stack(stack, 1, "ffi.sym_self")) return;
    namev = urb_pop(stack);
    if (!namev.p) {
        urbc_runtime_fail(urbc_runtime_current(), "ffi.sym_self: name is null");
        return;
    }
    sym = urbc_dyn_sym_self((const char *)namev.p);
    urb_push(stack, (Value){ .p = sym });
}

void urbc_op_ffi_bind(List *stack)
{
    Value sigv, ptrv;
    const UrbcFfiDescriptor *desc = NULL;
    void *handle = NULL;
    UrbcRuntime *rt = urbc_runtime_current();
    char err[URBC_ERROR_CAP];
    if (!urbc_ffi_need_stack(stack, 2, "ffi.bind")) return;
    sigv = urb_pop(stack);
    ptrv = urb_pop(stack);
    if (!sigv.p) {
        urbc_runtime_fail(rt, "ffi.bind: signature is null");
        return;
    }
    if (urbc_ffi_descriptor_cache_get(rt, (const char *)sigv.p, &desc,
                                      err, sizeof(err)) != URBC_OK) {
        urbc_runtime_fail(rt, "%s", err);
        return;
    }
    if (urbc_ffi_bind_handle_desc(rt, ptrv.p, desc, &handle,
                                  err, sizeof(err)) != URBC_OK) {
        urbc_runtime_fail(rt, "%s", err);
        return;
    }
    urb_push(stack, (Value){ .p = handle });
}

void urbc_op_ffi_call0(List *stack) { urbc_ffi_calln(stack, 0, "ffi.call0"); }
void urbc_op_ffi_call1(List *stack) { urbc_ffi_calln(stack, 1, "ffi.call1"); }
void urbc_op_ffi_call2(List *stack) { urbc_ffi_calln(stack, 2, "ffi.call2"); }
void urbc_op_ffi_call3(List *stack) { urbc_ffi_calln(stack, 3, "ffi.call3"); }
void urbc_op_ffi_call4(List *stack) { urbc_ffi_calln(stack, 4, "ffi.call4"); }

void urbc_op_ffi_callback(List *stack)
{
    Value callablev, sigv;
    const UrbcFfiDescriptor *desc = NULL;
    void *fn_ptr = NULL;
    UrbcRuntime *rt = urbc_runtime_current();
    char err[URBC_ERROR_CAP];
    if (!urbc_ffi_need_stack(stack, 2, "ffi.callback")) return;
    callablev = urb_pop(stack);
    sigv = urb_pop(stack);
    if (!sigv.p) {
        urbc_runtime_fail(rt, "ffi.callback: signature is null");
        return;
    }
    if (urbc_ffi_descriptor_cache_get(rt, (const char *)sigv.p, &desc,
                                      err, sizeof(err)) != URBC_OK) {
        urbc_runtime_fail(rt, "%s", err);
        return;
    }
    if (urbc_ffi_make_callback_desc(rt, desc, callablev,
                                    &fn_ptr, err, sizeof(err)) != URBC_OK) {
        urbc_runtime_fail(rt, "%s", err);
        return;
    }
    urb_push(stack, (Value){ .p = fn_ptr });
}

void urbc_op_ffi_errno(List *stack)
{
    urb_push(stack, (Value){ .i = errno });
}

void urbc_op_ffi_dlerror(List *stack)
{
    const char *e = urbc_dyn_last_error();
    char *copy;
    if (!e) {
        urb_push(stack, (Value){ .p = NULL });
        return;
    }
    copy = (char *)urbc_runtime_alloc_owned(urbc_runtime_current(), strlen(e) + 1);
    if (!copy) {
        urbc_runtime_fail(urbc_runtime_current(), "ffi.dlerror: out of memory");
        return;
    }
    strcpy(copy, e);
    urb_push(stack, (Value){ .p = copy });
}

void urbc_op_ffi_callv(List *stack)
{
    Value extrav, argcv;
    Value argv[FSIG_MAX_ARGS];
    uint8_t kinds[FSIG_MAX_ARGS];
    Value fnv;
    Value out;
    UrbcBoundFn *bf;
    int argc;
    int extra;
    int i;
    char err[URBC_ERROR_CAP];
    if (!urbc_ffi_need_stack(stack, 3, "ffi.callv")) return;
    extrav = urb_pop(stack);
    argcv = urb_pop(stack);
    argc = (int)argcv.i;
    extra = (int)extrav.i;
    if (argc < 0 || argc > FSIG_MAX_ARGS) {
        urbc_runtime_fail(urbc_runtime_current(), "ffi.callv: invalid argc");
        return;
    }
    if (extra < 0 || extra > argc) {
        urbc_runtime_fail(urbc_runtime_current(), "ffi.callv: invalid extra argc");
        return;
    }
    if (!urbc_ffi_need_stack(stack, 1 + argc + extra, "ffi.callv")) return;
    fnv = stack->data[stack->size - (UHalf)(argc + extra + 1)];
    bf = urbc_bound_from_value(fnv, "ffi.callv");
    if (!bf) return;
    for (i = extra - 1; i >= 0; i--) kinds[i] = (uint8_t)urb_pop(stack).u;
    for (i = argc - 1; i >= 0; i--) argv[i] = urb_pop(stack);
    fnv = urb_pop(stack);
    if (urbc_ffi_call_bound(urbc_runtime_current(), fnv.p, argc, argv,
                            extra > 0 ? kinds : NULL, &out,
                            err, sizeof(err)) != URBC_OK) {
        urbc_runtime_fail(urbc_runtime_current(), "%s", err);
        return;
    }
    urb_push(stack, out);
}

void urbc_op_host_call0(List *stack)
{
    (void)urbc_host_calln(stack, 0, "host.call0");
}

void urbc_op_host_call1(List *stack)
{
    (void)urbc_host_calln(stack, 1, "host.call1");
}

void urbc_op_host_call2(List *stack)
{
    (void)urbc_host_calln(stack, 2, "host.call2");
}

void urbc_op_host_call3(List *stack)
{
    (void)urbc_host_calln(stack, 3, "host.call3");
}
