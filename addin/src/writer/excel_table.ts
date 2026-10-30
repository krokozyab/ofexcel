// TableIO through Office.js (CONTRACTS.md §11), with what the phase 0 probe
// found on Excel for Mac (tools/addin-probe experiments 3–5, PHASE0_REPORT.md
// §4.8): number formats before values, so that "0012" and "=1+1" stay text;
// Table.resize, which leaves the rows it gave up filled, so they are cleared;
// calculation suspended for each write.

import type { TableIO, TableShape, TableTarget } from './writer';
import type { CellValue } from './values';

/** A number format for a whole range: Office takes one value for every cell. */
function setNumberFormat(range: Excel.Range, format: string): void {
	(range as unknown as { numberFormat: string }).numberFormat = format;
}

function table(context: Excel.RequestContext, target: TableTarget): Excel.Table {
	return context.workbook.tables.getItem(target.table);
}

export const excelTables: TableIO = {
	async tableExists(name) {
		return Excel.run(async (context) => {
			const found = context.workbook.tables.getItemOrNullObject(name);
			await context.sync();
			return !found.isNullObject;
		});
	},

	async create(name, headers, formats, rows) {
		return Excel.run(async (context) => {
			const sheet = context.workbook.worksheets.getActiveWorksheet();
			const selected = context.workbook.getSelectedRange();
			sheet.load('name');
			selected.load('rowIndex,columnIndex');
			await context.sync();
			const area = sheet.getRangeByIndexes(selected.rowIndex, selected.columnIndex, 1 + rows, headers.length);
			const used = area.getUsedRangeOrNullObject(true);
			area.load('address');
			await context.sync();
			if (!used.isNullObject) {
				throw new Error(`The cells ${area.address} are not empty. Select an empty cell to put the table at`);
			}
			const header = sheet.getRangeByIndexes(selected.rowIndex, selected.columnIndex, 1, headers.length);
			setNumberFormat(header, '@');
			header.values = [headers];
			formats.forEach((format, column) =>
				setNumberFormat(sheet.getRangeByIndexes(selected.rowIndex + 1, selected.columnIndex + column, rows, 1), format),
			);
			const created = sheet.tables.add(area, true);
			created.name = name;
			await context.sync();
			return { sheet: sheet.name, table: name };
		});
	},

	async inspect(target) {
		return Excel.run(async (context): Promise<TableShape | undefined> => {
			const found = context.workbook.tables.getItemOrNullObject(target.table);
			await context.sync();
			if (found.isNullObject) {
				return undefined;
			}
			const header = found.getHeaderRowRange();
			const body = found.getDataBodyRange();
			header.load('values');
			body.load('rowCount');
			await context.sync();
			return { headers: header.values[0].map(String), rows: body.rowCount };
		});
	},

	async setQueryHeaders(target, oldCount, headers) {
		await Excel.run(async (context) => {
			const bound = table(context, target);
			for (let index = oldCount; index < headers.length; index++) {
				bound.columns.add(index, undefined, headers[index]);
			}
			for (let index = oldCount - 1; index >= headers.length; index--) {
				bound.columns.getItemAt(index).delete();
			}
			await context.sync();
			const header = bound.getHeaderRowRange().getCell(0, 0).getResizedRange(0, headers.length - 1);
			setNumberFormat(header, '@');
			header.values = [headers];
			await context.sync();
		});
	},

	async resize(target, rows) {
		await Excel.run(async (context) => {
			const bound = table(context, target);
			const whole = bound.getRange();
			const body = bound.getDataBodyRange();
			whole.load('rowIndex,columnIndex,columnCount');
			body.load('rowCount');
			await context.sync();
			const sheet = bound.worksheet;
			const current = body.rowCount;
			if (rows === current) {
				return;
			}
			if (rows > current) {
				// Excel would take whatever is below the table into it.
				const below = sheet.getRangeByIndexes(whole.rowIndex + 1 + current, whole.columnIndex, rows - current, whole.columnCount);
				const used = below.getUsedRangeOrNullObject(true);
				below.load('address');
				await context.sync();
				if (!used.isNullObject) {
					throw new Error(`The table cannot grow to ${rows} rows: the cells ${below.address} below it are not empty`);
				}
			}
			if (Office.context.requirements.isSetSupported('ExcelApi', '1.13')) {
				bound.resize(sheet.getRangeByIndexes(whole.rowIndex, whole.columnIndex, 1 + rows, whole.columnCount));
				if (rows < current) {
					// Table.resize does not empty what it gave up: the old rows would
					// stay on the sheet under the table (PHASE0_REPORT.md §4.8).
					sheet
						.getRangeByIndexes(whole.rowIndex + 1 + rows, whole.columnIndex, current - rows, whole.columnCount)
						.clear(Excel.ClearApplyTo.contents);
				}
			} else if (rows > current) {
				bound.rows.add(undefined, Array.from({ length: rows - current }, () => new Array(whole.columnCount).fill('')));
			} else {
				// Deleted with the rows below shifted up: nothing is left to clear.
				bound.getDataBodyRange().getRow(rows).getResizedRange(current - rows - 1, 0).delete(Excel.DeleteShiftDirection.up);
			}
			await context.sync();
		});
	},

	async setFormats(target, formats) {
		await Excel.run(async (context) => {
			const body = table(context, target).getDataBodyRange();
			for (const { column, format } of formats) {
				setNumberFormat(body.getColumn(column), format);
			}
			await context.sync();
		});
	},

	async write(target, row, values: CellValue[][]) {
		if (values.length === 0) {
			return;
		}
		await Excel.run(async (context) => {
			context.application.suspendApiCalculationUntilNextSync();
			const body = table(context, target).getDataBodyRange();
			body.getCell(row, 0).getResizedRange(values.length - 1, values[0].length - 1).values = values;
			await context.sync();
		});
	},

	async clear(target, row, count, columns) {
		await Excel.run(async (context) => {
			table(context, target)
				.getDataBodyRange()
				.getCell(row, 0)
				.getResizedRange(count - 1, columns - 1)
				.clear(Excel.ClearApplyTo.contents);
			await context.sync();
		});
	},
};
