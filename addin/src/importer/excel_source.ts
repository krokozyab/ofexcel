// The cells of an import's source through Office.js (CONTRACTS.md §12 item
// 1): the Excel Table the selection is in, or else the selected range -- the
// region around it when one cell is selected -- with its first row as the
// header. Read a chunk of rows at a time.

import type { SourceCell } from './infer';

export interface SourceRef {
	sheet: string;
	table?: string;
	/** "A1:D20", header row included. */
	range?: string;
}

export interface SourceShape {
	ref: SourceRef;
	/** The header row's values. */
	headers: unknown[];
	/** Rows under the header. */
	rows: number;
}

export interface ImportSource {
	/** What the selection points at. */
	selection(): Promise<SourceShape>;
	/** The source as it is now: a table may have grown since it was imported. */
	shape(ref: SourceRef): Promise<SourceShape>;
	/** Rows `offset`.. of the source, under the header, `limit` at most. */
	read(ref: SourceRef, offset: number, limit: number): Promise<SourceCell[][]>;
}

/** The part of the source under its header: the table's body, or the range
 * less its first row. */
function body(context: Excel.RequestContext, ref: SourceRef): Excel.Range {
	if (ref.table) {
		return context.workbook.tables.getItem(ref.table).getDataBodyRange();
	}
	const range = context.workbook.worksheets.getItem(ref.sheet).getRange(ref.range);
	return range.getOffsetRange(1, 0).getResizedRange(-1, 0);
}

function header(context: Excel.RequestContext, ref: SourceRef): Excel.Range {
	if (ref.table) {
		return context.workbook.tables.getItem(ref.table).getHeaderRowRange();
	}
	return context.workbook.worksheets.getItem(ref.sheet).getRange(ref.range).getRow(0);
}

async function shapeOf(context: Excel.RequestContext, ref: SourceRef): Promise<SourceShape> {
	const top = header(context, ref);
	top.load('values,rowCount');
	const under = ref.table ? body(context, ref) : undefined;
	under?.load('rowCount');
	const whole = ref.table ? undefined : context.workbook.worksheets.getItem(ref.sheet).getRange(ref.range);
	whole?.load('rowCount');
	await context.sync();
	return {
		ref,
		headers: (top.values as unknown[][])[0],
		rows: under ? under.rowCount : whole!.rowCount - 1,
	};
}

export const excelSource: ImportSource = {
	async selection() {
		return Excel.run(async (context) => {
			const selected = context.workbook.getSelectedRange();
			const tables = selected.getTables(false);
			const sheet = selected.worksheet;
			selected.load('address,cellCount');
			tables.load('items/name');
			sheet.load('name');
			await context.sync();
			if (tables.items.length > 0) {
				return shapeOf(context, { sheet: sheet.name, table: tables.items[0].name });
			}
			const range = selected.cellCount === 1 ? selected.getSurroundingRegion() : selected;
			range.load('address,rowCount');
			await context.sync();
			if (range.rowCount < 2) {
				throw new Error('Select a table, or a range whose first row holds the column names and the rows under it');
			}
			const address = range.address.slice(range.address.lastIndexOf('!') + 1);
			return shapeOf(context, { sheet: sheet.name, range: address });
		});
	},

	async shape(ref) {
		return Excel.run((context) => shapeOf(context, ref));
	},

	async read(ref, offset, limit) {
		return Excel.run(async (context) => {
			const under = body(context, ref);
			under.load('rowIndex,columnIndex,rowCount,columnCount');
			await context.sync();
			const count = Math.min(limit, under.rowCount - offset);
			if (count <= 0) {
				return [];
			}
			const chunk = under.worksheet.getRangeByIndexes(under.rowIndex + offset, under.columnIndex, count, under.columnCount);
			chunk.load('values,text,numberFormat,valueTypes');
			await context.sync();
			const values = chunk.values as unknown[][];
			const text = chunk.text as string[][];
			const formats = chunk.numberFormat as string[][];
			const kinds = chunk.valueTypes as string[][];
			return values.map((row, r) =>
				row.map((value, c) => ({ value, text: text[r][c], format: String(formats[r][c]), kind: kinds[r][c] })),
			);
		});
	},
};
