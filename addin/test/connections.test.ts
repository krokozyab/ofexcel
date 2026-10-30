// Connections and signing in (TASKS.md D2): the list from /v1/status, adding a
// connection, a sign-in that runs in the background while the rest works, the
// warning 15 minutes before a sign-in runs out (DECISIONS.md R5).

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { DaemonClient } from '../src/api/client';
import type { ConnectionStatus } from '../src/api/types';
import {
	ConnectionsModel,
	elapsed,
	EXPIRY_WARNING_MS,
	expiryWarning,
	normalizeEndpoint,
	SIGN_IN_TIMEOUT_MS,
} from '../src/connections/model';

interface Call {
	method: string;
	path: string;
	body: unknown;
	signal: AbortSignal | undefined;
}

/** A daemon with connections, a sign-in the test finishes, and the rest. */
function fakeDaemon(timeoutMs?: number) {
	const calls: Call[] = [];
	const statuses: ConnectionStatus[] = [
		{ name: 'fusion_test', environment: 'TEST', host: 'fa.example.com', signed_in: false },
	];
	let finishSignIn: ((ok: boolean) => void) | undefined;
	const json = (status: number, body: unknown) => new Response(JSON.stringify(body), { status });
	const fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
		const path = String(input).replace('https://localhost:47321', '');
		const body = init?.body ? JSON.parse(String(init.body)) : undefined;
		calls.push({ method: init?.method ?? 'GET', path, body, signal: init?.signal ?? undefined });
		if (path === '/v1/status') {
			return json(200, { connections: statuses, active_runs: 0, warnings: [] });
		}
		if (path === '/v1/connections' && init?.method === 'POST') {
			if (!/^https:\/\/[^/]+$/.test(body.endpoint)) {
				return json(422, { error: { code: 'E_BAD_REQUEST', message: 'endpoint must be https://host[:port]' } });
			}
			statuses.push({ name: body.name, environment: body.environment, host: body.endpoint.slice(8), signed_in: false });
			return json(201, body);
		}
		if (path === '/v1/auth/login') {
			const ok = await new Promise<boolean>((resolve) => {
				finishSignIn = resolve;
			});
			if (!ok) {
				return json(409, {
					error: { code: 'E_FUSION_AUTH', message: 'Signing in to Oracle Fusion did not finish: closed' },
				});
			}
			const connection = statuses.find((item) => item.name === body.connection)!;
			Object.assign(connection, { signed_in: true, user: 'analyst@example.com', expires_in: 4 * 3600 });
			return json(200, { user: 'analyst@example.com' });
		}
		if (path === '/v1/auth/logout') {
			const connection = statuses.find((item) => item.name === body.connection)!;
			Object.assign(connection, { signed_in: false, user: undefined, expires_in: undefined });
			return new Response(null, { status: 204 });
		}
		if (path.endsWith('/test')) {
			return json(200, { ok: false, error: { code: 'E_FUSION_AUTH', message: 'Not signed in to Oracle Fusion' } });
		}
		return json(404, { error: { code: 'E_NOT_FOUND', message: 'No such route' } });
	};
	return {
		calls,
		statuses,
		client: new DaemonClient({
			baseUrl: 'https://localhost:47321',
			fetch: fetch as typeof globalThis.fetch,
			timeoutMs,
		}),
		finishSignIn: (ok: boolean) => finishSignIn?.(ok),
	};
}

async function settle() {
	for (let i = 0; i < 10; i++) {
		await new Promise((resolve) => setImmediate(resolve));
	}
}

test('the list comes from /v1/status, with each environment', async () => {
	const daemon = fakeDaemon();
	const model = new ConnectionsModel(daemon.client);
	assert.equal(model.ready, false);
	await model.refresh();
	assert.equal(model.ready, true);
	assert.deepEqual(
		model.connections.map((connection) => [connection.name, connection.environment, connection.signed_in]),
		[['fusion_test', 'TEST', false]],
	);
});

test('adding a connection: the address is cleaned up first, the daemon\'s refusal comes back', async () => {
	const daemon = fakeDaemon();
	const model = new ConnectionsModel(daemon.client);
	await model.create({
		name: 'fusion_prod',
		environment: 'PROD',
		endpoint: ' FA-Prod.Example.com/fscmUI/faces/FuseWelcome ',
		report_path: ' /Custom/Financials/RP_ARB.xdo ',
	});
	const posted = daemon.calls.find((call) => call.method === 'POST' && call.path === '/v1/connections')!;
	assert.deepEqual(posted.body, {
		name: 'fusion_prod',
		environment: 'PROD',
		endpoint: 'https://fa-prod.example.com',
		report_path: '/Custom/Financials/RP_ARB.xdo',
	});
	assert.deepEqual(
		model.connections.map((connection) => connection.name),
		['fusion_test', 'fusion_prod'],
	);

	await assert.rejects(
		model.create({ name: 'bad', environment: 'DEV', endpoint: 'http://x.example.com', report_path: '/r' }),
		{ code: 'E_BAD_REQUEST', message: 'endpoint must be https://host[:port]' },
	);
});

