'use strict';
const { ffi, memory: mem } = require('../');

const libc = ffi.open('libc.so.6');
const putsDesc = ffi.describe('i32 puts(cstring)');
const getenvDesc = ffi.describe('cstring getenv(cstring)');
const puts   = ffi.bind(ffi.sym(libc, 'puts'), putsDesc);
const getenv = ffi.bind(ffi.sym(libc, 'getenv'), getenvDesc);

puts('hello from urb-ffi');
const home = getenv('HOME');
console.log('HOME =', home);

ffi.close(libc);
