// Development server for the QExcel add-in probe (phase 0 only).
//
// HTTPS on https://localhost:3000 with the Office development certificate
// (`npx office-addin-dev-certs install`), static files from ./public, and the
// few dynamic endpoints the probe needs:
//
//   GET  /functions.json          custom function metadata, variant 1 or 2
//   GET  /state                   {"metadata_variant": n}
//   POST /meta-variant            {"variant": n} -- switches /functions.json
//   POST /report?name=<session>   saves the probe's report to ./reports/<session>.json
//   GET  /assets/icon-<n>.png     a generated n x n icon
//   *    /daemon/<path>           proxied to ofexceld on http://127.0.0.1:47321/<path>
//
// The proxy exists because Excel for Mac's WebView refuses fetch() from this
// HTTPS page to http://127.0.0.1 outright (phase 0, first probe run). Behind it
// the daemon is same-origin HTTPS, which is the local profile's shape; the probe
// uses it to measure writes when the direct route is blocked.
//
// Everything is served with Cache-Control: no-store, so that what the probe
// measures about metadata caching is Excel's own cache, not HTTP's.
//
// No dependencies: node server.mjs

import fs from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';

const ROOT = path.dirname(fileURLToPath(import.meta.url));
const PUBLIC = path.join(ROOT, 'public');
const STATE_DIR = path.join(ROOT, 'state');
const REPORTS = path.join(ROOT, 'reports');
const STATE_FILE = path.join(STATE_DIR, 'state.json');
const CERT_DIR = path.join(os.homedir(), '.office-addin-dev-certs');
const PORT = Number(process.env.PROBE_PORT || 3000);

const CONTENT_TYPES = {
	'.html': 'text/html; charset=utf-8',
	'.js': 'text/javascript; charset=utf-8',
	'.css': 'text/css; charset=utf-8',
	'.json': 'application/json; charset=utf-8',
	'.xml': 'application/xml; charset=utf-8',
};

function readState() {
	try {
		return JSON.parse(fs.readFileSync(STATE_FILE, 'utf8'));
	} catch {
		return { metadata_variant: 1 };
	}
}

function writeState(state) {
	fs.mkdirSync(STATE_DIR, { recursive: true });
	fs.writeFileSync(STATE_FILE, JSON.stringify(state, null, 2));
}

//! Variant 2 adds PROBE.NEW. Whether it shows up after reloading the add-in, or
//! only after restarting Excel, is what phase 0 check 9 asks.
function metadata(variant) {
	const functions = [
		{
			id: 'ARGS',
			name: 'ARGS',
			description: 'Describes how each argument arrived: type, null, empty string, matrix shape',
			parameters: [{ name: 'args', type: 'any', dimensionality: 'matrix', repeating: true, optional: true }],
			result: { type: 'string', dimensionality: 'scalar' },
		},
		{
			id: 'SCALARARGS',
			name: 'SCALARARGS',
			description: 'Describes two optional scalar arguments',
			parameters: [
				{ name: 'a', type: 'any', optional: true },
				{ name: 'b', type: 'any', optional: true },
			],
			result: { type: 'string' },
		},
		{
			id: 'META',
			name: 'META',
			description: 'The metadata variant the runtime fetched when it started',
			parameters: [],
			result: { type: 'number' },
		},
		{
			id: 'LATE',
			name: 'LATE',
			description: 'Associated 2 s after the runtime starts, after an await',
			parameters: [],
			result: { type: 'string' },
		},
		{
			id: 'MATRIX',
			name: 'MATRIX',
			description: 'A rows x cols matrix of numbers',
			parameters: [
				{ name: 'rows', type: 'number' },
				{ name: 'cols', type: 'number' },
			],
			result: { type: 'any', dimensionality: 'matrix' },
		},
	];
	if (variant >= 2) {
		functions.push({
			id: 'NEW',
			name: 'NEW',
			description: 'Present only in metadata variant 2',
			parameters: [],
			result: { type: 'number' },
		});
	}
	return { functions };
}

