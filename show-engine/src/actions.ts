/**
 * The `ohg.*` action registry (Task 8) — the declarative surface every host
 * bridge (Companion, OSC, the three native panels) invokes against. Spec
 * §4.2 says "the engine registers `ohg.*` actions with the host's control
 * server" and gives the full action list; the pre-flight scan on this plan
 * found two of its declared param signatures would silently corrupt data
 * (see the two owner-decision blocks below) and this file is where those
 * corrections land.
 *
 * Three properties this file exists to guarantee, because a malformed OSC
 * packet from a Companion button reaches a LIVE show:
 *
 * 1. `invokeAction` never throws. Every engine method this registry can
 *    reach is capable of throwing (an unknown look id, an occupied slot, an
 *    out-of-range box/cell — see the call sites for the specific methods),
 *    and arity/type mismatches are exactly as likely from a hand-wired OSC
 *    client as a real operator typo. Both classes collapse to
 *    `{kind:"error", message}`, never a propagated exception.
 * 2. A rejected invoke never mutates. Arity/type validation, param
 *    coercion, and `ProgramSource`/`Role` parsing all run BEFORE any engine
 *    method is called — the engine call is the very last thing dispatch
 *    does, and every engine method this file calls either validates before
 *    it mutates or doesn't mutate at all on its throwing path (confirmed at
 *    each call site below; see `liveSlots.ts`/`galleryDirector.ts`/
 *    `lookController.ts`, whose `assertSlot`/`assertCell`/range checks all
 *    run before their first write).
 * 3. A refusal is not an error. `ohg.look.nextGuest`/`prevGuest` under
 *    manual box fill (or with no look selected) is the engine correctly
 *    saying no, not failing — `LookController.refusal()`'s own message is
 *    surfaced verbatim as `{kind:"refused", reason}`, distinct from
 *    `{kind:"error"}`, so a host can render it as an operator-facing
 *    refusal rather than a fault.
 *
 * **Two owner decisions, 2026-08-12, that override spec §4.2's declared
 * signatures for this engine (see the pre-flight-scan note in this plan's
 * progress ledger). Both are applied verbatim, not just to the two
 * signatures spec literally got wrong:**
 *
 * - **Participant ids and PINs are `string` params, not `int`.** Spec §4.2
 *   declares `ohg.panelist.add (int zoomID, …)`, `ohg.panelist.role.set
 *   (int pin, …)`, and (elsewhere) `ohg.mukana.override.set/delete (int
 *   pin, …)`. A 4-digit PIN like `"0042"` survives `int` coercion as `42`,
 *   and `personKeyForPin("42")` is a DIFFERENT person's key from
 *   `personKeyForPin("0042")` — a silent identity swap that would put the
 *   wrong name and role on air. Participant ids are opaque host strings
 *   (design spec §5), not guaranteed numeric. So every id/PIN param in this
 *   registry is `"string"`: `ohg.panelist.add`'s `participantId`,
 *   `ohg.panelist.replace`'s `participantId`, and every `pin` param
 *   (`ohg.panelist.role.set`, `ohg.mukana.override.set`,
 *   `ohg.mukana.override.delete`) — the identical corruption risk applies
 *   to all three, not only the two the brief's illustrative table spells
 *   out; narrowing the fix to just those two would leave the exact bug this
 *   decision exists to close reachable through `ohg.mukana.override.*`.
 *   Nothing is lost on the wire: the host stack has a native String param
 *   type. `invokeAction` requires the JS value to ALREADY be a string for
 *   any `"string"` param — it never coerces a number into one, which would
 *   silently reintroduce the exact swap this decision forbids.
 * - **`ProgramSource` encodes as one prefixed string.** `ohg.program.preview`
 *   and `ohg.program.directCut` each take a single `string source`, parsed
 *   by `parseProgramSource`/formatted by `formatProgramSource` (below) per
 *   the design spec's five variants. An unparseable value is a validation
 *   failure like any other — `{kind:"error"}`, engine untouched.
 *
 * **Structural guarantees, enforced by dedicated tests in
 * `actions.test.ts`, because drift here is silent:** `OHG_ACTIONS` and the
 * `dispatch` switch below are a CLOSED 1:1 set — mirroring
 * `StudioControlSurfaceCoverageTests.Adapter_HandlesEveryRegisteredAction`,
 * the C# registry's own coverage test (`OHG_DISPATCHED_ACTION_IDS`, kept by
 * hand beside the switch, is this file's `SupportedActionIds` twin) — and
 * every id maps through `oscAddressFor` to a unique OSC address.
 */

