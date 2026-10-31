// The Tables section of the pane (SPEC.md §5.4, §5.7, CONTRACTS.md §11.2–11.3):
// each table bound to a query, when it was last refreshed and whether it was
// truncated, and Refresh; while it runs, rows, speed, time and Cancel; its
// history of runs; a refused schema with "Apply new schema"; a stopped write
// with "Retry write".

import type { ConnectionsModel } from '../connections/model';
import type { Binding } from '../writer/bindings';
import { describeParams, describeRun, describeTruncation, formatTime } from '../writer/runs';
import type { TablesModel } from '../writer/tables';
import { h } from './dom';

export class TablesView {
	private readonly list = h('div', { class: 'tables' });

	constructor(
		root: HTMLElement,
		private readonly model: TablesModel,
		private readonly connections: ConnectionsModel,
		/** Asks again about the workbook's queries not trusted (§9.3). */
		private readonly review: () => void,
	) {
		root.append(h('section', { class: 'bound' }, h('h2', {}, 'Tables'), this.list));
		model.onChange(() => this.render());
		connections.onChange(() => this.render());
		this.render();
	}

	/** §9.3 item 5: the user's connections to choose the binding's from. */
	private chooser(binding: Binding): HTMLElement | null {
		const mine = this.connections.connections;
		if (!binding.connection || mine.length === 0) {
			return null;
		}
		const select = h(
			'select',
			{},
			...mine.map((connection) =>
				h(
					'option',
					{ value: connection.name, selected: connection.name === binding.connection!.name },
					`${connection.name} · ${connection.environment} · ${connection.host}`,
				),
			),
		) as HTMLSelectElement;
		return h(
			'div',
			{ class: 'actions' },
			select,
			h(
				'button',
				{
					onclick: () => {
						const chosen = mine.find((connection) => connection.name === select.value);
						if (chosen) {
							void this.model.chooseConnection(binding.binding_id, { name: chosen.name, host: chosen.host });
						}
					},
				},
				'Run on this connection',
			),
		);
	}

	/** The runs of the binding's query, newest first (SPEC.md §5.7). */
	private historyOf(binding: Binding): HTMLElement {
		const runs = this.model.history.get(binding.binding_id);
		if (!runs) {
			return h('div', { class: 'note' }, 'Reading the history…');
		}
		if (runs.length === 0) {
			return h('div', { class: 'note' }, 'No runs of this query from this workbook yet.');
		}
		return h(
			'ul',
			{ class: 'history' },
			...runs.map((run) => {
				const params = describeParams(run.params);
				return h(
					'li',
					{ 'data-status': run.status },
					h('div', {}, describeRun(run)),
					h('div', { class: 'note' }, `definition ${run.query.hash.slice(0, 12)}` + (params ? ` · ${params}` : '')),
					run.error ? h('div', { class: 'problem' }, run.error.message) : null,
				);
			}),
		);
	}

	private render(): void {
		const model = this.model;
		const items: Node[] = [];
		if (model.loading) {
			items.push(
				h(
					'div',
					{ class: 'binding' },
					h('strong', {}, `Loading ${model.loading.query} into a table`),
					model.loading.busy ? h('div', { class: 'signing-in' }, model.loading.busy) : null,
					model.loading.runId
						? h('div', { class: 'actions' }, h('button', { onclick: () => void model.cancel() }, 'Cancel'))
						: null,
					model.loading.error ? h('div', { class: 'problem' }, model.loading.error) : null,
				),
			);
		}
		if (model.bindings.length === 0 && !model.loading) {
			items.push(h('div', { class: 'note' }, 'No tables yet. Select a cell and use Load to table under the query.'));
		}
		for (const binding of model.bindings) {
			const state = model.states.get(binding.binding_id) ?? {};
			const blocked = model.blocked(binding);
			const busy = state.busy !== undefined;
			const last = binding.last_success;
			items.push(
				h(
					'div',
					{ class: 'binding' },
					h(
						'div',
						{ class: 'title' },
						h('strong', {}, binding.target.table),
						h('span', { class: 'host' }, `${binding.target.sheet} · ${binding.query.id}`),
					),
					last
						? h(
								'div',
								{ class: 'state' },
								`Updated ${formatTime(last.at)} · ${last.rows.toLocaleString('en-US')} rows`,
							)
						: null,
					last?.truncated
						? h('div', { class: 'warning' }, h('span', { class: 'badge' }, 'Truncated'), ` ${describeTruncation(last.rows)}`)
						: null,
					binding.partial
						? h('div', { class: 'problem' }, 'Table partially updated: the last write stopped part way')
						: null,
					blocked ? h('div', { class: 'warning' }, blocked) : null,
					blocked && binding.connection && !blocked.startsWith('Query not trusted') ? this.chooser(binding) : null,
					blocked.startsWith('Query not trusted')
						? h('div', { class: 'actions' }, h('button', { onclick: () => this.review() }, 'Review queries'))
						: null,
					state.busy ? h('div', { class: 'signing-in' }, state.busy) : null,
					state.done && !busy ? h('div', { class: 'note' }, state.done) : null,
					state.error && state.error !== blocked ? h('div', { class: 'problem' }, state.error) : null,
					// §5.7: a failed run never takes the last result away.
					state.error && state.error !== blocked && last && !binding.partial
						? h('div', { class: 'note' }, `The table still holds the result of ${formatTime(last.at)}`)
						: null,
					...(state.warnings ?? []).map((warning) => h('div', { class: 'warning' }, warning)),
					h(
						'div',
						{ class: 'actions' },
						h(
							'button',
							{ disabled: busy || blocked !== '', onclick: () => void model.refresh(binding.binding_id) },
							'Refresh',
						),
						state.runId
							? h('button', { onclick: () => void model.cancel(binding.binding_id) }, 'Cancel')
							: null,
						h(
							'button',
							{ onclick: () => void model.toggleHistory(binding.binding_id) },
							model.historyOpen.has(binding.binding_id) ? 'Hide history' : 'History',
						),
						binding.partial
							? h('button', { disabled: busy, onclick: () => void model.retryWrite(binding.binding_id) }, 'Retry write')
							: null,
						state.schema
							? h(
									'button',
									{ disabled: busy, onclick: () => void model.refresh(binding.binding_id, true) },
									'Apply new schema',
								)
							: null,
					),
					model.historyOpen.has(binding.binding_id) ? this.historyOf(binding) : null,
				),
			);
		}
		this.list.replaceChildren(...items);
	}
}
