// The query file's header as the editor changes it (CONTRACTS.md §2): one
// directive at a time, the rest of the text as the author wrote it. The
// daemon parses the file; this only finds and rewrites `-- @key: value` lines.

export type Engine = 'oracle' | 'local';

/** The text a new query starts with. */
export function newQueryText(engine: Engine): string {
	const body = engine === 'oracle' ? 'SELECT 1 AS one FROM dual' : 'SELECT 1 AS one';
	return `-- @id: new_query\n-- @engine: ${engine}\n-- @version: 1\n${body}\n`;
}

/** Index of the first body line: the header is every line from the top that is
 * blank or starts with "--" (§2.1). */
function headerEnd(lines: string[]): number {
	let end = 0;
	while (end < lines.length && (lines[end].trim() === '' || lines[end].trimStart().startsWith('--'))) {
		end++;
	}
	return end;
}

const DIRECTIVE = /^\s*--\s*@([a-z_]+)(?=[\s:]|$)\s*:?\s*(.*)$/;

function keyOf(line: string): string | undefined {
	return DIRECTIVE.exec(line)?.[1];
}

/** The value of the first `-- @key` line in the header, trimmed; undefined if none. */
export function directive(text: string, key: string): string | undefined {
	const lines = text.split('\n');
	for (let i = 0; i < headerEnd(lines); i++) {
		const match = DIRECTIVE.exec(lines[i]);
		if (match && match[1] === key) {
			return match[2].trim();
		}
	}
	return undefined;
}

/** The engine the header declares; oracle when it says nothing usable. */
export function engineOf(text: string): Engine {
	return directive(text, 'engine') === 'local' ? 'local' : 'oracle';
}

/** Sets `-- @key: value`, replacing the first such line, or inserting one after
 * the header's last directive; undefined removes every such line. */
export function setDirective(text: string, key: string, value: string | undefined): string {
	const lines = text.split('\n');
	const end = headerEnd(lines);
	const line = `-- @${key}: ${value}`;
	let found = false;
	const out: string[] = [];
	for (let i = 0; i < lines.length; i++) {
		if (i < end && keyOf(lines[i]) === key) {
			if (value !== undefined && !found) {
				out.push(line);
			}
			found = true;
			continue;
		}
		out.push(lines[i]);
	}
	if (found || value === undefined) {
		return out.join('\n');
	}
	return insertAfterLastDirective(out, line).join('\n');
}

function insertAfterLastDirective(lines: string[], line: string, preferKey?: string): string[] {
	const end = headerEnd(lines);
	let at = -1;
	let preferred = -1;
	for (let i = 0; i < end; i++) {
		const key = keyOf(lines[i]);
		if (key) {
			at = i;
			if (key === preferKey) {
				preferred = i;
			}
		}
	}
	const index = (preferred >= 0 ? preferred : at) + 1;
	return [...lines.slice(0, index), line, ...lines.slice(index)];
}

export interface ParamDeclaration {
	name: string;
	/** A §2.2 ptype: integer, date, list<id>, enum(A,B)… */
	type: string;
	/** Text as §2.2 reads a default; "" for an empty string; undefined for none. */
	defaultValue?: string;
	description?: string;
}

/** A default as a header writes it (§2.2): quoted when it would not read back
 * as written -- empty, with "|" or a quote, or spaces at either end. */
export function formatDefault(value: string): string {
	if (value === '' || /[|"]/.test(value) || value.trim() !== value) {
		return `"${value.replace(/"/g, '\\"')}"`;
	}
	return value;
}

/** Adds `-- @param name: type [= default] [| description]` after the last
 * @param, or after the last directive. */
export function declareParam(text: string, declaration: ParamDeclaration): string {
	let line = `-- @param ${declaration.name}: ${declaration.type}`;
	if (declaration.defaultValue !== undefined) {
		line += ` = ${formatDefault(declaration.defaultValue)}`;
	}
	if (declaration.description) {
		line += ` | ${declaration.description}`;
	}
	const lines = text.split('\n');
	let lastParam = -1;
	for (let i = 0; i < headerEnd(lines); i++) {
		if (keyOf(lines[i]) === 'param') {
			lastParam = i;
		}
	}
	if (lastParam >= 0) {
		return [...lines.slice(0, lastParam + 1), line, ...lines.slice(lastParam + 1)].join('\n');
	}
	return insertAfterLastDirective(lines, line).join('\n');
}
