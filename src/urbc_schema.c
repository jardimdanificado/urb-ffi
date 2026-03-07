#include "urbc_internal.h"

#define URBC_SCHEMA_IN_PROGRESS ((void *)(uintptr_t)1)

static size_t urbc_align_up(size_t n, size_t al)
{
    return (n + al - 1u) & ~(al - 1u);
}

static const uint8_t *urbc_schema_raw(UrbcImage *image, uint16_t schema_index)
{
    if (!image || !image->schema_bytes || schema_index >= image->header.schema_count) return NULL;
    return image->schema_bytes + image->schema_offsets[schema_index];
}

static UrbcStatus urbc_schema_validate_field(UrbcImage *image, const uint8_t *p,
                                             char *err, size_t err_cap)
{
    uint16_t name_idx = urbc_read_u16(p + 0);
    uint8_t field_kind = p[2];
    uint8_t prim_type = p[3];
    uint16_t aux = urbc_read_u16(p + 4);
    uint16_t ref_schema_index = urbc_read_u16(p + 6);
    uint16_t reserved = urbc_read_u16(p + 8);
    size_t sz, al;

    if (reserved != 0) {
        urbc_copy_error(err, err_cap, "schema field reserved must be zero");
        return URBC_ERR_FORMAT;
    }
    if (name_idx >= image->header.string_count) {
        urbc_copy_error(err, err_cap, "schema field name index out of range");
        return URBC_ERR_RANGE;
    }
    switch (field_kind) {
    case URBC_FIELD_PRIM:
        if (!urbc_prim_size_align(prim_type, &sz, &al)) {
            urbc_copy_error(err, err_cap, "invalid primitive field type");
            return URBC_ERR_FORMAT;
        }
        break;
    case URBC_FIELD_ARRAY:
        if (!urbc_prim_size_align(prim_type, &sz, &al)) {
            urbc_copy_error(err, err_cap, "invalid array primitive type");
            return URBC_ERR_FORMAT;
        }
        if (aux == 0) {
            urbc_copy_error(err, err_cap, "array field count must be > 0");
            return URBC_ERR_RANGE;
        }
        break;
    case URBC_FIELD_STRUCT:
        if (ref_schema_index >= image->header.schema_count) {
            urbc_copy_error(err, err_cap, "struct field schema index out of range");
            return URBC_ERR_RANGE;
        }
        break;
    case URBC_FIELD_POINTER:
        break;
    default:
        urbc_copy_error(err, err_cap, "invalid schema field kind");
        return URBC_ERR_FORMAT;
    }
    return URBC_OK;
}

UrbcStatus urbc_schema_build_cache(UrbcImage *image, char *err, size_t err_cap)
{
    uint16_t i;
    size_t off = 0;
    if (!image) {
        urbc_copy_error(err, err_cap, "invalid image");
        return URBC_ERR_ARGUMENT;
    }
    if (image->header.schema_count == 0) return URBC_OK;
    if (!image->schema_bytes) {
        urbc_copy_error(err, err_cap, "schema section missing");
        return URBC_ERR_FORMAT;
    }
    if (image->schema_offsets) return URBC_OK;

    image->schema_offsets = (uint32_t *)calloc(image->header.schema_count, sizeof(uint32_t));
    image->schema_lengths = (uint32_t *)calloc(image->header.schema_count, sizeof(uint32_t));
    image->schema_cache = (void **)calloc(image->header.schema_count, sizeof(void *));
    if (!image->schema_offsets || !image->schema_lengths || !image->schema_cache) {
        urbc_copy_error(err, err_cap, "out of memory");
        return URBC_ERR_ALLOC;
    }

    for (i = 0; i < image->header.schema_count; i++) {
        const uint8_t *base;
        uint8_t schema_kind;
        uint8_t reserved;
        uint16_t field_count;
        uint16_t k;
        size_t len;
        UrbcStatus st = urbc_expect_space(off, 4, image->schema_size, err, err_cap);
        if (st != URBC_OK) return st;
        image->schema_offsets[i] = (uint32_t)off;
        base = image->schema_bytes + off;
        schema_kind = base[0];
        reserved = base[1];
        field_count = urbc_read_u16(base + 2);
        if (schema_kind != URBC_SCHEMA_STRUCT && schema_kind != URBC_SCHEMA_UNION) {
            urbc_copy_error(err, err_cap, "invalid schema kind");
            return URBC_ERR_FORMAT;
        }
        if (reserved != 0) {
            urbc_copy_error(err, err_cap, "schema reserved must be zero");
            return URBC_ERR_FORMAT;
        }
        len = 4u + (size_t)field_count * 10u;
        st = urbc_expect_space(off, len, image->schema_size, err, err_cap);
        if (st != URBC_OK) return st;
        for (k = 0; k < field_count; k++) {
            st = urbc_schema_validate_field(image, base + 4u + (size_t)k * 10u, err, err_cap);
            if (st != URBC_OK) return st;
        }
        image->schema_lengths[i] = (uint32_t)len;
        off += len;
    }
    if (off != image->schema_size) {
        urbc_copy_error(err, err_cap, "schema section size mismatch");
        return URBC_ERR_FORMAT;
    }
    return URBC_OK;
}

