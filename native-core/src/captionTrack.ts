import type { MediaCoreCaptionCue, MediaCoreCaptionTrack } from "./protocol.js";

const IDLE_TRACK: MediaCoreCaptionTrack = {
  enabled: true,
  status: "idle",
  latencyMs: 180,
  warnings: []
};

export class CaptionTrackModel {
  private enabled = true;
  private currentCue?: MediaCoreCaptionCue;

  setEnabled(enabled: boolean) {
    this.enabled = enabled;
    return this.snapshot();
  }

  pushCue(text: string, atMs: number, speaker?: string) {
    const trimmed = text.trim();
    if (!trimmed) {
      return this.snapshot(["Caption cue ignored because text was empty."]);
    }

    this.currentCue = {
      text: trimmed,
      speaker,
      atMs,
      confidence: Math.max(82, 97 - Math.floor(trimmed.length / 28))
    };
    return this.snapshot();
  }

  snapshot(extraWarnings: string[] = []): MediaCoreCaptionTrack {
    const warnings = [...extraWarnings];
    if (!this.enabled) {
      return {
        enabled: false,
        status: "idle",
        currentCue: this.currentCue,
        latencyMs: 0,
        warnings: ["Caption track disabled."]
      };
    }

    if (!this.currentCue) {
      return {
        ...IDLE_TRACK,
        enabled: true,
        warnings
      };
    }

    if (this.currentCue.text.length > 96) {
      warnings.push("Caption line is long; compact mode enabled.");
    }

    return {
      enabled: true,
      status: warnings.length > 0 ? "warning" : "live",
      currentCue: this.currentCue,
      latencyMs: 180,
      warnings
    };
  }
}