import type { ShowEngine } from "./showEngine.js";
import { isRole, type ProgramSource, type Role } from "./contracts.js";
import { personKeyForPin } from "./personKey.js";

export type ActionParamType = "string" | "int" | "double" | "bool";

export type ActionParam = {
  name: string;
  type: ActionParamType;
  required: boolean;
  description: string;
};

export type ActionDefinition = {
  id: string;
  title: string;
  description: string;
  params: readonly ActionParam[];
};

export type ActionResult =
  | { kind: "ok" }
  | { kind: "refused"; reason: string }
  | { kind: "error"; message: string };

/**
 * Parse the single prefixed-string wire encoding of a `ProgramSource` (the
 * second owner decision above). Returns `null` for anything that doesn't
 * match one of the five variants exactly — an empty `look:` id, a
 * non-digit or negative `slot:` payload, or an unrecognized bare word — so
 * `invokeAction` can turn that into `{kind:"error"}` without ever
 * constructing a bogus `ProgramSource`.
 */
export function parseProgramSource(value: string): ProgramSource | null {
  if (value === "black") return { kind: "black" };
  if (value === "gallery") return { kind: "gallery" };
  if (value === "activeSpeaker") return { kind: "activeSpeaker" };

  if (value.startsWith("look:")) {
    const lookId = value.slice("look:".length);
    return lookId.length > 0 ? { kind: "look", lookId } : null;
  }

  if (value.startsWith("slot:")) {
    const raw = value.slice("slot:".length);
    if (!/^\d+$/.test(raw)) return null;
    return { kind: "slot", slot: Number(raw) };
  }

  return null;
}

/**
 * The inverse of `parseProgramSource`. Task 9 publishes this exact string
 * on the `ohg` state node's program feedback field, so this and
 * `parseProgramSource` must round-trip both ways for all five variants
 * (see `actions.test.ts`) — any asymmetry would make the feedback field
 * disagree with what `ohg.program.preview`/`directCut` actually accept.
 */
export function formatProgramSource(source: ProgramSource): string {
  switch (source.kind) {
    case "black":
      return "black";
    case "gallery":
      return "gallery";
    case "activeSpeaker":
      return "activeSpeaker";
    case "look":
      return `look:${source.lookId}`;
    case "slot":
      return `slot:${source.slot}`;
  }
}

/**
 * The full spec §4.2 action list, params positional and typed exactly as a
 * host bridge will send them (the two owner decisions above already
 * applied). Declaration order here is documentation order, not a
 * dispatch-relevant one — `dispatch`'s switch is what actually routes each
 * id.
 */
