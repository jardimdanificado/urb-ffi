'use strict';
const { ffi, memory: mem } = require('../../../');

console.log('urb-ffi ffi.sym_self demo');

// strlen e puts estão no processo sem dlopen
const strlen = ffi.bind(ffi.sym_self('strlen'), 'u64 strlen(cstring)');
const puts   = ffi.bind(ffi.sym_self('puts'),   'i32 puts(cstring)');

const msg = 'hello world from sym_self';
puts(msg);
console.log(`strlen("${msg}") =`, strlen(msg));

const path = ffi.bind(ffi.sym_self('getenv'), 'cstring getenv(cstring)')('PATH');
console.log('PATH (primeiros 60 chars):', path ? path.slice(0, 60) + '...' : '(nulo)');
