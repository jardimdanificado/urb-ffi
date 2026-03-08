'use strict';
const { ffi, memory: mem } = require('../../../');

const libc = ffi.open('libc.so.6');
const puts   = ffi.bind(ffi.sym(libc, 'puts'),   'i32 puts(cstring)');
const getenv = ffi.bind(ffi.sym(libc, 'getenv'), 'cstring getenv(cstring)');

puts('hello from urb-ffi');
const home = getenv('HOME');
console.log('HOME =', home);

ffi.close(libc);
