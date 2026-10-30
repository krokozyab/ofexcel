// The health check (SPEC.md §5.8): at start, every 30 seconds, on focus, and
// on becoming visible; a state change reported once.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { DaemonClient } from '../src/api/client';
import { E_OFFLINE } from '../src/api/errors';
import { HEALTH_INTERVAL_MS, HealthMonitor, type DaemonState, type Timer } from '../src/api/health';
import type { Health } from '../src/api/types';

const HEALTH: Health = {
	product: 'qexcel',
	version: '0.1.0-dev',
	api: { version: '1.0', min_client: '1.0' },
	profile: 'hosted',
	data_version: 0,
	catalog_version: '',
	time: '2026-10-30T12:00:00Z',
};

function manualTimer() {
	const timer = {
		interval: 0,
		tick: () => {},
		stopped: false,
		every(ms: number, callback: () => void) {
			timer.interval = ms;
			timer.tick = callback;
			return () => {
				timer.stopped = true;
			};
		},
	};
	return timer satisfies Timer;
}

/** A daemon that answers from `script`: a Health, or nothing (down). */
function daemon(script: Array<Health | 'down'>) {
	let requests = 0;
	const fetch = async () => {
		requests++;
		const next = script.length > 1 ? script.shift()! : script[0];
		if (next === 'down') {
			throw new TypeError('Load failed');
		}
		return new Response(JSON.stringify(next), { status: 200 });
	};
	return {
		client: new DaemonClient({ baseUrl: 'https://localhost:47321', fetch: fetch as typeof globalThis.fetch }),
		requests: () => requests,
	};
}

async function settle() {
	for (let i = 0; i < 5; i++) {
		await new Promise((resolve) => setImmediate(resolve));
	}
}

test('checks at start, every 30 s, on focus and when visible again; reports changes once', async () => {
	const { client, requests } = daemon([HEALTH, HEALTH, 'down', HEALTH, { ...HEALTH, version: '0.2.0' }]);
	const timer = manualTimer();
	const states: DaemonState[] = [];
	const monitor = new HealthMonitor(client, { onChange: (state) => states.push(state), timer });
	const window = new EventTarget();
	const document = Object.assign(new EventTarget(), { visibilityState: 'visible' }) as unknown as Document;

	monitor.start(window, document);
	await settle();
	assert.equal(timer.interval, HEALTH_INTERVAL_MS);
	assert.equal(requests(), 1);
	assert.deepEqual(
		states.map((state) => state.kind),
		['online'],
	);

	// The same answer again: nothing to report.
	timer.tick();
	await settle();
	assert.equal(requests(), 2);
	assert.equal(states.length, 1);

	window.dispatchEvent(new Event('focus'));
	await settle();
	assert.equal(requests(), 3);
	assert.equal(monitor.state.kind, 'offline');
	assert.equal(monitor.state.kind === 'offline' && monitor.state.error.code, E_OFFLINE);

	(document as unknown as { visibilityState: string }).visibilityState = 'hidden';
	document.dispatchEvent(new Event('visibilitychange'));
	await settle();
	assert.equal(requests(), 3);
	(document as unknown as { visibilityState: string }).visibilityState = 'visible';
	document.dispatchEvent(new Event('visibilitychange'));
	await settle();
	assert.equal(requests(), 4);

	timer.tick();
	await settle();
	assert.deepEqual(
		states.map((state) => (state.kind === 'online' ? `online ${state.health.version}` : state.kind)),
		['online 0.1.0-dev', 'offline', 'online 0.1.0-dev', 'online 0.2.0'],
	);

	monitor.stop();
	assert.ok(timer.stopped);
	window.dispatchEvent(new Event('focus'));
	await settle();
	assert.equal(requests(), 5);
});

test('a daemon that needs another add-in is incompatible, not online', async () => {
	const { client } = daemon([{ ...HEALTH, api: { version: '1.4', min_client: '1.2' } }]);
	const monitor = new HealthMonitor(client, { onChange: () => {}, timer: manualTimer() });
	assert.equal((await monitor.check()).kind, 'incompatible');
});

test('checks that overlap share one request', async () => {
	const { client, requests } = daemon([HEALTH]);
	const monitor = new HealthMonitor(client, { onChange: () => {}, timer: manualTimer() });
	await Promise.all([monitor.check(), monitor.check(), monitor.check()]);
	assert.equal(requests(), 1);
});
