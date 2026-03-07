#include "urbc_internal.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct UrbcExecReg {
    UrbcOpId id;
    Function fn;
} UrbcExecReg;

static URBC_THREAD_LOCAL UrbcRuntime *g_urbc_runtime = NULL;

#ifdef _WIN32
void urbc_runtime_lock(UrbcRuntime *rt)
{
    DWORD self;
    if (!rt || !rt->lock_ready) return;
    self = GetCurrentThreadId();
    if (rt->lock_owned && rt->lock_owner == self) {
        rt->lock_depth++;
        return;
    }
    EnterCriticalSection(&rt->lock);
    rt->lock_owner = self;
    rt->lock_owned = 1;
    rt->lock_depth = 1;
}

void urbc_runtime_unlock(UrbcRuntime *rt)
{
    DWORD self;
    if (!rt || !rt->lock_ready || !rt->lock_owned) return;
    self = GetCurrentThreadId();
    if (rt->lock_owner != self) return;
    if (rt->lock_depth > 1) {
        rt->lock_depth--;
        return;
    }
    rt->lock_depth = 0;
    rt->lock_owned = 0;
    LeaveCriticalSection(&rt->lock);
}
#else
void urbc_runtime_lock(UrbcRuntime *rt)
{
    if (!rt || !rt->lock_ready) return;
    if (rt->lock_owned && pthread_equal(rt->lock_owner, pthread_self())) {
        rt->lock_depth++;
        return;
    }
    (void)pthread_mutex_lock(&rt->lock);
    rt->lock_owner = pthread_self();
    rt->lock_owned = 1;
    rt->lock_depth = 1;
}

void urbc_runtime_unlock(UrbcRuntime *rt)
{
    if (!rt || !rt->lock_ready || !rt->lock_owned) return;
    if (!pthread_equal(rt->lock_owner, pthread_self())) return;
    if (rt->lock_depth > 1) {
        rt->lock_depth--;
        return;
    }
    rt->lock_depth = 0;
    rt->lock_owned = 0;
    (void)pthread_mutex_unlock(&rt->lock);
}
#endif

UrbcRuntime *urbc_runtime_current(void)
{
    return g_urbc_runtime;
}

void urbc_runtime_set_current(UrbcRuntime *rt)
{
    g_urbc_runtime = rt;
}

void urbc_runtime_fail(UrbcRuntime *rt, const char *fmt, ...)
{
    va_list ap;
    if (!rt) return;
    rt->failed = 1;
    va_start(ap, fmt);
    vsnprintf(rt->last_error, sizeof(rt->last_error), fmt, ap);
    va_end(ap);
}

void urbc_image_init(UrbcImage *image)
{
    if (!image) return;
    memset(image, 0, sizeof(*image));
}

void urbc_image_destroy(UrbcImage *image)
{
    uint16_t i;
    if (!image) return;
    if (image->schema_cache) {
        for (i = 0; i < image->header.schema_count; i++) {
            UrbcCompiledSchema *schema = (UrbcCompiledSchema *)image->schema_cache[i];
            if (schema) free(schema->fields);
            free(schema);
        }
    }
    if (image->owned_heap) {
        while (image->owned_heap->size > 0) {
            Value v = urb_pop(image->owned_heap);
            free(v.p);
        }
        urb_free(image->owned_heap);
    }
    free(image->strings);
    free(image->signature_string_indices);
    free(image->schema_bytes);
    free(image->schema_offsets);
    free(image->schema_lengths);
    free(image->schema_cache);
    if (image->mem) urb_free(image->mem);
    memset(image, 0, sizeof(*image));
}

void urbc_runtime_init(UrbcRuntime *rt)
{
    if (!rt) return;
    memset(rt, 0, sizeof(*rt));
#ifdef _WIN32
    InitializeCriticalSection(&rt->lock);
    rt->lock_ready = 1;
#else
    if (pthread_mutex_init(&rt->lock, NULL) == 0)
        rt->lock_ready = 1;
#endif
}

