'use strict';
const { ffi, memory: mem } = require('../../../');

// realloc
let p = mem.alloc(8n);
mem.writei32(p, 1); mem.writei32(p + 4n, 2);
p = mem.realloc(p, 16n);
mem.writei32(p + 8n, 3); mem.writei32(p + 12n, 4);
console.log('realloc:', mem.readi32(p), mem.readi32(p+4n), mem.readi32(p+8n), mem.readi32(p+12n));

// allocStr
const s = mem.allocStr('urb-ffi');
console.log('allocStr:', mem.readcstring(s));
mem.free(s);

// copy / zero / set / compare
const a = mem.alloc(8n), b = mem.alloc(8n);
mem.writei32(a, 0xDEAD); mem.writei32(a + 4n, 0xBEEF);
mem.copy(b, a, 8n);
console.log('copy:', mem.readi32(b) === mem.readi32(a) ? 'ok' : 'fail');
mem.zero(b, 8n);
console.log('zero:', mem.readi32(b), mem.readi32(b+4n));
mem.set(b, 0xFF, 4n);
console.log('set byte:', mem.readu8(b));
console.log('compare diferente:', mem.compare(a, b, 8n) !== 0 ? 'ok' : 'fail');
mem.copy(b, a, 8n);
console.log('compare igual:', mem.compare(a, b, 8n) === 0 ? 'ok' : 'fail');

// readptr / writeptr
const pp = mem.alloc(BigInt(mem.sizeof_ptr()));
mem.writeptr(pp, a);
const recovered = mem.readptr(pp);
console.log('readptr/writeptr:', mem.readi32(recovered) === mem.readi32(a) ? 'ok' : 'fail');

// readArray / writeArray
const arr_ptr = mem.alloc(16n);
mem.writeArray(arr_ptr, 'i32', [10, 20, 30, 40]);
const vals = mem.readArray(arr_ptr, 'i32', 4);
console.log('readArray i32:', vals);

const farr = mem.alloc(24n);
mem.writeArray(farr, 'f64', [Math.PI, Math.E, 1.5]);
const fvals = mem.readArray(farr, 'f64', 3);
console.log('readArray f64:', fvals.map(v => v.toFixed(4)));

// ffi.errno
const libc = ffi.open('libc.so.6');
const strtol = ffi.bind(ffi.sym(libc, 'strtol'), 'i64 strtol(cstring, pointer, i32)');
strtol('not_a_number', mem.nullptr(), 10n);
console.log('errno after strtol:', ffi.errno());
ffi.close(libc);

mem.free(a); mem.free(b); mem.free(pp); mem.free(arr_ptr); mem.free(farr); mem.free(p);
console.log('done');
