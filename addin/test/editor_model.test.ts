// The editor's model (TASKS.md D3): typing is checked, never previewed; Preview
// goes on request with values from constants and cells; the date hint of
// CONTRACTS.md §3.3; saving to a project and to the workbook.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { DaemonClient } from '../src/api/client';
import type { Preview, Validation } from '../src/api/types';
import { EditorModel, ORACLE_DATE_HINT, PREVIEW_ROWS, type SheetAccess, type WorkbookAccess } from '../src/editor/model';
import { emptyPayload, type WorkbookPayload } from '../src/workbook/payload';

const WORKBOOK = '0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d';

const TEXT = '-- @id: invoices\n-- @engine: oracle\n-- @version: 1\n-- @connection: dev\n' +
	'-- @param bu: integer\n-- @param since: date = 2026-01-01\nSELECT * FROM ap_invoices_all WHERE org_id = :bu ' +
	'AND invoice_date >= :since AND x = :status\n';

function validation(text: string): Validation {
	const id = /@id: (\w+)/.exec(text)?.[1] ?? '';
	const params = [...text.matchAll(/@param (\w+): ([\w<>(),]+)(?: = (\S+))?/g)].map((match) => ({
		name: match[1],
		type: match[2],
		...(match[3] ? { default: match[3] } : {}),
	}));
	return {
		ok: true,
		header: {
			id,
			hash: `hash-of-${id}`,
			version: 1,
			engine: text.includes('@engine: local') ? 'local' : 'oracle',
			params,
			columns: [],
			source: { kind: 'workbook', ref: '' },
			trusted: false,
		},
		params_in_text: [...new Set([...text.matchAll(/[:$](\w+)/g)].map((match) => match[1]))].filter(
			(name) => !['id', 'engine', 'version', 'connection', 'param'].includes(name),
		),
		errors: [],
		warnings: [],
	};
}

const PREVIEW: Preview = {
	columns: [
		{ name: 'INVOICE_ID', header: 'INVOICE_ID', type: 'id' },
		{ name: 'INVOICE_DATE', header: 'Invoice date', type: 'date' },
	],
	rows: [['101', '2026-01-02']],
	elapsed_ms: 1200,
	truncated: false,
	rendered_sql: 'SELECT …',
	warnings: [],
};

function fakeDaemon(options: { refuseWorkbook?: boolean } = {}) {
	const calls: Array<{ method: string; path: string; body: any }> = [];
	const fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
		const path = String(input).replace('https://localhost:47321', '');
		const body = init?.body ? JSON.parse(String(init.body)) : undefined;
		const method = init?.method ?? 'GET';
		calls.push({ method, path, body });
		const json = (status: number, value: unknown) => new Response(JSON.stringify(value), { status });
		if (path === '/v1/queries/validate') {
			return json(200, validation(body.text));
		}
		if (path === '/v1/preview') {
			return json(200, PREVIEW);
		}
		if (path.startsWith('/v1/queries/')) {
			return json(200, validation(body.text).header);
		}
		if (path.startsWith('/v1/workbooks/')) {
			if (options.refuseWorkbook) {
				return json(422, { error: { code: 'E_QUERY_PARSE', message: 'line 2: unknown directive @engnie' } });
			}
			return json(200, { queries: body.queries.map((query: { text: string }) => validation(query.text).header) });
		}
		if (path === '/v1/trust') {
			return json(200, { trusted: body.hashes });
		}
		return json(404, { error: { code: 'E_NOT_FOUND', message: 'No such route' } });
	};
	return { calls, client: new DaemonClient({ baseUrl: 'https://localhost:47321', fetch: fetch as typeof globalThis.fetch }) };
}

function fakeWorkbook(initial?: WorkbookPayload) {
	const state = { id: '', payload: initial, writes: 0 };
	const access: WorkbookAccess = {
		ensureId: async () => (state.id ||= WORKBOOK),
		read: async () => state.payload,
		write: async (payload) => {
			state.payload = payload;
			state.writes++;
		},
	};
	return { state, access };
}

function fakeSheet(cells: Record<string, unknown[][]>, date1904 = false): SheetAccess {
	return {
		read: async (ref) => cells[ref] ?? [[null]],
		selected: async () => 'Sheet1!C3',
		date1904: async () => date1904,
	};
}

