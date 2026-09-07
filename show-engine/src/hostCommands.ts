/**
 * Host command emission — turns each tick's derived state into `HostAdapter`
 * calls, and ONLY the calls needed to bring the host in sync with what
 * changed. `ShowEngine.tick()` owns sequencing (seat the roster, gate the
 * active speaker, resolve the look, derive overlays); this module owns the
 * separate question of what to tell the host about the result, keeping a
 * private copy of the last value sent per command family so a re-run over
 * unchanged state is silent.
 *
 * Why diffing matters here specifically: a real `HostAdapter` turns each of
 * these calls into work a live show cannot afford at tick rate — a Show
 * Input rebind, a scene-preset swap, an overlay re-raster. This repo's own
 * operator-facing shell (CoreVideo Pro, see the root `CLAUDE.md`) has a
 * documented crash class from rebuilding bound structures at frame rate
 * instead of only on real change; re-emitting an unchanged command every
 * tick is that same churn, aimed at a different host. `assignSlot` and
 * `applyLook`/`setGallery` are diffed differently on purpose:
 * `HostAdapter.assignSlot` is a per-slot call, so this module diffs and
 * emits per slot (never a full sweep). `applyLook`/`setGallery` each take
 * the WHOLE collection in one call, so there is nothing to diff but
 * "does this tick's whole collection differ from the last one sent" — when
 * it does, the whole thing goes, because that is the only shape the host
 * interface offers.
 *
 * `setNameplates`/`setQuestion` are the one family that does NOT do its own
 * diffing here: `OverlayDirector.update` already performs structural change
 * detection over exactly this state (see its own doc comment — re-rendering
 * an identical lower third would restart its on-air animation), so
 * `emitOverlays` takes that verdict as an input rather than recomputing it.
 *
 * Every "last sent" field starts `null`/empty, which is what makes the first
 * call to each `emit*` method send the full current state without any
 * separate "is this the first tick" branch: nothing has been recorded yet,
 * so everything reads as changed.
 */

import type { GalleryCell, Slot } from "./contracts.js";
import type { HostAdapter, LookPlacement } from "./hostAdapter.js";
import type { LookResolution } from "./lookDirector.js";
import type { OverlayState } from "./overlayDirector.js";

/** Structural equality for two maps with the same key/value types — used to decide whether a whole-collection command is worth re-sending. */
function mapsEqual<K, V>(a: ReadonlyMap<K, V>, b: ReadonlyMap<K, V>): boolean {
  if (a.size !== b.size) return false;
  for (const [key, value] of a) {
    if (!b.has(key) || b.get(key) !== value) return false;
  }
  return true;
}

/**
 * Slot → participant id, the shape `assignSlot` binds. An OFFLINE occupant
 * (a Zoom "left" — `liveSlots.refresh` keeps their seat, marked offline, so
 * a reconnect returns them to the same slot rather than dropping them) reads
 * as `null` here even though the seat itself is still held: `assignSlot` is
 * a live video-source binding (CVP: Show Input assignment / spine
 * subscription), and there is no feed to bind to for someone no longer in
 * the meeting. The seat metadata that survives their departure (nameplate,
 * box assignment) is a separate concern this map does not speak to.
 */
function slotParticipantIds(slots: readonly Slot[]): Map<number, string | null> {
  const next = new Map<number, string | null>();
  for (const entry of slots) {
    const participantId =
      entry.panelist !== null && entry.panelist.online ? entry.panelist.participantId : null;
    next.set(entry.slot, participantId);
  }
  return next;
}

/** Box number → roster slot, the guest-box part of the `LookPlacement` `applyLook` sends. `LookResolution.hostSlot`/`readerSlot` ride as separate `LookPlacement` fields, not entries in this map. */
function boxSlots(look: LookResolution): Map<number, number | null> {
  const next = new Map<number, number | null>();
  for (const assignment of look.boxes) {
    next.set(assignment.box, assignment.slot);
  }
  return next;
}

