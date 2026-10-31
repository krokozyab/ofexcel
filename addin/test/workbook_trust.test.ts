// Opening a workbook (TASKS.md D5, CONTRACTS.md §9.3): its queries registered,
// the untrusted ones asked about with their SQL and tables, Trust and Don't
// trust, a binding on another host's connection.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { DaemonClient } from '../src/api/client';
import type { ConnectionStatus } from '../src/api/types';
import type { WorkbookAccess } from '../src/editor/model';
import { emptyPayload, type WorkbookPayload } from '../src/workbook/payload';
import { tablesInSql } from '../src/workbook/sql_tables';
import { connectionProblem, TRUST_WARNING, WorkbookTrust } from '../src/workbook/trust';
import type { Binding } from '../src/writer/bindings';

const WORKBOOK = '0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d';

const INVOICES = "-- @id: invoices\n-- @engine: oracle\n-- @version: 1\n-- @title: Open invoices\n" +
	"SELECT i.* FROM ap_invoices_all i JOIN poz_suppliers s ON s.vendor_id = i.vendor_id WHERE i.note <> 'FROM x'\n";
const LOCAL = '-- @id: totals\n-- @engine: local\n-- @version: 1\nSELECT count(*) FROM invoices\n';

const DEV: ConnectionStatus = { name: 'dev', environment: 'DEV', host: 'fa-dev.example.com', signed_in: true };

function binding(connection?: { name: string; host: string }): Binding {
	return {
		binding_id: 'b1',
		target: { kind: 'table', sheet: 'Sheet1', table: 'tblInvoices' },
		query: { id: 'invoices', hash: 'h-invoices' },
		...(connection ? { connection } : {}),
		params: {},
		schema_policy: 'strict',
		query_columns: 3,
	};
}

/** A daemon that knows `trusted` hashes already. */
function fakeDaemon(trusted: string[]) {
	const calls: Array<{ path: string; body: any }> = [];
	const fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
		const path = String(input).replace('https://localhost:47321', '');
		const body = init?.body ? JSON.parse(String(init.body)) : undefined;
		calls.push({ path, body });
		const json = (value: unknown) => new Response(JSON.stringify(value), { status: 200 });
		if (path === `/v1/workbooks/${WORKBOOK}/queries`) {
			return json({
				queries: body.queries.map(({ text }: { text: string }) => {
					const id = /@id: (\w+)/.exec(text)![1];
					const hash = `h-${id}`;
					const title = /@title: (.+)/.exec(text)?.[1];
					return { id, hash, ...(title ? { title } : {}), trusted: trusted.includes(hash) };
				}),
			});
		}
		if (path === '/v1/trust') {
			return json({ trusted: body.hashes });
		}
		return new Response('{}', { status: 404 });
	};
	return { calls, client: new DaemonClient({ baseUrl: 'https://localhost:47321', fetch: fetch as typeof globalThis.fetch }) };
}

function workbook(payload?: WorkbookPayload): WorkbookAccess {
	return { ensureId: async () => WORKBOOK, read: async () => payload, write: async () => {} };
}

function payload(): WorkbookPayload {
	const value = emptyPayload(WORKBOOK);
	value.queries.push(
		{ id: 'invoices', hash: 'h-invoices', text: INVOICES, saved_by: 'author@example.com', saved_at: '2026-10-30T12:00:00Z' },
		{ id: 'totals', hash: 'h-totals', text: LOCAL, saved_at: '2026-10-30T12:01:00Z' },
	);
	value.bindings.push(binding({ name: 'dev', host: 'fa-dev.example.com' }));
	return value;
}

test('the tables a query reads, outside literals and comments', () => {
	assert.deepEqual(tablesInSql("SELECT * FROM a, fusion.b x, c WHERE d = 'FROM e' -- JOIN f\n JOIN g ON 1 = 1"), [
		'a',
		'fusion.b',
		'c',
		'g',
	]);
	assert.deepEqual(tablesInSql('SELECT * FROM (SELECT * FROM inner_t) t JOIN "Quoted" q ON 1 = 1'), ['inner_t', '"Quoted"']);
	assert.deepEqual(tablesInSql("SELECT q'[FROM fake]' FROM real_t /* FROM no */"), ['real_t']);
	assert.deepEqual(tablesInSql('SELECT * FROM t1 LEFT JOIN t2 ON 1 = 1, t3'), ['t1', 't2']);
	assert.deepEqual(tablesInSql('SELECT 1'), []);
});

test('the queries not trusted are asked about, in full, and do not run meanwhile', async () => {
	const daemon = fakeDaemon(['h-totals']);
	const trust = new WorkbookTrust(daemon.client, workbook(payload()), () => [DEV]);
	assert.ok(trust.allows('h-invoices'));
	await trust.open();

	assert.deepEqual(daemon.calls[0].body, { queries: [{ text: INVOICES }, { text: LOCAL }] });
	assert.equal(trust.state.kind, 'asking');
	const asked = trust.state.kind === 'asking' ? trust.state.queries : [];
	assert.equal(asked.length, 1);
	assert.deepEqual(asked[0], {
		id: 'invoices',
		hash: 'h-invoices',
		title: 'Open invoices',
		savedBy: 'author@example.com',
		savedAt: '2026-10-30T12:00:00Z',
		text: INVOICES,
		tables: ['ap_invoices_all', 'poz_suppliers'],
		connections: [{ name: 'dev', host: 'fa-dev.example.com', environment: 'DEV' }],
	});
	assert.ok(!trust.allows('h-invoices'));
	assert.ok(trust.allows('h-totals'));
	assert.match(TRUST_WARNING, /run under your Fusion account/);

	await trust.trust();
	assert.deepEqual(daemon.calls.at(-1), { path: '/v1/trust', body: { workbook_id: WORKBOOK, hashes: ['h-invoices'] } });
	assert.equal(trust.state.kind, 'decided');
	assert.ok(trust.allows('h-invoices'));
});

test("Don't trust leaves them blocked; a workbook without queries asks nothing", async () => {
	const declined = new WorkbookTrust(fakeDaemon([]).client, workbook(payload()), () => []);
	await declined.open();
	declined.decline();
	assert.equal(declined.state.kind, 'decided');
	assert.ok(!declined.allows('h-invoices'));
	assert.ok(!declined.allows('h-totals'));

	const daemon = fakeDaemon([]);
	const empty = new WorkbookTrust(daemon.client, workbook(undefined), () => []);
	await empty.open();
	assert.equal(empty.state.kind, 'decided');
	assert.equal(daemon.calls.length, 0);

	const allTrusted = new WorkbookTrust(fakeDaemon(['h-invoices', 'h-totals']).client, workbook(payload()), () => []);
	await allTrusted.open();
	assert.equal(allTrusted.state.kind, 'decided');
});

test('a binding on another host, or on a connection the user lacks, says so with both hosts', () => {
	assert.equal(connectionProblem(binding({ name: 'dev', host: 'FA-DEV.example.com' }), [DEV]), '');
	assert.equal(connectionProblem(binding(), [DEV]), '');
	assert.equal(
		connectionProblem(binding({ name: 'dev', host: 'fa-other.example.com' }), [DEV]),
		'The workbook was made for fa-other.example.com; your connection dev points to fa-dev.example.com. ' +
			'Choose the connection to run on',
	);
	assert.match(connectionProblem(binding({ name: 'prod', host: 'fa-prod.example.com' }), [DEV]), /which you do not have/);
});