test('typing is checked by the daemon and never previewed', async () => {
	const daemon = fakeDaemon();
	const model = new EditorModel(daemon.client, fakeWorkbook().access, fakeSheet({}));
	for (const text of [TEXT, TEXT + ' ', TEXT.replace(':status', ':state')]) {
		model.setText(text);
		await model.validate();
	}
	assert.ok(daemon.calls.every((call) => call.path === '/v1/queries/validate'));
	assert.deepEqual(model.undeclared, ['state']);
	assert.deepEqual(
		model.params.map((param) => param.name),
		['bu', 'since'],
	);

	model.declare({ name: 'state', type: 'string', defaultValue: 'OPEN' });
	assert.ok(model.text.includes('-- @param since: date = 2026-01-01\n-- @param state: string = OPEN\n'));
	assert.equal(daemon.calls.filter((call) => call.path === '/v1/preview').length, 0);
});

test('Preview on request: the text, the connection, values from constants and cells, 200 rows', async () => {
	const daemon = fakeDaemon();
	const model = new EditorModel(
		daemon.client,
		fakeWorkbook().access,
		fakeSheet({ 'Sheet1!B2': [[44562]] }, true),
	);
	model.setText(TEXT);
	await model.validate();
	model.setConnection('dev');
	model.setSource('bu', { kind: 'value', value: '204' });
	model.setSource('since', { kind: 'cell', ref: 'Sheet1!B2' });
	await model.runPreview();

	const preview = daemon.calls.find((call) => call.path === '/v1/preview')!;
	assert.deepEqual(preview.body, {
		text: TEXT,
		params: { bu: '204', since: '2026-01-02' },
		limit: PREVIEW_ROWS,
		connection: 'dev',
	});
	assert.equal(model.preview?.rows.length, 1);
	// §3.3: an Oracle query's date columns get the hint.
	assert.deepEqual(model.dateColumns, ['Invoice date']);
	assert.ok(ORACLE_DATE_HINT.includes('CAST(column AS TIMESTAMP)'));

	// An empty value is left out, so the default applies; the selected cell becomes the source.
	model.setSource('bu', { kind: 'value', value: '' });
	await model.useSelectedCell('since');
	assert.deepEqual(model.sources.get('since'), { kind: 'cell', ref: 'Sheet1!C3' });
	assert.deepEqual(await model.paramValues(), {});

	// No hint for a Local query.
	model.setEngine('local');
	assert.ok(!model.text.includes('@connection'));
	await model.validate();
	await model.runPreview();
	assert.deepEqual(model.dateColumns, []);
});

test('a cell that holds more than a scalar parameter takes says so, and nothing is sent', async () => {
	const daemon = fakeDaemon();
	const model = new EditorModel(daemon.client, fakeWorkbook().access, fakeSheet({ 'A1:A2': [[1], [2]] }));
	model.setText(TEXT);
	await model.validate();
	model.setSource('bu', { kind: 'cell', ref: 'A1:A2' });
	await model.runPreview();
	assert.match(model.previewError, /Parameter bu \(A1:A2\): a integer parameter takes one cell, not 2 × 1/);
	assert.equal(daemon.calls.filter((call) => call.path === '/v1/preview').length, 0);
});

test('saving to a project', async () => {
	const daemon = fakeDaemon();
	const model = new EditorModel(daemon.client, fakeWorkbook().access, fakeSheet({}));
	model.setText(TEXT);
	await model.saveToProject('finance');
	const put = daemon.calls.find((call) => call.method === 'PUT')!;
	assert.equal(put.path, '/v1/queries/invoices');
	assert.deepEqual(put.body, { project: 'finance', text: TEXT });
	assert.deepEqual(model.saved, { ok: true, message: 'Saved invoices to project finance' });
});

