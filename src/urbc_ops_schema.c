#include "urbc_internal.h"

static int urbc_schema_need_stack(List *stack, Int n, const char *name)
{
    UrbcRuntime *rt = urbc_runtime_current();
    if (!stack || stack->size < n) {
        urbc_runtime_fail(rt, "%s: stack underflow", name);
        return 0;
    }
    return 1;
}

static UrbcViewHandle *urbc_new_view(UrbcRuntime *rt, UrbcCompiledSchema *schema,
                                     uintptr_t base_ptr, const char *what)
{
    UrbcViewHandle *view = (UrbcViewHandle *)urbc_runtime_alloc_owned(rt, sizeof(*view));
    if (!view) {
        urbc_runtime_fail(rt, "%s: out of memory", what);
        return NULL;
    }
    view->magic = URBC_VIEW_MAGIC;
    view->base_ptr = base_ptr;
    view->schema = schema;
    return view;
}

static UrbcViewArrayHandle *urbc_new_varray(UrbcRuntime *rt, UrbcCompiledSchema *schema,
                                            uintptr_t base_ptr, uint32_t count,
                                            const char *what)
{
    UrbcViewArrayHandle *arr = (UrbcViewArrayHandle *)urbc_runtime_alloc_owned(rt, sizeof(*arr));
    if (!arr) {
        urbc_runtime_fail(rt, "%s: out of memory", what);
        return NULL;
    }
    arr->magic = URBC_VARRAY_MAGIC;
    arr->base_ptr = base_ptr;
    arr->count = count;
    arr->schema = schema;
    arr->stride = schema->size;
    return arr;
}

static UrbcViewHandle *urbc_expect_view(Value v, const char *name)
{
    UrbcRuntime *rt = urbc_runtime_current();
    if (!v.p || *(uint32_t *)v.p != URBC_VIEW_MAGIC) {
        urbc_runtime_fail(rt, "%s: expected view handle", name);
        return NULL;
    }
    return (UrbcViewHandle *)v.p;
}

static UrbcViewArrayHandle *urbc_expect_varray(Value v, const char *name)
{
    UrbcRuntime *rt = urbc_runtime_current();
    if (!v.p || *(uint32_t *)v.p != URBC_VARRAY_MAGIC) {
        urbc_runtime_fail(rt, "%s: expected view array handle", name);
        return NULL;
    }
    return (UrbcViewArrayHandle *)v.p;
}

static uintptr_t urbc_field_addr(UrbcViewHandle *view, UrbcFieldInfo *field)
{
    return view->base_ptr + field->offset;
}

UrbcStatus urbc_view_make_handle(UrbcRuntime *rt, Value schema_value, uintptr_t base_ptr,
                                 Value *out_view, char *err, size_t err_cap)
{
    UrbcCompiledSchema *schema;
    UrbcViewHandle *view;
    UrbcRuntime *prev;
    if (!rt || !out_view) {
        urbc_copy_error(err, err_cap, "invalid view.make arguments");
        return URBC_ERR_ARGUMENT;
    }
    urbc_runtime_lock(rt);
    prev = urbc_runtime_current();
    urbc_runtime_set_current(rt);
    rt->failed = 0;
    rt->last_error[0] = '\0';
    schema = urbc_schema_from_const(rt, schema_value);
    if (!schema) goto fail;
    view = urbc_new_view(rt, schema, base_ptr, "view.make");
    if (!view) goto fail;
    *out_view = (Value){ .p = view };
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_OK;
fail:
    urbc_copy_error(err, err_cap, rt->last_error);
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_ERR_RUNTIME;
}

UrbcStatus urbc_view_array_handle(UrbcRuntime *rt, Value schema_value, uintptr_t base_ptr,
                                  uint32_t count, Value *out_array,
                                  char *err, size_t err_cap)
{
    UrbcCompiledSchema *schema;
    UrbcViewArrayHandle *arr;
    UrbcRuntime *prev;
    if (!rt || !out_array) {
        urbc_copy_error(err, err_cap, "invalid view.array arguments");
        return URBC_ERR_ARGUMENT;
    }
    urbc_runtime_lock(rt);
    prev = urbc_runtime_current();
    urbc_runtime_set_current(rt);
    rt->failed = 0;
    rt->last_error[0] = '\0';
    schema = urbc_schema_from_const(rt, schema_value);
    if (!schema) goto fail;
    arr = urbc_new_varray(rt, schema, base_ptr, count, "view.array");
    if (!arr) goto fail;
    *out_array = (Value){ .p = arr };
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_OK;
fail:
    urbc_copy_error(err, err_cap, rt->last_error);
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_ERR_RUNTIME;
}

UrbcStatus urbc_view_get_value(UrbcRuntime *rt, Value handle, const char *name,
                               Value *out_value, char *err, size_t err_cap)
{
    UrbcRuntime *prev;
    if (!rt || !name || !out_value) {
        urbc_copy_error(err, err_cap, "invalid view.get arguments");
        return URBC_ERR_ARGUMENT;
    }
    urbc_runtime_lock(rt);
    prev = urbc_runtime_current();
    urbc_runtime_set_current(rt);
    rt->failed = 0;
    rt->last_error[0] = '\0';

    if (handle.p && *(uint32_t *)handle.p == URBC_VIEW_MAGIC) {
        UrbcViewHandle *view = urbc_expect_view(handle, "view.get");
        UrbcFieldInfo *field;
        if (!view) goto fail;
        field = urbc_schema_field(view->schema, name);
        if (!field) {
            urbc_runtime_fail(rt, "view.get: field not found");
            goto fail;
        }
        switch (field->field_kind) {
        case URBC_FIELD_PRIM:
        case URBC_FIELD_POINTER:
            *out_value = urbc_value_from_memory(field->prim_type, (void *)urbc_field_addr(view, field));
            break;
        case URBC_FIELD_ARRAY:
            *out_value = (Value){ .u = (UInt)urbc_field_addr(view, field) };
            break;
        case URBC_FIELD_STRUCT: {
            UrbcCompiledSchema *sub = urbc_schema_compile(rt, field->ref_schema_index);
            UrbcViewHandle *subv;
            if (!sub) goto fail;
            subv = urbc_new_view(rt, sub, urbc_field_addr(view, field), "view.get");
            if (!subv) goto fail;
            *out_value = (Value){ .p = subv };
            break;
        }
        default:
            urbc_runtime_fail(rt, "view.get: unsupported field kind");
            goto fail;
        }
    } else if (handle.p && *(uint32_t *)handle.p == URBC_VARRAY_MAGIC) {
        UrbcViewArrayHandle *arr = urbc_expect_varray(handle, "view.get");
        char *end = NULL;
        unsigned long idx;
        UrbcViewHandle *elem;
        if (!arr) goto fail;
        if (strcmp(name, "count") == 0) {
            *out_value = (Value){ .u = arr->count };
        } else if (strcmp(name, "ptr") == 0) {
            *out_value = (Value){ .u = (UInt)arr->base_ptr };
        } else {
            idx = strtoul(name, &end, 10);
            if (!end || *end != '\0') {
                urbc_runtime_fail(rt, "view.get: invalid array index string");
                goto fail;
            }
            if (idx >= arr->count) {
                urbc_runtime_fail(rt, "view.get: array index out of range");
                goto fail;
            }
            elem = urbc_new_view(rt, arr->schema,
                                 arr->base_ptr + (uintptr_t)idx * arr->stride,
                                 "view.get");
            if (!elem) goto fail;
            *out_value = (Value){ .p = elem };
        }
    } else {
        urbc_runtime_fail(rt, "view.get: expected view or view array handle");
        goto fail;
    }

    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_OK;
fail:
    urbc_copy_error(err, err_cap, rt->last_error);
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_ERR_RUNTIME;
}

UrbcStatus urbc_view_set_value(UrbcRuntime *rt, Value handle, const char *name,
                               Value value, char *err, size_t err_cap)
{
    UrbcViewHandle *view;
    UrbcFieldInfo *field;
    UrbcRuntime *prev;
    if (!rt || !name) {
        urbc_copy_error(err, err_cap, "invalid view.set arguments");
        return URBC_ERR_ARGUMENT;
    }
    urbc_runtime_lock(rt);
    prev = urbc_runtime_current();
    urbc_runtime_set_current(rt);
    rt->failed = 0;
    rt->last_error[0] = '\0';
    view = urbc_expect_view(handle, "view.set");
    if (!view) goto fail;
    field = urbc_schema_field(view->schema, name);
    if (!field) {
        urbc_runtime_fail(rt, "view.set: field not found");
        goto fail;
    }
    if (field->field_kind != URBC_FIELD_PRIM && field->field_kind != URBC_FIELD_POINTER) {
        urbc_runtime_fail(rt, "view.set: field is not directly assignable");
        goto fail;
    }
    if (!urbc_value_to_memory(field->prim_type, value, (void *)urbc_field_addr(view, field))) {
        urbc_runtime_fail(rt, "view.set: incompatible value for field");
        goto fail;
    }
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_OK;
fail:
    urbc_copy_error(err, err_cap, rt->last_error);
    urbc_runtime_set_current(prev);
    urbc_runtime_unlock(rt);
    return URBC_ERR_RUNTIME;
}

void urbc_op_schema_sizeof(List *stack)
{
    UrbcCompiledSchema *schema;
    Value sv;
    if (!urbc_schema_need_stack(stack, 1, "schema.sizeof")) return;
    sv = urb_pop(stack);
    schema = urbc_schema_from_const(urbc_runtime_current(), sv);
    if (!schema) return;
    urb_push(stack, (Value){ .u = (UInt)schema->size });
}

void urbc_op_schema_offsetof(List *stack)
{
    Value namev, sv;
    UrbcCompiledSchema *schema;
    UrbcFieldInfo *field;
    if (!urbc_schema_need_stack(stack, 2, "schema.offsetof")) return;
    namev = urb_pop(stack);
    sv = urb_pop(stack);
    if (!namev.p) {
        urbc_runtime_fail(urbc_runtime_current(), "schema.offsetof: field name is null");
        return;
    }
    schema = urbc_schema_from_const(urbc_runtime_current(), sv);
    if (!schema) return;
    field = urbc_schema_field(schema, (const char *)namev.p);
    if (!field) {
        urbc_runtime_fail(urbc_runtime_current(), "schema.offsetof: field not found");
        return;
    }
    urb_push(stack, (Value){ .u = (UInt)field->offset });
}

void urbc_op_view_make(List *stack)
{
    Value sv, pv;
    UrbcCompiledSchema *schema;
    UrbcViewHandle *view;
    if (!urbc_schema_need_stack(stack, 2, "view.make")) return;
    sv = urb_pop(stack);
    pv = urb_pop(stack);
    schema = urbc_schema_from_const(urbc_runtime_current(), sv);
    if (!schema) return;
    view = urbc_new_view(urbc_runtime_current(), schema, (uintptr_t)pv.u, "view.make");
    if (!view) return;
    urb_push(stack, (Value){ .p = view });
}

void urbc_op_view_array(List *stack)
{
    Value countv, sv, pv;
    UrbcCompiledSchema *schema;
    UrbcViewArrayHandle *arr;
    if (!urbc_schema_need_stack(stack, 3, "view.array")) return;
    countv = urb_pop(stack);
    sv = urb_pop(stack);
    pv = urb_pop(stack);
    schema = urbc_schema_from_const(urbc_runtime_current(), sv);
    if (!schema) return;
    arr = urbc_new_varray(urbc_runtime_current(), schema, (uintptr_t)pv.u, (uint32_t)countv.u, "view.array");
    if (!arr) return;
    urb_push(stack, (Value){ .p = arr });
}

