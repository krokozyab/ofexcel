// Parameter values from the sheet (SPEC.md §5.2 item 3): a cell reference,
// and what its value becomes for the daemon. Excel hands a date over as a
// serial number, which the daemon does not take: it is turned into
// YYYY-MM-DD here, in the workbook's date system (CONTRACTS.md §10.2).

/** A parameter value as /v1/preview takes it (CONTRACTS.md §8.2 ParamValue). */
export type ParamValue = string | number | boolean | Array<string | number | boolean>;

const MS_PER_DAY = 86_400_000;
const EPOCH_1900 = Date.UTC(1899, 11, 30);
const EPOCH_1904 = Date.UTC(1904, 0, 1);

function fromSerial(serial: number, date1904: boolean): Date {
	return new Date((date1904 ? EPOCH_1904 : EPOCH_1900) + Math.round(serial * MS_PER_DAY));
}

/** 46023 → "2026-01-01" (1900 system); 44561 → "2026-01-01" (1904). */
export function serialToDate(serial: number, date1904: boolean): string {
	return fromSerial(Math.floor(serial), date1904).toISOString().slice(0, 10);
}

/** 46023.5 → "2026-01-01 12:00:00", to the second. */
export function serialToTimestamp(serial: number, date1904: boolean): string {
	const date = fromSerial(Math.round(serial * 86_400) / 86_400, date1904);
	return date.toISOString().slice(0, 19).replace('T', ' ');
}

function isEmpty(value: unknown): boolean {
	return value === null || value === undefined || value === '';
}

function scalar(value: unknown, type: string, date1904: boolean): string | number | boolean {
	if (typeof value === 'number') {
		if (type === 'date') {
			return serialToDate(value, date1904);
		}
		if (type === 'timestamp') {
			return serialToTimestamp(value, date1904);
		}
		return value;
	}
	if (typeof value === 'boolean') {
		return value;
	}
	return String(value);
}

/** The value of cells `values` (as Range.values gives them) for a parameter of
 * §2.2 type `type`; undefined when empty, which takes the default. A scalar
 * takes one cell; list<T> takes every non-empty cell, row by row. Throws when
 * a scalar is given more than one cell. */
export function cellsToParam(values: unknown[][], type: string, date1904: boolean): ParamValue | undefined {
	const list = /^list<(.+)>$/.exec(type);
	if (list) {
		const elements = values.flat().filter((value) => !isEmpty(value));
		return elements.length ? elements.map((value) => scalar(value, list[1], date1904)) : undefined;
	}
	if (values.length !== 1 || values[0].length !== 1) {
		throw new Error(`a ${type} parameter takes one cell, not ${values.length} × ${values[0]?.length ?? 0}`);
	}
	return isEmpty(values[0][0]) ? undefined : scalar(values[0][0], type, date1904);
}

/** "Sheet1!B2", "'My sheet'!B2:C3", "B2", "rate": the sheet, if named, and the
 * address or name within it. */
export function parseRef(ref: string): { sheet?: string; address: string } {
	const text = ref.trim();
	const quoted = /^'((?:[^']|'')+)'!(.+)$/.exec(text);
	if (quoted) {
		return { sheet: quoted[1].replace(/''/g, "'"), address: quoted[2] };
	}
	const bang = text.lastIndexOf('!');
	if (bang > 0) {
		return { sheet: text.slice(0, bang), address: text.slice(bang + 1) };
	}
	return { address: text };
}
