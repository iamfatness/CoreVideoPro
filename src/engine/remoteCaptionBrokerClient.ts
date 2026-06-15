import { estimateConfidence, type CaptionBrokerClient, type CaptionBrokerStartOptions, type CaptionBrokerStatus, type CaptionCue } from "./captionBrokerClient";

type CaptionBridgeCue = {
  id: string;
  text: string;
  atMs: number;
  speaker?: string;
  confidence?: number;
  isFinal?: boolean;
};

type CaptionBridge = {
  startCaptionBrokerSession?(
    productionSessionId?: string,
    language?: string
  ): Promise<{ ok: boolean; session?: { brokerSessionId: string }; message?: string }>;
  pushCaptionBrokerChunk?(payload: {
    brokerSessionId?: string;
    atMs: number;
    speakerId?: string;
    speakerName?: string;
    audioLevel?: number;
    audioBase64?: string;
  }): Promise<{ ok: boolean; latencyMs?: number; cues?: CaptionBridgeCue[]; message?: string }>;
  endCaptionBrokerSession?(brokerSessionId?: string): Promise<{ ok: boolean; ended?: boolean; message?: string }>;
};

export function isCaptionBridge(value: unknown): value is CaptionBridge {
  return (
    typeof value === "object" &&
    value !== null &&
    "startCaptionBrokerSession" in value &&
    typeof (value as CaptionBridge).startCaptionBrokerSession === "function"
  );
}

function mapCue(cue: CaptionBridgeCue, attribution: boolean): CaptionCue {
  return {
    id: cue.id,
    text: cue.text,
    speakerName: attribution ? cue.speaker : undefined,
    confidencePct:
      typeof cue.confidence === "number"
        ? cue.confidence <= 1
          ? Math.round(cue.confidence * 100)
          : Math.round(cue.confidence)
        : estimateConfidence(cue.text),
    atMs: cue.atMs,
    isFinal: cue.isFinal ?? true
  };
}

export class RemoteCaptionBrokerClient implements CaptionBrokerClient {
  private status: CaptionBrokerStatus = "idle";
  private attribution = true;
  private brokerSessionId: string | undefined;
  private readonly listeners = new Set<(cue: CaptionCue) => void>();

  constructor(private readonly bridge: CaptionBridge) {}

  async start(options: CaptionBrokerStartOptions = {}): Promise<void> {
    if (!this.bridge.startCaptionBrokerSession) {
      this.status = "error";
      return;
    }
    this.status = "connecting";
    if (options.speakerAttribution !== undefined) {
      this.attribution = options.speakerAttribution;
    }
    const result = await this.bridge.startCaptionBrokerSession(undefined, options.languageCode);
    if (!result.ok || !result.session?.brokerSessionId) {
      this.status = "error";
      return;
    }
    this.brokerSessionId = result.session.brokerSessionId;
    this.status = "streaming";
  }

  async stop(): Promise<void> {
    if (this.brokerSessionId && this.bridge.endCaptionBrokerSession) {
      await this.bridge.endCaptionBrokerSession(this.brokerSessionId);
    }
    this.brokerSessionId = undefined;
    this.status = "idle";
  }

  getStatus(): CaptionBrokerStatus {
    return this.status;
  }

  onCue(listener: (cue: CaptionCue) => void): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  observe(input: { atMs: number; text: string; speakerName?: string }): void {
    if (this.status !== "streaming" || !this.bridge.pushCaptionBrokerChunk) {
      return;
    }
    void this.bridge
      .pushCaptionBrokerChunk({
        brokerSessionId: this.brokerSessionId,
        atMs: input.atMs,
        speakerName: input.speakerName,
        audioLevel: input.text.trim().length > 0 ? 50 : 0
      })
      .then((result) => {
        if (!result.ok || !result.cues?.length) {
          return;
        }
        for (const cue of result.cues) {
          const mapped = mapCue(cue, this.attribution);
          for (const listener of this.listeners) {
            listener(mapped);
          }
        }
      });
  }
}