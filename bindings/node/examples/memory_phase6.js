'use strict';

const { ffi, memory: mem } = require('..');

const Mode = ffi.type.enum({
	IDLE: 0,
	RUNNING: 2,
});

const Point = {
	x: 'i32',
	y: 'i32',
};

const UnaryOp = ffi.type.func(ffi.type.i32(), [ffi.type.i32()], { name: 'unary_op' });

const Holder = {
	mode: Mode,
	cb: UnaryOp,
	point: { type: 'pointer', to: Point },
	nested: Point,
};

const holderPtr = mem.alloc(BigInt(mem.struct_sizeof(Holder)));
const pointPtr = mem.alloc(BigInt(mem.struct_sizeof(Point)));
mem.zero(holderPtr, BigInt(mem.struct_sizeof(Holder)));
mem.zero(pointPtr, BigInt(mem.struct_sizeof(Point)));

const point = mem.view(pointPtr, Point);
point.x = 10;
point.y = 20;

const addThree = ffi.callback(UnaryOp, (value) => value + 3);
const holder = mem.view(holderPtr, Holder);
holder.mode = 2;
holder.cb = addThree.ptr;
holder.point = pointPtr;
holder.nested = { x: 7, y: 9 };

console.log('enum mode', holder.mode);
console.log('fnptr call', holder.cb(39));
console.log('nested struct', holder.nested.x, holder.nested.y);
console.log('pointer deref', holder.point.deref().x, holder.point.deref().y);

holder.point.write({ x: 33, y: 44 });
console.log('pointer write', point.x, point.y);

const Packet = {
	len: 'u32',
	bytes: { type: 'u8', flexible: true },
};

const packetSize = mem.struct_sizeof(Packet) + 5;
const packetPtr = mem.alloc(BigInt(packetSize));
mem.zero(packetPtr, BigInt(packetSize));

const packet = mem.view(packetPtr, Packet, packetSize);
packet.len = 5;
packet.bytes = [1, 2, 3, 4, 5];

console.log('flex len', packet.len, 'byteSize', packet.byteSize);
console.log('flex bytes', packet.bytes.join(','));

mem.free(packetPtr);
mem.free(pointPtr);
mem.free(holderPtr);
