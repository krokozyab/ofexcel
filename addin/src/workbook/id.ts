// The workbook's identity (CONTRACTS.md §9.1): a UUID v4 in the document's
// settings, created when the first query or binding is saved in it. Until
// then the workbook has none, and requests go without X-OF-Workbook.

import { uuidV4 } from './payload';

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

/** The workbook's id, created and saved in its settings if it has none yet. */
export async function ensureWorkbookId(): Promise<string> {
	const existing = workbookId();
	if (existing) {
		return existing;
	}
	const id = uuidV4(crypto.getRandomValues(new Uint8Array(16)));
	const settings = Office.context.document.settings;
	settings.set(WORKBOOK_ID_SETTING, id);
	await new Promise<void>((resolve, reject) =>
		settings.saveAsync((result) =>
			result.status === Office.AsyncResultStatus.Succeeded ? resolve() : reject(result.error),
		),
	);
	return id;
}
