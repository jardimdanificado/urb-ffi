'use strict';

const { ffi, memory: mem } = require('..');

const libcName = process.platform === 'darwin'
	? '/usr/lib/libSystem.B.dylib'
	: process.platform === 'win32'
		? 'ucrtbase.dll'
		: 'libc.so.6';

const libc = ffi.open(libcName, ffi.flags.NOW | ffi.flags.LOCAL);
const putsType = ffi.type.func(ffi.type.i32(), [ffi.type.cstring()], { name: 'puts' });
const puts = ffi.bind(ffi.sym(libc, 'puts'), putsType);

puts('hello from urb-ffi manual type descriptors (node)');

const intPtr = mem.alloc(4);
const intRef = ffi.global(intPtr, ffi.type.i32());
intRef.value = 123;
console.log('global i32 =', intRef.value);

const pointType = ffi.type.struct([
	{ name: 'x', type: ffi.type.i32() },
	{ name: 'y', type: ffi.type.i32() },
]);
const pointPtr = mem.alloc(ffi.type.sizeof(pointType));
const point = ffi.global(pointPtr, pointType);
point.x = 7;
point.y = 11;
console.log('point =', point.x, point.y);

mem.free(pointPtr);
mem.free(intPtr);
ffi.close(libc);