/**
 * ZoomMediaSpineServiceTransport implementations for the renderer:
 *
 * - InMemoryZoomMediaSpineTransport: synthetic, used by the mock engine bundle
 *   and browser preview (no native bridge needed).
 * - NativeHostZoomMediaSpineTransport: wraps NativeHostBridge.syncZoomMediaSpine;
 *   delegates zoom-tick / zoom-snapshot to the supervised media-core child process
 *   and acknowledges lifecycle commands (configure / join / roster / leave) locally
 *   since the spine sync payload already encodes the full session state.
 */
import type { ZoomMediaSpineServiceTransport, ZoomMediaSpineServiceResponse } from "./zoomMediaSpineServiceExecutor";
import type { ZoomMediaSpineServiceRequest } from "./zoomMediaSpineServicePlan";
import type {
  ZoomMediaSpineNativeSnapshot,
  ZoomMediaSpineNativeSyncEngine
} from "./zoomMediaSpineNativeSync";
import type { NativeHostBridge } from "./nativeHostBridge";
import type { ZoomMediaSpineSyncPayload } from "./zoomMediaSpineSync";

function syntheticSpineSnapshot(sdkVersion = "stub"): ZoomMediaSpineNativeSnapshot {
  return {
    meetingState: "in-meeting",
    sdkVersion,
    participantCount: 0,
    participants: [],
    subscriptions: [],
    recording: { evidence: { programFramesWritten: 0, isoFramesWritten: 0, audioPacketsObserved: 0, subscribedVideoFeeds: 0 } },
    warnings: [],
    events: ["InMemoryZoomMediaSpineTransport: synthetic ack."]
  };
}

/**
 * In-memory transport — acknowledges all service requests with synthetic data.
 * Suitable for the browser preview, tests, and any path where no native bridge
 * is available.
 */
export class InMemoryZoomMediaSpineTransport implements ZoomMediaSpineServiceTransport {
  async request(request: ZoomMediaSpineServiceRequest): Promise<ZoomMediaSpineServiceResponse> {
    return { id: request.id, ok: true, zoom: syntheticSpineSnapshot() };
  }
}

/**
 * Bridge-backed transport. Delegates zoom-tick and zoom-snapshot to the native
 * media-core child process via `bridge.syncZoomMediaSpine`; all lifecycle
 * commands (configure / join / roster / subscriptions / leave) are acknowledged
 * locally — the batch spine-sync payload carries the full session state.
 */
export class NativeHostZoomMediaSpineTransport implements ZoomMediaSpineServiceTransport {
  private lastPayload: ZoomMediaSpineSyncPayload | undefined;
  private lastElapsedMs = 0;

  constructor(private readonly bridge: NativeHostBridge) {}

  /** Called by the session controller to keep the transport up-to-date with the current payload. */
  updatePayload(payload: ZoomMediaSpineSyncPayload, elapsedMs: number): void {
    this.lastPayload = payload;
    this.lastElapsedMs = elapsedMs;
  }

  async request(request: ZoomMediaSpineServiceRequest): Promise<ZoomMediaSpineServiceResponse> {
    switch (request.type) {
      case "zoom-tick":
      case "zoom-snapshot": {
        if (!this.bridge.syncZoomMediaSpine || !this.lastPayload) {
          return { id: request.id, ok: true, zoom: syntheticSpineSnapshot() };
        }
        try {
          const elapsedMs = request.type === "zoom-tick" ? request.elapsedMs : this.lastElapsedMs;
          const zoom = await this.bridge.syncZoomMediaSpine(this.lastPayload, elapsedMs);
          return { id: request.id, ok: true, zoom };
        } catch (error) {
          const message = error instanceof Error ? error.message : "Zoom spine sync failed.";
          return { id: request.id, ok: false, error: { code: "transport-error", message } };
        }
      }
      default:
        // Lifecycle commands (configure / join / roster / subscriptions / recording / leave)
        // are acknowledged — the batch payload carries this state on the next tick.
        return { id: request.id, ok: true, zoom: syntheticSpineSnapshot() };
    }
  }
}

/**
 * Convenience: create the right transport from an optional bridge. Returns
 * NativeHostZoomMediaSpineTransport when a bridge with syncZoomMediaSpine is
 * available, InMemoryZoomMediaSpineTransport otherwise.
 */
export function createZoomMediaSpineTransport(bridge?: NativeHostBridge): {
  transport: ZoomMediaSpineServiceTransport;
  nativeTransport: NativeHostZoomMediaSpineTransport | undefined;
} {
  if (bridge?.syncZoomMediaSpine) {
    const nativeTransport = new NativeHostZoomMediaSpineTransport(bridge);
    return { transport: nativeTransport, nativeTransport };
  }
  return { transport: new InMemoryZoomMediaSpineTransport(), nativeTransport: undefined };
}

/** Narrowing helper used by the spine sync engine to feed the transport the latest payload. */
export function isNativeHostZoomMediaSpineTransport(
  transport: ZoomMediaSpineServiceTransport
): transport is NativeHostZoomMediaSpineTransport {
  return transport instanceof NativeHostZoomMediaSpineTransport;
}

/** A ZoomMediaSpineNativeSyncEngine that routes ticks through the session transport. */
export class TransportBackedZoomMediaSpineSyncEngine implements ZoomMediaSpineNativeSyncEngine {
  constructor(
    private readonly transport: ZoomMediaSpineServiceTransport,
    private readonly nativeTransport: NativeHostZoomMediaSpineTransport | undefined
  ) {}

  async syncProduction(
    _state: Parameters<ZoomMediaSpineNativeSyncEngine["syncProduction"]>[0],
    _readiness: Parameters<ZoomMediaSpineNativeSyncEngine["syncProduction"]>[1],
    elapsedMs: number
  ): ReturnType<ZoomMediaSpineNativeSyncEngine["syncProduction"]> {
    const id = `transport-tick-${elapsedMs}`;
    const response = await this.transport.request({ id, type: "zoom-tick", elapsedMs });
    if (response.ok) {
      return response.zoom;
    }
    throw new Error(`Transport tick failed: ${response.error.message}`);
  }
}
