// Whether the daemon is there, and which (SPEC.md §5.8): /v1/health every 30
// seconds and whenever Excel gets the focus back. Each check also carries the
// versions to the caches.

import { DaemonClient, isCompatible } from './client';
import { DaemonError, E_OFFLINE } from './errors';
import type { Health } from './types';

export const HEALTH_INTERVAL_MS = 30_000;

export type DaemonState =
	| { kind: 'connecting' }
	| { kind: 'online'; health: Health }
	/** The daemon is there, but this add-in is too old or too new for its API. */
	| { kind: 'incompatible'; health: Health }
	| { kind: 'offline'; error: DaemonError };

/** setInterval and clearInterval, replaceable in tests. */
export interface Timer {
	every(ms: number, callback: () => void): () => void;
}

const realTimer: Timer = {
	every(ms, callback) {
		const handle = setInterval(callback, ms);
		return () => clearInterval(handle);
	},
};

export interface HealthMonitorOptions {
	onChange: (state: DaemonState) => void;
	intervalMs?: number;
	timer?: Timer;
}

export class HealthMonitor {
	private current: DaemonState = { kind: 'connecting' };
	private running: Promise<DaemonState> | undefined;
	private readonly stops: Array<() => void> = [];

	constructor(
		private readonly client: DaemonClient,
		private readonly options: HealthMonitorOptions,
	) {}

	get state(): DaemonState {
		return this.current;
	}

	/** Checks now, then every interval, and on `focus` from `focusTarget` and
	 * `visibilitychange` to visible from `visibilityTarget`. */
	start(focusTarget?: EventTarget, visibilityTarget?: Document): void {
		void this.check();
		this.stops.push(
			(this.options.timer ?? realTimer).every(this.options.intervalMs ?? HEALTH_INTERVAL_MS, () => void this.check()),
		);
		if (focusTarget) {
			const onFocus = () => void this.check();
			focusTarget.addEventListener('focus', onFocus);
			this.stops.push(() => focusTarget.removeEventListener('focus', onFocus));
		}
		if (visibilityTarget) {
			const onVisible = () => {
				if (visibilityTarget.visibilityState === 'visible') {
					void this.check();
				}
			};
			visibilityTarget.addEventListener('visibilitychange', onVisible);
			this.stops.push(() => visibilityTarget.removeEventListener('visibilitychange', onVisible));
		}
	}

	stop(): void {
		for (const stop of this.stops.splice(0)) {
			stop();
		}
	}

	/** One check; a check already under way is shared rather than repeated. */
	check(): Promise<DaemonState> {
		if (!this.running) {
			this.running = this.checkOnce().finally(() => {
				this.running = undefined;
			});
		}
		return this.running;
	}

	private async checkOnce(): Promise<DaemonState> {
		let next: DaemonState;
		try {
			const health = await this.client.health();
			next = isCompatible(health) ? { kind: 'online', health } : { kind: 'incompatible', health };
		} catch (error) {
			next = {
				kind: 'offline',
				error: error instanceof DaemonError ? error : new DaemonError(E_OFFLINE, String(error), 0),
			};
		}
		if (!sameState(this.current, next)) {
			this.current = next;
			this.options.onChange(next);
		}
		return next;
	}
}

/** What the status bar would show differently; `time` moves on every check. */
function sameState(a: DaemonState, b: DaemonState): boolean {
	if (a.kind !== b.kind) {
		return false;
	}
	if ((a.kind === 'online' || a.kind === 'incompatible') && (b.kind === 'online' || b.kind === 'incompatible')) {
		return (
			a.health.version === b.health.version &&
			a.health.profile === b.health.profile &&
			a.health.api.version === b.health.api.version &&
			a.health.api.min_client === b.health.api.min_client
		);
	}
	if (a.kind === 'offline' && b.kind === 'offline') {
		return a.error.code === b.error.code && a.error.message === b.error.message;
	}
	return true;
}
