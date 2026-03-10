'use strict';

const fs = require('fs');
const path = require('path');

function loadNative() {
	const candidates = [
		['dist/urb-ffi.node', 'dist build'],
		['build/Release/urb_ffi.node', 'node-gyp release build'],
		['build/Debug/urb_ffi.node', 'node-gyp debug build'],
	];

	for (const [relativePath] of candidates) {
		const filePath = path.join(__dirname, relativePath);
		if (!fs.existsSync(filePath)) continue;
		return require(filePath);
	}

	throw new Error('Could not locate the urb-ffi native addon. Run `npm install` or `npm run build`.');
}

const native = loadNative();

const PTR_SIZE = native.sizeofPtr();

const TYPE_ALIASES = new Map([
	['bool', 'bool'],
	['boolean', 'bool'],
	['i8', 'i8'],
	['int8', 'i8'],
	['u8', 'u8'],
	['uint8', 'u8'],
	['byte', 'u8'],
	['i16', 'i16'],
	['int16', 'i16'],
	['u16', 'u16'],
	['uint16', 'u16'],
	['i32', 'i32'],
	['int32', 'i32'],
	['int', 'i32'],
	['u32', 'u32'],
	['uint32', 'u32'],
	['uint', 'u32'],
	['i64', 'i64'],
	['int64', 'i64'],
	['long', 'i64'],
	['u64', 'u64'],
	['uint64', 'u64'],
	['ulong', 'u64'],
	['f32', 'f32'],
	['float32', 'f32'],
	['float', 'f32'],
	['f64', 'f64'],
	['float64', 'f64'],
	['double', 'f64'],
	['pointer', 'pointer'],
	['ptr', 'pointer'],
	['cstring', 'cstring'],
	['string', 'cstring'],
]);

const TYPE_INFO = Object.freeze({
	bool:    { size: 1, align: 1 },
	i8:      { size: 1, align: 1 },
	u8:      { size: 1, align: 1 },
	i16:     { size: 2, align: 2 },
	u16:     { size: 2, align: 2 },
	i32:     { size: 4, align: 4 },
	u32:     { size: 4, align: 4 },
	i64:     { size: 8, align: 8 },
	u64:     { size: 8, align: 8 },
	f32:     { size: 4, align: 4 },
	f64:     { size: 8, align: 8 },
	pointer: { size: PTR_SIZE, align: PTR_SIZE },
	cstring: { size: PTR_SIZE, align: PTR_SIZE },
});

const SCHEMA_CACHE = new WeakMap();
const SCHEMA_STACK = new WeakSet();

function alignUp(value, align) {
	return (value + align - 1) & ~(align - 1);
}

function toNonNegativeInt(value, what) {
	const n = typeof value === 'bigint' ? Number(value) : Number(value);
	if (!Number.isInteger(n) || n < 0) {
		throw new TypeError(`${what} must be a non-negative integer`);
	}
	return n;
}

function normalizeType(type) {
	if (typeof type !== 'string') {
		throw new TypeError('type must be a string');
	}
	const normalized = TYPE_ALIASES.get(type.trim());
	if (!normalized) {
		throw new TypeError(`unsupported type: ${type}`);
	}
	return normalized;
}

function compileField(name, desc) {
	if (typeof desc === 'string') {
		const type = normalizeType(desc);
		const info = TYPE_INFO[type];
		if (type === 'pointer') {
			return { name, kind: 'pointer', size: info.size, align: info.align };
		}
		return { name, kind: 'prim', type, size: info.size, align: info.align };
	}

	if (Array.isArray(desc)) {
		if (desc.length !== 2) {
			throw new TypeError(`array field ${name} must be [type, count]`);
		}
		const type = normalizeType(desc[0]);
		const count = toNonNegativeInt(desc[1], `array count for ${name}`);
		if (count === 0) {
			throw new TypeError(`array field ${name} must have count > 0`);
		}
		const info = TYPE_INFO[type];
		return {
			name,
			kind: 'array',
			type,
			count,
			size: info.size * count,
			align: info.align,
		};
	}

	if (!desc || typeof desc !== 'object') {
		throw new TypeError(`invalid field descriptor for ${name}`);
	}

	if (desc.__pointer) {
		return { name, kind: 'pointer', size: PTR_SIZE, align: PTR_SIZE };
	}

	const schema = compileSchema(desc);
	return {
		name,
		kind: 'struct',
		schema,
		size: schema.size,
		align: schema.align,
	};
}

