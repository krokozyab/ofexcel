// The workbook's tables bound to queries (SPEC.md §5.4, CONTRACTS.md §9.2,
// §11): loading a query into a new table, refreshing a bound one, and writing
// a stopped write again. The run finishes completely before the sheet is
// touched; a failed run leaves the table as it was. Progress, cancelling and
// history are TASKS.md D6's.

import type { DaemonClient } from '../api/client';
import { DaemonError } from '../api/errors';
import type { Run, Validation } from '../api/types';
import { cellsToParam, type ParamValue } from '../editor/cells';
import type { SheetAccess, WorkbookAccess } from '../editor/model';
import { uuidV4 } from '../workbook/payload';
import { type Binding, bindingsOf, nowIso, type ParamBinding, withBinding } from './bindings';
import { describeSchemaChange, type SchemaPlan } from './schema';
import { PartialWrite, type RowsPage, SchemaChanged, type TableIO, writeResult } from './writer';

/** How often a run going on is asked about. */
export const RUN_POLL_MS = 500;

export interface LoadRequest {
	query: { id: string; hash: string };
	connection?: { name: string; host: string };
	params: Record<string, ParamBinding>;
}

export interface TableState {
	/** What is going on, for the pane: "Running the query…", "Writing 5000 of 100000 rows". */
	busy?: string;
	error?: string;
	/** The refused schema, for "Apply new schema". */
	schema?: SchemaPlan;
	warnings?: string[];
}

export interface TablesModelOptions {
	sleep?: (ms: number) => Promise<void>;
	newId?: () => string;
}

/** Why a binding may not run now, or "": its query is not trusted, its
 * connection is not the one the workbook was made for (CONTRACTS.md §9.3). */
export type BindingGuard = (binding: Binding) => string;

export type TablesListener = () => void;

export class TablesModel {
	bindings: Binding[] = [];
	readonly states = new Map<string, TableState>();
	/** A load into a new table under way or just failed; its binding is saved
	 * only once the table exists. */
	loading: (TableState & { query: string }) | undefined;
	private readonly listeners = new Set<TablesListener>();
	private readonly sleep: (ms: number) => Promise<void>;
	private readonly newId: () => string;
	private guard: BindingGuard = () => '';

	constructor(
		private readonly client: DaemonClient,
		private readonly workbook: WorkbookAccess,
		private readonly sheet: SheetAccess,
		private readonly io: TableIO,
		options: TablesModelOptions = {},
	) {
		this.sleep = options.sleep ?? ((ms) => new Promise((resolve) => setTimeout(resolve, ms)));
		this.newId = options.newId ?? (() => uuidV4(crypto.getRandomValues(new Uint8Array(16))));
	}

	setGuard(guard: BindingGuard): void {
		this.guard = guard;
		this.emit();
	}

	/** Why `binding` may not run now; "" when it may. */
	blocked(binding: Binding): string {
		return this.guard(binding);
	}

	/** §9.3 item 5: the person chose the connection a binding runs on; the
	 * workbook remembers it, with its host. */
	async chooseConnection(bindingId: string, connection: { name: string; host: string }): Promise<void> {
		try {
			await this.save({ ...this.find(bindingId), connection });
			this.states.delete(bindingId);
		} catch (error) {
			this.states.set(bindingId, { error: messageOf(error) });
		}
		this.emit();
	}

	onChange(listener: TablesListener): () => void {
		this.listeners.add(listener);
		return () => this.listeners.delete(listener);
	}

	/** The bindings the workbook holds. */
	async load(): Promise<void> {
		this.bindings = bindingsOf(await this.workbook.read());
		this.emit();
	}

	/** Runs the query and writes the result into a new table at the selection. */
	async loadToTable(request: LoadRequest): Promise<void> {
		const binding: Binding = {
			binding_id: this.newId(),
			target: { kind: 'table', sheet: '', table: '' },
			query: request.query,
			...(request.connection ? { connection: request.connection } : {}),
			params: request.params,
			schema_policy: 'strict',
			query_columns: 0,
		};
		this.loading = { query: request.query.id, busy: 'Running the query…' };
		this.emit();
		try {
			const run = await this.run(binding, (busy) => this.setLoading({ busy }));
			await this.write(binding, run.run_id, false, true, (busy) => this.setLoading({ busy }));
			this.loading = undefined;
		} catch (error) {
			this.loading = { query: request.query.id, error: messageOf(error) };
		}
		this.emit();
	}

	/** Runs the bound query again and rewrites its table (§11.2). `applyNew`
	 * takes a changed set of columns as it is ("Apply new schema"). */
	async refresh(bindingId: string, applyNew = false): Promise<void> {
		const binding = this.find(bindingId);
		const reason = this.guard(binding);
		if (reason) {
			this.setState(bindingId, { error: reason });
			return;
		}
		this.states.set(bindingId, { busy: 'Running the query…' });
		this.emit();
		try {
			const run = await this.run(binding, (busy) => this.setState(bindingId, { busy }));
			await this.write(binding, run.run_id, applyNew, false, (busy) => this.setState(bindingId, { busy }));
		} catch (error) {
			this.fail(bindingId, error);
		}
		this.emit();
	}

