import { afterEach, describe, expect, it, vi } from "vitest";
import {
  ZoomVideoFrameStore,
  connectFrameStoreToBridge,
  type ZoomVideoFrame
} from "./zoomVideoFrames";
import type { NativeHostBridge } from "./nativeHostBridge";

afterEach(() => {
  vi.useRealTimers();
});

function frame(participantId: string, frameId: number, overrides: Partial<ZoomVideoFrame> = {}): ZoomVideoFrame {
  return {
    participantId,
    frameId,
    width: 2,
    height: 1,
    rgba: new Uint8ClampedArray([0, 0, 0, 255, 255, 255, 255, 255]),
    ...overrides
  };
}

describe("ZoomVideoFrameStore", () => {
  it("stores and returns the latest frame per participant", () => {
    const store = new ZoomVideoFrameStore();
    store.push(frame("p1", 1));
    store.push(frame("p2", 1));
    expect(store.getLatest("p1")?.frameId).toBe(1);
    expect(store.getLatest("p2")?.frameId).toBe(1);
    expect(store.getLatest("p3")).toBeUndefined();
  });

  it("ignores stale or duplicate frame ids", () => {
    const store = new ZoomVideoFrameStore();
    store.push(frame("p1", 5));
    store.push(frame("p1", 3));
    store.push(frame("p1", 5));
    expect(store.getLatest("p1")?.frameId).toBe(5);
  });

  it("notifies only the matching participant's subscribers", () => {
    const store = new ZoomVideoFrameStore();
    const p1Listener = vi.fn();
    const p2Listener = vi.fn();
    store.subscribe("p1", p1Listener);
    store.subscribe("p2", p2Listener);

    store.push(frame("p1", 1));
    expect(p1Listener).toHaveBeenCalledTimes(1);
    expect(p2Listener).not.toHaveBeenCalled();
  });

  it("stops notifying after unsubscribe", () => {
    const store = new ZoomVideoFrameStore();
    const listener = vi.fn();
    const unsubscribe = store.subscribe("p1", listener);
    store.push(frame("p1", 1));
    unsubscribe();
    store.push(frame("p1", 2));
    expect(listener).toHaveBeenCalledTimes(1);
  });

  it("does not deliver stale frames to subscribers", () => {
    const store = new ZoomVideoFrameStore();
    const listener = vi.fn();
    store.subscribe("p1", listener);
    store.push(frame("p1", 4));
    store.push(frame("p1", 2));
    expect(listener).toHaveBeenCalledTimes(1);
  });

  it("reports native backing only after bridge frames are delivered", () => {
    vi.useFakeTimers();
    vi.setSystemTime(1000);
    const store = new ZoomVideoFrameStore();

    expect(store.getDiagnostics("p1")).toMatchObject({
      backing: "fallback-simulated",
      subscribed: false,
      deliveredFrameCount: 0,
      stale: false
    });

    const unsubscribe = store.subscribe("p1", vi.fn());
    store.push(frame("p1", 12));

    expect(store.getDiagnostics("p1", 1250)).toMatchObject({
      backing: "native-frame",
      subscribed: true,
      subscriberCount: 1,
      deliveredFrameCount: 1,
      latestFrameId: 12,
      latestFrameAgeMs: 250,
      stale: false
    });

    expect(store.getDiagnostics("p1", 2501)).toMatchObject({
      latestFrameAgeMs: 1501,
      stale: true
    });

    unsubscribe();
    expect(store.getDiagnostics("p1", 2501)).toMatchObject({
      backing: "native-frame",
      subscribed: false,
      deliveredFrameCount: 1,
      stale: true
    });
  });

  it("summarizes subscribed delivered and stale frame counts across the renderer store", () => {
    vi.useFakeTimers();
    vi.setSystemTime(5000);
    const store = new ZoomVideoFrameStore();
    store.subscribe("p1", vi.fn());
    store.subscribe("p2", vi.fn());
    store.push(frame("p1", 1));
    store.push(frame("p1", 2));
    store.push(frame("p2", 1));

    expect(store.getStoreDiagnostics(6201)).toEqual({
      subscribedParticipantCount: 2,
      deliveredParticipantCount: 2,
      deliveredFrameCount: 3,
      staleFrameCount: 2,
      staleThresholdMs: 1000
    });
  });
});

describe("connectFrameStoreToBridge", () => {
  it("returns undefined when the host has no frame stream", () => {
    const store = new ZoomVideoFrameStore();
    const bridge = { request: vi.fn(), platform: "linux", host: "test-host" } as unknown as NativeHostBridge;
    expect(connectFrameStoreToBridge(store, bridge)).toBeUndefined();
    expect(connectFrameStoreToBridge(store, undefined)).toBeUndefined();
  });

  it("feeds pushed frames into the store and unsubscribes cleanly", () => {
    const store = new ZoomVideoFrameStore();
    let captured: ((frame: ZoomVideoFrame) => void) | undefined;
    const unsubscribe = vi.fn();
    const bridge = {
      request: vi.fn(),
      platform: "darwin",
      host: "electron",
      onZoomVideoFrame: (listener: (frame: ZoomVideoFrame) => void) => {
        captured = listener;
        return unsubscribe;
      }
    } as unknown as NativeHostBridge;

    const dispose = connectFrameStoreToBridge(store, bridge);
    expect(typeof dispose).toBe("function");

    captured?.(frame("p1", 7));
    expect(store.getLatest("p1")?.frameId).toBe(7);

    dispose?.();
    expect(unsubscribe).toHaveBeenCalledTimes(1);
  });
});
