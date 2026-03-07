#ifndef URBC_LOADER_H
#define URBC_LOADER_H 1

#include "urbc/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

UrbcStatus urbc_load_from_memory(const void *data, size_t size,
                                 UrbcImage *out,
                                 char *err, size_t err_cap);
UrbcStatus urbc_load_from_file(const char *path,
                               UrbcImage *out,
                               char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif
