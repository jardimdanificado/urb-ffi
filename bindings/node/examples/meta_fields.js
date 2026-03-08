'use strict';
const { ffi, memory: mem } = require('../');

// ── 1. __pointer — campo tratado como ponteiro nativo ─────────────────────────
const NodeSchema = {
    value: 'i32',
    next:  { __pointer: true },
};
const node_sz = mem.struct_sizeof(NodeSchema);
console.log('sizeof(Node) =', node_sz);

const n1 = mem.alloc(BigInt(node_sz));
const n2 = mem.alloc(BigInt(node_sz));
mem.zero(n1, BigInt(node_sz)); mem.zero(n2, BigInt(node_sz));
mem.writei32(n1, 3);
mem.writeptr(n1 + BigInt(mem.struct_offsetof(NodeSchema, 'next')), n2);
mem.writei32(n2, 7);

const v1 = mem.view(n1, NodeSchema);
const v2 = mem.view(n2, NodeSchema);
console.log('node1.value =', v1.value);
console.log('node1.next (addr) =', typeof v1.next);  // bigint
console.log('node2.value =', v2.value);

// ── 2. __union dentro de struct ───────────────────────────────────────────────
const FloatBitsInner = { __union: true, f: 'f32', u: 'u32', b: ['u8', 4] };
const FloatBitsSchema = { data: FloatBitsInner };
console.log('sizeof(FloatBits) =', mem.struct_sizeof(FloatBitsSchema));

const fb = mem.alloc(4n);
mem.writef32(fb, 3.14);
const fbv = mem.view(fb, FloatBitsSchema);
console.log('data.f =', fbv.data.f.toFixed(2));
console.log('data.u =', '0x' + fbv.data.u.toString(16));
console.log('data.b =', fbv.data.b);

// ── 3. __struct explícito (padrão) ────────────────────────────────────────────
const Vec3 = { x: 'f32', y: 'f32', z: 'f32' };
const RaySchema = { __struct: true, origin: Vec3, direction: Vec3 };
const ray_sz = mem.struct_sizeof(RaySchema);
console.log('sizeof(Ray) =', ray_sz);

const ray = mem.alloc(BigInt(ray_sz));
mem.zero(ray, BigInt(ray_sz));
const rv = mem.view(ray, RaySchema);
rv.origin.x = 1.0; rv.origin.y = 2.0; rv.origin.z = 3.0;
rv.direction.y = 1.0;

const rv2 = mem.view(ray, RaySchema);
console.log('origin    =', rv2.origin.x, rv2.origin.y, rv2.origin.z);
console.log('direction =', rv2.direction.x, rv2.direction.y, rv2.direction.z);

// ── 4. __union no topo via memory.view ───────────────────────────────────────
const U2 = { __union: true, i: 'i32', u: 'u32', f: 'f32' };
const ubuf = mem.alloc(4n);
mem.writef32(ubuf, -1.5);
const uv = mem.view(ubuf, U2);
console.log('union.f =', uv.f.toFixed(2));
console.log('union.i =', uv.i);

// ── 5. struct_sizeof respeita __union ─────────────────────────────────────────
console.log('sizeof(union{i64,u8}) =', mem.struct_sizeof({ __union: true, a: 'i64', b: 'u8' }));

mem.free(n1); mem.free(n2); mem.free(fb); mem.free(ray); mem.free(ubuf);
console.log('done');
