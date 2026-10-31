// "Missing data for this workbook" (SPEC.md §7.5): what its formulas asked for
// and did not find, and "Refresh missing" for all of it. Shown only when there
// is something missing, or the last "Refresh missing" left problems.

import { describeEntry, type MissingModel } from '../writer/missing';
import { h } from './dom';

export class MissingView {
	private readonly section = h('section', { class: 'missing' });

	constructor(
		root: HTMLElement,
		private readonly model: MissingModel,
	) {
		root.append(this.section);
		model.onChange(() => this.render());
		this.render();
	}

	private render(): void {
		const model = this.model;
		const entries = model.entries;
		if (entries.length === 0 && model.problems.length === 0 && model.busy === undefined) {
			this.section.hidden = true;
			this.section.replaceChildren();
			return;
		}
		const runnable = new Set(model.runnable());
		const busy = model.busy !== undefined;
		const parts: HTMLElement[] = [h('h2', {}, 'Missing data')];
		if (entries.length) {
			parts.push(
				h('div', { class: 'note' }, 'Formulas in this workbook asked for data that has not been fetched:'),
				h(
					'ul',
					{ class: 'history' },
					...entries.map((entry) =>
						h(
							'li',
							{},
							describeEntry(entry),
							runnable.has(entry) ? null : h('span', { class: 'note' }, ' · query not trusted'),
						),
					),
				),
			);
		}
		if (busy) {
			parts.push(h('div', { class: 'signing-in' }, model.busy ?? ''));
		}
		parts.push(...model.problems.map((problem) => h('div', { class: 'problem' }, problem)));
		parts.push(
			h(
				'div',
				{ class: 'actions' },
				busy
					? h('button', { onclick: () => void model.cancel() }, 'Cancel')
					: h(
							'button',
							{ class: 'primary', disabled: runnable.size === 0, onclick: () => void model.refreshAll() },
							'Refresh missing',
						),
			),
		);
		this.section.hidden = false;
		this.section.replaceChildren(...parts);
	}
}