//! A solid square PNG, so the repository carries no binary icons.
function iconPng(size) {
	const row = 1 + size * 4;
	const raw = Buffer.alloc(row * size);
	for (let y = 0; y < size; y++) {
		raw[y * row] = 0; // filter: none
		for (let x = 0; x < size; x++) {
			const at = y * row + 1 + x * 4;
			const border = x < size / 8 || y < size / 8 || x >= size - size / 8 || y >= size - size / 8;
			raw[at] = border ? 0x10 : 0x21;
			raw[at + 1] = border ? 0x5c : 0x73;
			raw[at + 2] = border ? 0x37 : 0x46;
			raw[at + 3] = 0xff;
		}
	}
	const chunk = (type, data) => {
		const length = Buffer.alloc(4);
		length.writeUInt32BE(data.length);
		const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
		const crc = Buffer.alloc(4);
		crc.writeUInt32BE(zlib.crc32(body));
		return Buffer.concat([length, body, crc]);
	};
	const header = Buffer.alloc(13);
	header.writeUInt32BE(size, 0);
	header.writeUInt32BE(size, 4);
	header[8] = 8; // bit depth
	header[9] = 6; // RGBA
	return Buffer.concat([
		Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
		chunk('IHDR', header),
		chunk('IDAT', zlib.deflateSync(raw)),
		chunk('IEND', Buffer.alloc(0)),
	]);
}

function send(response, status, contentType, body) {
	response.writeHead(status, { 'Content-Type': contentType, 'Cache-Control': 'no-store' });
	response.end(body);
}

function sendJson(response, status, value) {
	send(response, status, CONTENT_TYPES['.json'], JSON.stringify(value));
}

function readBody(request, limit = 8 * 1024 * 1024) {
	return new Promise((resolve, reject) => {
		const chunks = [];
		let size = 0;
		request.on('data', (chunk) => {
			size += chunk.length;
			if (size > limit) {
				reject(new Error('body too large'));
				request.destroy();
				return;
			}
			chunks.push(chunk);
		});
		request.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
		request.on('error', reject);
	});
}

const DAEMON_PORT = Number(process.env.PROBE_DAEMON_PORT || 47321);

//! Forwards to the daemon as the hosted add-in would reach it, with the Origin
//! the daemon expects; a same-origin GET from the page carries none.
function proxyToDaemon(request, response, url) {
	const headers = { Origin: `https://localhost:${PORT}`, Host: `127.0.0.1:${DAEMON_PORT}` };
	for (const name of ['content-type', 'x-of-workbook', 'x-of-session']) {
		if (request.headers[name]) {
			headers[name] = request.headers[name];
		}
	}
	const upstream = http.request(
		{ host: '127.0.0.1', port: DAEMON_PORT, method: request.method, path: url.pathname.slice('/daemon'.length) + url.search, headers },
		(daemonResponse) => {
			response.writeHead(daemonResponse.statusCode, Object.assign({}, daemonResponse.headers, { 'Cache-Control': 'no-store' }));
			daemonResponse.pipe(response);
		},
	);
	upstream.on('error', (error) => sendJson(response, 502, { error: `daemon unreachable: ${error.message}` }));
	request.pipe(upstream);
}

