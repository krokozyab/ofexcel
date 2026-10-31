// Tables bound to queries (TASKS.md D4, SPEC.md §5.4): loading into a new
// table, a refresh whose run fails leaving the table alone, a write that stops
// part way and is written again.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { DaemonClient } from '../src/api/client';
import type { SheetAccess, WorkbookAccess } from '../src/editor/model';
import { emptyPayload, type WorkbookPayload } from '../src/workbook/payload';
import type { Binding } from '../src/writer/bindings';
import { TablesModel } from '../src/writer/tables';
import type { CellValue } from '../src/writer/values';
import type { TableIO, TableTarget } from '../src/writer/writer';

const WORKBOOK = '0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d';
const TEXT = '-- @id: invoices\n-- @engine: oracle\n-- @version: 1\n-- @param since: date\n-- @param bu: integer\nSELECT …';

/** The daemon: runs that end as `outcome` after one poll, pages of `rows` rows. */
function fakeDaemon(script: { outcome?: 'succeeded' | 'failed'; rows?: number; gone?: boolean } = {}) {
	const calls: Array<{ method: string; path: string; body: any }> = [];
	let runs = 0;
	const fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
		const path = String(input).replace('https://localhost:47321', '');
		const body = init?.body ? JSON.parse(String(init.body)) : undefined;
		calls.push({ method: init?.method ?? 'GET', path, body });
		const json = (status: number, value: unknown) => new Response(JSON.stringify(value), { status });
		const run = (status: string) => ({
			run_id: `run-${runs}`,
			instance_key: 'k',
			query: { id: 'invoices', hash: 'h1' },
			connection: 'dev',
			status,
			rows_fetched: 3,
			truncated: false,
			...(status === 'failed' ? { error: { code: 'E_FUSION_REJECTED', message: 'Oracle Fusion rejected the query: ORA-00942' } } : {}),
		});
		if (path === '/v1/runs') {
			runs++;
			return json(202, run('running'));
		}
		if (path === '/v1/queries/validate') {
			return json(200, {
				ok: true,
				header: { params: [{ name: 'since', type: 'date' }, { name: 'bu', type: 'integer' }] },
				params_in_text: [],
				errors: [],
				warnings: [],
			});
		}
		const rows = /^\/v1\/runs\/(run-\d+)\/rows\?offset=(\d+)&limit=(\d+)$/.exec(path);
		if (rows) {
			if (script.gone) {
				return json(410, { error: { code: 'E_GONE', message: 'This version of the result is gone' } });
			}
			const total = script.rows ?? 3;
			const offset = Number(rows[2]);
			const count = Math.max(0, Math.min(Number(rows[3]), total - offset));
			return json(200, {
				columns: [
					{ name: 'ID', header: 'ID', type: 'id' },
					{ name: 'DAY', header: 'DAY', type: 'date' },
				],
				rows: Array.from({ length: count }, (_, index) => [`${offset + index}`, '2026-01-01']),
				offset,
				limit: Number(rows[3]),
				total,
				truncated: false,
			});
		}
		if (/^\/v1\/runs\/run-\d+$/.test(path)) {
			return json(200, run(script.outcome ?? 'succeeded'));
		}
		return json(404, { error: { code: 'E_NOT_FOUND', message: 'No such route' } });
	};
	return { calls, client: new DaemonClient({ baseUrl: 'https://localhost:47321', fetch: fetch as typeof globalThis.fetch }) };
}

function fakeWorkbook() {
	const payload: WorkbookPayload = emptyPayload(WORKBOOK);
	payload.queries.push({ id: 'invoices', hash: 'h1', text: TEXT, saved_at: 't' });
	const state = { payload, writes: 0 };
	const access: WorkbookAccess = {
		ensureId: async () => WORKBOOK,
		read: async () => state.payload,
		write: async (next) => {
			state.payload = next;
			state.writes++;
		},
	};
	return { state, access };
}

const sheet: SheetAccess = {
	read: async (ref) => (ref === 'Sheet1!B2' ? [[44562]] : [[null]]),
	selected: async () => 'Sheet1!A1',
	date1904: async () => true,
};

/** A table in memory that can be told to fail a write. */
class FakeTables implements TableIO {
	rows: CellValue[][] = [];
	headers: string[] = [];
	failWrites = false;
	name = '';
	async tableExists() {
		return false;
	}
	async create(name: string, headers: string[], _formats: string[], rows: number) {
		this.name = name;
		this.headers = headers;
		this.rows = Array.from({ length: rows }, () => headers.map(() => ''));
		return { sheet: 'Sheet1', table: name };
	}
	async inspect(target: TableTarget) {
		return target.table === this.name ? { headers: this.headers, rows: this.rows.length } : undefined;
	}
	async setQueryHeaders() {}
	async resize(_: TableTarget, rows: number) {
		this.rows = Array.from({ length: rows }, (_, index) => this.rows[index] ?? this.headers.map(() => ''));
	}
	async setFormats() {}
	async write(_: TableTarget, row: number, values: CellValue[][]) {
		if (this.failWrites) {
			throw new Error('GeneralException');
		}
		values.forEach((value, index) => (this.rows[row + index] = [...value]));
	}
	async clear() {}
}

function model(daemon = fakeDaemon(), tables = new FakeTables(), workbook = fakeWorkbook()) {
	const tablesModel = new TablesModel(daemon.client, workbook.access, sheet, tables, {
		sleep: async () => {},
		newId: () => 'b1',
	});
	return { daemon, tables, workbook, model: tablesModel };
}