test('saving to the workbook: an id, every query registered, this one trusted, then written', async () => {
	const daemon = fakeDaemon();
	const existing = emptyPayload(WORKBOOK);
	existing.queries.push({ id: 'other', hash: 'old', text: '-- @id: other\n…', saved_at: 't' });
	existing.bindings.push({ binding_id: 'kept' });
	const workbook = fakeWorkbook(existing);
	const model = new EditorModel(daemon.client, workbook.access, fakeSheet({}));
	model.setText(TEXT);
	await model.saveToWorkbook('analyst@example.com');

	assert.deepEqual(model.saved, { ok: true, message: 'Saved invoices to this workbook' });
	const put = daemon.calls.find((call) => call.method === 'PUT')!;
	assert.equal(put.path, `/v1/workbooks/${WORKBOOK}/queries`);
	assert.deepEqual(put.body, { queries: [{ text: '-- @id: other\n…' }, { text: TEXT }] });
	const trust = daemon.calls.find((call) => call.path === '/v1/trust')!;
	assert.deepEqual(trust.body, { workbook_id: WORKBOOK, hashes: ['hash-of-invoices'] });

	const written = workbook.state.payload!;
	assert.equal(workbook.state.writes, 1);
	assert.deepEqual(
		written.queries.map((query) => [query.id, query.hash, query.saved_by]),
		[
			['other', 'hash-of-other', undefined],
			['invoices', 'hash-of-invoices', 'analyst@example.com'],
		],
	);
	assert.match(written.queries[1].saved_at, /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/);
	assert.deepEqual(written.bindings, [{ binding_id: 'kept' }]);
});

test('saving a query again takes its tables along to the new text (D9)', async () => {
	const daemon = fakeDaemon();
	const existing = emptyPayload(WORKBOOK);
	existing.queries.push(
		{ id: 'invoices', hash: 'hash-before-edit', text: '-- @id: invoices\n…', saved_at: 't' },
		{ id: 'other', hash: 'hash-of-other', text: '-- @id: other\n…', saved_at: 't' },
	);
	const binding = (id: string, query: { id: string; hash: string }, params: Record<string, unknown>) => ({
		binding_id: id,
		target: { kind: 'table', sheet: 'Sheet1', table: id },
		query,
		params,
		schema_policy: 'strict',
		query_columns: 2,
	});
	existing.bindings.push(
		binding('tblInvoices', { id: 'Invoices', hash: 'hash-before-edit' }, {
			bu: { value: '204' },
			since: { ref: 'Sheet1!B2' },
			dropped: { value: 'x' },
		}),
		binding('tblOther', { id: 'other', hash: 'hash-of-other' }, { a: { value: '1' } }),
	);
	const workbook = fakeWorkbook(existing);
	const model = new EditorModel(daemon.client, workbook.access, fakeSheet({}));
	model.setText(TEXT);
	await model.saveToWorkbook();

	const [invoices, other] = workbook.state.payload!.bindings as Array<{ query: unknown; params: unknown }>;
	assert.deepEqual(invoices.query, { id: 'Invoices', hash: 'hash-of-invoices' });
	assert.deepEqual(invoices.params, { bu: { value: '204' }, since: { ref: 'Sheet1!B2' } });
	// A query whose text did not change keeps its bindings as they were.
	assert.deepEqual(other.query, { id: 'other', hash: 'hash-of-other' });
	assert.deepEqual(other.params, { a: { value: '1' } });
});

test('a text the daemon refuses never reaches the workbook', async () => {
	const daemon = fakeDaemon({ refuseWorkbook: true });
	const workbook = fakeWorkbook();
	const model = new EditorModel(daemon.client, workbook.access, fakeSheet({}));
	model.setText(TEXT);
	await model.saveToWorkbook();
	assert.deepEqual(model.saved, { ok: false, message: 'line 2: unknown directive @engnie' });
	assert.equal(workbook.state.writes, 0);
	assert.equal(daemon.calls.filter((call) => call.path === '/v1/trust').length, 0);
});

test('a header that does not parse cannot be saved', async () => {
	const daemon = fakeDaemon();
	const model = new EditorModel(daemon.client, fakeWorkbook().access, fakeSheet({}));
	model.setText('SELECT 1');
	await model.saveToProject('finance');
	assert.equal(model.saved?.ok, false);
	assert.match(model.saved!.message, /cannot be saved until its header parses/);
	assert.equal(daemon.calls.filter((call) => call.method === 'PUT').length, 0);
});
