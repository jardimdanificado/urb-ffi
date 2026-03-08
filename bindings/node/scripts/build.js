#!/usr/bin/env node
'use strict';

const { spawnSync } = require('child_process');
const path = require('path');

const bindingDir = path.resolve(__dirname, '..');
const cmd = process.platform === 'win32' ? 'node-gyp.cmd' : 'node-gyp';
const args = ['rebuild'];

function run(command, commandArgs) {
	return spawnSync(command, commandArgs, {
		cwd: bindingDir,
		stdio: 'inherit',
		env: process.env,
	});
}

let result = run(cmd, args);
if (result.error && result.error.code === 'ENOENT') {
	try {
		const nodeGypBin = require.resolve('node-gyp/bin/node-gyp.js');
		result = run(process.execPath, [nodeGypBin, ...args]);
	} catch {
		// ignore and report the original failure below
	}
}

if (result.status !== 0) {
	process.exit(result.status || 1);
}
