'use strict';
const { memory: mem } = require('../../../');

// alloc 4 bytes, write/read i32
let p = mem.alloc(4n);
mem.writei32(p, 42);
console.log('readi32:', mem.readi32(p));

// negative signed
mem.writei32(p, -7);
console.log('readi32 negativo:', mem.readi32(p));

// realloc para f64
p = mem.realloc(p, 8n);
mem.writef64(p, 3.14159);
console.log('readf64:', mem.readf64(p));

// cstring
let s = mem.alloc(32n);
mem.writecstring(s, 'olá do urb-ffi');
console.log('readcstring:', mem.readcstring(s));

mem.free(p);
mem.free(s);
console.log('tudo ok — sem leaks');