int urbc_prim_size_align(uint8_t prim_type, size_t *sz, size_t *al)
{
    switch (prim_type) {
    case URBC_PRIM_BOOL:
    case URBC_PRIM_I8:
    case URBC_PRIM_U8:      *sz = 1; *al = 1; return 1;
    case URBC_PRIM_I16:
    case URBC_PRIM_U16:     *sz = 2; *al = 2; return 1;
    case URBC_PRIM_I32:
    case URBC_PRIM_U32:
    case URBC_PRIM_F32:     *sz = 4; *al = 4; return 1;
    case URBC_PRIM_I64:
    case URBC_PRIM_U64:
    case URBC_PRIM_F64:     *sz = 8; *al = 8; return 1;
    case URBC_PRIM_POINTER:
    case URBC_PRIM_CSTRING: *sz = sizeof(void *); *al = sizeof(void *); return 1;
    default: return 0;
    }
}

Value urbc_value_from_memory(uint8_t prim_type, const void *addr)
{
    Value out;
    memset(&out, 0, sizeof(out));
    if (!addr) return out;
    switch (prim_type) {
    case URBC_PRIM_BOOL: { uint8_t v; memcpy(&v, addr, 1); out.i = !!v; break; }
    case URBC_PRIM_I8:   { int8_t v; memcpy(&v, addr, 1); out.i = v; break; }
    case URBC_PRIM_U8:   { uint8_t v; memcpy(&v, addr, 1); out.u = v; break; }
    case URBC_PRIM_I16:  { int16_t v; memcpy(&v, addr, 2); out.i = v; break; }
    case URBC_PRIM_U16:  { uint16_t v; memcpy(&v, addr, 2); out.u = v; break; }
    case URBC_PRIM_I32:  { int32_t v; memcpy(&v, addr, 4); out.i = v; break; }
    case URBC_PRIM_U32:  { uint32_t v; memcpy(&v, addr, 4); out.u = v; break; }
    case URBC_PRIM_I64:  { int64_t v; memcpy(&v, addr, 8); out.i = (Int)v; break; }
    case URBC_PRIM_U64:  { uint64_t v; memcpy(&v, addr, 8); out.u = (UInt)v; break; }
    case URBC_PRIM_F32:  { float v; memcpy(&v, addr, 4); out.f = (Float)v; break; }
    case URBC_PRIM_F64:  { double v; memcpy(&v, addr, 8); out.f = (Float)v; break; }
    case URBC_PRIM_POINTER:
    case URBC_PRIM_CSTRING: {
        uintptr_t v = 0; memcpy(&v, addr, sizeof(v)); out.p = (void *)v; break;
    }
    default: break;
    }
    return out;
}

int urbc_value_to_memory(uint8_t prim_type, Value v, void *addr)
{
    if (!addr) return 0;
    switch (prim_type) {
    case URBC_PRIM_BOOL: { uint8_t x = (uint8_t)(v.i != 0); memcpy(addr, &x, 1); return 1; }
    case URBC_PRIM_I8:   { int8_t x = (int8_t)v.i; memcpy(addr, &x, 1); return 1; }
    case URBC_PRIM_U8:   { uint8_t x = (uint8_t)v.u; memcpy(addr, &x, 1); return 1; }
    case URBC_PRIM_I16:  { int16_t x = (int16_t)v.i; memcpy(addr, &x, 2); return 1; }
    case URBC_PRIM_U16:  { uint16_t x = (uint16_t)v.u; memcpy(addr, &x, 2); return 1; }
    case URBC_PRIM_I32:  { int32_t x = (int32_t)v.i; memcpy(addr, &x, 4); return 1; }
    case URBC_PRIM_U32:  { uint32_t x = (uint32_t)v.u; memcpy(addr, &x, 4); return 1; }
    case URBC_PRIM_I64:  { int64_t x = (int64_t)v.i; memcpy(addr, &x, 8); return 1; }
    case URBC_PRIM_U64:  { uint64_t x = (uint64_t)v.u; memcpy(addr, &x, 8); return 1; }
    case URBC_PRIM_F32:  { float x = (float)v.f; memcpy(addr, &x, 4); return 1; }
    case URBC_PRIM_F64:  { double x = (double)v.f; memcpy(addr, &x, 8); return 1; }
    case URBC_PRIM_POINTER:
    case URBC_PRIM_CSTRING: { uintptr_t x = (uintptr_t)v.p; memcpy(addr, &x, sizeof(x)); return 1; }
    default: return 0;
    }
}

