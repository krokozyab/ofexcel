// Smart Refresh (SPEC.md §7.5, CONTRACTS.md §8.3 /v1/missing): the data the
// workbook's formulas asked for and did not find, and one button that fetches
// all of it. Oracle queries run first, then Local ones with their missing
// dependencies (deps = missing), so a Local query finds what was just fetched.

import type { DaemonClient } from '../api/client';
import { DaemonError } from '../api/errors';
import type { MissingEntry, Run } from '../api/types';
import { describeFetching, describeParams, RunEnded, waitForRun } from './runs';

/** What a missing entry's connection is when the query is a Local one. */
const LOCAL = 'local';

export interface MissingModelOptions {
	sleep?: (ms: number) => Promise<void>;
	now?: () => number;
	/** Whether the query `hash` may run: false for a query not trusted (§9.3). */
	allows?: (hash: string) => boolean;
}

export type MissingListener = () => void;

export class MissingModel {
	entries: MissingEntry[] = [];
	/** What "Refresh missing" is doing: "2 of 5: ap_invoices · Fetched 1,234 rows · 0:02". */
	busy: string | undefined;
	/** The runs that did not succeed, one line each, from the last "Refresh missing". */
	problems: string[] = [];
	private runId: string | undefined;
	private stopped = false;
	private readonly listeners = new Set<MissingListener>();
	private readonly sleep: (ms: number) => Promise<void>;
	private readonly now: () => number;
	private readonly allows: (hash: string) => boolean;

	constructor(
		private readonly client: DaemonClient,
		/** The workbook's id, or "" while it has none (§9.1). */
		private readonly workbookId: () => string,
		options: MissingModelOptions = {},
	) {
		this.sleep = options.sleep ?? ((ms) => new Promise((resolve) => setTimeout(resolve, ms)));
		this.now = options.now ?? (() => Date.now());
		this.allows = options.allows ?? (() => true);
	}

	onChange(listener: MissingListener): () => void {
		this.listeners.add(listener);
		return () => this.listeners.delete(listener);
	}

	/** GET /v1/missing for this workbook. A workbook without an id has none. */
	async load(): Promise<void> {
		const id = this.workbookId();
		if (!id) {
			this.entries = [];
		} else {
			this.entries = (await this.client.get<{ missing: MissingEntry[] }>(`/v1/missing?workbook=${id}`)).missing;
		}
		this.emit();
	}

	/** The entries "Refresh missing" runs: those of queries not trusted wait. */
	runnable(): MissingEntry[] {
		return this.entries.filter((entry) => this.allows(entry.query.hash));
	}

	/** "Refresh missing": every runnable entry, Oracle first, one at a time. */
	async refreshAll(): Promise<void> {
		if (this.busy !== undefined) {
			return;
		}
		const workbookId = this.workbookId();
		const queue = this.runnable().sort(
			(a, b) => Number(a.connection === LOCAL) - Number(b.connection === LOCAL),
		);
		this.problems = [];
		this.stopped = false;
		for (const [index, entry] of queue.entries()) {
			if (this.stopped) {
				break;
			}
			const label = `${index + 1} of ${queue.length}: ${entry.query.id}`;
			this.setBusy(`${label} · starting`);
			try {
				const started = await this.client.post<Run>('/v1/runs', {
					query: entry.query,
					...(entry.connection === LOCAL ? {} : { connection: entry.connection }),
					params: entry.params,
					...(workbookId ? { workbook_id: workbookId } : {}),
					deps: 'missing',
				});
				this.runId = started.run_id;
				const since = this.now();
				await waitForRun(this.client, started, {
					sleep: this.sleep,
					progress: (run) =>
						this.setBusy(
							this.stopped ? 'Cancelling…' : `${label} · ${describeFetching(run.rows_fetched, this.now() - since)}`,
						),
				});
			} catch (error) {
				if (!(error instanceof RunEnded && error.run.status === 'cancelled')) {
					this.problems.push(`${describeEntry(entry)}: ${messageOf(error)}`);
				}
			}
			this.runId = undefined;
		}
		this.busy = undefined;
		try {
			await this.load();
		} catch (error) {
			this.problems.push(messageOf(error));
			this.emit();
		}
	}

	/** Stops "Refresh missing": the run going on is cancelled, the rest not started. */
	async cancel(): Promise<void> {
		if (this.busy === undefined) {
			return;
		}
		this.stopped = true;
		this.setBusy('Cancelling…');
		if (this.runId) {
			try {
				await this.client.post<Run>(`/v1/runs/${this.runId}/cancel`, {});
			} catch {
				// The run goes on being followed and ends one way or another.
			}
		}
	}

	private setBusy(text: string): void {
		this.busy = text;
		this.emit();
	}

	private emit(): void {
		for (const listener of [...this.listeners]) {
			listener();
		}
	}
}

/** "ap_invoices (bu = 204; since = 2026-01-01) on dev". */
export function describeEntry(entry: MissingEntry): string {
	const params = describeParams(entry.params);
	return (
		entry.query.id +
		(params ? ` (${params})` : '') +
		(entry.connection === LOCAL ? '' : ` on ${entry.connection}`)
	);
}

function messageOf(error: unknown): string {
	if (error instanceof DaemonError) {
		return error.message;
	}
	return error instanceof Error ? error.message : String(error);
}
