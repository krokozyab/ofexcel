// The sheet, as the editor reads it: the selected cell, a referenced cell's
// values, and the workbook's date system.

import { parseRef } from './cells';

/** The address of the selection, "Sheet1!B2". */
export async function selectedAddress(): Promise<string> {
	return Excel.run(async (context) => {
		const range = context.workbook.getSelectedRange();
		range.load('address');
		await context.sync();
		return range.address;
	});
}

/** The values of `ref`: "Sheet1!B2", "B2" on the active sheet, or a defined name. */
export async function readCells(ref: string): Promise<unknown[][]> {
	return Excel.run(async (context) => {
		const { sheet, address } = parseRef(ref);
		let range: Excel.Range;
		if (sheet) {
			range = context.workbook.worksheets.getItem(sheet).getRange(address);
		} else {
			const name = context.workbook.names.getItemOrNullObject(address);
			await context.sync();
			range = name.isNullObject
				? context.workbook.worksheets.getActiveWorksheet().getRange(address)
				: name.getRange();
		}
		range.load('values');
		await context.sync();
		return range.values as unknown[][];
	});
}

/** Whether the workbook counts dates from 1904: DATE(2026,1,1) is 46023 in the
 * 1900 system and 44561 in the 1904 one. Asked of Excel's DATE function, so
 * nothing is written to a sheet. */
export async function uses1904DateSystem(): Promise<boolean> {
	return Excel.run(async (context) => {
		const result = context.workbook.functions.date(2026, 1, 1);
		result.load('value');
		await context.sync();
		return result.value === 44561;
	});
}
