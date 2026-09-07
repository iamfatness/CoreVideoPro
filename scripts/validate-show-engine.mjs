#!/usr/bin/env node
import { pathToFileURL } from "node:url"
import { judge, anyFailed, formatTable } from "./validate-show-engine-judge.mjs"

// Re-exported so `judge` stays importable from this file directly (its home per the spec);
// the actual implementation lives in validate-show-engine-judge.mjs so it can be unit-tested
// with plain `node --test` — see that file's header for why.
export { judge, anyFailed, formatTable }

// Operator drill for the OHG show-engine host bridge (Plan 7a Task 14).
//
// THIS IS AN OPERATOR-RUN CHECK, NOT A CI TEST. It talks HTTP to a REAL,
// ALREADY-RUNNING CoreVideoPro.WinUI app on this machine — the CI harness
// (and this script itself) cannot launch the WinUI app. Before running it,
// the owner must have:
//   1. CoreVideoPro.WinUI.exe running, with its control API up on the given
//      --base (default http://127.0.0.1:8011).
//   2. The OHG show engine started against it (real Zoom engine OR the fake
//      engine — either is fine; the drill only cares that the show engine's
//      health reads "running").
//   3. An OHG config (--ohg-config / whatever the shell was launched with)
//      whose PRESETS/looks actually exist — an empty/placeholder config will
//      make `ohg.program.preview`/`ohg.panelist.remove` behave oddly even
//      though the drill's own asserted actions don't depend on panelist
//      content.
//
// usage: node scripts/validate-show-engine.mjs --base http://127.0.0.1:8011 [--token <bearer>]
//
// Checks (spec-listed, Task 14):
//   - GET  /manifest  -> exactly 28 `ohg.*` actions
//   - GET  /manifest  -> feedbackFields includes "ohg/health/engine"
//   - GET  /state      -> `ohg` is present (non-null) and `ohgEngineHealth === "running"`
//   - POST /invoke {action:"ohg.program.preview", args:["black"]} -> HTTP 200 {ok:true}
//   - GET  /state      -> ohg.program.preview === {kind:"black"} (show-engine/src/programBus.ts
//     ProgramState.preview; show-engine/src/contracts.ts ProgramSource for the {kind:"black"} shape)
//   - POST /invoke {action:"ohg.panelist.remove", args:["0042"]} -> HTTP 422 refusal
//     (NOTE: "ohg.panelist.remove"'s wire param is actually a 1-based `slot: int`, not a PIN
//     -- see show-engine/src/actions.ts. "0042" coerces to slot 42, which is out of range for
//     any realistic show config, so LiveSlots.assertSlot throws and the dispatcher turns that
//     into a refused/error ActionResult -> HTTP 422. Functionally identical to "nobody has that
//     PIN": either way, invoking it against a config that does not have 42 seats refuses loudly.)
//   - Task 11 controller ruling: after `ohg.look.set` of the first configured look, nothing
//     should be on preview for an unfilled box. `/state.ohg` carries no configured-look LIST
//     (only the resolved `look`/`manualBoxes` for whatever look is CURRENTLY active), so this
//     drill cannot pick "the first configured look" without reading ShowConfig off disk, which
//     an HTTP-only operator tool has no access to -- this step is SKIPPED with that reason. Even
//     if it could run, the cleared-box "nothing on preview" claim is not judgeable from
//     `/state`'s `nativeProgramVideoSources`/`previewSceneId` (those describe the SCENE bus, not
//     per-box look-render fill) -- printed as NOT JUDGEABLE rather than asserted, per the ruling.
//
// Prints a PASS/FAIL/SKIP table and exits 1 if any check FAILs (SKIP/NOTE rows never fail the run).
//
// The pure judge (manifest, stateBefore, invokeResults, stateAfter) -> check rows lives in
// ./validate-show-engine-judge.mjs (re-exported above) so it can be unit-tested with plain
// `node --test` — see that file's header for why it isn't inline here.

// ---------------------------------------------------------------------------
// Thin CLI: fetch the real endpoints and hand them to judge().
// ---------------------------------------------------------------------------

