'use strict';

const { ffi, memory: mem } = require('../');

function openFirst(names) {
	for (const name of names) {
		const handle = ffi.open(name);
		if (handle) {
			return { handle, name };
		}
	}
	throw new Error(`unable to open any runtime library: ${names.join(', ')}`);
}

const runtimeLibs = process.platform === 'win32'
	? ['ucrtbase.dll', 'msvcrt.dll']
	: process.platform === 'darwin'
		? ['/usr/lib/libSystem.B.dylib']
		: ['libc.so.6'];

const { handle: libc, name } = openFirst(runtimeLibs);
const putsDesc = ffi.describe('i32 puts(cstring)');
const qsortDesc = ffi.describe('void qsort(pointer, u64, u64, pointer)');
const cmpDesc = ffi.describe('i32 cmp(pointer, pointer)');
const puts = ffi.bind(ffi.sym(libc, 'puts'), putsDesc);
const qsort = ffi.bind(ffi.sym(libc, 'qsort'), qsortDesc);

puts(`hello from urb-ffi smoke (${process.platform}) via ${name}`);

const nums = [42, 7, 99, -3, 15];
const buf = mem.alloc(BigInt(nums.length * 4));
for (let i = 0; i < nums.length; i++) {
	mem.writei32(buf + BigInt(i * 4), nums[i]);
}

const cmp = ffi.callback(cmpDesc, (a, b) => {
	const va = mem.readi32(a);
	const vb = mem.readi32(b);
	return va < vb ? -1 : va > vb ? 1 : 0;
});

qsort(buf, BigInt(nums.length), 4n, cmp.ptr);
const sorted = [];
for (let i = 0; i < nums.length; i++) {
	sorted.push(mem.readi32(buf + BigInt(i * 4)));
}

console.log('sorted =', sorted);
mem.free(buf);
ffi.close(libc);
