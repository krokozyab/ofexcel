// The task pane, and the shared runtime it is (SPEC.md §5.1): finds the
// daemon, keeps an eye on it and on the connections, asks about the workbook's
// queries when it opens (CONTRACTS.md §9.3), and shows the connections, the
// data the workbook's formulas miss (SPEC.md §7.5), the tables, the imports
// (CONTRACTS.md §12) and the query editor. It also runs the OF functions
// (CONTRACTS.md §10), and recalculates them when what they show moves.

import { DaemonClient } from '../api/client';
import { type DaemonState, HealthMonitor } from '../api/health';
import { locateDaemon } from '../config';
import { ConnectionsModel } from '../connections/model';
import { excelSource } from '../importer/excel_source';
import { ImportsModel } from '../importer/imports';
import { readCells, selectedAddress, uses1904DateSystem } from '../editor/excel';
import { EditorModel } from '../editor/model';
import { FormulaService } from '../functions/formulas';
import { formulaStatus, workbookAsof } from '../functions/local';
import { Recalculation, registerFunctions } from '../functions/register';
import { ensureWorkbookId, workbookId } from '../workbook/id';
import { readPayload, writePayload } from '../workbook/office';
import { defaultConnection, setDefaultConnection } from '../workbook/settings';
import { connectionProblem, WorkbookTrust } from '../workbook/trust';
import type { Binding } from '../writer/bindings';
import { excelTables } from '../writer/excel_table';
import { MissingModel } from '../writer/missing';
import { TablesModel } from '../writer/tables';
import { ConnectionsView } from './connections_view';
import { EditorSection } from './editor_view';
import { ImportsView } from './imports_view';
import { MissingView } from './missing_view';
import { describeStatus } from './status';
import { TablesView } from './tables_view';
import { TrustDialog } from './trust_view';

/** Why a table of a query the person did not confirm does not refresh. */
const NOT_TRUSTED = 'Query not trusted: it was not confirmed for this workbook. Review queries to be asked again';

Office.onReady(() => {
	const location = locateDaemon(document, window.location.origin);
	const client = new DaemonClient({
		baseUrl: location.baseUrl,
		sessionToken: location.sessionToken,
		workbookId,
	});

	const connections = new ConnectionsModel(client);
	const workbook = { ensureId: ensureWorkbookId, read: readPayload, write: writePayload };
	const sheet = { read: readCells, selected: selectedAddress, date1904: uses1904DateSystem };
	const editor = new EditorModel(client, workbook, sheet);
	const imports = new ImportsModel(client, workbook, excelSource, uses1904DateSystem);
	// §12 item 5: a Local query's imports kept up to date are imported again before it runs.
	const beforeRun = (query: { hash: string }, busy: (text: string) => void) => imports.beforeRun(query.hash, busy);
	const tables = new TablesModel(client, workbook, sheet, excelTables, { beforeRun });
	const trust = new WorkbookTrust(client, workbook, () => connections.connections);
	const missing = new MissingModel(client, workbookId, { allows: (hash) => trust.allows(hash), beforeRun });
	const readMissing = () =>
		void missing.load().catch(() => {
			// The daemon away: the health line says so; the list waits for the next beat.
		});

	// §9.3: an untrusted query does not run; a table bound to another host's
	// connection runs once the person chose one of theirs.
	const guard = (binding: Binding) =>
		trust.allows(binding.query.hash)
			? connectionProblem(binding, connections.connections)
			: NOT_TRUSTED;
	tables.setGuard(guard);
	trust.onChange(() => {
		tables.setGuard(guard);
		readMissing();
	});
	connections.onChange(() => tables.setGuard(guard));

	// The OF functions (§10): registered first, as Excel may be waiting for them.
	let date1904: Promise<boolean> | undefined;
	const workbookDate1904 = () => (date1904 ??= uses1904DateSystem().catch(() => false));
	let daemon: DaemonState = { kind: 'connecting' };
	const status = () => {
		const chosen = defaultConnection();
		const connection = connections.connections.find((item) => item.name === chosen);
		return formulaStatus({
			online: daemon.kind === 'online',
			defaultSignedIn: !chosen || connection?.signed_in === true,
			stale:
				tables.bindings.some((binding) => binding.partial !== undefined) ||
				[...tables.states.values()].some((state) => state.error !== undefined),
		});
	};
	const formulas = new FormulaService(client, {
		connection: defaultConnection,
		workbookId,
		date1904: workbookDate1904,
	});
	void registerFunctions({
		service: formulas,
		workbookAsof: async () => workbookAsof(tables.bindings, await workbookDate1904()),
		status,
	}).catch(() => {
		// functions.json unreadable: the formulas show Excel's own #NAME? until Excel restarts.
	});
	// What the formulas show moved: recalculate (SPEC.md §5.8).
	const recalculation = new Recalculation();
	client.versions.onChange(() => recalculation.request());
	let shown = { status: status(), asof: workbookAsof(tables.bindings, false) };
	const followFormulas = () => {
		const now = { status: status(), asof: workbookAsof(tables.bindings, false) };
		if (now.status !== shown.status || now.asof !== shown.asof) {
			shown = now;
			recalculation.request();
		}
	};
	tables.onChange(followFormulas);
	connections.onChange(followFormulas);

	const main = document.querySelector('main');
	if (main) {
		new ConnectionsView(main, connections, {
			current: defaultConnection,
			choose: async (name) => {
				await setDefaultConnection(name);
				recalculation.request();
			},
		});
		new MissingView(main, missing);
		new TablesView(main, tables, connections, () => void trust.open());
		new ImportsView(main, imports);
		new EditorSection(main, editor, connections, client, tables);
		new TrustDialog(document.body, trust);
	}
	// §9.3 items 1–3 when the pane opens: the workbook's queries registered and
	// the untrusted ones asked about, then its tables listed.
	void (async () => {
		await connections.refresh();
		try {
			await trust.open();
			await tables.load();
			await imports.load();
			readMissing();
		} catch {
			// Outside Excel, or a part this add-in cannot read: nothing to ask about.
		}
	})();

	const bar = document.getElementById('status');
	const monitor = new HealthMonitor(client, {
		onChange(state) {
			daemon = state;
			followFormulas();
			if (!bar) {
				return;
			}
			const line = describeStatus(state, location);
			bar.textContent = line.text;
			bar.title = line.detail;
			bar.dataset.tone = line.tone;
		},
		// The sign-in states move with the daemon's: read them on the same beat.
		onChecked(state) {
			if (state.kind === 'online') {
				void connections.refresh();
				readMissing();
			}
		},
	});
	monitor.start(window, document);
});
