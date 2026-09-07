/**
 * Pure helpers for turning the shell's `feedbackFields` list (GET /manifest,
 * Task 5 — the static `ControlManifest.StateFields` plus each
 * `IControlActionProvider`'s `FeedbackFieldTemplates`, e.g. the OHG show
 * engine's `OHG_FIELD_TEMPLATES`) into concrete Companion variable ids and
 * feedback registrations.
 *
 * Only `ohg/`-prefixed fields are handled here — the static, non-`ohg/`
 * fields (`recording`, `input/{slot}/inShow`, ...) already have their own
 * hand-authored entries in variables.ts/feedbacks.ts and are ignored by
 * this module so the two mechanisms never double-register the same field.
 */

/** Number of OHG "slots" the shell exposes ({slot} placeholders expand 1..slots). */
const DEFAULT_SLOT_COUNT = 10

/**
 * Expand every `ohg/` field in `fields`, substituting `{slot}` with each of
 * `1..slots` (inclusive). Fields with no `{slot}` placeholder pass through
 * unchanged. Any field that does not start with `ohg/` is dropped — those
 * are handled by the existing static VARIABLE_FIELDS/FEEDBACK_FLAGS lists.
 */
export function expandFeedbackFields(fields: readonly string[], slots: number = DEFAULT_SLOT_COUNT): string[] {
	const out: string[] = []
	for (const field of fields) {
		if (!field.startsWith('ohg/')) continue
		if (field.includes('{slot}')) {
			for (let slot = 1; slot <= slots; slot++) {
				out.push(field.replace('{slot}', String(slot)))
			}
		} else {
			out.push(field)
		}
	}
	return out
}

/** `ohg/slot/3/tally` -> `ohg_slot_3_tally`. Companion variable/feedback ids can't contain `/`. */
export function variableIdFor(field: string): string {
	return field.replace(/\//g, '_')
}

/** Whether an (already-expanded) field is a tally lamp worth a boolean feedback. */
export function isTallyField(field: string): boolean {
	return field.endsWith('/tally')
}

/**
 * The `ohg/` fields the SHELL publishes as top-level ControlState scalars
 * (`ohgEngineHealth` / `ohgShadowLastCommand`) rather than inside the flat
 * `ohgFields` map. They appear in `/manifest`'s feedbackFields (they are
 * `ControlManifest.StateFields` entries), and their expanded ids collide
 * exactly with the module's hand-authored `ohg_health_engine` /
 * `ohg_shadow_lastCommand` variables — so expanding them would register the
 * same two variable ids twice and then look them up in a map that never
 * carries them. Dropped here; the hand-authored pair keeps the friendly names
 * and reads from the scalars.
 */
export const SHELL_SCALAR_FIELDS: readonly string[] = ['ohg/health/engine', 'ohg/shadow/lastCommand']

/** Drop the shell-scalar fields from an expanded field list. */
export function withoutShellScalarFields(fields: readonly string[]): string[] {
	return fields.filter((field) => !SHELL_SCALAR_FIELDS.includes(field))
}
