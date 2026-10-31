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

export interface Warning {
	code: string;
	message: string;
}

/** A parameter as a query declares it (CONTRACTS.md §8.2 ParamDef). */
export interface ParamDef {
	name: string;
	type: string;
	default?: string;
	description?: string;
}

/** A result column: its contract type (§3.2) and the header to show. */
export interface ColumnInfo {
	name: string;
	header: string;
	type: string;
}

/** A registered query (CONTRACTS.md §8.2 QuerySummary). */
export interface QuerySummary {
	id: string;
	hash: string;
	version: number;
	engine: 'oracle' | 'local';
	title?: string;
	description?: string;
	params: ParamDef[];
	columns: ColumnInfo[];
	source: { kind: 'project' | 'workbook' | 'example'; ref: string };
	trusted: boolean;
}

export interface Problem {
	code: string;
	message: string;
	line?: number;
}

/** POST /v1/queries/validate. */
export interface Validation {
	ok: boolean;
	header?: QuerySummary;
	params_in_text: string[];
	errors: Problem[];
	warnings: Warning[];
}

/** POST /v1/preview. */
export interface Preview {
	columns: ColumnInfo[];
	rows: unknown[][];
	elapsed_ms: number;
	truncated: boolean;
	rendered_sql?: string;
	warnings: Warning[];
}

/** Parameters as the daemon keeps them (CONTRACTS.md §1.2): strings, lists of strings. */
export type CanonicalParams = Record<string, string | string[]>;

/** A run (CONTRACTS.md §8.2 Run), as far as the add-in reads it. */
export interface Run {
	run_id: string;
	instance_key: string;
	query: { id: string; hash: string };
	connection: string;
	environment?: string;
	params: CanonicalParams;
	status: 'queued' | 'running' | 'succeeded' | 'failed' | 'cancelled';
	started_at: string;
	finished_at?: string;
	rows_fetched: number;
	truncated: boolean;
	parent_run_id?: string;
	depends_on: string[];
	error?: { code: string; message: string };
}

/** GET /v1/missing: data a formula of the workbook asked for and did not find. */
export interface MissingEntry {
	query: { id: string; hash: string };
	connection: string;
	params: CanonicalParams;
	first_seen: string;
	last_seen: string;
	requests: number;
}
