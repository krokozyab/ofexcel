// What the shared runtime answers without the daemon (CONTRACTS.md §10.2):
// OF.ASOF() with no arguments -- the oldest last refresh among the workbook's
// tables -- and OF.STATUS().

import type { Binding } from '../writer/bindings';

/** Serial numbers of 1970-01-01 in the 1900 and the 1904 date system. */
const EPOCH_SERIAL_1900 = 25569;
const EPOCH_SERIAL_1904 = 24107;
const MS_PER_DAY = 86_400_000;

/** An instant as an Excel serial date-time in the machine's local time, as
 * the daemon gives OF.ASOF(query) (DECISIONS.md R11). */
export function localSerial(ms: number, date1904: boolean): number {
	const local = ms - new Date(ms).getTimezoneOffset() * 60_000;
	return (date1904 ? EPOCH_SERIAL_1904 : EPOCH_SERIAL_1900) + local / MS_PER_DAY;
}

/** OF.ASOF(): the earliest last_success.at of the workbook's tables, as a
 * serial date-time; undefined when no table has been refreshed. */
export function workbookAsof(bindings: Binding[], date1904: boolean): number | undefined {
	const times = bindings
		.map((binding) => (binding.last_success ? Date.parse(binding.last_success.at) : Number.NaN))
		.filter((time) => Number.isFinite(time));
	return times.length ? localSerial(Math.min(...times), date1904) : undefined;
}

export type FormulaStatus = 'OK' | 'NOT_SIGNED_IN' | 'STALE' | 'OFFLINE';

export interface StatusFacts {
	/** The daemon answers /v1/health. */
	online: boolean;
	/** The workbook's default connection, if it has one, is signed in. */
	defaultSignedIn: boolean;
	/** A table's last refresh failed, or its last write stopped part way. */
	stale: boolean;
}

/** OF.STATUS(): the first of OFFLINE, NOT_SIGNED_IN, STALE that holds, else OK. */
export function formulaStatus(facts: StatusFacts): FormulaStatus {
	if (!facts.online) {
		return 'OFFLINE';
	}
	if (!facts.defaultSignedIn) {
		return 'NOT_SIGNED_IN';
	}
	return facts.stale ? 'STALE' : 'OK';
}
