// The API client (TASKS.md D1): the headers it sends, the errors it reads,
// the versions it follows. A recorded fetch stands in for the daemon.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { compareApiVersions, DaemonClient, isCompatible } from '../src/api/client';
import { DaemonError, E_BAD_RESPONSE, E_OFFLINE, E_TIMEOUT } from '../src/api/errors';
import type { Health } from '../src/api/types';

interface Sent {
	url: string;
	method: string;
	headers: Record<string, string>;
	body: string | undefined;
}

/** A fetch that records each request and answers with `answer`. */
function recorder(answer: (sent: Sent) => Response | Promise<Response>) {
	const sent: Sent[] = [];
	const fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
		const request: Sent = {
			url: String(input),
			method: init?.method ?? 'GET',
			headers: { ...(init?.headers as Record<string, string>) },
			body: init?.body as string | undefined,
		};
		sent.push(request);
		return answer(request);
	};
	return { sent, fetch: fetch as typeof globalThis.fetch };
}

function json(status: number, body: unknown, headers: Record<string, string> = {}): Response {
	return new Response(JSON.stringify(body), {
		status,
		headers: { 'Content-Type': 'application/json; charset=utf-8', ...headers },
	});
}

const HEALTH: Health = {
	product: 'qexcel',
	version: '0.1.0-dev',
	api: { version: '1.0', min_client: '1.0' },
	profile: 'local',
	data_version: 7,
	catalog_version: 'abc',
	time: '2026-10-30T12:00:00Z',
};

test('local profile: session token on every request, workbook id when there is one', async () => {
	let workbook = '';
	const { sent, fetch } = recorder(() => json(200, { ok: true }));
	const client = new DaemonClient({
		baseUrl: 'https://localhost:47321/',
		sessionToken: 's3cr3t',
		workbookId: () => workbook,
		fetch,
	});

	await client.get('/v1/status');
	assert.equal(sent[0].url, 'https://localhost:47321/v1/status');
	assert.equal(sent[0].method, 'GET');
	assert.deepEqual(sent[0].headers, { 'X-OF-Session': 's3cr3t' });
	assert.equal(sent[0].body, undefined);

	workbook = '0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d';
	await client.post('/v1/runs', { query: { id: 'q', hash: 'h' } });
	assert.equal(sent[1].method, 'POST');
	assert.deepEqual(sent[1].headers, {
		'Content-Type': 'application/json',
		'X-OF-Session': 's3cr3t',
		'X-OF-Workbook': workbook,
	});
	assert.equal(sent[1].body, '{"query":{"id":"q","hash":"h"}}');
});

test('hosted profile: no session token', async () => {
	const { sent, fetch } = recorder(() => json(200, {}));
	const client = new DaemonClient({ baseUrl: 'https://127.0.0.1:47321', fetch });
	await client.put('/v1/queries/q', { project: 'p', text: 't' });
	await client.delete('/v1/datasets/k');
	assert.deepEqual(sent[0].headers, { 'Content-Type': 'application/json' });
	assert.equal(sent[1].method, 'DELETE');
	assert.deepEqual(sent[1].headers, {});
});

test('ErrorBody becomes a DaemonError with code, message, status and details', async () => {
	const { fetch } = recorder(() =>
		json(422, {
			error: { code: 'E_QUERY_PARSE', message: 'line 2: unknown directive @engnie', details: { errors: [1] } },
		}),
	);
	const client = new DaemonClient({ baseUrl: 'https://localhost:47321', fetch });
	await assert.rejects(client.post('/v1/queries/validate', { text: 'x' }), (error: unknown) => {
		assert.ok(error instanceof DaemonError);
		assert.equal(error.code, 'E_QUERY_PARSE');
		assert.equal(error.message, 'line 2: unknown directive @engnie');
		assert.equal(error.status, 422);
		assert.deepEqual(error.details, { errors: [1] });
		return true;
	});
});

