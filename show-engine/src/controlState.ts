/**
 * The flattened control-state projection (Task 9). `ShowSnapshot` is rich,
 * nested JSON — the shape every operator surface renders from. A Bitfocus
 * Companion button or a plain OSC feedback client cannot render that; it can
 * only ever light up on a SCALAR. `projectControlFields` is the one function
 * that turns a snapshot into exactly that: a flat map of `ohg/...` feedback
 * fields, one value each, matching the host stack's own field-name
 * convention (`ControlManifest.cs:37-45` — camelCase segments, a `{slot}`
 * template expanded at runtime, all under a shared prefix) and design spec
 * §4.3's flattened list.
 *
 * Pure: same snapshot in, same record out. No clock, no I/O, no engine
 * instance — this module never imports `ShowEngine`.
 */

import type { ShowCapabilities } from "./contracts.js";
import { formatProgramSource } from "./actions.js";
import type { ShowSnapshot } from "./showSnapshot.js";

/**
 * OSC/Companion feedback carries scalars only — a field whose value is an
 * object or array cannot be fed back onto a physical button. This union is
 * the enforcement, not just a convention: nothing that returns a structured
 * value can typecheck as a control field, so no future field can smuggle a
 * structure through `projectControlFields`'s return type.
 */
export type ControlFieldValue = string | number | boolean | null;

/**
 * The closed set of optional integrations `ShowCapabilities` names. Spelled
 * out once here and reused for both `OHG_FIELD_TEMPLATES` and the
 * projection itself, so the two can never independently drift on which
 * three names exist.
 */
const CAPABILITY_NAMES: readonly (keyof ShowCapabilities)[] = ["registry", "handsQueue", "questionFeed"];

/**
 * The stable field-name templates a host declares up front — this
 * package's twin of `ControlManifest.StateFields`. `{slot}` is the one
 * RUNTIME template (expanded per live slot); capability names are a closed,
 * fixed set of three, so they're spelled out as concrete keys rather than a
 * second template placeholder syntax — `controlState.test.ts`'s
 * coverage test only ever substitutes `{slot}`.
 */
export const OHG_FIELD_TEMPLATES: readonly string[] = [
  "ohg/slot/{slot}/name",
  "ohg/slot/{slot}/role",
  "ohg/slot/{slot}/tally",
  "ohg/program/mode",
  "ohg/queue/current",
  "ohg/health/mukana",
  ...CAPABILITY_NAMES.map((name) => `ohg/capabilities/${name}/state`)
];

/**
 * Per-slot fields for one live slot. An empty seat (`panelist === null`)
 * publishes NO fields at all — not even `tally: false` — because there is
 * no name or role to report for a hole, and a partial field set for it
 * (tally only) would be more misleading than an absent one: a host bridge
 * sees three consistent fields for an occupied slot, and nothing for an
 * empty one, never a slot that half-exists.
 */
function projectSlot(
  fields: Record<string, ControlFieldValue>,
  slotNumber: number,
  panelistName: string,
  panelistRole: string,
  onAir: boolean
): void {
  fields[`ohg/slot/${slotNumber}/name`] = panelistName;
  fields[`ohg/slot/${slotNumber}/role`] = panelistRole;
  fields[`ohg/slot/${slotNumber}/tally`] = onAir;
}

/**
 * Project a `ShowSnapshot` to the flattened `ohg/...` feedback fields a host
 * bridge (Companion, OSC) polls or subscribes to. Every key this produces
 * matches a template in `OHG_FIELD_TEMPLATES` after `{slot}` substitution,
 * and vice versa for every slot this snapshot actually occupies — see the
 * structural coverage tests in `controlState.test.ts`, which is what keeps
 * the two from drifting apart as either changes.
 */
export function projectControlFields(snapshot: ShowSnapshot): Record<string, ControlFieldValue> {
  const fields: Record<string, ControlFieldValue> = {};
  const onAirSlots = new Set(snapshot.tally.onAirSlots);

  for (const slot of snapshot.slots) {
    const panelist = slot.panelist;
    if (panelist === null) continue; // holes publish no fields — see projectSlot's doc comment
    projectSlot(fields, slot.slot, panelist.displayName, panelist.role, onAirSlots.has(slot.slot));
  }

  fields["ohg/program/mode"] = formatProgramSource(snapshot.program.program);
  fields["ohg/queue/current"] = snapshot.queue.current;

  // "mukana" names the panelist-registry sync channel specifically — the
  // `hands`/`question` endpoints already surface through their own
  // capability fields (`handsQueue`/`questionFeed` state, below), so this
  // is `health.panelists`, not an aggregate across all three endpoints.
  fields["ohg/health/mukana"] = snapshot.health.panelists.state;

  for (const name of CAPABILITY_NAMES) {
    fields[`ohg/capabilities/${name}/state`] = snapshot.capabilities[name].state;
  }

  return fields;
}
