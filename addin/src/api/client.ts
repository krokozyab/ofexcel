// The daemon's HTTP API as the add-in calls it (CONTRACTS.md §8): JSON in and
// out, X-OF-Session in the local profile, X-OF-Workbook when the workbook has
// an id, ErrorBody turned into DaemonError, and the version headers of every
// response followed for the caches.

import { DaemonError, E_BAD_RESPONSE, E_OFFLINE, E_TIMEOUT, readError } from './errors';
import type { Health } from './types';
import { VersionTracker } from './versions';

/** The API version this add-in speaks (CONTRACTS.md §8.3 /v1/health). */
export const CLIENT_API_VERSION = '1.0';

/** How long a request waits by default. Sign-in waits longer (§8.3 /v1/auth/login). */
export const DEFAULT_TIMEOUT_MS = 30_000;

export interface DaemonClientOptions {
	/** https://localhost:47321 or https://127.0.0.1:47321; no trailing slash. */
	baseUrl: string;
	/** The local profile's session token, from <meta name="qexcel-session">. */
	sessionToken?: string;
	/** The workbook's id for X-OF-Workbook; "" while it has none. */
	workbookId?: () => string;
	fetch?: typeof fetch;
	timeoutMs?: number;
}

export interface RequestOptions {
	timeoutMs?: number;
}

export class DaemonClient {
	readonly baseUrl: string;
	readonly versions = new VersionTracker();
	private readonly sessionToken: string;
	private readonly workbookId: () => string;
	private readonly fetchImpl: typeof fetch;
	private readonly timeoutMs: number;

	constructor(options: DaemonClientOptions) {
		this.baseUrl = options.baseUrl.replace(/\/+$/, '');
		this.sessionToken = options.sessionToken ?? '';
		this.workbookId = options.workbookId ?? (() => '');
		// Bound: a WebView's fetch refuses to be called with another `this`.
		this.fetchImpl = options.fetch ?? fetch.bind(globalThis);
		this.timeoutMs = options.timeoutMs ?? DEFAULT_TIMEOUT_MS;
	}

	get<T>(path: string, options?: RequestOptions): Promise<T> {
		return this.request<T>('GET', path, undefined, options);
	}

	post<T>(path: string, body: unknown, options?: RequestOptions): Promise<T> {
		return this.request<T>('POST', path, body, options);
	}

	put<T>(path: string, body: unknown, options?: RequestOptions): Promise<T> {
		return this.request<T>('PUT', path, body, options);
	}

	delete<T>(path: string, options?: RequestOptions): Promise<T> {
		return this.request<T>('DELETE', path, undefined, options);
	}

	/** GET /v1/health; its body's versions count as much as the headers. */
	async health(): Promise<Health> {
		const health = await this.get<Health>('/v1/health');
		this.versions.observe(health.data_version, health.catalog_version);
		return health;
	}

	/** Resolves with the parsed body (undefined for 204); rejects with DaemonError. */
	async request<T>(method: string, path: string, body?: unknown, options: RequestOptions = {}): Promise<T> {
		const headers: Record<string, string> = {};
		if (body !== undefined) {
			headers['Content-Type'] = 'application/json';
		}
		if (this.sessionToken) {
			headers['X-OF-Session'] = this.sessionToken;
		}
		const workbook = this.workbookId();
		if (workbook) {
			headers['X-OF-Workbook'] = workbook;
		}

		const controller = new AbortController();
		const timer = setTimeout(() => controller.abort(), options.timeoutMs ?? this.timeoutMs);
		let response: Response;
		try {
			response = await this.fetchImpl(this.baseUrl + path, {
				method,
				headers,
				body: body === undefined ? undefined : JSON.stringify(body),
				cache: 'no-store',
				signal: controller.signal,
			});
		} catch {
			if (controller.signal.aborted) {
				throw new DaemonError(E_TIMEOUT, `The QExcel daemon did not answer ${method} ${path} in time`, 0);
			}
			// fetch says no more than "Load failed": a stopped daemon and an
			// untrusted certificate look the same from here.
			throw new DaemonError(
				E_OFFLINE,
				`The QExcel daemon does not answer at ${this.baseUrl}. Start ofexceld, ` +
					'and check that its certificate is installed (ofexceld install-cert)',
				0,
			);
		} finally {
			clearTimeout(timer);
		}

		this.versions.observeHeaders(response.headers);
		if (!response.ok) {
			throw await readError(response);
		}
		if (response.status === 204) {
			return undefined as T;
		}
		const text = await response.text();
		try {
			return JSON.parse(text) as T;
		} catch {
			throw new DaemonError(E_BAD_RESPONSE, `The daemon's answer to ${method} ${path} is not JSON`, response.status);
		}
	}
}

/** "1.10" > "1.9": versions compared number by number. */
export function compareApiVersions(a: string, b: string): number {
	const left = a.split('.').map((part) => Number.parseInt(part, 10) || 0);
	const right = b.split('.').map((part) => Number.parseInt(part, 10) || 0);
	for (let i = 0; i < Math.max(left.length, right.length); i++) {
		const difference = (left[i] ?? 0) - (right[i] ?? 0);
		if (difference !== 0) {
			return difference;
		}
	}
	return 0;
}

/** The daemon accepts this add-in: same major API version, and not older than its min_client. */
export function isCompatible(health: Health, clientVersion = CLIENT_API_VERSION): boolean {
	const major = (version: string) => version.split('.')[0];
	return (
		major(health.api.version) === major(clientVersion) && compareApiVersions(clientVersion, health.api.min_client) >= 0
	);
}