async function handle(request, response) {
	const url = new URL(request.url, `https://localhost:${PORT}`);
	const log = `${new Date().toISOString()} ${request.method} ${url.pathname}${url.search}`;

	if (url.pathname.startsWith('/daemon/')) {
		return proxyToDaemon(request, response, url);
	}

	if (request.method === 'GET' && url.pathname === '/functions.json') {
		const variant = readState().metadata_variant;
		console.log(`${log} -> metadata variant ${variant}`);
		return sendJson(response, 200, metadata(variant));
	}
	if (request.method === 'GET' && url.pathname === '/state') {
		return sendJson(response, 200, readState());
	}
	if (request.method === 'POST' && url.pathname === '/meta-variant') {
		const { variant } = JSON.parse(await readBody(request));
		if (![1, 2].includes(variant)) {
			return sendJson(response, 400, { error: 'variant must be 1 or 2' });
		}
		writeState({ ...readState(), metadata_variant: variant });
		console.log(`${log} -> metadata variant now ${variant}`);
		return sendJson(response, 200, readState());
	}
	if (request.method === 'POST' && url.pathname === '/report') {
		const name = url.searchParams.get('name') || '';
		if (!/^[a-z0-9-]{1,80}$/.test(name)) {
			return sendJson(response, 400, { error: 'name must be [a-z0-9-]' });
		}
		const body = await readBody(request);
		JSON.parse(body); // refuse anything that is not JSON
		fs.mkdirSync(REPORTS, { recursive: true });
		const file = path.join(REPORTS, `${name}.json`);
		fs.writeFileSync(file, body);
		console.log(`${log} -> saved ${file}`);
		return sendJson(response, 200, { saved: file });
	}
	const icon = url.pathname.match(/^\/assets\/icon-(16|32|64|80|128)\.png$/);
	if (request.method === 'GET' && icon) {
		return send(response, 200, 'image/png', iconPng(Number(icon[1])));
	}
	if (request.method === 'GET') {
		const relative = url.pathname === '/' ? 'taskpane.html' : url.pathname.slice(1);
		const file = path.join(PUBLIC, path.normalize(relative));
		if (!file.startsWith(PUBLIC + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
			console.log(`${log} -> 404`);
			return send(response, 404, 'text/plain; charset=utf-8', 'not found');
		}
		console.log(log);
		return send(response, 200, CONTENT_TYPES[path.extname(file)] || 'application/octet-stream', fs.readFileSync(file));
	}
	return send(response, 405, 'text/plain; charset=utf-8', 'method not allowed');
}

function main() {
	const key = path.join(CERT_DIR, 'localhost.key');
	const cert = path.join(CERT_DIR, 'localhost.crt');
	if (!fs.existsSync(key) || !fs.existsSync(cert)) {
		console.error(`No development certificate in ${CERT_DIR}. Run: npx office-addin-dev-certs install`);
		process.exit(1);
	}
	const server = https.createServer({ key: fs.readFileSync(key), cert: fs.readFileSync(cert) }, (request, response) => {
		handle(request, response).catch((error) => {
			console.error(error);
			if (!response.headersSent) {
				sendJson(response, 500, { error: String(error) });
			}
		});
	});
	server.listen(PORT, '127.0.0.1', () => {
		console.log(`QExcel add-in probe on https://localhost:${PORT} (metadata variant ${readState().metadata_variant})`);
	});

	// A cross-origin HTTPS endpoint on loopback, standing in for a daemon that
	// speaks HTTPS with a trusted certificate: does the WebView allow that, when
	// it refuses http://127.0.0.1?
	const origin = `https://localhost:${PORT}`;
	const crossOrigin = https.createServer({ key: fs.readFileSync(key), cert: fs.readFileSync(cert) }, (request, response) => {
		const cors = {
			'Access-Control-Allow-Origin': origin,
			'Access-Control-Allow-Headers': 'Content-Type, X-OF-Workbook',
			'Access-Control-Allow-Private-Network': 'true',
			Vary: 'Origin',
			'Cache-Control': 'no-store',
		};
		console.log(`${new Date().toISOString()} [cross-origin :${PORT + 1}] ${request.method} ${request.url} Origin=${request.headers.origin}`);
		if (request.method === 'OPTIONS') {
			response.writeHead(204, cors);
			return response.end();
		}
		response.writeHead(200, Object.assign({ 'Content-Type': CONTENT_TYPES['.json'] }, cors));
		response.end(JSON.stringify({ ok: true, origin: request.headers.origin || null }));
	});
	crossOrigin.listen(PORT + 1, '127.0.0.1', () => console.log(`cross-origin HTTPS test endpoint on https://localhost:${PORT + 1}`));
}

main();
