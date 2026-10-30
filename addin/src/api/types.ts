// The daemon's API, as far as the add-in uses it (CONTRACTS.md §8.1–8.3).

/** GET /v1/health. */
export interface Health {
	product: string;
	version: string;
	api: { version: string; min_client: string };
	profile: 'hosted' | 'local';
	data_version: number;
	catalog_version: string;
	time: string;
}

/** The body of every error the daemon sends (CONTRACTS.md §8.1). */
export interface ErrorBody {
	error: { code: string; message: string; details?: unknown };
}

export type Environment = 'DEV' | 'TEST' | 'PROD';

/** A connection as GET /v1/status reports it. */
export interface ConnectionStatus {
	name: string;
	environment: Environment;
	/** host[:port] of the endpoint. */
	host: string;
	signed_in: boolean;
	/** The Fusion user, when the daemon knows it. */
	user?: string;
	/** Seconds until the sign-in expires; only when signed in. */
	expires_in?: number;
}

/** GET /v1/status. */
export interface Status {
	connections: ConnectionStatus[];
	active_runs: number;
	warnings: Array<{ code: string; message: string }>;
}

/** POST /v1/connections, and GET /v1/connections' items. */
export interface ConnectionDefinition {
	name: string;
	environment: Environment;
	/** https://host[:port], no path. */
	endpoint: string;
	/** The BI Publisher SQL report, /… */
	report_path: string;
}

/** POST /v1/connections/{name}/test. */
export interface ConnectionTest {
	ok: boolean;
	user?: string;
	error?: { code: string; message: string };
}