export const OHG_ACTIONS: readonly ActionDefinition[] = [
  {
    id: "ohg.panelist.add",
    title: "Add panelist",
    description: "Seat a published participant into a live slot, or the first empty slot when omitted.",
    params: [
      { name: "participantId", type: "string", required: true, description: "The host's participant id (opaque; not guaranteed numeric)." },
      { name: "slot", type: "int", required: false, description: "1-based slot number, or 0/omitted for the first empty slot." }
    ]
  },
  {
    id: "ohg.panelist.remove",
    title: "Remove panelist",
    description: "Clear a live slot, leaving a hole.",
    params: [{ name: "slot", type: "int", required: true, description: "1-based slot number to clear." }]
  },
  {
    id: "ohg.panelist.replace",
    title: "Replace panelist",
    description: "Overwrite whoever holds a slot, occupied or not.",
    params: [
      { name: "slot", type: "int", required: true, description: "1-based slot number to overwrite." },
      { name: "participantId", type: "string", required: true, description: "The host's participant id to seat there." }
    ]
  },
  {
    id: "ohg.panelist.role.set",
    title: "Set panelist role",
    description: "Assign an editorial role to whoever carries this PIN; demotes a prior exclusive-role holder.",
    params: [
      { name: "pin", type: "string", required: true, description: "4-digit Mukana PIN, carried as a string so a leading zero is never lost." },
      { name: "role", type: "string", required: true, description: "One of the engine's Role values (e.g. panelist, host, reader)." }
    ]
  },
  {
    id: "ohg.panelist.syncAll",
    title: "Sync panelists now",
    description: "Force every configured Mukana endpoint to poll on the next tick.",
    params: []
  },
  {
    id: "ohg.program.preview",
    title: "Stage preview",
    description: "Stage a source in preview (no-op on program until cut/auto).",
    params: [{ name: "source", type: "string", required: true, description: "A ProgramSource wire string: black | gallery | activeSpeaker | look:<id> | slot:<n>." }]
  },
  {
    id: "ohg.program.cut",
    title: "Cut",
    description: "Cut program to whatever is currently staged in preview.",
    params: []
  },
  {
    id: "ohg.program.auto",
    title: "Auto",
    description: "Transition program to preview using the host's default transition.",
    params: []
  },
  {
    id: "ohg.program.directCut",
    title: "Direct cut",
    description: "Cut program straight to a source, bypassing preview.",
    params: [{ name: "source", type: "string", required: true, description: "A ProgramSource wire string: black | gallery | activeSpeaker | look:<id> | slot:<n>." }]
  },
  {
    id: "ohg.program.asFollow.set",
    title: "Active-speaker follow",
    description: "Toggle whether program cuts to follow the active speaker.",
    params: [{ name: "on", type: "bool", required: true, description: "True to follow the active speaker." }]
  },
  {
    id: "ohg.look.set",
    title: "Select look",
    description: "Select the active on-screen arrangement by id.",
    params: [{ name: "name", type: "string", required: true, description: "A configured look id." }]
  },
  {
    id: "ohg.look.nextGuest",
    title: "Next guest",
    description: "Page the active look's queue window forward by one. Refuses under manual box fill or off the end of the range.",
    params: []
  },
  {
    id: "ohg.look.prevGuest",
    title: "Previous guest",
    description: "Page the active look's queue window back by one. Refuses under manual box fill or off the end of the range.",
    params: []
  },
  {
    id: "ohg.look.box.assign",
    title: "Assign manual box",
    description: "Write one manual box-to-slot assignment for the active look.",
    params: [
      { name: "box", type: "int", required: true, description: "1-based box number within the active look." },
      { name: "slot", type: "int", required: true, description: "1-based roster slot to place in that box." }
    ]
  },
  {
    id: "ohg.look.box.clear",
    title: "Clear manual box",
    description: "Remove one manual box assignment, leaving the rest untouched.",
    params: [{ name: "box", type: "int", required: true, description: "1-based box number to clear." }]
  },
  {
    id: "ohg.gallery.resetFromSlots",
    title: "Reset gallery from slots",
    description: "Compact the gallery from current seating; blanks skipped, cell 1 = first occupied seat.",
    params: []
  },
  {
    id: "ohg.gallery.replace",
    title: "Replace gallery cell",
    description: "Put a roster slot in a gallery cell, overwriting whatever was there.",
    params: [
      { name: "cell", type: "int", required: true, description: "1-based gallery cell number." },
      { name: "slot", type: "int", required: true, description: "1-based roster slot to place in that cell." }
    ]
  },
  {
    id: "ohg.gallery.remove",
    title: "Remove gallery cell",
    description: "Blank one gallery cell, leaving every other cell untouched.",
    params: [{ name: "cell", type: "int", required: true, description: "1-based gallery cell number to blank." }]
  },
  {
    id: "ohg.gallery.empty",
    title: "Empty gallery",
    description: "Blank every gallery cell.",
    params: []
  },
  {
    id: "ohg.gallery.smart.set",
    title: "Smart gallery",
    description: "Toggle speaker-recency reordering of the gallery's occupied cells.",
    params: [{ name: "on", type: "bool", required: true, description: "True to reorder by speaker recency every tick." }]
  },
  {
    id: "ohg.gfx.headline.in",
    title: "Headline in",
    description: "Show the operator-set headline overlay.",
    params: []
  },
  {
    id: "ohg.gfx.headline.out",
    title: "Headline out",
    description: "Hide the headline overlay, retaining its text.",
    params: []
  },
  {
    id: "ohg.gfx.headline.change",
    title: "Change headline",
    description: "Set the headline overlay's text (does not change visibility).",
    params: [
      { name: "name", type: "string", required: true, description: "Headline name text." },
      { name: "location", type: "string", required: true, description: "Headline location text." }
    ]
  },
  {
    id: "ohg.gfx.question.in",
    title: "Question in",
    description: "Show the audience question overlay.",
    params: []
  },
  {
    id: "ohg.gfx.question.out",
    title: "Question out",
    description: "Hide the audience question overlay.",
    params: []
  },
  {
    id: "ohg.mukana.sync",
    title: "Sync Mukana now",
    description: "Force every configured Mukana endpoint to poll on the next tick (same effect as ohg.panelist.syncAll).",
    params: []
  },
  {
    id: "ohg.mukana.override.set",
    title: "Set role override",
    description: "Write an operator role override for whoever carries this PIN, demoting a prior exclusive-role holder.",
    params: [
      { name: "pin", type: "string", required: true, description: "4-digit Mukana PIN, carried as a string so a leading zero is never lost." },
      { name: "name", type: "string", required: true, description: "Display name for the override." },
      { name: "location", type: "string", required: true, description: "Location for the override." },
      { name: "role", type: "string", required: true, description: "One of the engine's Role values (e.g. panelist, host, reader)." }
    ]
  },
  {
    id: "ohg.mukana.override.delete",
    title: "Delete role override",
    description: "Remove an operator role override, reverting that person to whatever Mukana (or nothing) declares.",
    params: [{ name: "pin", type: "string", required: true, description: "4-digit Mukana PIN, carried as a string so a leading zero is never lost." }]
  }
];

