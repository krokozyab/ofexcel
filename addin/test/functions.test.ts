// The OF functions (TASKS.md D8, CONTRACTS.md §10, SPEC.md §5.8): the batcher
// (quiet time, size limit, the same call once), the client cache and its
// reset when the versions move, serial dates in arguments, and what the
// runtime answers itself: OF.ASOF() and OF.STATUS().

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { DaemonClient } from '../src/api/client';
import { FormulaBatcher, type FormulaCall, type FormulaResult, type Timer } from '../src/functions/batcher';
import { convertDates, FormulaError, FormulaService } from '../src/functions/formulas';
import { formulaStatus, localSerial, workbookAsof } from '../src/functions/local';
import { cellError, implementations } from '../src/functions/register';
import type { Binding } from '../src/writer/bindings';

/** A timer run by hand: `fire()` runs what is due. */
function manualTimer() {
	let next: (() => void) | undefined;
	let sets = 0;
	const timer: Timer = {
		set: (callback) => {
			sets++;
			next = callback;
			return sets;
		},
		clear: () => {
			next = undefined;
		},
	};
	return {
		timer,
		fire: () => {
			const callback = next;
			next = undefined;
			callback?.();
		},
		get pending() {
			return next !== undefined;
		},
	};
}

const tick = () => new Promise((resolve) => setImmediate(resolve));

test('the batcher sends after the quiet time, the same call once, and at 2,000 calls at once', async () => {
	const sent: FormulaCall[][] = [];
	const send = async (calls: FormulaCall[]): Promise<FormulaResult[]> => {
		sent.push(calls);
		return calls.map((call) => ({ v: `${call.query}:${JSON.stringify(call.args)}` }));
	};
	const clock = manualTimer();
	const batcher = new FormulaBatcher(send, clock.timer, 20, 3);

	const a = batcher.call({ kind: 'table', query: 'ap', args: [[[1]]] });
	const b = batcher.call({ kind: 'table', query: 'AP', args: [[[1]]] });
	const c = batcher.call({ kind: 'call', query: 'ap', args: [[[1]]] });
	assert.equal(sent.length, 0);
	clock.fire();
	assert.deepEqual(await Promise.all([a, b, c]), [{ v: 'ap:[[[1]]]' }, { v: 'ap:[[[1]]]' }, { v: 'ap:[[[1]]]' }]);
	assert.equal(sent.length, 1);
	assert.deepEqual(sent[0].map((call) => call.kind), ['table', 'call']);

	// The size limit sends without waiting.
	const many = [1, 2, 3].map((n) => batcher.call({ kind: 'table', query: 'q', args: [[[n]]] }));
	await Promise.all(many);
	assert.equal(sent.length, 2);
	assert.equal(sent[1].length, 3);
	assert.ok(!clock.pending);

	const failing = new FormulaBatcher(async () => {
		throw new Error('ofexceld is not available');
	}, clock.timer);
	const failed = failing.call({ kind: 'table', query: 'q', args: [] });
	clock.fire();
	assert.deepEqual(await failed, { e: { code: 'notAvailable', msg: 'ofexceld is not available' } });
});

test('serial numbers become dates for date, timestamp and list parameters only', () => {
	const args = [[[46023]], [[46023.5]], [[46023, null], [46054, 'x']], [[46023]], [[46023]]];
	assert.deepEqual(convertDates(args, ['date', 'timestamp', 'list<date>', 'integer'], false), [
		[['2026-01-01']],
		[['2026-01-01 12:00:00']],
		[['2026-01-01', null], ['2026-02-01', 'x']],
		[[46023]],
		[[46023]],
	]);
	assert.deepEqual(convertDates([[[44561]]], ['date'], true), [[['2026-01-01']]]);
	assert.deepEqual(convertDates([[['2026-03-01']]], ['date'], false), [[['2026-03-01']]]);
});

