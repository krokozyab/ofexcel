// Importing a sheet's table (TASKS.md D7, CONTRACTS.md §12): the name, the
// column types guessed by §12's rules, the cells' encoding, the chunks sent,
// importing again by hand and before a run of a Local query that reads it.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { DaemonClient } from '../src/api/client';
import type { WorkbookAccess } from '../src/editor/model';
import type { ImportSource, SourceRef, SourceShape } from '../src/importer/excel_source';
import { ImportsModel } from '../src/importer/imports';
import {
	cellValue,
	ColumnGuess,
	columnNames,
	importName,
	looksLikeDateFormat,
	type SourceCell,
} from '../src/importer/infer';
import { dependsOf, importsOf, importsReadBy } from '../src/importer/workbook_imports';
import { emptyPayload, type WorkbookPayload } from '../src/workbook/payload';

const WORKBOOK = '0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d';

function num(value: number, text = String(value), format = 'General'): SourceCell {
	return { value, text, format, kind: 'Double' };
}
function str(value: string): SourceCell {
	return { value, text: value, format: 'General', kind: 'String' };
}
const EMPTY: SourceCell = { value: '', text: '', format: 'General', kind: 'Empty' };
const ERROR: SourceCell = { value: '#N/A', text: '#N/A', format: 'General', kind: 'Error' };
const bool = (value: boolean): SourceCell => ({ value, text: value ? 'TRUE' : 'FALSE', format: 'General', kind: 'Boolean' });

function guess(...cells: SourceCell[]): string {
	const column = new ColumnGuess();
	cells.forEach((cell) => column.add(cell));
	return column.type();
}

test('the import name from a table name (§12 item 2)', () => {
	assert.equal(importName('Bank Statement 2026'), 'bank_statement_2026');
	assert.equal(importName('tblBank'), 'tblbank');
	assert.equal(importName('2026 rates'), 't_2026_rates');
	assert.equal(importName('_raw'), 't__raw');
	assert.equal(importName('Äpfel-Liste'), 't__pfel_liste');
	assert.equal(importName('x'.repeat(80)).length, 63);
});

test('column names: empty headers named, repeated ones numbered', () => {
	assert.deepEqual(columnNames(['Amount', '', null, 'amount', 'Amount', 2026]), [
		'Amount',
		'column_2',
		'column_3',
		'amount_2',
		'Amount_3',
		'2026',
	]);
});

test('a format looks like a date when it has y or d outside text and sections', () => {
	for (const format of ['m/d/yyyy', 'yyyy-mm-dd hh:mm', 'dd.mm.yy', '[$-409]d-mmm-yy', 'D MMM']) {
		assert.ok(looksLikeDateFormat(format), format);
	}
	for (const format of ['General', '@', '0.00', '[Red]0.00', '"day "0', '0\\d', 'hh:mm:ss', '#,##0']) {
		assert.ok(!looksLikeDateFormat(format), format);
	}
});

test('column types by the rules of §12 item 3', () => {
	assert.equal(guess(num(46023, '1/1/26', 'm/d/yy'), num(46024, '1/2/26', 'm/d/yy'), EMPTY), 'date');
	assert.equal(guess(num(46023.5, '1/1/26 12:00', 'm/d/yy h:mm'), num(46024)), 'timestamp');
	assert.equal(guess(num(12, '0012', '0000'), num(345, '0345', '0000')), 'id');
	assert.equal(guess(num(1234567890123456), num(1)), 'id');
	assert.equal(guess(num(1), num(-20), EMPTY, num(300)), 'integer');
	assert.equal(guess(num(1), num(2.5)), 'number');
	assert.equal(guess(bool(true), bool(false), EMPTY), 'boolean');
	assert.equal(guess(num(1), str('two')), 'string');
	assert.equal(guess(str('0012')), 'string');
	assert.equal(guess(EMPTY, EMPTY), 'string');
	// Only the first filled cell's format counts; text in a date column is text.
	assert.equal(guess(str('n/a'), num(46023, '1/1/26', 'm/d/yy')), 'string');
	// Error cells take no part, and are counted.
	const column = new ColumnGuess();
	[num(1), ERROR, num(2)].forEach((cell) => column.add(cell));
	assert.equal(column.type(), 'integer');
	assert.equal(column.errors, 1);
});

