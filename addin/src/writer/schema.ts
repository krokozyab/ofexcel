// Whether a new result fits the table it refreshes (CONTRACTS.md §11.2 item 2):
// the headers of the table's first query_columns columns against the new
// result's, without regard to case.

export type SchemaPolicy = 'strict' | 'append';

export interface SchemaPlan {
	/** False: write nothing, say why (E_SCHEMA_CHANGED, on the pane's side). */
	ok: boolean;
	/** The query columns' headers after the write, in table order. */
	headers: string[];
	/** For each table query column, the result column whose values it takes. */
	order: number[];
	/** Headers the result has and the table had not, in result order. */
	added: string[];
	/** Headers the table had and the result has not. */
	removed: string[];
	/** The columns are the same set in another order. */
	reordered: boolean;
}

const lower = (name: string) => name.toLowerCase();

/** `existing`: the table's query column headers; `incoming`: the new result's.
 * strict: anything but the same columns in the same order is refused.
 * append: new columns go after the existing ones, which keep their place and
 * take their values by name; a column gone is refused. `applyNew` is the
 * pane's "Apply new schema": the result's columns as they are. */
export function compareSchema(
	existing: string[],
	incoming: string[],
	policy: SchemaPolicy,
	applyNew = false,
): SchemaPlan {
	const incomingIndex = new Map(incoming.map((name, index) => [lower(name), index]));
	const existingSet = new Set(existing.map(lower));
	const added = incoming.filter((name) => !existingSet.has(lower(name)));
	const removed = existing.filter((name) => !incomingIndex.has(lower(name)));
	const same = added.length === 0 && removed.length === 0;
	const reordered = same && existing.some((name, index) => lower(name) !== lower(incoming[index]));
	const asIs: SchemaPlan = {
		ok: true,
		headers: [...incoming],
		order: incoming.map((_, index) => index),
		added,
		removed,
		reordered,
	};
	if (applyNew) {
		return asIs;
	}
	if (policy === 'strict') {
		return { ...asIs, ok: same && !reordered };
	}
	if (removed.length) {
		return { ...asIs, ok: false };
	}
	const headers = [...existing, ...added];
	return {
		ok: true,
		headers,
		order: headers.map((name) => incomingIndex.get(lower(name))!),
		added,
		removed,
		reordered,
	};
}

/** The pane's words for a refused schema. */
export function describeSchemaChange(plan: SchemaPlan): string {
	const parts: string[] = [];
	if (plan.added.length) {
		parts.push(`new columns: ${plan.added.join(', ')}`);
	}
	if (plan.removed.length) {
		parts.push(`columns gone: ${plan.removed.join(', ')}`);
	}
	if (plan.reordered) {
		parts.push('the columns are in another order');
	}
	return `The query's columns changed (${parts.join('; ')}). The table was not changed`;
}
