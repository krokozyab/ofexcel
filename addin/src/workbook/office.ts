// The workbook's QExcel part (CONTRACTS.md §9.2), through Excel's own API for
// custom XML parts (ExcelApi 1.5): Office.context.document.customXmlParts is
// Word's, and Excel for Mac does not have it. Replacing the part deletes the
// old one and adds the new one in one sync.

import { payloadFromXml, payloadToXml, WORKBOOK_NAMESPACE, type WorkbookPayload } from './payload';

/** The payload the workbook holds; undefined when it holds none. */
export async function readPayload(): Promise<WorkbookPayload | undefined> {
	return Excel.run(async (context) => {
		const parts = context.workbook.customXmlParts.getByNamespace(WORKBOOK_NAMESPACE);
		parts.load('items');
		await context.sync();
		if (parts.items.length === 0) {
			return undefined;
		}
		const xml = parts.items[0].getXml();
		await context.sync();
		return payloadFromXml(xml.value);
	});
}

export async function writePayload(payload: WorkbookPayload): Promise<void> {
	await Excel.run(async (context) => {
		const parts = context.workbook.customXmlParts.getByNamespace(WORKBOOK_NAMESPACE);
		parts.load('items');
		await context.sync();
		for (const part of parts.items) {
			part.delete();
		}
		context.workbook.customXmlParts.add(payloadToXml(payload));
		await context.sync();
	});
}
