#include "urbc_internal.h"

#include <errno.h>

UrbcStatus urbc_host_call(UrbcRuntime *rt,
                          const char *name,
                          int argc,
                          const Value *argv,
                          Value *out,
                          char *err, size_t err_cap)
{
    UrbcHostBinding *binding;
    Value ret;
    UrbcRuntime *prev;
    if (!rt || !name || argc < 0 || (argc > 0 && !argv)) {
        urbc_copy_error(err, err_cap, "invalid host call arguments");
        return URBC_ERR_ARGUMENT;
    }
    urbc_runtime_lock(rt);
    prev = urbc_runtime_current();
    urbc_runtime_set_current(rt);
    rt->failed = 0;
    rt->last_error[0] = '\0';
    binding = urbc_runtime_find_host(rt, name);
    if (!binding) {
        urbc_runtime_fail(rt, "host.call: host binding not found");
        goto fail;
    }
    ret = urbc_host_invoke(rt, binding, argc, argv);
    if (out) *out = ret;
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_OK;
fail:
    urbc_copy_error(err, err_cap, rt->last_error);
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_ERR_RUNTIME;
}

UrbcStatus urbc_ffi_open(const char *path,
                         int flags,
                         void **out_handle,
                         char *err, size_t err_cap)
{
    void *handle;
    if (!path || !out_handle) {
        urbc_copy_error(err, err_cap, "invalid ffi.open arguments");
        return URBC_ERR_ARGUMENT;
    }
    handle = urbc_dyn_open(path, flags);
    if (!handle) {
        urbc_copy_error(err, err_cap, urbc_dyn_last_error());
        return URBC_ERR_RUNTIME;
    }
    *out_handle = handle;
    return URBC_OK;
}

void urbc_ffi_close(void *handle)
{
    (void)urbc_dyn_close(handle);
}

UrbcStatus urbc_ffi_sym(void *handle,
                        const char *name,
                        void **out_symbol,
                        char *err, size_t err_cap)
{
    void *sym;
    if (!name || !out_symbol) {
        urbc_copy_error(err, err_cap, "invalid ffi.sym arguments");
        return URBC_ERR_ARGUMENT;
    }
    sym = urbc_dyn_sym(handle, name);
    if (!sym) {
        const char *e = urbc_dyn_last_error();
        urbc_copy_error(err, err_cap, e ? e : "symbol not found");
        return URBC_ERR_RUNTIME;
    }
    *out_symbol = sym;
    return URBC_OK;
}

UrbcStatus urbc_ffi_sym_self(const char *name,
                             void **out_symbol,
                             char *err, size_t err_cap)
{
    void *sym;
    if (!name || !out_symbol) {
        urbc_copy_error(err, err_cap, "invalid ffi.sym_self arguments");
        return URBC_ERR_ARGUMENT;
    }
    sym = urbc_dyn_sym_self(name);
    if (!sym) {
        const char *e = urbc_dyn_last_error();
        urbc_copy_error(err, err_cap, e ? e : "symbol not found");
        return URBC_ERR_RUNTIME;
    }
    *out_symbol = sym;
    return URBC_OK;
}

UrbcStatus urbc_ffi_bind(UrbcRuntime *rt,
                         void *fn_ptr,
                         const char *sig,
                         void **out_bound,
                         char *err, size_t err_cap)
{
    return urbc_ffi_bind_handle(rt, fn_ptr, sig, out_bound, err, err_cap);
}

UrbcStatus urbc_ffi_bind_desc(UrbcRuntime *rt,
                              void *fn_ptr,
                              const UrbcFfiDescriptor *desc,
                              void **out_bound,
                              char *err, size_t err_cap)
{
    return urbc_ffi_bind_handle_desc(rt, fn_ptr, desc, out_bound, err, err_cap);
}

UrbcStatus urbc_ffi_call(UrbcRuntime *rt,
                         void *bound_handle,
                         int argc,
                         const Value *argv,
                         const uint8_t *vararg_types,
                         Value *out,
                         char *err, size_t err_cap)
{
    return urbc_ffi_call_bound(rt, bound_handle, argc, argv, vararg_types,
                               out, err, err_cap);
}

UrbcStatus urbc_ffi_callback(UrbcRuntime *rt,
                             const char *sig,
                             Value callable,
                             void **out_fn_ptr,
                             char *err, size_t err_cap)
{
    return urbc_ffi_make_callback(rt, sig, callable, out_fn_ptr, err, err_cap);
}

UrbcStatus urbc_ffi_callback_desc(UrbcRuntime *rt,
                                  const UrbcFfiDescriptor *desc,
                                  Value callable,
                                  void **out_fn_ptr,
                                  char *err, size_t err_cap)
{
    return urbc_ffi_make_callback_desc(rt, desc, callable, out_fn_ptr, err, err_cap);
}

int urbc_ffi_errno_value(void)
{
    return errno;
}

