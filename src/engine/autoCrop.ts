import type { FeedHealth, Participant } from "../domain/production";

export type AutoCropFraming = {
  zoom: number;
  headroomPercent: number;
  centerY: number;
  framingLabel: "tight" | "balanced" | "wide";
  quality: "good" | "soft" | "low";
  warning?: string;
};

const LOW_QUALITY_HEALTH: ReadonlySet<FeedHealth> = new Set<FeedHealth>([
  "low-resolution",
  "recovering",
  "video-off"
]);

const SOFT_CONFIDENCE_THRESHOLD = 90;

export function isLowQualityFeed(health: FeedHealth, cropConfidence: number): boolean {
  return LOW_QUALITY_HEALTH.has(health) || cropConfidence < 70;
}

/**
 * Compute a face-aware framing recommendation for a participant. Higher crop
 * confidence permits a tighter, better-centered shot; weaker confidence backs
 * off to a safer wide frame. Headroom keeps the eyeline on the upper third.
 */
export function computeAutoCrop(participant: Participant): AutoCropFraming {
  const confidence = clampPercent(participant.cropConfidence);
  const lowQuality = isLowQualityFeed(participant.health, confidence);

  // Confident faces can be cropped tighter (more zoom); soft ones stay wide.
  const zoom = lowQuality ? 1 : round2(1.08 + (confidence / 100) * 0.22);

  // Tighter shots want a touch less headroom so the face stays centered.
  const headroomPercent = lowQuality ? 14 : Math.round(13 - (confidence / 100) * 4);

  // Eyeline on the upper third: smaller centerY pulls the face up.
  const centerY = round2(0.5 - headroomPercent / 200);

  const framingLabel: AutoCropFraming["framingLabel"] = zoom >= 1.24 ? "tight" : zoom >= 1.12 ? "balanced" : "wide";

  const quality: AutoCropFraming["quality"] = lowQuality ? "low" : confidence < SOFT_CONFIDENCE_THRESHOLD ? "soft" : "good";

  return Object.freeze({
    zoom,
    headroomPercent,
    centerY,
    framingLabel,
    quality,
    warning: buildWarning(participant, quality)
  });
}

export function describeFraming(framing: AutoCropFraming): string {
  return `${framing.framingLabel} frame - ${framing.headroomPercent}% headroom - ${framing.quality} feed`;
}

function buildWarning(participant: Participant, quality: AutoCropFraming["quality"]): string | undefined {
  if (quality === "low") {
    return `Low-quality feed for ${participant.name}; holding a safe wide frame.`;
  }

  if (quality === "soft") {
    return `Face lock is soft for ${participant.name}; auto-crop eased back.`;
  }

  return undefined;
}

function clampPercent(value: number): number {
  if (Number.isNaN(value)) {
    return 0;
  }

  return Math.min(100, Math.max(0, value));
}

function round2(value: number): number {
  return Math.round(value * 100) / 100;
}
