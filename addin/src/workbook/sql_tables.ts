// The tables a query reads, for the trust dialog (CONTRACTS.md §9.3 item 3):
// the names after FROM and JOIN, outside literals and comments. For a person
// to read before trusting; not a security check.

const SKIP_AFTER_TABLE = new Set([
	'where', 'on', 'join', 'inner', 'left', 'right', 'full', 'outer', 'cross', 'natural', 'group', 'order',
	'having', 'union', 'minus', 'intersect', 'except', 'fetch', 'offset', 'limit', 'using', 'connect', 'start',
	'window', 'qualify', 'pivot', 'unpivot', 'for', 'model', 'sample', 'partition', 'as', 'lateral',
]);

type Token = { kind: 'word' | 'name' | 'punct'; text: string };

/** Words, quoted names and punctuation; literals and comments left out. */
function tokens(sql: string): Token[] {
	const out: Token[] = [];
	let at = 0;
	while (at < sql.length) {
		const c = sql[at];
		const next = sql[at + 1];
		if (c === '-' && next === '-') {
			const end = sql.indexOf('\n', at);
			at = end < 0 ? sql.length : end + 1;
		} else if (c === '/' && next === '*') {
			const end = sql.indexOf('*/', at + 2);
			at = end < 0 ? sql.length : end + 2;
		} else if ((c === 'q' || c === 'Q') && next === "'" && at + 2 < sql.length) {
			// Oracle's q'[…]': the closing is the matching bracket, or the same character.
			const open = sql[at + 2];
			const close = { '[': ']', '{': '}', '(': ')', '<': '>' }[open] ?? open;
			const end = sql.indexOf(`${close}'`, at + 3);
			at = end < 0 ? sql.length : end + 2;
		} else if (c === "'") {
			let end = at + 1;
			while (end < sql.length && !(sql[end] === "'" && sql[end + 1] !== "'")) {
				end += sql[end] === "'" ? 2 : 1;
			}
			at = end + 1;
		} else if (c === '"') {
			let end = at + 1;
			while (end < sql.length && !(sql[end] === '"' && sql[end + 1] !== '"')) {
				end += sql[end] === '"' ? 2 : 1;
			}
			out.push({ kind: 'name', text: sql.slice(at, end + 1) });
			at = end + 1;
		} else if (/[A-Za-z_]/.test(c)) {
			let end = at + 1;
			while (end < sql.length && /[\w$#]/.test(sql[end])) {
				end++;
			}
			out.push({ kind: 'word', text: sql.slice(at, end) });
			at = end;
		} else if (/\s/.test(c)) {
			at++;
		} else {
			out.push({ kind: 'punct', text: c });
			at++;
		}
	}
	return out;
}

/** The tables after FROM and JOIN, and after the commas of a FROM list, each
 * once, in order. Subqueries are skipped; their own FROM is found anyway. */
export function tablesInSql(sql: string): string[] {
	const list = tokens(sql);
	const found: string[] = [];
	const seen = new Set<string>();
	let expectTable = false;
	let inFromList = false;
	for (let i = 0; i < list.length; i++) {
		const token = list[i];
		const word = token.kind === 'word' ? token.text.toLowerCase() : '';
		if (word === 'from' || word === 'join') {
			expectTable = true;
			inFromList = word === 'from';
			continue;
		}
		if (expectTable) {
			expectTable = false;
			if (token.kind === 'punct') {
				continue; // "(": a subquery
			}
			// A dotted name: schema.table.
			let name = token.text;
			while (list[i + 1]?.text === '.' && list[i + 2] && list[i + 2].kind !== 'punct') {
				name += `.${list[i + 2].text}`;
				i += 2;
			}
			if (!seen.has(name.toLowerCase())) {
				seen.add(name.toLowerCase());
				found.push(name);
			}
			continue;
		}
		if (inFromList) {
			if (token.text === ',') {
				expectTable = true;
			} else if (token.text === ')' || (word !== 'as' && SKIP_AFTER_TABLE.has(word))) {
				inFromList = false;
			}
		}
	}
	return found;
}
