// Writing a finished run's rows into an Excel Table (CONTRACTS.md §11; SPEC.md
// §5.4): a new table at the selection, or the refresh of a bound one -- schema
// compared first, then resized, formatted and written in chunks, only into the
// query's own columns. The Excel side is TableIO (excel_table.ts); this part
// decides what to do, and is tested without Excel.

import type { ColumnInfo } from '../api/types';
import { compareSchema, type SchemaPlan, type SchemaPolicy } from './schema';
import {
	type CellValue,
	chunkRows,
	converterFor,
	headerNames,
	isTextType,
	numberFormatFor,
	tableNameFor,
	WRITE_CHUNK_ROWS,
} from './values';

export interface TableTarget {
	sheet: string;
	table: string;
}

export interface TableShape {
	/** Every column's header, the query's first. */
	headers: string[];
	/** Data rows, not counting the header. */
	rows: number;
}

/** What the writer asks of Excel. */
export interface TableIO {
	tableExists(name: string): Promise<boolean>;
	/** A table `name` at the selection: the header row, the formats, `rows`
	 * data rows. Refuses cells that are not empty. */
	create(name: string, headers: string[], formats: string[], rows: number): Promise<TableTarget>;
	/** The table's headers and data rows; undefined when it is gone. */
	inspect(target: TableTarget): Promise<TableShape | undefined>;
	/** The first `oldCount` columns' headers become `headers`: columns are added
	 * or removed at the end of the query's columns, before the user's own. */
	setQueryHeaders(target: TableTarget, oldCount: number, headers: string[]): Promise<void>;
	/** The table holds `rows` data rows. It grows only over empty cells; after
	 * shrinking, the rows it left are cleared (§11.2 item 4). */
	resize(target: TableTarget, rows: number): Promise<void>;
	setFormats(target: TableTarget, formats: Array<{ column: number; format: string }>): Promise<void>;
	/** `values` from data row `row`, first column on; calculation is suspended
	 * for the write (§11.2 item 5). */
	write(target: TableTarget, row: number, values: CellValue[][]): Promise<void>;
	/** Empties the first `columns` columns of `count` data rows from `row`. */
	clear(target: TableTarget, row: number, count: number, columns: number): Promise<void>;
}

/** GET /v1/runs/{id}/rows (CONTRACTS.md §8.2 RowsPage). */
export interface RowsPage {
	columns: ColumnInfo[];
	rows: unknown[][];
	offset: number;
	limit: number;
	total: number;
	truncated: boolean;
}

export type PageReader = (offset: number, limit: number) => Promise<RowsPage>;

/** The result's columns no longer match the table: nothing was written. */
export class SchemaChanged extends Error {
	constructor(readonly plan: SchemaPlan) {
		super('The query\'s columns changed');
		this.name = 'SchemaChanged';
	}
}

/** The write stopped part way: the table holds a mix of old and new rows (§11.3). */
export class PartialWrite extends Error {
	constructor(
		readonly target: TableTarget,
		/** The query's columns in the table as the write left it. */
		readonly queryColumns: number,
		readonly cause: unknown,
	) {
		super(`The table was partially updated: ${cause instanceof Error ? cause.message : String(cause)}`);
		this.name = 'PartialWrite';
	}
}

export interface ExistingTable {
	target: TableTarget;
	queryColumns: number;
	policy: SchemaPolicy;
	/** "Apply new schema": the result's columns as they are. */
	applyNew?: boolean;
}

export interface WriteOptions {
	/** For a new table's name. */
	queryId: string;
	date1904: boolean;
	/** The table to refresh; a new one when absent. */
	existing?: ExistingTable;
	/** Called after each chunk with the rows written so far. */
	progress?: (written: number, total: number) => void;
}

export interface WriteResult {
	target: TableTarget;
	queryColumns: number;
	rows: number;
	truncated: boolean;
	/** "N values truncated in column X" (§11.4). */
	warnings: string[];
}

async function freeTableName(io: TableIO, queryId: string): Promise<string> {
	const taken = new Set<string>();
	for (;;) {
		const name = tableNameFor(queryId, (candidate) => taken.has(candidate));
		if (!(await io.tableExists(name))) {
			return name;
		}
		taken.add(name);
	}
}

export async function writeResult(io: TableIO, read: PageReader, options: WriteOptions): Promise<WriteResult> {
	const first = await read(0, WRITE_CHUNK_ROWS);
	const columns = first.columns;
	const total = first.total;
	const incoming = headerNames(columns);
	// A table keeps at least one data row: its header alone is not a table.
	const tableRows = Math.max(total, 1);

	let target: TableTarget;
	let order = columns.map((_, index) => index);
	let formats: Array<{ column: number; format: string }> = [];
	let queryColumns = columns.length;
	if (!options.existing) {
		target = await io.create(
			await freeTableName(io, options.queryId),
			incoming,
			columns.map((column) => numberFormatFor(column.type)),
			tableRows,
		);
	} else {
		const existing = options.existing;
		target = existing.target;
		const shape = await io.inspect(target);
		if (!shape) {
			throw new Error(`The table ${target.table} is gone: it was deleted or renamed. Load the query into a table again`);
		}
		const current = shape.headers.slice(0, existing.queryColumns);
		const plan = compareSchema(current, incoming, existing.policy, existing.applyNew);
		if (!plan.ok) {
			throw new SchemaChanged(plan);
		}
		order = plan.order;
		queryColumns = plan.headers.length;
		const fresh = new Set(plan.added.map((name) => name.toLowerCase()));
		formats = plan.headers
			.map((header, column) => ({ header, column, type: columns[order[column]].type }))
			// Text formats every time (§11.2 item 3); the rest only for new columns.
			.filter((item) => isTextType(item.type) || fresh.has(item.header.toLowerCase()) || existing.applyNew)
			.map((item) => ({ column: item.column, format: numberFormatFor(item.type) }));
		if (plan.headers.join('\n') !== current.join('\n')) {
			await io.setQueryHeaders(target, existing.queryColumns, plan.headers);
		}
	}

	const converters = order.map((index) => converterFor(columns[index].type, options.date1904));
	try {
		if (options.existing) {
			await io.resize(target, tableRows);
			if (formats.length) {
				await io.setFormats(target, formats);
			}
		}
		let page = first;
		let written = 0;
		for (;;) {
			const values = page.rows.map((row) => order.map((index, column) => converters[column].convert(row[index])));
			for (const chunk of chunkRows(values)) {
				await io.write(target, written, chunk);
				written += chunk.length;
				options.progress?.(written, total);
			}
			if (written >= total || page.rows.length === 0) {
				break;
			}
			page = await read(written, WRITE_CHUNK_ROWS);
		}
		if (total === 0) {
			await io.clear(target, 0, 1, queryColumns);
		}
	} catch (error) {
		throw new PartialWrite(target, queryColumns, error);
	}

	const warnings = converters
		.map((converter, column) =>
			converter.truncated
				? `${converter.truncated} values truncated in column ${columns[order[column]].header || columns[order[column]].name}`
				: '',
		)
		.filter(Boolean);
	return { target, queryColumns, rows: total, truncated: first.truncated, warnings };
}
