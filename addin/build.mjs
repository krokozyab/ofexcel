// Builds the add-in (TASKS.md D1): dist/ is what is served -- by ofexceld at
// /addin/ in the local profile (ofexcel_addin_dir, and built in by TASKS.md
// E1), or by a web host in the hosted one. build/manifest.local.xml is the
// manifest for the local profile on the default port, to sideload, and
// build/init.local.sql an init.sql that serves dist/ that way
// (`ofexceld serve --init addin/build/init.local.sql`).
//
//   node build.mjs [--base=https://localhost:47321/addin]

import * as esbuild from 'esbuild';
import { copyFileSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { deflateSync } from 'node:zlib';

const DEFAULT_BASE = 'https://localhost:47321/addin';
const base = (process.argv.find((arg) => arg.startsWith('--base=')) ?? `--base=${DEFAULT_BASE}`)
	.slice('--base='.length)
	.replace(/\/+$/, '');

const { version } = JSON.parse(readFileSync('package.json', 'utf8'));

rmSync('dist', { recursive: true, force: true });
mkdirSync('dist/assets', { recursive: true });
mkdirSync('build', { recursive: true });

// One script for the shared runtime: the task pane now, the custom functions
// later (TASKS.md D8). Office's WebViews are Safari on the Mac and Chromium
// (WebView2) on Windows.
await esbuild.build({
	entryPoints: ['src/taskpane/main.ts'],
	bundle: true,
	format: 'iife',
	target: ['es2020', 'safari14', 'chrome90'],
	outfile: 'dist/taskpane.js',
	sourcemap: 'linked',
	legalComments: 'none',
	logLevel: 'warning',
});
copyFileSync('src/taskpane.html', 'dist/taskpane.html');
copyFileSync('functions.json', 'dist/functions.json');

for (const size of [16, 32, 80]) {
	writeFileSync(`dist/assets/icon-${size}.png`, squarePng(size, [0x21, 0x73, 0x46]));
}

writeFileSync(
	'build/manifest.local.xml',
	readFileSync('manifest.template.xml', 'utf8')
		.replaceAll('{{BASE_URL}}', base)
		.replaceAll('{{VERSION}}', officeVersion(version)),
);
writeFileSync(
	'build/init.local.sql',
	`SET ofexcel_profile = 'local';\nSET ofexcel_addin_dir = '${resolve('dist').replaceAll("'", "''")}';\n`,
);
console.log(`built dist/, build/manifest.local.xml for ${base} and build/init.local.sql`);

// "0.1.0-dev" as Office wants a version: four numbers, "0.1.0.0".
function officeVersion(text) {
	const numbers = text.split('-')[0].split('.').map((part) => Number.parseInt(part, 10) || 0);
	while (numbers.length < 4) {
		numbers.push(0);
	}
	return numbers.slice(0, 4).join('.');
}

// A square PNG of one colour: the ribbon icons until there is a real one.
function squarePng(size, rgb) {
	const row = Buffer.alloc(1 + size * 3);
	for (let x = 0; x < size; x++) {
		row.set(rgb, 1 + x * 3);
	}
	const pixels = Buffer.concat(Array.from({ length: size }, () => row));
	const header = Buffer.alloc(13);
	header.writeUInt32BE(size, 0);
	header.writeUInt32BE(size, 4);
	header.set([8, 2, 0, 0, 0], 8); // 8-bit RGB, no interlace
	return Buffer.concat([
		Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
		chunk('IHDR', header),
		chunk('IDAT', deflateSync(pixels)),
		chunk('IEND', Buffer.alloc(0)),
	]);
}

function chunk(type, data) {
	const length = Buffer.alloc(4);
	length.writeUInt32BE(data.length);
	const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
	const crc = Buffer.alloc(4);
	crc.writeUInt32BE(crc32(body));
	return Buffer.concat([length, body, crc]);
}

function crc32(bytes) {
	let crc = 0xffffffff;
	for (const byte of bytes) {
		crc ^= byte;
		for (let bit = 0; bit < 8; bit++) {
			crc = crc & 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
		}
	}
	return (crc ^ 0xffffffff) >>> 0;
}
