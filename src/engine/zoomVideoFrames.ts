import type { NativeHostBridge } from "./nativeHostBridge";

/**
 * A single decoded participant video frame, already downscaled to a thumbnail
 * and converted to RGBA by the native Zoom capture engine. `rgba` is
 * `width * height * 4` bytes in row-major order, ready for `ImageData`.
 */
export type ZoomVideoFrame = {
  participantId: string;
  width: number;
  height: number;
  rgba: Uint8ClampedArray;
  /** Monotonic frame id from the engine; used to drop out-of-order frames. */
  frameId: number;
};

export type ZoomVideoFrameListener = (frame: ZoomVideoFrame) => void;

export const ZOOM_VIDEO_FRAME_STALE_MS = 1000;

export type ZoomVideoFrameBacking = "native-frame" | "fallback-simulated";

export type ZoomVideoFrameDiagnostics = {
  participantId: string;
  backing: ZoomVideoFrameBacking;
  subscribed: boolean;
  subscriberCount: number;
  deliveredFrameCount: number;
  latestFrameId?: number;
  latestFrameAgeMs?: number;
  stale: boolean;
  staleThresholdMs: number;
};

export type ZoomVideoFrameStoreDiagnostics = {
  subscribedParticipantCount: number;
  deliveredParticipantCount: number;
  deliveredFrameCount: number;
  staleFrameCount: number;
  staleThresholdMs: number;
};

/**
 * Holds the latest RGBA frame per participant and fans out push notifications to
 * per-participant subscribers (the canvas tiles). Pure and host-agnostic so it
 * is fully unit-testable without Electron, a canvas, or the Zoom engine.
 */
export class ZoomVideoFrameStore {
  private readonly latest = new Map<string, ZoomVideoFrame>();
  private readonly latestReceivedAtMs = new Map<string, number>();
  private readonly deliveredFrameCounts = new Map<string, number>();
  private readonly listeners = new Map<string, Set<ZoomVideoFrameListener>>();

  /** Apply an incoming frame, ignoring stale (older or equal) frame ids. */
  push(frame: ZoomVideoFrame): void {
    const current = this.latest.get(frame.participantId);
    if (current && frame.frameId <= current.frameId) {
      return;
    }
    this.latest.set(frame.participantId, frame);
    this.latestReceivedAtMs.set(frame.participantId, Date.now());
    this.deliveredFrameCounts.set(frame.participantId, (this.deliveredFrameCounts.get(frame.participantId) ?? 0) + 1);
    const subscribers = this.listeners.get(frame.participantId);
    if (subscribers) {
      for (const listener of subscribers) {
        listener(frame);
      }
    }
  }

  getLatest(participantId: string): ZoomVideoFrame | undefined {
    return this.latest.get(participantId);
  }

  getDiagnostics(participantId: string, nowMs = Date.now()): ZoomVideoFrameDiagnostics {
    const latestFrame = this.latest.get(participantId);
    const receivedAtMs = this.latestReceivedAtMs.get(participantId);
    const latestFrameAgeMs = receivedAtMs === undefined ? undefined : Math.max(0, nowMs - receivedAtMs);
    const deliveredFrameCount = this.deliveredFrameCounts.get(participantId) ?? 0;
    const subscriberCount = this.listeners.get(participantId)?.size ?? 0;

    return {
      participantId,
      backing: latestFrame ? "native-frame" : "fallback-simulated",
      subscribed: subscriberCount > 0,
      subscriberCount,
      deliveredFrameCount,
      latestFrameId: latestFrame?.frameId,
      latestFrameAgeMs,
      stale: latestFrame ? (latestFrameAgeMs ?? Number.POSITIVE_INFINITY) > ZOOM_VIDEO_FRAME_STALE_MS : false,
      staleThresholdMs: ZOOM_VIDEO_FRAME_STALE_MS
    };
  }

  getStoreDiagnostics(nowMs = Date.now()): ZoomVideoFrameStoreDiagnostics {
    let staleFrameCount = 0;
    for (const participantId of this.latest.keys()) {
      if (this.getDiagnostics(participantId, nowMs).stale) {
        staleFrameCount += 1;
      }
    }

    return {
      subscribedParticipantCount: this.listeners.size,
      deliveredParticipantCount: this.latest.size,
      deliveredFrameCount: [...this.deliveredFrameCounts.values()].reduce((sum, count) => sum + count, 0),
      staleFrameCount,
      staleThresholdMs: ZOOM_VIDEO_FRAME_STALE_MS
    };
  }

  /** Subscribe to frames for one participant. Returns an unsubscribe function. */
  subscribe(participantId: string, listener: ZoomVideoFrameListener): () => void {
    let subscribers = this.listeners.get(participantId);
    if (!subscribers) {
      subscribers = new Set();
      this.listeners.set(participantId, subscribers);
    }
    subscribers.add(listener);
    return () => {
      const set = this.listeners.get(participantId);
      if (!set) {
        return;
      }
      set.delete(listener);
      if (set.size === 0) {
        this.listeners.delete(participantId);
      }
    };
  }

  clear(): void {
    this.latest.clear();
    this.latestReceivedAtMs.clear();
    this.deliveredFrameCounts.clear();
  }
}

/** Process-wide store the renderer reads from and the bridge feeds into. */
export const zoomVideoFrameStore = new ZoomVideoFrameStore();

/**
 * Wire a bridge's `onZoomVideoFrame` push stream into a frame store. Returns an
 * unsubscribe function, or `undefined` when the host does not provide the stream
 * (the in-container mock), leaving tiles on the simulated placeholder.
 */
export function connectFrameStoreToBridge(
  store: ZoomVideoFrameStore,
  bridge: NativeHostBridge | undefined
): (() => void) | undefined {
  if (!bridge?.onZoomVideoFrame) {
    return undefined;
  }
  return bridge.onZoomVideoFrame((frame) => store.push(frame));
}
