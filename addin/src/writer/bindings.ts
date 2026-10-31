// A binding: a query written into an Excel Table, as the workbook keeps it
// (CONTRACTS.md §9.2 Binding). Pure: what changes in the payload when a table
// is created, refreshed, or left partly written.

import type { ParamValue } from '../editor/cells';
import type { WorkbookPayload } from '../workbook/payload';
import type { SchemaPolicy } from './schema';

export type ParamBinding = { ref: string } | { value: ParamValue };

export interface Binding {
	binding_id: string;
	target: { kind: 'table'; sheet: string; table: string };
	query: { id: string; hash: string };
	/** For an Oracle query: the connection and the host it pointed to. */
	connection?: { name: string; host: string };
	params: Record<string, ParamBinding>;
	schema_policy: SchemaPolicy;
	/** How many of the table's first columns belong to the query. */
	query_columns: number;
	last_success?: { at: string; rows: number; truncated: boolean; run_id: string };
	/** A write stopped part way (§11.3): the table holds a mix. */
	partial?: { at: string; run_id: string };
}

export function bindingsOf(payload: WorkbookPayload | undefined): Binding[] {
	return (payload?.bindings ?? []) as Binding[];
}

/** The payload with `binding` in place of the one with its id, or added. */
export function withBinding(payload: WorkbookPayload, binding: Binding): WorkbookPayload {
	const bindings = bindingsOf(payload);
	const found = bindings.some((existing) => existing.binding_id === binding.binding_id);
	return {
		...payload,
		bindings: found
			? bindings.map((existing) => (existing.binding_id === binding.binding_id ? binding : existing))
			: [...bindings, binding],
	};
}

/** After the workbook's queries were saved again (TASKS.md D9): each binding
 * follows its query to its new hash, and loses the parameters the new text no
 * longer declares. `params` gives each query's parameter names by id (lower
 * case); a query without an entry keeps its bindings as they are. */
export function rebind(payload: WorkbookPayload, params: Map<string, string[]>): WorkbookPayload {
	const hashes = new Map(payload.queries.map((query) => [query.id.toLowerCase(), query.hash]));
	return {
		...payload,
		bindings: bindingsOf(payload).map((binding) => {
			// The payload is read as it is; a binding this add-in did not write may lack a query.
			if (typeof binding.query?.id !== 'string') {
				return binding;
			}
			const id = binding.query.id.toLowerCase();
			const hash = hashes.get(id);
			const names = params.get(id);
			if (!hash || hash === binding.query.hash || !names) {
				return binding;
			}
			const kept = Object.fromEntries(
				Object.entries(binding.params).filter(([name]) => names.includes(name.toLowerCase())),
			);
			return { ...binding, query: { ...binding.query, hash }, params: kept };
		}),
	};
}

export function withoutBinding(payload: WorkbookPayload, bindingId: string): WorkbookPayload {
	return { ...payload, bindings: bindingsOf(payload).filter((binding) => binding.binding_id !== bindingId) };
}

/** YYYY-MM-DDTHH:MM:SSZ, as CONTRACTS.md §1 writes time. */
export function nowIso(now = new Date()): string {
	return now.toISOString().slice(0, 19) + 'Z';
}
