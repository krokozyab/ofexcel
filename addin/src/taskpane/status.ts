// The task pane's status bar: which daemon, which profile, or why there is none.

import type { DaemonLocation } from '../config';
import { CLIENT_API_VERSION } from '../api/client';
import type { DaemonState } from '../api/health';

export type StatusTone = 'pending' | 'ok' | 'warning' | 'error';

export interface StatusLine {
	text: string;
	tone: StatusTone;
	/** More, for the tooltip. */
	detail: string;
}

export function describeStatus(state: DaemonState, location: DaemonLocation): StatusLine {
	switch (state.kind) {
		case 'connecting':
			return { text: 'Connecting to ofexceld…', tone: 'pending', detail: location.baseUrl };
		case 'online':
			return {
				text: `ofexceld ${state.health.version} · ${state.health.profile}`,
				tone: 'ok',
				detail: `${location.baseUrl} · API ${state.health.api.version}`,
			};
		case 'incompatible':
			return {
				text: `ofexceld ${state.health.version} needs another add-in version`,
				tone: 'warning',
				detail:
					`The daemon speaks API ${state.health.api.version} and accepts add-ins from ` +
					`${state.health.api.min_client}; this add-in speaks ${CLIENT_API_VERSION}`,
			};
		case 'offline':
			return { text: 'ofexceld is not available', tone: 'error', detail: state.error.message };
	}
}
