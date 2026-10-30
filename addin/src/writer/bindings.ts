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

export function withoutBinding(payload: WorkbookPayload, bindingId: string): WorkbookPayload {
	return { ...payload, bindings: bindingsOf(payload).filter((binding) => binding.binding_id !== bindingId) };
}

/** YYYY-MM-DDTHH:MM:SSZ, as CONTRACTS.md §1 writes time. */
export function nowIso(now = new Date()): string {
	return now.toISOString().slice(0, 19) + 'Z';
}