const ACTIONS_BY_ID = new Map<string, ActionDefinition>(OHG_ACTIONS.map((action) => [action.id, action]));

/**
 * The dispatch switch's own id list, kept by hand beside it — this file's
 * `StudioControlSurface.SupportedActionIds` twin. `actions.test.ts` asserts
 * this and `OHG_ACTIONS`'s ids are the SAME set in both directions, so a
 * deleted `case` or a definition with no matching dispatch both fail loudly
 * rather than silently returning "unknown action" for something the
 * manifest still advertises.
 */
export const OHG_DISPATCHED_ACTION_IDS: readonly string[] = [
  "ohg.panelist.add",
  "ohg.panelist.remove",
  "ohg.panelist.replace",
  "ohg.panelist.role.set",
  "ohg.panelist.syncAll",
  "ohg.program.preview",
  "ohg.program.cut",
  "ohg.program.auto",
  "ohg.program.directCut",
  "ohg.program.asFollow.set",
  "ohg.look.set",
  "ohg.look.nextGuest",
  "ohg.look.prevGuest",
  "ohg.look.box.assign",
  "ohg.look.box.clear",
  "ohg.gallery.resetFromSlots",
  "ohg.gallery.replace",
  "ohg.gallery.remove",
  "ohg.gallery.empty",
  "ohg.gallery.smart.set",
  "ohg.gfx.headline.in",
  "ohg.gfx.headline.out",
  "ohg.gfx.headline.change",
  "ohg.gfx.question.in",
  "ohg.gfx.question.out",
  "ohg.mukana.sync",
  "ohg.mukana.override.set",
  "ohg.mukana.override.delete"
];

