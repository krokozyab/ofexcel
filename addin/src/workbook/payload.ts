// What a workbook keeps of QExcel (CONTRACTS.md §9.2): one CustomXmlPart in
// urn:qexcel:workbook:v1 with the payload as JSON in CDATA. No secrets, tokens
// or passwords are ever in it. Reading it when a workbook opens, and asking
// whether to trust it, is TASKS.md D5's.

export const WORKBOOK_NAMESPACE = 'urn:qexcel:workbook:v1';

export interface WorkbookQuery {
	id: string;
	hash: string;
	text: string;
	/** The Fusion user who saved it, when known. */
	saved_by?: string;
	saved_at: string;
}

export interface WorkbookPayload {
	schema: 1;
	workbook_id: string;
	queries: WorkbookQuery[];
	/** Kept as they are here; TASKS.md D4 and D7 write them. */
	bindings: unknown[];
	imports: unknown[];
}

export function emptyPayload(workbookId: string): WorkbookPayload {
	return { schema: 1, workbook_id: workbookId, queries: [], bindings: [], imports: [] };
}

/** The part's XML. "]]>" can occur in the JSON only inside a string, where
 * "]]\u003e" means the same and does not end the CDATA section. */
export function payloadToXml(payload: WorkbookPayload): string {
	const json = JSON.stringify(payload).replace(/\]\]>/g, ']]\\u003e');
	return `<qexcel xmlns="${WORKBOOK_NAMESPACE}" schema="1"><payload><![CDATA[${json}]]></payload></qexcel>`;
}

/** The payload in a part's XML. Throws when the part is not one this version reads. */
export function payloadFromXml(xml: string): WorkbookPayload {
	const start = xml.indexOf('<![CDATA[');
	const end = xml.lastIndexOf(']]>');
	if (start < 0 || end < start) {
		throw new Error('The workbook\'s QExcel part has no payload');
	}
	const payload = JSON.parse(xml.slice(start + 9, end)) as Partial<WorkbookPayload>;
	if (payload.schema !== 1 || typeof payload.workbook_id !== 'string' || !Array.isArray(payload.queries)) {
		throw new Error(`The workbook's QExcel part has schema ${String(payload.schema)}, which this add-in does not read`);
	}
	return {
		schema: 1,
		workbook_id: payload.workbook_id,
		queries: payload.queries,
		bindings: Array.isArray(payload.bindings) ? payload.bindings : [],
		imports: Array.isArray(payload.imports) ? payload.imports : [],
	};
}

/** The payload with `query` in place of the one with the same id (without
 * regard to case), or added at the end: a workbook keeps one text per query. */
export function withQuery(payload: WorkbookPayload, query: WorkbookQuery): WorkbookPayload {
	const lower = query.id.toLowerCase();
	const index = payload.queries.findIndex((existing) => existing.id.toLowerCase() === lower);
	const queries = [...payload.queries];
	if (index >= 0) {
		queries[index] = query;
	} else {
		queries.push(query);
	}
	return { ...payload, queries };
}

/** A UUID v4 in lower case (§9.1), from 16 random bytes. */
export function uuidV4(random: Uint8Array): string {
	const bytes = Uint8Array.from(random.slice(0, 16));
	bytes[6] = (bytes[6] & 0x0f) | 0x40;
	bytes[8] = (bytes[8] & 0x3f) | 0x80;
	const hex = Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');
	return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}
