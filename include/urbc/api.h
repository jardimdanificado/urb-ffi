#ifndef URBC_API_H
#define URBC_API_H 1

#include "urbc/ffi_sig.h"
#include "urbc/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum UrbcDlOpenFlags {
    URBC_DLOPEN_LAZY     = 1 << 0,
    URBC_DLOPEN_NOW      = 1 << 1,
    URBC_DLOPEN_LOCAL    = 1 << 2,
    URBC_DLOPEN_GLOBAL   = 1 << 3,
    URBC_DLOPEN_NODELETE = 1 << 4,
    URBC_DLOPEN_NOLOAD   = 1 << 5,
} UrbcDlOpenFlags;

typedef struct UrbcFfiDescriptor UrbcFfiDescriptor;

UrbcStatus urbc_host_call(UrbcRuntime *rt,
                          const char *name,
                          int argc,
                          const Value *argv,
                          Value *out,
                          char *err, size_t err_cap);

UrbcStatus urbc_ffi_open(const char *path,
                         int flags,
                         void **out_handle,
                         char *err, size_t err_cap);
void urbc_ffi_close(void *handle);
UrbcStatus urbc_ffi_sym(void *handle,
                        const char *name,
                        void **out_symbol,
                        char *err, size_t err_cap);
UrbcStatus urbc_ffi_sym_self(const char *name,
                             void **out_symbol,
                             char *err, size_t err_cap);
UrbcStatus urbc_ffi_describe(const char *sig,
                             UrbcFfiDescriptor **out_desc,
                             char *err, size_t err_cap);
void urbc_ffi_descriptor_release(UrbcFfiDescriptor *desc);
UrbcStatus urbc_ffi_descriptor_copy_parsed(const UrbcFfiDescriptor *desc,
                                           FsigParsed *out_sig,
                                           char *err, size_t err_cap);
UrbcStatus urbc_ffi_bind(UrbcRuntime *rt,
                         void *fn_ptr,
                         const char *sig,
                         void **out_bound,
                         char *err, size_t err_cap);
UrbcStatus urbc_ffi_bind_desc(UrbcRuntime *rt,
                              void *fn_ptr,
                              const UrbcFfiDescriptor *desc,
                              void **out_bound,
                              char *err, size_t err_cap);
UrbcStatus urbc_ffi_call(UrbcRuntime *rt,
                         void *bound_handle,
                         int argc,
                         const Value *argv,
                         const uint8_t *vararg_types,
                         Value *out,
                         char *err, size_t err_cap);
UrbcStatus urbc_ffi_callback(UrbcRuntime *rt,
                             const char *sig,
                             Value callable,
                             void **out_fn_ptr,
                             char *err, size_t err_cap);
UrbcStatus urbc_ffi_callback_desc(UrbcRuntime *rt,
                                  const UrbcFfiDescriptor *desc,
                                  Value callable,
                                  void **out_fn_ptr,
                                  char *err, size_t err_cap);
int urbc_ffi_errno_value(void);
UrbcStatus urbc_ffi_dlerror_copy(UrbcRuntime *rt,
                                 char **out_message,
                                 char *err, size_t err_cap);

UrbcStatus urbc_schema_compile_value(UrbcRuntime *rt,
                                     Value schema_value,
                                     Value *out_schema,
                                     char *err, size_t err_cap);
UrbcStatus urbc_schema_sizeof(UrbcRuntime *rt,
                              Value schema_value,
                              size_t *out_size,
                              char *err, size_t err_cap);
UrbcStatus urbc_schema_offsetof(UrbcRuntime *rt,
                                Value schema_value,
                                const char *field_name,
                                size_t *out_offset,
                                char *err, size_t err_cap);
UrbcStatus urbc_view_make(UrbcRuntime *rt,
                          Value schema_value,
                          void *ptr,
                          Value *out_view,
                          char *err, size_t err_cap);
UrbcStatus urbc_view_array(UrbcRuntime *rt,
                           Value schema_value,
                           void *ptr,
                           uint32_t count,
                           Value *out_array,
                           char *err, size_t err_cap);
UrbcStatus urbc_view_get(UrbcRuntime *rt,
                         Value handle,
                         const char *name,
                         Value *out_value,
                         char *err, size_t err_cap);
UrbcStatus urbc_view_set(UrbcRuntime *rt,
                         Value handle,
                         const char *name,
                         Value value,
                         char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif
