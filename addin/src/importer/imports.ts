// The workbook's imports (CONTRACTS.md §12, SPEC.md §5.6): a table of the
// sheet read, its columns' types guessed and shown for the person to change,
// then sent in chunks -- POST /v1/imports, rows 5,000 at a time, commit -- and
// kept in the workbook to import again, by hand or before each run of a Local
// query that reads it.

import type { DaemonClient } from '../api/client';
import { DaemonError } from '../api/errors';
import type { WorkbookAccess } from '../editor/model';
import { emptyPayload } from '../workbook/payload';
import type { ImportSource, SourceRef, SourceShape } from './excel_source';
import { cellValue, ColumnGuess, columnNames, importName, isImportName } from './infer';
import { importsOf, importsReadBy, type WorkbookImport, withImport } from './workbook_imports';

/** Rows per chunk, read from the sheet and sent (§12 item 4). */
export const IMPORT_CHUNK_ROWS = 5000;

/** An import being set up: what was read, and the name and types to import with. */
export interface ImportDraft {
	shape: SourceShape;
	name: string;
	columns: Array<{ name: string; type: string; guessed: string }>;
	reimport: boolean;
	/** Cells holding an Excel error, imported empty. */
	errors: number;
	busy?: string;
	error?: string;
}

export interface ImportState {
	busy?: string;
	error?: string;
	/** "Imported 1,234 rows at 2026-10-31 09:00:00 UTC". */
	done?: string;
}

export type ImportsListener = () => void;

export class ImportsModel {
	imports: WorkbookImport[] = [];
	readonly states = new Map<string, ImportState>();
	draft: ImportDraft | undefined;
	/** Reading the selection, before there is a draft; or why it failed. */
	reading: { busy?: string; error?: string } | undefined;
	private readonly listeners = new Set<ImportsListener>();

	constructor(
		private readonly client: DaemonClient,
		private readonly workbook: WorkbookAccess,
		private readonly source: ImportSource,
		private readonly date1904: () => Promise<boolean>,
	) {}

	onChange(listener: ImportsListener): () => void {
		this.listeners.add(listener);
		return () => this.listeners.delete(listener);
	}

	async load(): Promise<void> {
		this.imports = importsOf(await this.workbook.read());
		this.emit();
	}

	/** "Import table": reads the selection's table, every row, and guesses its
	 * columns' types (§12 item 3), for the person to check before importing. */
	async analyze(): Promise<void> {
		this.draft = undefined;
		this.reading = { busy: 'Reading the selection…' };
		this.emit();
		try {
			const shape = await this.source.selection();
			const names = columnNames(shape.headers);
			const guesses = names.map(() => new ColumnGuess());
			for (let offset = 0; offset < shape.rows; offset += IMPORT_CHUNK_ROWS) {
				this.reading = { busy: `Reading ${count(Math.min(offset + IMPORT_CHUNK_ROWS, shape.rows))} of ${count(shape.rows)} rows` };
				this.emit();
				for (const row of await this.source.read(shape.ref, offset, IMPORT_CHUNK_ROWS)) {
					row.forEach((cell, column) => guesses[column]?.add(cell));
				}
			}
			const label = shape.ref.table ?? shape.ref.sheet;
			const existing = this.imports.find((entry) => sameSource(entry, shape.ref));
			this.draft = {
				shape,
				name: existing?.name ?? importName(label),
				columns: names.map((name, index) => {
					const guessed = guesses[index].type();
					const kept = existing?.columns.find((column) => column.name.toLowerCase() === name.toLowerCase());
					return { name, type: kept?.type ?? guessed, guessed };
				}),
				reimport: existing?.reimport_before_run ?? false,
				errors: guesses.reduce((sum, guess) => sum + guess.errors, 0),
			};
			this.reading = undefined;
		} catch (error) {
			this.reading = { error: messageOf(error) };
		}
		this.emit();
	}

	setDraft(change: Partial<Pick<ImportDraft, 'name' | 'reimport'>> & { column?: { index: number; type: string } }): void {
		if (!this.draft) {
			return;
		}
		if (change.name !== undefined) {
			this.draft.name = change.name.trim();
		}
		if (change.reimport !== undefined) {
			this.draft.reimport = change.reimport;
		}
		if (change.column) {
			this.draft.columns[change.column.index].type = change.column.type;
		}
		this.emit();
	}

	cancelDraft(): void {
		this.draft = undefined;
		this.reading = undefined;
		this.emit();
	}

	/** "Import": sends the draft's rows, then keeps the import in the workbook. */
	async importDraft(): Promise<void> {
		const draft = this.draft;
		if (!draft || draft.busy !== undefined) {
			return;
		}
		if (!isImportName(draft.name)) {
			draft.error = 'The name takes lower-case letters, digits and _, starts with a letter, and is up to 63 long';
			this.emit();
			return;
		}
		const entry: WorkbookImport = {
			name: draft.name,
			sheet: draft.shape.ref.sheet,
			...(draft.shape.ref.table ? { table: draft.shape.ref.table } : { range: draft.shape.ref.range }),
			reimport_before_run: draft.reimport,
			columns: draft.columns.map(({ name, type }) => ({ name, type })),
		};
		draft.error = undefined;
		try {
			const rows = await this.send(entry, draft.shape, (busy) => {
				draft.busy = busy;
				this.emit();
			});
			await this.save(entry);
			this.states.set(entry.name, { done: `Imported ${count(rows)} rows at ${new Date().toISOString().slice(11, 19)} UTC` });
			this.draft = undefined;
		} catch (error) {
			draft.busy = undefined;
			draft.error = messageOf(error);
		}
		this.emit();
	}

