// What a table holds (CONTRACTS.md §11.1, §11.4) and whether a new result fits
// it (§11.2 item 2). TASKS.md D4: values (dates 1900/1904, decimal, null) and
// schema comparison.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { compareSchema, describeSchemaChange } from '../src/writer/schema';
import {
	chunkRows,
	converterFor,
	headerNames,
	isTextType,
	MAX_CELL_CHARACTERS,
	numberFormatFor,
	tableNameFor,
	toSerial,
} from '../src/writer/values';

test('values as Excel takes them (§11.4)', () => {
	const convert = (type: string, value: unknown, date1904 = false) => converterFor(type, date1904).convert(value);
	// Strings stay strings, whatever they look like.
	assert.equal(convert('id', '0012'), '0012');
	assert.equal(convert('string', '=1+1'), '=1+1');
	assert.equal(convert('integer', 42), 42);
	assert.equal(convert('number', 0.25), 0.25);
	// decimal arrives as a string (§3.2) and becomes Number(string).
	assert.equal(convert('decimal(38,2)', '-0.66'), -0.66);
	assert.equal(convert('decimal(18,0)', '1234567'), 1234567);
	// Dates: serial numbers in the workbook's date system.
	assert.equal(convert('date', '2026-01-01'), 46023);
	assert.equal(convert('date', '2026-01-01', true), 44561);
	assert.equal(convert('timestamp', '2026-01-01 12:00:00'), 46023.5);
	assert.equal(convert('timestamp', '2026-01-01 06:00:00', true), 44561.25);
	assert.equal(convert('timestamptz', '2026-01-01T18:00:00Z'), 46023.75);
	assert.equal(convert('boolean', true), true);
	assert.equal(convert('boolean', false), false);
	// NULL is an empty cell in every type.
	for (const type of ['id', 'string', 'integer', 'number', 'decimal(10,2)', 'date', 'timestamp', 'boolean']) {
		assert.equal(convert(type, null), '');
	}
});

test('a text longer than a cell is cut, and counted for the warning', () => {
	const text = converterFor('text', false);
	const long = 'x'.repeat(MAX_CELL_CHARACTERS + 5);
	assert.equal((text.convert(long) as string).length, MAX_CELL_CHARACTERS);
	assert.equal(text.convert('short'), 'short');
	assert.equal(text.truncated, 1);
});

test('serial numbers to the microsecond', () => {
	assert.equal(toSerial('1899-12-30', false), 0);
	assert.equal(toSerial('1904-01-01', true), 0);
	assert.equal(toSerial('2026-01-01 00:00:00.5', false), 46023 + 0.5 / 86_400);
	assert.equal(toSerial('not a date', false), undefined);
});

test('number formats (§11.1 item 2)', () => {
	assert.equal(numberFormatFor('id'), '@');
	assert.equal(numberFormatFor('string'), '@');
	assert.equal(numberFormatFor('text'), '@');
	assert.equal(numberFormatFor('date'), 'yyyy-mm-dd');
	assert.equal(numberFormatFor('timestamp'), 'yyyy-mm-dd hh:mm:ss');
	assert.equal(numberFormatFor('timestamptz'), 'yyyy-mm-dd hh:mm:ss');
	assert.equal(numberFormatFor('decimal(18,0)'), '#,##0');
	assert.equal(numberFormatFor('decimal(38,2)'), '#,##0.00');
	assert.equal(numberFormatFor('integer'), '0');
	assert.equal(numberFormatFor('number'), 'General');
	assert.equal(numberFormatFor('boolean'), 'General');
	assert.ok(isTextType('id') && isTextType('text') && !isTextType('date'));
});

test('headers (§11.1 item 1): header or name, Column_n, (UTC), repeats suffixed, 255 characters', () => {
	assert.deepEqual(
		headerNames([
			{ name: 'VENDOR_ID', header: 'Supplier', type: 'id' },
			{ name: 'AMT', header: '', type: 'decimal(38,2)' },
			{ name: '', header: '', type: 'string' },
			{ name: 'CREATED', header: 'CREATED', type: 'timestamptz' },
			{ name: 'amt', header: 'amt', type: 'string' },
			{ name: 'AMT', header: 'AMT', type: 'string' },
		]),
		['Supplier', 'AMT', 'Column_3', 'CREATED (UTC)', 'amt_2', 'AMT_3'],
	);
	assert.deepEqual(headerNames([{ name: 'Total', header: 'Total', type: 'number' }], ['TOTAL']), ['Total_2']);
	const long = headerNames([{ name: 'x'.repeat(300), header: '', type: 'string' }])[0];
	assert.equal(long.length, 255);
});

test('table names (§11.1 item 3)', () => {
	const taken = new Set(['tblApInvoices', 'tblApInvoices2']);
	assert.equal(tableNameFor('ap_invoices', () => false), 'tblApInvoices');
	assert.equal(tableNameFor('ap_invoices', (name) => taken.has(name)), 'tblApInvoices3');
	assert.equal(tableNameFor('d4_check', () => false), 'tblD4Check');
});

test('chunks of at most 5000 rows and about 4 MB', () => {
	const rows = Array.from({ length: 12_001 }, (_, index) => [index]);
	assert.deepEqual(
		chunkRows(rows).map((chunk) => chunk.length),
		[5000, 5000, 2001],
	);
	const wide = Array.from({ length: 10 }, () => ['x'.repeat(1000)]);
	assert.deepEqual(
		chunkRows(wide, 5000, 3500).map((chunk) => chunk.length),
		[3, 3, 3, 1],
	);
	// One row larger than the limit still goes, alone.
	assert.deepEqual(
		chunkRows([['x'.repeat(100)], ['y']], 5000, 50).map((chunk) => chunk.length),
		[1, 1],
	);
	assert.deepEqual(chunkRows([]), []);
});

test('schemas: strict refuses any change; append adds new columns after the old ones', () => {
	const same = compareSchema(['ID', 'AMT'], ['id', 'amt'], 'strict');
	assert.ok(same.ok);
	assert.deepEqual(same.order, [0, 1]);

	const reordered = compareSchema(['ID', 'AMT'], ['AMT', 'ID'], 'strict');
	assert.ok(!reordered.ok);
	assert.equal(
		describeSchemaChange(reordered),
		'The query\'s columns changed (the columns are in another order). The table was not changed',
	);

	const grown = compareSchema(['ID', 'AMT'], ['ID', 'NOTE', 'AMT'], 'strict');
	assert.ok(!grown.ok);
	assert.deepEqual(grown.added, ['NOTE']);

	const appended = compareSchema(['ID', 'AMT'], ['AMT', 'NOTE', 'ID'], 'append');
	assert.ok(appended.ok);
	assert.deepEqual(appended.headers, ['ID', 'AMT', 'NOTE']);
	assert.deepEqual(appended.order, [2, 0, 1]);

	const lost = compareSchema(['ID', 'AMT'], ['ID'], 'append');
	assert.ok(!lost.ok);
	assert.match(describeSchemaChange(lost), /columns gone: AMT/);

	// "Apply new schema": the result as it is.
	const applied = compareSchema(['ID', 'AMT'], ['ID', 'NOTE'], 'strict', true);
	assert.ok(applied.ok);
	assert.deepEqual(applied.headers, ['ID', 'NOTE']);
	assert.deepEqual(applied.order, [0, 1]);
});
