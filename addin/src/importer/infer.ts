// Importing a sheet's table (CONTRACTS.md §12, SPEC.md §5.6): the import's
// name, its columns' names, the type each column is guessed to have from its
// cells, and what each cell becomes for /v1/imports/{id}/rows (§3.2 encoding,
// dates as strings).

import { serialToDate, serialToTimestamp } from '../editor/cells';

/** One cell as Excel gives it: Range.values, .text, .numberFormat, .valueTypes. */
export interface SourceCell {
	value: unknown;
	/** What the cell shows: "0012" for 12 in format "0000". */
	text: string;
	format: string;
	/** Excel.RangeValueType: "Empty", "String", "Double", "Boolean", "Error", … */
	kind: string;
}

/** The types an import column may have (§3.2 without timestamptz), in the
 * order the override list offers them. */
export const IMPORT_TYPES = ['string', 'text', 'id', 'integer', 'number', 'date', 'timestamp', 'boolean'] as const;

const MAX_NAME = 63;
const MAX_COLUMN_NAME = 255;

/** §12 item 2: "Bank Statement 2026" → "bank_statement_2026"; "2026 rates" →
 * "t_2026_rates". A name that would not start with a letter gets "t_", so it
 * is an identifier of §1. */
export function importName(label: string): string {
	let name = label.toLowerCase().replace(/[^a-z0-9_]/g, '_');
	if (!/^[a-z]/.test(name)) {
		name = `t_${name}`;
	}
	return name.slice(0, MAX_NAME);
}

/** An identifier of §1, as the daemon checks it. */
export function isImportName(name: string): boolean {
	return /^[a-z][a-z0-9_]{0,62}$/.test(name);
}

/** The header row's texts as column names: an empty header becomes column_N,
 * a repeated one (ignoring case) gets _2, _3…, as the daemon wants them all
 * different. */
export function columnNames(headers: unknown[]): string[] {
	const seen = new Set<string>();
	return headers.map((header, index) => {
		const base = (header === null || header === undefined ? '' : String(header)).trim().slice(0, MAX_COLUMN_NAME) ||
			`column_${index + 1}`;
		let name = base;
		for (let suffix = 2; seen.has(name.toLowerCase()); suffix++) {
			name = `${base.slice(0, MAX_COLUMN_NAME - String(suffix).length - 1)}_${suffix}`;
		}
		seen.add(name.toLowerCase());
		return name;
	});
}

/** §12 item 3: a format "looks like a date" if it has y or d, outside quoted
 * text, [colour] and [$-locale] sections and \-escaped characters, and is not
 * the text format "@". */
export function looksLikeDateFormat(format: string): boolean {
	if (format.trim() === '@') {
		return false;
	}
	const bare = format
		.replace(/"[^"]*"/g, '')
		.replace(/\[[^\]]*\]/g, '')
		.replace(/\\./g, '')
		.toLowerCase();
	return /[yd]/.test(bare);
}

function isBlank(cell: SourceCell): boolean {
	return cell.kind === 'Empty' || cell.value === null || cell.value === undefined || cell.value === '';
}

/** A number shown with leading zeros ("0012") or longer than 15 digits: an
 * identifier, not a quantity. */
function looksLikeId(cell: SourceCell, value: number): boolean {
	return /^[-+]?0\d/.test(cell.text.trim()) || Math.abs(value) >= 1e15;
}

/** The type of one column, guessed from its cells as they come, a chunk at a
 * time (§12 item 3). Error cells (#N/A…) take no part: they are imported empty. */
export class ColumnGuess {
	private firstFormat: string | undefined;
	private filled = 0;
	private numbers = 0;
	private booleans = 0;
	private fraction = false;
	private idLike = false;
	/** Cells holding an Excel error, imported as empty. */
	errors = 0;

	add(cell: SourceCell): void {
		if (isBlank(cell)) {
			return;
		}
		if (cell.kind === 'Error') {
			this.errors++;
			return;
		}
		this.filled++;
		this.firstFormat ??= cell.format;
		if (typeof cell.value === 'number') {
			this.numbers++;
			if (!Number.isInteger(cell.value)) {
				this.fraction = true;
			}
			if (looksLikeId(cell, cell.value)) {
				this.idLike = true;
			}
		} else if (typeof cell.value === 'boolean') {
			this.booleans++;
		}
	}

	type(): string {
		if (this.filled === 0) {
			return 'string';
		}
		const allNumbers = this.numbers === this.filled;
		if (allNumbers && looksLikeDateFormat(this.firstFormat ?? '')) {
			return this.fraction ? 'timestamp' : 'date';
		}
		if (allNumbers) {
			return this.idLike ? 'id' : this.fraction ? 'number' : 'integer';
		}
		if (this.booleans === this.filled) {
			return 'boolean';
		}
		return 'string';
	}
}

/** What a cell becomes in a column of `type` for /v1/imports/{id}/rows:
 * null for an empty or error cell; dates as YYYY-MM-DD strings in the
 * workbook's date system; for text columns what the cell shows, so "0012"
 * keeps its zeros. A value the type cannot take goes as it is, and the daemon
 * refuses it with the row and the column. */
export function cellValue(cell: SourceCell, type: string, date1904: boolean): string | number | boolean | null {
	if (isBlank(cell) || cell.kind === 'Error') {
		return null;
	}
	const value = cell.value;
	switch (type) {
		case 'string':
		case 'text':
			return typeof value === 'string' ? value : cell.text;
		case 'id':
			if (typeof value === 'number') {
				const shown = cell.text.trim();
				return /^[-+]?\d+$/.test(shown) ? shown : Number.isInteger(value) ? String(value) : cell.text;
			}
			return typeof value === 'string' ? value.trim() : String(value);
		case 'date':
			return typeof value === 'number' ? serialToDate(value, date1904) : (value as string | boolean);
		case 'timestamp':
			return typeof value === 'number' ? serialToTimestamp(value, date1904) : (value as string | boolean);
		default:
			return value as string | number | boolean;
	}
}
