// The custom functions in the shared runtime (SPEC.md §5.5, CONTRACTS.md
// §10.1): functions.json read after the page loads and each function in it
// associated with its implementation -- the phase 0 probe showed that
// CustomFunctions.associate works after an await (PHASE0_REPORT.md §4.8).
// When the daemon's data or catalog moves, or the status or the tables'
// refresh times do, the workbook is recalculated so the formulas follow.

import type { FormulaCall } from './batcher';
import { FormulaError, type FormulaService } from './formulas';
import type { FormulaStatus } from './local';

/** What of the custom functions runtime this add-in uses; the typings are a
 * separate package, which this one does without. */
declare const CustomFunctions: {
	associate(id: string, implementation: (...args: never[]) => unknown): void;
	Error: new (code: string, message?: string) => Error;
	ErrorCode: Record<string, string>;
};

/** Excel shows at most 255 characters of an error's message. */
const MAX_MESSAGE = 255;

export interface FormulaRuntime {
	service: FormulaService;
	/** OF.ASOF(): the earliest last refresh of the workbook's tables, or undefined. */
	workbookAsof(): Promise<number | undefined>;
	status(): FormulaStatus;
}

interface FunctionMetadata {
	id: string;
	name: string;
}

function isEmpty(value: unknown): boolean {
	return value === null || value === undefined || value === '';
}

/** The implementations, by the ids of functions.json. */
export function implementations(runtime: FormulaRuntime): Record<string, (...args: unknown[]) => Promise<unknown> | unknown> {
	const query = (kind: FormulaCall['kind']) => (name: unknown, args: unknown) =>
		runtime.service.evaluate(kind, name, (args as unknown[][][] | null | undefined) ?? []);
	return {
		TABLE: query('table'),
		CALL: query('call'),
		ASOF: async (name: unknown, args: unknown) => {
			if (!isEmpty(name)) {
				return query('asof')(name, args);
			}
			const asof = await runtime.workbookAsof();
			if (asof === undefined) {
				throw new FormulaError('notAvailable', 'No table of this workbook has been refreshed yet');
			}
			return asof;
		},
		STATUS: () => runtime.status(),
	};
}

/** The error code a cell shows, and whether it takes a message. A custom
 * function cannot return #NAME? (Excel keeps it for errors of input), and
 * only #VALUE! and #N/A carry a message of their own: an unknown query is a
 * wrong value of the first argument, #VALUE! with the daemon's words. */
export function cellError(code: string): { code: string; message: boolean } {
	switch (code) {
		case 'invalidValue':
		case 'invalidName':
			return { code: 'invalidValue', message: true };
		case 'notAvailable':
			return { code: 'notAvailable', message: true };
		case 'divisionByZero':
		case 'invalidNumber':
		case 'nullReference':
			return { code, message: false };
		default:
			return { code: 'notAvailable', message: true };
	}
}

/** A FormulaError, or any other, as the error Excel shows in the cell. */
function excelError(error: unknown): Error {
	const shown = cellError(error instanceof FormulaError ? error.code : 'notAvailable');
	const code = CustomFunctions.ErrorCode[shown.code] ?? CustomFunctions.ErrorCode.notAvailable;
	if (!shown.message) {
		return new CustomFunctions.Error(code);
	}
	return new CustomFunctions.Error(code, (error instanceof Error ? error.message : String(error)).slice(0, MAX_MESSAGE));
}

/** Reads functions.json and associates each function; resolves with the ids
 * associated. Outside Excel there is nothing to associate with. */
export async function registerFunctions(runtime: FormulaRuntime, metadataUrl = 'functions.json'): Promise<string[]> {
	if (typeof CustomFunctions === 'undefined') {
		return [];
	}
	const metadata = (await (await fetch(metadataUrl, { cache: 'no-store' })).json()) as { functions: FunctionMetadata[] };
	const known = implementations(runtime);
	const associated: string[] = [];
	for (const { id, name } of metadata.functions) {
		const implementation =
			known[id] ??
			(() => {
				throw new FormulaError('notAvailable', `OF.${name} is not available in this version of QExcel`);
			});
		CustomFunctions.associate(id, async (...args: never[]) => {
			try {
				return await implementation(...(args as unknown[]));
			} catch (error) {
				throw excelError(error);
			}
		});
		associated.push(id);
	}
	return associated;
}

/** Recalculates the workbook once, a moment after the last request, so that a
 * burst of changes costs one recalculation. */
export class Recalculation {
	private handle: ReturnType<typeof setTimeout> | undefined;

	constructor(
		private readonly recalculate: () => Promise<void> = fullRecalculation,
		private readonly delayMs = 300,
	) {}

	request(): void {
		if (this.handle !== undefined) {
			clearTimeout(this.handle);
		}
		this.handle = setTimeout(() => {
			this.handle = undefined;
			void this.recalculate().catch(() => {
				// Excel busy, or closing: the formulas follow on the next change.
			});
		}, this.delayMs);
	}
}

/** Every formula of the workbook, ours among them: Excel cannot be told which
 * cells hold OF functions without a search of every sheet. */
export async function fullRecalculation(): Promise<void> {
	await Excel.run(async (context) => {
		context.workbook.application.calculate(Excel.CalculationType.full);
		await context.sync();
	});
}
