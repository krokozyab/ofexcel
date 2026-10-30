// The workbook's identity (CONTRACTS.md §9.1): a UUID v4 in the document's
// settings, created when the first query or binding is saved in it (TASKS.md
// D5). Until then the workbook has none, and requests go without X-OF-Workbook.

export const WORKBOOK_ID_SETTING = 'qexcel.workbook_id';

export function workbookId(): string {
	try {
		const value: unknown = Office.context.document.settings.get(WORKBOOK_ID_SETTING);
		return typeof value === 'string' ? value : '';
	} catch {
		// Not in Office, or no document yet.
		return '';
	}
}
