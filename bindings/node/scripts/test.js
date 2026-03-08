#!/usr/bin/env node
'use strict';

const { spawnSync } = require('child_process');
const path = require('path');

const rootDir = path.resolve(__dirname, '..', '..', '..');
const bindingDir = path.resolve(__dirname, '..');
const buildScript = path.join(bindingDir, 'scripts', 'build.js');
const node = process.execPath;
const examples = process.platform === 'win32'
	? ['smoke.js']
	: ['hello.js', 'memory.js', 'view.js', 'callback.js', 'meta_fields.js', 'memory_utils.js', 'sym_self.js'];

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

run([buildScript]);
for (const example of examples) {
	console.log(`=== ${example.replace(/\.js$/, '')} ===`);
	run([path.join(bindingDir, 'examples', example)]);
}
