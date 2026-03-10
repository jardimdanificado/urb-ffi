#ifndef URBC_INTERNAL_H
#define URBC_INTERNAL_H 1

#include "urbc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct UrbcConstWire {
    uint8_t kind;
    uint8_t reserved;
    uint16_t aux;
} UrbcConstWire;

typedef struct UrbcTokenWire {
    uint8_t kind;
    uint8_t reserved;
    uint16_t value;
} UrbcTokenWire;

#define URBC_SCHEMA_CONST_MAGIC    0x5542534bu
#define URBC_COMPILED_SCHEMA_MAGIC 0x55425343u
#define URBC_VIEW_MAGIC            0x55525657u
#define URBC_VARRAY_MAGIC          0x55525641u
#define URBC_BOUND_MAGIC           0x5542464eu
#define URBC_CALLBACK_MAGIC        0x55424342u

typedef struct UrbcFieldInfo {
    const char *name;
    uint16_t name_index;
    uint8_t field_kind;
    uint8_t prim_type;
    uint16_t aux;
    uint16_t ref_schema_index;
    size_t offset;
    size_t size;
    size_t align;
} UrbcFieldInfo;

typedef struct UrbcCompiledSchema {
    uint32_t magic;
    uint16_t schema_index;
    uint8_t schema_kind;
    uint8_t reserved;
    uint16_t field_count;
    size_t size;
    size_t align;
    UrbcFieldInfo *fields;
} UrbcCompiledSchema;

typedef struct UrbcViewHandle {
    uint32_t magic;
    uintptr_t base_ptr;
    UrbcCompiledSchema *schema;
} UrbcViewHandle;

typedef struct UrbcViewArrayHandle {
    uint32_t magic;
    uintptr_t base_ptr;
    uint32_t count;
    UrbcCompiledSchema *schema;
    size_t stride;
} UrbcViewArrayHandle;

typedef struct UrbcCallbackHandle {
    uint32_t magic;
    void *closure;
    void *fn_ptr;
    void *host_target;
} UrbcCallbackHandle;

#ifndef _WIN32
void urbc_runtime_lock(UrbcRuntime *rt);
void urbc_runtime_unlock(UrbcRuntime *rt);
#endif
void *urbc_dyn_open(const char *path, int flags);
int urbc_dyn_close(void *handle);
void *urbc_dyn_sym(void *handle, const char *name);
void *urbc_dyn_sym_self(const char *name);
const char *urbc_dyn_last_error(void);

UrbcRuntime *urbc_runtime_current(void);
void urbc_runtime_set_current(UrbcRuntime *rt);
void urbc_runtime_fail(UrbcRuntime *rt, const char *fmt, ...);
void urbc_copy_error(char *dst, size_t cap, const char *src);

uint16_t urbc_read_u16(const uint8_t *p);
uint32_t urbc_read_u32(const uint8_t *p);
uint64_t urbc_read_u64(const uint8_t *p);
int64_t urbc_read_i64(const uint8_t *p);
double urbc_read_f64(const uint8_t *p);

UrbcStatus urbc_expect_space(size_t offset, size_t need, size_t size,
                             char *err, size_t err_cap);
char *urbc_dup_string_range(const uint8_t *src, size_t len,
                            char *err, size_t err_cap);
void *urbc_runtime_alloc_owned(UrbcRuntime *rt, size_t size);

UrbcStatus urbc_schema_build_cache(UrbcImage *image, char *err, size_t err_cap);
UrbcCompiledSchema *urbc_schema_from_const(UrbcRuntime *rt, Value schema_val);
UrbcCompiledSchema *urbc_schema_compile(UrbcRuntime *rt, uint16_t schema_index);
UrbcFieldInfo *urbc_schema_field(UrbcCompiledSchema *schema, const char *name);
int urbc_prim_size_align(uint8_t prim_type, size_t *sz, size_t *al);
Value urbc_value_from_memory(uint8_t prim_type, const void *addr);
int urbc_value_to_memory(uint8_t prim_type, Value v, void *addr);
UrbcHostBinding *urbc_host_resolve(UrbcRuntime *rt, Value callable, const char *what);
Value urbc_host_invoke(UrbcRuntime *rt, UrbcHostBinding *binding, int argc, const Value *argv);
void urbc_ffi_cleanup_runtime(UrbcRuntime *rt);
UrbcStatus urbc_ffi_bind_handle(UrbcRuntime *rt, void *fn_ptr, const char *sig,
                                void **out_handle, char *err, size_t err_cap);
UrbcStatus urbc_ffi_bind_handle_desc(UrbcRuntime *rt, void *fn_ptr,
                                     const UrbcFfiDescriptor *desc,
                                     void **out_handle, char *err, size_t err_cap);