function compileSchema(schema) {
	if (!schema || typeof schema !== 'object' || Array.isArray(schema)) {
		throw new TypeError('schema must be a plain object');
	}
	const cached = SCHEMA_CACHE.get(schema);
	if (cached) {
		return cached;
	}
	if (SCHEMA_STACK.has(schema)) {
		throw new TypeError('recursive by-value schema is not supported');
	}

	SCHEMA_STACK.add(schema);
	try {
		const kind = schema.__union ? 'union' : 'struct';
		const fields = [];
		for (const [name, desc] of Object.entries(schema)) {
			if (name.startsWith('__')) continue;
			fields.push(compileField(name, desc));
		}

		let offset = 0;
		let maxAlign = 1;
		let maxSize = 0;

		for (const field of fields) {
			maxAlign = Math.max(maxAlign, field.align);
			if (kind === 'union') {
				field.offset = 0;
				maxSize = Math.max(maxSize, field.size);
			} else {
				offset = alignUp(offset, field.align);
				field.offset = offset;
				offset += field.size;
			}
		}

		const meta = {
			kind,
			align: maxAlign,
			size: alignUp(kind === 'union' ? maxSize : offset, maxAlign),
			fields,
			fieldMap: new Map(fields.map((field) => [field.name, field])),
		};

		SCHEMA_CACHE.set(schema, meta);
		return meta;
	} finally {
		SCHEMA_STACK.delete(schema);
	}
}

function makeAbiType(kind, props = {}) {
	return Object.freeze({ __ffiAbiType: true, kind, ...props });
}

function isAbiType(value) {
	return !!value && typeof value === 'object' && value.__ffiAbiType === true;
}

function isFfiDescriptor(value) {
	return !!value && typeof value === 'object' && value.__ffiDescriptor === true;
}

function sanitizeAbiName(name) {
	const text = String(name || 'fn').trim();
	const safe = text.replace(/[^A-Za-z0-9_]/g, '_');
	return safe && /^[A-Za-z_]/.test(safe) ? safe : `fn_${safe || 'anon'}`;
}

function normalizeAbiType(type, what = 'type') {
	if (isAbiType(type)) {
		return type;
	}
	if (typeof type === 'string') {
		const text = type.trim();
		if (text === 'void') return makeAbiType('void');
		const normalized = normalizeType(text);
		if (normalized === 'pointer') return makeAbiType('pointer', { to: null });
		if (normalized === 'cstring') return makeAbiType('cstring');
		return makeAbiType('primitive', { name: normalized });
	}
	throw new TypeError(`${what} must be a type string or ffi.type.* descriptor`);
}

function normalizeAbiFieldEntries(fields, what) {
	if (Array.isArray(fields)) {
		return fields.map((entry, index) => {
			if (Array.isArray(entry)) {
				if (entry.length !== 2) {
					throw new TypeError(`${what}[${index}] must be [name, type]`);
				}
				return {
					name: String(entry[0]),
					type: normalizeAbiType(entry[1], `${what}[${index}] type`),
				};
			}
			if (!entry || typeof entry !== 'object') {
				throw new TypeError(`${what}[${index}] must be an object or [name, type] tuple`);
			}
			if (typeof entry.name !== 'string' || !entry.name) {
				throw new TypeError(`${what}[${index}] must have a non-empty name`);
			}
			return {
				name: entry.name,
				type: normalizeAbiType(entry.type, `${what}[${index}] type`),
			};
		});
	}
	if (fields && typeof fields === 'object') {
		return Object.entries(fields).map(([name, type]) => ({
			name,
			type: normalizeAbiType(type, `${what}.${name}`),
		}));
	}
	throw new TypeError(`${what} must be an array of fields or a plain object`);
}

