'use strict';
const { ffi, memory: mem } = require('../');

console.log('urb-ffi ffi.sym_self demo');

// strlen e puts estão no processo sem dlopen
const strlenDesc = ffi.describe('u64 strlen(cstring)');
const putsDesc = ffi.describe('i32 puts(cstring)');
const getenvDesc = ffi.describe('cstring getenv(cstring)');
const strlen = ffi.bind(ffi.sym_self('strlen'), strlenDesc);
const puts   = ffi.bind(ffi.sym_self('puts'), putsDesc);

const msg = 'hello world from sym_self';
puts(msg);
console.log(`strlen("${msg}") =`, strlen(msg));

const path = ffi.bind(ffi.sym_self('getenv'), getenvDesc)('PATH');
console.log('PATH (primeiros 60 chars):', path ? path.slice(0, 60) + '...' : '(nulo)');
