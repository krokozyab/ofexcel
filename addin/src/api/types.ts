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
