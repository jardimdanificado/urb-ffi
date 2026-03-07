#include "urbc_internal.h"

typedef struct UrbcSectionCursor {
    const uint8_t *base;
    size_t size;
    size_t off;
} UrbcSectionCursor;

static UrbcStatus urbc_cursor_take(UrbcSectionCursor *c, size_t n,
                                   const uint8_t **out,
                                   char *err, size_t err_cap)
{
    UrbcStatus st = urbc_expect_space(c->off, n, c->size, err, err_cap);
    if (st != URBC_OK) return st;
    *out = c->base + c->off;
    c->off += n;
    return URBC_OK;
}

static UrbcStatus urbc_load_strings(UrbcImage *out,
                                    const uint8_t *buf, size_t len,
                                    char *err, size_t err_cap)
{
    UrbcSectionCursor c = { buf, len, 0 };
    uint16_t i;
    out->strings = (char **)calloc(out->header.string_count, sizeof(char *));
    if (!out->strings) return URBC_ERR_ALLOC;
    for (i = 0; i < out->header.string_count; i++) {
        const uint8_t *p;
        uint16_t slen;
        UrbcStatus st = urbc_cursor_take(&c, 2, &p, err, err_cap);
        if (st != URBC_OK) return st;
        slen = urbc_read_u16(p);
        st = urbc_cursor_take(&c, slen, &p, err, err_cap);
        if (st != URBC_OK) return st;
        out->strings[i] = urbc_dup_string_range(p, slen, err, err_cap);
        if (!out->strings[i]) return URBC_ERR_ALLOC;
        if (!out->owned_heap) out->owned_heap = urb_new(out->header.string_count + out->header.const_count + 8);
        if (!out->owned_heap) return URBC_ERR_ALLOC;
        urb_push(out->owned_heap, (Value){ .p = out->strings[i] });
    }
    if (c.off != len) {
        urbc_copy_error(err, err_cap, "string section size mismatch");
        return URBC_ERR_FORMAT;
    }
    return URBC_OK;
}

static UrbcStatus urbc_load_signatures(UrbcImage *out,
                                       const uint8_t *buf, size_t len,
                                       char *err, size_t err_cap)
{
    UrbcSectionCursor c = { buf, len, 0 };
    uint16_t i;
    out->signature_string_indices = (uint16_t *)calloc(out->header.signature_count, sizeof(uint16_t));
    if (!out->signature_string_indices) return URBC_ERR_ALLOC;
    for (i = 0; i < out->header.signature_count; i++) {
        const uint8_t *p;
        UrbcStatus st = urbc_cursor_take(&c, 2, &p, err, err_cap);
        if (st != URBC_OK) return st;
        out->signature_string_indices[i] = urbc_read_u16(p);
        if (out->signature_string_indices[i] >= out->header.string_count) {
            urbc_copy_error(err, err_cap, "signature string index out of range");
            return URBC_ERR_RANGE;
        }
    }
    if (c.off != len) {
        urbc_copy_error(err, err_cap, "signature section size mismatch");
        return URBC_ERR_FORMAT;
    }
    return URBC_OK;
}

static UrbcStatus urbc_load_schemas(UrbcImage *out,
                                    const uint8_t *buf, size_t len,
                                    char *err, size_t err_cap)
{
    (void)err; (void)err_cap;
    if (len == 0) return URBC_OK;
    out->schema_bytes = (uint8_t *)malloc(len);
    if (!out->schema_bytes) return URBC_ERR_ALLOC;
    memcpy(out->schema_bytes, buf, len);
    out->schema_size = len;
    return URBC_OK;
}

static UrbcStatus urbc_push_owned(UrbcImage *out, void *p)
{
    if (!out->owned_heap) {
        out->owned_heap = urb_new(out->header.const_count + out->header.string_count + 8);
        if (!out->owned_heap) return URBC_ERR_ALLOC;
    }
    urb_push(out->owned_heap, (Value){ .p = p });
    return URBC_OK;
}