type CoerceOutcome = { ok: true; value: string | number | boolean } | { ok: false };

/**
 * Validate + coerce one raw arg against its declared `ActionParamType`.
 * `"string"` is deliberately STRICT — no number/boolean is ever turned into
 * a string here, because that is exactly the silent PIN/participant-id
 * corruption the owner decisions above forbid. `"int"`/`"double"`/`"bool"`
 * accept a same-shaped numeric/boolean string too (a plain OSC client may
 * not have distinct wire types), mirroring the host stack's own
 * `ControlActionRegistry.TryCoerce` (`OscAddressMap.cs`'s C# sibling) so a
 * bridge's coercion behavior and this one agree.
 */
function coerceArg(raw: unknown, type: ActionParamType): CoerceOutcome {
  switch (type) {
    case "string":
      return typeof raw === "string" ? { ok: true, value: raw } : { ok: false };
    case "int": {
      if (typeof raw === "number" && Number.isInteger(raw)) return { ok: true, value: raw };
      if (typeof raw === "string" && /^-?\d+$/.test(raw)) return { ok: true, value: Number(raw) };
      return { ok: false };
    }
    case "double": {
      if (typeof raw === "number" && Number.isFinite(raw)) return { ok: true, value: raw };
      if (typeof raw === "string" && raw.trim().length > 0 && Number.isFinite(Number(raw))) {
        return { ok: true, value: Number(raw) };
      }
      return { ok: false };
    }
    case "bool": {
      if (typeof raw === "boolean") return { ok: true, value: raw };
      if (raw === "true") return { ok: true, value: true };
      if (raw === "false") return { ok: true, value: false };
      return { ok: false };
    }
  }
}

/** A well-typed error result, formatted consistently across every validation failure site below. */
function errorResult(message: string): ActionResult {
  return { kind: "error", message };
}

/**
 * Validate `args` against `def.params` (arity + per-param type), returning
 * either the bound, coerced positional values or an `ActionResult` error to
 * return immediately. Extra trailing args are rejected (never silently
 * ignored — a Companion button bound to the wrong action should fail
 * loudly, not partially apply); missing optional args bind to `undefined`;
 * missing required args are an error. Runs to completion before
 * `invokeAction` calls `dispatch`, so nothing here can leave the engine
 * touched.
 */
function bindArgs(
  id: string,
  def: ActionDefinition,
  args: readonly unknown[]
): { ok: true; bound: (string | number | boolean | undefined)[] } | { ok: false; result: ActionResult } {
  if (args.length > def.params.length) {
    return {
      ok: false,
      result: errorResult(`${id}: expected at most ${def.params.length} argument(s), got ${args.length}`)
    };
  }

  const bound: (string | number | boolean | undefined)[] = [];
  for (let i = 0; i < def.params.length; i += 1) {
    const param = def.params[i];
    if (param === undefined) continue;
    const raw = args[i];

    if (raw === undefined) {
      if (param.required) {
        return {
          ok: false,
          result: errorResult(`${id}: missing required argument '${param.name}' at position ${i}`)
        };
      }
      bound.push(undefined);
      continue;
    }

    const coerced = coerceArg(raw, param.type);
    if (!coerced.ok) {
      return {
        ok: false,
        result: errorResult(
          `${id}: argument '${param.name}' must be ${param.type} (got ${JSON.stringify(raw)})`
        )
      };
    }
    bound.push(coerced.value);
  }

  return { ok: true, bound };
}

