import type { MediaCoreFrame, MediaCoreFrameKind, MediaCoreFrameSourceSnapshot, MediaCoreMediaSourceKind } from "./protocol.js";

export type MediaCoreFrameSourceRequest = {
  sourceId: string;
  participantId?: string;
  kind: MediaCoreFrameKind;
};

export type MediaCoreFrameSourceResult = {
  frames: MediaCoreFrame[];
  snapshot: MediaCoreFrameSourceSnapshot;
};

export interface MediaFrameSource {
  readonly adapterId: string;
  readonly kind: MediaCoreMediaSourceKind;
  render(sources: MediaCoreFrameSourceRequest[], elapsedMs: number): MediaCoreFrameSourceResult;
  snapshot(elapsedMs: number): MediaCoreFrameSourceSnapshot;
}

export class TestPatternMediaSource implements MediaFrameSource {
  readonly adapterId = "test-pattern";
  readonly kind = "test-pattern";
  private readonly frameNumbers = new Map<string, number>();
  private droppedFrameCount = 0;
  private lowResolutionFrameCount = 0;
  private lastFrameTimestampMs?: number;

  render(sources: MediaCoreFrameSourceRequest[], elapsedMs: number): MediaCoreFrameSourceResult {
    let droppedThisTick = 0;
    let lowResolutionThisTick = 0;
    const frames = sources.map((source, index) => {
      const nextFrameNumber = (this.frameNumbers.get(source.sourceId) ?? 0) + 1;
      this.frameNumbers.set(source.sourceId, nextFrameNumber);
      const isScreenShare = source.kind === "screen-share";
      const isSyntheticDrop = nextFrameNumber % 90 === 0;
      const isLowResolution = !isScreenShare && index >= 4;

      if (isSyntheticDrop) {
        droppedThisTick += 1;
      }
      if (isLowResolution) {
        lowResolutionThisTick += 1;
      }

      return {
        sourceId: source.sourceId,
        participantId: source.participantId,
        kind: source.kind,
        frameNumber: nextFrameNumber,
        timestampMs: elapsedMs,
        width: isScreenShare ? 1920 : isLowResolution ? 960 : 1280,
        height: isScreenShare ? 1080 : isLowResolution ? 540 : 720,
        fps: isScreenShare ? 30 : 60,
        health: isSyntheticDrop ? "dropped" : isLowResolution ? "low-resolution" : "live"
      } satisfies MediaCoreFrame;
    });

    this.droppedFrameCount += droppedThisTick;
    this.lowResolutionFrameCount += lowResolutionThisTick;
    this.lastFrameTimestampMs = frames.length ? elapsedMs : this.lastFrameTimestampMs;

    return {
      frames,
      snapshot: this.buildSnapshot(sources.length, frames, elapsedMs)
    };
  }

  snapshot(elapsedMs: number): MediaCoreFrameSourceSnapshot {
    return this.buildSnapshot(0, [], elapsedMs);
  }

  private buildSnapshot(subscribedSourceCount: number, frames: MediaCoreFrame[], elapsedMs: number): MediaCoreFrameSourceSnapshot {
    const staleFrameCount = frames.filter((frame) => elapsedMs - frame.timestampMs > 250).length;
    const lowResolutionFrameCount = frames.filter((frame) => frame.health === "low-resolution").length;
    const droppedFrameCount = frames.filter((frame) => frame.health === "dropped").length;
    const warnings = [
      lowResolutionFrameCount > 0 ? `${lowResolutionFrameCount} source frame${lowResolutionFrameCount === 1 ? "" : "s"} below target resolution.` : undefined,
      droppedFrameCount > 0 ? `${droppedFrameCount} source frame${droppedFrameCount === 1 ? "" : "s"} dropped by media source.` : undefined
    ].filter(Boolean) as string[];

    return {
      adapterId: this.adapterId,
      kind: this.kind,
      status: warnings.length ? "degraded" : subscribedSourceCount > 0 ? "subscribed" : "idle",
      subscribedSourceCount,
      liveFrameCount: frames.filter((frame) => frame.health === "live").length,
      staleFrameCount,
      droppedFrameCount: this.droppedFrameCount,
      lowResolutionFrameCount: this.lowResolutionFrameCount,
      lastFrameTimestampMs: this.lastFrameTimestampMs,
      warnings
    };
  }
}
