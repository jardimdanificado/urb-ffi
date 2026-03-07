#include "urbc_internal.h"

static void urbc_require_stack(List *stack, Int n, const char *what)
{
    UrbcRuntime *rt = urbc_runtime_current();
    if (!stack || stack->size < n) urbc_runtime_fail(rt, "%s: stack underflow", what);
}

static int urbc_truthy(Value v)
{
    return v.i != 0;
}

void urbc_op_stack_dup(List *stack)
{
    urbc_require_stack(stack, 1, "stack.dup");
    if (urbc_runtime_current()->failed) return;
    urb_push(stack, stack->data[stack->size - 1]);
}

void urbc_op_stack_pop(List *stack)
{
    urbc_require_stack(stack, 1, "stack.pop");
    if (urbc_runtime_current()->failed) return;
    (void)urb_pop(stack);
}

void urbc_op_stack_swap(List *stack)
{
    Value a, b;
    urbc_require_stack(stack, 2, "stack.swap");
    if (urbc_runtime_current()->failed) return;
    a = urb_pop(stack);
    b = urb_pop(stack);
    urb_push(stack, a);
    urb_push(stack, b);
}

void urbc_op_ptr_add(List *stack)
{
    Value n, p;
    urbc_require_stack(stack, 2, "ptr.add");
    if (urbc_runtime_current()->failed) return;
    n = urb_pop(stack);
    p = urb_pop(stack);
    urb_push(stack, (Value){ .u = p.u + (UInt)n.i });
}

void urbc_op_ptr_sub(List *stack)
{
    Value b, a;
    urbc_require_stack(stack, 2, "ptr.sub");
    if (urbc_runtime_current()->failed) return;
    b = urb_pop(stack);
    a = urb_pop(stack);
    urb_push(stack, (Value){ .i = (Int)(a.u - b.u) });
}

void urbc_op_cmp_eq(List *stack)
{
    Value b, a;
    urbc_require_stack(stack, 2, "cmp.eq");
    if (urbc_runtime_current()->failed) return;
    b = urb_pop(stack);
    a = urb_pop(stack);
    urb_push(stack, (Value){ .i = (a.u == b.u) });
}

void urbc_op_cmp_ne(List *stack)
{
    Value b, a;
    urbc_require_stack(stack, 2, "cmp.ne");
    if (urbc_runtime_current()->failed) return;
    b = urb_pop(stack);
    a = urb_pop(stack);
    urb_push(stack, (Value){ .i = (a.u != b.u) });
}

void urbc_op_cmp_lt_i64(List *stack)
{
    Value b, a;
    urbc_require_stack(stack, 2, "cmp.lt_i64");
    if (urbc_runtime_current()->failed) return;
    b = urb_pop(stack);
    a = urb_pop(stack);
    urb_push(stack, (Value){ .i = (a.i < b.i) });
}

void urbc_op_cmp_le_i64(List *stack)
{
    Value b, a;
    urbc_require_stack(stack, 2, "cmp.le_i64");
    if (urbc_runtime_current()->failed) return;
    b = urb_pop(stack);
    a = urb_pop(stack);
    urb_push(stack, (Value){ .i = (a.i <= b.i) });
}

void urbc_op_cmp_gt_i64(List *stack)
{
    Value b, a;
    urbc_require_stack(stack, 2, "cmp.gt_i64");
    if (urbc_runtime_current()->failed) return;
    b = urb_pop(stack);
    a = urb_pop(stack);
    urb_push(stack, (Value){ .i = (a.i > b.i) });
}

void urbc_op_cmp_ge_i64(List *stack)
{
    Value b, a;
    urbc_require_stack(stack, 2, "cmp.ge_i64");
    if (urbc_runtime_current()->failed) return;
    b = urb_pop(stack);
    a = urb_pop(stack);
    urb_push(stack, (Value){ .i = (a.i >= b.i) });
}

void urbc_op_logic_not(List *stack)
{
    Value a;
    urbc_require_stack(stack, 1, "logic.not");
    if (urbc_runtime_current()->failed) return;
    a = urb_pop(stack);
    urb_push(stack, (Value){ .i = !urbc_truthy(a) });
}
