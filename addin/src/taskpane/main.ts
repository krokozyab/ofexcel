// The task pane, and the shared runtime it is (SPEC.md §5.1): finds the
// daemon, keeps an eye on it, and shows what it found in the status bar.

import { DaemonClient } from '../api/client';
import { HealthMonitor } from '../api/health';
import { locateDaemon } from '../config';
import { workbookId } from '../workbook';
import { describeStatus } from './status';

Office.onReady(() => {
	const location = locateDaemon(document, window.location.origin);
	const client = new DaemonClient({
		baseUrl: location.baseUrl,
		sessionToken: location.sessionToken,
		workbookId,
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
	});
	monitor.start(window, document);
});
