// Errors from the daemon, and the few the client gives when there is no
// answer to read one from.

import type { ErrorBody } from './types';

/** No answer: the daemon is not running, or the WebView does not trust its certificate. */
export const E_OFFLINE = 'E_OFFLINE';
/** No answer in time; the request was abandoned. */
export const E_TIMEOUT = 'E_TIMEOUT';
/** An answer that is not the API's: no ErrorBody, or a body that is not JSON. */
export const E_BAD_RESPONSE = 'E_BAD_RESPONSE';

export class DaemonError extends Error {
	constructor(
		/** A CONTRACTS.md §14 code, or one of the three above. */
		readonly code: string,
		/** English, for the user (DECISIONS.md R7). */
		message: string,
		/** The HTTP status; 0 when there was no answer. */
		readonly status: number,
		readonly details?: unknown,
	) {
		super(message);
		this.name = 'DaemonError';
	}
}

function isErrorBody(value: unknown): value is ErrorBody {
	if (typeof value !== 'object' || value === null) {
		return false;
	}
	const error = (value as { error?: unknown }).error;
	return (
		typeof error === 'object' &&
		error !== null &&
		typeof (error as { code?: unknown }).code === 'string' &&
		typeof (error as { message?: unknown }).message === 'string'
	);
}

/** The error a response that is not 2xx carries. */
export async function readError(response: Response): Promise<DaemonError> {
	let body: unknown;
	try {
		body = JSON.parse(await response.text());
	} catch {
		body = undefined;
	}
	if (isErrorBody(body)) {
		return new DaemonError(body.error.code, body.error.message, response.status, body.error.details);
	}
	return new DaemonError(
		E_BAD_RESPONSE,
		`The daemon answered HTTP ${response.status} without an error description`,
		response.status,
	);
}
