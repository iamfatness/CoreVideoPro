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
