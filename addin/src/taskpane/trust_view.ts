// The trust dialog (CONTRACTS.md §9.3 item 3): over the whole pane, before
// anything else is done with the workbook's queries. Each query in full -- its
// SQL, the tables it reads, where it runs -- and what trusting it means.

import { TRUST_WARNING, type WorkbookTrust } from '../workbook/trust';
import { h } from './dom';

export class TrustDialog {
	private readonly overlay = h('div', { class: 'overlay', role: 'dialog', 'aria-modal': 'true' });

	constructor(
		root: HTMLElement,
		private readonly trust: WorkbookTrust,
	) {
		root.append(this.overlay);
		trust.onChange(() => this.render());
		this.render();
	}

	private render(): void {
		const state = this.trust.state;
		if (state.kind !== 'asking') {
			this.overlay.hidden = true;
			this.overlay.replaceChildren();
			return;
		}
		this.overlay.hidden = false;
		const count = state.queries.length;
		this.overlay.replaceChildren(
			h(
				'div',
				{ class: 'dialog' },
				h('h2', {}, count === 1 ? 'This workbook has a query to confirm' : `This workbook has ${count} queries to confirm`),
				h(
					'p',
					{ class: 'note' },
					'They were not written on this computer. Read them before they run.',
				),
				...state.queries.map((query) =>
					h(
						'div',
						{ class: 'trust-query' },
						h('div', { class: 'title' }, h('strong', {}, query.id), query.title ? h('span', {}, ` — ${query.title}`) : null),
						h(
							'div',
							{ class: 'note' },
							`Saved ${query.savedAt.replace('T', ' ').replace('Z', ' UTC')}` +
								(query.savedBy ? ` by ${query.savedBy}` : ''),
						),
						...query.connections.map((connection) =>
							h(
								'div',
								{ class: 'note', 'data-environment': connection.environment ?? '' },
								`Runs on ${connection.name} (${connection.host})`,
								connection.environment ? h('span', { class: 'environment' }, ` ${connection.environment}`) : null,
							),
						),
						query.tables.length ? h('div', { class: 'note' }, `Reads ${query.tables.join(', ')}`) : null,
						h('pre', { class: 'sql' }, query.text),
					),
				),
				h('div', { class: 'warning' }, TRUST_WARNING),
				this.trust.error ? h('div', { class: 'problem' }, this.trust.error) : null,
				h(
					'div',
					{ class: 'actions' },
					h('button', { class: 'primary', onclick: () => void this.trust.trust() }, 'Trust'),
					h('button', { onclick: () => this.trust.decline() }, "Don't trust"),
				),
			),
		);
	}
}
