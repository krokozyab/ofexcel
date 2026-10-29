// Custom functions of the add-in probe. Loaded by taskpane.html, which is the
// shared runtime, so these run in the same page as the task pane.
//
// What they answer (CONTRACTS.md §16, PHASE0_REPORT.md §7):
//   ARGS, SCALARARGS -- A7: how empty cells, omitted and empty-string arguments arrive;
//   META, NEW        -- check 9: which metadata Excel uses after the server changes it;
//   LATE             -- whether an association made after an await still works,
//                       which is how the real dispatcher registers functions;
//   MATRIX           -- a spilled result.

(function () {
	'use strict';

	window.probeRuntime = window.probeRuntime || {
		startedAt: new Date().toISOString(),
		metadataVariant: -1,
		lateAssociated: false,
		associateErrors: [],
	};
	const runtime = window.probeRuntime;

	function describeScalar(value) {
		return {
			type: value === null ? 'null' : typeof value,
			value: value === undefined ? '(undefined)' : value,
			isEmptyString: value === '',
		};
	}

	function describe(value) {
		if (Array.isArray(value)) {
			return {
				kind: 'matrix',
				rows: value.length,
				cols: value.length && Array.isArray(value[0]) ? value[0].length : 0,
				cells: value.map((row) => (Array.isArray(row) ? row.map(describeScalar) : describeScalar(row))),
			};
		}
		return describeScalar(value);
	}

	function associate(id, implementation) {
		try {
			CustomFunctions.associate(id, implementation);
		} catch (error) {
			runtime.associateErrors.push(`${id}: ${error && error.message}`);
		}
	}

	// A repeating parameter arrives as one array holding every repetition.
	associate('ARGS', (args) => JSON.stringify({ count: args ? args.length : 0, args: (args || []).map(describe) }));
	associate('SCALARARGS', (a, b) => JSON.stringify({ a: describe(a), b: describe(b) }));
	associate('META', () => runtime.metadataVariant);
	associate('NEW', () => 2);
	associate('MATRIX', (rows, cols) => {
		const matrix = [];
		for (let r = 0; r < rows; r++) {
			const row = [];
			for (let c = 0; c < cols; c++) {
				row.push(r * cols + c);
			}
			matrix.push(row);
		}
		return matrix;
	});

	// The real dispatcher fetches functions.json and associates what it finds,
	// so its associations happen after an await. LATE stands in for that.
	fetch('/state', { cache: 'no-store' })
		.then((response) => response.json())
		.then((state) => {
			runtime.metadataVariant = state.metadata_variant;
		})
		.catch((error) => runtime.associateErrors.push(`state: ${error && error.message}`))
		.then(() => new Promise((resolve) => setTimeout(resolve, 2000)))
		.then(() => {
			associate('LATE', () => 'late association works');
			runtime.lateAssociated = true;
		});
})();