	/** Writes the run of a stopped write again (§11.3), without running the query. */
	async retryWrite(bindingId: string): Promise<void> {
		const binding = this.find(bindingId);
		const runId = binding.partial?.run_id ?? binding.last_success?.run_id;
		const reason = this.guard(binding);
		if (!runId || reason) {
			if (reason) {
				this.setState(bindingId, { error: reason });
			}
			return;
		}
		this.setState(bindingId, { busy: 'Writing the rows again…' });
		try {
			await this.write(binding, runId, false, false, (busy) => this.setState(bindingId, { busy }));
		} catch (error) {
			if (error instanceof DaemonError && error.code === 'E_GONE') {
				this.states.set(bindingId, { error: 'That result is gone: a newer run replaced it. Refresh to run the query again' });
			} else {
				this.fail(bindingId, error);
			}
		}
		this.emit();
	}

	private find(bindingId: string): Binding {
		const binding = this.bindings.find((item) => item.binding_id === bindingId);
		if (!binding) {
			throw new Error(`No binding ${bindingId}`);
		}
		return binding;
	}

	private fail(bindingId: string, error: unknown): void {
		if (error instanceof SchemaChanged) {
			this.states.set(bindingId, { error: describeSchemaChange(error.plan), schema: error.plan });
		} else {
			this.states.set(bindingId, { error: messageOf(error) });
		}
	}

	/** POST /v1/runs, then GET /v1/runs/{id} until it is over. Resolves with a
	 * succeeded run; rejects with why it did not succeed. */
	private async run(binding: Binding, busy: (text: string) => void): Promise<Run> {
		const payload = await this.workbook.read();
		const started = await this.client.post<Run>('/v1/runs', {
			query: binding.query,
			...(binding.connection ? { connection: binding.connection.name } : {}),
			params: await this.paramValues(binding),
			...(payload ? { workbook_id: payload.workbook_id } : {}),
		});
		let run = started;
		while (run.status === 'running' || run.status === 'queued') {
			busy(`Running the query… ${run.rows_fetched.toLocaleString('en-US')} rows so far`);
			await this.sleep(RUN_POLL_MS);
			run = await this.client.get<Run>(`/v1/runs/${run.run_id}`);
		}
		if (run.status !== 'succeeded') {
			throw new Error(run.error ? run.error.message : `The run ended ${run.status}`);
		}
		return run;
	}

	/** The parameters' values: constants as they are, cells read now and turned
	 * into values for each parameter's type. */
	private async paramValues(binding: Binding): Promise<Record<string, ParamValue>> {
		const refs = Object.entries(binding.params).filter(([, source]) => 'ref' in source);
		const types = refs.length ? await this.paramTypes(binding) : new Map<string, string>();
		const date1904 = refs.length ? await this.sheet.date1904() : false;
		const values: Record<string, ParamValue> = {};
		for (const [name, source] of Object.entries(binding.params)) {
			if ('value' in source) {
				values[name] = source.value;
				continue;
			}
			const value = cellsToParam(await this.sheet.read(source.ref), types.get(name) ?? 'string', date1904);
			if (value !== undefined) {
				values[name] = value;
			}
		}
		return values;
	}

	/** The bound query's parameter types, from its text in the workbook. */
	private async paramTypes(binding: Binding): Promise<Map<string, string>> {
		const query = (await this.workbook.read())?.queries.find((item) => item.hash === binding.query.hash);
		if (!query) {
			return new Map();
		}
		const validation = await this.client.post<Validation>('/v1/queries/validate', { text: query.text });
		return new Map((validation.header?.params ?? []).map((param) => [param.name, param.type]));
	}

	private async write(
		binding: Binding,
		runId: string,
		applyNew: boolean,
		isNew: boolean,
		busy: (text: string) => void,
	): Promise<void> {
		busy('Writing the rows…');
		const read = (offset: number, limit: number) =>
			this.client.get<RowsPage>(`/v1/runs/${runId}/rows?offset=${offset}&limit=${limit}`);
		let updated: Binding;
		let warnings: string[] = [];
		try {
			const result = await writeResult(this.io, read, {
				queryId: binding.query.id,
				date1904: await this.sheet.date1904(),
				...(isNew
					? {}
					: {
							existing: {
								target: { sheet: binding.target.sheet, table: binding.target.table },
								queryColumns: binding.query_columns,
								policy: binding.schema_policy,
								applyNew,
							},
						}),
				progress: (written, total) =>
					busy(`Writing ${written.toLocaleString('en-US')} of ${total.toLocaleString('en-US')} rows`),
			});
			const { partial: _partial, ...rest } = binding;
			updated = {
				...rest,
				target: { kind: 'table', ...result.target },
				query_columns: result.queryColumns,
				last_success: { at: nowIso(), rows: result.rows, truncated: result.truncated, run_id: runId },
			};
			warnings = result.warnings;
		} catch (error) {
			if (error instanceof PartialWrite) {
				await this.save({
					...binding,
					target: { kind: 'table', ...error.target },
					query_columns: error.queryColumns,
					partial: { at: nowIso(), run_id: runId },
				});
			}
			throw error;
		}
		await this.save(updated);
		this.states.set(updated.binding_id, warnings.length ? { warnings } : {});
	}

	/** The binding into the workbook's payload, and into the list. */
	private async save(binding: Binding): Promise<void> {
		const workbookId = await this.workbook.ensureId();
		const payload = await this.workbook.read();
		if (!payload) {
			throw new Error('The workbook holds no QExcel queries: save the query to the workbook first');
		}
		const next = withBinding({ ...payload, workbook_id: workbookId }, binding);
		await this.workbook.write(next);
		this.bindings = bindingsOf(next);
	}

	private setLoading(state: TableState): void {
		this.loading = { query: this.loading?.query ?? '', ...state };
		this.emit();
	}

	private setState(bindingId: string, state: TableState): void {
		this.states.set(bindingId, state);
		this.emit();
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
