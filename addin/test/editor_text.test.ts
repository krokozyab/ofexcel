// The editor's pure parts (TASKS.md D3): rewriting the header, values from
// cells, the alias of a table for column completions.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { cellsToParam, parseRef, serialToDate, serialToTimestamp } from '../src/editor/cells';
import { tableForAlias } from '../src/editor/completions';
import { declareParam, directive, engineOf, formatDefault, newQueryText, setDirective } from '../src/editor/query_text';

const QUERY = '-- @id: ap_invoices\n-- @engine: oracle\n-- @version: 1\n-- @connection: dev\n\n' +
	'-- a comment\nSELECT * FROM ap_invoices_all WHERE org_id = :bu\n';

test('directives: read, replace, insert, remove; the body is never touched', () => {
	assert.equal(directive(QUERY, 'engine'), 'oracle');
	assert.equal(directive(QUERY, 'param'), undefined);
	assert.equal(engineOf(QUERY), 'oracle');

	const local = setDirective(setDirective(QUERY, 'engine', 'local'), 'connection', undefined);
	assert.equal(
		local,
		'-- @id: ap_invoices\n-- @engine: local\n-- @version: 1\n\n-- a comment\n' +
			'SELECT * FROM ap_invoices_all WHERE org_id = :bu\n',
	);
	assert.equal(engineOf(local), 'local');

	const titled = setDirective(QUERY, 'title', 'AP invoices');
	assert.equal(titled.split('\n')[4], '-- @title: AP invoices');
	// A "-- @" inside the body is not a directive.
	const body = '-- @id: q\n-- @engine: oracle\n-- @version: 1\nSELECT 1 FROM dual\n-- @engine: local\n';
	assert.equal(setDirective(body, 'engine', 'local').split('\n')[1], '-- @engine: local');
	assert.equal(setDirective(body, 'engine', 'local').split('\n')[4], '-- @engine: local');
	assert.equal(engineOf('SELECT 1'), 'oracle');
});

test('declaring a parameter: after the last @param, or after the last directive; defaults quoted when needed', () => {
	const one = declareParam(QUERY, { name: 'bu', type: 'integer', defaultValue: '204' });
	assert.equal(one.split('\n')[4], '-- @param bu: integer = 204');
	const two = declareParam(one, { name: 'd', type: 'date' });
	assert.deepEqual(one.split('\n').slice(0, 5).concat(['-- @param d: date']), two.split('\n').slice(0, 6));
	assert.equal(
		declareParam(QUERY, { name: 's', type: 'string', defaultValue: 'a | b', description: 'Status' }).split('\n')[4],
		'-- @param s: string = "a | b" | Status',
	);
	assert.equal(formatDefault(''), '""');
	assert.equal(formatDefault(' x'), '" x"');
	assert.equal(formatDefault('say "hi"'), '"say \\"hi\\""');
	assert.equal(formatDefault('2026-01-01'), '2026-01-01');
	assert.ok(newQueryText('local').includes('-- @engine: local'));
});

test('serial dates in both date systems', () => {
	assert.equal(serialToDate(46023, false), '2026-01-01');
	assert.equal(serialToDate(44561, true), '2026-01-01');
	assert.equal(serialToDate(46023.75, false), '2026-01-01');
	assert.equal(serialToTimestamp(46023.5, false), '2026-01-01 12:00:00');
	assert.equal(serialToTimestamp(44561 + 1 / 86400, true), '2026-01-01 00:00:01');
});

test('cell values as parameter values', () => {
	assert.equal(cellsToParam([[46023]], 'date', false), '2026-01-01');
	assert.equal(cellsToParam([[44561]], 'date', true), '2026-01-01');
	assert.equal(cellsToParam([['2026-03-01']], 'date', false), '2026-03-01');
	assert.equal(cellsToParam([[204]], 'integer', false), 204);
	assert.equal(cellsToParam([[true]], 'boolean', false), true);
	assert.equal(cellsToParam([['']], 'integer', false), undefined);
	assert.equal(cellsToParam([[null]], 'string', false), undefined);
	assert.deepEqual(cellsToParam([[1, ''], [3, null]], 'list<id>', false), [1, 3]);
	assert.deepEqual(cellsToParam([[46023], [46024]], 'list<date>', false), ['2026-01-01', '2026-01-02']);
	assert.equal(cellsToParam([[''], ['']], 'list<id>', false), undefined);
	assert.throws(() => cellsToParam([[1, 2]], 'integer', false), /one cell, not 1 × 2/);
});

test('cell references', () => {
	assert.deepEqual(parseRef('Sheet1!B2'), { sheet: 'Sheet1', address: 'B2' });
	assert.deepEqual(parseRef("'My sheet'!B2:C3"), { sheet: 'My sheet', address: 'B2:C3' });
	assert.deepEqual(parseRef("'It''s'!A1"), { sheet: "It's", address: 'A1' });
	assert.deepEqual(parseRef(' B2 '), { address: 'B2' });
	assert.deepEqual(parseRef('rate'), { address: 'rate' });
});

test('the table behind an alias, for column completions', () => {
	const sql = 'SELECT i.invoice_id FROM ap_invoices_all i JOIN poz_suppliers AS s ON s.vendor_id = i.vendor_id\n' +
		'LEFT JOIN fusion.hz_parties p ON 1 = 1 WHERE gl_ledgers.x = 1';
	assert.equal(tableForAlias(sql, 'i'), 'ap_invoices_all');
	assert.equal(tableForAlias(sql, 'S'), 'poz_suppliers');
	assert.equal(tableForAlias(sql, 'p'), 'fusion.hz_parties');
	assert.equal(tableForAlias(sql, 'ap_invoices_all'), 'ap_invoices_all');
	assert.equal(tableForAlias(sql, 'hz_parties'), 'fusion.hz_parties');
	assert.equal(tableForAlias(sql, 'gl_ledgers'), undefined);
	// "FROM t WHERE": WHERE is no alias.
	assert.equal(tableForAlias('SELECT * FROM t WHERE x = 1', 'where'), undefined);
});
