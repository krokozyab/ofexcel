// The Query section of the pane (SPEC.md §5.2 items 2–5): the SQL editor
// (CodeMirror 6) with the Oracle | Local switch and the connection's label,
// the problems the daemon finds as the text is typed, the parameters and
// where their values come from, Preview on its button only, and saving.

import { autocompletion, closeBrackets, closeBracketsKeymap, completionKeymap } from '@codemirror/autocomplete';
import { defaultKeymap, history, historyKeymap, indentWithTab } from '@codemirror/commands';
import { PLSQL, PostgreSQL, sql } from '@codemirror/lang-sql';
import { bracketMatching, defaultHighlightStyle, syntaxHighlighting } from '@codemirror/language';
import { Compartment, EditorState } from '@codemirror/state';
import { drawSelection, EditorView, keymap, lineNumbers } from '@codemirror/view';

import type { DaemonClient } from '../api/client';
import type { ConnectionsModel } from '../connections/model';
import { sqlCompletions } from '../editor/completions';
import { type EditorModel, ORACLE_DATE_HINT, type ParamSource } from '../editor/model';
import { directive, type Engine } from '../editor/query_text';
import { h } from './dom';

/** How long typing pauses before the text is checked; checking never reaches Fusion. */
const VALIDATE_AFTER_MS = 400;
const PROJECT_SETTING = 'qexcel.project';
const DEFAULT_PROJECT = 'excel';
const PARAM_TYPES = ['string', 'integer', 'number', 'decimal(18,2)', 'date', 'timestamp', 'boolean', 'id', 'list<id>', 'list<string>'];

export class EditorSection {
	private readonly editor: EditorView;
	private readonly dialect = new Compartment();
	private validateTimer: ReturnType<typeof setTimeout> | undefined;
	private paramsSignature = '';
	private shownEngine: Engine;

	private readonly engineButtons = new Map<Engine, HTMLButtonElement>();
	private readonly connectionSelect = h('select', { class: 'connection-select' }) as HTMLSelectElement;
	private readonly label = h('div', { class: 'label' });
	private readonly problems = h('div', { class: 'problems' });
	private readonly paramsBox = h('div', { class: 'params' });
	private readonly previewButton = h('button', { class: 'primary' }, 'Preview') as HTMLButtonElement;
	private readonly previewBox = h('div', { class: 'preview' });
	private readonly projectInput = h('input', { class: 'project', title: 'Project: a folder under ~/QExcel/projects' }) as HTMLInputElement;
	private readonly saveStatus = h('div', { class: 'save-status', role: 'status' });
	private readonly saveButtons: HTMLButtonElement[] = [];

	constructor(
		root: HTMLElement,
		private readonly model: EditorModel,
		private readonly connections: ConnectionsModel,
		client: DaemonClient,
	) {
		this.shownEngine = model.engine;
		this.editor = new EditorView({
			parent: h('div', { class: 'editor' }),
			state: EditorState.create({
				doc: model.text,
				extensions: [
					lineNumbers(),
					history(),
					drawSelection(),
					closeBrackets(),
					bracketMatching(),
					syntaxHighlighting(defaultHighlightStyle, { fallback: true }),
					this.dialect.of(sql({ dialect: model.engine === 'oracle' ? PLSQL : PostgreSQL })),
					autocompletion({
						override: [sqlCompletions(client, { engine: () => model.engine, connection: () => model.connection })],
					}),
					keymap.of([...closeBracketsKeymap, ...defaultKeymap, ...historyKeymap, ...completionKeymap, indentWithTab]),
					EditorView.lineWrapping,
					EditorView.updateListener.of((update) => {
						if (update.docChanged) {
							model.setText(update.state.doc.toString());
							this.scheduleValidate();
						}
					}),
				],
			}),
		});

		for (const engine of ['oracle', 'local'] as Engine[]) {
			const button = h(
				'button',
				{ class: 'segment', onclick: () => model.setEngine(engine) },
				engine === 'oracle' ? 'Oracle' : 'Local',
			) as HTMLButtonElement;
			this.engineButtons.set(engine, button);
		}
		this.connectionSelect.addEventListener('change', () => model.setConnection(this.connectionSelect.value));
		this.previewButton.addEventListener('click', () => void model.runPreview());
		this.projectInput.value = readSetting(PROJECT_SETTING) || DEFAULT_PROJECT;
		const toProject = h('button', { onclick: () => this.saveToProject() }, 'Save to project') as HTMLButtonElement;
		const toWorkbook = h('button', { onclick: () => this.saveToWorkbook() }, 'Save to workbook') as HTMLButtonElement;
		this.saveButtons.push(toProject, toWorkbook);

		root.append(
			h(
				'section',
				{ class: 'query' },
				h(
					'div',
					{ class: 'toolbar' },
					h('h2', {}, 'Query'),
					h('button', { onclick: () => model.newQuery(model.engine) }, 'New'),
				),
				h(
					'div',
					{ class: 'toolbar' },
					h('div', { class: 'segments' }, ...this.engineButtons.values()),
					this.connectionSelect,
				),
				this.label,
				this.editor.dom.parentElement!,
				this.problems,
				this.paramsBox,
				h('div', { class: 'toolbar' }, this.previewButton),
				this.previewBox,
				h('div', { class: 'toolbar save' }, h('label', {}, 'Project ', this.projectInput), toProject, toWorkbook),
				this.saveStatus,
			),
		);

		model.onChange(() => this.render());
		connections.onChange(() => this.renderConnections());
		this.renderConnections();
		this.render();
		void model.validate();
	}