void urbc_runtime_destroy(UrbcRuntime *rt)
{
    if (!rt) return;
    urbc_runtime_lock(rt);
    urbc_ffi_cleanup_runtime(rt);
    if (rt->owned_externals) {
        while (rt->owned_externals->size > 0) {
            UrbcTrackedExternal *ext = (UrbcTrackedExternal *)urb_pop(rt->owned_externals).p;
            if (ext) {
                if (ext->release) ext->release(ext->ptr, ext->opaque);
                free(ext);
            }
        }
        urb_free(rt->owned_externals);
    }
    if (rt && rt->host_bindings) {
        while (rt->host_bindings->size > 0) {
            UrbcHostBinding *b = (UrbcHostBinding *)urb_pop(rt->host_bindings).p;
            if (b) {
                free(b->name);
                free(b);
            }
        }
        urb_free(rt->host_bindings);
    }
    if (rt->owns_stack && rt->stack) urb_free(rt->stack);
    if (rt->owns_exec && rt->exec) urb_free(rt->exec);
#ifdef _WIN32
    urbc_runtime_unlock(rt);
    if (rt->lock_ready)
        DeleteCriticalSection(&rt->lock);
#else
    urbc_runtime_unlock(rt);
    if (rt->lock_ready)
        (void)pthread_mutex_destroy(&rt->lock);
#endif
    memset(rt, 0, sizeof(*rt));
}

const char *urbc_runtime_error(const UrbcRuntime *rt)
{
    return rt ? rt->last_error : "";
}

UrbcHostBinding *urbc_runtime_find_host(UrbcRuntime *rt, const char *name)
{
    UHalf i;
    if (!rt || !rt->host_bindings || !name) return NULL;
    urbc_runtime_lock(rt);
    for (i = 0; i < rt->host_bindings->size; i++) {
        UrbcHostBinding *b = (UrbcHostBinding *)rt->host_bindings->data[i].p;
        if (b && b->name && strcmp(b->name, name) == 0) {
            urbc_runtime_unlock(rt);
            return b;
        }
    }
    urbc_runtime_unlock(rt);
    return NULL;
}

UrbcStatus urbc_runtime_register_host(UrbcRuntime *rt,
                                      const char *name,
                                      UrbcHostFn fn,
                                      void *opaque,
                                      char *err, size_t err_cap)
{
    UrbcHostBinding *binding;
    size_t len;
    if (!rt || !name || !fn) {
        urbc_copy_error(err, err_cap, "invalid host binding arguments");
        return URBC_ERR_ARGUMENT;
    }
    urbc_runtime_lock(rt);
    if (!rt->host_bindings) {
        rt->host_bindings = urb_new(8);
        if (!rt->host_bindings) {
            urbc_runtime_unlock(rt);
            urbc_copy_error(err, err_cap, "out of memory");
            return URBC_ERR_ALLOC;
        }
    }
    binding = urbc_runtime_find_host(rt, name);
    if (binding) {
        binding->fn = fn;
        binding->opaque = opaque;
        urbc_runtime_unlock(rt);
        return URBC_OK;
    }
    binding = (UrbcHostBinding *)calloc(1, sizeof(*binding));
    if (!binding) {
        urbc_runtime_unlock(rt);
        urbc_copy_error(err, err_cap, "out of memory");
        return URBC_ERR_ALLOC;
    }
    len = strlen(name);
    binding->name = (char *)malloc(len + 1);
    if (!binding->name) {
        free(binding);
        urbc_runtime_unlock(rt);
        urbc_copy_error(err, err_cap, "out of memory");
        return URBC_ERR_ALLOC;
    }
    memcpy(binding->name, name, len + 1);
    binding->fn = fn;
    binding->opaque = opaque;
    urb_push(rt->host_bindings, (Value){ .p = binding });
    urbc_runtime_unlock(rt);
    return URBC_OK;
}

