#!/usr/bin/env node
'use strict';

const { spawnSync } = require('child_process');
const path = require('path');

const rootDir = path.resolve(__dirname, '..', '..', '..');
const bindingDir = path.resolve(__dirname, '..');
const buildScript = path.join(bindingDir, 'scripts', 'build.js');
const node = process.execPath;
const compiler = process.env.CC || 'cc';
const byvalueSource = path.join(bindingDir, 'examples', 'byvalue.c');
const byvalueLib = path.join(
	bindingDir,
	'examples',
	`libbyvalue${process.platform === 'darwin' ? '.dylib' : '.so'}`,
);
const examples = process.platform === 'win32'
	? ['smoke.js']
	: ['hello.js', 'manual_types.js', 'memory.js', 'view.js', 'memory_phase6.js', 'callback.js', 'meta_fields.js', 'memory_utils.js', 'sym_self.js', 'byvalue.js'];

function run(args, cwd = rootDir) {
	const result = spawnSync(node, args, {
		cwd,
		stdio: 'inherit',
		env: process.env,
	});
	if (result.status !== 0) {
		process.exit(result.status || 1);
	}
}

function runCommand(command, args, cwd = rootDir) {
	const result = spawnSync(command, args, {
		cwd,
		stdio: 'inherit',
		env: process.env,
	});
	if (result.status !== 0) {
		process.exit(result.status || 1);
	}
}

run([buildScript]);
if (process.platform !== 'win32') {
	const compileArgs = process.platform === 'darwin'
		? ['-dynamiclib', '-fPIC', '-pthread', '-o', byvalueLib, byvalueSource]
		: ['-shared', '-fPIC', '-pthread', '-o', byvalueLib, byvalueSource];
	runCommand(compiler, compileArgs);
}
for (const example of examples) {
	console.log(`=== ${example.replace(/\.js$/, '')} ===`);
	run([path.join(bindingDir, 'examples', example)]);
}
