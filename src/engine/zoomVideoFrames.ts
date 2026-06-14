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

/**
 * Holds the latest RGBA frame per participant and fans out push notifications to
 * per-participant subscribers (the canvas tiles). Pure and host-agnostic so it
 * is fully unit-testable without Electron, a canvas, or the Zoom engine.
 */
export class ZoomVideoFrameStore {
  private readonly latest = new Map<string, ZoomVideoFrame>();
  private readonly listeners = new Map<string, Set<ZoomVideoFrameListener>>();

  /** Apply an incoming frame, ignoring stale (older or equal) frame ids. */
  push(frame: ZoomVideoFrame): void {
    const current = this.latest.get(frame.participantId);
    if (current && frame.frameId <= current.frameId) {
      return;
    }
    this.latest.set(frame.participantId, frame);
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