UrbcStatus urbc_runtime_track_external(UrbcRuntime *rt,
                                       void *ptr,
                                       UrbcExternalReleaseFn release,
                                       void *opaque,
                                       char *err, size_t err_cap)
{
    UrbcTrackedExternal *ext;
    if (!rt || !ptr || !release) {
        urbc_copy_error(err, err_cap, "invalid external tracking arguments");
        return URBC_ERR_ARGUMENT;
    }
    urbc_runtime_lock(rt);
    if (!rt->owned_externals) {
        rt->owned_externals = urb_new(8);
        if (!rt->owned_externals) {
            urbc_runtime_unlock(rt);
            urbc_copy_error(err, err_cap, "out of memory");
            return URBC_ERR_ALLOC;
        }
    }
    ext = (UrbcTrackedExternal *)calloc(1, sizeof(*ext));
    if (!ext) {
        urbc_runtime_unlock(rt);
        urbc_copy_error(err, err_cap, "out of memory");
        return URBC_ERR_ALLOC;
    }
    ext->ptr = ptr;
    ext->release = release;
    ext->opaque = opaque;
    urb_push(rt->owned_externals, (Value){ .p = ext });
    urbc_runtime_unlock(rt);
    return URBC_OK;
}

UrbcHostBinding *urbc_host_resolve(UrbcRuntime *rt, Value callable, const char *what)
{
    if (!rt) return NULL;
    if (!callable.p) {
        urbc_runtime_fail(rt, "%s: host callable is null", what);
        return NULL;
    }
    if (rt->host_bindings) {
        UrbcHostBinding *by_ptr = (UrbcHostBinding *)callable.p;
        UHalf i;
        for (i = 0; i < rt->host_bindings->size; i++) {
            if ((UrbcHostBinding *)rt->host_bindings->data[i].p == by_ptr) return by_ptr;
        }
    }
    return urbc_runtime_find_host(rt, (const char *)callable.p);
}

Value urbc_host_invoke(UrbcRuntime *rt, UrbcHostBinding *binding, int argc, const Value *argv)
{
    if (!binding || !binding->fn) {
        urbc_runtime_fail(rt, "host callable missing");
        return (Value){ .i = 0 };
    }
    return binding->fn(rt, argc, argv, binding->opaque);
}

void *urbc_runtime_alloc_owned(UrbcRuntime *rt, size_t size)
{
    void *p;
    if (!rt || !rt->image || size == 0) return NULL;
    p = calloc(1, size);
    if (!p) return NULL;
    if (!rt->image->owned_heap) {
        rt->image->owned_heap = urb_new(16);
        if (!rt->image->owned_heap) {
            free(p);
            return NULL;
        }
    }
    urb_push(rt->image->owned_heap, (Value){ .p = p });
    return p;
}

UrbcStatus urbc_runtime_bind(UrbcRuntime *rt, UrbcImage *image,
                             List *exec, List *stack,
                             char *err, size_t err_cap)
{
    if (!rt || !image) {
        urbc_copy_error(err, err_cap, "invalid arguments");
        return URBC_ERR_ARGUMENT;
    }
    rt->image = image;
    rt->exec = exec ? exec : urb_new(128);
    rt->stack = stack ? stack : urb_new(image->header.max_stack ? image->header.max_stack : 16);
    rt->owns_exec = exec ? 0 : 1;
    rt->owns_stack = stack ? 0 : 1;
    rt->failed = 0;
    rt->last_error[0] = '\0';

    if (!rt->exec || !rt->stack) {
        urbc_copy_error(err, err_cap, "out of memory");
        return URBC_ERR_ALLOC;
    }
    if (!exec) {
        UrbcStatus st = urbc_exec_init_default(rt->exec, err, err_cap);
        if (st != URBC_OK) return st;
    }
    return URBC_OK;
}

static int urbc_is_truthy(Value v)
{
    return v.i != 0;
}

