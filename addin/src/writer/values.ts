// A result as an Excel Table holds it (CONTRACTS.md §11.1, §11.4): the values
// from the daemon's JSON (§3.2), the number formats, the headers, the table's
// name, and the chunks it is written in. Pure: no Office.js.

import type { ColumnInfo } from '../api/types';

/** The most a cell holds (§11.4). */
export const MAX_CELL_CHARACTERS = 32_767;
/** Excel's limit on a table column's header. */
export const MAX_HEADER_CHARACTERS = 255;
/** Rows per write: ofexcel_write_chunk_rows's default (§11.2 item 5). */
export const WRITE_CHUNK_ROWS = 5000;
/** …and never much more than this much JSON per write. */
export const WRITE_CHUNK_BYTES = 4 * 1024 * 1024;

const EPOCH_1900 = Date.UTC(1899, 11, 30);
const EPOCH_1904 = Date.UTC(1904, 0, 1);
const MS_PER_DAY = 86_400_000;

export type CellValue = string | number | boolean;

/** "2026-01-31", "2026-01-31 10:20:30.5", "2026-01-31T10:20:30.5Z" as an Excel
 * serial number: days from 1899-12-30 (1900 system) or 1904-01-01, the time as
 * the fraction of a day. Undefined for text that is not a date. */
export function toSerial(text: string, date1904: boolean): number | undefined {
	const match = /^(\d{4})-(\d{2})-(\d{2})(?:[ T](\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,9}))?)?Z?$/.exec(text);
	if (!match) {
		return undefined;
	}
	const fraction = match[7] ? Number(`0.${match[7]}`) * 1000 : 0;
	const utc = Date.UTC(
		Number(match[1]),
		Number(match[2]) - 1,
		Number(match[3]),
		Number(match[4] ?? 0),
		Number(match[5] ?? 0),
		Number(match[6] ?? 0),
	);
	return (utc + fraction - (date1904 ? EPOCH_1904 : EPOCH_1900)) / MS_PER_DAY;
}

/** What one column's values become. `truncated` counts the `text` values cut
 * to MAX_CELL_CHARACTERS, for the warning of §11.4. */
export interface ColumnConverter {
	convert(value: unknown): CellValue;
	truncated: number;
}

/** §11.4 for a column of contract type `type`. NULL is "": an empty cell. */
export function converterFor(type: string, date1904: boolean): ColumnConverter {
	const kind = type.replace(/\(.*$/, '');
	const converter: ColumnConverter = {
		truncated: 0,
		convert(value: unknown): CellValue {
			if (value === null || value === undefined) {
				return '';
			}
			switch (kind) {
				case 'integer':
				case 'number':
					return typeof value === 'number' ? value : Number(value);
				case 'decimal':
					return Number(value);
				case 'date':
				case 'timestamp':
				case 'timestamptz':
					return toSerial(String(value), date1904) ?? String(value);
				case 'boolean':
					return value === true || value === 'true';
				default: {
					const text = String(value);
					// Characters, not UTF-16 units: an emoji counts once.
					const characters = Array.from(text);
					if (characters.length > MAX_CELL_CHARACTERS) {
						converter.truncated++;
						return characters.slice(0, MAX_CELL_CHARACTERS).join('');
					}
					return text;
				}
			}
		},
	};
	return converter;
}

/** §11.1 item 2: the number format of a column of `type`. */
export function numberFormatFor(type: string): string {
	const decimal = /^decimal\((\d+),(\d+)\)$/.exec(type);
	if (decimal) {
		const scale = Number(decimal[2]);
		return scale === 0 ? '#,##0' : `#,##0.${'0'.repeat(scale)}`;
	}
	switch (type) {
		case 'id':
		case 'string':
		case 'text':
			return '@';
		case 'date':
			return 'yyyy-mm-dd';
		case 'timestamp':
		case 'timestamptz':
			return 'yyyy-mm-dd hh:mm:ss';
		case 'integer':
			return '0';
		default:
			return 'General';
	}
}

/** Text columns get their format at every refresh (§11.2 item 3): without it
 * Excel turns "0012" into 12 and "=1+1" into a formula. */
export function isTextType(type: string): boolean {
	return type === 'id' || type === 'string' || type === 'text';
}

/** §11.1 item 1: the header of each column -- `header`, else the name; empty
 * becomes Column_<n>; " (UTC)" after a timestamptz; at most 255 characters;
 * repeats (without regard to case, also of `taken`) get _2, _3… */
export function headerNames(columns: ColumnInfo[], taken: string[] = []): string[] {
	const used = new Set(taken.map((name) => name.toLowerCase()));
	return columns.map((column, index) => {
		let base = (column.header || column.name).trim();
		if (!base) {
			base = `Column_${index + 1}`;
		}
		if (column.type === 'timestamptz') {
			base += ' (UTC)';
		}
		base = Array.from(base).slice(0, MAX_HEADER_CHARACTERS).join('');
		let name = base;
		for (let suffix = 2; used.has(name.toLowerCase()); suffix++) {
			const tail = `_${suffix}`;
			name = Array.from(base).slice(0, MAX_HEADER_CHARACTERS - tail.length).join('') + tail;
		}
		used.add(name.toLowerCase());
		return name;
	});
}

/** §11.1 item 3: tbl<QueryId in PascalCase>, with 2, 3… if the workbook has
 * that name already. `ap_invoices` → tblApInvoices. */
export function tableNameFor(queryId: string, taken: (name: string) => boolean): string {
	const pascal = queryId
		.split(/[^A-Za-z0-9]+/)
		.filter(Boolean)
		.map((part) => part[0].toUpperCase() + part.slice(1))
		.join('');
	const base = `tbl${pascal || 'Query'}`;
	let name = base;
	for (let suffix = 2; taken(name); suffix++) {
		name = `${base}${suffix}`;
	}
	return name;
}

/** `rows` cut into writes of at most WRITE_CHUNK_ROWS rows and about
 * WRITE_CHUNK_BYTES of JSON (§11.2 item 5); a single larger row is written
 * on its own. */
export function chunkRows<T>(rows: T[][], maxRows = WRITE_CHUNK_ROWS, maxBytes = WRITE_CHUNK_BYTES): T[][][] {
	const chunks: T[][][] = [];
	let current: T[][] = [];
	let bytes = 0;
	for (const row of rows) {
		const size = JSON.stringify(row).length + 1;
		if (current.length > 0 && (current.length >= maxRows || bytes + size > maxBytes)) {
			chunks.push(current);
			current = [];
			bytes = 0;
		}
		current.push(row);
		bytes += size;
	}
	if (current.length) {
		chunks.push(current);
	}
	return chunks;
}
