// The task pane, and the shared runtime it is (SPEC.md §5.1): finds the
// daemon, keeps an eye on it and on the connections, asks about the workbook's
// queries when it opens (CONTRACTS.md §9.3), and shows the tables, the
// connections and the query editor.

import { DaemonClient } from '../api/client';
import { HealthMonitor } from '../api/health';
import { locateDaemon } from '../config';
import { ConnectionsModel } from '../connections/model';
import { readCells, selectedAddress, uses1904DateSystem } from '../editor/excel';
import { EditorModel } from '../editor/model';
import { ensureWorkbookId, workbookId } from '../workbook/id';
import { readPayload, writePayload } from '../workbook/office';
import { connectionProblem, WorkbookTrust } from '../workbook/trust';
import type { Binding } from '../writer/bindings';
import { excelTables } from '../writer/excel_table';
import { TablesModel } from '../writer/tables';
import { ConnectionsView } from './connections_view';
import { EditorSection } from './editor_view';
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
	const tables = new TablesModel(client, workbook, sheet, excelTables);
	const trust = new WorkbookTrust(client, workbook, () => connections.connections);

	// §9.3: an untrusted query does not run; a table bound to another host's
	// connection runs once the person chose one of theirs.
	const guard = (binding: Binding) =>
		trust.allows(binding.query.hash)
			? connectionProblem(binding, connections.connections)
			: NOT_TRUSTED;
	tables.setGuard(guard);
	trust.onChange(() => tables.setGuard(guard));
	connections.onChange(() => tables.setGuard(guard));

	const main = document.querySelector('main');
	if (main) {
		new ConnectionsView(main, connections);
		new TablesView(main, tables, connections, () => void trust.open());
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
		} catch {
			// Outside Excel, or a part this add-in cannot read: nothing to ask about.
		}
	})();

	const bar = document.getElementById('status');
	const monitor = new HealthMonitor(client, {
		onChange(state) {
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
			}
		},
	});
	monitor.start(window, document);
});