static UrbcStatus urbc_load_constants(UrbcImage *out,
                                      const uint8_t *buf, size_t len,
                                      char *err, size_t err_cap)
{
    UrbcSectionCursor c = { buf, len, 0 };
    uint16_t i;
    if (!out->mem) {
        out->mem = urb_new(out->header.const_count + out->header.code_count + 8);
        if (!out->mem) return URBC_ERR_ALLOC;
    }
    for (i = 0; i < out->header.const_count; i++) {
        const uint8_t *p;
        UrbcConstWire w;
        Value v;
        UrbcStatus st = urbc_cursor_take(&c, 4, &p, err, err_cap);
        if (st != URBC_OK) return st;
        w.kind = p[0]; w.reserved = p[1]; w.aux = urbc_read_u16(p + 2);
        if (w.reserved != 0) {
            urbc_copy_error(err, err_cap, "constant reserved field must be zero");
            return URBC_ERR_FORMAT;
        }
        memset(&v, 0, sizeof(v));
        switch (w.kind) {
        case URBC_CONST_BOOL:
            st = urbc_cursor_take(&c, 1, &p, err, err_cap);
            if (st != URBC_OK) return st;
            v.i = p[0] ? 1 : 0;
            break;
        case URBC_CONST_I64:
            st = urbc_cursor_take(&c, 8, &p, err, err_cap);
            if (st != URBC_OK) return st;
            v.i = (Int)urbc_read_i64(p);
            break;
        case URBC_CONST_U64:
            st = urbc_cursor_take(&c, 8, &p, err, err_cap);
            if (st != URBC_OK) return st;
            v.u = (UInt)urbc_read_u64(p);
            break;
        case URBC_CONST_F64:
            st = urbc_cursor_take(&c, 8, &p, err, err_cap);
            if (st != URBC_OK) return st;
            v.f = (Float)urbc_read_f64(p);
            break;
        case URBC_CONST_STRING: {
            char *s;
            if (w.aux >= out->header.string_count) {
                urbc_copy_error(err, err_cap, "string const index out of range");
                return URBC_ERR_RANGE;
            }
            s = out->strings[w.aux];
            v.p = s;
            break;
        }
        case URBC_CONST_SIG: {
            uint16_t si;
            if (w.aux >= out->header.signature_count) {
                urbc_copy_error(err, err_cap, "signature const index out of range");
                return URBC_ERR_RANGE;
            }
            si = out->signature_string_indices[w.aux];
            v.p = out->strings[si];
            break;
        }
        case URBC_CONST_SCHEMA: {
            UrbcSchemaConst *sc = (UrbcSchemaConst *)malloc(sizeof(*sc));
            if (!sc) return URBC_ERR_ALLOC;
            sc->magic = URBC_SCHEMA_CONST_MAGIC;
            sc->schema_index = w.aux;
            sc->reserved = 0;
            st = urbc_push_owned(out, sc);
            if (st != URBC_OK) { free(sc); return st; }
            v.p = sc;
            break;
        }
        case URBC_CONST_NULLPTR:
            v.p = NULL;
            break;
        default:
            urbc_copy_error(err, err_cap, "unsupported constant kind");
            return URBC_ERR_UNIMPLEMENTED;
        }
        urb_push(out->mem, v);
    }
    if (c.off != len) {
        urbc_copy_error(err, err_cap, "constant section size mismatch");
        return URBC_ERR_FORMAT;
    }
    return URBC_OK;
}