	/** "Import again": the same source with the same columns and types. */
	async reimport(name: string): Promise<void> {
		const entry = this.find(name);
		this.setState(name, { busy: 'Reading the table…' });
		try {
			await this.reimportEntry(entry, (busy) => this.setState(name, { busy }));
		} catch (error) {
			this.setState(name, { error: messageOf(error) });
		}
	}

	async setReimportBeforeRun(name: string, value: boolean): Promise<void> {
		try {
			await this.save({ ...this.find(name), reimport_before_run: value });
		} catch (error) {
			this.setState(name, { error: messageOf(error) });
		}
		this.emit();
	}

	/** §12 item 5, before a run of the query `hash`: the imports it reads that
	 * are kept up to date are imported again. Throws when one fails: the run
	 * would read stale rows. */
	async beforeRun(hash: string, busy: (text: string) => void): Promise<void> {
		const payload = await this.workbook.read();
		if (!payload) {
			return;
		}
		const names = new Set(importsReadBy(payload, hash));
		for (const entry of importsOf(payload)) {
			if (!entry.reimport_before_run || !names.has(entry.name)) {
				continue;
			}
			const report = (text: string) => {
				busy(`Importing ${entry.name} again: ${text}`);
				this.setState(entry.name, { busy: text });
			};
			try {
				await this.reimportEntry(entry, report);
			} catch (error) {
				this.setState(entry.name, { error: messageOf(error) });
				throw new Error(`Importing ${entry.name} again failed: ${messageOf(error)}`);
			}
		}
	}

	private async reimportEntry(entry: WorkbookImport, busy: (text: string) => void): Promise<void> {
		const ref: SourceRef = { sheet: entry.sheet, ...(entry.table ? { table: entry.table } : { range: entry.range }) };
		const shape = await this.source.shape(ref);
		const names = columnNames(shape.headers);
		const expected = entry.columns.map((column) => column.name);
		if (names.length !== expected.length || names.some((name, index) => name.toLowerCase() !== expected[index].toLowerCase())) {
			throw new Error(
				`The columns of ${entry.table ?? `${entry.sheet}!${entry.range}`} changed since it was imported: ` +
					'select it and use Import table to import it with its new columns',
			);
		}
		const rows = await this.send(entry, shape, busy);
		this.setState(entry.name, { done: `Imported ${count(rows)} rows at ${new Date().toISOString().slice(11, 19)} UTC` });
	}

	/** POST /v1/imports, the rows in chunks, commit. Resolves with the rows imported. */
	private async send(entry: WorkbookImport, shape: SourceShape, busy: (text: string) => void): Promise<number> {
		const workbookId = await this.workbook.ensureId();
		const date1904 = await this.date1904();
		const source = entry.table ?? `${entry.sheet}!${entry.range}`;
		const { import_id: id } = await this.client.post<{ import_id: string }>('/v1/imports', {
			name: entry.name,
			workbook_id: workbookId,
			source,
			columns: entry.columns,
		});
		for (let offset = 0; offset < shape.rows; offset += IMPORT_CHUNK_ROWS) {
			busy(`Sending ${count(Math.min(offset + IMPORT_CHUNK_ROWS, shape.rows))} of ${count(shape.rows)} rows`);
			const cells = await this.source.read(shape.ref, offset, IMPORT_CHUNK_ROWS);
			const rows = cells
				.map((row) => row.map((cell, column) => cellValue(cell, entry.columns[column].type, date1904)))
				// A row with nothing in it -- the empty row of a table without rows -- is not data.
				.filter((row) => row.some((value) => value !== null));
			if (rows.length) {
				await this.client.post(`/v1/imports/${id}/rows`, { rows });
			}
		}
		busy('Committing…');
		const committed = await this.client.post<{ name: string; rows: number }>(`/v1/imports/${id}/commit`, {});
		return committed.rows;
	}

	private async save(entry: WorkbookImport): Promise<void> {
		const workbookId = await this.workbook.ensureId();
		const payload = (await this.workbook.read()) ?? emptyPayload(workbookId);
		const next = withImport({ ...payload, workbook_id: workbookId }, entry);
		await this.workbook.write(next);
		this.imports = importsOf(next);
	}

	private find(name: string): WorkbookImport {
		const entry = this.imports.find((item) => item.name === name);
		if (!entry) {
			throw new Error(`No import ${name}`);
		}
		return entry;
	}

	private setState(name: string, state: ImportState): void {
		this.states.set(name, state);
		this.emit();
	}

	private emit(): void {
		for (const listener of [...this.listeners]) {
			listener();
		}
	}
}

function sameSource(entry: WorkbookImport, ref: SourceRef): boolean {
	return ref.table
		? entry.table?.toLowerCase() === ref.table.toLowerCase()
		: entry.sheet === ref.sheet && entry.range === ref.range;
}

function count(value: number): string {
	return value.toLocaleString('en-US');
}

function messageOf(error: unknown): string {
	if (error instanceof DaemonError) {
		return error.message;
	}
	return error instanceof Error ? error.message : String(error);
}
