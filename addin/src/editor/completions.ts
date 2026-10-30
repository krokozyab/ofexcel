// Completions in the SQL editor (SPEC.md §5.2 item 2). Oracle SQL: Fusion's
// tables from ofquack's dictionary, and a table's columns after "alias." or
// "table." (CONTRACTS.md §8.3 /v1/dictionary/*). Local SQL: the registered
// queries, which a Local query reads under their ids (§5.3). Each answer is
// kept, so typing on asks the daemon once per prefix and table.

import type { Completion, CompletionContext, CompletionResult, CompletionSource } from '@codemirror/autocomplete';

import type { DaemonClient } from '../api/client';
import type { QuerySummary } from '../api/types';
import type { Engine } from './query_text';

/** Words that follow a table in FROM or JOIN and are not its alias. */
const NOT_ALIASES = new Set([
	'where', 'on', 'join', 'inner', 'left', 'right', 'full', 'outer', 'cross', 'group', 'order', 'having', 'union',
	'minus', 'intersect', 'except', 'fetch', 'offset', 'limit', 'using', 'connect', 'start', 'natural', 'window',
	'qualify', 'pivot', 'unpivot', 'as',
]);

/** The table `alias` names in `sql`: "ap_invoices_all i" or "… AS i", or the
 * table itself when it is used unaliased. Undefined when nothing matches. */
export function tableForAlias(sql: string, alias: string): string | undefined {
	const pattern = /\b(?:from|join)\s+([A-Za-z_][\w$#]*(?:\.[A-Za-z_][\w$#]*)?)(?:\s+(?:as\s+)?([A-Za-z_][\w$#]*))?/gi;
	const wanted = alias.toLowerCase();
	for (const match of sql.matchAll(pattern)) {
		const table = match[1];
		const name = match[2]?.toLowerCase();
		if (name && !NOT_ALIASES.has(name) && name === wanted) {
			return table;
		}
		const bare = table.split('.').pop()!.toLowerCase();
		if (bare === wanted) {
			return table;
		}
	}
	return undefined;
}

export interface CompletionOptions {
	engine: () => Engine;
	/** The connection the dictionary is asked through; "" for none. */
	connection: () => string;
}

/** The CodeMirror source. */
export function sqlCompletions(client: DaemonClient, options: CompletionOptions): CompletionSource {
	const tables = new Map<string, Promise<Completion[]>>();
	const columns = new Map<string, Promise<Completion[]>>();
	let queries: Promise<Completion[]> | undefined;

	const ask = <T>(cache: Map<string, Promise<T>>, key: string, load: () => Promise<T>): Promise<T> => {
		let found = cache.get(key);
		if (!found) {
			found = load();
			cache.set(key, found);
			// A failure is not kept: the next keystroke asks again.
			found.catch(() => cache.delete(key));
		}
		return found;
	};

	return async (context: CompletionContext): Promise<CompletionResult | null> => {
		const word = context.matchBefore(/[A-Za-z_][\w$#]*\.[\w$#]*$|[A-Za-z_][\w$#]*$/);
		if (!word || (word.from === word.to && !context.explicit)) {
			return null;
		}
		const dot = word.text.indexOf('.');
		const engine = options.engine();
		const connection = options.connection();
		try {
			if (dot > 0) {
				if (engine !== 'oracle' || !connection) {
					return null;
				}
				const table = tableForAlias(context.state.doc.toString(), word.text.slice(0, dot));
				if (!table) {
					return null;
				}
				const key = `${connection}\n${table.toUpperCase()}`;
				const options = await ask(columns, key, async () => {
					const answer = await client.get<{ columns: Array<{ name: string; type?: string; description?: string }> }>(
						`/v1/dictionary/columns?connection=${encodeURIComponent(connection)}&table=${encodeURIComponent(table)}`,
					);
					return answer.columns.map((column) => ({
						label: column.name,
						type: 'property',
						detail: column.type,
						info: column.description,
					}));
				});
				return { from: word.from + dot + 1, options, validFor: /^[\w$#]*$/ };
			}
			if (word.text.length < 2 && !context.explicit) {
				return null;
			}
			if (engine === 'local') {
				queries ??= client
					.get<{ queries: QuerySummary[] }>('/v1/queries')
					.then((answer) => answer.queries.map((query) => ({ label: query.id, type: 'class', detail: query.engine })));
				return { from: word.from, options: await queries, validFor: /^[\w$#]*$/ };
			}
			if (!connection) {
				return null;
			}
			// The first two letters choose the list; CodeMirror filters it as typing goes on.
			const prefix = word.text.slice(0, 2).toUpperCase();
			const options = await ask(tables, `${connection}\n${prefix}`, async () => {
				const answer = await client.get<{ tables: Array<{ name: string; description?: string }> }>(
					`/v1/dictionary/tables?connection=${encodeURIComponent(connection)}&q=${encodeURIComponent(prefix)}&limit=1000`,
				);
				return answer.tables.map((table) => ({ label: table.name, type: 'type', info: table.description }));
			});
			return { from: word.from, options, validFor: /^[\w$#]*$/ };
		} catch {
			// No dictionary (not signed in, daemon away): no completions, no noise.
			return null;
		}
	};
}