/**
 * Route one already-validated invoke to its `ShowEngine` method(s) and map
 * the outcome to an `ActionResult`. Every case here is the LAST thing that
 * runs for its action — no further validation after this point — so a
 * thrown `Error` from the engine (unknown look id, occupied slot,
 * out-of-range box/cell/slot) is a genuine engine-side rejection, not a
 * malformed invoke; `invokeAction`'s try/catch turns it into
 * `{kind:"error"}` rather than propagating it.
 */
function dispatch(engine: ShowEngine, id: string, bound: readonly (string | number | boolean | undefined)[]): ActionResult {
  switch (id) {
    case "ohg.panelist.add": {
      const participantId = bound[0] as string;
      const rawSlot = bound[1] as number | undefined;
      // Wire value 0 (and an omitted slot) both mean "first empty" — the
      // wire-level translation Task 6 deliberately left to this layer,
      // since `LiveSlots.assertSlot` throws for `slot < 1` and would fail
      // loudly on a raw 0 rather than auto-placing.
      const slot = rawSlot === undefined || rawSlot === 0 ? undefined : rawSlot;
      engine.addPanelist(participantId, slot);
      return { kind: "ok" };
    }
    case "ohg.panelist.remove": {
      const slot = bound[0] as number;
      engine.removePanelist(slot);
      return { kind: "ok" };
    }
    case "ohg.panelist.replace": {
      const slot = bound[0] as number;
      const participantId = bound[1] as string;
      engine.replacePanelist(slot, participantId);
      return { kind: "ok" };
    }
    case "ohg.panelist.role.set": {
      const pin = bound[0] as string;
      const role = bound[1] as string;
      if (!isRole(role)) return errorResult(`${id}: '${role}' is not a known role`);
      engine.setRole(pin, role);
      return { kind: "ok" };
    }
    case "ohg.panelist.syncAll": {
      engine.syncAll();
      return { kind: "ok" };
    }
    case "ohg.program.preview": {
      const raw = bound[0] as string;
      const source = parseProgramSource(raw);
      if (source === null) return errorResult(`${id}: '${raw}' is not a valid ProgramSource`);
      engine.setPreview(source);
      return { kind: "ok" };
    }
    case "ohg.program.cut": {
      engine.cut();
      return { kind: "ok" };
    }
    case "ohg.program.auto": {
      engine.auto();
      return { kind: "ok" };
    }
    case "ohg.program.directCut": {
      const raw = bound[0] as string;
      const source = parseProgramSource(raw);
      if (source === null) return errorResult(`${id}: '${raw}' is not a valid ProgramSource`);
      engine.directCut(source);
      return { kind: "ok" };
    }
    case "ohg.program.asFollow.set": {
      const on = bound[0] as boolean;
      engine.setActiveSpeakerFollow(on);
      return { kind: "ok" };
    }
    case "ohg.look.set": {
      const lookId = bound[0] as string;
      engine.setLook(lookId);
      return { kind: "ok" };
    }
    case "ohg.look.nextGuest": {
      engine.nextGuest();
      const refusal = engine.pagingRefusal();
      return refusal === null ? { kind: "ok" } : { kind: "refused", reason: refusal.message };
    }
    case "ohg.look.prevGuest": {
      engine.prevGuest();
      const refusal = engine.pagingRefusal();
      return refusal === null ? { kind: "ok" } : { kind: "refused", reason: refusal.message };
    }
    case "ohg.look.box.assign": {
      const box = bound[0] as number;
      const slot = bound[1] as number;
      engine.assignBox(box, slot);
      return { kind: "ok" };
    }
    case "ohg.look.box.clear": {
      const box = bound[0] as number;
      engine.clearBox(box);
      return { kind: "ok" };
    }
    case "ohg.gallery.resetFromSlots": {
      engine.resetGalleryFromSlots();
      return { kind: "ok" };
    }
    case "ohg.gallery.replace": {
      const cell = bound[0] as number;
      const slot = bound[1] as number;
      engine.replaceGalleryCell(cell, slot);
      return { kind: "ok" };
    }
    case "ohg.gallery.remove": {
      const cell = bound[0] as number;
      engine.removeGalleryCell(cell);
      return { kind: "ok" };
    }
    case "ohg.gallery.empty": {
      engine.emptyGallery();
      return { kind: "ok" };
    }
    case "ohg.gallery.smart.set": {
      const on = bound[0] as boolean;
      engine.setSmartGallery(on);
      return { kind: "ok" };
    }
    case "ohg.gfx.headline.in": {
      engine.setHeadlineVisible(true);
      return { kind: "ok" };
    }
    case "ohg.gfx.headline.out": {
      engine.setHeadlineVisible(false);
      return { kind: "ok" };
    }
    case "ohg.gfx.headline.change": {
      const name = bound[0] as string;
      const location = bound[1] as string;
      engine.setHeadline({ name, location });
      return { kind: "ok" };
    }
    case "ohg.gfx.question.in": {
      engine.setQuestionVisible(true);
      return { kind: "ok" };
    }
    case "ohg.gfx.question.out": {
      engine.setQuestionVisible(false);
      return { kind: "ok" };
    }
    case "ohg.mukana.sync": {
      engine.syncAll();
      return { kind: "ok" };
    }
    case "ohg.mukana.override.set": {
      const pin = bound[0] as string;
      const name = bound[1] as string;
      const location = bound[2] as string;
      const role = bound[3] as string;
      if (!isRole(role)) return errorResult(`${id}: '${role}' is not a known role`);
      engine.setOverride({ personKey: personKeyForPin(pin), displayName: name, location, role: role as Role });
      return { kind: "ok" };
    }
    case "ohg.mukana.override.delete": {
      const pin = bound[0] as string;
      engine.clearOverride(personKeyForPin(pin));
      return { kind: "ok" };
    }
    default:
      return errorResult(`ohg action: unknown action id ${JSON.stringify(id)}`);
  }
}

