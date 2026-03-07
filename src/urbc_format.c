#include "urbc_internal.h"

const char *urbc_status_name(UrbcStatus status)
{
    switch (status) {
    case URBC_OK: return "ok";
    case URBC_ERR_ARGUMENT: return "argument";
    case URBC_ERR_FORMAT: return "format";
    case URBC_ERR_RANGE: return "range";
    case URBC_ERR_ALLOC: return "alloc";
    case URBC_ERR_UNIMPLEMENTED: return "unimplemented";
    case URBC_ERR_RUNTIME: return "runtime";
    default: return "unknown";
    }
}

void urbc_copy_error(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    snprintf(dst, cap, "%s", src);
}

uint16_t urbc_read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

uint32_t urbc_read_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

uint64_t urbc_read_u64(const uint8_t *p)
{
    return (uint64_t)p[0]
         | ((uint64_t)p[1] << 8)
         | ((uint64_t)p[2] << 16)
         | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32)
         | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48)
         | ((uint64_t)p[7] << 56);
}

int64_t urbc_read_i64(const uint8_t *p)
{
    return (int64_t)urbc_read_u64(p);
}

double urbc_read_f64(const uint8_t *p)
{
    union { uint64_t u; double d; } v;
    v.u = urbc_read_u64(p);
    return v.d;
}

UrbcStatus urbc_expect_space(size_t offset, size_t need, size_t size,
                             char *err, size_t err_cap)
{
    if (offset > size || need > size - offset) {
        urbc_copy_error(err, err_cap, "buffer too short");
        return URBC_ERR_FORMAT;
    }
    return URBC_OK;
}

char *urbc_dup_string_range(const uint8_t *src, size_t len,
                            char *err, size_t err_cap)
{
    char *s = (char *)malloc(len + 1);
    if (!s) {
        urbc_copy_error(err, err_cap, "out of memory");
        return NULL;
    }
    memcpy(s, src, len);
    s[len] = '\0';
    return s;
}

UrbcStatus urbc_header_read(const void *data, size_t size, UrbcHeader *out,
                            char *err, size_t err_cap)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!data || !out) {
        urbc_copy_error(err, err_cap, "invalid arguments");
        return URBC_ERR_ARGUMENT;
    }
    if (size < 40) {
        urbc_copy_error(err, err_cap, "header too short");
        return URBC_ERR_FORMAT;
    }
    if (p[0] != URBC_MAGIC_0 || p[1] != URBC_MAGIC_1 || p[2] != URBC_MAGIC_2 || p[3] != URBC_MAGIC_3) {
        urbc_copy_error(err, err_cap, "bad magic");
        return URBC_ERR_FORMAT;
    }
    out->version_major  = p[4];
    out->version_minor  = p[5];
    out->profile        = p[6];
    out->flags          = p[7];
    out->max_stack      = urbc_read_u16(p + 8);
    out->string_count   = urbc_read_u16(p + 10);
    out->signature_count= urbc_read_u16(p + 12);
    out->schema_count   = urbc_read_u16(p + 14);
    out->const_count    = urbc_read_u16(p + 16);
    out->code_count     = urbc_read_u16(p + 18);
    out->string_bytes   = urbc_read_u32(p + 20);
    out->signature_bytes= urbc_read_u32(p + 24);
    out->schema_bytes   = urbc_read_u32(p + 28);
    out->const_bytes    = urbc_read_u32(p + 32);
    out->code_bytes     = urbc_read_u32(p + 36);

    if (out->version_major != URBC_VERSION_MAJOR) {
        urbc_copy_error(err, err_cap, "unsupported major version");
        return URBC_ERR_FORMAT;
    }
    if (out->profile != URBC_PROFILE_URB64) {
        urbc_copy_error(err, err_cap, "unsupported profile");
        return URBC_ERR_FORMAT;
    }
    if (out->flags != 0) {
        urbc_copy_error(err, err_cap, "unsupported flags");
        return URBC_ERR_FORMAT;
    }
    return URBC_OK;
}
