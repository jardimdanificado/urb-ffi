#include <limits.h>
#include <pthread.h>
#include <stdint.h>

struct Point {
    int32_t x;
    int32_t y;
};

int32_t sum_point(struct Point p)
{
    return p.x + p.y;
}

struct Point swap_point(struct Point p)
{
    struct Point q = { p.y, p.x };
    return q;
}

int32_t call_cb(struct Point p, int32_t (*cb)(struct Point))
{
    return cb(p);
}

struct Point map_point(struct Point p, struct Point (*cb)(struct Point))
{
    return cb(p);
}

static int32_t plus_five(int32_t x)
{
    return x + 5;
}

int32_t call_with_op(int32_t x, int32_t (*cb)(int32_t (*)(int32_t), int32_t))
{
    return cb(plus_five, x);
}

typedef struct ThreadCallCtx {
    int32_t (*cb)(int32_t);
    int32_t arg;
    int32_t out;
} ThreadCallCtx;

static void *run_thread_cb(void *opaque)
{
    ThreadCallCtx *ctx = (ThreadCallCtx *)opaque;
    if (!ctx || !ctx->cb) return NULL;
    ctx->out = ctx->cb(ctx->arg);
    return NULL;
}

int32_t call_on_thread(int32_t (*cb)(int32_t), int32_t arg)
{
    pthread_t thread;
    ThreadCallCtx ctx;
    ctx.cb = cb;
    ctx.arg = arg;
    ctx.out = INT32_MIN;
    if (pthread_create(&thread, NULL, run_thread_cb, &ctx) != 0) {
        return INT32_MIN;
    }
    if (pthread_join(thread, NULL) != 0) {
        return INT32_MIN;
    }
    return ctx.out;
}
