// Runs the add-in's unit tests (TASKS.md D1): each test/*.test.ts is bundled
// by esbuild for Node and run by `node --test`. No Excel, no daemon.

import * as esbuild from 'esbuild';
import { spawnSync } from 'node:child_process';
import { readdirSync, rmSync } from 'node:fs';

const tests = readdirSync('test')
	.filter((name) => name.endsWith('.test.ts'))
	.sort();

rmSync('build/test', { recursive: true, force: true });
await esbuild.build({
	entryPoints: tests.map((name) => `test/${name}`),
	bundle: true,
	platform: 'node',
	format: 'esm',
	target: 'node20',
	outdir: 'build/test',
	outExtension: { '.js': '.mjs' },
	logLevel: 'warning',
});

const result = spawnSync(
	process.execPath,
	['--test', ...tests.map((name) => `build/test/${name.replace(/\.ts$/, '.mjs')}`)],
	{ stdio: 'inherit' },
);
process.exit(result.status ?? 1);
