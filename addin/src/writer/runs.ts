// Following a run (SPEC.md §5.7, DECISIONS.md R4): asking about it until it is
// over, and the words the pane shows for it -- rows, speed and time while it
// goes, then the history line. There is no estimate of the time left: how many
// rows a query returns is not known before it ends.

import type { DaemonClient } from '../api/client';
import type { CanonicalParams, Run } from '../api/types';

/** How often a run going on is asked about. */
export const RUN_POLL_MS = 500;

/** A run that ended other than succeeded. */
export class RunEnded extends Error {
	constructor(readonly run: Run) {
		super(
			run.status === 'cancelled'
				? 'Cancelled'
				: run.error
					? run.error.message
					: `The run ended ${run.status}`,
		);
	}
}

export interface RunWatch {
	sleep: (ms: number) => Promise<void>;
	/** Called on every answer while the run goes on. */
	progress: (run: Run) => void;
}

/** GET /v1/runs/{id} until the run is over. Resolves with a succeeded run;
 * rejects with RunEnded otherwise. */
export async function waitForRun(client: DaemonClient, started: Run, watch: RunWatch): Promise<Run> {
	let run = started;
	while (run.status === 'running' || run.status === 'queued') {
		watch.progress(run);
		await watch.sleep(RUN_POLL_MS);
		run = await client.get<Run>(`/v1/runs/${run.run_id}`);
	}
	if (run.status !== 'succeeded') {
		throw new RunEnded(run);
	}
	return run;
}

/** 0:07, 3:12, 1:02:05. */
export function formatElapsed(ms: number): string {
	const total = Math.max(0, Math.floor(ms / 1000));
	const seconds = String(total % 60).padStart(2, '0');
	const minutes = Math.floor(total / 60) % 60;
	const hours = Math.floor(total / 3600);
	return hours ? `${hours}:${String(minutes).padStart(2, '0')}:${seconds}` : `${minutes}:${seconds}`;
}

/** R4: "Fetched 12,345 rows · 5,200 rows/s · 0:02". The speed once a second
 * has gone by, so the first answers do not show a wild number. */
export function describeFetching(rows: number, elapsedMs: number): string {
	const parts = [`Fetched ${count(rows)} rows`];
	if (elapsedMs >= 1000 && rows > 0) {
		parts.push(`${count(Math.round(rows / (elapsedMs / 1000)))} rows/s`);
	}
	parts.push(formatElapsed(elapsedMs));
	return parts.join(' · ');
}

/** "Writing 5,000 of 100,000 rows · 0:03". */
export function describeWriting(written: number, total: number, elapsedMs: number): string {
	return `Writing ${count(written)} of ${count(total)} rows · ${formatElapsed(elapsedMs)}`;
}

/** SPEC.md §5.7: what a truncated result says, wherever it is shown. */
export function describeTruncation(rows: number): string {
	return `Showing ${count(rows)} of more than ${count(rows)} rows (query limit)`;
}

/** 2026-10-31T09:15:02Z → "2026-10-31 09:15:02 UTC". */
export function formatTime(iso: string): string {
	return iso.replace('T', ' ').replace('Z', ' UTC');
}

/** "bu = 204, since = 2026-01-01"; "" without parameters. */
export function describeParams(params: CanonicalParams): string {
	return Object.entries(params)
		.map(([name, value]) => `${name} = ${Array.isArray(value) ? value.join(', ') : value}`)
		.join('; ');
}

/** One line of the history: when, how it ended, rows, how long, where. */
export function describeRun(run: Run): string {
	const parts = [formatTime(run.started_at), run.status];
	if (run.status === 'succeeded' || run.rows_fetched > 0) {
		parts.push(`${count(run.rows_fetched)} rows`);
	}
	if (run.truncated) {
		parts.push('truncated');
	}
	if (run.finished_at) {
		parts.push(formatElapsed(Date.parse(run.finished_at) - Date.parse(run.started_at)));
	}
	parts.push(run.environment ? `${run.connection} (${run.environment})` : run.connection);
	return parts.join(' · ');
}

function count(value: number): string {
	return value.toLocaleString('en-US');
}
