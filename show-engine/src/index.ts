/**
 * Public surface of the OHG show engine.
 * Plan 1 shipped the identity and roster core; Plan 2 adds the direction
 * layer — speaker recency, gallery, looks, the hands queue, and the
 * program bus; Plan 3 adds the outputs layer — the shared active-speaker
 * gate, per-endpoint Mukana parsing, look paging, tally derivation, and
 * the overlay director — exported from here alongside both; this revision
 * adds the capability layer — person-key resolution and per-integration
 * capability state, so host adapters can name every type they need.
 */

export {
  coerceRole,
  DEFAULT_SKIP_ROLES,
  EXCLUSIVE_ROLES,
  isRole,
  isBoxFill,
  isPlateTone,
  isTallySource,
  BOX_FILLS,
  PLATE_TONES,
  programSourcesEqual,
  ROLES,
  TALLY_SOURCES,
  type BoxFill,
  type Capability,
  type CapabilityState,
  type GalleryCell,
  type Identity,
  type LookDefinition,
  type MukanaDb,
  type MukanaQuestion,
  type MukanaRecord,
  type Panelist,
  type Participant,
  type PlateTone,
  type ProgramSource,
  type QueueState,
  type Role,
  type ShowCapabilities,
  type Slot,
  type TallySource
} from "./contracts.js";
export { personKeyForPin, resolvePersonKey, type PersonKey } from "./personKey.js";
export { canUse, resolveCapabilities, type HealthByEndpoint } from "./capabilities.js";

export { parseShowEngineConfig, type MukanaConfig, type ShowEngineConfig } from "./config.js";
export { extractPin, identityFromName, splitDisplayName } from "./identity.js";
export { ZoomIngest, type ZoomEvent } from "./zoomIngest.js";
export {
  detectDormantEnvelope,
  MukanaRegistry,
  parseMukanaPanelists,
  parseMukanaQuestion,
  type DormantOutcome,
  type MukanaOutcome,
  type QuestionOutcome
} from "./mukanaParse.js";
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
  clampPage,
  effectiveBoxFill,
  findChairSlots,
  pageCountFor,
  resolveLook,
  type BoxAssignment,
  type LookResolution,
  type ManualBoxAssignments,
  type Nameplate,
  type NameplatePosition
} from "./lookDirector.js";
export { ProgramBus, type ProgramState } from "./programBus.js";
export { StateStore, STATE_VERSION, type PersistedShowState, type StateFs } from "./persistence.js";
export { shouldFollowSpeaker } from "./speakerGate.js";
export {
  deriveTally,
  tallyEquals,
  type TallyMode,
  type TallyState
} from "./tallyPublisher.js";
export {
  OverlayDirector,
  type OverlayInput,
  type OverlayState,
  type QuestionOverlay
} from "./overlayDirector.js";

/**
 * The orchestrator layer. `ShowEngine` is what a host process constructs
 * and ticks; `ShowSnapshot` is what it publishes; `HostAdapter` is the port
 * a host implements against it, with `MockHost` as the in-memory double
 * every adapter's own conformance tests can drive. These were all defined
 * but unreachable from outside the package until this export block — the
 * three host adapters in Plans 6-9 are built entirely against these names.
 */
export { systemClock, type Clock } from "./clock.js";
export type { HostAdapter, HostCapabilities } from "./hostAdapter.js";
export { MockHost, type HostCall } from "./mockHost.js";
export {
  buildSnapshot,
  type BuildSnapshotInput,
  type ShowSnapshot
} from "./showSnapshot.js";
export { ShowEngine, type ShowEngineDeps } from "./showEngine.js";