/** A daemon answering /v1/queries and /v1/batch; its data_version can be moved. */
function fakeDaemon() {
	const calls: Array<{ path: string; body: any; workbook: string | null }> = [];
	const state: { dataVersion: number; fail: boolean; onBatch?: () => void } = { dataVersion: 1, fail: false };
	const fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
		const path = String(input).replace('https://localhost:47321', '');
		const body = init?.body ? JSON.parse(String(init.body)) : undefined;
		const headers = new Headers(init?.headers);
		calls.push({ path, body, workbook: headers.get('X-OF-Workbook') });
		const json = (value: unknown) =>
			new Response(JSON.stringify(value), {
				status: 200,
				headers: { 'X-OF-Data-Version': String(state.dataVersion), 'X-OF-Catalog-Version': 'c1' },
			});
		if (path.startsWith('/v1/queries')) {
			return json({
				queries: [
					{ id: 'ap_invoices', params: [{ name: 'bu', type: 'integer' }, { name: 'since', type: 'date' }] },
					{ id: 'AP_INVOICES', params: [{ name: 'other', type: 'string' }] },
				],
			});
		}
		if (path === '/v1/batch') {
			state.onBatch?.();
			return json({
				data_version: state.dataVersion,
				catalog_version: 'c1',
				results: body.calls.map((call: FormulaCall & { i: number }) =>
					state.fail || call.query === 'missing'
						? { i: call.i, e: { code: 'notAvailable', msg: `No data for ${call.query}: refresh it` } }
						: { i: call.i, v: [['ARGS'], [JSON.stringify(call.args)]] },
				),
			});
		}
		return new Response('{}', { status: 404 });
	};
	const client = new DaemonClient({
		baseUrl: 'https://localhost:47321',
		fetch: fetch as typeof globalThis.fetch,
		workbookId: () => '0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d',
	});
	return { calls, client, state };
}

/** A timer that runs the batch on the next turn, as the 20 ms would. */
const soon: Timer = { set: (callback) => setTimeout(callback, 0), clear: (handle) => clearTimeout(handle as never) };

test('formulas: dates converted by parameter type, the default connection and date system sent, results cached', async () => {
	const daemon = fakeDaemon();
	const service = new FormulaService(
		daemon.client,
		{ connection: () => 'dev', workbookId: () => '0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d', date1904: async () => false },
		soon,
	);

	const value = await service.evaluate('table', 'AP_Invoices', [[[204]], [[46023]]]);
	assert.deepEqual(value, [['ARGS'], ['[[[204]],[["2026-01-01"]]]']]);
	const batch = daemon.calls.find((call) => call.path === '/v1/batch')!;
	assert.deepEqual(batch.body, {
		calls: [{ i: 0, kind: 'table', query: 'AP_Invoices', args: [[[204]], [['2026-01-01']]] }],
		connection: 'dev',
		date_system: 1900,
	});
	assert.equal(batch.workbook, '0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d');
	assert.equal(daemon.calls[0].path, '/v1/queries?workbook=0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d');

	// Cached: no second batch, and the query list is read once.
	await service.evaluate('table', 'ap_invoices', [[[204]], [[46023]]]);
	assert.equal(daemon.calls.filter((call) => call.path === '/v1/batch').length, 1);
	assert.equal(daemon.calls.filter((call) => call.path.startsWith('/v1/queries')).length, 1);

	// The data moved: the cache and the query list go.
	daemon.state.dataVersion = 2;
	daemon.client.versions.observe(2, 'c1');
	await service.evaluate('table', 'ap_invoices', [[[204]], [[46023]]]);
	assert.equal(daemon.calls.filter((call) => call.path === '/v1/batch').length, 2);
	assert.equal(daemon.calls.filter((call) => call.path.startsWith('/v1/queries')).length, 2);

	// Errors are the cell's, and not cached.
	await assert.rejects(service.evaluate('table', 'missing', []), (error: unknown) => {
		assert.ok(error instanceof FormulaError);
		assert.equal(error.code, 'notAvailable');
		assert.equal(error.message, 'No data for missing: refresh it');
		return true;
	});
	await assert.rejects(service.evaluate('table', 'missing', []));
	assert.equal(daemon.calls.filter((call) => call.path === '/v1/batch').length, 4);

	await assert.rejects(service.evaluate('table', '', []), /Name the query/);
});