test('signing in runs in the background: the rest works meanwhile, the elapsed time shows', async () => {
	const daemon = fakeDaemon();
	let now = 1_000_000;
	const model = new ConnectionsModel(daemon.client, { now: () => now });
	await model.refresh();

	const signingIn = model.signIn('fusion_test');
	await settle();
	assert.equal(model.connections[0].signingInSince, 1_000_000);
	const login = daemon.calls.find((call) => call.path === '/v1/auth/login')!;
	assert.deepEqual(login.body, { connection: 'fusion_test' });

	// Meanwhile: the list reads, another connection is added, and a second
	// sign-in on the same connection does not open another window.
	now += 17_000;
	await model.refresh();
	await model.create({ name: 'fusion_dev', environment: 'DEV', endpoint: 'dev.example.com', report_path: '/r' });
	await model.signIn('fusion_test');
	assert.equal(daemon.calls.filter((call) => call.path === '/v1/auth/login').length, 1);
	assert.equal(model.connections[0].signingInSince, 1_000_000);
	assert.equal(elapsed(model.connections[0].signingInSince!, now), '0:17');

	daemon.finishSignIn(true);
	await signingIn;
	const signedIn = model.connections[0];
	assert.equal(signedIn.signingInSince, undefined);
	assert.equal(signedIn.signed_in, true);
	assert.equal(signedIn.user, 'analyst@example.com');
	assert.equal(signedIn.expiresAt, now + 4 * 3600 * 1000);
});

test('a sign-in that does not finish says why, and the connection can be signed in again', async () => {
	const daemon = fakeDaemon();
	const model = new ConnectionsModel(daemon.client);
	await model.refresh();
	const signingIn = model.signIn('fusion_test');
	await settle();
	daemon.finishSignIn(false);
	await signingIn;
	assert.equal(model.connections[0].signed_in, false);
	assert.equal(model.connections[0].problem, 'Signing in to Oracle Fusion did not finish: closed');

	const again = model.signIn('fusion_test');
	await settle();
	assert.equal(model.connections[0].problem, undefined);
	daemon.finishSignIn(true);
	await again;
	assert.equal(model.connections[0].signed_in, true);
});

test('the sign-in waits past the client\'s ordinary timeout', async () => {
	const daemon = fakeDaemon(20);
	const model = new ConnectionsModel(daemon.client);
	await model.refresh();
	const signingIn = model.signIn('fusion_test');
	// Four times the ordinary timeout: a request with it would have given up.
	await new Promise((resolve) => setTimeout(resolve, 80));
	const login = daemon.calls.find((call) => call.path === '/v1/auth/login')!;
	assert.equal(login.signal?.aborted, false);
	daemon.finishSignIn(true);
	await signingIn;
	assert.equal(model.connections[0].problem, undefined);
	assert.equal(model.connections[0].signed_in, true);
	assert.ok(SIGN_IN_TIMEOUT_MS > 300_000);
});

test('sign out and test', async () => {
	const daemon = fakeDaemon();
	const model = new ConnectionsModel(daemon.client);
	await model.refresh();
	const signingIn = model.signIn('fusion_test');
	await settle();
	daemon.finishSignIn(true);
	await signingIn;

	await model.signOut('fusion_test');
	assert.deepEqual(daemon.calls.at(-2)?.body, { connection: 'fusion_test' });
	assert.equal(model.connections[0].signed_in, false);
	assert.equal(model.connections[0].expiresAt, undefined);

	await model.test('fusion_test');
	assert.equal(daemon.calls.at(-2)?.path, '/v1/connections/fusion_test/test');
	assert.deepEqual(model.connections[0].tested, { ok: false, message: 'Not signed in to Oracle Fusion' });
	model.dismiss('fusion_test');
	assert.equal(model.connections[0].tested, undefined);
});

test('the warning 15 minutes before the sign-in runs out', () => {
	const now = 5_000_000;
	const connection = {
		name: 'fusion_test',
		environment: 'TEST' as const,
		host: 'fa.example.com',
		signed_in: true,
		expiresAt: now + EXPIRY_WARNING_MS + 1000,
	};
	assert.equal(expiryWarning(connection, now), '');
	assert.equal(
		expiryWarning({ ...connection, expiresAt: now + EXPIRY_WARNING_MS }, now),
		'The sign-in expires in 15 minutes. Sign in again to keep refreshing.',
	);
	assert.equal(
		expiryWarning({ ...connection, expiresAt: now + 20_000 }, now),
		'The sign-in expires in 1 minute. Sign in again to keep refreshing.',
	);
	// Gone already: the state says "not signed in" once the daemon says so.
	assert.equal(expiryWarning({ ...connection, expiresAt: now - 1 }, now), '');
	assert.equal(expiryWarning({ ...connection, signed_in: false, expiresAt: now + 60_000 }, now), '');
});

test('the Fusion address as the daemon wants it', () => {
	assert.equal(normalizeEndpoint('fa-x.example.com'), 'https://fa-x.example.com');
	assert.equal(normalizeEndpoint('HTTPS://FA-X.Example.COM:8443/'), 'https://fa-x.example.com:8443');
	assert.equal(normalizeEndpoint('https://fa-x.example.com/fscmUI/faces/FuseWelcome?x=1'), 'https://fa-x.example.com');
	// Not https: left for the daemon to refuse with its own words.
	assert.equal(normalizeEndpoint('http://fa-x.example.com'), 'http://fa-x.example.com');
	assert.equal(elapsed(0, 65_000), '1:05');
	assert.equal(elapsed(0, 720_000), '12:00');
});
