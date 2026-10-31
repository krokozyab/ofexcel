// The formula batcher (SPEC.md §5.8): calls of OF.TABLE, OF.CALL and
// OF.ASOF wait in a queue and go to /v1/batch together, after 20 ms with no
// new call or at 2,000 calls. The same call twice in a batch is sent once and
// answers both.

/** One formula call as /v1/batch takes it (CONTRACTS.md §10.3). */
export interface FormulaCall {
	kind: 'table' | 'call' | 'asof';
	query: string;
	/** One matrix per argument, as Excel passes them. */
	args: unknown[][][];
}

/** The answer to one call: a value, or an error for the cell. */
export interface FormulaResult {
	v?: unknown;
	e?: { code: string; msg: string };
}

/** Sends a batch; resolves with one result per call, in order. */
export type SendBatch = (calls: FormulaCall[]) => Promise<FormulaResult[]>;

export interface Timer {
	set(callback: () => void, ms: number): unknown;
	clear(handle: unknown): void;
}

export const BATCH_QUIET_MS = 20;
export const BATCH_MAX_CALLS = 2000;

const realTimer: Timer = {
	set: (callback, ms) => setTimeout(callback, ms),
	clear: (handle) => clearTimeout(handle as ReturnType<typeof setTimeout>),
};

/** What makes two calls the same call. */
export function callKey(call: FormulaCall): string {
	return JSON.stringify([call.kind, call.query.toLowerCase(), call.args]);
}

interface Pending {
	call: FormulaCall;
	waiters: Array<(result: FormulaResult) => void>;
}

export class FormulaBatcher {
	private queue = new Map<string, Pending>();
	private handle: unknown;

	constructor(
		private readonly send: SendBatch,
		private readonly timer: Timer = realTimer,
		private readonly quietMs = BATCH_QUIET_MS,
		private readonly maxCalls = BATCH_MAX_CALLS,
	) {}

	/** Queues `call`; resolves with its result. Never rejects: a batch that
	 * fails answers each of its calls with the error. */
	call(call: FormulaCall): Promise<FormulaResult> {
		return new Promise((resolve) => {
			const key = callKey(call);
			const pending = this.queue.get(key);
			if (pending) {
				pending.waiters.push(resolve);
			} else {
				this.queue.set(key, { call, waiters: [resolve] });
			}
			if (this.queue.size >= this.maxCalls) {
				this.flush();
				return;
			}
			if (this.handle !== undefined) {
				this.timer.clear(this.handle);
			}
			this.handle = this.timer.set(() => this.flush(), this.quietMs);
		});
	}

	/** Sends what is queued now. */
	flush(): void {
		if (this.handle !== undefined) {
			this.timer.clear(this.handle);
			this.handle = undefined;
		}
		if (this.queue.size === 0) {
			return;
		}
		const batch = [...this.queue.values()];
		this.queue = new Map();
		void this.send(batch.map((pending) => pending.call)).then(
			(results) =>
				batch.forEach((pending, index) => {
					const result = results[index] ?? { e: { code: 'notAvailable', msg: 'No answer for this call' } };
					pending.waiters.forEach((resolve) => resolve(result));
				}),
			(error: unknown) => {
				const result = { e: { code: 'notAvailable', msg: error instanceof Error ? error.message : String(error) } };
				batch.forEach((pending) => pending.waiters.forEach((resolve) => resolve(result)));
			},
		);
	}
}
