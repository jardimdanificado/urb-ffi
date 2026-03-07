#include "urbc/ffi_sig.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void skip_ws(const char **p)
{
    while (**p && isspace((unsigned char)**p))
        (*p)++;
}

static size_t read_token(const char *p, char *out, size_t cap)
{
    size_t n = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (n + 1 < cap) out[n] = *p;
        n++;
        p++;
    }
    if (n < cap) out[n] = '\0';
    return n;
}

static const struct { const char *name; FsigBase base; } base_table[] = {
    { "void",    FSIG_VOID    },
    { "i8",      FSIG_I8      },
    { "int8",    FSIG_I8      },
    { "u8",      FSIG_U8      },
    { "uint8",   FSIG_U8      },
    { "byte",    FSIG_U8      },
    { "i16",     FSIG_I16     },
    { "int16",   FSIG_I16     },
    { "u16",     FSIG_U16     },
    { "uint16",  FSIG_U16     },
    { "i32",     FSIG_I32     },
    { "int32",   FSIG_I32     },
    { "int",     FSIG_I32     },
    { "u32",     FSIG_U32     },
    { "uint32",  FSIG_U32     },
    { "uint",    FSIG_U32     },
    { "i64",     FSIG_I64     },
    { "int64",   FSIG_I64     },
    { "long",    FSIG_I64     },
    { "u64",     FSIG_U64     },
    { "uint64",  FSIG_U64     },
    { "ulong",   FSIG_U64     },
    { "f32",     FSIG_F32     },
    { "float32", FSIG_F32     },
    { "float",   FSIG_F32     },
    { "f64",     FSIG_F64     },
    { "float64", FSIG_F64     },
    { "double",  FSIG_F64     },
    { "bool",    FSIG_BOOL    },
    { "_Bool",   FSIG_BOOL    },
    { "cstring", FSIG_CSTRING },
    { "string",  FSIG_CSTRING },
    { "pointer", FSIG_POINTER },
    { "ptr",     FSIG_POINTER },
    { NULL,      0 }
};

const char *fsig_base_name(FsigBase base)
{
    switch (base) {
    case FSIG_VOID: return "void";
    case FSIG_I8: return "i8";
    case FSIG_U8: return "u8";
    case FSIG_I16: return "i16";
    case FSIG_U16: return "u16";
    case FSIG_I32: return "i32";
    case FSIG_U32: return "u32";
    case FSIG_I64: return "i64";
    case FSIG_U64: return "u64";
    case FSIG_F32: return "f32";
    case FSIG_F64: return "f64";
    case FSIG_BOOL: return "bool";
    case FSIG_CSTRING: return "cstring";
    case FSIG_POINTER: return "pointer";
    default: return "unknown";
    }
}

static int parse_type(const char **pp, FsigType *out, char *err, size_t err_cap)
{
    const char *p = *pp;
    FsigBase base = (FsigBase)-1;
    char tok[64];
    size_t tlen;
    int i;
    memset(out, 0, sizeof(*out));
    skip_ws(&p);
    if (strncmp(p, "const", 5) == 0 && !isalnum((unsigned char)p[5]) && p[5] != '_') {
        out->is_const = 1;
        p += 5;
        skip_ws(&p);
    }
    tlen = read_token(p, tok, sizeof(tok));
    if (tlen == 0) {
        if (err) snprintf(err, err_cap, "tipo esperado em '%s'", p);
        return 0;
    }
    p += tlen;
    for (i = 0; base_table[i].name; i++) {
        if (strcmp(base_table[i].name, tok) == 0) {
            base = base_table[i].base;
            break;
        }
    }
    if ((int)base == -1) {
        if (err) snprintf(err, err_cap, "tipo desconhecido: '%s'", tok);
        return 0;
    }
    out->base = base;
    if (base == FSIG_POINTER) {
        skip_ws(&p);
        if (*p == '(') {
            p++;
            skip_ws(&p);
            tlen = read_token(p, out->tag, sizeof(out->tag));
            p += tlen;
            skip_ws(&p);
            if (*p != ')') {
                if (err) snprintf(err, err_cap, "')' esperado após pointer(tag)");
                return 0;
            }
            p++;
        }
    }
    skip_ws(&p);
    while (*p == '*') {
        out->base = FSIG_POINTER;
        p++;
        skip_ws(&p);
    }
    *pp = p;
    return 1;
}

int fsig_parse(const char *sig, FsigParsed *out, char *err, size_t err_cap)
{
    const char *p = sig;
    size_t nlen;
    if (!sig || !out) return 0;
    memset(out, 0, sizeof(*out));
    skip_ws(&p);
    if (!parse_type(&p, &out->ret, err, err_cap)) return 0;
    skip_ws(&p);
    nlen = read_token(p, out->name, sizeof(out->name));
    p += nlen;
    skip_ws(&p);
    if (*p != '(') return 1;
    p++;
    skip_ws(&p);
    if (*p == ')') return 1;
    while (*p) {
        FsigType arg;
        if (strncmp(p, "...", 3) == 0) {
            out->has_varargs = 1;
            p += 3;
            skip_ws(&p);
            if (*p == ')') break;
            if (err) snprintf(err, err_cap, "'...' deve ser o último argumento");
            return 0;
        }
        if (out->argc >= FSIG_MAX_ARGS) {
            if (err) snprintf(err, err_cap, "muitos argumentos");
            return 0;
        }
        if (!parse_type(&p, &arg, err, err_cap)) return 0;
        out->args[out->argc++] = arg;
        skip_ws(&p);
        if (*p && *p != ',' && *p != ')') {
            char dummy[64];
            size_t skip = read_token(p, dummy, sizeof(dummy));
            p += skip;
            skip_ws(&p);
        }
        if (*p == ',') { p++; skip_ws(&p); continue; }
        if (*p == ')') break;
        if (err) snprintf(err, err_cap, "',' ou ')' esperado, encontrou '%c'", *p);
        return 0;
    }
    return 1;
}
