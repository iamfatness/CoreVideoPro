// RC5 (docs/superpowers/plans/2026-11-21-mac-control-audio-phase2.md): the module's own
// parsing and presets against what a macOS CoreVideo Pro actually serves. The fixture is
// written by the Mac shell harness case control/companion-request-shapes, which also
// replays this module's request shapes over a real socket and fails when the fixture
// drifts from the Mac server.
import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { normalizeManifest } from './manifest.js'
import { buildPresets } from './presets.js'
import { FEEDBACK_FLAGS } from './feedbacks.js'
import { VARIABLE_FIELDS } from './variables.js'

const fixture = JSON.parse(readFileSync(new URL('./fixtures/mac-control.json', import.meta.url), 'utf8')) as {
	manifest: unknown
	stateKeys: string[]
}

/** What a Mac does not have yet, with the reason. Adding a row here is a decision, not a fix. */
const MAC_ABSENT_ACTIONS: Record<string, string> = {
	'automation.autoAssignInputs.set': 'blocked on STU (Studio and Settings follow-ons); the Mac answers 422',
}
const MAC_ABSENT_FIELDS: Record<string, string> = {
	autoAssignInputs: 'its action is blocked on STU',
	autoLowerThirds: 'graphics rules are blocked on GFX',
	viewMode: 'view.setMode is blocked on STU (owner decision 3)',
}

describe('macOS control server (recorded fixture)', () => {
	const manifest = normalizeManifest(fixture.manifest)

	it('parses the Mac manifest with the module normalizer', () => {
		expect(manifest.actions.length).toBe(40)
		expect(manifest.actions.every((a) => a.id && a.title)).toBe(true)
		expect(manifest.root).toBe('/cvp')
	})

	it('every preset action is on the Mac or documented absent', () => {
		const ids = new Set(manifest.actions.map((a) => a.id))
		type Steps = Array<{ down: Array<{ actionId: string }> }>
		const presetActions = Object.values(buildPresets()).flatMap((preset) =>
			((preset as { steps?: Steps } | undefined)?.steps ?? []).flatMap((step) => step.down.map((d) => d.actionId)),
		)
		expect(presetActions.length).toBe(7)
		for (const id of presetActions) {
			expect(ids.has(id) || id in MAC_ABSENT_ACTIONS, `${id} on the Mac or documented absent`).toBe(true)
			expect(ids.has(id) && id in MAC_ABSENT_ACTIONS, `${id} cannot be both present and documented absent`).toBe(false)
		}
	})

	it('every feedback and variable the module reads is published by the Mac or documented absent', () => {
		const keys = new Set(fixture.stateKeys)
		for (const id of [...FEEDBACK_FLAGS.map((f) => f.id), ...VARIABLE_FIELDS.map((v) => v.id)]) {
			expect(keys.has(id) || id in MAC_ABSENT_FIELDS, `${id} published or documented absent`).toBe(true)
			expect(keys.has(id) && id in MAC_ABSENT_FIELDS, `${id} cannot be both`).toBe(false)
		}
	})
})