function abiTypeToString(type) {
	const abiType = normalizeAbiType(type);
	switch (abiType.kind) {
	case 'void':
		return 'void';
	case 'primitive':
		return abiType.name;
	case 'cstring':
		return 'cstring';
	case 'pointer':
		return abiType.to ? `pointer(${abiTypeToString(abiType.to)})` : 'pointer';
	case 'array':
		return `array(${abiTypeToString(abiType.element)}, ${abiType.length})`;
	case 'struct':
	case 'union':
		return `${abiType.kind}(${abiType.fields.map((field) => `${field.name}: ${abiTypeToString(field.type)}`).join(', ')})`;
	case 'enum':
		return `enum(${abiTypeToString(abiType.underlying)})`;
	case 'function': {
		const prefix = abiType.abi && abiType.abi !== 'default' ? `abi(${abiType.abi}) ` : '';
		const args = abiType.args.map((arg) => abiTypeToString(arg));
		if (abiType.varargs) args.push('...');
		return `${prefix}${abiTypeToString(abiType.ret)} ${sanitizeAbiName(abiType.name)}(${args.join(', ')})`;
	}
	default:
		throw new TypeError(`unsupported ABI type kind: ${abiType.kind}`);
	}
}

function abiTypeToNativeLeaf(type) {
	const abiType = normalizeAbiType(type);
	switch (abiType.kind) {
	case 'void':
		return 'void';
	case 'primitive':
		return abiType.name;
	case 'cstring':
		return 'cstring';
	case 'pointer':
		return 'pointer';
	case 'enum':
		return abiTypeToNativeLeaf(abiType.underlying);
	default:
		return null;
	}
}

function abiFunctionToNativeSignature(fnType) {
	if (fnType.abi && fnType.abi !== 'default') {
		return null;
	}
	const ret = abiTypeToNativeLeaf(fnType.ret);
	if (!ret) return null;
	const args = fnType.args.map((arg) => abiTypeToNativeLeaf(arg));
	if (args.some((arg) => !arg)) return null;
	const parts = [...args];
	if (fnType.varargs) parts.push('...');
	return `${ret} ${sanitizeAbiName(fnType.name)}(${parts.join(', ')})`;
}

function createFfiDescriptor(sig, handle, type, nativeSig) {
	const descriptor = {
		__ffiDescriptor: true,
		sig: String(sig),
		type: type || null,
		nativeSig: nativeSig || String(sig),
		nativeCapable: Boolean(handle),
	};
	if (handle) {
		Object.defineProperty(descriptor, 'handle', {
			value: handle,
			enumerable: false,
			writable: false,
			configurable: false,
		});
	}
	return Object.freeze(descriptor);
}

function describeFunctionType(fnType) {
	const sig = abiTypeToString(fnType);
	const nativeSig = abiFunctionToNativeSignature(fnType);
	const handle = nativeSig ? native.describe(nativeSig) : null;
	return createFfiDescriptor(sig, handle, fnType, nativeSig || sig);
}

function ensureFfiDescriptor(value, context) {
	if (isFfiDescriptor(value)) {
		return value;
	}
	if (typeof value === 'string') {
		const text = String(value);
		return createFfiDescriptor(text, native.describe(text), null, text);
	}
	if (isAbiType(value) && value.kind === 'function') {
		return describeFunctionType(value);
	}
	throw new TypeError(`${context} expects a signature string, ffi.type.func(...), or descriptor`);
}

function abiTypeToLayoutSchema(type) {
	const abiType = normalizeAbiType(type);
	switch (abiType.kind) {
	case 'primitive':
		return abiType.name;
	case 'cstring':
		return 'cstring';
	case 'pointer':
	case 'function':
		return { __pointer: true };
	case 'enum':
		return abiTypeToLayoutSchema(abiType.underlying);
	case 'array': {
		const element = abiTypeToLayoutSchema(abiType.element);
		if (typeof element !== 'string') {
			throw new TypeError('only arrays of scalar/pointer-compatible elements are layout-compatible in this binding');
		}
		return [element, abiType.length];
	}
	case 'struct':
	case 'union': {
		const schema = abiType.kind === 'union' ? { __union: true } : {};
		for (const field of abiType.fields) {
			schema[field.name] = abiTypeToLayoutSchema(field.type);
		}
		return schema;
	}
	default:
		throw new TypeError(`type ${abiTypeToString(abiType)} cannot be converted to a memory layout`);
	}
}

function abiTypeSize(type) {
	const abiType = normalizeAbiType(type);
	switch (abiType.kind) {
	case 'void':
		return 0;
	case 'primitive':
		return TYPE_INFO[abiType.name].size;
	case 'cstring':
	case 'pointer':
	case 'function':
		return PTR_SIZE;
	case 'enum':
		return abiTypeSize(abiType.underlying);
	case 'array':
		return abiTypeSize(abiType.element) * abiType.length;
	case 'struct':
	case 'union':
		return compileSchema(abiTypeToLayoutSchema(abiType)).size;
	default:
		throw new TypeError(`unsupported ABI type kind: ${abiType.kind}`);
	}
}

function normalizeGlobalType(value) {
	if (isFfiDescriptor(value)) {
		if (!value.type) {
			throw new TypeError('ffi.global requires a type descriptor or a manual function descriptor');
		}
		return value.type.kind === 'function'
			? makeAbiType('pointer', { to: value.type })
			: value.type;
	}
	const type = normalizeAbiType(value, 'global type');
	return type.kind === 'function' ? makeAbiType('pointer', { to: type }) : type;
}

function readAbiValue(ptr, type) {
	const abiType = normalizeAbiType(type);
	switch (abiType.kind) {
	case 'primitive':
		return readPrimitive(ptr, abiType.name);
	case 'enum':
		return readAbiValue(ptr, abiType.underlying);
	case 'cstring':
	case 'pointer':
	case 'function':
		return native.readptr(ptr);
	default:
		throw new TypeError(`ffi.global cannot read ${abiTypeToString(abiType)} directly`);
	}
}

function writeAbiValue(ptr, type, value) {
	const abiType = normalizeAbiType(type);
	switch (abiType.kind) {
	case 'primitive':
		writePrimitive(ptr, abiType.name, value);
		return;
	case 'enum':
		writeAbiValue(ptr, abiType.underlying, value);
		return;
	case 'cstring':
	case 'pointer':
	case 'function':
		native.writeptr(ptr, value);
		return;
	default:
		throw new TypeError(`ffi.global cannot write ${abiTypeToString(abiType)} directly`);
	}
}

function createScalarGlobal(ptr, type) {
	const basePtr = BigInt(ptr);
	const abiType = normalizeAbiType(type);
	const read = () => readAbiValue(basePtr, abiType);
	const write = (value) => writeAbiValue(basePtr, abiType, value);
	return new Proxy({ ptr: basePtr, type: abiType }, {
		get(target, prop) {
			if (prop === 'ptr') return basePtr;
			if (prop === 'type') return abiType;
			if (prop === 'value') return read();
			if (prop === 'read') return read;
			if (prop === 'write') return write;
			return target[prop];
		},
		set(target, prop, value) {
			if (prop === 'value') {
				write(value);
				return true;
			}
			target[prop] = value;
			return true;
		},
		has(_, prop) {
			return prop === 'ptr' || prop === 'type' || prop === 'value' || prop === 'read' || prop === 'write';
		},
		ownKeys() {
			return ['ptr', 'type', 'value'];
		},
		getOwnPropertyDescriptor(_, prop) {
			if (prop === 'ptr' || prop === 'type' || prop === 'value') {
				return { enumerable: true, configurable: true };
			}
			return undefined;
		},
	});
}

function createArrayGlobal(ptr, type) {
	const basePtr = BigInt(ptr);
	const abiType = normalizeAbiType(type);
	const length = abiType.length;
	if (abiType.element.kind === 'struct' || abiType.element.kind === 'union') {
		return createViewArray(basePtr, compileSchema(abiTypeToLayoutSchema(abiType.element)), length);
	}
	const stride = abiTypeSize(abiType.element);
	return new Proxy({ ptr: basePtr, length, count: length }, {
		get(_, prop) {
			if (prop === 'ptr') return basePtr;
			if (prop === 'length' || prop === 'count') return length;
			if (prop === 'toArray') {
				return () => {
					const out = new Array(length);
					for (let i = 0; i < length; i++) {
						out[i] = readAbiValue(basePtr + BigInt(i * stride), abiType.element);
					}
					return out;
				};
			}
			if (!isArrayIndex(prop)) return undefined;
			const index = Number(prop);
			if (index >= length) return undefined;
			return readAbiValue(basePtr + BigInt(index * stride), abiType.element);
		},
		set(_, prop, value) {
			if (!isArrayIndex(prop)) return false;
			const index = Number(prop);
			if (index >= length) return false;
			writeAbiValue(basePtr + BigInt(index * stride), abiType.element, value);
			return true;
		},
		has(_, prop) {
			if (prop === 'ptr' || prop === 'length' || prop === 'count') return true;
			return isArrayIndex(prop) && Number(prop) < length;
		},
		ownKeys() {
			const keys = ['ptr', 'length', 'count'];
			for (let i = 0; i < length; i++) keys.push(String(i));
			return keys;
		},
		getOwnPropertyDescriptor(_, prop) {
			if (prop === 'ptr' || prop === 'length' || prop === 'count') {
				return { enumerable: true, configurable: true };
			}
			if (isArrayIndex(prop) && Number(prop) < length) {
				return { enumerable: true, configurable: true };
			}
			return undefined;
		},
	});
}

const ffiType = {
	primitive(name) {
		return normalizeAbiType(name, 'primitive type');
	},
	void() {
		return makeAbiType('void');
	},
	cstring() {
		return makeAbiType('cstring');
	},
	pointer(to = null) {
		return makeAbiType('pointer', { to: to == null ? null : normalizeAbiType(to, 'pointer target') });
	},
	array(element, length) {
		const count = toNonNegativeInt(length, 'array length');
		if (count <= 0) throw new TypeError('array length must be greater than zero');
		return makeAbiType('array', { element: normalizeAbiType(element, 'array element'), length: count });
	},
	struct(fields, options = {}) {
		return makeAbiType('struct', {
			name: options.name ? String(options.name) : null,
			fields: normalizeAbiFieldEntries(fields, 'struct fields'),
		});
	},
	union(fields, options = {}) {
		return makeAbiType('union', {
			name: options.name ? String(options.name) : null,
			fields: normalizeAbiFieldEntries(fields, 'union fields'),
		});
	},
	enum(values = {}, options = {}) {
		return makeAbiType('enum', {
			values: values && typeof values === 'object' ? { ...values } : {},
			underlying: normalizeAbiType(options.underlying || options.base || 'i32', 'enum underlying type'),
		});
	},
	func(ret, args = [], options = {}) {
		if (!Array.isArray(args)) throw new TypeError('ffi.type.func args must be an array');
		return makeAbiType('function', {
			name: sanitizeAbiName(options.name || 'fn'),
			abi: options.abi ? String(options.abi) : 'default',
			varargs: Boolean(options.varargs),
			ret: normalizeAbiType(ret, 'function return type'),
			args: args.map((arg, index) => normalizeAbiType(arg, `function arg ${index}`)),
		});
	},
	layout(type) {
		return abiTypeToLayoutSchema(normalizeAbiType(type));
	},
	sizeof(type) {
		return abiTypeSize(type);
	},
	offsetof(type, fieldName) {
		const abiType = normalizeAbiType(type);
		if (abiType.kind !== 'struct' && abiType.kind !== 'union') {
			throw new TypeError('ffi.type.offsetof expects a struct or union type');
		}
		const meta = compileSchema(abiTypeToLayoutSchema(abiType));
		const field = meta.fieldMap.get(String(fieldName));
		if (!field) {
			throw new Error(`field not found: ${fieldName}`);
		}
		return field.offset;
	},
};

for (const name of ['bool', 'i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64', 'f32', 'f64']) {
	ffiType[name] = () => makeAbiType('primitive', { name });
}

function isArrayIndex(prop) {
	return typeof prop === 'string' && /^(0|[1-9]\d*)$/.test(prop);
}

function readPrimitive(ptr, type) {
	switch (type) {
	case 'bool':    return native.readu8(ptr) !== 0;
	case 'i8':      return native.readi8(ptr);
	case 'u8':      return native.readu8(ptr);
	case 'i16':     return native.readi16(ptr);
	case 'u16':     return native.readu16(ptr);
	case 'i32':     return native.readi32(ptr);
	case 'u32':     return native.readu32(ptr);
	case 'i64':     return native.readi64(ptr);
	case 'u64':     return native.readu64(ptr);
	case 'f32':     return native.readf32(ptr);
	case 'f64':     return native.readf64(ptr);
	case 'pointer':
	case 'cstring': return native.readptr(ptr);
	default:
		throw new TypeError(`unsupported read type: ${type}`);
	}
}

