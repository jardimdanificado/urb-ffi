#include "urbc_internal.h"

static int urbc_mem_fail_if_short(List *stack, Int n, const char *name)
{
    UrbcRuntime *rt = urbc_runtime_current();
    if (!stack || stack->size < n) {
        urbc_runtime_fail(rt, "%s: stack underflow", name);
        return 1;
    }
    return 0;
}

#define URBC_MEM_READ_FN(name, ctype, field)                \
void urbc_op_mem_##name(List *stack)                        \
{                                                           \
    Value p, out; ctype v = 0;                              \
    if (urbc_mem_fail_if_short(stack, 1, "mem." #name)) return; \
    p = urb_pop(stack);                                     \
    if (p.p) memcpy(&v, p.p, sizeof(v));                    \
    memset(&out, 0, sizeof(out));                           \
    out.field = v;                                          \
    urb_push(stack, out);                                   \
}

#define URBC_MEM_WRITE_FN(name, ctype, field)               \
void urbc_op_mem_##name(List *stack)                        \
{                                                           \
    Value p, v; ctype tmp;                                  \
    if (urbc_mem_fail_if_short(stack, 2, "mem." #name)) return; \
    v = urb_pop(stack);                                     \
    p = urb_pop(stack);                                     \
    tmp = (ctype)v.field;                                   \
    if (p.p) memcpy(p.p, &tmp, sizeof(tmp));                \
}

void urbc_op_mem_alloc(List *stack)
{
    Value n;
    if (urbc_mem_fail_if_short(stack, 1, "mem.alloc")) return;
    n = urb_pop(stack);
    urb_push(stack, (Value){ .p = malloc((size_t)n.u) });
}

void urbc_op_mem_free(List *stack)
{
    Value p;
    if (urbc_mem_fail_if_short(stack, 1, "mem.free")) return;
    p = urb_pop(stack);
    free(p.p);
}

void urbc_op_mem_realloc(List *stack)
{
    Value n, p;
    if (urbc_mem_fail_if_short(stack, 2, "mem.realloc")) return;
    n = urb_pop(stack);
    p = urb_pop(stack);
    urb_push(stack, (Value){ .p = realloc(p.p, (size_t)n.u) });
}

void urbc_op_mem_zero(List *stack)
{
    Value n, p;
    if (urbc_mem_fail_if_short(stack, 2, "mem.zero")) return;
    n = urb_pop(stack);
    p = urb_pop(stack);
    if (p.p && n.u) memset(p.p, 0, (size_t)n.u);
}

void urbc_op_mem_copy(List *stack)
{
    Value n, src, dst;
    if (urbc_mem_fail_if_short(stack, 3, "mem.copy")) return;
    n = urb_pop(stack);
    src = urb_pop(stack);
    dst = urb_pop(stack);
    if (dst.p && src.p && n.u) memcpy(dst.p, src.p, (size_t)n.u);
}

void urbc_op_mem_set(List *stack)
{
    Value n, bytev, p;
    if (urbc_mem_fail_if_short(stack, 3, "mem.set")) return;
    n = urb_pop(stack);
    bytev = urb_pop(stack);
    p = urb_pop(stack);
    if (p.p && n.u) memset(p.p, bytev.i & 0xFF, (size_t)n.u);
}

void urbc_op_mem_compare(List *stack)
{
    Value n, b, a;
    int r = 0;
    if (urbc_mem_fail_if_short(stack, 3, "mem.compare")) return;
    n = urb_pop(stack);
    b = urb_pop(stack);
    a = urb_pop(stack);
    if (a.p && b.p && n.u) r = memcmp(a.p, b.p, (size_t)n.u);
    urb_push(stack, (Value){ .i = r });
}

void urbc_op_mem_nullptr(List *stack)
{
    urb_push(stack, (Value){ .p = NULL });
}

void urbc_op_mem_sizeof_ptr(List *stack)
{
    urb_push(stack, (Value){ .u = sizeof(void *) });
}

void urbc_op_mem_readptr(List *stack)
{
    Value p; uintptr_t v = 0;
    if (urbc_mem_fail_if_short(stack, 1, "mem.readptr")) return;
    p = urb_pop(stack);
    if (p.p) memcpy(&v, p.p, sizeof(v));
    urb_push(stack, (Value){ .u = (UInt)v });
}

void urbc_op_mem_writeptr(List *stack)
{
    Value p, v; uintptr_t tmp;
    if (urbc_mem_fail_if_short(stack, 2, "mem.writeptr")) return;
    v = urb_pop(stack);
    p = urb_pop(stack);
    tmp = (uintptr_t)v.u;
    if (p.p) memcpy(p.p, &tmp, sizeof(tmp));
}

void urbc_op_mem_readcstring(List *stack)
{
    Value p;
    if (urbc_mem_fail_if_short(stack, 1, "mem.readcstring")) return;
    p = urb_pop(stack);
    urb_push(stack, (Value){ .p = p.p });
}

void urbc_op_mem_writecstring(List *stack)
{
    Value p, s;
    if (urbc_mem_fail_if_short(stack, 2, "mem.writecstring")) return;
    s = urb_pop(stack);
    p = urb_pop(stack);
    if (p.p && s.p) memcpy(p.p, s.p, strlen((const char *)s.p) + 1);
}

URBC_MEM_READ_FN(readi8,  int8_t,  i)
URBC_MEM_READ_FN(readu8,  uint8_t, u)
URBC_MEM_READ_FN(readi16, int16_t, i)
URBC_MEM_READ_FN(readu16, uint16_t, u)
URBC_MEM_READ_FN(readi32, int32_t, i)
URBC_MEM_READ_FN(readu32, uint32_t, u)
URBC_MEM_READ_FN(readi64, int64_t, i)
URBC_MEM_READ_FN(readu64, uint64_t, u)
URBC_MEM_READ_FN(readf32, float,   f)
URBC_MEM_READ_FN(readf64, double,  f)

URBC_MEM_WRITE_FN(writei8,  int8_t,  i)
URBC_MEM_WRITE_FN(writeu8,  uint8_t, u)
URBC_MEM_WRITE_FN(writei16, int16_t, i)
URBC_MEM_WRITE_FN(writeu16, uint16_t, u)
URBC_MEM_WRITE_FN(writei32, int32_t, i)
URBC_MEM_WRITE_FN(writeu32, uint32_t, u)
URBC_MEM_WRITE_FN(writei64, int64_t, i)
URBC_MEM_WRITE_FN(writeu64, uint64_t, u)
URBC_MEM_WRITE_FN(writef32, float,   f)
URBC_MEM_WRITE_FN(writef64, double,  f)
