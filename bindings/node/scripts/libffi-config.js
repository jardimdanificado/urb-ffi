#!/usr/bin/env node
'use strict';

const { execFileSync } = require('child_process');
const path = require('path');

function shellSplit(text) {
	const matches = text.match(/"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|[^\s]+/g);
	if (!matches) return [];
	return matches.map((token) => {
		if ((token.startsWith('"') && token.endsWith('"')) || (token.startsWith("'") && token.endsWith("'"))) {
			return token.slice(1, -1);
		}
		return token;
	});
}

function quoteTokens(tokens) {
	return tokens.map((token) => (/\s/.test(token) ? JSON.stringify(token) : token));
}

function parsePkgConfig(field) {
	try {
		return shellSplit(execFileSync('pkg-config', [field, 'libffi'], { encoding: 'utf8' }).trim());
	} catch {
		return null;
	}
}

function getEnvConfig() {
	const includeDirs = [];
	const libraries = [];
	const cflags = [];
	const includeEnv = process.env.LIBFFI_INCLUDE_DIR;
	const libDir = process.env.LIBFFI_LIB_DIR;
	const libName = process.env.LIBFFI_LIB_NAME || 'ffi';
	const libsEnv = process.env.LIBFFI_LIBS;
	const cflagsEnv = process.env.LIBFFI_CFLAGS;

	if (includeEnv) {
		includeDirs.push(...includeEnv.split(path.delimiter).filter(Boolean));
	}
	if (cflagsEnv) {
		cflags.push(...shellSplit(cflagsEnv));
	}
	if (libsEnv) {
		libraries.push(...shellSplit(libsEnv));
	} else if (libDir) {
		if (process.platform === 'win32') {
			libraries.push(`/LIBPATH:${libDir}`);
			libraries.push(libName.toLowerCase().endsWith('.lib') ? libName : `${libName}.lib`);
		} else {
			libraries.push(`-L${libDir}`);
			libraries.push(libName.startsWith('-l') ? libName : `-l${libName}`);
		}
	}

	return { includeDirs, libraries, cflags };
}

function getPkgConfig() {
	const cflagTokens = parsePkgConfig('--cflags');
	const libTokens = parsePkgConfig('--libs');
	if (!cflagTokens && !libTokens) {
		return null;
	}
	const includeDirs = [];
	const cflags = [];
	for (const token of cflagTokens || []) {
		if (token.startsWith('-I')) includeDirs.push(token.slice(2));
		else cflags.push(token);
	}
	return {
		includeDirs,
		libraries: libTokens || [],
		cflags,
	};
}

function fail(message) {
	console.error(message);
	process.exit(1);
}

const mode = process.argv[2];
const envConfig = getEnvConfig();
const pkgConfig = process.platform === 'win32' ? null : getPkgConfig();
const config = {
	includeDirs: envConfig.includeDirs.length ? envConfig.includeDirs : (pkgConfig ? pkgConfig.includeDirs : []),
	libraries: envConfig.libraries.length ? envConfig.libraries : (pkgConfig ? pkgConfig.libraries : []),
	cflags: envConfig.cflags.length ? envConfig.cflags : (pkgConfig ? pkgConfig.cflags : []),
};

if (process.platform === 'win32') {
	if (!config.includeDirs.length) {
		fail('Windows build requires LIBFFI_INCLUDE_DIR to point to the libffi headers.');
	}
	if (!config.libraries.length) {
		fail('Windows build requires LIBFFI_LIB_DIR/LIBFFI_LIB_NAME or LIBFFI_LIBS for libffi.');
	}
	config.cflags = [];
}

switch (mode) {
case 'include-dirs':
	process.stdout.write(quoteTokens(config.includeDirs).join(' '));
	break;
case 'libraries':
	process.stdout.write(quoteTokens(config.libraries).join(' '));
	break;
case 'cflags':
	process.stdout.write(quoteTokens(config.cflags).join(' '));
	break;
default:
	fail('usage: libffi-config.js <include-dirs|libraries|cflags>');
}
