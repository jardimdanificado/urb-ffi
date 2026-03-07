#ifndef URBC_RUNTIME_H
#define URBC_RUNTIME_H 1

#include "urbc/format.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct UrbcSchemaConst {
    uint32_t magic;
    uint16_t schema_index;
    uint16_t reserved;
} UrbcSchemaConst;

struct UrbcRuntime;
typedef Value (*UrbcHostFn)(struct UrbcRuntime *rt, int argc, const Value *argv, void *opaque);
typedef void (*UrbcExternalReleaseFn)(void *ptr, void *opaque);

typedef struct UrbcHostBinding {
    char *name;
    UrbcHostFn fn;
    void *opaque;
} UrbcHostBinding;

typedef struct UrbcTrackedExternal {
    void *ptr;
    UrbcExternalReleaseFn release;
    void *opaque;
} UrbcTrackedExternal;

typedef struct UrbcImage {
    UrbcHeader header;
    List *mem;
    List *owned_heap;
    char **strings;
    uint16_t *signature_string_indices;
    uint8_t *schema_bytes;
    size_t schema_size;
    uint32_t *schema_offsets;
    uint32_t *schema_lengths;
    void **schema_cache;
    UInt entry_pc;
} UrbcImage;

typedef struct UrbcRuntime {
    UrbcImage *image;
    List *exec;
    List *stack;
    List *host_bindings;
    List *owned_callbacks;
    List *owned_externals;
    int owns_exec;
    int owns_stack;
    int failed;
    char last_error[URBC_ERROR_CAP];
    void *host_opaque;
#ifdef _WIN32
    CRITICAL_SECTION lock;
    DWORD lock_owner;
    unsigned lock_depth;
    int lock_owned;
    int lock_ready;
#else
    pthread_mutex_t lock;
    pthread_t lock_owner;
    unsigned lock_depth;
    int lock_owned;
    int lock_ready;
#endif
} UrbcRuntime;

void urbc_image_init(UrbcImage *image);
void urbc_image_destroy(UrbcImage *image);

void urbc_runtime_init(UrbcRuntime *rt);
void urbc_runtime_destroy(UrbcRuntime *rt);
UrbcStatus urbc_runtime_bind(UrbcRuntime *rt, UrbcImage *image,
                             List *exec, List *stack,
                             char *err, size_t err_cap);
UrbcStatus urbc_runtime_run(UrbcRuntime *rt, char *err, size_t err_cap);
const char *urbc_runtime_error(const UrbcRuntime *rt);
UrbcStatus urbc_runtime_register_host(UrbcRuntime *rt,
                                      const char *name,
                                      UrbcHostFn fn,
                                      void *opaque,
                                      char *err, size_t err_cap);
UrbcHostBinding *urbc_runtime_find_host(UrbcRuntime *rt, const char *name);
UrbcStatus urbc_runtime_track_external(UrbcRuntime *rt,
                                       void *ptr,
                                       UrbcExternalReleaseFn release,
                                       void *opaque,
                                       char *err, size_t err_cap);

UrbcStatus urbc_exec_init_default(List *exec, char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif
