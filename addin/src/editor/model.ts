// The SQL editor's state and what it asks the daemon (SPEC.md §5.2 items 2–5):
// the text, checked as it is typed by /v1/queries/validate, which only parses
// and never reaches Fusion; the parameters and where their values come from;
// Preview, only when asked (an Oracle query is never previewed as it is
// typed); and saving to a project or to the workbook.

import type { DaemonClient } from '../api/client';
import { DaemonError } from '../api/errors';
import type { ParamDef, Preview, QuerySummary, Validation } from '../api/types';
import { emptyPayload, withQuery, type WorkbookPayload } from '../workbook/payload';
import { cellsToParam, type ParamValue } from './cells';
import { declareParam, engineOf, newQueryText, setDirective, type Engine, type ParamDeclaration } from './query_text';

/** Rows a Preview asks for (CONTRACTS.md §5.4). */
export const PREVIEW_ROWS = 200;

/** What Preview shows under an Oracle query's date columns (CONTRACTS.md §3.3). */
export const ORACLE_DATE_HINT =
	'Oracle DATE columns come without the time of day. To keep the time, use CAST(column AS TIMESTAMP)';

/** Where a parameter's value comes from for Preview. */
export type ParamSource = { kind: 'value'; value: string } | { kind: 'cell'; ref: string };

/** The workbook, as saving a query needs it (src/workbook/). */
export interface WorkbookAccess {
	ensureId(): Promise<string>;
	read(): Promise<WorkbookPayload | undefined>;
	write(payload: WorkbookPayload): Promise<void>;
}

/** The sheet, as parameter values from cells need it (excel.ts). */
export interface SheetAccess {
	read(ref: string): Promise<unknown[][]>;
	selected(): Promise<string>;
	date1904(): Promise<boolean>;
}

export type EditorListener = () => void;

export class EditorModel {
	text: string;
	/** The connection Preview of an Oracle query goes through; "" for none. */
	connection = '';
	readonly sources = new Map<string, ParamSource>();
	validation: Validation | undefined;
	/** Why the text could not be checked at all (the daemon is away…); "". */
	validationError = '';
	preview: Preview | undefined;
	previewError = '';
	previewing = false;
	/** The last save's outcome, for the pane to say. */
	saved: { ok: boolean; message: string } | undefined;
	saving = false;
	private readonly listeners = new Set<EditorListener>();

	constructor(
		private readonly client: DaemonClient,
		private readonly workbook: WorkbookAccess,
		private readonly sheet: SheetAccess,
	) {
		this.text = newQueryText('oracle');
	}

	onChange(listener: EditorListener): () => void {
		this.listeners.add(listener);
		return () => this.listeners.delete(listener);
	}

	get engine(): Engine {
		return engineOf(this.text);
	}

	/** The declared parameters, as the daemon parsed them last. */
	get params(): ParamDef[] {
		return this.validation?.header?.params ?? [];
	}

	/** Names the text uses that @param does not declare. */
	get undeclared(): string[] {
		const declared = new Set(this.params.map((param) => param.name));
		return (this.validation?.params_in_text ?? []).filter((name) => !declared.has(name));
	}

	/** A change typed in the editor: nothing goes to the daemon here. */
	setText(text: string): void {
		this.text = text;
		this.saved = undefined;
		this.emit();
	}

	newQuery(engine: Engine): void {
		this.setText(newQueryText(engine));
		this.sources.clear();
		this.preview = undefined;
		this.previewError = '';
	}

	/** Oracle | Local: @engine, and no @connection for Local (§2.2, P13). */
	setEngine(engine: Engine): void {
		let text = setDirective(this.text, 'engine', engine);
		if (engine === 'local') {
			text = setDirective(text, 'connection', undefined);
		}
		this.preview = undefined;
		this.setText(text);
	}

	setConnection(name: string): void {
		this.connection = name;
		this.preview = undefined;
		this.emit();
	}

	/** Adds `-- @param` for a name the text uses. */
	declare(declaration: ParamDeclaration): void {
		this.setText(declareParam(this.text, declaration));
	}

	setSource(name: string, source: ParamSource): void {
		this.sources.set(name, source);
		this.emit();
	}

	/** The selection becomes `name`'s cell. */
	async useSelectedCell(name: string): Promise<void> {
		this.setSource(name, { kind: 'cell', ref: await this.sheet.selected() });
	}

	/** POST /v1/queries/validate: the daemon parses the header and checks the
	 * body (tokenizer or §7); nothing reaches Fusion. */
	async validate(): Promise<void> {
		const text = this.text;
		try {
			const validation = await this.client.post<Validation>('/v1/queries/validate', { text });
			if (text !== this.text) {
				return; // typed on meanwhile; a later check follows
			}
			this.validation = validation;
			this.validationError = '';
		} catch (error) {
			this.validationError = messageOf(error);
		}
		this.emit();
	}

	/** POST /v1/preview with the current text: only from the Preview button. */
	async runPreview(): Promise<void> {
		this.previewing = true;
		this.previewError = '';
		this.emit();
		try {
			const body: Record<string, unknown> = { text: this.text, params: await this.paramValues(), limit: PREVIEW_ROWS };
			// For a Local query, the connection of its Oracle dependencies.
			if (this.connection) {
				body.connection = this.connection;
			}
			this.preview = await this.client.post<Preview>('/v1/preview', body);
		} catch (error) {
			this.preview = undefined;
			this.previewError = messageOf(error);
		} finally {
			this.previewing = false;
			this.emit();
		}
	}

	/** Each parameter's value, from its constant or its cell; empty ones are
	 * left out, so that the default applies. */
	async paramValues(): Promise<Record<string, ParamValue>> {
		const values: Record<string, ParamValue> = {};
		let date1904: boolean | undefined;
		for (const param of this.params) {
			const source = this.sources.get(param.name);
			if (!source) {
				continue;
			}
			if (source.kind === 'value') {
				if (source.value !== '') {
					values[param.name] = /^list</.test(param.type)
						? source.value.split(',').map((element) => element.trim())
						: source.value;
				}
				continue;
			}
			date1904 ??= await this.sheet.date1904();
			let value: ParamValue | undefined;
			try {
				value = cellsToParam(await this.sheet.read(source.ref), param.type, date1904);
			} catch (error) {
				throw new Error(`Parameter ${param.name} (${source.ref}): ${messageOf(error)}`);
			}
			if (value !== undefined) {
				values[param.name] = value;
			}
		}
		return values;
	}

	/** The Preview's date columns, when the hint of §3.3 applies to them. */
	get dateColumns(): string[] {
		if (this.engine !== 'oracle' || !this.preview) {
			return [];
		}
		return this.preview.columns.filter((column) => column.type === 'date').map((column) => column.header);
	}

	/** PUT /v1/queries/{id}: the file in the project, trusted as the editor's. */
	async saveToProject(project: string): Promise<void> {
		await this.save(async () => {
			const id = await this.queryId();
			const summary = await this.client.put<QuerySummary>(`/v1/queries/${encodeURIComponent(id)}`, {
				project,
				text: this.text,
			});
			return `Saved ${summary.id} to project ${project}`;
		});
	}

	/** Into the workbook (CONTRACTS.md §9.2): the workbook gets an id if it has
	 * none, its queries are registered with the daemon, and this text -- the
	 * author's own -- is trusted for it, so that it can run. The workbook is
	 * written last: a text the daemon refuses never reaches it. */
	async saveToWorkbook(savedBy?: string): Promise<void> {
		await this.save(async () => {
			const id = await this.queryId();
			const workbookId = await this.workbook.ensureId();
			let payload = (await this.workbook.read()) ?? emptyPayload(workbookId);
			payload = withQuery(payload, {
				id,
				hash: '',
				text: this.text,
				...(savedBy ? { saved_by: savedBy } : {}),
				saved_at: new Date().toISOString().slice(0, 19) + 'Z',
			});
			const registered = await this.client.put<{ queries: QuerySummary[] }>(
				`/v1/workbooks/${workbookId}/queries`,
				{ queries: payload.queries.map((query) => ({ text: query.text })) },
			);
			payload = {
				...payload,
				workbook_id: workbookId,
				queries: payload.queries.map((query, index) => ({ ...query, hash: registered.queries[index].hash })),
			};
			const hash = payload.queries.find((query) => query.id.toLowerCase() === id.toLowerCase())!.hash;
			await this.client.post('/v1/trust', { workbook_id: workbookId, hashes: [hash] });
			await this.workbook.write(payload);
			return `Saved ${id} to this workbook`;
		});
	}

	/** The @id of the text as it is now, parsed again rather than taken from a
	 * check of an older text. */
	private async queryId(): Promise<string> {
		await this.validate();
		const id = this.validation?.header?.id;
		if (!id) {
			throw new Error('The query cannot be saved until its header parses: see the problems above');
		}
		return id;
	}

	private async save(work: () => Promise<string>): Promise<void> {
		this.saving = true;
		this.saved = undefined;
		this.emit();
		try {
			this.saved = { ok: true, message: await work() };
		} catch (error) {
			this.saved = { ok: false, message: messageOf(error) };
		} finally {
			this.saving = false;
			this.emit();
		}
	}

	private emit(): void {
		for (const listener of [...this.listeners]) {
			listener();
		}
	}
}

function messageOf(error: unknown): string {
	if (error instanceof DaemonError) {
		return error.message;
	}
	return error instanceof Error ? error.message : String(error);
}
