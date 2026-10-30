// Building the pane's DOM without a framework: an element, its attributes and
// listeners, its children. Text goes in as text, never as HTML.

type Child = Node | string | null | undefined | false;

export interface Attributes {
	[name: string]: string | boolean | ((event: Event) => void) | undefined;
}

/** h('button', { class: 'primary', onclick: go }, 'Sign in'). An `on…` key is
 * a listener; `true` sets a boolean attribute, `false` and undefined leave it out. */
export function h(tag: string, attributes: Attributes = {}, ...children: Child[]): HTMLElement {
	const element = document.createElement(tag);
	for (const [name, value] of Object.entries(attributes)) {
		if (typeof value === 'function') {
			element.addEventListener(name.slice(2), value);
		} else if (value === true) {
			element.setAttribute(name, '');
		} else if (typeof value === 'string') {
			element.setAttribute(name, value);
		}
	}
	for (const child of children) {
		if (child !== null && child !== undefined && child !== false) {
			element.append(child);
		}
	}
	return element;
}
