#ifndef URBC_FORMAT_H
#define URBC_FORMAT_H 1

#include "urbc/common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define URBC_MAGIC_0 'D'
#define URBC_MAGIC_1 'F'
#define URBC_MAGIC_2 'F'
#define URBC_MAGIC_3 'I'
#define URBC_PROFILE_URB64 1

typedef struct UrbcHeader {
    uint8_t  version_major;
    uint8_t  version_minor;
    uint8_t  profile;
    uint8_t  flags;
    uint16_t max_stack;
    uint16_t string_count;
    uint16_t signature_count;
    uint16_t schema_count;
    uint16_t const_count;
    uint16_t code_count;
    uint32_t string_bytes;
    uint32_t signature_bytes;
    uint32_t schema_bytes;
    uint32_t const_bytes;
    uint32_t code_bytes;
} UrbcHeader;

typedef enum UrbcConstKind {
    URBC_CONST_BOOL = 0,
    URBC_CONST_I64,
    URBC_CONST_U64,
    URBC_CONST_F64,
    URBC_CONST_STRING,
    URBC_CONST_SIG,
    URBC_CONST_SCHEMA,
    URBC_CONST_NULLPTR,
} UrbcConstKind;

typedef enum UrbcTokenKind {
    URBC_TOKEN_ALIAS = 0,
    URBC_TOKEN_OP,
    URBC_TOKEN_CONST_REF,
} UrbcTokenKind;

typedef enum UrbcAliasId {
    URBC_ALIAS_GOTO = 0,
    URBC_ALIAS_GOIF = 1,
    URBC_ALIAS_GOIE = 2,
    URBC_ALIAS_EXEC = 3,
    URBC_ALIAS_MEM  = 4,
} UrbcAliasId;

typedef enum UrbcSchemaKind {
    URBC_SCHEMA_STRUCT = 0,
    URBC_SCHEMA_UNION  = 1,
} UrbcSchemaKind;

typedef enum UrbcFieldKind {
    URBC_FIELD_PRIM = 0,
    URBC_FIELD_ARRAY,
    URBC_FIELD_STRUCT,
    URBC_FIELD_POINTER,
} UrbcFieldKind;

typedef enum UrbcPrimType {
    URBC_PRIM_INVALID = 0,
    URBC_PRIM_BOOL    = 1,
    URBC_PRIM_I8      = 2,
    URBC_PRIM_U8      = 3,
    URBC_PRIM_I16     = 4,
    URBC_PRIM_U16     = 5,
    URBC_PRIM_I32     = 6,
    URBC_PRIM_U32     = 7,
    URBC_PRIM_I64     = 8,
    URBC_PRIM_U64     = 9,
    URBC_PRIM_F32     = 10,
    URBC_PRIM_F64     = 11,
    URBC_PRIM_POINTER = 12,
    URBC_PRIM_CSTRING = 13,
} UrbcPrimType;

typedef enum UrbcOpId {
    URBC_OP_STACK_DUP = 0,
    URBC_OP_STACK_POP = 1,
    URBC_OP_STACK_SWAP = 2,
    URBC_OP_PTR_ADD = 3,
    URBC_OP_PTR_SUB = 4,
    URBC_OP_CMP_EQ = 5,
    URBC_OP_CMP_NE = 6,
    URBC_OP_CMP_LT_I64 = 7,
    URBC_OP_CMP_LE_I64 = 8,
    URBC_OP_CMP_GT_I64 = 9,
    URBC_OP_CMP_GE_I64 = 10,
    URBC_OP_LOGIC_NOT = 11,

    URBC_OP_MEM_ALLOC = 20,
    URBC_OP_MEM_FREE = 21,
    URBC_OP_MEM_REALLOC = 22,
    URBC_OP_MEM_ZERO = 23,
    URBC_OP_MEM_COPY = 24,
    URBC_OP_MEM_SET = 25,
    URBC_OP_MEM_COMPARE = 26,
    URBC_OP_MEM_NULLPTR = 27,
    URBC_OP_MEM_SIZEOF_PTR = 28,
    URBC_OP_MEM_READPTR = 29,
    URBC_OP_MEM_WRITEPTR = 30,
    URBC_OP_MEM_READCSTRING = 31,
    URBC_OP_MEM_WRITECSTRING = 32,
    URBC_OP_MEM_READI8 = 33,
    URBC_OP_MEM_READU8 = 34,
    URBC_OP_MEM_READI16 = 35,
    URBC_OP_MEM_READU16 = 36,
    URBC_OP_MEM_READI32 = 37,
    URBC_OP_MEM_READU32 = 38,
    URBC_OP_MEM_READI64 = 39,
    URBC_OP_MEM_READU64 = 40,
    URBC_OP_MEM_READF32 = 41,
    URBC_OP_MEM_READF64 = 42,
    URBC_OP_MEM_WRITEI8 = 43,
    URBC_OP_MEM_WRITEU8 = 44,
    URBC_OP_MEM_WRITEI16 = 45,
    URBC_OP_MEM_WRITEU16 = 46,
    URBC_OP_MEM_WRITEI32 = 47,
    URBC_OP_MEM_WRITEU32 = 48,
    URBC_OP_MEM_WRITEI64 = 49,
    URBC_OP_MEM_WRITEU64 = 50,
    URBC_OP_MEM_WRITEF32 = 51,
    URBC_OP_MEM_WRITEF64 = 52,

    URBC_OP_SCHEMA_SIZEOF = 60,
    URBC_OP_SCHEMA_OFFSETOF = 61,
    URBC_OP_VIEW_MAKE = 62,
    URBC_OP_VIEW_ARRAY = 63,
    URBC_OP_VIEW_GET = 64,
    URBC_OP_VIEW_SET = 65,
    URBC_OP_UNION_MAKE = 66,
    URBC_OP_UNION_SIZEOF = 67,

    URBC_OP_FFI_OPEN = 80,
    URBC_OP_FFI_CLOSE = 81,
    URBC_OP_FFI_SYM = 82,
    URBC_OP_FFI_SYM_SELF = 83,
    URBC_OP_FFI_BIND = 84,
    URBC_OP_FFI_CALL0 = 85,
    URBC_OP_FFI_CALL1 = 86,
    URBC_OP_FFI_CALL2 = 87,
    URBC_OP_FFI_CALL3 = 88,
    URBC_OP_FFI_CALL4 = 89,
    URBC_OP_FFI_CALLBACK = 90,
    URBC_OP_FFI_ERRNO = 91,
    URBC_OP_FFI_DLERROR = 92,
    URBC_OP_FFI_CALLV = 93,

    URBC_OP_HOST_CALL0 = 100,
    URBC_OP_HOST_CALL1 = 101,
    URBC_OP_HOST_CALL2 = 102,
    URBC_OP_HOST_CALL3 = 103,
} UrbcOpId;

UrbcStatus urbc_header_read(const void *data, size_t size, UrbcHeader *out,
                            char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif
