// Connections to Fusion and signing in to them, as the task pane sees them
// (SPEC.md §3.1, §5.2 item 1; DECISIONS.md R5). Signing in takes 17 to 120
// seconds in a browser window, so it runs in the background: everything else
// stays usable, and the pane counts the seconds. A sign-in about to run out
// is said so 15 minutes ahead.

import type { DaemonClient } from '../api/client';
import { DaemonError } from '../api/errors';
import type { ConnectionDefinition, ConnectionStatus, ConnectionTest, Status } from '../api/types';

/** POST /v1/auth/login answers within 300 s; the client waits longer (CONTRACTS.md §8.3). */
export const SIGN_IN_TIMEOUT_MS = 310_000;
/** How long before a sign-in runs out the pane warns (DECISIONS.md R5). */
export const EXPIRY_WARNING_MS = 15 * 60_000;
/** Where ofquack's SQL report usually lives; the form starts with it. */
export const DEFAULT_REPORT_PATH = '/Custom/Financials/RP_ARB.xdo';

export interface ConnectionView extends ConnectionStatus {
	/** When the sign-in runs out (ms since the epoch), from expires_in. */
	expiresAt?: number;
	/** A sign-in is under way since this time (ms since the epoch). */
	signingInSince?: number;
	/** What the last action on this connection said went wrong. */
	problem?: string;
	/** The last test's result, for as long as the pane shows it. */
	tested?: { ok: boolean; message: string };
}

export type ModelListener = () => void;

export interface ConnectionsModelOptions {
	now?: () => number;
}

export class ConnectionsModel {
	connections: ConnectionView[] = [];
	/** Why the list could not be read; "" when it could. */
	error = '';
	private loaded = false;
	/** The last GET /v1/status, and when it came. */
	private statuses: ConnectionStatus[] = [];
	private statusAt = 0;
	private readonly listeners = new Set<ModelListener>();
	private readonly signingIn = new Map<string, number>();
	private readonly problems = new Map<string, string>();
	private readonly tests = new Map<string, { ok: boolean; message: string }>();
	private readonly now: () => number;

	constructor(
		private readonly client: DaemonClient,
		options: ConnectionsModelOptions = {},
	) {
		this.now = options.now ?? Date.now;
	}

	/** True once the list was read at least once. */
	get ready(): boolean {
		return this.loaded;
	}

	onChange(listener: ModelListener): () => void {
		this.listeners.add(listener);
		return () => this.listeners.delete(listener);
	}

	/** GET /v1/status; the sign-in states come with it. */
	async refresh(): Promise<void> {
		try {
			const status = await this.client.get<Status>('/v1/status');
			this.statuses = status.connections;
			this.statusAt = this.now();
			this.error = '';
			this.loaded = true;
		} catch (error) {
			this.error = messageOf(error);
		}
		this.update();
	}

	/** POST /v1/connections. Rejects with the daemon's error, for the form to show. */
	async create(definition: ConnectionDefinition): Promise<void> {
		await this.client.post<ConnectionDefinition>('/v1/connections', {
			...definition,
			endpoint: normalizeEndpoint(definition.endpoint),
			report_path: definition.report_path.trim(),
		});
		await this.refresh();
	}

	/** POST /v1/auth/login, in the background as far as the caller is concerned:
	 * the promise settles when the person is done in the browser. A second call
	 * for the same connection while one is under way joins nothing; it returns. */
	async signIn(name: string): Promise<void> {
		if (this.signingIn.has(name)) {
			return;
		}
		this.signingIn.set(name, this.now());
		this.problems.delete(name);
		this.tests.delete(name);
		this.update();
		try {
			await this.client.post('/v1/auth/login', { connection: name }, { timeoutMs: SIGN_IN_TIMEOUT_MS });
		} catch (error) {
			this.problems.set(name, messageOf(error));
		} finally {
			this.signingIn.delete(name);
		}
		await this.refresh();
	}

	/** POST /v1/auth/logout: ofquack forgets the token; the browser profile stays. */
	async signOut(name: string): Promise<void> {
		this.problems.delete(name);
		this.tests.delete(name);
		try {
			await this.client.post('/v1/auth/logout', { connection: name });
		} catch (error) {
			this.problems.set(name, messageOf(error));
		}
		await this.refresh();
	}

	/** POST /v1/connections/{name}/test: SELECT 1 FROM dual through the connection. */
	async test(name: string): Promise<void> {
		this.problems.delete(name);
		try {
			const result = await this.client.post<ConnectionTest>(
				`/v1/connections/${encodeURIComponent(name)}/test`,
				undefined,
			);
			this.tests.set(
				name,
				result.ok
					? { ok: true, message: result.user ? `Works, signed in as ${result.user}` : 'Works' }
					: { ok: false, message: result.error?.message ?? 'The test failed' },
			);
		} catch (error) {
			this.problems.set(name, messageOf(error));
		}
		await this.refresh();
	}

	/** Forgets what the last action said about `name`. */
	dismiss(name: string): void {
		this.problems.delete(name);
		this.tests.delete(name);
		this.update();
	}

	private view(connection: ConnectionStatus): ConnectionView {
		const view: ConnectionView = { ...connection };
		if (connection.signed_in && connection.expires_in !== undefined) {
			view.expiresAt = this.statusAt + connection.expires_in * 1000;
		}
		const since = this.signingIn.get(connection.name);
		if (since !== undefined) {
			view.signingInSince = since;
		}
		const problem = this.problems.get(connection.name);
		if (problem) {
			view.problem = problem;
		}
		const tested = this.tests.get(connection.name);
		if (tested) {
			view.tested = tested;
		}
		return view;
	}

	/** The views again, from the last status and what the actions left. */
	private update(): void {
		this.connections = this.statuses.map((connection) => this.view(connection));
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

/** The sign-in is still valid and runs out within EXPIRY_WARNING_MS. */
export function expiresSoon(connection: ConnectionView, now: number): boolean {
	return (
		connection.signed_in &&
		connection.expiresAt !== undefined &&
		connection.expiresAt - now <= EXPIRY_WARNING_MS &&
		connection.expiresAt > now
	);
}

/** The pane's warning for a sign-in that runs out soon, or "" (DECISIONS.md R5). */
export function expiryWarning(connection: ConnectionView, now: number): string {
	if (!expiresSoon(connection, now)) {
		return '';
	}
	// Under a minute still reads as one.
	const minutes = Math.max(1, Math.ceil((connection.expiresAt! - now) / 60_000));
	return `The sign-in expires in ${minutes === 1 ? '1 minute' : `${minutes} minutes`}. Sign in again to keep refreshing.`;
}

/** What a person pastes as the Fusion address, as the daemon wants it:
 * https://host[:port], lower case, no path ("https://fa-x.oraclecloud.com/fscmUI/…"
 * is the same instance). Anything that is not a URL is left for the daemon to refuse. */
export function normalizeEndpoint(input: string): string {
	const text = input.trim();
	const withScheme = /^[a-z][a-z0-9+.-]*:\/\//i.test(text) ? text : `https://${text}`;
	try {
		const url = new URL(withScheme);
		if (url.protocol !== 'https:' || !url.hostname) {
			return text;
		}
		return `https://${url.host.toLowerCase()}`;
	} catch {
		return text;
	}
}

/** "1:05", "12:00": minutes and seconds since `since`. */
export function elapsed(since: number, now: number): string {
	const seconds = Math.max(0, Math.floor((now - since) / 1000));
	return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
}