function writePrimitive(ptr, type, value) {
	switch (type) {
	case 'bool':    native.writeu8(ptr, value ? 1 : 0); break;
	case 'i8':      native.writei8(ptr, value); break;
	case 'u8':      native.writeu8(ptr, value); break;
	case 'i16':     native.writei16(ptr, value); break;
	case 'u16':     native.writeu16(ptr, value); break;
	case 'i32':     native.writei32(ptr, value); break;
	case 'u32':     native.writeu32(ptr, value); break;
	case 'i64':     native.writei64(ptr, value); break;
	case 'u64':     native.writeu64(ptr, value); break;
	case 'f32':     native.writef32(ptr, value); break;
	case 'f64':     native.writef64(ptr, value); break;
	case 'pointer':
	case 'cstring': native.writeptr(ptr, value); break;
	default:
		throw new TypeError(`unsupported write type: ${type}`);
	}
}

function readArray(ptr, type, count) {
	const info = TYPE_INFO[normalizeType(type)];
	const values = new Array(toNonNegativeInt(count, 'count'));
	const base = BigInt(ptr);
	for (let i = 0; i < values.length; i++) {
		values[i] = readPrimitive(base + BigInt(i * info.size), normalizeType(type));
	}
	return values;
}

function writeArray(ptr, type, values) {
	if (!Array.isArray(values)) {
		throw new TypeError('values must be an array');
	}
	const normalizedType = normalizeType(type);
	const info = TYPE_INFO[normalizedType];
	const base = BigInt(ptr);
	for (let i = 0; i < values.length; i++) {
		writePrimitive(base + BigInt(i * info.size), normalizedType, values[i]);
	}
}

function createView(ptr, schemaMeta) {
	const basePtr = BigInt(ptr);
	const target = { ptr: basePtr };

	return new Proxy(target, {
		get(_, prop) {
			if (prop === 'ptr') return basePtr;
			if (typeof prop !== 'string') return undefined;

			const field = schemaMeta.fieldMap.get(prop);
			if (!field) return undefined;

			const addr = basePtr + BigInt(field.offset);
			switch (field.kind) {
			case 'prim':
				return readPrimitive(addr, field.type);
			case 'pointer':
				return native.readptr(addr);
			case 'array':
				return readArray(addr, field.type, field.count);
			case 'struct':
				return createView(addr, field.schema);
			default:
				return undefined;
			}
		},
		set(_, prop, value) {
			if (typeof prop !== 'string') return false;
			const field = schemaMeta.fieldMap.get(prop);
			if (!field) return false;

			const addr = basePtr + BigInt(field.offset);
			switch (field.kind) {
			case 'prim':
				writePrimitive(addr, field.type, value);
				return true;
			case 'pointer':
				native.writeptr(addr, value);
				return true;
			default:
				throw new TypeError(`field ${prop} is not directly assignable`);
			}
		},
		has(_, prop) {
			return prop === 'ptr' || schemaMeta.fieldMap.has(prop);
		},
		ownKeys() {
			return ['ptr', ...schemaMeta.fieldMap.keys()];
		},
		getOwnPropertyDescriptor(_, prop) {
			if (prop === 'ptr') {
				return { enumerable: true, configurable: true };
			}
			if (schemaMeta.fieldMap.has(prop)) {
				return { enumerable: true, configurable: true };
			}
			return undefined;
		},
	});
}

function createViewArray(ptr, schemaMeta, count) {
	const basePtr = BigInt(ptr);
	const size = schemaMeta.size;
	const length = toNonNegativeInt(count, 'count');
	const target = { ptr: basePtr, count: length, length };

	return new Proxy(target, {
		get(_, prop) {
			if (prop === 'ptr') return basePtr;
			if (prop === 'count' || prop === 'length') return length;
			if (prop === Symbol.iterator) {
				return function* iterator() {
					for (let i = 0; i < length; i++) {
						yield createView(basePtr + BigInt(i * size), schemaMeta);
					}
				};
			}
			if (!isArrayIndex(prop)) return undefined;

			const index = Number(prop);
			if (index >= length) return undefined;
			return createView(basePtr + BigInt(index * size), schemaMeta);
		},
		has(_, prop) {
			if (prop === 'ptr' || prop === 'count' || prop === 'length') return true;
			return isArrayIndex(prop) && Number(prop) < length;
		},
		ownKeys() {
			const keys = ['ptr', 'count', 'length'];
			for (let i = 0; i < length; i++) keys.push(String(i));
			return keys;
		},
		getOwnPropertyDescriptor(_, prop) {
			if (prop === 'ptr' || prop === 'count' || prop === 'length') {
				return { enumerable: true, configurable: true };
			}
			if (isArrayIndex(prop) && Number(prop) < length) {
				return { enumerable: true, configurable: true };
			}
			return undefined;
		},
	});
}

const ffi = {
	flags: native.dlopenFlags,
	type: ffiType,
	describe(sig) {
		return ensureFfiDescriptor(sig, 'ffi.describe');
	},
	open(path, flags = 0) {
		return native.open(String(path), flags);
	},
	close(handle) {
		return native.close(handle);
	},
	sym(handle, name) {
		return native.sym(handle, String(name));
	},
	sym_self(name) {
		return native.symSelf(String(name));
	},
	bind(ptr, descriptor) {
		const desc = ensureFfiDescriptor(descriptor, 'ffi.bind');
		if (!desc.handle) {
			throw new TypeError(`ffi.bind descriptor is not natively callable yet: ${desc.sig}`);
		}
		const handle = native.bind(ptr, desc.handle);
		const fn = (...args) => native.callBound(handle, args);
		Object.defineProperties(fn, {
			handle: { value: handle, enumerable: false },
			sig: { value: String(desc.sig || ''), enumerable: true },
			descriptor: { value: desc, enumerable: false },
		});
		return fn;
	},
	callback(descriptor, fn) {
		const desc = ensureFfiDescriptor(descriptor, 'ffi.callback');
		if (!desc.handle) {
			throw new TypeError(`ffi.callback descriptor is not natively callable yet: ${desc.sig}`);
		}
		const result = native.callback(desc.handle, fn);
		if (result && Object.prototype.hasOwnProperty.call(result, 'handle')) {
			Object.defineProperty(result, 'handle', {
				value: result.handle,
				enumerable: false,
				writable: false,
				configurable: false,
			});
		}
		if (result && typeof result === 'object') {
			Object.defineProperty(result, 'sig', {
				value: String(desc.sig || ''),
				enumerable: true,
				writable: false,
				configurable: false,
			});
		}
		return result;
	},
	global(ptr, descriptor) {
		const type = normalizeGlobalType(descriptor);
		if (type.kind === 'struct' || type.kind === 'union') {
			return createView(ptr, compileSchema(abiTypeToLayoutSchema(type)));
		}
		if (type.kind === 'array') {
			return createArrayGlobal(ptr, type);
		}
		return createScalarGlobal(ptr, type);
	},
	errno() {
		return native.errnoValue();
	},
	dlerror() {
		return native.dlerror();
	},
};

const memory = {
	alloc: native.alloc,
	free: native.free,
	realloc: native.realloc,
	zero: native.zero,
	copy: native.copy,
	set: native.set,
	compare: native.compare,
	nullptr: native.nullptr,
	sizeof_ptr() {
		return PTR_SIZE;
	},
	readptr: native.readptr,
	writeptr: native.writeptr,
	readcstring: native.readcstring,
	writecstring: native.writecstring,
	readi8: native.readi8,
	readu8: native.readu8,
	readi16: native.readi16,
	readu16: native.readu16,
	readi32: native.readi32,
	readu32: native.readu32,
	readi64: native.readi64,
	readu64: native.readu64,
	readf32: native.readf32,
	readf64: native.readf64,
	writei8: native.writei8,
	writeu8: native.writeu8,
	writei16: native.writei16,
	writeu16: native.writeu16,
	writei32: native.writei32,
	writeu32: native.writeu32,
	writei64: native.writei64,
	writeu64: native.writeu64,
	writef32: native.writef32,
	writef64: native.writef64,
	allocStr(value) {
		const text = String(value);
		const ptr = native.alloc(Buffer.byteLength(text, 'utf8') + 1);
		native.writecstring(ptr, text);
		return ptr;
	},
	readArray,
	writeArray,
	struct_sizeof(schema) {
		return compileSchema(schema).size;
	},
	struct_offsetof(schema, fieldName) {
		const meta = compileSchema(schema);
		const field = meta.fieldMap.get(String(fieldName));
		if (!field) {
			throw new Error(`field not found: ${fieldName}`);
		}
		return field.offset;
	},
	view(ptr, schema) {
		return createView(ptr, compileSchema(schema));
	},
	viewArray(ptr, schema, count) {
		return createViewArray(ptr, compileSchema(schema), count);
	},
};

module.exports = { ffi, memory };
