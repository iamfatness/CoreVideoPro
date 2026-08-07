/**
 * The published show snapshot — the one object every operator surface (a
 * WinUI panel, a SwiftUI panel, an OSC feedback loop, a WebSocket push)
 * renders from. `buildSnapshot` exists so this shape is testable without
 * constructing an engine, and so a future control integration has one
 * function to call once per tick.
 *
 * The load-bearing property: `buildSnapshot` deep-copies every mutable
 * structure it publishes. The engine holds live module state that keeps
 * mutating every tick, and a consumer may hold a snapshot across ticks — a
 * shared reference would let the next tick rewrite a snapshot a surface is
 * mid-render on.
 */

import type { GalleryCell, Panelist, ShowCapabilities, Slot, QueueState } from "./contracts.js";
import type { ProgramState } from "./programBus.js";
import type {
  BoxAssignment,
  LookResolution,
  ManualBoxAssignments,
  Nameplate,
  NameplatePosition
} from "./lookDirector.js";
import type { TallyState } from "./tallyPublisher.js";
import type { OverlayState } from "./overlayDirector.js";
import type { MukanaEndpoint, MukanaHealth } from "./mukanaClient.js";

export type ShowSnapshot = {
  revision: number;
  panelists: Panelist[];
  slots: Slot[];
  gallery: GalleryCell[];
  queue: QueueState;
  program: ProgramState;
  look: LookResolution | null;
  page: number;
  manualBoxes: ManualBoxAssignments;
  tally: TallyState;
  overlays: OverlayState;
  capabilities: ShowCapabilities;
  health: Record<MukanaEndpoint, MukanaHealth>;
  /** Panelists the live roster did not have a seat for, e.g. capacity overflow on `rebuild`. */
  unseated: Panelist[];
  /**
   * Why the last `nextGuest`/`prevGuest` call did not move the page, or
   * `null` when the last paging attempt (if any) succeeded. Set only under
   * manual box fill, where there is no queue window to move through — a
   * typed refusal rather than a throw or a silent no-op (spec §4).
   */
  pagingRefused: string | null;
};

export type BuildSnapshotInput = {
  revision: number;
  panelists: ReadonlyMap<string, Panelist>;
  slots: readonly Slot[];
  gallery: readonly GalleryCell[];
  queue: QueueState;
  program: ProgramState;
  look: LookResolution | null;
  page: number;
  manualBoxes: ManualBoxAssignments;
  tally: TallyState;
  overlays: OverlayState;
  capabilities: ShowCapabilities;
  health: Record<MukanaEndpoint, MukanaHealth>;
  unseated: readonly Panelist[];
  /** Optional so existing callers building this input keep compiling; omitted means null. */
  pagingRefused?: string | null;
};

function clonePanelist(panelist: Panelist): Panelist {
  return { ...panelist };
}

function cloneSlot(slot: Slot): Slot {
  return { slot: slot.slot, panelist: slot.panelist === null ? null : clonePanelist(slot.panelist) };
}

function cloneGalleryCell(cell: GalleryCell): GalleryCell {
  return { ...cell };
}

function cloneQueue(queue: QueueState): QueueState {
  return {
    previous: [...queue.previous],
    current: queue.current,
    upcoming: [...queue.upcoming]
  };
}

function cloneProgram(program: ProgramState): ProgramState {
  return {
    program: { ...program.program },
    preview: { ...program.preview },
    activeSpeakerFollow: program.activeSpeakerFollow,
    activeSpeakerId: program.activeSpeakerId
  };
}

function cloneNameplatePosition(position: NameplatePosition): NameplatePosition {
  return { ...position };
}

function cloneNameplate(nameplate: Nameplate): Nameplate {
  return { ...nameplate, position: cloneNameplatePosition(nameplate.position) };
}

function cloneBoxAssignment(box: BoxAssignment): BoxAssignment {
  return { ...box };
}

function cloneLook(look: LookResolution | null): LookResolution | null {
  if (look === null) return null;
  return {
    ...look,
    boxes: look.boxes.map(cloneBoxAssignment),
    nameplates: look.nameplates.map(cloneNameplate)
  };
}

function cloneManualBoxes(manualBoxes: ManualBoxAssignments): ManualBoxAssignments {
  return { ...manualBoxes };
}

function cloneTally(tally: TallyState): TallyState {
  return {
    mode: tally.mode,
    onAirSlots: [...tally.onAirSlots],
    onAirPins: [...tally.onAirPins],
    onAirParticipantIds: [...tally.onAirParticipantIds]
  };
}

function cloneOverlays(overlays: OverlayState): OverlayState {
  return {
    nameplates: overlays.nameplates.map(cloneNameplate),
    question: overlays.question === null ? null : { ...overlays.question }
  };
}

function cloneCapabilities(capabilities: ShowCapabilities): ShowCapabilities {
  return {
    registry: { ...capabilities.registry },
    handsQueue: { ...capabilities.handsQueue },
    questionFeed: { ...capabilities.questionFeed }
  };
}

function cloneHealth(health: Record<MukanaEndpoint, MukanaHealth>): Record<MukanaEndpoint, MukanaHealth> {
  return {
    panelists: { ...health.panelists },
    hands: { ...health.hands },
    question: { ...health.question }
  };
}

/**
 * Assemble the one object every operator surface renders from. Pure: same
 * input always yields a structurally equal output, with no clock, I/O, or
 * module instances involved. `panelists` is flattened from the input map
 * to an array sorted by `participantId`, so two snapshots built from the
 * same roster compare equal regardless of Map insertion order. Every
 * mutable structure is deep-copied so a snapshot a consumer holds across
 * ticks can never be rewritten by the engine's next tick.
 */
export function buildSnapshot(input: BuildSnapshotInput): ShowSnapshot {
  const panelists = Array.from(input.panelists.values())
    .map(clonePanelist)
    .sort((a, b) => (a.participantId < b.participantId ? -1 : a.participantId > b.participantId ? 1 : 0));

  return {
    revision: input.revision,
    panelists,
    slots: input.slots.map(cloneSlot),
    gallery: input.gallery.map(cloneGalleryCell),
    queue: cloneQueue(input.queue),
    program: cloneProgram(input.program),
    look: cloneLook(input.look),
    page: input.page,
    manualBoxes: cloneManualBoxes(input.manualBoxes),
    tally: cloneTally(input.tally),
    overlays: cloneOverlays(input.overlays),
    capabilities: cloneCapabilities(input.capabilities),
    health: cloneHealth(input.health),
    unseated: input.unseated.map(clonePanelist),
    pagingRefused: input.pagingRefused ?? null
  };
}