test('a result asked for before the versions moved is not cached', async () => {
	const daemon = fakeDaemon();
	const service = new FormulaService(
		daemon.client,
		{ connection: () => '', workbookId: () => '', date1904: async () => true },
		soon,
	);
	await service.evaluate('call', 'q', []);
	// A refresh ends while the batch is on its way: its answer may be the old data.
	daemon.state.onBatch = () => {
		daemon.state.onBatch = undefined;
		daemon.state.dataVersion = 5;
		daemon.client.versions.observe(5, 'c1');
	};
	await service.evaluate('call', 'q', [[[1]]]);
	await service.evaluate('call', 'q', [[[1]]]);
	const batches = daemon.calls.filter((call) => call.path === '/v1/batch');
	assert.equal(batches.length, 3);
	assert.equal(batches[0].body.date_system, 1904);
	assert.equal(batches[0].body.connection, undefined);
	assert.equal(daemon.calls[0].path, '/v1/queries');
});

function binding(at?: string, extra: Partial<Binding> = {}): Binding {
	return {
		binding_id: at ?? 'none',
		target: { kind: 'table', sheet: 'S', table: 'T' },
		query: { id: 'q', hash: 'h' },
		params: {},
		schema_policy: 'strict',
		query_columns: 1,
		...(at ? { last_success: { at, rows: 1, truncated: false, run_id: 'r' } } : {}),
		...extra,
	};
}

test('OF.ASOF() is the oldest refresh of the workbook, in local time; OF.STATUS() in its order', async () => {
	const at = Date.UTC(2026, 0, 1, 12, 0, 0);
	const offset = new Date(at).getTimezoneOffset() / (24 * 60);
	assert.ok(Math.abs(localSerial(at, false) - (46023.5 - offset)) < 1e-9);
	assert.ok(Math.abs(localSerial(at, true) - (44561.5 - offset)) < 1e-9);
	assert.equal(
		workbookAsof([binding('2026-01-02T00:00:00Z'), binding('2026-01-01T12:00:00Z'), binding()], false),
		localSerial(at, false),
	);
	assert.equal(workbookAsof([binding()], false), undefined);

	assert.equal(formulaStatus({ online: false, defaultSignedIn: false, stale: true }), 'OFFLINE');
	assert.equal(formulaStatus({ online: true, defaultSignedIn: false, stale: true }), 'NOT_SIGNED_IN');
	assert.equal(formulaStatus({ online: true, defaultSignedIn: true, stale: true }), 'STALE');
	assert.equal(formulaStatus({ online: true, defaultSignedIn: true, stale: false }), 'OK');

	const asked: Array<[string, unknown, unknown]> = [];
	const runtime = {
		service: {
			evaluate: async (kind: string, name: unknown, args: unknown) => {
				asked.push([kind, name, args]);
				return 1;
			},
		} as unknown as FormulaService,
		workbookAsof: async () => 46023.5,
		status: () => 'STALE' as const,
	};
	const functions = implementations(runtime);
	assert.equal(await functions.ASOF(null, null), 46023.5);
	assert.equal(await functions.ASOF('', []), 46023.5);
	assert.equal(await functions.ASOF('ap', [[[1]]]), 1);
	assert.equal(await functions.TABLE('ap', null), 1);
	assert.deepEqual(asked, [
		['asof', 'ap', [[[1]]]],
		['table', 'ap', []],
	]);
	assert.equal(functions.STATUS(), 'STALE');
	await assert.rejects(
		Promise.resolve(implementations({ ...runtime, workbookAsof: async () => undefined }).ASOF(null, null)),
		/No table of this workbook has been refreshed yet/,
	);
});

test('the error a cell shows: #NAME? is not a custom function result, so an unknown query is #VALUE!', () => {
	assert.deepEqual(cellError('invalidName'), { code: 'invalidValue', message: true });
	assert.deepEqual(cellError('invalidValue'), { code: 'invalidValue', message: true });
	assert.deepEqual(cellError('notAvailable'), { code: 'notAvailable', message: true });
	assert.deepEqual(cellError('invalidNumber'), { code: 'invalidNumber', message: false });
	assert.deepEqual(cellError('E_SOMETHING'), { code: 'notAvailable', message: true });
});
