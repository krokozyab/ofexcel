// Writing a run into a table (TASKS.md D4, CONTRACTS.md §11) against a table
// that lives in memory: created, refreshed (grown, shrunk, reordered by name),
// refused when the schema changed, left partly written when a write fails.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import type { ColumnInfo } from '../src/api/types';
import type { CellValue } from '../src/writer/values';
import { PartialWrite, type RowsPage, SchemaChanged, type TableIO, type TableTarget, writeResult } from '../src/writer/writer';

/** One table in memory: headers, rows (each as wide as the headers), formats. */
class FakeTable implements TableIO {
	headers: string[] = [];
	rows: CellValue[][] = [];
	formats = new Map<number, string>();
	formatCalls: Array<Array<{ column: number; format: string }>> = [];
	writes: Array<{ row: number; count: number }> = [];
	names = new Set<string>();
	failWriteAt = -1;
	target: TableTarget = { sheet: 'Sheet1', table: '' };

	async tableExists(name: string) {
		return this.names.has(name);
	}
	async create(name: string, headers: string[], formats: string[], rows: number) {
		this.names.add(name);
		this.target = { sheet: 'Sheet1', table: name };
		this.headers = [...headers];
		this.rows = Array.from({ length: rows }, () => headers.map(() => ''));
		formats.forEach((format, column) => this.formats.set(column, format));
		return this.target;
	}
	async inspect() {
		return this.target.table ? { headers: [...this.headers], rows: this.rows.length } : undefined;
	}
	async setQueryHeaders(_: TableTarget, oldCount: number, headers: string[]) {
		const user = this.headers.slice(oldCount);
		const userRows = this.rows.map((row) => row.slice(oldCount));
		this.headers = [...headers, ...user];
		this.rows = userRows.map((rest) => [...headers.map(() => '' as CellValue), ...rest]);
	}
	async resize(_: TableTarget, rows: number) {
		const width = this.headers.length;
		this.rows = Array.from({ length: rows }, (_, index) => this.rows[index] ?? new Array(width).fill(''));
	}
	async setFormats(_: TableTarget, formats: Array<{ column: number; format: string }>) {
		this.formatCalls.push(formats);
		for (const { column, format } of formats) {
			this.formats.set(column, format);
		}
	}
	async write(_: TableTarget, row: number, values: CellValue[][]) {
		if (this.failWriteAt >= 0 && row >= this.failWriteAt) {
			throw new Error('InvalidArgument: the range is busy');
		}
		this.writes.push({ row, count: values.length });
		values.forEach((value, index) => {
			this.rows[row + index].splice(0, value.length, ...value);
		});
	}
	async clear(_: TableTarget, row: number, count: number, columns: number) {
		for (let index = row; index < row + count; index++) {
			this.rows[index].fill('', 0, columns);
		}
	}
}

const COLUMNS: ColumnInfo[] = [
	{ name: 'INVOICE_ID', header: 'INVOICE_ID', type: 'id' },
	{ name: 'AMT', header: 'Amount', type: 'decimal(38,2)' },
	{ name: 'DAY', header: 'DAY', type: 'date' },
];

/** A run of `total` rows, read in pages. */
function run(total: number, columns = COLUMNS, row = (index: number): unknown[] => [`${index + 1}`, '1.5', '2026-01-01']) {
	const reads: Array<{ offset: number; limit: number }> = [];
	const read = async (offset: number, limit: number): Promise<RowsPage> => {
		reads.push({ offset, limit });
		const count = Math.max(0, Math.min(limit, total - offset));
		return {
			columns,
			rows: Array.from({ length: count }, (_, index) => row(offset + index)),
			offset,
			limit,
			total,
			truncated: false,
		};
	};
	return { read, reads };
}

test('a new table: name, headers, formats, every row in chunks', async () => {
	const table = new FakeTable();
	table.names.add('tblApInvoices');
	const { read, reads } = run(12_000);
	const progress: number[] = [];
	const result = await writeResult(table, read, {
		queryId: 'ap_invoices',
		date1904: false,
		progress: (written) => progress.push(written),
	});
	assert.deepEqual(result.target, { sheet: 'Sheet1', table: 'tblApInvoices2' });
	assert.equal(result.queryColumns, 3);
	assert.equal(result.rows, 12_000);
	assert.deepEqual(table.headers, ['INVOICE_ID', 'Amount', 'DAY']);
	assert.deepEqual([...table.formats.values()], ['@', '#,##0.00', 'yyyy-mm-dd']);
	assert.deepEqual(table.rows[0], ['1', 1.5, 46023]);
	assert.deepEqual(table.rows[11_999], ['12000', 1.5, 46023]);
	assert.deepEqual(reads, [
		{ offset: 0, limit: 5000 },
		{ offset: 5000, limit: 5000 },
		{ offset: 10_000, limit: 5000 },
	]);
	assert.deepEqual(progress, [5000, 10_000, 12_000]);
});