	private scheduleValidate(): void {
		clearTimeout(this.validateTimer);
		this.validateTimer = setTimeout(() => void this.model.validate(), VALIDATE_AFTER_MS);
	}

	private render(): void {
		const model = this.model;
		// A change the model made itself (Oracle | Local, a declared parameter).
		if (model.text !== this.editor.state.doc.toString()) {
			this.editor.dispatch({ changes: { from: 0, to: this.editor.state.doc.length, insert: model.text } });
		}
		const engine = model.engine;
		if (engine !== this.shownEngine) {
			this.shownEngine = engine;
			this.editor.dispatch({ effects: this.dialect.reconfigure(sql({ dialect: engine === 'oracle' ? PLSQL : PostgreSQL })) });
		}
		for (const [key, button] of this.engineButtons) {
			button.classList.toggle('selected', key === engine);
		}
		this.renderLabel();
		this.renderProblems();
		this.renderParams();
		this.renderPreview();
		this.previewButton.disabled = model.previewing || (engine === 'oracle' && !model.connection);
		this.previewButton.textContent = model.previewing ? 'Previewing…' : 'Preview';
		this.previewButton.title =
			engine === 'oracle' && !model.connection ? 'Choose the connection to preview through' : 'First 200 rows';
		for (const button of this.saveButtons) {
			button.disabled = model.saving;
		}
		this.saveStatus.textContent = model.saving ? 'Saving…' : (model.saved?.message ?? '');
		this.saveStatus.className = `save-status ${model.saved && !model.saved.ok ? 'problem' : 'ok'}`;
	}

	/** Where the SQL runs, always in sight (SPEC.md §4): "Oracle · dev · DEV" or "Local · DuckDB". */
	private renderLabel(): void {
		if (this.model.engine === 'local') {
			this.label.replaceChildren('Local · DuckDB');
			this.label.removeAttribute('data-environment');
			return;
		}
		const connection = this.connections.connections.find((item) => item.name === this.model.connection);
		if (!connection) {
			this.label.replaceChildren('Oracle · no connection');
			this.label.removeAttribute('data-environment');
			return;
		}
		this.label.dataset.environment = connection.environment;
		this.label.replaceChildren(
			`Oracle · ${connection.name} · `,
			h('span', { class: 'environment' }, connection.environment),
			connection.signed_in ? '' : ' · not signed in',
		);
	}

	private renderConnections(): void {
		const names = this.connections.connections.map((connection) => connection.name);
		const wanted = this.model.connection || directive(this.model.text, 'connection') || '';
		const chosen = names.includes(wanted)
			? wanted
			: (this.connections.connections.find((connection) => connection.signed_in)?.name ?? names[0] ?? '');
		this.connectionSelect.replaceChildren(
			...this.connections.connections.map((connection) =>
				h('option', { value: connection.name, selected: connection.name === chosen }, `${connection.name} · ${connection.environment}`),
			),
		);
		this.connectionSelect.disabled = names.length === 0;
		if (chosen !== this.model.connection) {
			this.model.setConnection(chosen);
		} else {
			this.renderLabel();
		}
	}

	private renderProblems(): void {
		const model = this.model;
		const items: Node[] = [];
		if (model.validationError) {
			items.push(h('div', { class: 'problem' }, `The query cannot be checked: ${model.validationError}`));
		}
		for (const error of model.validation?.errors ?? []) {
			items.push(h('div', { class: 'problem' }, error.line ? `Line ${error.line}: ${error.message}` : error.message));
		}
		for (const warning of model.validation?.warnings ?? []) {
			if (warning.code !== 'W_PARAM_UNUSED') {
				items.push(h('div', { class: 'warning' }, warning.message));
			}
		}
		this.problems.replaceChildren(...items);
	}

	/** Rebuilt only when the parameters or their kinds of source change, so that
	 * a value being typed keeps its focus. */
	private renderParams(): void {
		const model = this.model;
		const signature = JSON.stringify([
			model.params.map((param) => [param.name, param.type, param.default, model.sources.get(param.name)?.kind]),
			model.undeclared,
			model.engine,
		]);
		if (signature === this.paramsSignature) {
			return;
		}
		this.paramsSignature = signature;
		const rows: Node[] = [];
		if (model.params.length || model.undeclared.length) {
			rows.push(h('h3', {}, 'Parameters'));
		}
		for (const param of model.params) {
			rows.push(this.paramRow(param.name, param.type, param.default));
		}
		for (const name of model.undeclared) {
			rows.push(this.declareRow(name));
		}
		this.paramsBox.replaceChildren(...rows);
	}

