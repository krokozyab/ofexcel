// Smart Refresh and the words of a run (TASKS.md D6, SPEC.md §5.7, §7.5): the
// data the workbook's formulas miss, "Refresh missing" in dependency order,
// cancelling it; progress, truncation and history lines.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { DaemonClient } from '../src/api/client';
import type { MissingEntry, Run } from '../src/api/types';
import { describeEntry, MissingModel } from '../src/writer/missing';
import { describeFetching, describeRun, describeTruncation, describeWriting, formatElapsed } from '../src/writer/runs';

const WORKBOOK = '0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d';

function entry(id: string, connection: string, params: MissingEntry['params'] = {}): MissingEntry {
	return {
		query: { id, hash: `h-${id}` },
		connection,
		params,
		first_seen: '2026-10-31T08:00:00Z',
		last_seen: '2026-10-31T08:05:00Z',
		requests: 2,
	};
}

/** A daemon with `missing` entries; each run polls once, `fail` ids fail, a
 * succeeded run takes its entry off the list. */
function fakeDaemon(missing: MissingEntry[], fail: string[] = []) {
	const calls: Array<{ method: string; path: string; body: any }> = [];
	let left = [...missing];
	let current: { id: string; query: string; cancelled: boolean } | undefined;
	let count = 0;
	const fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
		const path = String(input).replace('https://localhost:47321', '');
		const body = init?.body ? JSON.parse(String(init.body)) : undefined;
		calls.push({ method: init?.method ?? 'GET', path, body });
		const json = (status: number, value: unknown) => new Response(JSON.stringify(value), { status });
		const run = (status: Run['status']): Run => ({
			run_id: current!.id,
			instance_key: 'k',
			query: { id: current!.query, hash: `h-${current!.query}` },
			connection: 'dev',
			params: {},
			status,
			started_at: '2026-10-31T09:00:00Z',
			rows_fetched: 10,
			truncated: false,
			depends_on: [],
			...(status === 'failed' ? { error: { code: 'E_FUSION_REJECTED', message: 'ORA-00942' } } : {}),
		});
		if (path === `/v1/missing?workbook=${WORKBOOK}`) {
			return json(200, { missing: left });
		}
		if (path === '/v1/runs') {
			current = { id: `run-${++count}`, query: body.query.id, cancelled: false };
			return json(202, run('running'));
		}
		if (path.endsWith('/cancel')) {
			current!.cancelled = true;
			return json(200, run('cancelled'));
		}
		if (path === `/v1/runs/${current?.id}`) {
			if (current!.cancelled) {
				return json(200, run('cancelled'));
			}
			if (fail.includes(current!.query)) {
				return json(200, run('failed'));
			}
			left = left.filter((item) => item.query.id !== current!.query);
			return json(200, run('succeeded'));
		}
		return json(404, { error: { code: 'E_NOT_FOUND', message: 'No such route' } });
	};
	return { calls, client: new DaemonClient({ baseUrl: 'https://localhost:47321', fetch: fetch as typeof globalThis.fetch }) };
}

test('the words of a run: rows, speed and time; truncation; a history line', () => {
	assert.equal(formatElapsed(7_400), '0:07');
	assert.equal(formatElapsed(192_000), '3:12');
	assert.equal(formatElapsed(3_725_000), '1:02:05');
	assert.equal(describeFetching(300, 500), 'Fetched 300 rows · 0:00');
	assert.equal(describeFetching(10_400, 2_000), 'Fetched 10,400 rows · 5,200 rows/s · 0:02');
	assert.equal(describeWriting(5_000, 100_000, 3_000), 'Writing 5,000 of 100,000 rows · 0:03');
	assert.equal(describeTruncation(500_000), 'Showing 500,000 of more than 500,000 rows (query limit)');
	const run: Run = {
		run_id: 'r1',
		instance_key: 'k',
		query: { id: 'invoices', hash: 'h' },
		connection: 'dev',
		environment: 'DEV',
		params: {},
		status: 'succeeded',
		started_at: '2026-10-31T09:00:00Z',
		finished_at: '2026-10-31T09:03:12Z',
		rows_fetched: 1_000_000,
		truncated: true,
		depends_on: [],
	};
	assert.equal(describeRun(run), '2026-10-31 09:00:00 UTC · succeeded · 1,000,000 rows · truncated · 3:12 · dev (DEV)');
	assert.equal(
		describeEntry(entry('ap_invoices', 'dev', { bu: '204', periods: ['Jan-26', 'Feb-26'] })),
		'ap_invoices (bu = 204; periods = Jan-26, Feb-26) on dev',
	);
	assert.equal(describeEntry(entry('totals', 'local')), 'totals');
});

test('Refresh missing: Oracle first, Local after with its missing dependencies; the list read again', async () => {
	const daemon = fakeDaemon([
		entry('totals', 'local', { bu: '204' }),
		entry('ap_invoices', 'dev', { bu: '204' }),
		entry('secret', 'dev'),
	]);
	const missing = new MissingModel(daemon.client, () => WORKBOOK, {
		sleep: async () => {},
		allows: (hash) => hash !== 'h-secret',
	});
	await missing.load();
	assert.equal(missing.entries.length, 3);
	assert.deepEqual(missing.runnable().map((item) => item.query.id), ['totals', 'ap_invoices']);

	await missing.refreshAll();
	const started = daemon.calls.filter((call) => call.path === '/v1/runs').map((call) => call.body);
	assert.deepEqual(started, [
		{ query: { id: 'ap_invoices', hash: 'h-ap_invoices' }, connection: 'dev', params: { bu: '204' }, workbook_id: WORKBOOK, deps: 'missing' },
		{ query: { id: 'totals', hash: 'h-totals' }, params: { bu: '204' }, workbook_id: WORKBOOK, deps: 'missing' },
	]);
	assert.deepEqual(missing.entries.map((item) => item.query.id), ['secret']);
	assert.equal(missing.busy, undefined);
	assert.deepEqual(missing.problems, []);
});

test('a run that fails is reported and the rest still run; Cancel stops the rest', async () => {
	const failing = fakeDaemon([entry('a', 'dev'), entry('b', 'dev')], ['a']);
	const model = new MissingModel(failing.client, () => WORKBOOK, { sleep: async () => {} });
	await model.load();
	await model.refreshAll();
	assert.deepEqual(model.problems, ['a on dev: ORA-00942']);
	assert.deepEqual(model.entries.map((item) => item.query.id), ['a']);

	const daemon = fakeDaemon([entry('a', 'dev'), entry('b', 'dev')]);
	let stopping: MissingModel | undefined;
	const stopped = new MissingModel(daemon.client, () => WORKBOOK, {
		sleep: async () => {
			await stopping?.cancel();
		},
	});
	stopping = stopped;
	await stopped.load();
	await stopped.refreshAll();
	assert.equal(daemon.calls.filter((call) => call.path === '/v1/runs').length, 1);
	assert.ok(daemon.calls.some((call) => call.path === '/v1/runs/run-1/cancel'));
	assert.deepEqual(stopped.problems, []);
	assert.equal(stopped.entries.length, 2);

	const none = new MissingModel(daemon.client, () => '', {});
	await none.load();
	assert.deepEqual(none.entries, []);
});
