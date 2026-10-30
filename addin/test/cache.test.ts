// The client's result cache (SPEC.md §5.8): dropped whole when data_version or
// catalog_version moves, kept while they do not.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { VersionedCache } from '../src/api/cache';
import { VersionTracker } from '../src/api/versions';

test('the cache is dropped when either version changes, and only then', () => {
	const versions = new VersionTracker();
	const cache = new VersionedCache<number>(versions);

	// The first report fills nothing in; it is where the versions start.
	cache.set('a', 1);
	versions.observe(1, 'c1');
	assert.equal(cache.get('a'), 1);

	cache.set('b', 2);
	versions.observe(1, 'c1');
	versions.observe(undefined, undefined);
	assert.equal(cache.size, 2);

	versions.observe(2, undefined);
	assert.equal(cache.size, 0);

	cache.set('c', 3);
	versions.observe(undefined, 'c2');
	assert.equal(cache.get('c'), undefined);
	assert.deepEqual(versions.get(), { data: 2, catalog: 'c2' });
});

test('version headers: numbers, missing ones and garbage', () => {
	const versions = new VersionTracker();
	versions.observeHeaders(new Headers({ 'X-OF-Data-Version': '12', 'X-OF-Catalog-Version': '' }));
	assert.deepEqual(versions.get(), { data: 12, catalog: '' });
	versions.observeHeaders(new Headers({ 'X-OF-Data-Version': 'x' }));
	assert.deepEqual(versions.get(), { data: 12, catalog: '' });
	versions.observeHeaders(new Headers());
	assert.deepEqual(versions.get(), { data: 12, catalog: '' });
});

test('a listener can stop listening', () => {
	const versions = new VersionTracker();
	let calls = 0;
	const stop = versions.onChange(() => calls++);
	versions.observe(1, 'a');
	versions.observe(2, 'a');
	stop();
	versions.observe(3, 'a');
	assert.equal(calls, 1);
});
