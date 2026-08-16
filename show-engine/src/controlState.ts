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
import type { MukanaEndpoint, MukanaHealth } from "./mukanaClient.js";
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
 * The stable field-name templates a host declares up front — this
 * package's twin of `ControlManifest.StateFields`. `{slot}` is the one
 * RUNTIME template (expanded per live slot); capability names are a closed,
 * fixed set of three, so they're spelled out as concrete keys rather than a
 * second template placeholder syntax — `controlState.test.ts`'s
 * coverage test only ever substitutes `{slot}`.
 *
 * **Deliberately NOT derived from a shared name list with the projection
 * below (fix round 1).** A single `CAPABILITY_NAMES` array feeding both
 * this list and `projectControlFields`'s capability loop let a shrink on
 * either side shrink both — a review found dropping `questionFeed` from
 * that shared array left every structural coverage test green, because the
 * cross-check was comparing a shrunk list against itself. These three
 * strings are independent literals; the projection below instead reads its
 * capability set from `Object.keys(snapshot.capabilities)`, which is fixed
 * by the `ShowCapabilities` type (three required fields) and cannot be
 * shrunk without also changing that type everywhere it's used — so the two
 * sides can only agree by actually agreeing, not by sharing a variable.
 */
export const OHG_FIELD_TEMPLATES: readonly string[] = [
  "ohg/slot/{slot}/name",
  "ohg/slot/{slot}/role",
  "ohg/slot/{slot}/tally",
  "ohg/program/mode",
  "ohg/queue/current",
  "ohg/health/mukana",
  "ohg/capabilities/registry/state",
  "ohg/capabilities/handsQueue/state",
  "ohg/capabilities/questionFeed/state"
];

/**
 * Rank for "worst of three" Mukana-endpoint health (fix round 1).
 * `failing` outranks `dormant` outranks `ok`.
 */
const MUKANA_HEALTH_RANK: Record<MukanaHealth["state"], number> = { ok: 0, dormant: 1, failing: 2 };

/**
 * A single health lamp must mean "is Mukana OK", not "is the panelist
 * registry specifically OK". The first cut of this projection mapped
 * `ohg/health/mukana` to `health.panelists.state` alone — an operator
 * glancing at that one indicator would see green while the hands feed (or
 * the question feed) was dead, because those endpoints have their own
 * `ohg/capabilities/*` fields but never fed this one. Fixed: worst-of-three
 * across all three Mukana endpoints, so ANY endpoint failing makes the lamp
 * read `failing`, and any endpoint dormant (with the rest healthy) makes it
 * read `dormant`.
 */
function worstMukanaHealth(health: Record<MukanaEndpoint, MukanaHealth>): MukanaHealth["state"] {
  let worst: MukanaHealth["state"] = "ok";
  for (const endpoint of Object.values(health)) {
    if (MUKANA_HEALTH_RANK[endpoint.state] > MUKANA_HEALTH_RANK[worst]) {
      worst = endpoint.state;
    }
  }
  return worst;
}

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
  fields["ohg/health/mukana"] = worstMukanaHealth(snapshot.health);

  // `Object.keys`, not a shared name-list constant (fix round 1) — see
  // `OHG_FIELD_TEMPLATES`'s doc comment for why the two sides must not
  // share one array.
  for (const name of Object.keys(snapshot.capabilities) as (keyof ShowCapabilities)[]) {
    fields[`ohg/capabilities/${name}/state`] = snapshot.capabilities[name].state;
  }

  return fields;
}