	private paramRow(name: string, type: string, defaultValue: string | undefined): HTMLElement {
		const source: ParamSource = this.model.sources.get(name) ?? { kind: 'value', value: '' };
		const kind = h(
			'select',
			{
				onchange: (event: Event) => {
					const value = (event.target as HTMLSelectElement).value;
					this.model.setSource(name, value === 'cell' ? { kind: 'cell', ref: '' } : { kind: 'value', value: '' });
				},
			},
			h('option', { value: 'value', selected: source.kind === 'value' }, 'Value'),
			h('option', { value: 'cell', selected: source.kind === 'cell' }, 'Cell'),
		);
		let input: HTMLElement;
		if (source.kind === 'cell') {
			const ref = h('input', { placeholder: 'Sheet1!B2', value: source.ref }) as HTMLInputElement;
			ref.addEventListener('input', () => this.model.sources.set(name, { kind: 'cell', ref: ref.value }));
			input = h(
				'span',
				{ class: 'cell' },
				ref,
				h(
					'button',
					{
						onclick: () => {
							void this.model.useSelectedCell(name).then(() => {
								ref.value = (this.model.sources.get(name) as { ref: string }).ref;
							});
						},
					},
					'Selected cell',
				),
			);
		} else {
			const value = h('input', {
				placeholder: defaultValue === undefined ? 'required' : `default: ${defaultValue || '""'}`,
				value: source.value,
			}) as HTMLInputElement;
			value.addEventListener('input', () => this.model.sources.set(name, { kind: 'value', value: value.value }));
			input = value;
		}
		return h('div', { class: 'param' }, h('span', { class: 'name' }, name, h('small', {}, ` ${type}`)), kind, input);
	}

	private declareRow(name: string): HTMLElement {
		const type = h('select', {}, ...PARAM_TYPES.map((value) => h('option', { value }, value))) as HTMLSelectElement;
		const fallback = h('input', { placeholder: 'default (optional)' }) as HTMLInputElement;
		return h(
			'div',
			{ class: 'param undeclared' },
			h('span', { class: 'name' }, `${this.model.engine === 'oracle' ? ':' : '$'}${name}`, h('small', {}, ' not declared')),
			type,
			fallback,
			h(
				'button',
				{
					onclick: () =>
						this.model.declare({
							name,
							type: type.value,
							...(fallback.value !== '' ? { defaultValue: fallback.value } : {}),
						}),
				},
				'Declare',
			),
		);
	}

	private renderPreview(): void {
		const model = this.model;
		if (model.previewError) {
			this.previewBox.replaceChildren(h('div', { class: 'problem' }, model.previewError));
			return;
		}
		const preview = model.preview;
		if (!preview) {
			this.previewBox.replaceChildren();
			return;
		}
		const summary = `${preview.rows.length} rows in ${(preview.elapsed_ms / 1000).toFixed(1)} s` +
			(preview.truncated ? ' · more rows than shown' : '');
		const table = h(
			'table',
			{},
			h(
				'thead',
				{},
				h('tr', {}, ...preview.columns.map((column) => h('th', { title: column.name }, column.header, h('small', {}, column.type)))),
			),
			h(
				'tbody',
				{},
				...preview.rows.map((row) =>
					h('tr', {}, ...row.map((cell) => h('td', { class: cell === null ? 'null' : '' }, cell === null ? '' : String(cell)))),
				),
			),
		);
		const dates = model.dateColumns;
		const parts: Node[] = [h('div', { class: 'note' }, summary), h('div', { class: 'grid' }, table)];
		if (dates.length) {
			parts.push(h('div', { class: 'warning' }, `${dates.join(', ')}: ${ORACLE_DATE_HINT}`));
		}
		for (const warning of preview.warnings) {
			if (warning.code !== 'W_PARAM_UNUSED') {
				parts.push(h('div', { class: 'warning' }, warning.message));
			}
		}
		if (preview.rendered_sql) {
			parts.push(h('details', {}, h('summary', {}, 'SQL sent to Fusion'), h('pre', {}, preview.rendered_sql)));
		}
		this.previewBox.replaceChildren(...parts);
	}

	private saveToProject(): void {
		const project = this.projectInput.value.trim() || DEFAULT_PROJECT;
		writeSetting(PROJECT_SETTING, project);
		void this.model.saveToProject(project);
	}

	private saveToWorkbook(): void {
		const connection = this.connections.connections.find((item) => item.name === this.model.connection);
		void this.model.saveToWorkbook(connection?.user);
	}
}

// The project name is a convenience of this machine: per viewer, in local storage.
function readSetting(key: string): string {
	try {
		return localStorage.getItem(key) ?? '';
	} catch {
		return '';
	}
}

function writeSetting(key: string, value: string): void {
	try {
		localStorage.setItem(key, value);
	} catch {
		// Private mode or blocked storage: the name is simply not remembered.
	}
}