/** The full `LookPlacement` `applyLook` sends for `look` this tick. */
function lookPlacement(look: LookResolution): LookPlacement {
  return {
    lookId: look.lookId,
    scenePreset: look.scenePreset,
    hostSlot: look.hostSlot,
    readerSlot: look.readerSlot,
    boxes: boxSlots(look)
  };
}

/** Structural equality over every `LookPlacement` field — a change to any one of them (not just `lookId`/`boxes`) is a real placement change worth re-sending. */
function placementsEqual(a: LookPlacement, b: LookPlacement): boolean {
  return (
    a.lookId === b.lookId &&
    a.scenePreset === b.scenePreset &&
    a.hostSlot === b.hostSlot &&
    a.readerSlot === b.readerSlot &&
    mapsEqual(a.boxes, b.boxes)
  );
}

/** Gallery cell → roster slot (`0` = blank), the shape `setGallery` sends. */
function galleryCellSlots(cells: readonly GalleryCell[]): Map<number, number> {
  const next = new Map<number, number>();
  for (const cell of cells) {
    next.set(cell.cell, cell.slot);
  }
  return next;
}

/**
 * Diffs each tick's derived state against what was last sent to `host`, and
 * emits only the calls needed to catch it up. One method per command
 * family, each taking that family's new value and deciding for itself
 * whether it is worth sending — this is the whole seam: it never reaches
 * into `ShowEngine` and never resolves anything itself, so it is
 * constructible and testable with nothing but a `HostAdapter`.
 */
export class HostCommandEmitter {
  private readonly host: HostAdapter;

  private lastSlots: Map<number, string | null> | null = null;
  private lastPlacement: LookPlacement | null = null;
  private lastGallery: Map<number, number> | null = null;

  constructor(host: HostAdapter) {
    this.host = host;
  }

  /**
   * Emit `assignSlot` for every slot whose bound participant id changed
   * since the last call — never a full sweep. On the first call (nothing
   * recorded yet) every slot reads as changed, which is exactly the "emit
   * the full picture" behavior the first tick needs.
   */
  emitSlots(slots: readonly Slot[]): void {
    const next = slotParticipantIds(slots);
    for (const [slot, participantId] of next) {
      const known = this.lastSlots?.has(slot) ?? false;
      if (known && this.lastSlots?.get(slot) === participantId) continue;
      this.host.assignSlot(slot, participantId);
    }
    this.lastSlots = next;
  }

  /**
   * Emit `applyLook` when there is a resolved look AND ANY field of the
   * placement changed since the last call — not just the look id or the
   * boxes. `applyLook` takes the whole placement in one call, so a
   * `scenePreset` swap or either chair moving with an unchanged `lookId`
   * must still emit (there is no narrower call to make instead), exactly
   * as a single differing box re-sends the whole box map. A `null` look (no
   * look selected yet) emits nothing; there is no "unassign the look" host
   * command.
   */
  emitLook(look: LookResolution | null): void {
    if (look === null) return;

    const placement = lookPlacement(look);
    const changed = this.lastPlacement === null || !placementsEqual(placement, this.lastPlacement);

    if (changed) {
      this.host.applyLook(placement);
    }

    this.lastPlacement = placement;
  }

  /** Emit `setGallery` with the whole cell map when any cell changed since the last call. */
  emitGallery(cells: readonly GalleryCell[]): void {
    const next = galleryCellSlots(cells);
    if (this.lastGallery === null || !mapsEqual(next, this.lastGallery)) {
      this.host.setGallery(next);
    }
    this.lastGallery = next;
  }

  /**
   * Emit `setNameplates`/`setQuestion` together, gated on `changed` — the
   * boolean `OverlayDirector.update` already returned for this exact
   * derivation. This method does no diffing of its own on purpose (see the
   * file-level doc comment): re-deriving it here could disagree with the
   * director's own field-by-field comparison and would duplicate logic that
   * already lives in exactly one place.
   */
  emitOverlays(overlays: OverlayState, changed: boolean): void {
    if (!changed) return;
    this.host.setNameplates(overlays.nameplates);
    this.host.setQuestion(overlays.question);
  }
}