test('cells as /v1/imports rows take them: dates as strings, ids as shown, empty as null', () => {
	assert.equal(cellValue(num(46023, '1/1/26', 'm/d/yy'), 'date', false), '2026-01-01');
	assert.equal(cellValue(num(44561, '1/1/26', 'm/d/yy'), 'date', true), '2026-01-01');
	assert.equal(cellValue(num(46023.75), 'timestamp', false), '2026-01-01 18:00:00');
	assert.equal(cellValue(num(12, '0012', '0000'), 'id', false), '0012');
	assert.equal(cellValue(num(1234567890123456, '1.23457E+15'), 'id', false), '1234567890123456');
	assert.equal(cellValue(num(12, '0012', '0000'), 'string', false), '0012');
	assert.equal(cellValue(bool(true), 'string', false), 'TRUE');
	assert.equal(cellValue(num(2.5), 'number', false), 2.5);
	assert.equal(cellValue(bool(false), 'boolean', false), false);
	assert.equal(cellValue(str('2026-03-01'), 'date', false), '2026-03-01');
	assert.equal(cellValue(EMPTY, 'integer', false), null);
	assert.equal(cellValue(ERROR, 'string', false), null);
});

test('the imports a query reads, through the workbook queries it depends on', () => {
	assert.deepEqual(dependsOf('-- @id: rec\n-- @depends: xl_import.Bank\n-- @depends: ap_invoices(bu := $bu)\nSELECT 1'), {
		imports: ['bank'],
		queries: ['ap_invoices'],
	});
	const payload = emptyPayload(WORKBOOK);
	payload.queries.push(
		{ id: 'rec', hash: 'h-rec', text: '-- @depends: middle(x := 1)\n-- @depends: xl_import.bank\nSELECT 1', saved_at: 't' },
		{ id: 'middle', hash: 'h-middle', text: '-- @depends: xl_import.rates\n-- @depends: rec()\nSELECT 1', saved_at: 't' },
		{ id: 'other', hash: 'h-other', text: '-- @depends: xl_import.unrelated\nSELECT 1', saved_at: 't' },
	);
	assert.deepEqual(importsReadBy(payload, 'h-rec').sort(), ['bank', 'rates']);
	assert.deepEqual(importsReadBy(payload, 'h-middle').sort(), ['bank', 'rates']);
	assert.deepEqual(importsReadBy(payload, 'h-unknown'), []);
});

/** A sheet table of `rows` rows: ID shown with leading zeros, a date, an amount. */
function fakeSource(table: { rows: number; headers?: unknown[] }) {
	const reads: Array<{ offset: number; limit: number }> = [];
	const ref: SourceRef = { sheet: 'Bank', table: 'tblBank' };
	const shape = (): SourceShape => ({ ref, headers: table.headers ?? ['Ref', 'Booked', 'Amount'], rows: table.rows });
	const source: ImportSource = {
		selection: async () => shape(),
		shape: async () => shape(),
		read: async (_ref, offset, limit) => {
			reads.push({ offset, limit });
			return Array.from({ length: Math.max(0, Math.min(limit, table.rows - offset)) }, (_, index) => [
				num(offset + index, String(offset + index).padStart(6, '0'), '000000'),
				num(46023, '1/1/26', 'm/d/yy'),
				num(10.5),
			]);
		},
	};
	return { reads, source, table };
}

function fakeDaemon() {
	const calls: Array<{ path: string; body: any }> = [];
	let rows = 0;
	const fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
		const path = String(input).replace('https://localhost:47321', '');
		const body = init?.body ? JSON.parse(String(init.body)) : undefined;
		calls.push({ path, body });
		if (path === '/v1/imports') {
			rows = 0;
			return new Response(JSON.stringify({ import_id: 'imp-1' }), { status: 201 });
		}
		if (path === '/v1/imports/imp-1/rows') {
			rows += body.rows.length;
			return new Response(null, { status: 204 });
		}
		if (path === '/v1/imports/imp-1/commit') {
			return new Response(JSON.stringify({ name: 'tblbank', rows }), { status: 200 });
		}
		return new Response('{}', { status: 404 });
	};
	return { calls, client: new DaemonClient({ baseUrl: 'https://localhost:47321', fetch: fetch as typeof globalThis.fetch }) };
}

function fakeWorkbook(payload?: WorkbookPayload) {
	const state = { payload };
	const access: WorkbookAccess = {
		ensureId: async () => WORKBOOK,
		read: async () => state.payload,
		write: async (next) => {
			state.payload = next;
		},
	};
	return { state, access };
}

