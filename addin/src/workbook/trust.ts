// Opening a workbook (CONTRACTS.md §9.3; SPEC.md §8.3): its queries are
// registered with the daemon, and those this machine has not seen before --
// not in a project, not trusted earlier -- wait for the person to say whether
// they may run under their own Fusion account. Until then, and after "Don't
// trust", their tables do not refresh.

import type { DaemonClient } from '../api/client';
import { DaemonError } from '../api/errors';
import type { ConnectionStatus, QuerySummary } from '../api/types';
import type { WorkbookAccess } from '../editor/model';
import type { Binding } from '../writer/bindings';
import { bindingsOf } from '../writer/bindings';
import type { WorkbookQuery } from './payload';
import { tablesInSql } from './sql_tables';

/** What the dialog says under every query (§9.3 item 3; English, DECISIONS.md R7). */
export const TRUST_WARNING =
	'The query will run under your Fusion account. If you save and send the workbook, the data it fetched goes with it.';

/** One query the person is asked about. */
export interface TrustQuery {
	id: string;
	hash: string;
	title?: string;
	savedBy?: string;
	savedAt: string;
	text: string;
	/** The tables after FROM and JOIN: for reading, not for security. */
	tables: string[];
	/** Where its tables run: the bindings' connections and the environments. */
	connections: Array<{ name: string; host: string; environment?: string }>;
}

export type TrustState =
	| { kind: 'idle' }
	| { kind: 'asking'; queries: TrustQuery[] }
	| { kind: 'decided' };

export type TrustListener = () => void;

export class WorkbookTrust {
	state: TrustState = { kind: 'idle' };
	/** Why the workbook's queries could not be registered; "" when they were. */
	error = '';
	/** Hashes the person declined, or has not been asked about yet. */
	private readonly blocked = new Set<string>();
	private workbookId = '';
	private readonly listeners = new Set<TrustListener>();

	constructor(
		private readonly client: DaemonClient,
		private readonly workbook: WorkbookAccess,
		private readonly connections: () => ConnectionStatus[],
	) {}

	onChange(listener: TrustListener): () => void {
		this.listeners.add(listener);
		return () => this.listeners.delete(listener);
	}

	/** Whether the query `hash` may run: not waiting for an answer, not declined. */
	allows(hash: string): boolean {
		return !this.blocked.has(hash);
	}

	/** §9.3 items 1–3: read the workbook's part, register its texts, ask about
	 * the ones not trusted. A workbook without a part has nothing to ask. */
	async open(): Promise<void> {
		const payload = await this.workbook.read();
		if (!payload || payload.queries.length === 0) {
			this.state = { kind: 'decided' };
			this.emit();
			return;
		}
		this.workbookId = payload.workbook_id;
		let registered: QuerySummary[];
		try {
			registered = (
				await this.client.put<{ queries: QuerySummary[] }>(`/v1/workbooks/${payload.workbook_id}/queries`, {
					queries: payload.queries.map((query) => ({ text: query.text })),
				})
			).queries;
		} catch (error) {
			// Nothing of the workbook may run until its texts are known.
			for (const query of payload.queries) {
				this.blocked.add(query.hash);
			}
			this.error = error instanceof DaemonError ? error.message : String(error);
			this.state = { kind: 'decided' };
			this.emit();
			return;
		}
		const untrusted = registered
			.map((summary, index) => ({ summary, query: payload.queries[index] }))
			.filter(({ summary }) => !summary.trusted);
		for (const { summary } of untrusted) {
			this.blocked.add(summary.hash);
		}
		this.error = '';
		this.state = untrusted.length
			? {
					kind: 'asking',
					queries: untrusted.map(({ summary, query }) =>
						this.describe(summary, query, bindingsOf(payload)),
					),
				}
			: { kind: 'decided' };
		this.emit();
	}

	/** "Trust": POST /v1/trust for every query asked about. */
	async trust(): Promise<void> {
		if (this.state.kind !== 'asking') {
			return;
		}
		const hashes = this.state.queries.map((query) => query.hash);
		try {
			await this.client.post('/v1/trust', { workbook_id: this.workbookId, hashes });
		} catch (error) {
			this.error = error instanceof DaemonError ? error.message : String(error);
			this.emit();
			return;
		}
		for (const hash of hashes) {
			this.blocked.delete(hash);
		}
		this.error = '';
		this.state = { kind: 'decided' };
		this.emit();
	}

	/** "Don't trust": the queries stay blocked for as long as the pane is open. */
	decline(): void {
		this.state = { kind: 'decided' };
		this.emit();
	}

	private describe(summary: QuerySummary, query: WorkbookQuery, bindings: Binding[]): TrustQuery {
		const known = this.connections();
		const connections = new Map<string, TrustQuery['connections'][number]>();
		for (const binding of bindings) {
			if (binding.query.hash !== summary.hash || !binding.connection) {
				continue;
			}
			const environment = known.find((connection) => connection.name === binding.connection!.name)?.environment;
			connections.set(binding.connection.name, { ...binding.connection, ...(environment ? { environment } : {}) });
		}
		return {
			id: summary.id,
			hash: summary.hash,
			...(summary.title ? { title: summary.title } : {}),
			...(query.saved_by ? { savedBy: query.saved_by } : {}),
			savedAt: query.saved_at,
			text: query.text,
			tables: tablesInSql(bodyOf(query.text)),
			connections: [...connections.values()],
		};
	}

	private emit(): void {
		for (const listener of [...this.listeners]) {
			listener();
		}
	}
}

/** The SQL after the `-- @…` header: the header's words are not tables. */
function bodyOf(text: string): string {
	const lines = text.split('\n');
	let end = 0;
	while (end < lines.length && (lines[end].trim() === '' || lines[end].trimStart().startsWith('--'))) {
		end++;
	}
	return lines.slice(end).join('\n');
}

/** §9.3 item 5: the binding's connection is not the user's connection of that
 * name -- another host, or no such connection. "" when it is, or the binding
 * has no connection (a Local query). */
export function connectionProblem(binding: Binding, connections: ConnectionStatus[]): string {
	if (!binding.connection) {
		return '';
	}
	const { name, host } = binding.connection;
	const mine = connections.find((connection) => connection.name === name);
	if (!mine) {
		return `This table runs on connection ${name} (${host}), which you do not have. Choose one of your connections`;
	}
	if (mine.host.toLowerCase() !== host.toLowerCase()) {
		return `The workbook was made for ${host}; your connection ${name} points to ${mine.host}. Choose the connection to run on`;
	}
	return '';
}