async function main() {
	const args = process.argv.slice(2)
	const argValue = (name, fallback) => {
		const i = args.indexOf(name)
		return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback
	}
	const base = (argValue("--base", "http://127.0.0.1:8011") ?? "").replace(/\/+$/, "")
	const token = argValue("--token", undefined)

	const headers = { "Content-Type": "application/json" }
	if (token) headers["Authorization"] = `Bearer ${token}`

	/** @param {string} path */
	const getJson = async (path) => {
		const res = await fetch(`${base}${path}`, { headers })
		const body = await res.json().catch(() => null)
		return { status: res.status, body }
	}

	/** @param {string} action @param {unknown[]} actionArgs */
	const invoke = async (action, actionArgs) => {
		const res = await fetch(`${base}/invoke`, {
			method: "POST",
			headers,
			body: JSON.stringify({ action, args: actionArgs }),
		})
		const body = await res.json().catch(() => null)
		return { status: res.status, body }
	}

	console.log(`[validate-show-engine] base=${base}`)

	let manifestRes, stateBefore
	try {
		manifestRes = await getJson("/manifest")
		const stateBeforeRes = await getJson("/state")
		stateBefore = stateBeforeRes.body
	} catch (err) {
		console.error(`[validate-show-engine] FAILED TO CONNECT to ${base}: ${err?.message ?? err}`)
		console.error(
			"[validate-show-engine] Is CoreVideoPro.WinUI running with its control API up on this address? " +
				"See the script header for what must be running first.",
		)
		process.exit(1)
		return
	}

	const manifest = manifestRes.body

	// ohg.look.set: only attempt it if /state.ohg exposes SOME list of configured looks to pick
	// the first entry from. As of Task 14 it does not (only the currently-resolved `look` /
	// `manualBoxes`), so this always SKIPs today — kept as a real lookup (not a hardcoded skip)
	// so a future snapshot field that adds a look list makes this drill start exercising it.
	const configuredLooks =
		Array.isArray(stateBefore?.ohg?.looks) ? stateBefore.ohg.looks : Array.isArray(stateBefore?.ohg?.configuredLooks) ? stateBefore.ohg.configuredLooks : null

	let programPreviewBlack, panelistRemove, lookSet
	try {
		programPreviewBlack = await invoke("ohg.program.preview", ["black"])
		panelistRemove = await invoke("ohg.panelist.remove", ["0042"])

		if (configuredLooks && configuredLooks.length > 0) {
			const firstLookId =
				typeof configuredLooks[0] === "string" ? configuredLooks[0] : configuredLooks[0]?.id ?? configuredLooks[0]?.lookId
			const res = await invoke("ohg.look.set", [firstLookId])
			lookSet = { skipped: false, status: res.status, body: res.body }
		} else {
			lookSet = {
				skipped: true,
				reason:
					"no configured-look list is exposed on /state.ohg (only the currently-resolved `look`/`manualBoxes`) — cannot pick 'the first configured look' without reading ShowConfig off disk, which this HTTP-only drill cannot do",
			}
		}
	} catch (err) {
		console.error(`[validate-show-engine] FAILED mid-drill talking to ${base}: ${err?.message ?? err}`)
		process.exit(1)
		return
	}

	let stateAfter
	try {
		const stateAfterRes = await getJson("/state")
		stateAfter = stateAfterRes.body
	} catch (err) {
		console.error(`[validate-show-engine] FAILED TO CONNECT to ${base} reading final /state: ${err?.message ?? err}`)
		process.exit(1)
		return
	}

	const rows = judge({
		manifest,
		stateBefore,
		invokeResults: { programPreviewBlack, panelistRemove, lookSet },
		stateAfter,
	})

	console.log("")
	console.log(formatTable(rows))
	console.log("")

	if (anyFailed(rows)) {
		console.error("[validate-show-engine] FAIL — one or more checks failed.")
		process.exit(1)
	}

	console.log("[validate-show-engine] PASS")
}

const isMain = process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href
if (isMain) {
	main()
}