UrbcStatus urbc_ffi_call_bound(UrbcRuntime *rt, void *bound_handle, int argc,
                               const Value *argv, const uint8_t *vararg_types,
                               Value *out, char *err, size_t err_cap);
UrbcStatus urbc_ffi_make_callback(UrbcRuntime *rt, const char *sig, Value callable,
                                  void **out_fn_ptr, char *err, size_t err_cap);
UrbcStatus urbc_ffi_make_callback_desc(UrbcRuntime *rt,
                                       const UrbcFfiDescriptor *desc,
                                       Value callable,
                                       void **out_fn_ptr,
                                       char *err, size_t err_cap);
UrbcStatus urbc_view_make_handle(UrbcRuntime *rt, Value schema_value, uintptr_t base_ptr,
                                 Value *out_view, char *err, size_t err_cap);
UrbcStatus urbc_view_array_handle(UrbcRuntime *rt, Value schema_value, uintptr_t base_ptr,
                                  uint32_t count, Value *out_array,
                                  char *err, size_t err_cap);
UrbcStatus urbc_view_get_value(UrbcRuntime *rt, Value handle, const char *name,
                               Value *out_value, char *err, size_t err_cap);
UrbcStatus urbc_view_set_value(UrbcRuntime *rt, Value handle, const char *name,
                               Value value, char *err, size_t err_cap);

void urbc_op_stack_dup(List *stack);
void urbc_op_stack_pop(List *stack);
void urbc_op_stack_swap(List *stack);
void urbc_op_ptr_add(List *stack);
void urbc_op_ptr_sub(List *stack);
void urbc_op_cmp_eq(List *stack);
void urbc_op_cmp_ne(List *stack);
void urbc_op_cmp_lt_i64(List *stack);
void urbc_op_cmp_le_i64(List *stack);
void urbc_op_cmp_gt_i64(List *stack);
void urbc_op_cmp_ge_i64(List *stack);
void urbc_op_logic_not(List *stack);

void urbc_op_mem_alloc(List *stack);
void urbc_op_mem_free(List *stack);
void urbc_op_mem_realloc(List *stack);
void urbc_op_mem_zero(List *stack);
void urbc_op_mem_copy(List *stack);
void urbc_op_mem_set(List *stack);
void urbc_op_mem_compare(List *stack);
void urbc_op_mem_nullptr(List *stack);
void urbc_op_mem_sizeof_ptr(List *stack);
void urbc_op_mem_readptr(List *stack);
void urbc_op_mem_writeptr(List *stack);
void urbc_op_mem_readcstring(List *stack);
void urbc_op_mem_writecstring(List *stack);
void urbc_op_mem_readi8(List *stack);
void urbc_op_mem_readu8(List *stack);
void urbc_op_mem_readi16(List *stack);
void urbc_op_mem_readu16(List *stack);
void urbc_op_mem_readi32(List *stack);
void urbc_op_mem_readu32(List *stack);
void urbc_op_mem_readi64(List *stack);
void urbc_op_mem_readu64(List *stack);
void urbc_op_mem_readf32(List *stack);
void urbc_op_mem_readf64(List *stack);
void urbc_op_mem_writei8(List *stack);
void urbc_op_mem_writeu8(List *stack);
void urbc_op_mem_writei16(List *stack);
void urbc_op_mem_writeu16(List *stack);
void urbc_op_mem_writei32(List *stack);
void urbc_op_mem_writeu32(List *stack);
void urbc_op_mem_writei64(List *stack);
void urbc_op_mem_writeu64(List *stack);
void urbc_op_mem_writef32(List *stack);
void urbc_op_mem_writef64(List *stack);

void urbc_op_schema_sizeof(List *stack);
void urbc_op_schema_offsetof(List *stack);
void urbc_op_view_make(List *stack);
void urbc_op_view_array(List *stack);
void urbc_op_view_get(List *stack);
void urbc_op_view_set(List *stack);
void urbc_op_union_make(List *stack);
void urbc_op_union_sizeof(List *stack);

void urbc_op_ffi_open(List *stack);
void urbc_op_ffi_close(List *stack);
void urbc_op_ffi_sym(List *stack);
void urbc_op_ffi_sym_self(List *stack);
void urbc_op_ffi_bind(List *stack);
void urbc_op_ffi_call0(List *stack);
void urbc_op_ffi_call1(List *stack);
void urbc_op_ffi_call2(List *stack);
void urbc_op_ffi_call3(List *stack);
void urbc_op_ffi_call4(List *stack);
void urbc_op_ffi_callback(List *stack);
void urbc_op_ffi_errno(List *stack);
void urbc_op_ffi_dlerror(List *stack);
void urbc_op_ffi_callv(List *stack);

void urbc_op_host_call0(List *stack);
void urbc_op_host_call1(List *stack);
void urbc_op_host_call2(List *stack);
void urbc_op_host_call3(List *stack);

#endif
