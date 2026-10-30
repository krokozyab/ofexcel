// The task pane, and the shared runtime it is (SPEC.md §5.1): finds the
// daemon, keeps an eye on it and on the connections, and shows both, with the
// query editor under them.

import { DaemonClient } from '../api/client';
import { HealthMonitor } from '../api/health';
import { locateDaemon } from '../config';
import { ConnectionsModel } from '../connections/model';
import { readCells, selectedAddress, uses1904DateSystem } from '../editor/excel';
import { EditorModel } from '../editor/model';
import { ensureWorkbookId, workbookId } from '../workbook/id';
import { readPayload, writePayload } from '../workbook/office';
import { excelTables } from '../writer/excel_table';
import { TablesModel } from '../writer/tables';
import { ConnectionsView } from './connections_view';
import { EditorSection } from './editor_view';
import { TablesView } from './tables_view';
import { describeStatus } from './status';

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
	const main = document.querySelector('main');
	if (main) {
		new ConnectionsView(main, connections);
		new TablesView(main, tables);
		new EditorSection(main, editor, connections, client, tables);
	}
	void tables.load().catch(() => {
		// Outside Excel, or a part this add-in cannot read: no tables to show.
	});

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
