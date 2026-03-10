'use strict';
const { ffi, memory: mem } = require('../');

// qsort via callback — JS comparator → C function pointer
const libc = ffi.open('libc.so.6');
const qsortDesc = ffi.describe('void qsort(pointer, u64, u64, pointer)');
const cmpDesc = ffi.describe('i32 cmp(pointer, pointer)');
const qsort = ffi.bind(ffi.sym(libc, 'qsort'), qsortDesc);

const nums = [42, 7, 99, -3, 15];
const n    = nums.length;
const buf  = mem.alloc(BigInt(n * 4));
for (let i = 0; i < n; i++) mem.writei32(buf + BigInt(i * 4), nums[i]);

console.log('antes:', nums);

const cmp = ffi.callback(cmpDesc, (a, b) => {
    const va = mem.readi32(a);
    const vb = mem.readi32(b);
    return va < vb ? -1 : va > vb ? 1 : 0;
});

qsort(buf, BigInt(n), 4n, cmp.ptr);

const sorted = [];
for (let i = 0; i < n; i++) sorted.push(mem.readi32(buf + BigInt(i * 4)));
console.log('depois:', sorted);

mem.free(buf);
ffi.close(libc);
