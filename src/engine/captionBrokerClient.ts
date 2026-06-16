/**
 * Caption broker seam (Lane A · A7).
 *
 * Typed boundary between the renderer and a streaming transcription source. The
 * renderer starts the broker and subscribes to {@link CaptionCue}s; in-container
 * it is backed by {@link StubCaptionBrokerClient} (deterministic, no network).
 *
 * Lane ownership (see docs/agent-briefs/08-launch-plan-v1.md): Claude owns this
 * interface + the stub; Grok (C7) implements a cloud transcription broker
 * (Cloudflare Worker / Supabase Edge) that derives cues from program audio and
 * injects it in `engineBundle.ts` with zero renderer changes. The cue shape is
 * intentionally compatible with the existing caption overlay/quality engines
 * (`captionOverlay.ts`, `captionQuality.ts`).
 */

export type CaptionCue = {
  id: string;
  text: string;
  /** Attributed speaker, when attribution is enabled and confident. */
  speakerName?: string;
  /** ASR confidence 0–100 (aligns with `captionQuality.ts`). */
  confidencePct: number;
  /** Program-clock timestamp in ms. */
  atMs: number;
  /** Whether this is a finalized cue vs. an interim hypothesis. */
  isFinal: boolean;
};

export type CaptionBrokerStatus = "idle" | "connecting" | "streaming" | "error";

export type CaptionBrokerStartOptions = {
  /** BCP-47 language code, e.g. "en-US". */
  languageCode?: string;
  /** Whether to attribute cues to a speaker name. */
  speakerAttribution?: boolean;
};

export interface CaptionBrokerClient {
  /** Begin a transcription session. */
  start(options?: CaptionBrokerStartOptions): Promise<void>;
  /** End the session; no further cues are emitted after this resolves. */
  stop(): Promise<void>;
  getStatus(): CaptionBrokerStatus;
  /** Subscribe to cues. Returns an unsubscribe function. */
  onCue(listener: (cue: CaptionCue) => void): () => void;
  /**
   * Optional hook for hosts/tests that drive the broker from an existing caption
   * signal (e.g. the simulated Zoom session) instead of live program audio.
   * Real audio-based brokers omit this and produce cues from the audio path.
   */
  observe?(input: { atMs: number; text: string; speakerName?: string }): void;
}

export type StubCaptionBrokerOptions = {
  /** Attribution toggle default when `start` omits it. */
  speakerAttribution?: boolean;
};

/**
 * Deterministic, in-memory caption broker. Emits a cue for each {@link observe}
 * call while streaming, deriving a plausible confidence from the text so the
 * overlay/quality engines have realistic input. Grok's cloud broker replaces it.
 */
export class StubCaptionBrokerClient implements CaptionBrokerClient {
  private status: CaptionBrokerStatus = "idle";
  private attribution: boolean;
  private nextId = 1;
  private lastText = "";
  private readonly listeners = new Set<(cue: CaptionCue) => void>();

  constructor(options: StubCaptionBrokerOptions = {}) {
    this.attribution = options.speakerAttribution ?? true;
  }

  async start(options: CaptionBrokerStartOptions = {}): Promise<void> {
    if (options.speakerAttribution !== undefined) {
      this.attribution = options.speakerAttribution;
    }
    this.status = "streaming";
  }

  async stop(): Promise<void> {
    this.status = "idle";
    this.lastText = "";
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
    if (this.status !== "streaming") {
      return;
    }
    const text = input.text.trim();
    if (text.length === 0 || text === this.lastText) {
      return;
    }
    this.lastText = text;
    const cue: CaptionCue = {
      id: `cue-${this.nextId++}`,
      text,
      speakerName: this.attribution ? input.speakerName : undefined,
      confidencePct: estimateConfidence(text),
      atMs: input.atMs,
      isFinal: true
    };
    for (const listener of this.listeners) {
      listener(cue);
    }
  }
}

/** Plausible confidence: shorter, cleaner lines score higher. Range 70–97. */
export function estimateConfidence(text: string): number {
  const lengthPenalty = Math.floor(text.length / 24);
  const punctuationBonus = /[.?!]$/.test(text.trim()) ? 3 : 0;
  return Math.max(70, Math.min(97, 95 - lengthPenalty + punctuationBonus));
}