static UrbcStatus urbc_load_code(UrbcImage *out,
                                 const uint8_t *buf, size_t len,
                                 char *err, size_t err_cap)
{
    UrbcSectionCursor c = { buf, len, 0 };
    uint16_t i;
    for (i = 0; i < out->header.code_count; i++) {
        const uint8_t *p;
        UrbcTokenWire w;
        Value v;
        UrbcStatus st = urbc_cursor_take(&c, 4, &p, err, err_cap);
        if (st != URBC_OK) return st;
        w.kind = p[0]; w.reserved = p[1]; w.value = urbc_read_u16(p + 2);
        if (w.reserved != 0) {
            urbc_copy_error(err, err_cap, "token reserved field must be zero");
            return URBC_ERR_FORMAT;
        }
        memset(&v, 0, sizeof(v));
        switch (w.kind) {
        case URBC_TOKEN_ALIAS:
            if (w.value > URBC_ALIAS_MEM) {
                urbc_copy_error(err, err_cap, "alias id out of range");
                return URBC_ERR_RANGE;
            }
            v.i = INT_MIN + (Int)w.value;
            break;
        case URBC_TOKEN_OP:
            v.i = INT_MIN + OP_CODES_OFFSET + (Int)w.value;
            break;
        case URBC_TOKEN_CONST_REF:
            if (w.value >= out->header.const_count) {
                urbc_copy_error(err, err_cap, "const ref out of range");
                return URBC_ERR_RANGE;
            }
            v.i = INT_MAX - (Int)w.value;
            break;
        default:
            urbc_copy_error(err, err_cap, "unsupported token kind");
            return URBC_ERR_FORMAT;
        }
        urb_push(out->mem, v);
    }
    if (c.off != len) {
        urbc_copy_error(err, err_cap, "code section size mismatch");
        return URBC_ERR_FORMAT;
    }
    return URBC_OK;
}

UrbcStatus urbc_load_from_memory(const void *data, size_t size,
                                 UrbcImage *out,
                                 char *err, size_t err_cap)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 40;
    UrbcStatus st;
    if (!data || !out) {
        urbc_copy_error(err, err_cap, "invalid arguments");
        return URBC_ERR_ARGUMENT;
    }
    urbc_image_init(out);
    st = urbc_header_read(data, size, &out->header, err, err_cap);
    if (st != URBC_OK) return st;

    st = urbc_expect_space(off,
                           out->header.string_bytes + out->header.signature_bytes +
                           out->header.schema_bytes + out->header.const_bytes + out->header.code_bytes,
                           size, err, err_cap);
    if (st != URBC_OK) return st;

    st = urbc_load_strings(out, p + off, out->header.string_bytes, err, err_cap);
    if (st != URBC_OK) goto fail;
    off += out->header.string_bytes;

    st = urbc_load_signatures(out, p + off, out->header.signature_bytes, err, err_cap);
    if (st != URBC_OK) goto fail;
    off += out->header.signature_bytes;

    st = urbc_load_schemas(out, p + off, out->header.schema_bytes, err, err_cap);
    if (st != URBC_OK) goto fail;
    off += out->header.schema_bytes;

    st = urbc_schema_build_cache(out, err, err_cap);
    if (st != URBC_OK) goto fail;

    st = urbc_load_constants(out, p + off, out->header.const_bytes, err, err_cap);
    if (st != URBC_OK) goto fail;
    off += out->header.const_bytes;

    st = urbc_load_code(out, p + off, out->header.code_bytes, err, err_cap);
    if (st != URBC_OK) goto fail;
    out->entry_pc = out->header.const_count;
    return URBC_OK;

fail:
    urbc_image_destroy(out);
    return st;
}

UrbcStatus urbc_load_from_file(const char *path,
                               UrbcImage *out,
                               char *err, size_t err_cap)
{
    FILE *fp;
    long n;
    uint8_t *buf;
    size_t got;
    UrbcStatus st;

    if (!path || !out) {
        urbc_copy_error(err, err_cap, "invalid arguments");
        return URBC_ERR_ARGUMENT;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        urbc_copy_error(err, err_cap, "cannot open file");
        return URBC_ERR_RUNTIME;
    }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); urbc_copy_error(err, err_cap, "fseek failed"); return URBC_ERR_RUNTIME; }
    n = ftell(fp);
    if (n < 0) { fclose(fp); urbc_copy_error(err, err_cap, "ftell failed"); return URBC_ERR_RUNTIME; }
    rewind(fp);
    buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(fp); urbc_copy_error(err, err_cap, "out of memory"); return URBC_ERR_ALLOC; }
    got = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    if (got != (size_t)n) {
        free(buf);
        urbc_copy_error(err, err_cap, "short read");
        return URBC_ERR_RUNTIME;
    }
    st = urbc_load_from_memory(buf, (size_t)n, out, err, err_cap);
    free(buf);
    return st;
}
