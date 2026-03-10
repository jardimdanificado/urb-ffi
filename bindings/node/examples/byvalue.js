'use strict';

const path = require('path');
const { ffi, memory: mem } = require('..');

// load the helper library we compiled via Makefile
const libName = path.join(__dirname, 'libbyvalue' + (process.platform === 'darwin' ? '.dylib' : '.so'));
const lib = ffi.open(libName, ffi.flags.NOW | ffi.flags.LOCAL);

// define a simple struct layout
const pointType = ffi.type.struct([
    { name: 'x', type: ffi.type.i32() },
    { name: 'y', type: ffi.type.i32() },
]);

// allocate and populate a point
const pptr = mem.alloc(ffi.type.sizeof(pointType));
const point = ffi.global(pptr, pointType);
point.x = 7;
point.y = 11;

// call sum_point(struct Point) -> i32
const sumSig = ffi.type.func(ffi.type.i32(), [pointType], { name: 'sum_point' });
const sum = ffi.bind(ffi.sym(lib, 'sum_point'), sumSig);
console.log('sum_point', sum(point)); // expect 18

// call swap_point(Point) -> Point (by-value return)
const swapSig = ffi.type.func(pointType, [pointType], { name: 'swap_point' });
const swap = ffi.bind(ffi.sym(lib, 'swap_point'), swapSig);
const swappedPtr = swap(point);
const swapped = ffi.global(swappedPtr, pointType);
console.log('swapped', swapped.x, swapped.y);

// callback example: define a JS callback taking Point by value
const cbType = ffi.type.func(ffi.type.i32(), [pointType], { name: 'cb' });
const jsCb = ffi.callback(cbType, (pt) => {
    console.log('callback got', pt.x, pt.y);
    return pt.x * 100 + pt.y;
});
// call C helper call_cb(point, cbptr)
const callCbSig = ffi.type.func(ffi.type.i32(), [pointType, ffi.type.pointer(cbType)], { name: 'call_cb' });
const call_cb = ffi.bind(ffi.sym(lib, 'call_cb'), callCbSig);
console.log('call_cb returns', call_cb(point, jsCb.ptr));

// callback returning Point by value
const mapperType = ffi.type.func(pointType, [pointType], { name: 'mapper' });
const mapperCb = ffi.callback(mapperType, (pt) => ({
    x: pt.y + 1,
    y: pt.x + 2,
}));
const mapPointSig = ffi.type.func(pointType, [pointType, ffi.type.pointer(mapperType)], { name: 'map_point' });
const map_point = ffi.bind(ffi.sym(lib, 'map_point'), mapPointSig);
const mapped = ffi.global(map_point(point, mapperCb.ptr), pointType);
console.log('mapped', mapped.x, mapped.y);

// callback receiving a function pointer argument
const opType = ffi.type.func(ffi.type.i32(), [ffi.type.i32()], { name: 'op' });
const useOpType = ffi.type.func(ffi.type.i32(), [ffi.type.pointer(opType), ffi.type.i32()], { name: 'use_op' });
const useOpCb = ffi.callback(useOpType, (op, value) => {
    const applied = op(value);
    console.log('op(value)', applied);
    return applied * 2;
});
const callWithOpSig = ffi.type.func(ffi.type.i32(), [ffi.type.i32(), ffi.type.pointer(useOpType)], { name: 'call_with_op' });
const call_with_op = ffi.bind(ffi.sym(lib, 'call_with_op'), callWithOpSig);
console.log('call_with_op returns', call_with_op(10, useOpCb.ptr));

ffi.close(lib);
mem.free(pptr);