test('Load to table: the run with values from cells, every row written, the binding saved', async () => {
	const { daemon, tables, workbook, model: tablesModel } = model();
	await tablesModel.loadToTable({
		query: { id: 'invoices', hash: 'h1' },
		connection: { name: 'dev', host: 'fa.example.com' },
		params: { since: { ref: 'Sheet1!B2' }, bu: { value: '204' } },
	});
	assert.equal(tablesModel.loading, undefined);

	const started = daemon.calls.find((call) => call.path === '/v1/runs')!;
	assert.deepEqual(started.body, {
		query: { id: 'invoices', hash: 'h1' },
		connection: 'dev',
		params: { since: '2026-01-02', bu: '204' },
		workbook_id: WORKBOOK,
	});
	assert.equal(tables.name, 'tblInvoices');
	assert.deepEqual(tables.rows, [
		['0', 44561],
		['1', 44561],
		['2', 44561],
	]);

	const saved = workbook.state.payload.bindings as Binding[];
	assert.equal(saved.length, 1);
	assert.deepEqual(saved[0].target, { kind: 'table', sheet: 'Sheet1', table: 'tblInvoices' });
	assert.equal(saved[0].query_columns, 2);
	assert.equal(saved[0].schema_policy, 'strict');
	// The binding keeps the cell, not its value: a refresh reads it again.
	assert.deepEqual(saved[0].params, { since: { ref: 'Sheet1!B2' }, bu: { value: '204' } });
	assert.equal(saved[0].last_success?.rows, 3);
	assert.equal(saved[0].last_success?.run_id, 'run-1');
	assert.deepEqual(tablesModel.bindings, saved);
});

test('a run that fails leaves the table alone, and a new load saves no binding', async () => {
	const loaded = model();
	await loaded.model.loadToTable({ query: { id: 'invoices', hash: 'h1' }, params: {} });
	const before = JSON.stringify(loaded.tables.rows);

	// The same table, refreshed by a run that fails.
	const failing = fakeDaemon({ outcome: 'failed' });
	const refreshed = new TablesModel(failing.client, loaded.workbook.access, sheet, loaded.tables, {
		sleep: async () => {},
	});
	await refreshed.load();
	await refreshed.refresh('b1');
	assert.equal(refreshed.states.get('b1')?.error, 'Oracle Fusion rejected the query: ORA-00942');
	assert.equal(JSON.stringify(loaded.tables.rows), before);
	assert.equal((loaded.workbook.state.payload.bindings as Binding[])[0].last_success?.run_id, 'run-1');

	const fresh = model(fakeDaemon({ outcome: 'failed' }));
	await fresh.model.loadToTable({ query: { id: 'invoices', hash: 'h1' }, params: {} });
	assert.equal(fresh.model.loading?.error, 'Oracle Fusion rejected the query: ORA-00942');
	assert.equal(fresh.workbook.state.writes, 0);
	assert.equal(fresh.tables.name, '');
});

test('a write that stops is saved as partial; Retry write reads the same run; a run gone says so', async () => {
	const { tables, workbook, model: tablesModel } = model();
	await tablesModel.loadToTable({ query: { id: 'invoices', hash: 'h1' }, params: {} });

	tables.failWrites = true;
	await tablesModel.refresh('b1');
	const partial = (workbook.state.payload.bindings as Binding[])[0];
	assert.deepEqual(partial.partial?.run_id, 'run-2');
	assert.match(tablesModel.states.get('b1')!.error!, /partially updated: GeneralException/);

	tables.failWrites = false;
	await tablesModel.retryWrite('b1');
	const written = (workbook.state.payload.bindings as Binding[])[0];
	assert.equal(written.partial, undefined);
	assert.equal(written.last_success?.run_id, 'run-2');
	assert.equal(tablesModel.states.get('b1')?.error, undefined);

	// The run of a stopped write, replaced since: E_GONE.
	const gone = new TablesModel(fakeDaemon({ gone: true }).client, workbook.access, sheet, tables, { sleep: async () => {} });
	await gone.load();
	await gone.retryWrite('b1');
	assert.match(gone.states.get('b1')!.error!, /That result is gone/);
});

test('a blocked binding does not run; choosing a connection saves it with its host', async () => {
	const { daemon, workbook, model: tablesModel } = model();
	await tablesModel.loadToTable({
		query: { id: 'invoices', hash: 'h1' },
		connection: { name: 'dev', host: 'fa-other.example.com' },
		params: {},
	});
	const runs = daemon.calls.filter((call) => call.path === '/v1/runs').length;

	tablesModel.setGuard((binding) => (binding.connection?.host === 'fa-dev.example.com' ? '' : 'Choose the connection'));
	await tablesModel.refresh('b1');
	assert.equal(tablesModel.states.get('b1')?.error, 'Choose the connection');
	assert.equal(daemon.calls.filter((call) => call.path === '/v1/runs').length, runs);

	await tablesModel.chooseConnection('b1', { name: 'dev', host: 'fa-dev.example.com' });
	assert.deepEqual((workbook.state.payload.bindings as Binding[])[0].connection, { name: 'dev', host: 'fa-dev.example.com' });
	assert.equal(tablesModel.blocked(tablesModel.bindings[0]), '');
	await tablesModel.refresh('b1');
	assert.equal(daemon.calls.filter((call) => call.path === '/v1/runs').length, runs + 1);
	assert.deepEqual(daemon.calls.filter((call) => call.path === '/v1/runs').at(-1)?.body.connection, 'dev');
});
