// The workbook's default connection for formulas (CONTRACTS.md §10.2): the
// document setting qexcel.default_connection, saved with the workbook.

export const DEFAULT_CONNECTION_SETTING = 'qexcel.default_connection';

export function defaultConnection(): string {
	try {
		const value: unknown = Office.context.document.settings.get(DEFAULT_CONNECTION_SETTING);
		return typeof value === 'string' ? value : '';
	} catch {
		// Not in Office, or no document yet.
		return '';
	}
}

export async function setDefaultConnection(name: string): Promise<void> {
	const settings = Office.context.document.settings;
	settings.set(DEFAULT_CONNECTION_SETTING, name);
	await new Promise<void>((resolve, reject) =>
		settings.saveAsync((result) =>
			result.status === Office.AsyncResultStatus.Succeeded ? resolve() : reject(result.error),
		),
	);
}