UrbcCompiledSchema *urbc_schema_compile(UrbcRuntime *rt, uint16_t schema_index)
{
    UrbcImage *image;
    UrbcCompiledSchema *schema;
    const uint8_t *base;
    uint16_t field_count;
    uint16_t i;
    size_t offset = 0, max_align = 1, max_size = 0;

    if (!rt || !rt->image) return NULL;
    image = rt->image;
    if (schema_index >= image->header.schema_count) {
        urbc_runtime_fail(rt, "schema index out of range");
        return NULL;
    }
    if (image->schema_cache && image->schema_cache[schema_index] && image->schema_cache[schema_index] != URBC_SCHEMA_IN_PROGRESS)
        return (UrbcCompiledSchema *)image->schema_cache[schema_index];
    if (image->schema_cache && image->schema_cache[schema_index] == URBC_SCHEMA_IN_PROGRESS) {
        urbc_runtime_fail(rt, "recursive by-value schema not supported");
        return NULL;
    }
    base = urbc_schema_raw(image, schema_index);
    if (!base) {
        urbc_runtime_fail(rt, "schema bytes missing");
        return NULL;
    }
    field_count = urbc_read_u16(base + 2);
    schema = (UrbcCompiledSchema *)calloc(1, sizeof(*schema));
    if (!schema) {
        urbc_runtime_fail(rt, "out of memory compiling schema");
        return NULL;
    }
    schema->magic = URBC_COMPILED_SCHEMA_MAGIC;
    schema->schema_index = schema_index;
    schema->schema_kind = base[0];
    schema->field_count = field_count;
    schema->fields = field_count ? (UrbcFieldInfo *)calloc(field_count, sizeof(UrbcFieldInfo)) : NULL;
    if (field_count && !schema->fields) {
        free(schema);
        urbc_runtime_fail(rt, "out of memory allocating schema fields");
        return NULL;
    }
    image->schema_cache[schema_index] = URBC_SCHEMA_IN_PROGRESS;

    for (i = 0; i < field_count; i++) {
        const uint8_t *fp = base + 4u + (size_t)i * 10u;
        UrbcFieldInfo *fi = &schema->fields[i];
        fi->name_index = urbc_read_u16(fp + 0);
        fi->field_kind = fp[2];
        fi->prim_type = fp[3];
        fi->aux = urbc_read_u16(fp + 4);
        fi->ref_schema_index = urbc_read_u16(fp + 6);
        fi->name = image->strings[fi->name_index];

        switch (fi->field_kind) {
        case URBC_FIELD_PRIM:
            if (!urbc_prim_size_align(fi->prim_type, &fi->size, &fi->align)) goto fail;
            break;
        case URBC_FIELD_ARRAY: {
            size_t elem_sz, elem_al;
            if (!urbc_prim_size_align(fi->prim_type, &elem_sz, &elem_al)) goto fail;
            fi->size = elem_sz * fi->aux;
            fi->align = elem_al;
            break;
        }
        case URBC_FIELD_POINTER:
            fi->size = sizeof(void *);
            fi->align = sizeof(void *);
            fi->prim_type = URBC_PRIM_POINTER;
            break;
        case URBC_FIELD_STRUCT: {
            UrbcCompiledSchema *sub = urbc_schema_compile(rt, fi->ref_schema_index);
            if (!sub) goto fail;
            fi->size = sub->size;
            fi->align = sub->align;
            break;
        }
        default:
            goto fail;
        }

        if (fi->align > max_align) max_align = fi->align;
        if (schema->schema_kind == URBC_SCHEMA_UNION) {
            fi->offset = 0;
            if (fi->size > max_size) max_size = fi->size;
        } else {
            offset = urbc_align_up(offset, fi->align);
            fi->offset = offset;
            offset += fi->size;
        }
    }

    schema->align = max_align;
    schema->size = (schema->schema_kind == URBC_SCHEMA_UNION)
        ? urbc_align_up(max_size, max_align)
        : urbc_align_up(offset, max_align);
    image->schema_cache[schema_index] = schema;
    return schema;

fail:
    free(schema->fields);
    free(schema);
    image->schema_cache[schema_index] = NULL;
    urbc_runtime_fail(rt, "failed to compile schema");
    return NULL;
}

UrbcCompiledSchema *urbc_schema_from_const(UrbcRuntime *rt, Value schema_val)
{
    uint32_t magic;
    if (!schema_val.p) {
        urbc_runtime_fail(rt, "schema handle is null");
        return NULL;
    }
    magic = *(uint32_t *)schema_val.p;
    if (magic == URBC_SCHEMA_CONST_MAGIC) {
        UrbcSchemaConst *sc = (UrbcSchemaConst *)schema_val.p;
        if (sc->reserved != 0) {
            urbc_runtime_fail(rt, "invalid schema constant handle");
            return NULL;
        }
        return urbc_schema_compile(rt, sc->schema_index);
    }
    if (magic == URBC_COMPILED_SCHEMA_MAGIC) {
        return (UrbcCompiledSchema *)schema_val.p;
    }
    urbc_runtime_fail(rt, "value is not a schema handle");
    return NULL;
}

UrbcFieldInfo *urbc_schema_field(UrbcCompiledSchema *schema, const char *name)
{
    uint16_t i;
    if (!schema || !name) return NULL;
    for (i = 0; i < schema->field_count; i++)
        if (schema->fields[i].name && strcmp(schema->fields[i].name, name) == 0)
            return &schema->fields[i];
    return NULL;
}
