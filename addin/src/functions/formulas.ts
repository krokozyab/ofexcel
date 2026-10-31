// What OF.TABLE, OF.CALL and OF.ASOF ask the daemon (CONTRACTS.md §10.2–10.3,
// SPEC.md §5.8): arguments with Excel's serial dates turned into dates for the
// parameters that take them, the client cache, the batcher, /v1/batch with the
// workbook's default connection and date system.

import type { DaemonClient } from '../api/client';
import { VersionedCache } from '../api/cache';
import type { QuerySummary } from '../api/types';
import { serialToDate, serialToTimestamp } from '../editor/cells';
import { callKey, FormulaBatcher, type FormulaCall, type FormulaResult, type Timer } from './batcher';

/** A formula's error, for the cell: code is one of CustomFunctions.ErrorCode's names. */
export class FormulaError extends Error {
	constructor(
		readonly code: string,
		message: string,
	) {
		super(message);
	}
}

export interface FormulaSettings {
	/** The workbook's default connection (`qexcel.default_connection`), or "". */
	connection(): string;
	/** The workbook's id, or "" while it has none. */
	workbookId(): string;
	date1904(): Promise<boolean>;
}

/** Turns the numbers in arguments for date and timestamp parameters into
 * YYYY-MM-DD and YYYY-MM-DD HH:MM:SS (§10.2): Excel passes a date as a serial
 * number, and the daemon takes dates as strings. Arguments past the
 * parameters, and other types, go as they are. */
export function convertDates(args: unknown[][][], types: string[], date1904: boolean): unknown[][][] {
	return args.map((matrix, index) => {
		const type = /^list<(.+)>$/.exec(types[index] ?? '')?.[1] ?? types[index];
		if (type !== 'date' && type !== 'timestamp') {
			return matrix;
		}
		const convert = type === 'date' ? serialToDate : serialToTimestamp;
		return matrix.map((row) => row.map((cell) => (typeof cell === 'number' ? convert(cell, date1904) : cell)));
	});
}

export class FormulaService {
	private readonly batcher: FormulaBatcher;
	private readonly cache: VersionedCache<unknown>;
	/** The parameter types of each query id, as §10.2 picks the query. */
	private types: Promise<Map<string, string[]>> | undefined;
	/** Moves with every change of the versions: a result asked for before one is not cached. */
	private generation = 0;

	constructor(
		private readonly client: DaemonClient,
		private readonly settings: FormulaSettings,
		timer?: Timer,
	) {
		this.cache = new VersionedCache(client.versions);
		client.versions.onChange(() => {
			this.generation++;
			this.types = undefined;
		});
		this.batcher = new FormulaBatcher((calls) => this.send(calls), timer);
	}

	/** The value of one formula call; throws FormulaError for the cell. */
	async evaluate(kind: FormulaCall['kind'], query: unknown, args: unknown[][][] | null | undefined): Promise<unknown> {
		const id = typeof query === 'string' ? query.trim() : query === null || query === undefined ? '' : String(query);
		if (!id) {
			throw new FormulaError('invalidValue', 'Name the query: the first argument');
		}
		const date1904 = await this.settings.date1904();
		const types = (await this.paramTypes()).get(id.toLowerCase()) ?? [];
		const call: FormulaCall = { kind, query: id, args: convertDates(args ?? [], types, date1904) };
		const key = JSON.stringify([callKey(call), this.settings.connection(), date1904]);
		if (this.cache.get(key) !== undefined) {
			return this.cache.get(key);
		}
		const generation = this.generation;
		const result = await this.batcher.call(call);
		if (result.e) {
			throw new FormulaError(result.e.code, result.e.msg);
		}
		if (generation === this.generation) {
			this.cache.set(key, result.v);
		}
		return result.v;
	}

	private async send(calls: FormulaCall[]): Promise<FormulaResult[]> {
		const connection = this.settings.connection();
		const response = await this.client.post<{ data_version: number; catalog_version: string; results: Array<FormulaResult & { i: number }> }>(
			'/v1/batch',
			{
				calls: calls.map((call, i) => ({ i, ...call })),
				...(connection ? { connection } : {}),
				date_system: (await this.settings.date1904()) ? 1904 : 1900,
			},
		);
		this.client.versions.observe(response.data_version, response.catalog_version);
		const byIndex = new Map(response.results.map((result) => [result.i, result]));
		return calls.map((_, i) => byIndex.get(i) ?? { e: { code: 'notAvailable', msg: 'No answer for this call' } });
	}

	/** GET /v1/queries: the first definition of each id is the one §10.2 picks
	 * (the workbook's, then the projects', then the examples'; newest first). */
	private paramTypes(): Promise<Map<string, string[]>> {
		if (!this.types) {
			const workbook = this.settings.workbookId();
			const loading = this.client
				.get<{ queries: QuerySummary[] }>(`/v1/queries${workbook ? `?workbook=${workbook}` : ''}`)
				.then(({ queries }) => {
					const types = new Map<string, string[]>();
					for (const query of queries) {
						const id = query.id.toLowerCase();
						if (!types.has(id)) {
							types.set(id, query.params.map((param) => param.type));
						}
					}
					return types;
				});
			this.types = loading;
			// Unreachable now: the call goes without date conversion, and the next one asks again.
			loading.catch(() => {
				if (this.types === loading) {
					this.types = undefined;
				}
			});
		}
		return this.types.catch(() => new Map());
	}
}