test('Import table: types guessed and changed, rows sent in chunks of 5,000, the import kept in the workbook', async () => {
	const daemon = fakeDaemon();
	const sheet = fakeSource({ rows: 12_001 });
	const workbook = fakeWorkbook();
	const model = new ImportsModel(daemon.client, workbook.access, sheet.source, async () => false);

	await model.analyze();
	const draft = model.draft!;
	assert.equal(draft.name, 'tblbank');
	assert.deepEqual(
		draft.columns.map(({ name, type }) => [name, type]),
		[
			['Ref', 'id'],
			['Booked', 'date'],
			['Amount', 'number'],
		],
	);
	model.setDraft({ name: 'bank_statement', reimport: true, column: { index: 2, type: 'string' } });
	await model.importDraft();

	assert.equal(model.draft, undefined);
	assert.deepEqual(daemon.calls[0], {
		path: '/v1/imports',
		body: {
			name: 'bank_statement',
			workbook_id: WORKBOOK,
			source: 'tblBank',
			columns: [
				{ name: 'Ref', type: 'id' },
				{ name: 'Booked', type: 'date' },
				{ name: 'Amount', type: 'string' },
			],
		},
	});
	const chunks = daemon.calls.filter((call) => call.path === '/v1/imports/imp-1/rows').map((call) => call.body.rows);
	assert.deepEqual(chunks.map((rows) => rows.length), [5000, 5000, 2001]);
	assert.deepEqual(chunks[0][12], ['000012', '2026-01-01', '10.5']);
	assert.equal(daemon.calls.at(-1)?.path, '/v1/imports/imp-1/commit');
	assert.deepEqual(importsOf(workbook.state.payload), [
		{
			name: 'bank_statement',
			sheet: 'Bank',
			table: 'tblBank',
			reimport_before_run: true,
			columns: [
				{ name: 'Ref', type: 'id' },
				{ name: 'Booked', type: 'date' },
				{ name: 'Amount', type: 'string' },
			],
		},
	]);
	assert.match(model.states.get('bank_statement')?.done ?? '', /^Imported 12,001 rows at /);

	// The same table again: the name and the types chosen last time.
	await model.analyze();
	assert.equal(model.draft?.name, 'bank_statement');
	assert.equal(model.draft?.columns[2].type, 'string');
	assert.equal(model.draft?.columns[2].guessed, 'number');
	model.cancelDraft();

	model.setDraft({ name: 'Bad Name' });
	await model.analyze();
	model.setDraft({ name: 'Bad Name' });
	await model.importDraft();
	assert.match(model.draft?.error ?? '', /lower-case letters/);
});

test('before a run, the imports the query reads that are kept up to date are imported again', async () => {
	const payload = emptyPayload(WORKBOOK);
	payload.queries.push(
		{ id: 'rec', hash: 'h-rec', text: '-- @engine: local\n-- @depends: xl_import.bank\nSELECT 1', saved_at: 't' },
		{ id: 'plain', hash: 'h-plain', text: '-- @engine: local\n-- @depends: xl_import.rates\nSELECT 1', saved_at: 't' },
	);
	payload.imports.push(
		{ name: 'bank', sheet: 'Bank', table: 'tblBank', reimport_before_run: true, columns: [
			{ name: 'Ref', type: 'id' }, { name: 'Booked', type: 'date' }, { name: 'Amount', type: 'number' },
		] },
		{ name: 'rates', sheet: 'Rates', table: 'tblRates', reimport_before_run: false, columns: [] },
	);
	const daemon = fakeDaemon();
	const sheet = fakeSource({ rows: 3 });
	const model = new ImportsModel(daemon.client, fakeWorkbook(payload).access, sheet.source, async () => false);
	await model.load();
	const shown: string[] = [];

	await model.beforeRun('h-plain', (text) => shown.push(text));
	assert.equal(daemon.calls.length, 0);

	await model.beforeRun('h-rec', (text) => shown.push(text));
	assert.equal(daemon.calls[0].body.name, 'bank');
	assert.equal(daemon.calls.at(-1)?.path, '/v1/imports/imp-1/commit');
	assert.ok(shown.some((text) => text.startsWith('Importing bank again: ')));

	// The table's columns changed: the run is stopped, with why.
	sheet.table.headers = ['Ref', 'Booked', 'Amount', 'Note'];
	await assert.rejects(model.beforeRun('h-rec', () => {}), /Importing bank again failed: The columns of tblBank changed/);
	await model.reimport('bank');
	assert.match(model.states.get('bank')?.error ?? '', /changed since it was imported/);
});
