// The Imports section of the pane (SPEC.md §5.6, CONTRACTS.md §12): "Import
// table" from the selection, the guessed column types to check and change, and
// the workbook's imports with "Import again" and whether they are imported
// again before each run of a Local query that reads them.

import { IMPORT_TYPES } from '../importer/infer';
import type { ImportDraft, ImportsModel } from '../importer/imports';
import { h } from './dom';

export class ImportsView {
	private readonly body = h('div', {});

	constructor(
		root: HTMLElement,
		private readonly model: ImportsModel,
	) {
		root.append(h('section', { class: 'imports' }, h('h2', {}, 'Imports'), this.body));
		model.onChange(() => this.render());
		this.render();
	}

	private draftView(draft: ImportDraft): HTMLElement {
		const model = this.model;
		const busy = draft.busy !== undefined;
		const source = draft.shape.ref.table ?? `${draft.shape.ref.sheet}!${draft.shape.ref.range}`;
		const name = h('input', { type: 'text', value: draft.name, disabled: busy }) as HTMLInputElement;
		name.addEventListener('change', () => model.setDraft({ name: name.value }));
		const reimport = h('input', { type: 'checkbox', checked: draft.reimport, disabled: busy }) as HTMLInputElement;
		reimport.addEventListener('change', () => model.setDraft({ reimport: reimport.checked }));
		return h(
			'div',
			{ class: 'binding' },
			h('strong', {}, `Import ${source}`),
			h('div', { class: 'note' }, `${draft.shape.rows.toLocaleString('en-US')} rows · ${draft.columns.length} columns`),
			h('label', { class: 'field' }, 'Name ', name),
			h('div', { class: 'note' }, `Local SQL reads it as xl_import.${draft.name}`),
			h(
				'table',
				{ class: 'columns' },
				...draft.columns.map((column, index) => {
					const select = h(
						'select',
						{ disabled: busy },
						...IMPORT_TYPES.map((type) =>
							h('option', { value: type, selected: type === column.type }, type === column.guessed ? `${type} (guessed)` : type),
						),
					) as HTMLSelectElement;
					select.addEventListener('change', () => model.setDraft({ column: { index, type: select.value } }));
					return h('tr', {}, h('td', {}, column.name), h('td', {}, select));
				}),
			),
			h('label', { class: 'field' }, reimport, ' Import again before each run of a Local query that reads it'),
			draft.errors
				? h('div', { class: 'warning' }, `${draft.errors.toLocaleString('en-US')} cells hold an Excel error and are imported empty`)
				: null,
			busy ? h('div', { class: 'signing-in' }, draft.busy ?? '') : null,
			draft.error ? h('div', { class: 'problem' }, draft.error) : null,
			h(
				'div',
				{ class: 'actions' },
				h('button', { class: 'primary', disabled: busy, onclick: () => void model.importDraft() }, 'Import'),
				h('button', { disabled: busy, onclick: () => model.cancelDraft() }, 'Cancel'),
			),
		);
	}

	private render(): void {
		const model = this.model;
		const items: Array<HTMLElement | null> = [];
		if (model.draft) {
			items.push(this.draftView(model.draft));
		} else {
			items.push(
				h(
					'div',
					{ class: 'actions' },
					h(
						'button',
						{ disabled: model.reading?.busy !== undefined, onclick: () => void model.analyze() },
						'Import table',
					),
				),
				model.reading?.busy ? h('div', { class: 'signing-in' }, model.reading.busy) : null,
				model.reading?.error ? h('div', { class: 'problem' }, model.reading.error) : null,
			);
			if (model.imports.length === 0 && !model.reading) {
				items.push(
					h('div', { class: 'note' }, 'Select a table, or a range with column names in its first row, and use Import table.'),
				);
			}
		}
		for (const entry of model.imports) {
			const state = model.states.get(entry.name) ?? {};
			const busy = state.busy !== undefined;
			const reimport = h('input', { type: 'checkbox', checked: entry.reimport_before_run, disabled: busy }) as HTMLInputElement;
			reimport.addEventListener('change', () => void model.setReimportBeforeRun(entry.name, reimport.checked));
			items.push(
				h(
					'div',
					{ class: 'binding' },
					h(
						'div',
						{ class: 'title' },
						h('strong', {}, `xl_import.${entry.name}`),
						h('span', { class: 'host' }, entry.table ?? `${entry.sheet}!${entry.range}`),
					),
					h('div', { class: 'note' }, entry.columns.map((column) => `${column.name} ${column.type}`).join(', ')),
					h('label', { class: 'field' }, reimport, ' Import again before each run of a Local query that reads it'),
					state.busy ? h('div', { class: 'signing-in' }, state.busy) : null,
					state.done && !busy ? h('div', { class: 'note' }, state.done) : null,
					state.error ? h('div', { class: 'problem' }, state.error) : null,
					h(
						'div',
						{ class: 'actions' },
						h('button', { disabled: busy, onclick: () => void model.reimport(entry.name) }, 'Import again'),
					),
				),
			);
		}
		this.body.replaceChildren(...items.filter((item): item is HTMLElement => item !== null));
	}
}