test('an empty result keeps the header and one empty row', async () => {
	const table = new FakeTable();
	const result = await writeResult(table, run(0).read, { queryId: 'q', date1904: false });
	assert.equal(result.rows, 0);
	assert.equal(table.rows.length, 1);
	assert.deepEqual(table.rows[0], ['', '', '']);
});

test('a refresh: shrinks and grows, keeps the user\'s own columns, sets text formats every time', async () => {
	const table = new FakeTable();
	const first = await writeResult(table, run(10).read, { queryId: 'q', date1904: false });
	// A column of the user's own, right of the query's.
	table.headers.push('DOUBLE');
	table.rows.forEach((row) => row.push('=[@Amount]*2'));
	table.formatCalls = [];
	const existing = { target: first.target, queryColumns: 3, policy: 'strict' as const };

	await writeResult(table, run(4, COLUMNS, (index) => [`${index + 100}`, null, '2026-02-01']).read, {
		queryId: 'q',
		date1904: true,
		existing,
	});
	assert.equal(table.rows.length, 4);
	assert.deepEqual(table.rows[3], ['103', '', 44592, '=[@Amount]*2']);
	assert.deepEqual(table.formatCalls, [[{ column: 0, format: '@' }]]);

	await writeResult(table, run(6).read, { queryId: 'q', date1904: false, existing });
	assert.equal(table.rows.length, 6);
	assert.deepEqual(table.headers, ['INVOICE_ID', 'Amount', 'DAY', 'DOUBLE']);
	assert.deepEqual(table.rows[5].slice(0, 3), ['6', 1.5, 46023]);
});

test('strict: a changed result writes nothing', async () => {
	const table = new FakeTable();
	const first = await writeResult(table, run(3).read, { queryId: 'q', date1904: false });
	const before = JSON.stringify(table.rows);
	const changed = [COLUMNS[0], { name: 'NOTE', header: 'NOTE', type: 'string' }, ...COLUMNS.slice(1)];
	await assert.rejects(
		writeResult(table, run(5, changed).read, {
			queryId: 'q',
			date1904: false,
			existing: { target: first.target, queryColumns: 3, policy: 'strict' },
		}),
		(error: unknown) => error instanceof SchemaChanged && error.plan.added[0] === 'NOTE',
	);
	assert.equal(JSON.stringify(table.rows), before);
});

test('append, and "Apply new schema": the query\'s columns change, the user\'s stay', async () => {
	const table = new FakeTable();
	const first = await writeResult(table, run(2).read, { queryId: 'q', date1904: false });
	table.headers.push('MINE');
	table.rows.forEach((row) => row.push('kept'));
	// The result has a new column, in the middle, and the old ones reordered.
	const changed: ColumnInfo[] = [
		{ name: 'DAY', header: 'DAY', type: 'date' },
		{ name: 'NOTE', header: 'NOTE', type: 'string' },
		COLUMNS[0],
		COLUMNS[1],
	];
	const row = (index: number) => ['2026-01-01', `note ${index}`, `${index}`, '2'];
	const appended = await writeResult(table, run(2, changed, row).read, {
		queryId: 'q',
		date1904: false,
		existing: { target: first.target, queryColumns: 3, policy: 'append' },
	});
	assert.equal(appended.queryColumns, 4);
	assert.deepEqual(table.headers, ['INVOICE_ID', 'Amount', 'DAY', 'NOTE', 'MINE']);
	assert.deepEqual(table.rows[1], ['1', 2, 46023, 'note 1', 'kept']);

	const applied = await writeResult(table, run(1, [COLUMNS[0]], (index) => [`${index}`]).read, {
		queryId: 'q',
		date1904: false,
		existing: { target: first.target, queryColumns: 4, policy: 'strict', applyNew: true },
	});
	assert.equal(applied.queryColumns, 1);
	assert.deepEqual(table.headers, ['INVOICE_ID', 'MINE']);
	assert.deepEqual(table.rows[0], ['0', 'kept']);
});

test('a write that fails part way is a partial write, with the table it left', async () => {
	const table = new FakeTable();
	table.failWriteAt = 5000;
	await assert.rejects(writeResult(table, run(8000).read, { queryId: 'q', date1904: false }), (error: unknown) => {
		assert.ok(error instanceof PartialWrite);
		assert.equal(error.target.table, 'tblQ');
		assert.equal(error.queryColumns, 3);
		assert.match(error.message, /partially updated: InvalidArgument/);
		return true;
	});
	assert.deepEqual(table.writes, [{ row: 0, count: 5000 }]);
});

test('a table that is gone is said so', async () => {
	const table = new FakeTable();
	await assert.rejects(
		writeResult(table, run(1).read, {
			queryId: 'q',
			date1904: false,
			existing: { target: { sheet: 'S', table: 'tblGone' }, queryColumns: 3, policy: 'strict' },
		}),
		/The table tblGone is gone/,
	);
});