/**
 * Invoke one `ohg.*` action by id against `engine`. Never throws (see the
 * file-level doc comment's guarantee #1): arity/type validation and
 * `ProgramSource`/`Role` parsing all run before any engine call, and
 * whatever the engine call itself throws is caught here and mapped to
 * `{kind:"error"}` rather than propagated. An unknown `id` is the same
 * `{kind:"error"}` shape as every other rejection — there is no separate
 * "action not found" result kind.
 */
export function invokeAction(engine: ShowEngine, id: string, args: readonly unknown[]): ActionResult {
  const def = ACTIONS_BY_ID.get(id);
  if (def === undefined) {
    return errorResult(`ohg action: unknown action id ${JSON.stringify(id)}`);
  }

  const bindResult = bindArgs(id, def, args);
  if (!bindResult.ok) return bindResult.result;

  try {
    return dispatch(engine, id, bindResult.bound);
  } catch (error) {
    return errorResult(error instanceof Error ? error.message : String(error));
  }
}

/**
 * The OSC address a host must expose for `id`, by the host stack's own rule
 * (`OscAddressMap.ActionIdToAddress`, `OscAddressMap.cs:19-35`): the root
 * plus the id with every `.` replaced by `/`, no case or word
 * transformation. `ohg.look.box.assign` under the default root becomes
 * `/cvp/ohg/look/box/assign`. Kept byte-for-byte identical to the C# rule —
 * "improving" the casing here would make a bridge's address disagree with
 * the shipped host behavior it must match.
 */
export function oscAddressFor(id: string, root = "/cvp"): string {
  return `${root}/${id.replace(/\./g, "/")}`;
}