UrbcStatus urbc_ffi_dlerror_copy(UrbcRuntime *rt,
                                 char **out_message,
                                 char *err, size_t err_cap)
{
    const char *e;
    char *copy;
    size_t len;
    if (!rt || !out_message) {
        urbc_copy_error(err, err_cap, "invalid ffi.dlerror arguments");
        return URBC_ERR_ARGUMENT;
    }
    e = urbc_dyn_last_error();
    if (!e) {
        *out_message = NULL;
        return URBC_OK;
    }
    len = strlen(e);
    copy = (char *)urbc_runtime_alloc_owned(rt, len + 1);
    if (!copy) {
        urbc_copy_error(err, err_cap, "out of memory");
        return URBC_ERR_ALLOC;
    }
    memcpy(copy, e, len + 1);
    *out_message = copy;
    return URBC_OK;
}

UrbcStatus urbc_schema_compile_value(UrbcRuntime *rt,
                                     Value schema_value,
                                     Value *out_schema,
                                     char *err, size_t err_cap)
{
    UrbcCompiledSchema *schema;
    UrbcRuntime *prev;
    if (!rt || !out_schema) {
        urbc_copy_error(err, err_cap, "invalid schema arguments");
        return URBC_ERR_ARGUMENT;
    }
    urbc_runtime_lock(rt);
    prev = urbc_runtime_current();
    urbc_runtime_set_current(rt);
    rt->failed = 0;
    rt->last_error[0] = '\0';
    schema = urbc_schema_from_const(rt, schema_value);
    if (!schema) goto fail;
    *out_schema = (Value){ .p = schema };
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_OK;
fail:
    urbc_copy_error(err, err_cap, rt->last_error);
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_ERR_RUNTIME;
}

UrbcStatus urbc_schema_sizeof(UrbcRuntime *rt,
                              Value schema_value,
                              size_t *out_size,
                              char *err, size_t err_cap)
{
    UrbcCompiledSchema *schema;
    UrbcRuntime *prev;
    if (!rt || !out_size) {
        urbc_copy_error(err, err_cap, "invalid schema.sizeof arguments");
        return URBC_ERR_ARGUMENT;
    }
    urbc_runtime_lock(rt);
    prev = urbc_runtime_current();
    urbc_runtime_set_current(rt);
    rt->failed = 0;
    rt->last_error[0] = '\0';
    schema = urbc_schema_from_const(rt, schema_value);
    if (!schema) goto fail;
    *out_size = schema->size;
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_OK;
fail:
    urbc_copy_error(err, err_cap, rt->last_error);
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_ERR_RUNTIME;
}

UrbcStatus urbc_schema_offsetof(UrbcRuntime *rt,
                                Value schema_value,
                                const char *field_name,
                                size_t *out_offset,
                                char *err, size_t err_cap)
{
    UrbcCompiledSchema *schema;
    UrbcFieldInfo *field;
    UrbcRuntime *prev;
    if (!rt || !field_name || !out_offset) {
        urbc_copy_error(err, err_cap, "invalid schema.offsetof arguments");
        return URBC_ERR_ARGUMENT;
    }
    urbc_runtime_lock(rt);
    prev = urbc_runtime_current();
    urbc_runtime_set_current(rt);
    rt->failed = 0;
    rt->last_error[0] = '\0';
    schema = urbc_schema_from_const(rt, schema_value);
    if (!schema) goto fail;
    field = urbc_schema_field(schema, field_name);
    if (!field) {
        urbc_runtime_fail(rt, "schema.offsetof: field not found");
        goto fail;
    }
    *out_offset = field->offset;
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_OK;
fail:
    urbc_copy_error(err, err_cap, rt->last_error);
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_ERR_RUNTIME;
}

UrbcStatus urbc_view_make(UrbcRuntime *rt,
                          Value schema_value,
                          void *ptr,
                          Value *out_view,
                          char *err, size_t err_cap)
{
    return urbc_view_make_handle(rt, schema_value, (uintptr_t)ptr, out_view, err, err_cap);
}

UrbcStatus urbc_view_array(UrbcRuntime *rt,
                           Value schema_value,
                           void *ptr,
                           uint32_t count,
                           Value *out_array,
                           char *err, size_t err_cap)
{
    return urbc_view_array_handle(rt, schema_value, (uintptr_t)ptr, count, out_array, err, err_cap);
}

UrbcStatus urbc_view_get(UrbcRuntime *rt,
                         Value handle,
                         const char *name,
                         Value *out_value,
                         char *err, size_t err_cap)
{
    return urbc_view_get_value(rt, handle, name, out_value, err, err_cap);
}

UrbcStatus urbc_view_set(UrbcRuntime *rt,
                         Value handle,
                         const char *name,
                         Value value,
                         char *err, size_t err_cap)
{
    return urbc_view_set_value(rt, handle, name, value, err, err_cap);
}
