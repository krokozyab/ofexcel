// Where the daemon is (SPEC.md §5.1, DECISIONS.md R6). In the local profile
// ofexceld serves the add-in itself and puts the session token in the page
// (CONTRACTS.md §8.1): the API is on the page's own origin. In the hosted one
// the page comes from a web host, and the API is on this machine's loopback,
// at the address the page names or the default.

export const DEFAULT_DAEMON_URL = 'https://localhost:47321';

export interface DaemonLocation {
	profile: 'local' | 'hosted';
	baseUrl: string;
	/** The local profile's session token; "" in the hosted profile. */
	sessionToken: string;
}

/** What locateDaemon needs of a document: its <meta> tags. */
export interface MetaSource {
	querySelector(selector: string): { getAttribute(name: string): string | null } | null;
}

function meta(source: MetaSource, name: string): string {
	return source.querySelector(`meta[name="${name}"]`)?.getAttribute('content')?.trim() ?? '';
}

/** `origin` is the page's own, location.origin. */
export function locateDaemon(page: MetaSource, origin: string): DaemonLocation {
	const sessionToken = meta(page, 'qexcel-session');
	if (sessionToken) {
		return { profile: 'local', baseUrl: origin, sessionToken };
	}
	const named = meta(page, 'qexcel-daemon').replace(/\/+$/, '');
	return { profile: 'hosted', baseUrl: named || DEFAULT_DAEMON_URL, sessionToken: '' };
}
