// The workbook's tables bound to queries (SPEC.md §5.4, CONTRACTS.md §9.2,
// §11): loading a query into a new table, refreshing a bound one, and writing
// a stopped write again. The run finishes completely before the sheet is
// touched; a failed or cancelled run leaves the table as it was, with its last
// result. While a run goes on: rows, speed and time, and Cancel (SPEC.md §5.7,
// DECISIONS.md R4); each table's history of runs on request.

import type { DaemonClient } from '../api/client';
import { DaemonError } from '../api/errors';
import type { Run, Validation } from '../api/types';
import { cellsToParam, type ParamValue } from '../editor/cells';
import type { SheetAccess, WorkbookAccess } from '../editor/model';
import { uuidV4 } from '../workbook/payload';
import { type Binding, bindingsOf, nowIso, type ParamBinding, withBinding } from './bindings';
import { describeSchemaChange, type SchemaPlan } from './schema';
import { describeFetching, describeWriting, formatElapsed, RunEnded, waitForRun } from './runs';
import { PartialWrite, type RowsPage, SchemaChanged, type TableIO, writeResult } from './writer';

/** How many runs a table's history shows. */
export const HISTORY_LIMIT = 20;

export interface LoadRequest {
	query: { id: string; hash: string };
	connection?: { name: string; host: string };
	params: Record<string, ParamBinding>;
}

export interface TableState {
	/** What is going on, for the pane: "Running the query…", "Writing 5000 of 100000 rows". */
	busy?: string;
	/** The run going on, while it may be cancelled: not once the rows are being written. */
	runId?: string;
	/** How the last refresh went: "Fetched in 0:12 · written in 0:03". */
	done?: string;
	error?: string;
	/** The refused schema, for "Apply new schema". */
	schema?: SchemaPlan;
	warnings?: string[];
}

export interface TablesModelOptions {
	sleep?: (ms: number) => Promise<void>;
	newId?: () => string;
	now?: () => number;
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
	/** The runs of each binding's query from this workbook, newest first, for the
	 * bindings whose history is open. */
	readonly history = new Map<string, Run[]>();
	readonly historyOpen = new Set<string>();
	private readonly listeners = new Set<TablesListener>();
	private readonly sleep: (ms: number) => Promise<void>;
	private readonly newId: () => string;
	private readonly now: () => number;
	/** Runs asked to stop: they say "Cancelling…" until they do. */
	private readonly cancelling = new Set<string>();
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
		this.now = options.now ?? (() => Date.now());
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
		this.loading = { query: request.query.id, busy: 'Starting the query…' };
		this.emit();
		try {
			const run = await this.run(binding, (state) => this.setLoading(state));
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
		this.states.set(bindingId, { busy: 'Starting the query…' });
		this.emit();
		const started = this.now();
		try {
			const run = await this.run(binding, (state) => this.setState(bindingId, state));
			const fetched = this.now();
			await this.write(binding, run.run_id, applyNew, false, (busy) => this.setState(bindingId, { busy }));
			this.states.set(bindingId, {
				...this.states.get(bindingId),
				done: `Fetched in ${formatElapsed(fetched - started)} · written in ${formatElapsed(this.now() - fetched)}`,
			});
		} catch (error) {
			this.fail(bindingId, error);
		}
		this.emit();
		if (this.historyOpen.has(bindingId)) {
			await this.loadHistory(bindingId);
		}
	}

	/** "Cancel" (SPEC.md §5.7): the run of `bindingId`, or of the load into a new
	 * table when it is undefined. The run then ends cancelled and the table
	 * stays as it was. */
	async cancel(bindingId?: string): Promise<void> {
		const state = bindingId === undefined ? this.loading : this.states.get(bindingId);
		const runId = state?.runId;
		if (!runId) {
			return;
		}
		this.cancelling.add(runId);
		const cancelling = { ...state, busy: 'Cancelling…' };
		if (bindingId === undefined) {
			this.setLoading(cancelling);
		} else {
			this.setState(bindingId, cancelling);
		}
		try {
			await this.client.post<Run>(`/v1/runs/${runId}/cancel`, {});
		} catch {
			// The run goes on being followed; how it ends is shown then.
		}
	}

	/** Opens or closes the history of a binding's runs; opening reads it. */
	async toggleHistory(bindingId: string): Promise<void> {
		if (this.historyOpen.delete(bindingId)) {
			this.emit();
			return;
		}
		this.historyOpen.add(bindingId);
		await this.loadHistory(bindingId);
	}

	/** GET /v1/runs?query=&workbook=: the runs of the binding's query. */
	private async loadHistory(bindingId: string): Promise<void> {
		const binding = this.find(bindingId);
		const payload = await this.workbook.read();
		const workbook = payload ? `&workbook=${encodeURIComponent(payload.workbook_id)}` : '';
		try {
			const { runs } = await this.client.get<{ runs: Run[] }>(
				`/v1/runs?query=${encodeURIComponent(binding.query.id)}${workbook}&limit=${HISTORY_LIMIT}`,
			);
			// Only this binding's query: a query id saved again under the same id
			// has another hash, and other runs of it are not this table's.
			this.history.set(
				bindingId,
				runs.filter((run) => run.query.hash === binding.query.hash && run.parent_run_id === undefined),
			);
		} catch (error) {
			this.history.delete(bindingId);
			this.setState(bindingId, { ...this.states.get(bindingId), error: messageOf(error) });
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
		if (error instanceof RunEnded && error.run.status === 'cancelled') {
			this.states.set(bindingId, { error: 'Cancelled: the table was not changed' });
		} else if (error instanceof SchemaChanged) {
			this.states.set(bindingId, { error: describeSchemaChange(error.plan), schema: error.plan });
		} else {
			this.states.set(bindingId, { error: messageOf(error) });
		}
	}

	/** POST /v1/runs, then GET /v1/runs/{id} until it is over, telling `busy`
	 * the rows, speed and time. Resolves with a succeeded run; rejects with
	 * RunEnded when it did not succeed. */
	private async run(binding: Binding, busy: (state: TableState) => void): Promise<Run> {
		const payload = await this.workbook.read();
		const started = await this.client.post<Run>('/v1/runs', {
			query: binding.query,
			...(binding.connection ? { connection: binding.connection.name } : {}),
			params: await this.paramValues(binding),
			...(payload ? { workbook_id: payload.workbook_id } : {}),
		});
		const since = this.now();
		try {
			return await waitForRun(this.client, started, {
				sleep: this.sleep,
				progress: (run) =>
					busy({
						busy: this.cancelling.has(run.run_id)
							? 'Cancelling…'
							: describeFetching(run.rows_fetched, this.now() - since),
						runId: run.run_id,
					}),
			});
		} finally {
			this.cancelling.delete(started.run_id);
		}
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
		const since = this.now();
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
				progress: (written, total) => busy(describeWriting(written, total, this.now() - since)),
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
