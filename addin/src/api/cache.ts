// The client's half of the result cache (SPEC.md §5.8, CONTRACTS.md §10.3):
// results hold until data_version or catalog_version moves, and then all of
// them go at once. The batcher of TASKS.md D8 keeps formula results here.

import type { VersionTracker } from './versions';

export class VersionedCache<V> {
	private readonly entries = new Map<string, V>();

	constructor(versions: VersionTracker) {
		versions.onChange(() => this.clear());
	}

	get(key: string): V | undefined {
		return this.entries.get(key);
	}

	set(key: string, value: V): void {
		this.entries.set(key, value);
	}

	get size(): number {
		return this.entries.size;
	}

	clear(): void {
		this.entries.clear();
	}
}
