// data_version and catalog_version as the daemon last reported them
// (CONTRACTS.md §8.1, §10.3): every /v1 response carries both in headers, and
// /v1/health and /v1/batch in their bodies too. A change of either means
// cached results are stale (SPEC.md §5.8).

export interface Versions {
	data: number;
	catalog: string;
}

export type VersionListener = (current: Versions, previous: Versions) => void;

export class VersionTracker {
	private current: Versions | undefined;
	private readonly listeners = new Set<VersionListener>();

	/** What the daemon last reported; undefined before the first response. */
	get(): Versions | undefined {
		return this.current;
	}

	/** Called when either version moves; not for the first report, as nothing was cached before it. */
	onChange(listener: VersionListener): () => void {
		this.listeners.add(listener);
		return () => this.listeners.delete(listener);
	}

	/** Records what a response said; undefined is "not said" and keeps what was known. */
	observe(data: number | undefined, catalog: string | undefined): void {
		const previous = this.current;
		const next: Versions = {
			data: data ?? previous?.data ?? 0,
			catalog: catalog ?? previous?.catalog ?? '',
		};
		if (previous === undefined) {
			if (data !== undefined || catalog !== undefined) {
				this.current = next;
			}
			return;
		}
		if (next.data === previous.data && next.catalog === previous.catalog) {
			return;
		}
		this.current = next;
		for (const listener of [...this.listeners]) {
			listener(next, previous);
		}
	}

	/** The versions from a response's X-OF-Data-Version and X-OF-Catalog-Version. */
	observeHeaders(headers: Headers): void {
		const data = headers.get('X-OF-Data-Version');
		const catalog = headers.get('X-OF-Catalog-Version');
		const number = data === null || data.trim() === '' ? undefined : Number(data);
		this.observe(Number.isFinite(number) ? number : undefined, catalog ?? undefined);
	}
}
