'use strict';
const { ffi, memory: mem } = require('../');

// struct Point { i32 x; i32 y; f64 value; u64 flags; }
const PointSchema = {
    x:     'i32',
    y:     'i32',
    value: 'f64',
    flags: 'u64',
};

const sz = mem.struct_sizeof(PointSchema);
console.log('sizeof Point =', sz);                           // 24
console.log('offsetof x   =', mem.struct_offsetof(PointSchema, 'x'));
console.log('offsetof y   =', mem.struct_offsetof(PointSchema, 'y'));
console.log('offsetof value =', mem.struct_offsetof(PointSchema, 'value'));
console.log('offsetof flags =', mem.struct_offsetof(PointSchema, 'flags'));

// Aloca e popula
const p = mem.alloc(BigInt(sz));
mem.zero(p, BigInt(sz));
const v = mem.view(p, PointSchema);
v.x = 10; v.y = 20; v.value = 3.14; v.flags = 0xDEADBEEFn;

const v2 = mem.view(p, PointSchema);
console.log('x =', v2.x, 'y =', v2.y, 'value =', v2.value.toFixed(2),
            'flags =', '0x' + v2.flags.toString(16));

// viewArray: 3 pontos
const n = 3;
const arr_ptr = mem.alloc(BigInt(n * sz));
mem.zero(arr_ptr, BigInt(n * sz));
const pts = mem.viewArray(arr_ptr, PointSchema, n);
pts[0].x = 1; pts[1].x = 2; pts[2].x = 3;

const pts2 = mem.viewArray(arr_ptr, PointSchema, n);
for (let i = 0; i < n; i++) console.log(`pts[${i}].x =`, pts2[i].x);

// clock_gettime via librt / libc
const libc = ffi.open('libc.so.6');
const ClockSchema = { tv_sec: 'i64', tv_nsec: 'i64' };
const clock_ptr = mem.alloc(BigInt(mem.struct_sizeof(ClockSchema)));
const clockgettimeDesc = ffi.describe('i32 clock_gettime(i32, pointer)');
const clockgettime = ffi.bind(ffi.sym(libc, 'clock_gettime'), clockgettimeDesc);
clockgettime(0, clock_ptr);  // CLOCK_REALTIME = 0
const ts = mem.view(clock_ptr, ClockSchema);
console.log('tv_nsec =', ts.tv_nsec);

mem.free(p); mem.free(arr_ptr); mem.free(clock_ptr);
ffi.close(libc);
