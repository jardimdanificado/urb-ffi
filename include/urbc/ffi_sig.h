#ifndef URBC_FFI_SIG_H
#define URBC_FFI_SIG_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FSIG_VOID = 0,
    FSIG_I8,
    FSIG_U8,
    FSIG_I16,
    FSIG_U16,
    FSIG_I32,
    FSIG_U32,
    FSIG_I64,
    FSIG_U64,
    FSIG_F32,
    FSIG_F64,
    FSIG_BOOL,
    FSIG_CSTRING,
    FSIG_POINTER,
} FsigBase;

typedef struct FsigType {
    FsigBase base;
    char tag[64];
    int is_const;
} FsigType;

#define FSIG_MAX_ARGS 32

typedef struct FsigParsed {
    char      name[128];
    FsigType  ret;
    FsigType  args[FSIG_MAX_ARGS];
    int       argc;
    int       has_varargs;
} FsigParsed;

int fsig_parse(const char *sig, FsigParsed *out, char *err, size_t err_cap);
const char *fsig_base_name(FsigBase base);

#ifdef __cplusplus
}
#endif

#endif