void urbc_op_view_get(List *stack)
{
    Value namev, hv;
    if (!urbc_schema_need_stack(stack, 2, "view.get")) return;
    namev = urb_pop(stack);
    hv = urb_pop(stack);
    if (!namev.p) {
        urbc_runtime_fail(urbc_runtime_current(), "view.get: field name is null");
        return;
    }

    if (hv.p && *(uint32_t *)hv.p == URBC_VIEW_MAGIC) {
        UrbcViewHandle *view = urbc_expect_view(hv, "view.get");
        UrbcFieldInfo *field;
        if (!view) return;
        field = urbc_schema_field(view->schema, (const char *)namev.p);
        if (!field) {
            urbc_runtime_fail(urbc_runtime_current(), "view.get: field not found");
            return;
        }
        switch (field->field_kind) {
        case URBC_FIELD_PRIM:
        case URBC_FIELD_POINTER:
            urb_push(stack, urbc_value_from_memory(field->prim_type, (void *)urbc_field_addr(view, field)));
            return;
        case URBC_FIELD_ARRAY:
            urb_push(stack, (Value){ .u = (UInt)urbc_field_addr(view, field) });
            return;
        case URBC_FIELD_STRUCT: {
            UrbcCompiledSchema *sub = urbc_schema_compile(urbc_runtime_current(), field->ref_schema_index);
            UrbcViewHandle *subv;
            if (!sub) return;
            subv = urbc_new_view(urbc_runtime_current(), sub, urbc_field_addr(view, field), "view.get");
            if (!subv) return;
            urb_push(stack, (Value){ .p = subv });
            return;
        }
        default:
            urbc_runtime_fail(urbc_runtime_current(), "view.get: unsupported field kind");
            return;
        }
    }

    if (hv.p && *(uint32_t *)hv.p == URBC_VARRAY_MAGIC) {
        UrbcViewArrayHandle *arr = urbc_expect_varray(hv, "view.get");
        char *end = NULL;
        unsigned long idx;
        UrbcViewHandle *elem;
        if (!arr) return;
        if (strcmp((const char *)namev.p, "count") == 0) {
            urb_push(stack, (Value){ .u = arr->count });
            return;
        }
        if (strcmp((const char *)namev.p, "ptr") == 0) {
            urb_push(stack, (Value){ .u = (UInt)arr->base_ptr });
            return;
        }
        idx = strtoul((const char *)namev.p, &end, 10);
        if (!end || *end != '\0') {
            urbc_runtime_fail(urbc_runtime_current(), "view.get: invalid array index string");
            return;
        }
        if (idx >= arr->count) {
            urbc_runtime_fail(urbc_runtime_current(), "view.get: array index out of range");
            return;
        }
        elem = urbc_new_view(urbc_runtime_current(), arr->schema,
                     arr->base_ptr + (uintptr_t)idx * arr->stride,
                     "view.get");
        if (!elem) return;
        urb_push(stack, (Value){ .p = elem });
        return;
    }

    urbc_runtime_fail(urbc_runtime_current(), "view.get: expected view or view array handle");
}

void urbc_op_view_set(List *stack)
{
    Value vv, namev, hv;
    UrbcViewHandle *view;
    UrbcFieldInfo *field;
    if (!urbc_schema_need_stack(stack, 3, "view.set")) return;
    vv = urb_pop(stack);
    namev = urb_pop(stack);
    hv = urb_pop(stack);
    if (!namev.p) {
        urbc_runtime_fail(urbc_runtime_current(), "view.set: field name is null");
        return;
    }
    view = urbc_expect_view(hv, "view.set");
    if (!view) return;
    field = urbc_schema_field(view->schema, (const char *)namev.p);
    if (!field) {
        urbc_runtime_fail(urbc_runtime_current(), "view.set: field not found");
        return;
    }
    if (field->field_kind != URBC_FIELD_PRIM && field->field_kind != URBC_FIELD_POINTER) {
        urbc_runtime_fail(urbc_runtime_current(), "view.set: field is not directly assignable");
        return;
    }
    if (!urbc_value_to_memory(field->prim_type, vv, (void *)urbc_field_addr(view, field))) {
        urbc_runtime_fail(urbc_runtime_current(), "view.set: incompatible value for field");
    }
}

void urbc_op_union_make(List *stack)
{
    Value sv, pv;
    UrbcCompiledSchema *schema;
    UrbcViewHandle *view;
    if (!urbc_schema_need_stack(stack, 2, "union.make")) return;
    sv = urb_pop(stack);
    pv = urb_pop(stack);
    schema = urbc_schema_from_const(urbc_runtime_current(), sv);
    if (!schema) return;
    if (schema->schema_kind != URBC_SCHEMA_UNION) {
        urbc_runtime_fail(urbc_runtime_current(), "union.make: schema is not a union");
        return;
    }
    view = urbc_new_view(urbc_runtime_current(), schema, (uintptr_t)pv.u, "union.make");
    if (!view) return;
    urb_push(stack, (Value){ .p = view });
}

void urbc_op_union_sizeof(List *stack)
{
    Value sv;
    UrbcCompiledSchema *schema;
    if (!urbc_schema_need_stack(stack, 1, "union.sizeof")) return;
    sv = urb_pop(stack);
    schema = urbc_schema_from_const(urbc_runtime_current(), sv);
    if (!schema) return;
    if (schema->schema_kind != URBC_SCHEMA_UNION) {
        urbc_runtime_fail(urbc_runtime_current(), "union.sizeof: schema is not a union");
        return;
    }
    urb_push(stack, (Value){ .u = (UInt)schema->size });
}
