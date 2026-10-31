// An import as the workbook keeps it (CONTRACTS.md §9.2 imports), and which
// imports a query reads through its @depends, its own and those of the
// workbook's queries it depends on (§12 item 5). Pure.

import type { WorkbookPayload } from '../workbook/payload';

export interface WorkbookImport {
	name: string;
	sheet: string;
	/** An Excel Table, by name; it follows the table as it grows. */
	table?: string;
	/** Or a range with a header row, "A1:D20": always the same cells. */
	range?: string;
	/** §12 item 5: imported again before each run of a Local query that reads it. */
	reimport_before_run: boolean;
	columns: Array<{ name: string; type: string }>;
}

export function importsOf(payload: WorkbookPayload | undefined): WorkbookImport[] {
	return (payload?.imports ?? []) as WorkbookImport[];
}

/** The payload with `entry` in place of the import of that name, or added. */
export function withImport(payload: WorkbookPayload, entry: WorkbookImport): WorkbookPayload {
	const imports = importsOf(payload);
	const found = imports.some((existing) => existing.name === entry.name);
	return {
		...payload,
		imports: found ? imports.map((existing) => (existing.name === entry.name ? entry : existing)) : [...imports, entry],
	};
}

/** The @depends of a query text: imports by name, queries by id. */
export function dependsOf(text: string): { imports: string[]; queries: string[] } {
	const imports: string[] = [];
	const queries: string[] = [];
	for (const line of text.split('\n')) {
		const directive = /^\s*--\s*@depends\s*:\s*(.+?)\s*$/.exec(line);
		if (!directive) {
			continue;
		}
		const target = directive[1];
		const imported = /^xl_import\.([a-z][a-z0-9_]*)/i.exec(target);
		if (imported) {
			imports.push(imported[1].toLowerCase());
		} else {
			const query = /^([a-z][a-z0-9_]*)/i.exec(target);
			if (query) {
				queries.push(query[1].toLowerCase());
			}
		}
	}
	return { imports, queries };
}

/** The imports the query `hash` reads, directly or through the workbook's
 * queries it depends on, each once. A dependency that is not one of the
 * workbook's queries (a project's, an example's) is not followed: its
 * imports are not this workbook's. */
export function importsReadBy(payload: WorkbookPayload, hash: string): string[] {
	const byId = new Map(payload.queries.map((query) => [query.id.toLowerCase(), query]));
	const start = payload.queries.find((query) => query.hash === hash);
	const found = new Set<string>();
	const visited = new Set<string>();
	const visit = (text: string, id: string) => {
		if (visited.has(id)) {
			return;
		}
		visited.add(id);
		const depends = dependsOf(text);
		depends.imports.forEach((name) => found.add(name));
		for (const dependency of depends.queries) {
			const query = byId.get(dependency);
			if (query) {
				visit(query.text, dependency);
			}
		}
	};
	if (start) {
		visit(start.text, start.id.toLowerCase());
	}
	return [...found];
}
