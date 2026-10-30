// What the workbook keeps (CONTRACTS.md §9.2): the payload in CDATA, "]]>" in
// a query text included; one text per query id; the workbook's UUID.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
	emptyPayload,
	payloadFromXml,
	payloadToXml,
	uuidV4,
	withQuery,
	WORKBOOK_NAMESPACE,
} from '../src/workbook/payload';

const WORKBOOK = '0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d';

test('the payload round-trips, "]]>" inside a query included', () => {
	const text = "-- @id: q\n-- @engine: local\n-- @version: 1\nSELECT ']]>' AS tricky, '<x>&amp;' AS markup\n";
	const payload = withQuery(emptyPayload(WORKBOOK), {
		id: 'q',
		hash: 'a'.repeat(64),
		text,
		saved_at: '2026-10-30T12:00:00Z',
	});
	const xml = payloadToXml(payload);
	assert.ok(xml.startsWith(`<qexcel xmlns="${WORKBOOK_NAMESPACE}" schema="1"><payload><![CDATA[`));
	assert.ok(xml.endsWith(']]></payload></qexcel>'));
	// The only "]]>" left is the one that ends the section.
	assert.equal(xml.split(']]>').length, 2);
	assert.ok(xml.includes(']]\\u003e'));
	assert.deepEqual(payloadFromXml(xml), payload);
	assert.equal(payloadFromXml(xml).queries[0].text, text);
	// As Office may hand it back: with a declaration and whitespace.
	assert.deepEqual(payloadFromXml(`<?xml version="1.0" encoding="utf-8"?>\n${xml}\n`), payload);
});

test('a part this add-in cannot read is refused, not guessed at', () => {
	assert.throws(() => payloadFromXml('<qexcel/>'), /no payload/);
	assert.throws(
		() => payloadFromXml(`<qexcel><payload><![CDATA[{"schema":2,"workbook_id":"x","queries":[]}]]></payload></qexcel>`),
		/schema 2/,
	);
	// Missing lists are empty.
	const minimal = payloadFromXml(
		`<qexcel><payload><![CDATA[{"schema":1,"workbook_id":"${WORKBOOK}","queries":[]}]]></payload></qexcel>`,
	);
	assert.deepEqual(minimal, emptyPayload(WORKBOOK));
});

test('one text per query id: a save replaces, without regard to case, and keeps the order', () => {
	let payload = emptyPayload(WORKBOOK);
	payload = withQuery(payload, { id: 'a', hash: '1', text: 'A1', saved_at: 't1' });
	payload = withQuery(payload, { id: 'b', hash: '2', text: 'B1', saved_at: 't1' });
	payload = withQuery(payload, { id: 'A', hash: '3', text: 'A2', saved_at: 't2', saved_by: 'analyst' });
	assert.deepEqual(
		payload.queries.map((query) => [query.id, query.text, query.saved_by]),
		[
			['A', 'A2', 'analyst'],
			['b', 'B1', undefined],
		],
	);
});

test('the workbook id is a lower-case UUID v4', () => {
	const id = uuidV4(new Uint8Array(16).fill(0xff));
	assert.match(id, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
	assert.equal(uuidV4(new Uint8Array(16)), '00000000-0000-4000-8000-000000000000');
});
