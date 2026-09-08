// Pure judge for scripts/validate-show-engine.mjs (Plan 7a Task 14 operator drill).
//
// Split into its own file so it can be unit-tested with plain `node --test`
// (see scripts/validate-show-engine-judge.test.mjs / `npm run test:show-engine-drill-judge`)
// without dragging vitest's config resolution into a bare `scripts/tests/*.test.mjs`
// path (known failure mode for this repo's root vitest config) or fighting show-engine's
// TS rootDir by importing a .mjs from outside show-engine/src.
//
// No fetch, no process, no I/O — (manifest, stateBefore, invokeResults, stateAfter) in,
// check rows out. See validate-show-engine.mjs's header for what each check means and why.

/** @typedef {{ name: string, status: "PASS"|"FAIL"|"SKIP", detail: string }} CheckRow */

/** @returns {CheckRow} */
function pass(name, detail = "") {
	return { name, status: "PASS", detail }
}
/** @returns {CheckRow} */
function fail(name, detail) {
	return { name, status: "FAIL", detail }
}
/** @returns {CheckRow} */
function skip(name, detail) {
	return { name, status: "SKIP", detail }
}
/** @returns {CheckRow} */
function check(name, ok, detail) {
	return ok ? pass(name, detail) : fail(name, detail)
}

/**
 * @param {object} ctx
 * @param {any} ctx.manifest - parsed GET /manifest body
 * @param {any} ctx.stateBefore - parsed GET /state body, taken before any invoke
 * @param {object} ctx.invokeResults
 * @param {{status: number, body: any}} [ctx.invokeResults.programPreviewBlack] - POST /invoke ohg.program.preview ["black"]
 * @param {{status: number, body: any}} [ctx.invokeResults.panelistRemove] - POST /invoke ohg.panelist.remove ["0042"]
 * @param {{skipped: true, reason: string} | {skipped: false, status: number, body: any}} [ctx.invokeResults.lookSet]
 * @param {any} ctx.stateAfter - parsed GET /state body, taken after the invokes above
 * @returns {CheckRow[]}
 */
export function judge({ manifest, stateBefore, invokeResults, stateAfter }) {
	const rows = []

	// 1. manifest: exactly 28 ohg.* actions
	const actions = Array.isArray(manifest?.actions) ? manifest.actions : []
	const ohgActions = actions.filter((a) => typeof a?.id === "string" && a.id.startsWith("ohg."))
	rows.push(
		check(
			"manifest: exactly 28 ohg.* actions",
			ohgActions.length === 28,
			`found ${ohgActions.length} (${ohgActions.map((a) => a.id).join(", ") || "none"})`,
		),
	)

	// 2. manifest: feedbackFields includes ohg/health/engine
	const feedbackFields = Array.isArray(manifest?.feedbackFields) ? manifest.feedbackFields : []
	rows.push(
		check(
			'manifest: feedbackFields includes "ohg/health/engine"',
			feedbackFields.includes("ohg/health/engine"),
			`feedbackFields=${JSON.stringify(feedbackFields)}`,
		),
	)

	// 3. state: ohg is present (non-null)
	rows.push(
		check(
			"state: ohg node is present (non-null)",
			stateBefore != null && stateBefore.ohg != null,
			`ohg=${JSON.stringify(stateBefore?.ohg)}`,
		),
	)

	// 4. state: ohgEngineHealth === "running"
	rows.push(
		check(
			'state: ohgEngineHealth === "running"',
			stateBefore?.ohgEngineHealth === "running",
			`ohgEngineHealth=${JSON.stringify(stateBefore?.ohgEngineHealth)}`,
		),
	)

	// 5. invoke ohg.program.preview ["black"] -> HTTP 200 ok:true
	const preview = invokeResults?.programPreviewBlack
	rows.push(
		check(
			'invoke ohg.program.preview ["black"] -> HTTP 200 {ok:true}',
			preview?.status === 200 && preview?.body?.ok === true,
			`status=${preview?.status} body=${JSON.stringify(preview?.body)}`,
		),
	)

	// 6. state after: ohg.program.preview equals the black ProgramSource
	const previewSource = stateAfter?.ohg?.program?.preview
	rows.push(
		check(
			'state: ohg.program.preview === {kind:"black"}',
			previewSource != null && previewSource.kind === "black",
			`ohg.program.preview=${JSON.stringify(previewSource)}`,
		),
	)

	// 7. invoke ohg.panelist.remove ["0042"] -> HTTP 422 refusal
	const remove = invokeResults?.panelistRemove
	rows.push(
		check(
			'invoke ohg.panelist.remove ["0042"] -> HTTP 422 refusal body',
			remove?.status === 422 && remove?.body?.ok === false,
			`status=${remove?.status} body=${JSON.stringify(remove?.body)}`,
		),
	)

	// 8. Task 11 ruling: ohg.look.set of the first configured look.
	const lookSet = invokeResults?.lookSet
	if (!lookSet || lookSet.skipped) {
		rows.push(
			skip(
				"ohg.look.set (first configured look) — Task 11 ruling",
				lookSet?.reason ??
					"no configured-look list is exposed on /state.ohg (only the currently-resolved `look`/`manualBoxes`) — cannot pick 'the first configured look' without reading ShowConfig off disk, which this HTTP-only drill cannot do",
			),
		)
	} else {
		rows.push(
			check(
				"ohg.look.set (first configured look) — Task 11 ruling",
				lookSet.status === 200 && lookSet.body?.ok === true,
				`status=${lookSet.status} body=${JSON.stringify(lookSet.body)}`,
			),
		)
	}

	// 9. Task 11 ruling: nothing on preview for an unfilled box, after the look.set above.
	// Deliberately never asserted: /state carries `nativeProgramVideoSources`/`previewSceneId`
	// for the SCENE bus, not per-box look-render fill, so this can't be judged headlessly.
	rows.push(
		skip(
			"cleared-box blankness (nothing on preview for an unfilled box) — Task 11 ruling",
			"NOT JUDGEABLE from /state (needs a screenshot); see ledger",
		),
	)

	return rows
}

export function anyFailed(rows) {
	return rows.some((r) => r.status === "FAIL")
}

export function formatTable(rows) {
	const nameWidth = Math.max(4, ...rows.map((r) => r.name.length))
	const lines = [`${"STATUS".padEnd(6)}  ${"CHECK".padEnd(nameWidth)}  DETAIL`]
	for (const row of rows) {
		lines.push(`${row.status.padEnd(6)}  ${row.name.padEnd(nameWidth)}  ${row.detail}`)
	}
	return lines.join("\n")
}
