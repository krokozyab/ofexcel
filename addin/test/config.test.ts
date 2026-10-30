// Where the add-in looks for the daemon, and what its status bar says.

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { DaemonError, E_OFFLINE } from '../src/api/errors';
import { DEFAULT_DAEMON_URL, locateDaemon, type MetaSource } from '../src/config';
import { describeStatus } from '../src/taskpane/status';

function page(metas: Record<string, string>): MetaSource {
	return {
		querySelector(selector: string) {
			const name = /meta\[name="([^"]+)"\]/.exec(selector)?.[1] ?? '';
			return name in metas ? { getAttribute: () => metas[name] } : null;
		},
	};
}

test('local profile: the page carries the session token, the API is on its origin', () => {
	assert.deepEqual(locateDaemon(page({ 'qexcel-session': 'ab12' }), 'https://localhost:47999'), {
		profile: 'local',
		baseUrl: 'https://localhost:47999',
		sessionToken: 'ab12',
	});
});

test('hosted profile: the address the page names, or the default', () => {
	assert.deepEqual(locateDaemon(page({}), 'https://intranet.example.com'), {
		profile: 'hosted',
		baseUrl: DEFAULT_DAEMON_URL,
		sessionToken: '',
	});
	assert.equal(
		locateDaemon(page({ 'qexcel-daemon': 'https://127.0.0.1:47400/' }), 'https://intranet.example.com').baseUrl,
		'https://127.0.0.1:47400',
	);
});

test('the status bar: connecting, online, incompatible, offline', () => {
	const location = locateDaemon(page({}), 'https://intranet.example.com');
	const health = {
		product: 'qexcel',
		version: '0.1.0-dev',
		api: { version: '1.0', min_client: '1.0' },
		profile: 'hosted' as const,
		data_version: 0,
		catalog_version: '',
		time: '',
	};
	assert.equal(describeStatus({ kind: 'connecting' }, location).tone, 'pending');
	assert.deepEqual(describeStatus({ kind: 'online', health }, location), {
		text: 'ofexceld 0.1.0-dev · hosted',
		tone: 'ok',
		detail: 'https://localhost:47321 · API 1.0',
	});
	assert.equal(describeStatus({ kind: 'incompatible', health }, location).tone, 'warning');
	const offline = describeStatus(
		{ kind: 'offline', error: new DaemonError(E_OFFLINE, 'The QExcel daemon does not answer', 0) },
		location,
	);
	assert.deepEqual(offline, {
		text: 'ofexceld is not available',
		tone: 'error',
		detail: 'The QExcel daemon does not answer',
	});
});

test('functions.json is the one the daemon serves (CONTRACTS.md §10.1)', () => {
	const addin = JSON.parse(readFileSync('functions.json', 'utf8'));
	const source = readFileSync('../src/batch/formulas.cpp', 'utf8');
	const daemon = /CORE_FUNCTIONS\[\] = R"QEXCEL\(([\s\S]*?)\)QEXCEL"/.exec(source);
	assert.ok(daemon, 'CORE_FUNCTIONS not found in src/batch/formulas.cpp');
	assert.deepEqual(addin, JSON.parse(daemon[1]));
	assert.deepEqual(
		addin.functions.map((f: { id: string }) => f.id),
		['TABLE', 'CALL', 'ASOF', 'STATUS'],
	);
});
