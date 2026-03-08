/*
 * ffi_sig.h — parser de assinaturas estilo urb-ffi
 *
 * Suporta o mesmo formato de string da FFI do urb-ffi:
 *   "i32 puts(cstring)"
 *   "void* malloc(u64)"
 *   "pointer(inner) make_outer()"
 *   "f64 lerp(f64, f64, f64)"
 *   "void callback(pointer, i32, ...)"
 *
 * É um módulo autônomo — não depende de nenhuma parte do runtime do urb-ffi.
 */

#ifndef FFI_SIG_H
#define FFI_SIG_H

#include <stddef.h>
#include <stdint.h>

/* ---- tipos base ---------------------------------------------------- */
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
    FSIG_BOOL,     /* _Bool: u8 com normalização !!val */
    FSIG_CSTRING,  /* const char * — alias de pointer tratado como string */
    FSIG_POINTER,  /* void * genérico ou pointer(tag) */
} FsigBase;

/* ---- representação de um tipo -------------------------------------- */
typedef struct FsigType {
    FsigBase base;
    char tag[64]; /* preenchido quando base == FSIG_POINTER e há "pointer(tag)" */
    int  is_const;
} FsigType;

/* ---- assinatura completa ------------------------------------------ */
#define FSIG_MAX_ARGS 32

typedef struct FsigParsed {
    char      name[128];          /* nome da função */
    FsigType  ret;
    FsigType  args[FSIG_MAX_ARGS];
    int       argc;
    int       has_varargs;        /* 1 se tem "..." */
} FsigParsed;

/*
 * fsig_parse — parseia uma string de assinatura no formato da FFI do urb-ffi.
 *
 * Retorna 1 em sucesso, 0 em erro.
 * Em erro, err (se não-NULL) recebe uma mensagem descritiva (max err_cap bytes).
 */
int fsig_parse(const char *sig, FsigParsed *out, char *err, size_t err_cap);

/* Nome legível do tipo base, para debug/erro */
const char *fsig_base_name(FsigBase base);

#endif /* FFI_SIG_H */
