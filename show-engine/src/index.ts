/**
 * Public surface of the OHG show engine.
 * Plan 1 ships the identity and roster core; direction and output modules
 * (speaker recency, gallery, looks, program bus, tally, graphics) arrive in
 * Plan 2 and are exported from here as they land.
 */

export {
  coerceRole,
  EXCLUSIVE_ROLES,
  isRole,
  ROLES,
  type Identity,
  type MukanaDb,
  type MukanaRecord,
  type Panelist,
  type Participant,
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
export { StateStore, type ShowState, type StateFs } from "./persistence.js";
