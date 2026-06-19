import type { CaptionTranscriptEntry } from "../domain/production";
import type { NativeMediaCoreSnapshot } from "./nativeMediaCoreProtocol";

export const MAX_TRANSCRIPT_ENTRIES = 6;

export type CaptionTranscriptEntryPatch = CaptionTranscriptEntry;

export function resolveActiveOverlayIds(snapshot: NativeMediaCoreSnapshot): Set<string> {
  return new Set(
    snapshot.renderPlan.layers
      .filter((layer) => layer.kind === "overlay" && layer.overlayId)
      .map((layer) => layer.overlayId!.trim())
  );
}

export function resolveOverlayEnabledFlags(
  snapshot: NativeMediaCoreSnapshot,
  graphicIds: string[],
  engineRunning: boolean
): Record<string, boolean> | null {
  if (!engineRunning) {
    return null;
  }

  const activeOverlayIds = resolveActiveOverlayIds(snapshot);
  if (activeOverlayIds.size === 0 && snapshot.overlayCount === 0) {
    return null;
  }

  return Object.fromEntries(graphicIds.map((id) => [id, activeOverlayIds.has(id)]));
}

export function appendCaptionTranscriptFromSnapshot(
  snapshot: NativeMediaCoreSnapshot,
  existing: CaptionTranscriptEntryPatch[],
  speakerRoles?: Record<string, string>
): CaptionTranscriptEntryPatch[] {
  const cue = snapshot.captionTrack.currentCue;
  if (!cue?.text?.trim()) {
    return existing;
  }

  const text = cue.text.trim();
  const speakerName = cue.speaker?.trim() || "Program audio";
  const last = existing.at(-1);
  if (last && last.speakerName === speakerName && last.text === text) {
    return existing;
  }

  const role = speakerRoles?.[speakerName]?.trim() || "Speaker";
  const confidence = Math.max(0, Math.min(100, Math.round(cue.confidence ?? 0)));
  const entry: CaptionTranscriptEntryPatch = {
    id: `cc-${Math.round(cue.atMs)}-${slug(speakerName)}`,
    speakerName,
    role,
    text,
    confidence,
    atSeconds: Math.max(0, Math.round(cue.atMs / 1000))
  };

  return [...existing, entry].slice(-MAX_TRANSCRIPT_ENTRIES);
}

function slug(value: string): string {
  const slugged = value
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/(^-|-$)/g, "");
  return slugged || "speaker";
}