UrbcStatus urbc_runtime_run(UrbcRuntime *rt, char *err, size_t err_cap)
{
    UInt i;
    List *mem;
    List *exec;
    List *stack;

    if (!rt || !rt->image || !rt->exec || !rt->stack) {
        urbc_copy_error(err, err_cap, "runtime not bound");
        return URBC_ERR_ARGUMENT;
    }

    mem = rt->image->mem;
    exec = rt->exec;
    stack = rt->stack;
    urbc_runtime_lock(rt);
    urbc_runtime_set_current(rt);
    rt->failed = 0;
    rt->last_error[0] = '\0';

    for (i = rt->image->entry_pc; i < (UInt)mem->size && !rt->failed; i++) {
        if (mem->data[i].i < INT_MIN + exec->size + OP_CODES_OFFSET) {
            switch (mem->data[i].i) {
            case ALIAS_GOTO:
                i = (UInt)urb_pop(stack).i - 1;
                break;
            case ALIAS_GOIF: {
                Value condv = urb_pop(stack);
                Int posit = urb_pop(stack).i - 1;
                i = urbc_is_truthy(condv) ? (UInt)posit : i;
                break;
            }
            case ALIAS_GOIE: {
                Value condv = urb_pop(stack);
                Int posit_true = urb_pop(stack).i - 1;
                Int posit_else = urb_pop(stack).i - 1;
                i = urbc_is_truthy(condv) ? (UInt)posit_true : (UInt)posit_else;
                break;
            }
            case ALIAS_EXEC:
                urb_push(stack, (Value){ .p = exec });
                break;
            case ALIAS_MEM:
                urb_push(stack, (Value){ .p = mem });
                break;
            default:
                exec->data[(UHalf)(mem->data[i].i - INT_MIN - OP_CODES_OFFSET)].fn(stack);
                break;
            }
        } else if (mem->data[i].i > INT_MAX - mem->size) {
            urb_push(stack, mem->data[INT_MAX - mem->data[i].i]);
        } else {
            urb_push(stack, mem->data[i]);
        }
    }

    urbc_runtime_set_current(NULL);
    if (rt->failed) {
        urbc_copy_error(err, err_cap, rt->last_error);
        urbc_runtime_unlock(rt);
        return URBC_ERR_RUNTIME;
    }
    urbc_runtime_unlock(rt);
    return URBC_OK;
}

