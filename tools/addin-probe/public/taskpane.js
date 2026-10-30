// Task pane of the add-in probe. Each experiment answers a phase 0 question
// (PHASE0_REPORT.md §7, CONTRACTS.md §16) and is appended to a report that the
// dev server saves after every run, so the results survive even when the daemon
// is unreachable from the WebView.

(function () {
	'use strict';

	const runtime = window.probeRuntime || { startedAt: 'unknown' };
	const session =
		'probe-' +
		new Date()
			.toISOString()
			.replace(/[-:]/g, '')
			.replace(/\..*$/, '')
			.replace('T', '-')
			.toLowerCase();
	const report = { session, runtimeStartedAt: runtime.startedAt, pageLoadedAt: new Date().toISOString(), runs: [] };

	const TABLE = 'ap_lines';
	const EPOCH_1900 = Date.UTC(1899, 11, 30);
	let date1904 = false;

	// --- Plumbing ---------------------------------------------------------------

	const byId = (id) => document.getElementById(id);

	function log(message) {
		const line = `[${new Date().toISOString().slice(11, 19)}] ${message}\n`;
		byId('log').textContent += line;
		byId('log').scrollTop = byId('log').scrollHeight;
	}

	function status(message) {
		byId('status').textContent = message;
	}

	function daemon() {
		return byId('daemon').value.replace(/\/+$/, '');
	}

	function errorInfo(error) {
		if (!error) {
			return { message: String(error) };
		}
		return {
			name: error.name,
			message: error.message,
			code: error.code,
			debugInfo: error.debugInfo ? JSON.stringify(error.debugInfo).slice(0, 2000) : undefined,
		};
	}

	const round = (value) => Math.round(value);

	async function saveReport() {
		try {
			const response = await fetch(`/report?name=${session}`, {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify(report, null, 2),
			});
			if (!response.ok) {
				log(`report not saved: HTTP ${response.status}`);
			}
		} catch (error) {
			log(`report not saved: ${error.message}`);
		}
	}

	async function record(name, experiment) {
		status(`Running: ${name}…`);
		log(`▶ ${name}`);
		const started = performance.now();
		let result;
		try {
			result = Object.assign({ ok: true }, await experiment());
		} catch (error) {
			result = { ok: false, error: errorInfo(error) };
			log(`✖ ${name}: ${error && error.message}`);
		}
		result = Object.assign({ name, at: new Date().toISOString(), ms: round(performance.now() - started) }, result);
		report.runs.push(result);
		await saveReport();
		log(`${result.ok ? '✔' : '✖'} ${name} (${result.ms} ms)`);
		status(`Done: ${name}`);
		return result;
	}

	async function freshSheet(context, name) {
		const old = context.workbook.worksheets.getItemOrNullObject(name);
		await context.sync();
		if (!old.isNullObject) {
			old.delete();
			await context.sync();
		}
		const sheet = context.workbook.worksheets.add(name);
		sheet.activate();
		await context.sync();
		return sheet;
	}

	// --- Data from the daemon --------------------------------------------------

	//! Where data comes from: the daemon directly if the WebView lets the page
	//! reach it, else the dev server's same-origin proxy (/daemon/...).
	let activeBase = null;

	async function dataBase() {
		if (!activeBase) {
			const direct = await probeFetch(`${daemon()}/v1/health`);
			activeBase = direct.ok ? daemon() : `${location.origin}/daemon`;
			report.dataPath = activeBase;
			log(`data goes through ${activeBase}`);
		}
		return activeBase;
	}

	async function fetchPage(offset, limit) {
		const base = await dataBase();
		const started = performance.now();
		const response = await fetch(`${base}/v1/probe/rows?table=${TABLE}&offset=${offset}&limit=${limit}`, {
			cache: 'no-store',
		});
		if (!response.ok) {
			throw new Error(`HTTP ${response.status}: ${(await response.text()).slice(0, 300)}`);
		}
		const text = await response.text();
		const fetched = performance.now();
		const page = JSON.parse(text);
		return {
			page,
			chars: text.length,
			fetchMs: fetched - started,
			parseMs: performance.now() - fetched,
			server: {
				queryMs: Number(response.headers.get('X-OF-Probe-Query-Ms')) || 0,
				encodeMs: Number(response.headers.get('X-OF-Probe-Encode-Ms')) || 0,
				serializeMs: Number(response.headers.get('X-OF-Probe-Serialize-Ms')) || 0,
			},
		};
	}

	//! CONTRACTS.md §11.4: dates become serial numbers, decimals numbers, NULL "".
	function toSerial(text) {
		const match = /^(\d{4})-(\d{2})-(\d{2})(?:[ T](\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?)?/.exec(text);
		if (!match) {
			return text;
		}
		const millis = match[7] ? Math.floor(Number(`0.${match[7]}`) * 1000) : 0;
		const utc = Date.UTC(+match[1], +match[2] - 1, +match[3], +(match[4] || 0), +(match[5] || 0), +(match[6] || 0), millis);
		const serial = (utc - EPOCH_1900) / 86400000;
		return date1904 ? serial - 1462 : serial;
	}

	function converter(type) {
		if (type === 'date' || type === 'timestamp') {
			return (value) => (value === null ? '' : toSerial(value));
		}
		if (type.startsWith('decimal')) {
			return (value) => (value === null ? '' : Number(value));
		}
		return (value) => (value === null ? '' : value);
	}

	function numberFormat(type) {
		if (type === 'string') {
			return '@';
		}
		if (type === 'date') {
			return 'yyyy-mm-dd';
		}
		if (type === 'timestamp') {
			return 'yyyy-mm-dd hh:mm:ss';
		}
		if (type.startsWith('decimal')) {
			const scale = Number((type.match(/,(\d+)\)/) || [0, 0])[1]);
			return scale ? `#,##0.${'0'.repeat(scale)}` : '#,##0';
		}
		if (type === 'integer') {
			return '0';
		}
		return 'General';
	}

	function convertRows(columns, rows) {
		const converters = columns.map((column) => converter(column.type));
		return rows.map((row) => row.map((value, index) => converters[index](value)));
	}

	// --- 1. Environment ----------------------------------------------------------

	function highestVersion(set, from, to) {
		let best = null;
		for (let minor = from; minor <= to; minor++) {
			if (Office.context.requirements.isSetSupported(set, `1.${minor}`)) {
				best = `1.${minor}`;
			}
		}
		return best;
	}

	async function permissionState(name) {
		try {
			if (!navigator.permissions) {
				return 'no Permissions API';
			}
			return (await navigator.permissions.query({ name })).state;
		} catch (error) {
			return `unsupported: ${error.message}`;
		}
	}

	async function environment() {
		const diagnostics = Office.context.diagnostics;
		return {
			userAgent: navigator.userAgent,
			origin: location.origin,
			isSecureContext: window.isSecureContext,
			office: { host: diagnostics.host, platform: diagnostics.platform, version: diagnostics.version },
			sets: {
				ExcelApi: highestVersion('ExcelApi', 1, 22),
				SharedRuntime: highestVersion('SharedRuntime', 1, 3),
				CustomFunctionsRuntime: highestVersion('CustomFunctionsRuntime', 1, 6),
				DialogApi: highestVersion('DialogApi', 1, 3),
			},
			localNetworkAccessPermission: await permissionState('local-network-access'),
			runtime: Object.assign({}, runtime),
		};
	}

	// --- 2. Connectivity ---------------------------------------------------------

	async function probeFetch(url, options) {
		const started = performance.now();
		try {
			const response = await fetch(url, Object.assign({ cache: 'no-store' }, options));
			const text = await response.text();
			return {
				url,
				ok: response.ok,
				status: response.status,
				ms: round(performance.now() - started),
				allowOrigin: response.headers.get('Access-Control-Allow-Origin'),
				dataVersion: response.headers.get('X-OF-Data-Version'),
				body: text.slice(0, 300),
			};
		} catch (error) {
			return { url, ok: false, ms: round(performance.now() - started), error: errorInfo(error) };
		}
	}

	async function connectivity() {
		const bases = Array.from(new Set([daemon(), daemon().replace('127.0.0.1', 'localhost')]));
		const results = [];
		for (const base of bases) {
			results.push(Object.assign({ kind: 'simple GET /v1/health' }, await probeFetch(`${base}/v1/health`)));
			// A custom header makes the browser send a CORS preflight first.
			results.push(
				Object.assign(
					{ kind: 'GET with X-OF-Workbook (preflight)' },
					await probeFetch(`${base}/v1/probe/rows?table=${TABLE}&limit=1`, { headers: { 'X-OF-Workbook': 'probe' } }),
				),
			);
		}
		// Cross-origin HTTPS on loopback with the trusted development certificate:
		// what a daemon serving HTTPS on 127.0.0.1 would look like to the page.
		const crossOrigin = location.origin.replace(/:(\d+)$/, (_, port) => `:${Number(port) + 1}`);
		results.push(Object.assign({ kind: 'cross-origin HTTPS GET' }, await probeFetch(`${crossOrigin}/health`)));
		results.push(
			Object.assign(
				{ kind: 'cross-origin HTTPS GET with X-OF-Workbook (preflight)' },
				await probeFetch(`${crossOrigin}/health`, { headers: { 'X-OF-Workbook': 'probe' } }),
			),
		);
		results.push(Object.assign({ kind: 'cross-origin HTTPS via 127.0.0.1' }, await probeFetch(crossOrigin.replace('localhost', '127.0.0.1') + '/health')));
		// The same daemon behind the dev server's same-origin HTTPS proxy.
		results.push(Object.assign({ kind: 'same-origin proxy GET /daemon/v1/health' }, await probeFetch(`${location.origin}/daemon/v1/health`)));
		const base = await dataBase();
		const started = performance.now();
		let succeeded = 0;
		for (let i = 0; i < 20; i++) {
			if ((await probeFetch(`${base}/v1/health`)).ok) {
				succeeded++;
			}
		}
		return { results, dataPath: base, repeatedHealth: { requests: 20, succeeded, averageMs: round((performance.now() - started) / 20) } };
	}

	// --- 3. Writing to an Excel Table --------------------------------------------

	async function writeTable(total, chunk, sheetName) {
		const timing = { fetch: 0, parse: 0, convert: 0, formats: 0, write: 0, tableAdd: 0, serverQuery: 0, serverEncode: 0, serverSerialize: 0 };
		let columns = null;
		let written = 0;
		let chars = 0;
		let chunks = 0;
		const started = performance.now();
		await Excel.run(async (context) => {
			const sheet = await freshSheet(context, sheetName);
			for (let offset = 0; offset < total; offset += chunk) {
				const page = await fetchPage(offset, Math.min(chunk, total - offset));
				timing.fetch += page.fetchMs;
				timing.parse += page.parseMs;
				timing.serverQuery += page.server.queryMs;
				timing.serverEncode += page.server.encodeMs;
				timing.serverSerialize += page.server.serializeMs;
				chars += page.chars;
				if (!columns) {
					columns = page.page.columns;
					const formatsStarted = performance.now();
					sheet.getRangeByIndexes(0, 0, 1, columns.length).values = [columns.map((column) => column.name)];
					columns.forEach((column, index) => {
						sheet.getRangeByIndexes(1, index, total, 1).numberFormat = numberFormat(column.type);
					});
					await context.sync();
					timing.formats += performance.now() - formatsStarted;
				}
				const convertStarted = performance.now();
				const values = convertRows(columns, page.page.rows);
				timing.convert += performance.now() - convertStarted;
				if (!values.length) {
					break;
				}
				const writeStarted = performance.now();
				context.application.suspendApiCalculationUntilNextSync();
				sheet.getRangeByIndexes(1 + offset, 0, values.length, columns.length).values = values;
				await context.sync();
				timing.write += performance.now() - writeStarted;
				written += values.length;
				chunks++;
				status(`${sheetName}: ${written} / ${total}`);
			}
			const tableStarted = performance.now();
			const table = sheet.tables.add(sheet.getRangeByIndexes(0, 0, 1 + written, columns.length), true);
			table.name = `tbl_${sheetName}`;
			await context.sync();
			timing.tableAdd = performance.now() - tableStarted;
		});
		const totalMs = performance.now() - started;
		Object.keys(timing).forEach((key) => (timing[key] = round(timing[key])));
		return {
			dataPath: activeBase,
			rows: written,
			columns: columns ? columns.length : 0,
			columnTypes: columns ? columns.map((column) => `${column.name}:${column.type}`) : [],
			chunkRows: chunk,
			chunks,
			jsonChars: chars,
			timingMs: timing,
			totalMs: round(totalMs),
			rowsPerSecond: round(written / (totalMs / 1000)),
		};
	}

	//! A5: how big a single write can be. Stops at the first failure.
	async function chunks() {
		const attempts = [];
		for (const rows of [20000, 50000, 100000]) {
			const attempt = { rows };
			try {
				const page = await fetchPage(0, rows);
				const values = convertRows(page.page.columns, page.page.rows);
				attempt.cells = values.length * page.page.columns.length;
				attempt.valuesJsonChars = JSON.stringify(values).length;
				await Excel.run(async (context) => {
					const sheet = await freshSheet(context, 'p_chunk');
					const started = performance.now();
					context.application.suspendApiCalculationUntilNextSync();
					sheet.getRangeByIndexes(0, 0, values.length, page.page.columns.length).values = values;
					await context.sync();
					attempt.writeMs = round(performance.now() - started);
				});
				attempt.ok = true;
			} catch (error) {
				attempt.ok = false;
				attempt.error = errorInfo(error);
			}
			attempts.push(attempt);
			if (!attempt.ok) {
				break;
			}
		}
		return { attempts };
	}

	// --- 4. Refreshing an existing table (A8) -------------------------------------

	async function refresh() {
		const resizeSupported = Office.context.requirements.isSetSupported('ExcelApi', '1.13');
		const steps = [];
		await Excel.run(async (context) => {
			const sheet = await freshSheet(context, 'p_refresh');
			const first = await fetchPage(0, 1000);
			const columns = first.page.columns;
			const width = columns.length;
			sheet.getRangeByIndexes(0, 0, 1, width).values = [columns.map((column) => column.name)];
			sheet.getRangeByIndexes(1, 0, 1000, width).values = convertRows(columns, first.page.rows);
			const table = sheet.tables.add(sheet.getRangeByIndexes(0, 0, 1001, width), true);
			table.name = 'tbl_refresh';
			table.style = 'TableStyleMedium4';
			await context.sync();

			// What a user adds and expects a refresh to keep.
			table.columns.add(null, null, 'DOUBLE_AMOUNT');
			await context.sync();
			table.columns.getItem('DOUBLE_AMOUNT').getDataBodyRange().formulas = '=[@AMOUNT]*2';
			table.columns.getItem('DESCRIPTION').getDataBodyRange().format.font.bold = true;
			await context.sync();

			async function inspect(label, expectedRows) {
				const rows = table.rows;
				rows.load('count');
				table.load('style');
				const calcLast = table.columns.getItem('DOUBLE_AMOUNT').getDataBodyRange().getLastCell();
				calcLast.load('formulas,values');
				const boldLast = table.columns.getItem('DESCRIPTION').getDataBodyRange().getLastCell().format.font;
				boldLast.load('bold');
				const below = sheet.getRangeByIndexes(1 + expectedRows, 0, 1, width + 1);
				below.load('values');
				await context.sync();
				steps.push({
					step: label,
					expectedRows,
					rowCount: rows.count,
					style: table.style,
					calculatedColumnLastFormula: calcLast.formulas[0][0],
					calculatedColumnLastValue: calcLast.values[0][0],
					userBoldKeptOnLastRow: boldLast.bold,
					rowBelowTable: below.values[0].slice(0, 4),
				});
			}

			await inspect('initial', 1000);

			if (!resizeSupported) {
				steps.push({ step: 'resize', skipped: 'ExcelApi 1.13 not supported' });
				return;
			}
			const grown = await fetchPage(0, 1500);
			table.resize(sheet.getRangeByIndexes(0, 0, 1501, width + 1));
			await context.sync();
			sheet.getRangeByIndexes(1, 0, 1500, width).values = convertRows(columns, grown.page.rows);
			await context.sync();
			await inspect('grow to 1500', 1500);

			const shrunk = await fetchPage(0, 300);
			table.resize(sheet.getRangeByIndexes(0, 0, 301, width + 1));
			await context.sync();
			sheet.getRangeByIndexes(1, 0, 300, width).values = convertRows(columns, shrunk.page.rows);
			await context.sync();
			await inspect('shrink to 300', 300);
		});
		return { resizeSupported, steps };
	}

	// --- 5. Formula injection (A6) ------------------------------------------------

	async function injection() {
		const samples = ['=1+1', '+1', '-1', '@SUM(1,2)', '0012', '=HYPERLINK("https://example.com","x")', '-5.5', "'=1+1"];
		const modes = [];
		await Excel.run(async (context) => {
			const sheet = await freshSheet(context, 'p_inject');
			for (const [index, mode] of ['text format @', 'General'].entries()) {
				const range = sheet.getRangeByIndexes(0, index, samples.length, 1);
				if (mode === 'text format @') {
					range.numberFormat = '@';
					await context.sync();
				}
				range.values = samples.map((sample) => [sample]);
				await context.sync();
				range.load('values,formulas,valueTypes,numberFormat');
				await context.sync();
				modes.push({
					mode,
					cells: samples.map((sample, row) => ({
						input: sample,
						value: range.values[row][0],
						formula: range.formulas[row][0],
						valueType: range.valueTypes[row][0],
						becameFormula: String(range.formulas[row][0]).startsWith('=') && range.formulas[row][0] !== range.values[row][0],
					})),
				});
			}
		});
		return { modes };
	}

	// --- 6. Date system ---------------------------------------------------------

	async function dates() {
		let serial2026;
		await Excel.run(async (context) => {
			const sheet = await freshSheet(context, 'p_dates');
			const cell = sheet.getRange('A1');
			cell.formulas = [['=DATE(2026,1,1)']];
			await context.sync();
			cell.load('values');
			await context.sync();
			serial2026 = cell.values[0][0];
		});
		// 46023 in the 1900 system, 44561 in the 1904 one.
		date1904 = serial2026 === 44561;
		return { serialOf2026_01_01: serial2026, system: date1904 ? '1904' : serial2026 === 46023 ? '1900' : 'unknown', ourSerial: toSerial('2026-01-01') };
	}

	// --- 7. Custom functions (A7, late association) --------------------------------

	const FUNCTION_CASES = [
		['SCALARARGS(B1 empty, "")', '=PROBE.SCALARARGS(B1,"")'],
		['SCALARARGS(omitted, 1)', '=PROBE.SCALARARGS(,1)'],
		['SCALARARGS()', '=PROBE.SCALARARGS()'],
		['ARGS(B1 empty)', '=PROBE.ARGS(B1)'],
		['ARGS(B1:C2 partly empty)', '=PROBE.ARGS(B1:C2)'],
		['ARGS(D1 = "")', '=PROBE.ARGS(D1)'],
		['ARGS(0, FALSE, "x", DATE)', '=PROBE.ARGS(0,FALSE,"x",DATE(2026,1,1))'],
		['META()', '=PROBE.META()'],
		['LATE()', '=PROBE.LATE()'],
		['NEW()', '=PROBE.NEW()'],
		['MATRIX(2,3)', '=PROBE.MATRIX(2,3)'],
	];

	async function evaluate(cases, sheetName) {
		const results = [];
		await Excel.run(async (context) => {
			const sheet = await freshSheet(context, sheetName);
			sheet.getRange('C2').values = [[5]];
			sheet.getRange('D1').formulas = [['=""']];
			// Column A: labels; column F: formulas, every third row so spills have room.
			cases.forEach(([label, formula], index) => {
				sheet.getRangeByIndexes(3 + index * 3, 0, 1, 1).values = [[label]];
				sheet.getRangeByIndexes(3 + index * 3, 5, 1, 1).formulas = [[formula]];
			});
			await context.sync();

			const deadline = Date.now() + 20000;
			let cells;
			for (;;) {
				cells = cases.map((_, index) => {
					const cell = sheet.getRangeByIndexes(3 + index * 3, 5, 2, 3);
					cell.load('values,formulas,valueTypes');
					return cell;
				});
				await context.sync();
				const busy = cells.some((cell) => String(cell.values[0][0]).startsWith('#BUSY'));
				if (!busy || Date.now() > deadline) {
					break;
				}
				await new Promise((resolve) => setTimeout(resolve, 500));
			}
			cases.forEach(([label, formula], index) => {
				const cell = cells[index];
				let parsed = null;
				try {
					parsed = JSON.parse(cell.values[0][0]);
				} catch {
					// Not JSON: an error value, a number or a spill.
				}
				results.push({
					label,
					formula,
					formulaAfter: cell.formulas[0][0],
					value: parsed || cell.values[0][0],
					valueType: cell.valueTypes[0][0],
					spill: label.startsWith('MATRIX') ? cell.values : undefined,
				});
			});
		});
		return results;
	}

	async function functions() {
		return {
			runtime: Object.assign({}, runtime),
			results: await evaluate(FUNCTION_CASES, 'p_cf'),
		};
	}

	// --- 8. Metadata cache (check 9) ----------------------------------------------

	async function setVariant(variant) {
		const response = await fetch('/meta-variant', {
			method: 'POST',
			headers: { 'Content-Type': 'application/json' },
			body: JSON.stringify({ variant }),
		});
		return { server: await response.json(), runtime: Object.assign({}, runtime) };
	}

	async function metaCheck() {
		const state = await (await fetch('/state', { cache: 'no-store' })).json();
		return {
			serverVariant: state.metadata_variant,
			runtime: Object.assign({}, runtime),
			results: await evaluate(
				FUNCTION_CASES.filter(([label]) => label === 'META()' || label === 'NEW()'),
				'p_meta',
			),
		};
	}

	// --- Wiring -----------------------------------------------------------------

	const EXPERIMENTS = {
		environment: () => record('environment', environment),
		connectivity: () => record('connectivity', connectivity),
		dates: () => record('dates', dates),
		write10k: () => record('write 10k / chunk 5000', () => writeTable(10000, 5000, 'p_10k')),
		write100k: () => record('write 100k / chunk 5000', () => writeTable(100000, 5000, 'p_100k')),
		write500k: () => record('write 500k / chunk 10000', () => writeTable(500000, 10000, 'p_500k')),
		chunks: () => record('chunk size limit', chunks),
		refresh: () => record('refresh existing table', refresh),
		injection: () => record('formula injection', injection),
		functions: () => record('custom functions', functions),
		metaSwitch: () => record('metadata -> variant 2', () => setVariant(2)),
		metaCheck: () => record('metadata check', metaCheck),
		metaReset: () => record('metadata -> variant 1', () => setVariant(1)),
	};

	const RUN_ALL = ['environment', 'connectivity', 'dates', 'write10k', 'write100k', 'chunks', 'refresh', 'injection', 'functions'];

	function setBusy(busy) {
		document.querySelectorAll('button').forEach((button) => (button.disabled = busy));
	}

	async function run(names) {
		setBusy(true);
		try {
			for (const name of names) {
				await EXPERIMENTS[name]();
			}
		} finally {
			setBusy(false);
		}
	}

	Office.onReady((info) => {
		report.office = info;
		document.querySelectorAll('button[data-run]').forEach((button) => {
			button.addEventListener('click', () => run([button.dataset.run]));
		});
		byId('run-all').addEventListener('click', () => run(RUN_ALL));
		status(`Ready. Session ${session}`);
		log(`runtime started ${runtime.startedAt}; page loaded ${report.pageLoadedAt}`);
		saveReport();
	});
})();
