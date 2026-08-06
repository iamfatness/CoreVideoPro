/**
 * Capability resolution: the pure function that decides, per tick, whether
 * each optional Mukana-backed integration is currently usable by the rest of
 * the show engine.
 *
 * The whole point of this module is that a configured-but-failing
 * integration must behave identically to one that was never configured — a
 * third-party outage is a known state the show already handles, not an
 * incident. `canUse` is the only sanctioned way to read a `Capability`: it
 * collapses "unavailable" and "disabled" to the same `false`, so no call
 * site can accidentally grow logic that treats an outage differently from
 * an integration nobody turned on.
 */

import type { ShowEngineConfig } from "./config.js";
import type { Capability, CapabilityState, ShowCapabilities } from "./contracts.js";
import type { MukanaEndpoint, MukanaHealth } from "./mukanaClient.js";

export type HealthByEndpoint = Record<MukanaEndpoint, MukanaHealth>;

const DEFAULT_UNAVAILABLE_DETAIL = "integration unavailable";

/** Resolve one capability from whether it's configured and its endpoint's health. */
function resolveCapability(enabled: boolean, endpointHealth: MukanaHealth): Capability {
  if (!enabled) {
    return { state: "disabled", detail: null };
  }

  if (endpointHealth.state === "ok") {
    return { state: "available", detail: null };
  }

  // Both "failing" and "dormant" map to "unavailable": outside show hours the
  // backend genuinely is not there, and the show should behave as though it
  // has none until it appears — that is correct, not a compromise.
  const state: CapabilityState = "unavailable";
  const detail = endpointHealth.detail ?? DEFAULT_UNAVAILABLE_DETAIL;
  return { state, detail };
}

/**
 * Resolve every capability this show may use from the current config and
 * the latest health snapshot. Pure, no I/O, no state — returns a fresh
 * object (and fresh `Capability` objects within it) on every call.
 */
export function resolveCapabilities(
  config: ShowEngineConfig,
  health: HealthByEndpoint
): ShowCapabilities {
  return {
    registry: resolveCapability(config.integrations.registry, health.panelists),
    handsQueue: resolveCapability(config.integrations.handsQueue, health.hands),
    questionFeed: resolveCapability(config.integrations.questionFeed, health.question)
  };
}

/**
 * True only when the capability is `available`. Consumers must use this
 * rather than comparing `state` directly, so nothing in the package ever
 * branches on the difference between `unavailable` and `disabled`.
 */
export function canUse(capability: Capability): boolean {
  return capability.state === "available";
}