UrbcStatus urbc_exec_init_default(List *exec, char *err, size_t err_cap)
{
    const UrbcExecReg regs[] = {
        { URBC_OP_STACK_DUP, urbc_op_stack_dup },
        { URBC_OP_STACK_POP, urbc_op_stack_pop },
        { URBC_OP_STACK_SWAP, urbc_op_stack_swap },
        { URBC_OP_PTR_ADD, urbc_op_ptr_add },
        { URBC_OP_PTR_SUB, urbc_op_ptr_sub },
        { URBC_OP_CMP_EQ, urbc_op_cmp_eq },
        { URBC_OP_CMP_NE, urbc_op_cmp_ne },
        { URBC_OP_CMP_LT_I64, urbc_op_cmp_lt_i64 },
        { URBC_OP_CMP_LE_I64, urbc_op_cmp_le_i64 },
        { URBC_OP_CMP_GT_I64, urbc_op_cmp_gt_i64 },
        { URBC_OP_CMP_GE_I64, urbc_op_cmp_ge_i64 },
        { URBC_OP_LOGIC_NOT, urbc_op_logic_not },

        { URBC_OP_MEM_ALLOC, urbc_op_mem_alloc },
        { URBC_OP_MEM_FREE, urbc_op_mem_free },
        { URBC_OP_MEM_REALLOC, urbc_op_mem_realloc },
        { URBC_OP_MEM_ZERO, urbc_op_mem_zero },
        { URBC_OP_MEM_COPY, urbc_op_mem_copy },
        { URBC_OP_MEM_SET, urbc_op_mem_set },
        { URBC_OP_MEM_COMPARE, urbc_op_mem_compare },
        { URBC_OP_MEM_NULLPTR, urbc_op_mem_nullptr },
        { URBC_OP_MEM_SIZEOF_PTR, urbc_op_mem_sizeof_ptr },
        { URBC_OP_MEM_READPTR, urbc_op_mem_readptr },
        { URBC_OP_MEM_WRITEPTR, urbc_op_mem_writeptr },
        { URBC_OP_MEM_READCSTRING, urbc_op_mem_readcstring },
        { URBC_OP_MEM_WRITECSTRING, urbc_op_mem_writecstring },
        { URBC_OP_MEM_READI8, urbc_op_mem_readi8 },
        { URBC_OP_MEM_READU8, urbc_op_mem_readu8 },
        { URBC_OP_MEM_READI16, urbc_op_mem_readi16 },
        { URBC_OP_MEM_READU16, urbc_op_mem_readu16 },
        { URBC_OP_MEM_READI32, urbc_op_mem_readi32 },
        { URBC_OP_MEM_READU32, urbc_op_mem_readu32 },
        { URBC_OP_MEM_READI64, urbc_op_mem_readi64 },
        { URBC_OP_MEM_READU64, urbc_op_mem_readu64 },
        { URBC_OP_MEM_READF32, urbc_op_mem_readf32 },
        { URBC_OP_MEM_READF64, urbc_op_mem_readf64 },
        { URBC_OP_MEM_WRITEI8, urbc_op_mem_writei8 },
        { URBC_OP_MEM_WRITEU8, urbc_op_mem_writeu8 },
        { URBC_OP_MEM_WRITEI16, urbc_op_mem_writei16 },
        { URBC_OP_MEM_WRITEU16, urbc_op_mem_writeu16 },
        { URBC_OP_MEM_WRITEI32, urbc_op_mem_writei32 },
        { URBC_OP_MEM_WRITEU32, urbc_op_mem_writeu32 },
        { URBC_OP_MEM_WRITEI64, urbc_op_mem_writei64 },
        { URBC_OP_MEM_WRITEU64, urbc_op_mem_writeu64 },
        { URBC_OP_MEM_WRITEF32, urbc_op_mem_writef32 },
        { URBC_OP_MEM_WRITEF64, urbc_op_mem_writef64 },

        { URBC_OP_SCHEMA_SIZEOF, urbc_op_schema_sizeof },
        { URBC_OP_SCHEMA_OFFSETOF, urbc_op_schema_offsetof },
        { URBC_OP_VIEW_MAKE, urbc_op_view_make },
        { URBC_OP_VIEW_ARRAY, urbc_op_view_array },
        { URBC_OP_VIEW_GET, urbc_op_view_get },
        { URBC_OP_VIEW_SET, urbc_op_view_set },
        { URBC_OP_UNION_MAKE, urbc_op_union_make },
        { URBC_OP_UNION_SIZEOF, urbc_op_union_sizeof },

        { URBC_OP_FFI_OPEN, urbc_op_ffi_open },
        { URBC_OP_FFI_CLOSE, urbc_op_ffi_close },
        { URBC_OP_FFI_SYM, urbc_op_ffi_sym },
        { URBC_OP_FFI_SYM_SELF, urbc_op_ffi_sym_self },
        { URBC_OP_FFI_BIND, urbc_op_ffi_bind },
        { URBC_OP_FFI_CALL0, urbc_op_ffi_call0 },
        { URBC_OP_FFI_CALL1, urbc_op_ffi_call1 },
        { URBC_OP_FFI_CALL2, urbc_op_ffi_call2 },
        { URBC_OP_FFI_CALL3, urbc_op_ffi_call3 },
        { URBC_OP_FFI_CALL4, urbc_op_ffi_call4 },
        { URBC_OP_FFI_CALLBACK, urbc_op_ffi_callback },
        { URBC_OP_FFI_ERRNO, urbc_op_ffi_errno },
        { URBC_OP_FFI_DLERROR, urbc_op_ffi_dlerror },
        { URBC_OP_FFI_CALLV, urbc_op_ffi_callv },

        { URBC_OP_HOST_CALL0, urbc_op_host_call0 },
        { URBC_OP_HOST_CALL1, urbc_op_host_call1 },
        { URBC_OP_HOST_CALL2, urbc_op_host_call2 },
        { URBC_OP_HOST_CALL3, urbc_op_host_call3 },
    };
    size_t i;
    if (!exec) {
        urbc_copy_error(err, err_cap, "exec is null");
        return URBC_ERR_ARGUMENT;
    }
    while (exec->size > 0) urb_pop(exec);
    for (i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
        while (exec->size < (UHalf)regs[i].id) urb_push(exec, (Value){ .fn = NULL });
        if (exec->size == (UHalf)regs[i].id) urb_push(exec, (Value){ .fn = regs[i].fn });
        else exec->data[(UHalf)regs[i].id].fn = regs[i].fn;
    }
    return URBC_OK;
}