test('an error without ErrorBody, a body that is not JSON, no answer, no answer in time', async () => {
	const answers: Array<() => Response | Promise<Response>> = [
		() => new Response('<html>Bad gateway</html>', { status: 502 }),
		() => new Response('not json', { status: 200 }),
		() => Promise.reject(new TypeError('Load failed')),
	];
	const { fetch } = recorder(() => answers.shift()!());
	const client = new DaemonClient({ baseUrl: 'https://localhost:47321', fetch });

	await assert.rejects(client.get('/v1/health'), { code: E_BAD_RESPONSE, status: 502 });
	await assert.rejects(client.get('/v1/health'), { code: E_BAD_RESPONSE, status: 200 });
	await assert.rejects(client.get('/v1/health'), (error: unknown) => {
		assert.ok(error instanceof DaemonError);
		assert.equal(error.code, E_OFFLINE);
		assert.equal(error.status, 0);
		assert.match(error.message, /https:\/\/localhost:47321/);
		assert.match(error.message, /ofexceld install-cert/);
		return true;
	});

	const hanging = (_input: RequestInfo | URL, init?: RequestInit) =>
		new Promise<Response>((_resolve, reject) => {
			init?.signal?.addEventListener('abort', () => reject(new DOMException('aborted', 'AbortError')));
		});
	const slow = new DaemonClient({ baseUrl: 'https://localhost:47321', fetch: hanging as typeof fetch });
	await assert.rejects(slow.get('/v1/health', { timeoutMs: 10 }), { code: E_TIMEOUT, status: 0 });
});

test('204 resolves with nothing', async () => {
	const { fetch } = recorder(() => new Response(null, { status: 204 }));
	const client = new DaemonClient({ baseUrl: 'https://localhost:47321', fetch });
	assert.equal(await client.delete('/v1/datasets/k'), undefined);
});

test('versions are followed from the headers of every response, errors included, and from /v1/health', async () => {
	const answers = [
		json(200, {}, { 'X-OF-Data-Version': '3', 'X-OF-Catalog-Version': 'c1' }),
		json(409, { error: { code: 'E_CONFLICT', message: 'busy' } }, { 'X-OF-Data-Version': '4' }),
		json(200, {}),
		json(200, HEALTH, { 'X-OF-Data-Version': '7', 'X-OF-Catalog-Version': 'abc' }),
	];
	const { fetch } = recorder(() => answers.shift()!);
	const client = new DaemonClient({ baseUrl: 'https://localhost:47321', fetch });
	const changes: string[] = [];
	client.versions.onChange((current, previous) =>
		changes.push(`${previous.data}/${previous.catalog} -> ${current.data}/${current.catalog}`),
	);

	await client.get('/v1/status');
	assert.deepEqual(client.versions.get(), { data: 3, catalog: 'c1' });
	await assert.rejects(client.delete('/v1/datasets/k'), { code: 'E_CONFLICT' });
	assert.deepEqual(client.versions.get(), { data: 4, catalog: 'c1' });
	// A response without the headers (as a hosted page sees one without
	// Access-Control-Expose-Headers) changes nothing.
	await client.get('/v1/status');
	assert.deepEqual(client.versions.get(), { data: 4, catalog: 'c1' });
	const health = await client.health();
	assert.equal(health.version, '0.1.0-dev');
	assert.deepEqual(client.versions.get(), { data: 7, catalog: 'abc' });
	assert.deepEqual(changes, ['3/c1 -> 4/c1', '4/c1 -> 7/abc']);
});

test('API versions: same major, not older than min_client', () => {
	assert.ok(compareApiVersions('1.10', '1.9') > 0);
	assert.equal(compareApiVersions('1.0', '1'), 0);
	assert.ok(isCompatible(HEALTH));
	assert.ok(isCompatible({ ...HEALTH, api: { version: '1.3', min_client: '1.0' } }));
	assert.ok(!isCompatible({ ...HEALTH, api: { version: '1.3', min_client: '1.2' } }));
	assert.ok(!isCompatible({ ...HEALTH, api: { version: '2.0', min_client: '1.0' } }));
});
