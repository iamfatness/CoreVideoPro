import type { MediaCoreFrame } from "./protocol.js";

export type FakeFrameSource = {
  sourceId: string;
  participantId?: string;
  kind: MediaCoreFrame["kind"];
};

export class FakeFrameProducer {
  private readonly frameNumbers = new Map<string, number>();

  render(sources: FakeFrameSource[], elapsedMs: number): MediaCoreFrame[] {
    return sources.map((source, index) => {
      const nextFrameNumber = (this.frameNumbers.get(source.sourceId) ?? 0) + 1;
      this.frameNumbers.set(source.sourceId, nextFrameNumber);
      const isScreenShare = source.kind === "screen-share";
      const isSyntheticDrop = nextFrameNumber % 90 === 0;
      const isLowResolution = !isScreenShare && index >= 4;

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
      };
    });
  }
}
