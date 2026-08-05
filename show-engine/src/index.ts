/**
 * Public surface of the OHG show engine.
 * Plan 1 shipped the identity and roster core; Plan 2 adds the direction
 * layer — speaker recency, gallery, looks, the hands queue, and the
 * program bus — exported from here alongside it.
 */

export {
  coerceRole,
  EXCLUSIVE_ROLES,
  isRole,
  isPlateTone,
  PLATE_TONES,
  programSourcesEqual,
  ROLES,
  type GalleryCell,
  type Identity,
  type LookDefinition,
  type MukanaDb,
  type MukanaRecord,
  type Panelist,
  type Participant,
  type PlateTone,
  type ProgramSource,
  type QueueState,
  type Role,
  type Slot
} from "./contracts.js";

export { parseShowEngineConfig, type MukanaConfig, type ShowEngineConfig } from "./config.js";
export { extractPin, identityFromName, splitDisplayName } from "./identity.js";
export { ZoomIngest, type ZoomEvent } from "./zoomIngest.js";
export { MukanaRegistry, parseMukanaPanelists, type MukanaOutcome } from "./mukanaParse.js";
export {
  MukanaClient,
  type FetchLike,
  type FetchResponse,
  type MukanaEndpoint,
  type MukanaHealth
} from "./mukanaClient.js";
export { OverrideDb, type OverrideRecord } from "./overrideDb.js";
export { buildPanelistDb } from "./panelistDb.js";
export {
  LiveSlots,
  LiveSlotsRestoreError,
  type LiveSlotsOptions,
  type LiveSlotsState
} from "./liveSlots.js";
export {
  parseHandsPayload,
  queueOrder,
  stripChairs,
  type HandsOutcome
} from "./handsQueue.js";
export {
  FiloAssigner,
  RecencyScores,
  VisibleSetAssigner,
  type PlacementChange,
  type PositionAssigner
} from "./speakerRecency.js";
export { GalleryDirector, GalleryError, type GalleryState } from "./galleryDirector.js";
export {
  findChairSlots,
  pageCountFor,
  resolveLook,
  type BoxAssignment,
  type LookResolution,
  type Nameplate,
  type NameplatePosition
} from "./lookDirector.js";
export { ProgramBus, type ProgramState } from "./programBus.js";
export { StateStore, type ShowState, type StateFs } from "./persistence.js";
