// The Connections section of the pane (SPEC.md §3.1, §5.2 item 1): each
// connection with its environment's colour, its sign-in state and actions,
// and a form to add one. Sign-in runs in the background (DECISIONS.md R5):
// the section shows the seconds it has taken, and the rest of the pane works.

import { DaemonError } from '../api/errors';
import type { Environment } from '../api/types';
import {
	type ConnectionsModel,
	type ConnectionView,
	DEFAULT_REPORT_PATH,
	elapsed,
	expiryWarning,
} from '../connections/model';
import { h } from './dom';

const ENVIRONMENTS: Environment[] = ['DEV', 'TEST', 'PROD'];

export class ConnectionsView {
	private readonly list = h('div', { class: 'connections' });
	private readonly form: HTMLFormElement;
	private readonly formError = h('div', { class: 'problem', role: 'alert' });
	private ticking: ReturnType<typeof setInterval> | undefined;

	constructor(
		root: HTMLElement,
		private readonly model: ConnectionsModel,
		private readonly now: () => number = Date.now,
	) {
		this.form = this.buildForm();
		root.append(
			h(
				'section',
				{},
				h('h2', {}, 'Connections'),
				this.list,
				h('details', { class: 'add' }, h('summary', {}, 'Add a connection'), this.form),
			),
		);
		model.onChange(() => this.render());
		// The minutes left to a sign-in move even when nothing else does.
		setInterval(() => this.render(), 30_000);
		this.render();
	}

	render(): void {
		const now = this.now();
		const items: Node[] = [];
		if (this.model.error) {
			items.push(h('div', { class: 'problem' }, `Connections cannot be read: ${this.model.error}`));
		} else if (this.model.ready && this.model.connections.length === 0) {
			items.push(h('div', { class: 'note' }, 'No connections yet. Add one below.'));
		}
		for (const connection of this.model.connections) {
			items.push(this.item(connection, now));
		}
		this.list.replaceChildren(...items);
		this.tickWhileSigningIn();
	}

	private item(connection: ConnectionView, now: number): HTMLElement {
		const name = connection.name;
		const busy = connection.signingInSince !== undefined;
		let state: Node;
		if (busy) {
			state = h(
				'div',
				{ class: 'signing-in' },
				`Signing in to Fusion — finish in the browser window that opened (${elapsed(connection.signingInSince!, now)})`,
			);
		} else if (connection.signed_in) {
			state = h('div', { class: 'state ok' }, connection.user ? `Signed in as ${connection.user}` : 'Signed in');
		} else {
			state = h('div', { class: 'state' }, 'Not signed in');
		}

		const expiring = busy ? '' : expiryWarning(connection, now);
		const warning = expiring ? h('div', { class: 'warning' }, expiring) : null;

		const actions = h(
			'div',
			{ class: 'actions' },
			h(
				'button',
				{ class: connection.signed_in ? '' : 'primary', disabled: busy, onclick: () => void this.model.signIn(name) },
				connection.signed_in ? 'Sign in again' : 'Sign in',
			),
			h('button', { disabled: busy, onclick: () => void this.model.test(name) }, 'Test'),
			connection.signed_in
				? h('button', { disabled: busy, onclick: () => void this.model.signOut(name) }, 'Sign out')
				: null,
		);

		return h(
			'div',
			{ class: 'connection', 'data-environment': connection.environment },
			h(
				'div',
				{ class: 'title' },
				h('span', { class: 'environment' }, connection.environment),
				h('strong', {}, name),
				h('span', { class: 'host' }, connection.host),
			),
			state,
			warning,
			connection.tested
				? h('div', { class: connection.tested.ok ? 'state ok' : 'problem' }, connection.tested.message)
				: null,
			connection.problem ? h('div', { class: 'problem' }, connection.problem) : null,
			actions,
		);
	}

	/** Once a second while a sign-in is under way, for its elapsed time. */
	private tickWhileSigningIn(): void {
		const signingIn = this.model.connections.some((connection) => connection.signingInSince !== undefined);
		if (signingIn && this.ticking === undefined) {
			this.ticking = setInterval(() => this.render(), 1000);
		} else if (!signingIn && this.ticking !== undefined) {
			clearInterval(this.ticking);
			this.ticking = undefined;
		}
	}

	private buildForm(): HTMLFormElement {
		const field = (label: string, input: HTMLElement) => h('label', {}, label, input);
		const name = h('input', {
			name: 'name',
			required: true,
			pattern: '[a-z][a-z0-9_]{0,62}',
			placeholder: 'fusion_test',
			title: 'Lower case letters, digits and _, starting with a letter',
		}) as HTMLInputElement;
		const environment = h(
			'select',
			{ name: 'environment' },
			...ENVIRONMENTS.map((value) => h('option', { value, selected: value === 'TEST' }, value)),
		) as HTMLSelectElement;
		const endpoint = h('input', {
			name: 'endpoint',
			required: true,
			placeholder: 'https://your-instance.fa.ocs.oraclecloud.com',
		}) as HTMLInputElement;
		const reportPath = h('input', { name: 'report_path', required: true }) as HTMLInputElement;
		reportPath.value = DEFAULT_REPORT_PATH;
		const submit = h('button', { type: 'submit', class: 'primary' }, 'Add') as HTMLButtonElement;

		const form = h(
			'form',
			{},
			field('Name', name),
			field('Environment', environment),
			field('Fusion address', endpoint),
			field('SQL report path', reportPath),
			this.formError,
			submit,
		) as HTMLFormElement;
		form.addEventListener('submit', (event) => {
			event.preventDefault();
			submit.disabled = true;
			this.formError.textContent = '';
			this.model
				.create({
					name: name.value.trim(),
					environment: environment.value as Environment,
					endpoint: endpoint.value,
					report_path: reportPath.value,
				})
				.then(() => {
					name.value = '';
					endpoint.value = '';
					(form.parentElement as HTMLDetailsElement | null)?.removeAttribute('open');
				})
				.catch((error: unknown) => {
					this.formError.textContent = error instanceof DaemonError ? error.message : String(error);
				})
				.finally(() => {
					submit.disabled = false;
				});
		});
		return form;
	}
}
